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
// quit and a second after the game last wrote it (at least every minute while
// it keeps writing); NES the whole machine in
// <rom>.state, on quit, restored at the next start.
//
// Link cable (Game Boy and Game Boy Color): the console list's last row picks a
// nearby badge; both players pick each other, check that both screens show the
// same 6-digit code and accept. The serial port's bytes then go over the badge
// mesh (os_mesh APP messages) instead of a cable, tagged and encrypted with
// keys only the two badges hold ("Link pairing" below).

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "psa/crypto.h"
#include "core/os_cloud.h"
#include "esp_console.h"
#include "core/os.h"
#include "core/os_mem.h"
#include "core/os_mesh.h"
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
#define GB_X            ((OS_ROOT_W - GB_W) / 2)
#define GB_Y            (OS_STATUS_H + (OS_ROOT_H - GB_H) / 2)
#define NES_X           ((OS_ROOT_W - NES_W) / 2)
#define NES_Y           OS_STATUS_H
#define CHUNK_LINES     8                   // Game Boy lines per blit: 8 lines -> 12 rows
#define CHUNK_ROWS      12                  // screen rows per blit
#define CHUNK_PX        (NES_W * CHUNK_ROWS) // the wider picture of the two
#define FRAME_PX        (NES_W * NES_H)     // the bigger picture of the two
#define DISP_PRIO       2                   // above the emulator, below LVGL and the OS
#define GB_FRAME_US     16743               // 59.73 Hz
#define NES_FRAME_US    16639               // 60.10 Hz (NTSC)
#define SAVE_QUIET_US   (1000000LL)          // a save is written once the game stopped writing for this long
#define SAVE_EVERY_US   (60 * 1000000LL)     // or this long after the last one, for games that never stop
#define ROM_MAX         (4u << 20)
#define MAX_ROMS        48
#define PATH_MAX_LEN    200
#define SRAM_KEEP       (64 * 1024)         // SRAM left for Wi-Fi, LVGL, stacks before the core may take some

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
    bool ram_dirty;                         // not on the card yet
    bool ram_touched;                       // written during this frame
    uint16_t pal[4];                        // big-endian, as the panel takes it
    uint16_t *fb[2];                        // whole frames, PSRAM: the emulator fills fb[back]
    int back;
    volatile int show;                      // the frame the display task sends
    volatile bool disp_busy;                // it's still sending it
    bool drawn;                             // a frame was handed over
    uint16_t *chunk[2];                     // blit buffers, internal DMA RAM; one fills while the other is sent
    int x, y, w, h;                         // where the picture goes on the screen
    TaskHandle_t disp;
    SemaphoreHandle_t disp_done;
    char rom_path[PATH_MAX_LEN];
    char sav_path[PATH_MAX_LEN];            // <rom>.sav or <rom>.state
    // The task's own handles: if it ever outlives leave()'s wait, leave() forgets
    // the globals without deleting these, so the task never touches freed ones.
    QueueHandle_t link_q;                   // NULL = no cable
    SemaphoreHandle_t done;
} emu_t;

typedef enum { VIEW_SYSTEMS, VIEW_GAMES, VIEW_LINK, VIEW_PAIR, VIEW_PLAYING } view_t;

// Link cable over the badge mesh (Game Boy and Game Boy Color only).
// The clocking side (internal clock) numbers its transfers and sends each byte
// with its number n. The clocked side sends the byte it has ready, tagged with
// the number of the transfer it answers (the last n it took, plus one), once,
// and again every LINK_RESEND_MS while it waits, in case a frame got lost. The
// clocking side takes only the answer to its current transfer, waiting up to
// LINK_WAIT_MS; the clocked side takes the next byte the partner clocked.
// Anything else is stale and dropped, so a lost or late byte costs one
// transfer (the game sees "no cable" for it), never a lasting shift.
#define LINK_APP        "retro-link"
#define LINK_WAIT_MS    60
#define LINK_RESEND_MS  200
#define LINK_QUEUE      16
#define LINK_PEERS_MAX  12
#define MESH_PEERS_MAX  20                  // os_mesh.c's table
#define ROW_LINK        SYS_COUNT           // the console list's last row
#define PAIR_RESEND_MS  400
#define PAIR_QUEUE      8

typedef struct {
    uint8_t b;
    bool master;                            // the partner clocked this transfer
    uint32_t n;                             // its transfer number, or the one it answers
} link_byte_t;

// The keys of an accepted pairing: tag (HMAC) and encrypt every link byte.
// Each side tags with its own letter ('a': its public key sorts first), so a
// byte can't be sent back to the badge it came from.
typedef struct {
    uint8_t ktag[16], kenc[16];
    char me, peer;
} link_keys_t;

static char s_link_id[OS_MESH_ID_LEN + 1];  // partner badge, "" = no cable; kept while the badge runs
static char s_link_name[25];
static link_keys_t s_link_keys;             // set with s_link_id, read by the mesh and emulator tasks
static uint32_t s_link_tx_c;                // emulator task: number of the last message sent
static uint32_t s_link_rx_c;                // mesh task: number of the last message taken
static QueueHandle_t s_link_q;              // the mesh handler's; the emulator task uses emu_t.link_q
static int s_link_sub = -1;
static bool s_link_held;                    // os_mesh_acquire() taken
static uint32_t s_link_seq;                 // clocking: number of the current transfer
static uint32_t s_link_last;                // clocked: number of the last transfer taken
static int s_link_sent = -1;                // clocked: byte already sent for the waiting transfer
static uint32_t s_link_sent_n;
static TickType_t s_link_sent_at;
static os_mesh_peer_t s_peers[LINK_PEERS_MAX];
static int s_peer_count, s_tick;

static emu_t *s_emu;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_done;
static volatile bool s_quit;
static lv_obj_t *s_root, *s_msg, *s_hint;
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
        e->ram_dirty = e->ram_touched = true;
    }
}

static void gb_err(struct gb_s *gb, const enum gb_error_e err, const uint16_t addr)
{
    ESP_LOGE(TAG, "emulation error %d at 0x%04X", (int)err, addr);
    os_toast("Game Boy error %d at %04X", (int)err, addr);
    s_quit = true;
}

// ---- Link cable (mesh task and emulator task) ----

static void to_hex(const uint8_t *b, int n, char *out)
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < n; i++) {
        out[i * 2] = hex[b[i] >> 4];
        out[i * 2 + 1] = hex[b[i] & 15];
    }
    out[n * 2] = 0;
}

