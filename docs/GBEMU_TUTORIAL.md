# Tutorial: Retro, a Game Boy / Game Boy Color / NES app for the Solana OS badge

This walks through how the Retro app on branch `feat/gbemu-app` was built, flashed and
tested on a Breakpoint 2026 badge, from a Windows PC. No game ships with it: you play games
you dumped from cartridges you own, or free homebrew. For the beginner version of the setup,
see `GBEMU_SETUP.txt` in the repo root.

What you end up with:

- A **Retro** app in the badge launcher: pick a console (Game Boy, Game Boy Color, NES), then a
  game from its folder on the microSD card (`roms/gb`, `roms/gbc`, `roms/nes`).
- An `sdrecv` console command plus `tools/sd_put.py`, so files go onto the card over the USB cable.
- A full backup of the badge's flash, and the command to restore it.

## 1. Know the platform

Solana OS is the firmware of the BP26 badge: ESP32-C5 (one 240 MHz RISC-V core, 8 MB PSRAM,
16 MB flash), a 320x240 ILI9341 screen, 6 front buttons plus BOOT, a microSD card, 8 RGB LEDs.
The USB port is a CH340 serial chip, so the PC sees a COM port, not a drive.

There are two kinds of apps:

| | Built-in C app | App Store app |
|---|---|---|
| Language | C, compiled into the firmware | Lua 5.5, sandboxed |
| Where | `main/apps/<name>/`, listed in `main/apps/registry.c` | submitted at solana.com/breakpoint/badge/apps/submit |
| Speed | native | 200,000 VM instructions per callback, a callback every 30 ms (`docs/LUA_API.md`, "Limits") |
| Files | anything on the card | only the app's own bundled files, read-only (`main/lua/lua_fs.c`) |

An emulator has to run a whole CPU at about a million instructions a second, so it has to be
a built-in C app. That also means it can't go through the App Store (section 10).

## 2. Install the tools (once)

You need git, Python 3 and ESP-IDF v6.0.1 (the version the README names).

