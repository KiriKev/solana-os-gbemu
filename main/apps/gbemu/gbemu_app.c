// Game Boy: plays a .gb ROM from the microSD card (/sdcard/roms or /sdcard).
//
// The CPU/PPU core is Peanut-GB (peanut_gb.h, MIT, vendored next to this file).
// No ROM ships with the firmware: the user copies their own file to the card.
//
// Threads: the OS task runs the callbacks below (LVGL locked). The emulator
// runs in its own task at idle priority so the OS, LVGL and radios always
// preempt it, and it yields every frame (the browser emulator's tasks are
// cooperative). Each drawn frame lands in one of two RGB565 buffers; the task
// swaps the image's source under the LVGL lock, so LVGL never reads a buffer
// that is being written.
//
// Screen: 160x144 scaled 1.5x to 240x216, centred in the 320x218 app root.
// Keys (calibrated map): d-pad = d-pad, SELECT = A, CANCEL = B,
// UP+DOWN = START, LEFT+RIGHT = SELECT (opposite directions never reach a
// game). BOOT (HOME) leaves the app; cart RAM is saved to <rom>.sav then and
// every minute while it changes.

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_lvgl_port.h"                  // lvgl_port_lock/unlock, as core/os.c
#include "lvgl_private.h"                   // lv_image_cache_drop, as core/os.c
#include "core/os.h"
#include "core/os_mem.h"
#include "core/ui.h"

#define ENABLE_SOUND                0       // no speaker on the badge
#define ENABLE_LCD                  1
#define PEANUT_GB_IS_LITTLE_ENDIAN  1       // RISC-V and wasm
#include "peanut_gb.h"

static const char *TAG = "gbemu";

#define SCALE_W         240
#define SCALE_H         216
#define FRAME_US        16743               // 59.73 Hz
#define SAVE_EVERY_US   (60 * 1000000LL)
#define ROM_MAX         (4u << 20)
#define MAX_ROMS        32
#define PATH_MAX_LEN    200
#define SRAM_KEEP       (64 * 1024)         // SRAM left for Wi-Fi, LVGL, stacks before the core may take some
#define LOCK_WAIT_MS    4                   // how long a frame waits for LVGL to finish a redraw

// A neutral 4-shade palette, lightest first (colour 0 = white on a DMG).
static const uint32_t PALETTE_HEX[4] = {0xE0F8D0, 0x88C070, 0x346856, 0x081820};

typedef struct {
    struct gb_s gb;
    uint8_t *rom;
    size_t rom_size;
    uint8_t *ram;
    size_t ram_size;
    bool ram_dirty;
    uint16_t pal[4];
    uint16_t *buf[2];                   // [front, back]; the image shows buf[0]
    bool drawn;                         // a whole frame reached the back buffer
    char rom_path[PATH_MAX_LEN];
    char sav_path[PATH_MAX_LEN];
} emu_t;

static emu_t *s_emu;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_done;
static volatile bool s_quit;
static lv_image_dsc_t s_dsc[2];
static lv_obj_t *s_root, *s_img, *s_msg, *s_hint;
static ui_list_t *s_list;
static char s_roms[MAX_ROMS][PATH_MAX_LEN];
static int s_rom_count;

// ---- Peanut-GB callbacks (emulator task) ----

static uint8_t rom_read(struct gb_s *gb, const uint_fast32_t addr)
{
    emu_t *e = gb->direct.priv;
    return addr < e->rom_size ? e->rom[addr] : 0xFF;
}

static uint8_t ram_read(struct gb_s *gb, const uint_fast32_t addr)
{
    emu_t *e = gb->direct.priv;
    return addr < e->ram_size ? e->ram[addr] : 0xFF;
}

static void ram_write(struct gb_s *gb, const uint_fast32_t addr, const uint8_t val)
{
    emu_t *e = gb->direct.priv;
    if (addr < e->ram_size && e->ram[addr] != val) {
        e->ram[addr] = val;
        e->ram_dirty = true;
    }
}

static void gb_err(struct gb_s *gb, const enum gb_error_e err, const uint16_t addr)
{
    ESP_LOGE(TAG, "emulation error %d at 0x%04X", (int)err, addr);
    os_toast("Game Boy error %d at %04X", (int)err, addr);
    s_quit = true;
}