static int hex_val(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

// Exactly 2n lowercase hex digits, then the closing quote.
static bool from_hex(const char *in, uint8_t *out, int n)
{
    for (int i = 0; i < n; i++) {
        int hi = hex_val(in[i * 2]), lo = hi < 0 ? -1 : hex_val(in[i * 2 + 1]);
        if (lo < 0) {
            return false;
        }
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return in[n * 2] == '"';
}

// One-byte pad for message c from side dir: never repeats within a pairing.
static uint8_t link_pad(const link_keys_t *k, char dir, uint32_t c)
{
    char in[24], tag[OS_MESH_TAG_LEN + 1];
    snprintf(in, sizeof(in), "p|%c|%lu", dir, (unsigned long)c);
    os_mesh_tag(k->kenc, in, tag);
    return (uint8_t)(hex_val(tag[0]) << 4 | hex_val(tag[1]));
}

static void link_tag(const link_keys_t *k, char dir, unsigned be, int m, unsigned long n, uint32_t c,
                     char out[OS_MESH_TAG_LEN + 1])
{
    char in[64];
    snprintf(in, sizeof(in), "t|%c|%u|%d|%lu|%lu", dir, be, m, n, (unsigned long)c);
    os_mesh_tag(k->ktag, in, out);
}

// The message for byte b as message number c from this side.
static void link_seal(const link_keys_t *k, uint32_t c, uint8_t b, bool master, uint32_t n, char *out, size_t len)
{
    unsigned be = b ^ link_pad(k, k->me, c);
    char tag[OS_MESH_TAG_LEN + 1];
    link_tag(k, k->me, be, master, n, c, tag);
    snprintf(out, len, "{\"app\":\"" LINK_APP "\",\"b\":%u,\"m\":%d,\"n\":%lu,\"c\":%lu,\"t\":\"%s\"}", be, master,
             (unsigned long)n, (unsigned long)c, tag);
}

// The partner's byte, if json is a message it sealed after message last_c.
static bool link_open(const link_keys_t *k, const char *json, uint32_t last_c, link_byte_t *v, uint32_t *c)
{
    const char *b = strstr(json, "\"b\":"), *m = strstr(json, "\"m\":"), *n = strstr(json, "\"n\":");
    const char *cc = strstr(json, "\"c\":"), *t = strstr(json, "\"t\":\"");
    if (!b || !m || !n || !cc || !t || strlen(t + 5) < OS_MESH_TAG_LEN + 1 || t[5 + OS_MESH_TAG_LEN] != '"') {
        return false;
    }
    unsigned long be = strtoul(b + 4, NULL, 10), nv = strtoul(n + 4, NULL, 10), cv = strtoul(cc + 4, NULL, 10);
    int mv = m[4] == '1';
    if (be > 255 || cv <= last_c) {
        return false;                       // a replay, or out of range
    }
    char want[OS_MESH_TAG_LEN + 1];
    link_tag(k, k->peer, (unsigned)be, mv, nv, (uint32_t)cv, want);
    uint8_t diff = 0;
    for (int i = 0; i < OS_MESH_TAG_LEN; i++) {
        diff |= (uint8_t)(want[i] ^ t[5 + i]);
    }
    if (diff) {
        return false;
    }
    *c = (uint32_t)cv;
    *v = (link_byte_t){.b = (uint8_t)be ^ link_pad(k, k->peer, (uint32_t)cv), .master = mv, .n = (uint32_t)nv};
    return true;
}

// Mesh task: keep it short (os_mesh.h). Only bytes the paired partner sealed count.
static void link_heard(os_mesh_type_t type, const os_mesh_peer_t *from, const char *json, void *ctx)
{
    QueueHandle_t q = s_link_q;
    link_byte_t v;
    uint32_t c;
    if (!q || strcmp(from->id, s_link_id) || !strstr(json, "\"app\":\"" LINK_APP "\"") ||
        !link_open(&s_link_keys, json, s_link_rx_c, &v, &c)) {
        return;
    }
    s_link_rx_c = c;
    if (xQueueSend(q, &v, 0) != pdTRUE) {   // full: drop the oldest, keep the newest
        link_byte_t old;
        xQueueReceive(q, &old, 0);
        xQueueSend(q, &v, 0);
    }
}

static void link_send(uint8_t b, bool master, uint32_t n)
{
    char msg[128];
    link_seal(&s_link_keys, ++s_link_tx_c, b, master, n, msg, sizeof(msg));
    os_mesh_send(OS_MESH_APP, s_link_id, msg);  // fails while the partner is out of range: the game sees no cable
}

// Walnut-CGB calls this when a transfer starts, and on the clocked side again
// every 4096 cycles while it waits: that side sends once per transfer (plus a
// slow resend), not on every call.
static void link_tx(struct gb_s *gb, const uint8_t b)
{
    if (gb->hram_io[IO_SC] & SERIAL_SC_CLOCK_SRC) {
        link_send(b, true, ++s_link_seq);
        return;
    }
    uint32_t n = s_link_last + 1;
    TickType_t now = xTaskGetTickCount();
    if (b == s_link_sent && n == s_link_sent_n && now - s_link_sent_at < pdMS_TO_TICKS(LINK_RESEND_MS)) {
        return;
    }
    s_link_sent = b;
    s_link_sent_n = n;
    s_link_sent_at = now;
    link_send(b, false, n);
}

static enum gb_serial_rx_ret_e link_rx(struct gb_s *gb, uint8_t *rx)
{
    emu_t *e = gb->direct.priv;
    bool master = gb->hram_io[IO_SC] & SERIAL_SC_CLOCK_SRC;
    TickType_t end = xTaskGetTickCount() + (master ? pdMS_TO_TICKS(LINK_WAIT_MS) : 0);
    link_byte_t v;
    for (;;) {
        // Never wait once the app is closing: leave() is waiting for this task.
        int32_t left = (int32_t)(end - xTaskGetTickCount());
        if (xQueueReceive(e->link_q, &v, master && !s_quit && left > 0 ? left : 0) != pdTRUE) {
            return GB_SERIAL_RX_NO_CONNECTION;
        }
        if (master && !v.master && v.n == s_link_seq) {
            *rx = v.b;
            return GB_SERIAL_RX_SUCCESS;
        }
        if (!master && v.master) {
            s_link_last = v.n;
            s_link_sent = -1;
            *rx = v.b;
            return GB_SERIAL_RX_SUCCESS;
        }
        // An answer to an earlier transfer, a copy, or both sides clocking or
        // both waiting: nobody shifted it for this transfer, so it's dropped.
    }
}

static void link_hold(bool on)
{
    if (on != s_link_held) {
        on ? os_mesh_acquire() : os_mesh_release();
        s_link_held = on;
    }
}

// ---- Link pairing (OS task; the mesh handler only queues) ----
//
// Both badges make a fresh X25519 key pair. The badge whose id sorts first (the
// host) sends a commitment, the SHA-256 of its public key, and reveals the key
// only after the guest has sent its own; the guest reveals its key only for a
// commitment it holds. So neither side can choose its key after seeing the
// other's, and someone in between who swaps keys gets two different codes:
// both screens show a 6-digit code from the two keys and the shared secret,
// and the players compare them (1 in a million that a swap matches). The
// shared secret also gives the link keys. A new commitment (the host picked
// again) starts the guest over with a new key pair; a different key for the
// same commitment (the guest picked again) starts the host over. Each such
// restart would be one more try for someone in between, so after
// PAIR_RESTARTS_MAX of them the players pick each other again; after SELECT,
// a restart drops the link instead.
//
// Messages, "r" the first 8 bytes of the commitment they belong to:
//   host:  {"p":"c","v":<commitment>}  and, once it has the guest's key,
//          {"p":"k","v":<host key>,"r":..}
//   guest: {"p":"k","v":<guest key>,"r":..}
// Both keep sending every PAIR_RESEND_MS, also after SELECT and while the
// game runs, until Retro closes or the pairing is dropped, in case the
// partner missed one.

typedef enum { PAIR_OFF, PAIR_WAIT, PAIR_CONFIRM, PAIR_ACCEPTED } pair_state_t;

typedef struct {
    char p;                                 // 'c' commitment, 'k' key
    uint8_t v[32];
    uint8_t r[8];
    bool has_r;
} pair_msg_t;

// One side of a pairing: the protocol alone, no radio and no screen, so the
// self-test can run two of them against each other.
typedef struct {
    pair_state_t state;
    bool host;
    int restarts;                           // rounds started over after we had revealed a key
    psa_key_id_t key;
    uint8_t pk[32], commit[32];             // ours; commit: the host's (its own, or the one the guest holds)
    bool have_commit;
    uint8_t peer_pk[32];
    bool have_peer;
    char code[7];
    link_keys_t keys;
} pair_t;

// What a message did to a pair_t, worst last.
typedef enum {
    PAIR_EV_NONE,                           // nothing: a copy, or not for this round
    PAIR_EV_NEW,                            // a new state: show it and send at once
    PAIR_EV_RESTARTED,                      // the partner started over after we accepted
    PAIR_EV_TOO_MANY,                       // more than PAIR_RESTARTS_MAX restarts
    PAIR_EV_FAILED,                         // no key pair, or a key without a secret
} pair_ev_t;

// Each restart after a reveal is one more try at a matching code for someone
// in between; past this many, the players have to pick each other again.
#define PAIR_RESTARTS_MAX   2

static struct {
    pair_t p;
    bool demo;                              // the self-test's card: nothing to accept
    char id[OS_MESH_ID_LEN + 1], name[25];
    int level;
    TickType_t sent_at;
    QueueHandle_t q;
    int sub;
} s_pair = {.sub = -1};

static struct {
    lv_obj_t *box, *initial, *name, *sub, *status, *digit[6], *btn_a;
} s_card;

static void card_update(void);
static void card_show(void);
static void pair_start(const os_mesh_peer_t *p);

static void sha256_of(const uint8_t *in, size_t n, uint8_t out[32])
{
    size_t len = 0;
    psa_hash_compute(PSA_ALG_SHA_256, in, n, out, 32, &len);
}

// SHA-256 of label and three 32-byte parts.
static void sha256_parts(const char *label, const uint8_t *a, const uint8_t *b, const uint8_t *c, uint8_t out[32])
{
    psa_hash_operation_t h = PSA_HASH_OPERATION_INIT;
    size_t len = 0;
    psa_hash_setup(&h, PSA_ALG_SHA_256);
    psa_hash_update(&h, (const uint8_t *)label, strlen(label));
    psa_hash_update(&h, a, 32);
    psa_hash_update(&h, b, 32);
    psa_hash_update(&h, c, 32);
    psa_hash_finish(&h, out, 32, &len);
    psa_hash_abort(&h);
}

// A fresh X25519 key pair: *key the private key (PSA keeps it), pk the public key.
static bool pair_keygen(psa_key_id_t *key, uint8_t pk[32])
{
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_MONTGOMERY));
    psa_set_key_bits(&a, 255);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_DERIVE);
    psa_set_key_algorithm(&a, PSA_ALG_ECDH);
    size_t n = 0;
    *key = PSA_KEY_ID_NULL;
    if (psa_crypto_init() != PSA_SUCCESS || psa_generate_key(&a, key) != PSA_SUCCESS) {
        return false;
    }
    if (psa_export_public_key(*key, pk, 32, &n) != PSA_SUCCESS || n != 32) {
        psa_destroy_key(*key);
        *key = PSA_KEY_ID_NULL;
        return false;
    }
    return true;
}