1. Clone ESP-IDF somewhere with space (it's about 1-2 GB with its tools):

   ```bash
   git clone -b v6.0.1 --recursive --depth 1 --shallow-submodules https://github.com/espressif/esp-idf.git D:\esp\esp-idf
   ```

2. Install the ESP32-C5 toolchain **from PowerShell, not Git Bash**. From Git Bash the
   installer stops with `MSys/Mingw is not supported`.

   ```powershell
   $env:IDF_TOOLS_PATH='D:\esp\.espressif'; D:\esp\esp-idf\install.ps1 esp32c5
   ```

3. In every new PowerShell window where you build, load the environment:

   ```powershell
   $env:IDF_TOOLS_PATH='D:\esp\.espressif'; . D:\esp\esp-idf\export.ps1
   ```

   In cmd it's `set IDF_TOOLS_PATH=D:\esp\.espressif`, then `D:\esp\esp-idf\export.bat`.
   Skip the first line and it looks under `C:\Users\<you>\.espressif` and fails.

## 3. Get the firmware

```bash
git clone https://github.com/solana-foundation/solana-os.git D:\Projects\solana-os
```

```bash
git checkout -b feat/gbemu-app
```

Before writing code, read these. They are how every app works:

- `main/core/os.h`: the `os_app_t` struct. One app runs at a time and the OS hands it a root
  container. Callbacks run on the OS task with the LVGL lock held and must not block.
- `main/apps/about/about_app.c`: the smallest real app.
- `main/core/os_keys.h`, `components/bsp/include/bsp.h`, `main/core/theme.h`: keys, buttons,
  SD card, and the app area (320 x 218 below the status bar).

## 4. Add the emulator cores

Two small MIT-licensed cores do the emulation:

- **Walnut-CGB** (`walnut_cgb.h`, one header) runs Game Boy and Game Boy Color games. It reads
  the cartridge header and switches to colour mode by itself. The first version of the app
  used Peanut-GB, which Walnut-CGB grew out of. Peanut-GB has no colour mode.
- **agnes** (`agnes.c` + `agnes.h`) runs NES games. It supports the NROM, UxROM, MMC1 and MMC3
  cartridge types, which covers most NES games.

```bash
curl -fsSL -o main/apps/gbemu/walnut_cgb.h https://raw.githubusercontent.com/Mr-PauI/Walnut-CGB/a42c186917516cc9c80515ceae1121600a071628/walnut_cgb.h
```

```bash
curl -fsSL -o main/apps/gbemu/nes/agnes.c https://raw.githubusercontent.com/kgabis/agnes/0e4220b084c467e39c04805d955e78c463feadd0/agnes.c
```

```bash
curl -fsSL -o main/apps/gbemu/nes/agnes.h https://raw.githubusercontent.com/kgabis/agnes/0e4220b084c467e39c04805d955e78c463feadd0/agnes.h
```

Configure Walnut-CGB before the include. The badge has no speaker, so sound is off:

```c
#define ENABLE_SOUND                0
#define ENABLE_LCD                  1
#define WALNUT_GB_IS_LITTLE_ENDIAN  1       // RISC-V and wasm
#define WALNUT_FULL_GBC_SUPPORT     1
struct gb_s;                                // the header uses it in prototypes before defining it
#include "walnut_cgb.h"
```

Two build errors came from the cores, and both are worked around without editing the
vendored files:

1. `'struct gb_s' declared inside parameter list`: that's what the `struct gb_s;` line above fixes.
2. agnes trips `-Wtype-limits`, which is an error in this build. `nes/agnes_build.c` turns that
   warning off and includes `agnes.c`, and the build lists `agnes_build.c` instead of `agnes.c`.

## 5. Write the app (`main/apps/gbemu/gbemu_app.c`)

The app has five parts.

**Menus.** `on_enter` checks the card (`bsp_sd_get_info`) and shows the three consoles in a
`ui_list`, each with how many games its folder holds. SELECT opens the console's game list
(`roms/gb` plus the older `roms/` for Game Boy, `roms/gbc`, `roms/nes`). SELECT on a game
starts it, and CANCEL goes back to the consoles.

**Emulator task.** Frames can't run inside OS callbacks (they must not block), so the
emulator gets its own FreeRTOS task:

```c
xTaskCreate(emu_task, "gbemu", 6144, NULL, tskIDLE_PRIORITY, &s_task);
```

- **Idle priority** means the OS, LVGL and the radios always preempt it.
- The task calls `vTaskDelay(0)` every frame. That lets the idle task run and keeps the task
  watchdog quiet. It's also required in the browser emulator, whose tasks are cooperative.
- It paces itself to 59.73 frames per second (60.1 for NES) with `esp_timer_get_time()`. Walnut-CGB's
  `frame_skip` is on, so it emulates all 60 frames but draws every other one.

**Picture.** Walnut-CGB calls `draw_line()` once per screen line with 160 palette indices. The
app scales 160x144 by 1.5 into 240x216, which fits the 320x218 app area: every 2 source pixels
become 3, even lines are written twice and odd lines once. The output goes into one of two
RGB565 buffers in PSRAM. When a frame is complete, the app swaps which buffer the LVGL image
shows, under the LVGL lock:

```c
if (!lvgl_port_lock(LOCK_WAIT_MS)) return false;   // LVGL busy: show it next frame
/* swap buf[0]/buf[1], lv_image_cache_drop(), lv_image_set_src() */
lvgl_port_unlock();
```

LVGL only ever reads the front buffer, so there's no tearing.

Colour games: in colour mode, each pixel value indexes Walnut-CGB's ready-made RGB565 palette
(`gb->cgb.fixPalette`). Original Game Boy games get a 4-shade green palette.

NES: agnes renders 256x240. The app shows it 1:1 and cuts 11 rows at the top and bottom so it
fits the 218-row app area (`nes_draw()`). Like the Game Boy, it draws every other frame.

**Input.** Each frame the task reads `bsp_buttons_get()` through the calibrated key map
(`os_key_button()`):

| Badge | Game Boy |
|---|---|
| d-pad | d-pad |
| SELECT | A |
| CANCEL | B |
| UP + DOWN together | START |
| LEFT + RIGHT together | SELECT |
| BOOT (back) | quit the app |

Opposite directions never reach a game, so these chords are safe.

**Saves.** Game Boy cart RAM is loaded from `<rom>.sav`, and written back on quit and every
minute while it changes. NES games save the whole machine (`agnes_dump_state`) to
`<rom>.state` on quit and restore it at the next start. `on_exit` sets a quit flag and waits for the task. The task never waits on the LVGL
lock that `on_exit` holds, because `present()` gives up after 4 ms, so there's no deadlock.

The app is an `os_app_t`:

```c
const os_app_t app_gbemu = {
    .id = "retro", .name = "Retro", .symbol = LV_SYMBOL_PLAY, .accent = COL_GREEN,
    .on_enter = enter, .on_exit = leave, .on_key = key,
};
```

## 6. Register it (two one-line edits)

- `main/sources.cmake`: add `apps/gbemu/gbemu_app.c apps/gbemu/nes/agnes_build.c` to `SOLANA_OS_SRCS`. Both the badge and
  the browser emulator build from that list.
- `main/apps/registry.c`: add `app_gbemu` to the `extern` line and `&app_gbemu,` to `os_apps[]`.

## 7. Build

```powershell
idf.py set-target esp32c5
```

```powershell
idf.py build
```

The first build downloads LVGL and the display driver through the component manager. Warnings
count as errors. Ours failed twice before it went green:

1. `implicit declaration of function 'lvgl_port_lock'`: include `esp_lvgl_port.h` and
   `lvgl_private.h`, the way `core/os.c` does.
2. `'%s' directive output may be truncated`: a 255-character file name doesn't fit a
   200-byte path. Check the length and skip names that are too long, instead of `snprintf`.

A green build ends with `Project build complete`. Ours: `solana_os.bin` 0x2412d0 bytes, 44% of
the app partition free.

## 8. Back up the badge before flashing

Find the port in Device Manager under "Ports (COM & LPT)" (`USB-SERIAL CH340 (COMx)`). Then
read all 16 MB.

**One long read failed twice** with `Invalid head of packet ... serial noise`: at 921600 baud
after 236 KB, and at 460800 after 72%. What worked was 1 MB chunks at 230400 baud, each one
checked by esptool's MD5 and retried up to 3 times, then joined:

```powershell
esptool --chip esp32c5 -p COM3 -b 230400 read-flash 0x0 0x100000 chunk00.bin
# ... 0x100000, 0x200000, ... up to 0xF00000, then join the 16 files in order
```

Check the result is exactly 16,777,216 bytes. The backup holds your badge's identity and keys,
so keep it private and outside the repo. The screen goes black while esptool holds the chip in
its bootloader, and comes back when it resets the badge.

What the backup told us before we flashed anything:

- The badge ran Solana OS 2.0.0 in slot `ota_0`, so 2.1.0 plus the app is an upgrade.
- The partition table was byte-identical to our build.

## 9. Flash, upload a ROM, play

```powershell
idf.py -p COM3 -b 230400 flash
```

This writes the bootloader, the partition table, `otadata` and the app in `ota_0`. It doesn't
touch NVS (Wi-Fi, badge ID, keys), the `storage` partition (store apps) or the card. Every
region should report `Hash of data verified.` Later firmware-only updates use
`idf.py -p COMx -b 230400 app-flash`.

The COM number can change when you re-plug the badge (ours went from COM3 to COM5).

**Copy a ROM without a card reader.** `sdrecv` reuses the block protocol of `lua recv`: the
badge asks for base64 a block at a time, writes `<path>.put`, and renames it into place only
after the size and SHA-256 match. From cmd, no environment needed (ESP-IDF's Python has
pyserial):

```bat
D:\esp\.espressif\python_env\idf6.0_py3.13_env\Scripts\python.exe D:\Projects\solana-os\tools\sd_put.py --port COM5 "C:\path\to\game.gb" roms/gb/game.gb
```

Use `roms/gbc/` for Game Boy Color and `roms/nes/` for NES. It runs at about 8 KB/s and ends
with `done: roms/gb/game.gb, ... SHA-256 checked`. A wrong
checksum is refused with `recv: error the SHA-256 doesn't match`, and no partial file is left.

Then open **Retro** in the launcher, pick the console, then the game, and press SELECT.

**Measure.** The app logs `emulated fps / shown fps / resyncs` every 5 s on the console
(115200 baud). Read it with any serial monitor while a game runs.

## 10. Restore the original firmware

```powershell
esptool --chip esp32c5 -p COM5 -b 230400 write-flash 0 badge-full-backup.bin
```

This puts back all 16 MB exactly as they were, identity and store apps included.

## 11. Why it isn't in the App Store

The store only accepts Lua (it compiles `main.lua` when you submit), and three things rule an
emulator out:

1. **Speed.** 200,000 instructions per 30 ms is at most about 6.7 million Lua instructions a
   second. A Game Boy needs about 1 million CPU instructions a second at 15-30 Lua instructions
   each, plus 160x144 pixels 60 times a second: 30-45 million a second. That's 5-7x too slow
   (estimate).
2. **Files.** A Lua app can only read its own bundled files, so it can't see `/roms`. The
   firmware enforces this in `lua_fs.c`, and no app code can change it.
3. **Games.** Bundling a ROM inside a store app would distribute the game, which isn't an option.

So C apps are shared the way this one is: as an open-source repo that people build into
their own firmware and sideload. `GBEMU_SETUP.txt` in the repo root is the beginner's version
of these steps, including how to add the app to an existing solana-os checkout.

## Lessons learned

- **Read the app API before writing code.** `os.h` explains the threading rules, and every
  design choice above follows from them.
- **Build with warnings as errors** and treat each failure as information. Both of ours were
  real bugs.
- **Back up before you write anything** to a device you don't want to lose.
- **Chunk long serial transfers.** A cheap USB-serial chip drops bytes on long runs.
- **Measure, don't guess.** The fps log line exists so the next decision (Game Boy Color, NES)
  rests on a number.