// Line y of 144 into rows of the 240x216 back buffer: even lines fill two rows,
// odd lines one (0 -> 0,1; 1 -> 2; 2 -> 3,4 ...). Columns: 2 pixels -> 3.
static void draw_line(struct gb_s *gb, const uint8_t *px, const uint_fast8_t y)
{
    emu_t *e = gb->direct.priv;
    int row = (y & 1) ? (3 * y + 1) / 2 : 3 * y / 2;
    uint16_t *dst = e->buf[1] + row * SCALE_W;
    for (int x = 0; x < LCD_WIDTH; x += 2) {
        uint16_t a = e->pal[px[x] & 3], b = e->pal[px[x + 1] & 3];
        *dst++ = a;
        *dst++ = a;
        *dst++ = b;
    }
    if (!(y & 1)) {
        memcpy(e->buf[1] + (row + 1) * SCALE_W, e->buf[1] + row * SCALE_W, SCALE_W * sizeof(uint16_t));
    }
    if (y == LCD_HEIGHT - 1) {
        e->drawn = true;
    }
}

// ---- Files (emulator task) ----

static bool load_file(const char *path, uint8_t **out, size_t *size, size_t max)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    bool ok = n > 0 && (size_t)n <= max;
    uint8_t *buf = ok ? heap_caps_malloc(n, os_mem_bulk_caps()) : NULL;
    ok = buf && fread(buf, 1, n, f) == (size_t)n;
    fclose(f);
    if (!ok) {
        heap_caps_free(buf);
        return false;
    }
    *out = buf;
    *size = n;
    return true;
}

static void save_ram(emu_t *e)
{
    if (!e->ram_size || !e->ram_dirty) {
        return;
    }
    FILE *f = fopen(e->sav_path, "wb");
    if (f && fwrite(e->ram, 1, e->ram_size, f) == e->ram_size) {
        e->ram_dirty = false;
        ESP_LOGI(TAG, "saved %u B to %s", (unsigned)e->ram_size, e->sav_path);
    } else {
        ESP_LOGW(TAG, "save to %s failed", e->sav_path);
    }
    if (f) {
        fclose(f);
    }
}

// ---- Input ----

static uint8_t joypad(void)
{
    uint32_t b = bsp_buttons_get();
    bool up = b & os_key_button(OS_KEY_UP), down = b & os_key_button(OS_KEY_DOWN);
    bool left = b & os_key_button(OS_KEY_LEFT), right = b & os_key_button(OS_KEY_RIGHT);
    uint8_t held = 0;
    if (up && down) {
        held |= JOYPAD_START;
    } else {
        held |= (up ? JOYPAD_UP : 0) | (down ? JOYPAD_DOWN : 0);
    }
    if (left && right) {
        held |= JOYPAD_SELECT;
    } else {
        held |= (left ? JOYPAD_LEFT : 0) | (right ? JOYPAD_RIGHT : 0);
    }
    if (b & os_key_button(OS_KEY_SELECT)) {
        held |= JOYPAD_A;
    }
    if (b & os_key_button(OS_KEY_BACK)) {
        held |= JOYPAD_B;
    }
    return (uint8_t)~held;      // Peanut-GB: 0 = pressed
}

// ---- Emulator task ----

// Show the back buffer: swap under the LVGL lock, or keep drawing into the same
// back buffer and try again next frame if LVGL is mid-redraw.
static bool present(emu_t *e)
{
    if (!lvgl_port_lock(LOCK_WAIT_MS)) {
        return false;
    }
    if (!s_quit && s_img) {
        uint16_t *t = e->buf[0];
        e->buf[0] = e->buf[1];
        e->buf[1] = t;
        lv_image_dsc_t *d = s_dsc[0].data == (const uint8_t *)e->buf[0] ? &s_dsc[0] : &s_dsc[1];
        lv_image_cache_drop(d);
        lv_image_set_src(s_img, d);
    }
    lvgl_port_unlock();
    return true;
}