// From our key and the partner's public key: the code both screens show and
// the link keys. False for a key that gives no secret (a low-order point).
static bool pair_derive(psa_key_id_t key, const uint8_t pk[32], const uint8_t peer_pk[32], char code[7],
                        link_keys_t *k)
{
    uint8_t shared[32], h[32];
    size_t n = 0;
    if (psa_raw_key_agreement(PSA_ALG_ECDH, key, peer_pk, 32, shared, sizeof(shared), &n) != PSA_SUCCESS ||
        n != 32) {
        return false;
    }
    uint8_t any = 0;
    for (int i = 0; i < 32; i++) {
        any |= shared[i];
    }
    if (!any) {
        return false;                       // RFC 7748: a low-order key gives all zeros
    }
    bool first = memcmp(pk, peer_pk, 32) < 0;
    const uint8_t *a = first ? pk : peer_pk, *b = first ? peer_pk : pk;
    sha256_parts("retro-link code v1", a, b, shared, h);
    uint32_t v = (uint32_t)h[0] << 24 | (uint32_t)h[1] << 16 | (uint32_t)h[2] << 8 | h[3];
    snprintf(code, 7, "%06lu", (unsigned long)(v % 1000000));
    sha256_parts("retro-link keys v1", a, b, shared, h);
    memcpy(k->ktag, h, 16);
    memcpy(k->kenc, h + 16, 16);
    k->me = first ? 'a' : 'b';
    k->peer = first ? 'b' : 'a';
    memset(shared, 0, sizeof(shared));
    memset(h, 0, sizeof(h));
    return true;
}

// A pairing message, as pair_build writes them.
static bool pair_parse(const char *json, pair_msg_t *m)
{
    const char *p = strstr(json, "\"p\":\""), *v = strstr(json, "\"v\":\""), *r = strstr(json, "\"r\":\"");
    memset(m, 0, sizeof(*m));
    if (!strstr(json, "\"app\":\"" LINK_APP "\"") || !p || !v || (p[5] != 'c' && p[5] != 'k') ||
        !from_hex(v + 5, m->v, 32)) {
        return false;
    }
    m->p = p[5];
    m->has_r = r && from_hex(r + 5, m->r, 8);
    return true;
}

// What this side sends now (sent again every PAIR_RESEND_MS): returns how many.
static int pair_build(const pair_t *p, char out[2][200])
{
    char hex[65], r[17];
    int n = 0;
    to_hex(p->commit, 8, r);
    if (p->host) {
        to_hex(p->commit, 32, hex);
        snprintf(out[n++], 200, "{\"app\":\"" LINK_APP "\",\"p\":\"c\",\"v\":\"%s\"}", hex);
    }
    if (p->host ? p->have_peer : p->have_commit) {
        to_hex(p->pk, 32, hex);
        snprintf(out[n++], 200, "{\"app\":\"" LINK_APP "\",\"p\":\"k\",\"v\":\"%s\",\"r\":\"%s\"}", hex, r);
    }
    return n;
}

// A new key pair for a new round; the host commits to it.
static bool pair_round(pair_t *p)
{
    if (p->key != PSA_KEY_ID_NULL) {
        psa_destroy_key(p->key);
    }
    p->have_peer = false;
    p->state = PAIR_WAIT;
    if (!pair_keygen(&p->key, p->pk)) {
        return false;
    }
    if (p->host) {
        sha256_of(p->pk, 32, p->commit);
        p->have_commit = true;
    }
    return true;
}

static void pair_reset(pair_t *p)
{
    if (p->key != PSA_KEY_ID_NULL) {
        psa_destroy_key(p->key);
    }
    memset(p, 0, sizeof(*p));
}

