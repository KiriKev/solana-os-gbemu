// Retro: Game Boy, Game Boy Color and NES games from the microSD card.
//
// Pick a console, then a game from its folder on the card:
//   roms/gb  (and roms/, where earlier versions looked)   .gb .gbc
//   roms/gbc                                               .gb .gbc
//   roms/nes                                               .nes
// No game ships with the firmware: the user copies their own files to the card
// (tools/sd_put.py does it over USB).
//
// Cores, vendored next to this file, MIT: Walnut-CGB (walnut_cgb.h, Game Boy and
// Game Boy Color; it picks the mode from the cartridge header) and agnes
// (nes/agnes.c, NES).
//
// Threads: the OS task runs the callbacks below (LVGL locked). The emulator
// runs in its own task at idle priority so the OS, LVGL and radios always
// preempt it, and it yields every frame (the browser emulator's tasks are
// cooperative). Every other frame lands in one of two RGB565 buffers; the task
// swaps the image's source under the LVGL lock, so LVGL never reads a buffer
// that is being written.
//
// Screen: Game Boy 160x144 scaled 1.5x to 240x216; NES 256x240 shown 1:1 with
// 11 rows cut at the top and bottom (256x218), the app root's height.
// Keys (calibrated map): d-pad = d-pad, SELECT = A, CANCEL = B,
// UP+DOWN = START, LEFT+RIGHT = SELECT (opposite directions never reach a
// game). BOOT (HOME) leaves the app. Saves: Game Boy cart RAM in <rom>.sav, on
// quit and every minute while it changes; NES the whole machine in
// <rom>.state, on quit, restored at the next start.

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
#include "nes/agnes.h"

#define ENABLE_SOUND                0       // no speaker on the badge
#define ENABLE_LCD                  1
#define WALNUT_GB_IS_LITTLE_ENDIAN  1       // RISC-V and wasm
#define WALNUT_FULL_GBC_SUPPORT     1
struct gb_s;                                // walnut_cgb.h uses it in prototypes before defining it
#include "walnut_cgb.h"

static const char *TAG = "retro";

#define GB_W            240                 // 160 x 1.5
#define GB_H            216                 // 144 x 1.5
#define NES_W           AGNES_SCREEN_WIDTH
#define NES_H           OS_ROOT_H           // 218
#define NES_CROP        ((AGNES_SCREEN_HEIGHT - NES_H) / 2)
#define BUF_PX          (NES_W * NES_H)     // the bigger picture of the two
#define GB_FRAME_US     16743               // 59.73 Hz
#define NES_FRAME_US    16639               // 60.10 Hz (NTSC)
#define SAVE_EVERY_US   (60 * 1000000LL)
#define ROM_MAX         (4u << 20)
#define MAX_ROMS        48
#define PATH_MAX_LEN    200
#define SRAM_KEEP       (64 * 1024)         // SRAM left for Wi-Fi, LVGL, stacks before the core may take some
#define LOCK_WAIT_MS    4                   // how long a frame waits for LVGL to finish a redraw

typedef enum { SYS_GB, SYS_GBC, SYS_NES, SYS_COUNT } sys_t;

static const struct {
    const char *name;
    const char *dirs[2];                    // scanned in order; NULL = none
    bool nes;
} SYSTEMS[SYS_COUNT] = {
    [SYS_GB] = {"Game Boy", {"roms/gb", "roms"}, false},
    [SYS_GBC] = {"Game Boy Color", {"roms/gbc", NULL}, false},
    [SYS_NES] = {"NES", {"roms/nes", NULL}, true},
};

// Original Game Boy games: a neutral 4-shade palette, lightest first.
static const uint32_t DMG_HEX[4] = {0xE0F8D0, 0x88C070, 0x346856, 0x081820};

typedef struct {
    sys_t sys;
    struct gb_s gb;
    agnes_t *nes;
    uint8_t *rom;
    size_t rom_size;
    uint8_t *ram;                           // Game Boy cart RAM
    size_t ram_size;
    bool ram_dirty;
    uint16_t pal[4];
    uint16_t *buf[2];                       // [front, back]; the image shows buf[0]
    bool drawn;                             // a whole frame reached the back buffer
    char rom_path[PATH_MAX_LEN];
    char sav_path[PATH_MAX_LEN];            // <rom>.sav or <rom>.state
} emu_t;

