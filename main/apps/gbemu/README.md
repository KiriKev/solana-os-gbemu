# Retro: Game Boy, Game Boy Color and NES for Solana OS (badge firmware)

A built-in C app: pick a console, then a game from its folder on the microSD card. No games
are included.

## Files

- `gbemu_app.c`: the app (menus, emulator task, display, input, saves)
- `walnut_cgb.h`: Walnut-CGB core for Game Boy and Game Boy Color (MIT, licence in the file
  header), https://github.com/Mr-PauI/Walnut-CGB at commit a42c186917516cc9c80515ceae1121600a071628
- `nes/agnes.c`, `nes/agnes.h`: agnes NES core (MIT, licence in the file headers),
  https://github.com/kgabis/agnes at commit 0e4220b084c467e39c04805d955e78c463feadd0
- `nes/agnes_build.c`: builds `agnes.c` unchanged with `-Wtype-limits` off
- Registered in `main/sources.cmake` (badge + browser emulator) and `main/apps/registry.c`

## Use

1. Copy games to the card:

   | Console | File | Folder |
   |---|---|---|
   | Game Boy | `.gb` | `roms/gb` (also `roms/`) |
   | Game Boy Color | `.gbc` | `roms/gbc` |
   | NES | `.nes` | `roms/nes` |

   Without a card reader, over the USB cable:
   `python tools/sd_put.py --port COM5 <file>.nes roms/nes/<name>.nes`
   (ESP-IDF's Python has pyserial; about 8 KB/s, so a 1 MB file takes about 2 minutes).
2. Open **Retro** in the launcher, pick the console, pick the game, press SELECT. CANCEL goes
   back to the consoles.
3. Controls: d-pad = d-pad, SELECT = A, CANCEL = B, UP+DOWN = START, LEFT+RIGHT = SELECT.
   BOOT (back of the badge) quits.
4. Saves: Game Boy cart RAM goes to `<name>.sav` on quit and every minute while it changes.
   NES saves the whole machine to `<name>.state` on quit and continues from there next time.

## Design notes

- The emulator runs in its own task at idle priority and yields every frame, so the OS,
  LVGL and radios always preempt it. The browser emulator's tasks are cooperative, which
  is why the yield is needed there.
- Picture: Game Boy 160x144 scaled 1.5x to 240x216. NES 256x240 shown 1:1 with 11 rows cut
  at the top and bottom (256x218), the app area's height. Double-buffered RGB565 in PSRAM,
  swapped under the LVGL lock, so there's no tearing.
- Every frame is emulated and every other one is drawn.
- The console logs `emulated fps / shown fps / resyncs` every 5 s per system. That's the speed
  measurement.
- No sound: the solana-os README lists no speaker.