static pair_ev_t pair_step(pair_t *p, const pair_msg_t *m)
{
    if (!p->host && m->p == 'c') {
        if (p->have_commit && !memcmp(m->v, p->commit, 32)) {
            return PAIR_EV_NONE;            // a copy
        }
        // A new round (the host picked again): a new key pair, revealed only
        // for this commitment. A restart if we had revealed one already.
        if (p->state == PAIR_ACCEPTED) {
            return PAIR_EV_RESTARTED;
        }
        if (p->have_commit && ++p->restarts > PAIR_RESTARTS_MAX) {
            return PAIR_EV_TOO_MANY;
        }
        if (!pair_round(p)) {
            return PAIR_EV_FAILED;
        }
        memcpy(p->commit, m->v, 32);
        p->have_commit = true;
        return PAIR_EV_NEW;
    }
    if (m->p != 'k' || !m->has_r || !p->have_commit || memcmp(m->r, p->commit, 8)) {
        return PAIR_EV_NONE;
    }
    if (p->have_peer) {
        if (!memcmp(m->v, p->peer_pk, 32) || !p->host) {
            return PAIR_EV_NONE;            // a copy; the guest takes one host key per round
        }
        // The host got a different guest key for its round (the guest picked
        // again): a new round, as the host has revealed its key.
        if (p->state == PAIR_ACCEPTED) {
            return PAIR_EV_RESTARTED;
        }
        if (++p->restarts > PAIR_RESTARTS_MAX) {
            return PAIR_EV_TOO_MANY;
        }
        return pair_round(p) ? PAIR_EV_NEW : PAIR_EV_FAILED;
    }
    if (!p->host) {
        uint8_t h[32];
        sha256_of(m->v, 32, h);
        if (memcmp(h, p->commit, 32)) {
            return PAIR_EV_NONE;            // not the key the host committed to
        }
    }
    memcpy(p->peer_pk, m->v, 32);
    p->have_peer = true;
    if (!pair_derive(p->key, p->pk, p->peer_pk, p->code, &p->keys)) {
        return PAIR_EV_FAILED;
    }
    p->state = PAIR_CONFIRM;
    return PAIR_EV_NEW;
}

// Mesh task: queue the partner's pairing messages for the OS task.
static void pair_heard(os_mesh_type_t type, const os_mesh_peer_t *from, const char *json, void *ctx)
{
    QueueHandle_t q = s_pair.q;
    pair_msg_t m;
    if (q && !strcmp(from->id, s_pair.id) && pair_parse(json, &m)) {
        xQueueSend(q, &m, 0);               // full: dropped, the partner sends again
    }
}

static void pair_send(void)
{
    char msg[2][200];
    int n = pair_build(&s_pair.p, msg);
    for (int i = 0; i < n; i++) {
        os_mesh_send(OS_MESH_APP, s_pair.id, msg[i]);
    }
    s_pair.sent_at = xTaskGetTickCount();
}

static void card_close(void)
{
    if (s_card.box) {
        lv_obj_delete(s_card.box);
        if (s_list) {
            ui_list_show(s_list, true);     // card_show hid it
        }
    }
    memset(&s_card, 0, sizeof(s_card));
}

// Ends the pairing (not an accepted link) and wipes its secrets.
static void pair_stop(void)
{
    if (s_pair.sub >= 0) {
        os_mesh_unsubscribe(s_pair.sub);    // returns once pair_heard can't run any more
    }
    if (s_pair.q) {
        vQueueDelete(s_pair.q);
    }
    pair_reset(&s_pair.p);
    memset(&s_pair, 0, sizeof(s_pair));
    s_pair.sub = -1;
    card_close();
}

static void show_link(void);

static void pair_fail(const char *why)
{
    bool on_card = s_view == VIEW_PAIR;
    pair_stop();
    os_toast("%s", why);
    if (on_card) {
        show_link();
    }
}

static void pair_tick(void)
{
    pair_msg_t m;
    while (s_pair.q && xQueueReceive(s_pair.q, &m, 0) == pdTRUE) {
        pair_ev_t ev = pair_step(&s_pair.p, &m);
        if (ev == PAIR_EV_NEW) {
            card_update();
            pair_send();
        } else if (ev == PAIR_EV_RESTARTED) {
            if (s_view == VIEW_PLAYING) {
                pair_stop();                // the game holds the link: it stays as it is
                return;
            }
            // Our keys no longer match the partner's: drop the link.
            s_link_id[0] = s_link_name[0] = 0;
            memset(&s_link_keys, 0, sizeof(s_link_keys));
            pair_fail("The other badge paired again: pick it again in Link cable");
            if (s_view == VIEW_SYSTEMS) {
                ui_list_refresh(s_list);
            }
            return;
        } else if (ev == PAIR_EV_TOO_MANY) {
            pair_fail("The other badge started over too often: pick it again");
            return;
        } else if (ev == PAIR_EV_FAILED) {
            pair_fail("Pairing failed: pick the badge again");
            return;
        }
    }
    if (s_pair.p.state != PAIR_OFF && !s_pair.demo &&
        xTaskGetTickCount() - s_pair.sent_at >= pdMS_TO_TICKS(PAIR_RESEND_MS)) {
        pair_send();
    }
}

// A whole frame is in fb[back]: hand it to the display task, unless it's
// still sending the last one (then this frame is dropped and drawn over).
static void hand_off(emu_t *e)
{
    if (e->disp_busy) {
        return;
    }
    e->show = e->back;
    e->back ^= 1;
    e->disp_busy = true;
    e->drawn = true;
    xTaskNotifyGive(e->disp);
}

// Display task: copies the shown frame to the panel 12 rows at a time. Each
// blit waits only for the chunk before it, and the emulator runs meanwhile.
static void disp_task(void *arg)
{
    emu_t *e = arg;
    int cur = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_quit) {
            break;
        }
        const uint16_t *src = e->fb[e->show];
        for (int y0 = 0; y0 < e->h; y0 += CHUNK_ROWS) {
            int rows = e->h - y0 < CHUNK_ROWS ? e->h - y0 : CHUNK_ROWS;
            memcpy(e->chunk[cur], src + y0 * e->w, rows * e->w * sizeof(uint16_t));
            bsp_display_blit(e->x, e->y + y0, e->x + e->w, e->y + y0 + rows, e->chunk[cur]);
            cur ^= 1;
        }
        e->disp_busy = false;
    }
    bsp_display_blit_wait();
    xSemaphoreGive(e->disp_done);
    vTaskDelete(NULL);
}

// Line y of 144 into rows of the 240x216 frame: even lines fill two rows, odd
// lines one (0 -> 0,1; 1 -> 2; 2 -> 3,4 ...); 2 pixels -> 3 columns.
// Big-endian, as the panel takes it. Colour games index the core's RGB565
// palette; others get the 4 shades.
static void draw_line(struct gb_s *gb, const uint8_t *px, const uint_fast8_t y)
{
    emu_t *e = gb->direct.priv;
    const uint16_t *cgb = gb->cgb.cgbMode ? gb->cgb.fixPalette : NULL;
    int row = (y & 1) ? (3 * y + 1) / 2 : 3 * y / 2;
    uint16_t *start = e->fb[e->back] + row * GB_W, *dst = start;
    for (int x = 0; x < LCD_WIDTH; x += 2) {
        uint16_t a = cgb ? __builtin_bswap16(cgb[px[x] & 0x3F]) : e->pal[px[x] & 3];
        uint16_t b = cgb ? __builtin_bswap16(cgb[px[x + 1] & 0x3F]) : e->pal[px[x + 1] & 3];
        *dst++ = a;
        *dst++ = a;
        *dst++ = b;
    }
    if (!(y & 1)) {
        memcpy(start + GB_W, start, GB_W * sizeof(uint16_t));
    }
    if (y == LCD_HEIGHT - 1) {
        hand_off(e);
    }
}

