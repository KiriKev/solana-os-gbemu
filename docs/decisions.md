# Decisions

### DEC-2026-11-14-01: Game Boy emulator as a built-in C app, not a Lua app
Context: Run a user-supplied Game Boy ROM on the badge. Lua apps are interpreted; the GB CPU needs ~1M instructions/s emulated on one 240 MHz RISC-V core.
Options: Lua store app; built-in C app in main/apps; separate firmware.
Decision: Built-in C app main/apps/gbemu, registered in registry.c and sources.cmake so the browser emulator builds it too.
Evidence: INFERRED from main/core/os.h (C app API) and README (Lua apps run as bytecode). Speed not measured yet; the app logs emulated fps every 5 s.
Revert: delete main/apps/gbemu and the two registry/sources lines.

### DEC-2026-11-14-02: Peanut-GB core, vendored as one header
Context: Need a small portable GB core with no OS dependencies.
Options: Peanut-GB (single header, MIT); gnuboy (GPL, many files); write one.
Decision: Peanut-GB at d0bcca771c83a2638c93a9ae61f3d51f226dc905, ENABLE_SOUND 0 (no speaker listed in README).
Evidence: VERIFIED licence header in main/apps/gbemu/peanut_gb.h lines 2-4 (MIT); 102,999 bytes.
Revert: remove peanut_gb.h.

### DEC-2026-11-14-03: Emulator task at idle priority, yields every frame
Context: App callbacks must not block (os.h). The browser emulator's tasks are cooperative fibers (emulator/platform/freertos_emu.c header).
Options: run frames in on_tick (100 ms, too slow); own task at higher priority (starves idle task watchdog); own task at tskIDLE_PRIORITY with vTaskDelay(0) each frame.
Decision: own task at tskIDLE_PRIORITY; OS, LVGL and radios preempt it.
Evidence: INFERRED from FreeRTOS scheduling rules; freertos_emu.c vTaskDelay(0) yields to the scheduler (VERIFIED by reading lines 226-229).
Revert: change the priority in start() in gbemu_app.c.

### DEC-2026-11-14-04: 1.5x scale, double-buffered image swapped under the LVGL lock
Context: App area is 320x218 (theme.h OS_ROOT_H). GB is 160x144.
Options: 1x centred; 1.5x (240x216); LVGL image scaling (slow transform).
Decision: 1.5x nearest-neighbour in draw_line, two RGB565 buffers in PSRAM, src swapped under lvgl_port_lock(4 ms); a frame that misses the lock is shown later.
Evidence: INFERRED. Shown fps is logged next to emulated fps for measurement.
Revert: SCALE_W/SCALE_H and draw_line in gbemu_app.c.

### DEC-2026-11-14-05: START = UP+DOWN, SELECT = LEFT+RIGHT
Context: Badge has 6 front buttons (os_keys.h), the GB has 8. BOOT is HOME and stays the way out.
Options: A+B chords (games use A+B); hold-timers; opposite-direction chords.
Decision: Opposite-direction chords, which games never see. A brief one-frame direction press may leak while forming the chord.
Evidence: INFERRED.
Revert: joypad() in gbemu_app.c.

### DEC-2026-11-14-06: Private fork repo on KiriKev
Context: Kev asked for a new repo on his GitHub. main/assets fonts and logos may not be reused outside this firmware (LICENSE).
Options: public; private.
Decision: Private repo KiriKev/solana-os-gbemu; main = upstream 64a474b unchanged, work on feat/gbemu-app; upstream remote kept.
Evidence: VERIFIED `gh repo create` output https://github.com/KiriKev/solana-os-gbemu.
Revert: delete the GitHub repo.

### DEC-2026-11-14-07: Repo made public (replaces the visibility part of DEC-06)
Context: Kev 2026-11-14: "keep the repo public". Upstream solana-foundation/solana-os is public too; no ROM is in the repo.
Options: private; public.
Decision: Public, Kev's call.
Evidence: VERIFIED `gh repo view` visibility PUBLIC after `gh repo edit --visibility public`.
Revert: `gh repo edit KiriKev/solana-os-gbemu --visibility private --accept-visibility-change-consequences`.

### DEC-2026-11-14-08: Verified with the badge build only; browser emulator build skipped
Context: The repo has no test suite for C code; the gate is `idf.py build` with -Werror. The browser emulator build needs Emscripten, which is not installed.
Options: install Emscripten now (another download, needs Kev's yes); ship with the badge build green.
Decision: Badge build only. gbemu_app.c is in SOLANA_OS_SRCS, so the emulator build compiles it too, but that is not verified.
Evidence: VERIFIED `idf.py build` (ESP-IDF v6.0.1): "Project build complete", solana_os.bin 0x2412d0 bytes, 44% of the app partition free. The gate can fail: the first two builds failed on gbemu_app.c (implicit lvgl_port_lock; -Werror=format-truncation), both fixed.
Revert: n/a

### DEC-2026-11-14-09: sdrecv console command + tools/sd_put.py for files without a card reader
Context: Kev has no card reader; the badge's USB is a CH340 serial port, so the card is not a USB drive.
Options: USB mass storage (the C5 has no USB OTG, the port is serial); a new protocol; reuse lua recv's block protocol.
Decision: `sdrecv <path> <bytes> <sha256>` in main/lua/lua_cmd.c, reusing recv_stream() (refactored out of lua recv in its own commit). Path relative to /sdcard, [A-Za-z0-9._-/], no "..", max 8 MB; written to <path>.put, renamed over the old file only after size and SHA-256 match.
Evidence: VERIFIED on the badge (COM5): 40,000-byte random file -> "done: test/sdput_test.bin, 40000 bytes, SHA-256 checked, 5 s"; wrong-SHA negative test -> "recv: error the SHA-256 doesn't match", and `ls test` lists only sdput_test.bin (no .put or badsha.bin left behind).
Revert: remove cmd_sdrecv and its registration, tools/sd_put.py.