typedef enum { VIEW_SYSTEMS, VIEW_GAMES, VIEW_PLAYING } view_t;

static emu_t *s_emu;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_done;
static volatile bool s_quit;
static lv_image_dsc_t s_dsc[2];
static lv_obj_t *s_root, *s_img, *s_msg, *s_hint;
static ui_list_t *s_list;
static view_t s_view;
static sys_t s_sys;
static int s_counts[SYS_COUNT];
static char s_roms[MAX_ROMS][PATH_MAX_LEN];
static int s_rom_count;

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// ---- Walnut-CGB callbacks (emulator task) ----

static uint8_t rom_read(struct gb_s *gb, const uint_fast32_t addr)
{
    emu_t *e = gb->direct.priv;
    return addr < e->rom_size ? e->rom[addr] : 0xFF;
}

// Byte by byte: PSRAM and odd addresses don't mix with wide loads.
static uint16_t rom_read16(struct gb_s *gb, const uint_fast32_t addr)
{
    emu_t *e = gb->direct.priv;
    const uint8_t *p = e->rom + addr;
    return addr + 1 < e->rom_size ? (uint16_t)(p[0] | p[1] << 8) : 0xFFFF;
}

static uint32_t rom_read32(struct gb_s *gb, const uint_fast32_t addr)
{
    emu_t *e = gb->direct.priv;
    const uint8_t *p = e->rom + addr;
    return addr + 3 < e->rom_size ? p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24 : 0xFFFFFFFF;
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
// Colour games index the core's RGB565 palette; others get the 4 shades.
static void draw_line(struct gb_s *gb, const uint8_t *px, const uint_fast8_t y)
{
    emu_t *e = gb->direct.priv;
    const uint16_t *cgb = gb->cgb.cgbMode ? gb->cgb.fixPalette : NULL;
    int row = (y & 1) ? (3 * y + 1) / 2 : 3 * y / 2;
    uint16_t *dst = e->buf[1] + row * GB_W;
    for (int x = 0; x < LCD_WIDTH; x += 2) {
        uint16_t a = cgb ? cgb[px[x] & 0x3F] : e->pal[px[x] & 3];
        uint16_t b = cgb ? cgb[px[x + 1] & 0x3F] : e->pal[px[x + 1] & 3];
        *dst++ = a;
        *dst++ = a;
        *dst++ = b;
    }
    if (!(y & 1)) {
        memcpy(e->buf[1] + (row + 1) * GB_W, e->buf[1] + row * GB_W, GB_W * sizeof(uint16_t));
    }
    if (y == LCD_HEIGHT - 1) {
        e->drawn = true;
    }
}

// The NES picture, rows NES_CROP.. of 240, into the back buffer.
static void nes_draw(emu_t *e)
{
    uint16_t *dst = e->buf[1];
    for (int y = 0; y < NES_H; y++) {
        for (int x = 0; x < NES_W; x++) {
            agnes_color_t c = agnes_get_screen_pixel(e->nes, x, y + NES_CROP);
            *dst++ = rgb565(c.r, c.g, c.b);
        }
    }
    e->drawn = true;
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

static bool write_file(const char *path, const void *data, size_t n)
{
    FILE *f = fopen(path, "wb");
    bool ok = f && fwrite(data, 1, n, f) == n;
    if (f && fclose(f) != 0) {
        ok = false;
    }
    ESP_LOGI(TAG, "%s %u B to %s", ok ? "saved" : "FAILED to save", (unsigned)n, path);
    return ok;
}

static void save(emu_t *e)
{
    if (e->nes) {
        size_t n = agnes_state_size();
        agnes_state_t *st = heap_caps_malloc(n, os_mem_bulk_caps());
        if (st) {
            agnes_dump_state(e->nes, st);
            write_file(e->sav_path, st, n);
            heap_caps_free(st);
        }
    } else if (e->ram_size && e->ram_dirty && write_file(e->sav_path, e->ram, e->ram_size)) {
        e->ram_dirty = false;
    }
}

// ---- Input ----

// Held buttons as JOYPAD_* bits (1 = pressed).
static uint8_t buttons(void)
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
    return held;
}

static void nes_input(emu_t *e, uint8_t held)
{
    agnes_input_t in = {
        .a = held & JOYPAD_A, .b = held & JOYPAD_B, .select = held & JOYPAD_SELECT, .start = held & JOYPAD_START,
        .up = held & JOYPAD_UP, .down = held & JOYPAD_DOWN, .left = held & JOYPAD_LEFT, .right = held & JOYPAD_RIGHT,
    };
    agnes_set_input(e->nes, &in, NULL);
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

static bool start_gb(emu_t *e)
{
    enum gb_init_error_e ie = gb_init(&e->gb, rom_read, rom_read16, rom_read32, ram_read, ram_write, gb_err, e);
    if (ie != GB_INIT_NO_ERROR) {
        os_toast(ie == GB_INIT_CARTRIDGE_UNSUPPORTED ? "Cartridge type not supported" : "ROM header checksum is wrong");
        return false;
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
            return false;
        }
        e->ram_size = want;
    }
    gb_init_lcd(&e->gb, draw_line);
    e->gb.direct.frame_skip = true;         // draw every other frame (~30 fps), emulate all 60
    char title[17];
    ESP_LOGI(TAG, "%s: %u KB ROM, %u B cart RAM, %s mode", gb_get_rom_name(&e->gb, title),
             (unsigned)(e->rom_size >> 10), (unsigned)e->ram_size, e->gb.cgb.cgbMode ? "colour" : "original");
    return true;
}

static bool start_nes(emu_t *e)
{
    e->nes = agnes_make();
    if (!e->nes) {
        os_toast("Out of memory");
        return false;
    }
    if (!agnes_load_ines_data(e->nes, e->rom, e->rom_size)) {
        os_toast("Not an NES game this emulator can run");
        return false;
    }
    uint8_t *st = NULL;
    size_t got = 0;
    if (load_file(e->sav_path, &st, &got, agnes_state_size())) {
        bool ok = got == agnes_state_size() && agnes_restore_state(e->nes, (agnes_state_t *)st);
        ESP_LOGI(TAG, "state %s", ok ? "restored" : "ignored (doesn't fit)");
        heap_caps_free(st);
    }
    ESP_LOGI(TAG, "NES: %u KB ROM", (unsigned)(e->rom_size >> 10));
    return true;
}

static void emu_task(void *arg)
{
    emu_t *e = s_emu;
    bool nes = SYSTEMS[e->sys].nes;
    if (!load_file(e->rom_path, &e->rom, &e->rom_size, ROM_MAX)) {
        os_toast("Can't read that game (max 4 MB)");
        goto out;
    }
    if (!(nes ? start_nes(e) : start_gb(e))) {
        goto out;
    }
    const int64_t frame_us = nes ? NES_FRAME_US : GB_FRAME_US;
    int64_t next = esp_timer_get_time(), last_save = next, stat_t = next;
    int frames = 0, shown = 0, late = 0;
    int64_t busy_sum = 0, busy_max = 0;     // time spent emulating and drawing, the core's real cost
    while (!s_quit) {
        int64_t t0 = esp_timer_get_time();
        uint8_t held = buttons();
        if (nes) {
            nes_input(e, held);
            agnes_next_frame(e->nes);
            if (frames & 1) {               // every other frame, like the Game Boy's frame skip
                nes_draw(e);
            }
        } else {
            e->gb.direct.joypad = (uint8_t)~held;   // Walnut-CGB: 0 = pressed
            gb_run_frame(&e->gb);
        }
        int64_t busy = esp_timer_get_time() - t0;
        busy_sum += busy;
        busy_max = busy > busy_max ? busy : busy_max;
        if (e->drawn) {
            e->drawn = false;
            shown += present(e);
        }
        int64_t now = esp_timer_get_time();
        next += frame_us;
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
            ESP_LOGI(TAG, "%s: %.1f emulated fps (target %.1f), %.1f shown, %d resyncs; "
                     "emulate+draw %.2f ms/frame avg, %.2f max (budget %.2f)", SYSTEMS[e->sys].name,
                     frames / s, 1e6 / frame_us, shown / s, late,
                     busy_sum / 1000.0 / frames, busy_max / 1000.0, frame_us / 1000.0);
            stat_t = now;
            frames = shown = late = 0;
            busy_sum = busy_max = 0;
        }
        if (!nes && e->ram_dirty && now - last_save >= SAVE_EVERY_US) {
            save(e);
            last_save = now;
        }
    }
    save(e);
out:
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

// ---- Menus (OS task) ----

static bool is_game(sys_t sys, const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) {
        return false;
    }
    return SYSTEMS[sys].nes ? !strcasecmp(dot, ".nes") : (!strcasecmp(dot, ".gb") || !strcasecmp(dot, ".gbc"));
}