// The NES picture, rows NES_CROP.. of 240, into the frame.
static void nes_draw(emu_t *e)
{
    uint16_t *dst = e->fb[e->back];
    for (int y = 0; y < NES_H; y++) {
        for (int x = 0; x < NES_W; x++) {
            agnes_color_t c = agnes_get_screen_pixel(e->nes, x, y + NES_CROP);
            *dst++ = __builtin_bswap16(rgb565(c.r, c.g, c.b));
        }
    }
    hand_off(e);
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
    if (e->link_q) {
        gb_init_serial(&e->gb, link_tx, link_rx);
    }
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
    int64_t next = esp_timer_get_time(), last_save = next, last_write = next, stat_t = next;
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
            shown++;
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
        if (e->ram_touched) {
            e->ram_touched = false;
            last_write = now;
        }
        // An in-game save reaches the card about a second after the game says
        // "saved", so switching the badge off then keeps it.
        if (!nes && e->ram_dirty && (now - last_write >= SAVE_QUIET_US || now - last_save >= SAVE_EVERY_US)) {
            save(e);
            last_save = now;
        }
    }
    save(e);
out:
    xSemaphoreGive(e->done);
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
    if (s_view == VIEW_SYSTEMS && i == ROW_LINK) {
        snprintf(r->title, sizeof(r->title), "Link cable");
        snprintf(r->sub, sizeof(r->sub), "%s%s", s_link_id[0] ? "With " : "Off: play with a nearby badge",
                 s_link_id[0] ? (s_link_name[0] ? s_link_name : s_link_id) : "");
        snprintf(r->right, sizeof(r->right), "Game Boy");
        r->right_color = COL_DIM;
    } else if (s_view == VIEW_LINK) {
        if (i == 0) {
            snprintf(r->title, sizeof(r->title), "No cable");
            snprintf(r->sub, sizeof(r->sub), "Play alone");
        } else {
            const os_mesh_peer_t *p = &s_peers[i - 1];
            snprintf(r->title, sizeof(r->title), "%s", p->name[0] ? p->name : p->id);
            snprintf(r->sub, sizeof(r->sub), "Badge %s  " LV_SYMBOL_BULLET "  %d dBm", p->id, p->rssi);
        }
        if ((i == 0 && !s_link_id[0]) || (i > 0 && !strcmp(s_peers[i - 1].id, s_link_id))) {
            snprintf(r->right, sizeof(r->right), LV_SYMBOL_OK);
        }
    } else if (s_view == VIEW_SYSTEMS) {
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
    ui_list_set_count(s_list, SYS_COUNT + 1);
    ui_list_select(s_list, s_sys);
    hint(LV_SYMBOL_UP LV_SYMBOL_DOWN " console   SELECT open   CANCEL exit");
}

// Nearby badges heard over the radio (bridged ones are too slow for a cable).
static void refresh_peers(void)
{
    static os_mesh_peer_t all[MESH_PEERS_MAX];  // ~2.7 KB: too big for the OS task's stack
    int n = os_mesh_peers(all, MESH_PEERS_MAX);
    s_peer_count = 0;
    for (int i = 0; i < n && s_peer_count < LINK_PEERS_MAX; i++) {
        if (all[i].kind == OS_PEER_BADGE && !all[i].bridged) {
            s_peers[s_peer_count++] = all[i];
        }
    }
}

static void show_link(void)
{
    link_hold(true);                        // radio on the mesh channel, so badges are heard
    refresh_peers();
    s_view = VIEW_LINK;
    ui_list_set_count(s_list, s_peer_count + 1);
    ui_list_select(s_list, 0);
    hint(LV_SYMBOL_UP LV_SYMBOL_DOWN " badge   SELECT connect   CANCEL back");
}

// No os_mesh_pin(): the pin is one shared flag per peer, so unpinning here
// would also unpin a trade or X Dex partner. Both badges send HELLOs every
// ~2 s while on the mesh channel, which keeps the partner in the table.
static void pick_link(int i)
{
    if (i > 0 && i <= s_peer_count) {
        pair_start(&s_peers[i - 1]);        // the link starts once both accept the code
        return;
    }
    pair_stop();
    s_link_id[0] = s_link_name[0] = 0;
    memset(&s_link_keys, 0, sizeof(s_link_keys));
    link_hold(false);
    show_systems();
    ui_list_select(s_list, ROW_LINK);
}

// SELECT on the card: this badge takes the partner's bytes from now on. The
// pairing keeps answering until the game starts, in case the partner missed
// our key.
static void pair_accept(void)
{
    if (s_pair.demo) {
        pair_stop();
        show_systems();
        return;
    }
    if (s_pair.p.state != PAIR_CONFIRM) {
        return;
    }
    snprintf(s_link_id, sizeof(s_link_id), "%s", s_pair.id);
    snprintf(s_link_name, sizeof(s_link_name), "%s", s_pair.name);
    s_link_keys = s_pair.p.keys;
    s_link_tx_c = s_link_rx_c = 0;
    s_pair.p.state = PAIR_ACCEPTED;
    card_close();
    os_toast("Linked with %s: start the game, then its link menu", s_link_name[0] ? s_link_name : s_link_id);
    show_systems();
    ui_list_select(s_list, ROW_LINK);
}

// ---- Self-test: `retro selftest` on the console ----
//
// Runs the crypto and two pair_t sides against each other on this badge (also
// with lost messages, a partner starting over and someone trying codes),
// checks the link messages, and, if Retro is open on its menus with no pairing
// going, shows the card with the code it got.

extern const os_app_t app_gbemu;
static char s_demo_code[7];

static void demo_card(void *arg)
{
    if (os_current() != &app_gbemu || !s_list || (s_view != VIEW_SYSTEMS && s_view != VIEW_LINK) ||
        s_pair.p.state != PAIR_OFF) {
        return;
    }
    s_pair.demo = true;
    s_pair.p.state = PAIR_CONFIRM;
    snprintf(s_pair.id, sizeof(s_pair.id), "SELF01");
    snprintf(s_pair.name, sizeof(s_pair.name), "@selftest");
    s_pair.level = 1;
    memcpy(s_pair.p.code, s_demo_code, sizeof(s_pair.p.code));
    s_view = VIEW_PAIR;
    card_show();
}

// Delivers what `from` sends now to `to`, skipping every drop-th message of
// *count (0: none). Returns the worst event.
static pair_ev_t st_deliver(const pair_t *from, pair_t *to, int *count, int drop)
{
    char msg[2][200];
    pair_ev_t worst = PAIR_EV_NONE;
    int n = pair_build(from, msg);
    for (int i = 0; i < n; i++) {
        pair_msg_t m;
        if ((drop && ++*count % drop == 0) || !pair_parse(msg[i], &m)) {
            continue;
        }
        pair_ev_t ev = pair_step(to, &m);
        worst = ev > worst ? ev : worst;
        vTaskDelay(1);                      // key work takes ~0.1 s: let the idle task (watchdog) run
    }
    return worst;
}

// Resend rounds until both show a code; false on a failure or after `rounds`.
static bool st_run(pair_t *h, pair_t *g, int drop, int rounds)
{
    int count = 0;
    for (int i = 0; i < rounds; i++) {
        pair_ev_t a = st_deliver(h, g, &count, drop), b = st_deliver(g, h, &count, drop);
        if (a >= PAIR_EV_RESTARTED || b >= PAIR_EV_RESTARTED) {
            return false;
        }
        if (h->state == PAIR_CONFIRM && g->state == PAIR_CONFIRM) {
            return true;
        }
    }
    return false;
}

static bool st_begin(pair_t *p, bool host)
{
    vTaskDelay(1);
    pair_reset(p);
    p->host = host;
    return pair_round(p);
}

static bool st_same(const pair_t *h, const pair_t *g)
{
    return !strcmp(h->code, g->code) && !memcmp(h->keys.ktag, g->keys.ktag, 16) &&
           !memcmp(h->keys.kenc, g->keys.kenc, 16) && h->keys.me == g->keys.peer && h->keys.peer == g->keys.me;
}