static void emu_task(void *arg)
{
    emu_t *e = s_emu;
    if (!load_file(e->rom_path, &e->rom, &e->rom_size, ROM_MAX)) {
        os_toast("Can't read that ROM (max 4 MB)");
        goto out;
    }
    enum gb_init_error_e ie = gb_init(&e->gb, rom_read, ram_read, ram_write, gb_err, e);
    if (ie != GB_INIT_NO_ERROR) {
        os_toast(ie == GB_INIT_CARTRIDGE_UNSUPPORTED ? "Cartridge type not supported" : "ROM header checksum is wrong");
        goto out;
    }
    size_t want = 0;
    gb_get_save_size_s(&e->gb, &want);
    if (want) {
        uint8_t *sav = NULL;
        size_t got = 0;
        if (load_file(e->sav_path, &sav, &got, want) && got == want) {
            e->ram = sav;
        } else {
            heap_caps_free(sav);
            e->ram = heap_caps_calloc(1, want, os_mem_bulk_caps());
        }
        if (!e->ram) {
            os_toast("Out of memory for save RAM");
            goto out;
        }
        e->ram_size = want;
    }
    gb_init_lcd(&e->gb, draw_line);
    e->gb.direct.frame_skip = true;     // draw every other frame (~30 fps), emulate all 60
    char title[17];
    ESP_LOGI(TAG, "%s: %u KB ROM, %u B cart RAM", gb_get_rom_name(&e->gb, title),
             (unsigned)(e->rom_size >> 10), (unsigned)e->ram_size);

    int64_t next = esp_timer_get_time(), last_save = next, stat_t = next;
    int frames = 0, shown = 0, late = 0;
    while (!s_quit) {
        e->gb.direct.joypad = joypad();
        gb_run_frame(&e->gb);
        if (e->drawn) {
            e->drawn = false;
            shown += present(e);
        }
        int64_t now = esp_timer_get_time();
        next += FRAME_US;
        if (next - now >= 10000) {
            vTaskDelay(pdMS_TO_TICKS((next - now) / 1000));
        } else {
            if (now - next > 100000) {      // over 100 ms behind: drop it, don't race to catch up
                next = now;
                late++;
            }
            vTaskDelay(0);                  // yield: idle task, browser scheduler
        }
        frames++;
        if (now - stat_t >= 5000000) {      // speed check on the console
            double s = (now - stat_t) / 1e6;
            ESP_LOGI(TAG, "%.1f emulated fps (target 59.7), %.1f shown, %d resyncs", frames / s, shown / s, late);
            stat_t = now;
            frames = shown = late = 0;
        }
        if (e->ram_dirty && now - last_save >= SAVE_EVERY_US) {
            save_ram(e);
            last_save = now;
        }
    }
    save_ram(e);
out:
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

// ---- ROM list (OS task) ----

static bool is_rom(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && (!strcasecmp(dot, ".gb") || !strcasecmp(dot, ".gbc"));
}

static void scan(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) {
        return;
    }
    struct dirent *de;
    while ((de = readdir(d)) && s_rom_count < MAX_ROMS) {
        size_t dl = strlen(dir), nl = strlen(de->d_name);
        if (is_rom(de->d_name) && dl + 1 + nl < PATH_MAX_LEN) {    // longer paths are skipped
            char *p = s_roms[s_rom_count++];
            memcpy(p, dir, dl);
            p[dl] = '/';
            memcpy(p + dl + 1, de->d_name, nl + 1);
        }
    }
    closedir(d);
}

static void rom_row(int i, ui_row_t *row, void *ctx)
{
    const char *slash = strrchr(s_roms[i], '/');
    snprintf(row->title, sizeof(row->title), "%s", slash ? slash + 1 : s_roms[i]);
    snprintf(row->sub, sizeof(row->sub), "%s", s_roms[i]);
}

static void message(const char *text)
{
    s_msg = ui_label(s_root, &sol_font_14, COL_TEXT, text);
    lv_obj_set_width(s_msg, OS_ROOT_W - 32);
    lv_obj_set_style_text_align(s_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_msg, LV_ALIGN_CENTER, 0, -8);
}

// ---- Start / stop (OS task) ----