// Fills s_roms with sys's games (list = false only counts them).
static int scan(sys_t sys, bool list)
{
    int n = 0;
    for (int i = 0; i < 2 && SYSTEMS[sys].dirs[i]; i++) {
        char dir[64];
        snprintf(dir, sizeof(dir), BSP_SD_MOUNT "/%s", SYSTEMS[sys].dirs[i]);
        DIR *d = opendir(dir);
        if (!d) {
            continue;
        }
        struct dirent *de;
        while ((de = readdir(d)) && n < MAX_ROMS) {
            size_t dl = strlen(dir), nl = strlen(de->d_name);
            if (!is_game(sys, de->d_name) || dl + 1 + nl >= PATH_MAX_LEN) {     // longer paths are skipped
                continue;
            }
            if (list) {
                char *p = s_roms[n];
                memcpy(p, dir, dl);
                p[dl] = '/';
                memcpy(p + dl + 1, de->d_name, nl + 1);
            }
            n++;
        }
        closedir(d);
    }
    return n;
}

static void row(int i, ui_row_t *r, void *ctx)
{
    if (s_view == VIEW_SYSTEMS) {
        snprintf(r->title, sizeof(r->title), "%s", SYSTEMS[i].name);
        snprintf(r->sub, sizeof(r->sub), "/%s", SYSTEMS[i].dirs[0]);
        snprintf(r->right, sizeof(r->right), "%d game%s", s_counts[i], s_counts[i] == 1 ? "" : "s");
        r->right_color = s_counts[i] ? 0 : COL_DIM;
    } else {
        const char *slash = strrchr(s_roms[i], '/');
        snprintf(r->title, sizeof(r->title), "%s", slash ? slash + 1 : s_roms[i]);
        snprintf(r->sub, sizeof(r->sub), "%s", s_roms[i] + strlen(BSP_SD_MOUNT));
    }
}