static int cmd_retro(int argc, char **argv)
{
    if (argc != 2 || strcmp(argv[1], "selftest")) {
        printf("usage: retro selftest\n");
        return 1;
    }
    int ok = 0, bad = 0;
#define CHECK(cond, what)                       \
    do {                                        \
        if (cond) {                             \
            ok++;                               \
        } else {                                \
            bad++;                              \
            printf("FAIL: %s\n", what);         \
        }                                       \
    } while (0)
    // Crypto.
    psa_key_id_t ka = PSA_KEY_ID_NULL, kb = PSA_KEY_ID_NULL, km = PSA_KEY_ID_NULL;
    uint8_t pa[32], pb[32], pm[32], commit[32], h[32], zero[32] = {0};
    char code_a[7] = "", code_b[7] = "", code_m[7] = "", code_z[7];
    link_keys_t la = {0}, lb = {0}, lm = {0}, lz;
    bool gen = pair_keygen(&ka, pa) && pair_keygen(&kb, pb) && pair_keygen(&km, pm);
    vTaskDelay(1);
    CHECK(gen, "three X25519 key pairs");
    if (gen) {
        sha256_of(pa, 32, commit);
        sha256_of(pb, 32, h);
        CHECK(memcmp(h, commit, 32), "a commitment refuses another key");
        bool da = pair_derive(ka, pa, pb, code_a, &la), db = pair_derive(kb, pb, pa, code_b, &lb);
        CHECK(da && db && strlen(code_a) == 6 && !strcmp(code_a, code_b), "the same 6-digit code on both sides");
        CHECK(da && db && !memcmp(la.ktag, lb.ktag, 16) && !memcmp(la.kenc, lb.kenc, 16) && la.me == lb.peer &&
                  la.peer == lb.me && la.me != la.peer,
              "the same link keys, opposite sides");
        CHECK(pair_derive(ka, pa, pm, code_m, &lm) && strcmp(code_m, code_a), "a swapped key gives another code");
        CHECK(!pair_derive(ka, pa, zero, code_z, &lz), "a low-order key is refused");

        // Link messages.
        char msg[128];
        link_byte_t v = {0};
        uint32_t c = 0;
        unsigned first = 256;
        int plain = 0, same = 0;
        for (uint32_t i = 1; i <= 16; i++) {
            link_seal(&la, i, 0x5A, true, 7, msg, sizeof(msg));
            unsigned be = (unsigned)strtoul(strstr(msg, "\"b\":") + 4, NULL, 10);
            plain += be == 0x5A;
            same += be == first;
            first = i == 1 ? be : first;
        }
        CHECK(plain < 16 && same < 15, "bytes go out encrypted, each with its own pad");
        link_seal(&la, 1, 0x5A, true, 7, msg, sizeof(msg));
        CHECK(link_open(&lb, msg, 0, &v, &c) && v.b == 0x5A && v.master && v.n == 7 && c == 1, "the partner opens it");
        CHECK(!link_open(&lb, msg, 1, &v, &c), "a replay is refused");
        CHECK(!link_open(&la, msg, 0, &v, &c), "an echo to the sender is refused");
        CHECK(!link_open(&lm, msg, 0, &v, &c), "other keys are refused");
        char *t = strstr(msg, "\"t\":\"") + 5;
        t[0] = t[0] == '0' ? '1' : '0';
        CHECK(!link_open(&lb, msg, 0, &v, &c), "a changed tag is refused");
        link_seal(&la, 2, 0x5A, true, 7, msg, sizeof(msg));
        *(strstr(msg, "\"n\":") + 4) = '8';
        CHECK(!link_open(&lb, msg, 1, &v, &c), "a changed transfer number is refused");
        link_seal(&la, 3, 0x5A, true, 7, msg, sizeof(msg));
        char *b = strstr(msg, ",\"m\"") - 1;  // the byte's last digit: stays 0..255 either way
        *b = *b == '0' ? '1' : *b - 1;
        CHECK(!link_open(&lb, msg, 2, &v, &c), "a changed byte is refused");
    }
    psa_destroy_key(ka);
    psa_destroy_key(kb);
    psa_destroy_key(km);

    // The protocol: host H, guest G, someone in between M.
    static pair_t H, G, M;                  // ~400 B: off the console task's stack
    int count = 0;
    bool run = st_begin(&H, true) && st_begin(&G, false) && st_run(&H, &G, 0, 4);
    CHECK(run && st_same(&H, &G), "two sides pair, same code and keys");
    // Every third: with one message each way per round, every second would
    // always hit the guest's and starve it.
    run = st_begin(&H, true) && st_begin(&G, false) && st_run(&H, &G, 3, 20);
    CHECK(run && st_same(&H, &G), "they pair with every third message lost");
    run = run && st_begin(&G, false) && st_run(&H, &G, 0, 6);
    CHECK(run && st_same(&H, &G) && H.restarts == 1, "the guest picks again: the host starts over once");
    G.state = PAIR_ACCEPTED;
    CHECK(run && pair_round(&H) && st_deliver(&H, &G, &count, 0) == PAIR_EV_RESTARTED,
          "after SELECT, a host starting over drops the link");
    // M as host: the guest takes only the key H committed to.
    {
        pair_msg_t m = {.p = 'k', .has_r = true};
        bool fresh = st_begin(&H, true) && st_begin(&G, false) && st_begin(&M, true);
        st_deliver(&H, &G, &count, 0);      // G holds H's commitment
        memcpy(m.v, M.pk, 32);
        memcpy(m.r, H.commit, 8);
        CHECK(fresh && pair_step(&G, &m) == PAIR_EV_NONE && G.state == PAIR_WAIT,
              "the guest refuses a key the host didn't commit to");
    }
    // M as host, sending the guest new commitments to try codes.
    pair_ev_t ev = PAIR_EV_NONE;
    bool grind = st_begin(&G, false) && st_begin(&M, true);
    for (int i = 0; grind && i <= PAIR_RESTARTS_MAX + 1 && ev < PAIR_EV_RESTARTED; i++) {
        ev = (i == 0 || pair_round(&M)) ? st_deliver(&M, &G, &count, 0) : PAIR_EV_FAILED;
    }
    CHECK(grind && ev == PAIR_EV_TOO_MANY && G.restarts == PAIR_RESTARTS_MAX + 1,
          "new commitments: the guest gives up after the limit");
    // M as guest, sending the host new keys for its rounds.
    ev = PAIR_EV_NONE;
    grind = st_begin(&H, true) && st_begin(&M, false);
    M.restarts = -100;                      // M's own limit doesn't count
    for (int i = 0; grind && i < 4 * (PAIR_RESTARTS_MAX + 1) && ev < PAIR_EV_RESTARTED; i++) {
        st_deliver(&H, &M, &count, 0);      // M takes the host's current commitment
        if (i > 0 && H.state == PAIR_CONFIRM && !pair_round(&M)) {
            grind = false;                  // M: another key for the same round
        }
        ev = st_deliver(&M, &H, &count, 0);
    }
    CHECK(grind && ev == PAIR_EV_TOO_MANY, "new guest keys: the host gives up after the limit");
    pair_reset(&H);
    pair_reset(&G);
    pair_reset(&M);
#undef CHECK
    printf("retro selftest: %d passed, %d failed\n", ok, bad);
    if (!bad) {
        memcpy(s_demo_code, code_a, sizeof(s_demo_code));
        os_post(demo_card, NULL);
    }
    return bad ? 1 : 0;
}