static void start(const char *path)
{
    // The core's state is touched every cycle: SRAM when the badge can spare it.
    emu_t *e = NULL;
    if (heap_caps_get_free_size(OS_MEM_SRAM_MALLOC) > sizeof(*e) + SRAM_KEEP) {
        e = heap_caps_calloc(1, sizeof(*e), OS_MEM_SRAM);
    }
    if (!e) {
        e = heap_caps_calloc(1, sizeof(*e), os_mem_bulk_caps());
    }
    for (int i = 0; e && i < 2; i++) {
        e->buf[i] = heap_caps_calloc(SCALE_W * SCALE_H, sizeof(uint16_t), os_mem_bulk_caps());
        if (!e->buf[i]) {
            heap_caps_free(e->buf[0]);
            heap_caps_free(e);
            e = NULL;
        }
    }
    if (!e) {
        os_toast("Out of memory");
        return;
    }
    snprintf(e->rom_path, sizeof(e->rom_path), "%s", path);
    snprintf(e->sav_path, sizeof(e->sav_path), "%s", path);
    char *dot = strrchr(e->sav_path, '.');
    snprintf(dot, sizeof(e->sav_path) - (dot - e->sav_path), ".sav");
    for (int i = 0; i < 4; i++) {
        e->pal[i] = lv_color_to_u16(lv_color_hex(PALETTE_HEX[i]));
    }
    for (int i = 0; i < SCALE_W * SCALE_H; i++) {
        e->buf[0][i] = e->pal[0];
    }
    for (int i = 0; i < 2; i++) {
        s_dsc[i] = (lv_image_dsc_t){
            .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
                       .w = SCALE_W, .h = SCALE_H, .stride = SCALE_W * 2},
            .data_size = SCALE_W * SCALE_H * 2,
            .data = (const uint8_t *)e->buf[i],
        };
    }

    if (s_list) {
        ui_list_show(s_list, false);
    }
    if (s_hint) {
        lv_obj_delete(s_hint);
        s_hint = NULL;
    }
    if (s_msg) {
        lv_obj_delete(s_msg);
        s_msg = NULL;
    }
    lv_obj_set_style_bg_color(s_root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    s_img = lv_image_create(s_root);
    lv_image_set_src(s_img, &s_dsc[0]);
    lv_obj_align(s_img, LV_ALIGN_CENTER, 0, 0);

    s_emu = e;
    s_quit = false;
    s_done = xSemaphoreCreateBinary();
    if (xTaskCreate(emu_task, "gbemu", 6144, NULL, tskIDLE_PRIORITY, &s_task) != pdPASS) {
        os_toast("Can't start the emulator task");
        s_task = NULL;
        return;
    }
    os_toast("BOOT exit  " LV_SYMBOL_BULLET "  UP+DOWN start  " LV_SYMBOL_BULLET "  LEFT+RIGHT select");
}

static void enter(lv_obj_t *root)
{
    s_root = root;
    s_list = NULL;
    s_msg = s_img = s_hint = NULL;
    s_task = NULL;
    s_rom_count = 0;

    bsp_sd_info_t in;
    bsp_sd_get_info(&in);
    if (in.state != BSP_SD_MOUNTED) {
        message("No microSD card.\nInsert one with a .gb file in /roms.");
        return;
    }
    scan(BSP_SD_MOUNT "/roms");
    scan(BSP_SD_MOUNT);
    if (!s_rom_count) {
        message("No ROM found.\nCopy a .gb file to /roms on the card.");
        return;
    }
    s_list = ui_list_create(root, 0, OS_ROOT_H - UI_HINT_H, rom_row, NULL);
    ui_list_set_count(s_list, s_rom_count);
    s_hint = ui_hint(root, LV_SYMBOL_UP LV_SYMBOL_DOWN " choose   SELECT play   CANCEL exit");
}

static void leave(void)
{
    if (s_task) {
        s_quit = true;
        // The task saves the cart RAM and exits; it never waits on the LVGL lock
        // we hold here (present() gives up after 1 ms and sees s_quit).
        if (xSemaphoreTake(s_done, pdMS_TO_TICKS(3000)) != pdTRUE) {
            ESP_LOGW(TAG, "emulator task did not stop; leaking it");
            s_emu = NULL;
        }
        s_task = NULL;
    }
    if (s_done) {
        vSemaphoreDelete(s_done);
        s_done = NULL;
    }
    if (s_emu) {
        heap_caps_free(s_emu->rom);
        heap_caps_free(s_emu->ram);
        heap_caps_free(s_emu->buf[0]);
        heap_caps_free(s_emu->buf[1]);
        heap_caps_free(s_emu);
        s_emu = NULL;
    }
    s_img = NULL;
}

static bool key(os_key_t k, bool repeat)
{
    if (s_task) {
        return true;                    // the game reads the buttons itself; only HOME leaves
    }
    if (s_list && k == OS_KEY_SELECT && !repeat) {
        int i = ui_list_selected(s_list);
        if (i >= 0) {
            start(s_roms[i]);
        }
        return true;
    }
    return s_list && ui_list_key(s_list, k, repeat);
}

const os_app_t app_gbemu = {
    .id = "gbemu",
    .name = "Game Boy",
    .symbol = LV_SYMBOL_PLAY,
    .accent = COL_GREEN,
    .on_enter = enter,
    .on_exit = leave,
    .on_key = key,
};