static void hint(const char *text)
{
    if (s_hint) {
        lv_obj_delete(s_hint);
    }
    s_hint = text ? ui_hint(s_root, text) : NULL;
}

static void show_systems(void)
{
    s_view = VIEW_SYSTEMS;
    for (int i = 0; i < SYS_COUNT; i++) {
        s_counts[i] = scan(i, false);
    }
    ui_list_set_count(s_list, SYS_COUNT);
    ui_list_select(s_list, s_sys);
    hint(LV_SYMBOL_UP LV_SYMBOL_DOWN " console   SELECT open   CANCEL exit");
}

static void show_games(sys_t sys)
{
    s_rom_count = scan(sys, true);
    if (!s_rom_count) {
        os_toast("No games in /%s on the card", SYSTEMS[sys].dirs[0]);
        return;
    }
    s_sys = sys;
    s_view = VIEW_GAMES;
    ui_list_set_count(s_list, s_rom_count);
    ui_list_select(s_list, 0);
    hint(LV_SYMBOL_UP LV_SYMBOL_DOWN " game   SELECT play   CANCEL back");
}

static void message(const char *text)
{
    s_msg = ui_label(s_root, &sol_font_14, COL_TEXT, text);
    lv_obj_set_width(s_msg, OS_ROOT_W - 32);
    lv_obj_set_style_text_align(s_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_msg, LV_ALIGN_CENTER, 0, -8);
}

// ---- Start / stop (OS task) ----