// The badge list follows who's nearby. It's sorted by signal strength, so the
// selection follows the badge, not the row.
static void tick(void)
{
    if (s_pair.p.state != PAIR_OFF) {
        pair_tick();
    }
    if (!s_list || s_view != VIEW_LINK || ++s_tick % 10) {
        return;
    }
    int sel = ui_list_selected(s_list);
    char id[OS_MESH_ID_LEN + 1] = "";
    if (sel > 0 && sel <= s_peer_count) {
        snprintf(id, sizeof(id), "%s", s_peers[sel - 1].id);
    }
    refresh_peers();
    ui_list_set_count(s_list, s_peer_count + 1);
    int now = id[0] ? (sel <= s_peer_count ? sel : s_peer_count) : 0;   // gone: stay near, not on "No cable"
    for (int i = 0; id[0] && i < s_peer_count; i++) {
        if (!strcmp(s_peers[i].id, id)) {
            now = i + 1;
        }
    }
    ui_list_select(s_list, now);
}

// The pairing card, laid out like a Game Boy Color in Solana colours: the
// colour stripe, a screen with the partner and the code, A and B buttons.
#define CARD_W      (OS_ROOT_W - 24)
#define CARD_H      (OS_ROOT_H - UI_HINT_H - 4)
#define SCREEN_W    (CARD_W - 28)

static lv_obj_t *card_box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t bg, uint32_t border, int bw,
                          int radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, ui_col(bg), 0);
    lv_obj_set_style_border_width(o, bw, 0);
    lv_obj_set_style_border_color(o, ui_col(border), 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_scrollable(o, false);
    return o;
}

