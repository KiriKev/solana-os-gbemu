# Game Boy app for Solana OS (badge firmware)

A built-in C app that plays a `.gb` ROM from the microSD card. No ROM is included.

## Files

- `main/apps/gbemu/gbemu_app.c`: the app (ROM list, emulator task, display, input, saves)
- `main/apps/gbemu/peanut_gb.h`: Peanut-GB core (MIT, licence in the file header),
  https://github.com/deltabeard/Peanut-GB at commit d0bcca771c83a2638c93a9ae61f3d51f226dc905.
- Registered in `main/sources.cmake` (badge + browser emulator) and `main/apps/registry.c`.

## Use

1. Copy a ROM to the card: `/roms/<name>.gb` (or the card root).
2. Open **Game Boy** in the launcher, pick the ROM, press SELECT.
3. Controls: d-pad = d-pad, SELECT = A, CANCEL = B, UP+DOWN = START, LEFT+RIGHT = SELECT.
   BOOT (back of the badge) quits. The save is written to `<name>.sav` on quit and
   every minute while it changes.

## Design notes

- Emulator runs in its own task at idle priority and yields every frame, so the OS,
  LVGL and radios always preempt it. The browser emulator's tasks are cooperative, which
  is why the yield is needed there.
- Picture: 160x144 scaled 1.5x to 240x216 (the app area is 320x218). Double-buffered
  RGB565 in PSRAM, swapped under the LVGL lock, so there's no tearing.
- Peanut-GB frame skip is on: all 60 frames are emulated and every other one is drawn.
- Console logs `emulated fps / shown fps / resyncs` every 5 s. That's the speed measurement.
- No sound: the README of solana-os lists no speaker.