static void start(sys_t sys, const char *path)
{
    bool nes = SYSTEMS[sys].nes;
    // The Game Boy core's state is touched every cycle: SRAM when the badge can spare it.
    emu_t *e = NULL;
    if (!nes && heap_caps_get_free_size(OS_MEM_SRAM_MALLOC) > sizeof(*e) + SRAM_KEEP) {
        e = heap_caps_calloc(1, sizeof(*e), OS_MEM_SRAM);
    }
    if (!e) {
        e = heap_caps_calloc(1, sizeof(*e), os_mem_bulk_caps());
    }
    for (int i = 0; e && i < 2; i++) {
        e->buf[i] = heap_caps_calloc(BUF_PX, sizeof(uint16_t), os_mem_bulk_caps());
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
    e->sys = sys;
    snprintf(e->rom_path, sizeof(e->rom_path), "%s", path);
    snprintf(e->sav_path, sizeof(e->sav_path), "%s", path);
    char *dot = strrchr(e->sav_path, '.');
    snprintf(dot, sizeof(e->sav_path) - (dot - e->sav_path), nes ? ".state" : ".sav");
    for (int i = 0; i < 4; i++) {
        e->pal[i] = lv_color_to_u16(lv_color_hex(DMG_HEX[i]));
    }
    int w = nes ? NES_W : GB_W, h = nes ? NES_H : GB_H;
    for (int i = 0; i < 2; i++) {
        s_dsc[i] = (lv_image_dsc_t){
            .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
                       .w = w, .h = h, .stride = w * 2},
            .data_size = w * h * 2,
            .data = (const uint8_t *)e->buf[i],
        };
    }

    ui_list_show(s_list, false);
    hint(NULL);
    s_view = VIEW_PLAYING;
    lv_obj_set_style_bg_color(s_root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    s_img = lv_image_create(s_root);
    lv_image_set_src(s_img, &s_dsc[0]);
    lv_obj_align(s_img, LV_ALIGN_CENTER, 0, 0);

    s_emu = e;
    s_quit = false;
    s_done = xSemaphoreCreateBinary();
    if (xTaskCreate(emu_task, "retro", 6144, NULL, tskIDLE_PRIORITY, &s_task) != pdPASS) {
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

    bsp_sd_info_t in;
    bsp_sd_get_info(&in);
    if (in.state != BSP_SD_MOUNTED) {
        message("No microSD card.\nInsert one with games in /roms/gb, /roms/gbc or /roms/nes.");
        return;
    }
    s_list = ui_list_create(root, 0, OS_ROOT_H - UI_HINT_H, row, NULL);
    show_systems();
}

static void leave(void)
{
    if (s_task) {
        s_quit = true;
        // The task saves and exits; it never waits on the LVGL lock we hold here
        // (present() gives up after LOCK_WAIT_MS and sees s_quit).
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
        if (s_emu->nes) {
            agnes_destroy(s_emu->nes);
        }
        heap_caps_free(s_emu->rom);
        heap_caps_free(s_emu->ram);
        heap_caps_free(s_emu->buf[0]);
        heap_caps_free(s_emu->buf[1]);
        heap_caps_free(s_emu);
        s_emu = NULL;
    }
    s_img = NULL;
    s_hint = NULL;
}

static bool key(os_key_t k, bool repeat)
{
    if (s_view == VIEW_PLAYING || !s_list) {
        return s_view == VIEW_PLAYING;      // playing: the game reads the buttons, only HOME leaves
    }
    if (k == OS_KEY_SELECT && !repeat) {
        int i = ui_list_selected(s_list);
        if (i >= 0 && s_view == VIEW_SYSTEMS) {
            show_games(i);
        } else if (i >= 0) {
            start(s_sys, s_roms[i]);
        }
        return true;
    }
    if (k == OS_KEY_BACK && s_view == VIEW_GAMES) {
        if (!repeat) {
            show_systems();
        }
        return true;
    }
    return ui_list_key(s_list, k, repeat);  // BACK on the console list: not handled, the OS exits
}

const os_app_t app_gbemu = {
    .id = "retro",
    .name = "Retro",
    .symbol = LV_SYMBOL_PLAY,
    .accent = COL_GREEN,
    .on_enter = enter,
    .on_exit = leave,
    .on_key = key,
};