static lv_obj_t *card_button(lv_obj_t *parent, int x, int y, uint32_t bg, uint32_t fg, const char *letter,
                             const char *caption)
{
    lv_obj_t *b = card_box(parent, x, y, 32, 32, bg, bg, 0, LV_RADIUS_CIRCLE);
    lv_obj_t *l = ui_label(b, &sol_font_14, fg, letter);
    lv_obj_center(l);
    lv_obj_t *c = ui_label(parent, &sol_mono_12, COL_DIM, caption);
    lv_obj_set_width(c, 48);
    lv_obj_set_style_text_align(c, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(c, x - 8, y + 34);
    return b;
}

static void card_show(void)
{
    card_close();
    ui_list_show(s_list, false);
    lv_obj_t *box = card_box(s_root, 12, 2, CARD_W, CARD_H, COL_PANEL_HI, COL_BORDER, 1, 16);
    s_card.box = box;
    ui_gradient_bar(box, 14, 10, SCREEN_W, 4);
    lv_obj_t *t = ui_label(box, &sol_mono_12, COL_DIM, "RETRO LINK");
    lv_obj_set_pos(t, 14, 18);

    lv_obj_t *scr = card_box(box, 14, 34, SCREEN_W, 96, COL_BG, COL_PURPLE, 2, 10);
    lv_obj_t *av = card_box(scr, 10, 8, 30, 30, COL_PURPLE, COL_PURPLE, 0, LV_RADIUS_CIRCLE);
    s_card.initial = ui_label(av, &sol_font_14, COL_TEXT, "");
    lv_obj_center(s_card.initial);
    s_card.name = ui_label(scr, &sol_font_20, COL_TEXT, "");
    lv_obj_set_width(s_card.name, SCREEN_W - 60);
    lv_label_set_long_mode(s_card.name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(s_card.name, 48, 4);
    s_card.sub = ui_label(scr, &sol_mono_12, COL_DIM, "");
    lv_obj_set_pos(s_card.sub, 48, 27);
    const int dw = 34, gap = 6, mid = 8, x0 = (SCREEN_W - 4 - (6 * dw + 5 * gap + mid)) / 2;
    for (int i = 0; i < 6; i++) {
        lv_obj_t *d = card_box(scr, x0 + i * (dw + gap) + (i >= 3 ? mid : 0), 48, dw, 38, COL_PANEL, COL_GREEN, 1, 6);
        s_card.digit[i] = ui_label(d, &sol_font_20, COL_GREEN, "");
        lv_obj_center(s_card.digit[i]);
    }

    s_card.status = ui_label(box, &sol_font_14, COL_TEXT, "");
    lv_obj_set_width(s_card.status, CARD_W - 130);
    lv_label_set_long_mode(s_card.status, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_pos(s_card.status, 16, 140);
    card_button(box, CARD_W - 96, 150, COL_PURPLE, COL_TEXT, "B", "NO");
    s_card.btn_a = card_button(box, CARD_W - 52, 138, COL_GREEN, COL_BG, "A", "YES");
    hint("SELECT = A  yes     CANCEL = B  no");
    card_update();
}

static void card_update(void)
{
    if (!s_card.box) {
        return;
    }
    const char *name = s_pair.name[0] ? s_pair.name : s_pair.id, *ch = name;
    while (*ch && !((*ch >= '0' && *ch <= '9') || ((*ch | 32) >= 'a' && (*ch | 32) <= 'z'))) {
        ch++;                               // "@toly" -> T
    }
    char initial[2] = {*ch ? (char)(*ch >= 'a' && *ch <= 'z' ? *ch - 32 : *ch) : '?', 0};
    lv_label_set_text(s_card.initial, initial);
    lv_label_set_text(s_card.name, name);
    lv_label_set_text_fmt(s_card.sub, "Lv %d  " LV_SYMBOL_BULLET "  badge %s", s_pair.level, s_pair.id);
    bool shown = s_pair.p.state == PAIR_CONFIRM;
    for (int i = 0; i < 6; i++) {
        char d[2] = {shown ? s_pair.p.code[i] : '-', 0};
        lv_label_set_text(s_card.digit[i], d);
        lv_obj_set_style_text_color(s_card.digit[i], ui_col(shown ? COL_GREEN : COL_DIM), 0);
    }
    lv_label_set_text(s_card.status, s_pair.demo ? "Self-test: this is how the code looks."
                                     : shown ? "Same code on both badges? Then press A."
                                             : "Waiting. They pick you in Link cable.");
    lv_obj_set_style_bg_opa(s_card.btn_a, shown ? LV_OPA_COVER : LV_OPA_30, 0);
}

static void pair_start(const os_mesh_peer_t *p)
{
    pair_stop();
    os_cloud_info_t me;
    os_cloud_get_info(&me);
    if (strlen(me.id) != OS_MESH_ID_LEN || !strcmp(me.id, p->id)) {
        os_toast("This badge has no id yet: connect to Wi-Fi once");
        return;
    }
    snprintf(s_pair.id, sizeof(s_pair.id), "%s", p->id);
    snprintf(s_pair.name, sizeof(s_pair.name), "%s", p->name);
    s_pair.level = p->level;
    s_pair.p.host = strcmp(me.id, p->id) < 0;
    s_pair.q = xQueueCreate(PAIR_QUEUE, sizeof(pair_msg_t));
    s_pair.sub = s_pair.q ? os_mesh_subscribe(OS_MESH_APP, pair_heard, NULL) : -1;
    if (s_pair.sub < 0 || !pair_round(&s_pair.p)) {
        pair_stop();
        os_toast("Can't start pairing");
        return;
    }
    link_hold(true);
    s_view = VIEW_PAIR;
    card_show();
    pair_send();
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
    size_t sram_free = heap_caps_get_free_size(OS_MEM_SRAM_MALLOC);
    if (!nes && sram_free > sizeof(*e) + SRAM_KEEP) {
        e = heap_caps_calloc(1, sizeof(*e), OS_MEM_SRAM);
    }
    ESP_LOGI(TAG, "core state %u B in %s (SRAM free %u B)", (unsigned)sizeof(*e), e ? "SRAM" : "PSRAM",
             (unsigned)sram_free);
    if (!e) {
        e = heap_caps_calloc(1, sizeof(*e), os_mem_bulk_caps());
    }
    for (int i = 0; e && i < 2; i++) {
        e->chunk[i] = heap_caps_malloc(CHUNK_PX * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        e->fb[i] = heap_caps_calloc(FRAME_PX, sizeof(uint16_t), os_mem_bulk_caps());
        if (!e->chunk[i] || !e->fb[i]) {
            heap_caps_free(e->chunk[0]);
            heap_caps_free(e->chunk[1]);
            heap_caps_free(e->fb[0]);
            heap_caps_free(e->fb[1]);
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
        e->pal[i] = __builtin_bswap16(lv_color_to_u16(lv_color_hex(DMG_HEX[i])));
    }
    ui_list_show(s_list, false);
    hint(NULL);
    s_view = VIEW_PLAYING;
    lv_obj_set_style_bg_color(s_root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    // The game draws itself over this black root (bsp_display_blit), past LVGL.
    e->x = nes ? NES_X : GB_X;
    e->y = nes ? NES_Y : GB_Y;
    e->w = nes ? NES_W : GB_W;
    e->h = nes ? NES_H : GB_H;

    if (!nes && s_link_id[0]) {
        s_link_seq = s_link_last = 0;
        s_link_sent = -1;
        s_link_q = xQueueCreate(LINK_QUEUE, sizeof(link_byte_t));
        s_link_sub = s_link_q ? os_mesh_subscribe(OS_MESH_APP, link_heard, NULL) : -1;
    }

    s_emu = e;
    s_quit = false;
    e->disp_done = xSemaphoreCreateBinary();
    if (!e->disp_done || xTaskCreate(disp_task, "retro_disp", 3072, e, DISP_PRIO, &e->disp) != pdPASS) {
        os_toast("Can't start the display task");    // leave() frees what start() made
        e->disp = NULL;
        return;
    }
    s_done = xSemaphoreCreateBinary();
    e->done = s_done;
    e->link_q = s_link_sub >= 0 ? s_link_q : NULL;
    if (xTaskCreate(emu_task, "retro", 6144, NULL, tskIDLE_PRIORITY, &s_task) != pdPASS) {
        os_toast("Can't start the emulator task");
        s_task = NULL;
        return;
    }
    if (e->link_q) {
        os_toast("Link cable to %s  " LV_SYMBOL_BULLET "  BOOT exit", s_link_name[0] ? s_link_name : s_link_id);
    } else {
        os_toast("BOOT exit  " LV_SYMBOL_BULLET "  UP+DOWN start  " LV_SYMBOL_BULLET "  LEFT+RIGHT select");
    }
}

static void enter(lv_obj_t *root)
{
    static bool registered;
    if (!registered) {
        // The console copied os_cmd's table at boot: register with it directly.
        const esp_console_cmd_t c = {
            .command = "retro", .help = "retro selftest - check the link cable's pairing", .func = cmd_retro,
        };
        registered = esp_console_cmd_register(&c) == ESP_OK;
    }
    s_root = root;
    s_list = NULL;
    s_view = VIEW_SYSTEMS;                  // not the view the last session ended in
    s_msg = s_hint = NULL;
    s_task = NULL;

    bsp_sd_info_t in;
    bsp_sd_get_info(&in);
    if (in.state != BSP_SD_MOUNTED) {
        message("No microSD card.\nInsert one with games in /roms/gb, /roms/gbc or /roms/nes.");
        return;
    }
    s_list = ui_list_create(root, 0, OS_ROOT_H - UI_HINT_H, row, NULL);
    show_systems();
    if (s_link_id[0]) {
        link_hold(true);                    // keep hearing the partner chosen earlier
    }
}

static void leave(void)
{
    pair_stop();                            // the card goes with the root, after this
    if (s_task) {
        s_quit = true;
        // The task saves and exits; it never waits on the LVGL lock we hold here
        // (blits only wait for the panel, never for LVGL).
        if (xSemaphoreTake(s_done, pdMS_TO_TICKS(3000)) != pdTRUE) {
            // Still running: leak its state and the handles it holds (its own
            // copies in emu_t), rather than free what it may still touch.
            ESP_LOGW(TAG, "emulator task did not stop; leaking it");
            s_emu = NULL;
            s_done = NULL;
            if (s_link_sub >= 0) {
                os_mesh_unsubscribe(s_link_sub);
                s_link_sub = -1;
            }
            s_link_q = NULL;
        }
        s_task = NULL;
    }
    if (s_done) {
        vSemaphoreDelete(s_done);
        s_done = NULL;
    }
    if (s_link_sub >= 0) {
        os_mesh_unsubscribe(s_link_sub);    // returns once the handler can't run any more
        s_link_sub = -1;
    }
    if (s_link_q) {
        vQueueDelete(s_link_q);
        s_link_q = NULL;
    }
    link_hold(false);
    if (s_emu) {
        if (s_emu->nes) {
            agnes_destroy(s_emu->nes);
        }
        heap_caps_free(s_emu->rom);
        heap_caps_free(s_emu->ram);
        if (s_emu->disp) {
            xTaskNotifyGive(s_emu->disp);   // s_quit is set: it finishes its frame and stops
            if (xSemaphoreTake(s_emu->disp_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
                ESP_LOGW(TAG, "display task did not stop; leaking the frame state");
                s_emu = NULL;
            }
        }
    }
    if (s_emu) {
        if (s_emu->disp_done) {
            vSemaphoreDelete(s_emu->disp_done);
        }
        heap_caps_free(s_emu->chunk[0]);
        heap_caps_free(s_emu->chunk[1]);
        heap_caps_free(s_emu->fb[0]);
        heap_caps_free(s_emu->fb[1]);
        heap_caps_free(s_emu);
        s_emu = NULL;
    }
    s_hint = NULL;
}

static bool key(os_key_t k, bool repeat)
{
    if (s_view == VIEW_PLAYING || !s_list) {
        return s_view == VIEW_PLAYING;      // playing: the game reads the buttons, only HOME leaves
    }
    if (s_view == VIEW_PAIR) {
        if (!repeat && k == OS_KEY_SELECT) {
            pair_accept();
        } else if (!repeat && k == OS_KEY_BACK) {
            bool demo = s_pair.demo;
            pair_stop();
            demo ? show_systems() : show_link();
        }
        return true;
    }
    if (k == OS_KEY_SELECT && !repeat) {
        int i = ui_list_selected(s_list);
        if (i == ROW_LINK && s_view == VIEW_SYSTEMS) {
            show_link();
        } else if (i >= 0 && s_view == VIEW_SYSTEMS) {
            show_games(i);
        } else if (i >= 0 && s_view == VIEW_LINK) {
            pick_link(i);
        } else if (i >= 0) {
            start(s_sys, s_roms[i]);
        }
        return true;
    }
    if (k == OS_KEY_BACK && (s_view == VIEW_GAMES || s_view == VIEW_LINK)) {
        if (!repeat) {
            if (s_view == VIEW_LINK) {
                link_hold(s_link_id[0]);    // the list kept the radio; no cable, no need
            }
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
    .on_tick = tick,
};
