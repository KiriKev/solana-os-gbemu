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

### DEC-2026-11-14-10: Share as an open-source sideload repo, no PR to solana-os
Context: Fabian Schuh (Solana OS) in chat: "For C apps I would just have them in an open source GitHub repository and people can add it themselves to side load" and "I wouldn't do it as PR into Solana OS though".
Options: PR upstream; App Store (Lua only, ruled out in DEC-08 notes and docs/GBEMU_TUTORIAL.md section 11); open-source fork people build themselves.
Decision: Fork KiriKev/solana-os-gbemu with feat/gbemu-app as the default branch, a README banner, GBEMU_SETUP.txt (beginner steps with the reason for each, plus how to add the app to an existing checkout) and docs/GBEMU_TUTORIAL.md.
Evidence: REPORTED (chat message relayed by Kev 2026-11-14).
Revert: set main back as the default branch; remove the banner and the two docs.

### DEC-2026-11-14-11: Retro app: Walnut-CGB (GB + GBC) and agnes (NES), console menu first
Context: Kev wants one app that plays Game Boy, Game Boy Color and NES, picking the console first, then the game. Peanut-GB has no colour mode (0 matches for cgb in peanut_gb.h).
Options: Retro-Go (GPL, replaces Solana OS, no ESP32-C5 target); two MIT cores in the existing app.
Decision: Walnut-CGB a42c1869 replaces Peanut-GB (deletion in its own commit), agnes 0e4220b0 for NES via nes/agnes_build.c (-Wtype-limits off, vendored file unchanged), `struct gb_s;` forward declaration for walnut_cgb.h's prototypes. Folders roms/gb (+ legacy roms/), roms/gbc, roms/nes. App id "retro", name "Retro"; folder and symbol names stay gbemu to keep the diff small. NES picture 256x218 (11 rows cut top and bottom), NES saves as an agnes state on quit.
Evidence: VERIFIED `idf.py build`: Project build complete, solana_os.bin 0x245ca0 bytes, 43% free; flashed to the badge on COM5 ("Hash of data verified"); badge console `apps` lists "retro    Retro" and `open retro` logs "os: app: retro". NOT VERIFIED: a GBC or NES game running on the badge, and the fps of either.
Revert: git revert of this commit, then the Peanut-GB deletion commit.

### DEC-2026-11-14-12: -O2 for the two Retro emulator files
Context: Game Boy ran 20-28 of 59.7 fps, 19-29 ms emulate+draw per frame (budget 16.74), NES about 5 fps (165-190 ms). The firmware builds with -Os.
Options: leave -Os; -O2 for the whole firmware (bigger binary, untested elsewhere); -O2 for gbemu_app.c and agnes_build.c only.
Decision: per-file -O2 in main/CMakeLists.txt (badge build only; the browser emulator has its own flags). Verified in the verbose build that -O2 comes after -Os on the compile line, so it wins.
Evidence: VERIFIED badge console with a Game Boy Color-enhanced 1 MB game (colour mode, core state 50420 B in SRAM): after the change 24-29.5 fps, 15.9-25.5 ms/frame, vs 19.6-27.8 fps, 19-29 ms before. About 10-20 % faster; the emulation alone still takes about the whole frame budget, and per frame ~35 ms pass in total, so display and OS take about as long again.
Revert: remove the set_source_files_properties block from main/CMakeLists.txt.

### DEC-2026-11-15-13: Link cable over the badge mesh, numbered bytes, no peer pinning
Context: Kev wants Game Boy link play (trading) between two badges. The badge USB port is a CH340 device, so two badges can't talk over a USB cable; ESP-NOW via os_mesh is the existing badge-to-badge path. Only one badge is available, so the link itself can't be tested.
Options: raw os_radio (os_radio.h: "Only os_mesh uses this"); os_mesh APP messages; a PC relaying two COM ports.
Decision: os_mesh APP messages {"app":"retro-link","b","m","n"} to the picked partner only. The clocking side numbers transfers and waits up to 60 ms for the answer to its current number (not at all once the app is closing); the clocked side tags its byte with the transfer it answers, sends it once plus every 200 ms while waiting, and takes the next byte the partner clocked. Stale, duplicate and same-role bytes are dropped. No os_mesh_pin, because the pin is a single shared flag that Retro's unpin would clear for a trade or X Dex partner. If the emulator task outlives leave()'s 3 s wait, its queue and semaphore are leaked (the task holds its own copies in emu_t), not freed.
Evidence: VERIFIED idf.py build: Project build complete, solana_os.bin 0x247a60 bytes. A 3-lens review workflow (15 agents) confirmed 12 findings against the first version (stall past leave() then use-after-free, slave resends ~1000/s, stale bytes shifting exchanges, NULL s_list in tick, selection by row in a re-sorting list, peer cap before filtering, shared pin, stale view across launches); all are addressed in this commit. NOT VERIFIED: any link session (needs a second badge); flashing this build (badge was unplugged).
Revert: git revert this commit.

### DEC-2026-11-15-14: Web installer with esptool-js directly, not ESP Web Tools
Context: Kev wants a click-to-install path for laypeople: install Retro, then copy a roms folder. The badge must keep its NVS (badge ID, keys, Wi-Fi) and store apps.
Options: ESP Web Tools install button; esptool-js driven by our own page; a Windows .bat with a portable esptool.
Decision: our own page (installer/) on esptool-js 0.7.0 (21.09.2026, has esp32c5): writes the four parts from build/flasher_args.json with eraseAll false and MD5 verification, 230400 baud, CH340-only port filter. Copy games speaks sdrecv over Web Serial (installer/protocol.js), mapping roms/gb|gbc|nes (folder first, else file ending) and sanitizing names to the badge's path rule. Hosted as a static site (GitHub Pages) built by tools/build_installer.py.
Evidence: VERIFIED esp-web-tools 10.4.0 src/install-dialog.ts: "Default is to erase a device that does not support Improv Serial", and with new_install_prompt_erase the user is asked, so a layperson could still wipe the badge. VERIFIED `node installer/test.mjs`: 24 passed, 0 failed against a simulated sdrecv badge; mutants (chunk off by one, ".." kept, folder ignored) failed 11, 2 and 2 checks. MD5 matches Node's crypto on 7 cases plus a 2.39 MB buffer, and a mutated shift constant is caught. NOT VERIFIED: the page with a real badge (Web Serial needs a person to pick the port).
Revert: delete installer/ and tools/build_installer.py.

### DEC-2026-11-15-15: Backup and restore in the web installer
Context: Kev: people should be able to get back to their previous state. The installer keeps NVS, but there was no way back to the original firmware.
Options: link to the command-line backup in GBEMU_SETUP.txt only; a backup/restore pair on the page.
Decision: "Back up badge" reads all 16 MB in 1 MB chunks with esptool-js readFlash, checks each chunk against the MD5 the chip sends after it, retries a chunk up to 3 times with a reconnect (long reads over the CH340 broke off twice at 921600 and 460800 baud on 2026-11-14), checks the result looks like a badge (0xE9 at 0x2000, 0xAA50 at 0x8000) and downloads it. "Restore backup" accepts only such a file, asks for confirmation, writes it at 0x0 with MD5 verification.
Evidence: VERIFIED `node installer/test.mjs`: 32 passed, 0 failed; mutants (MD5 check removed, partition magic loosened, no retry) fail 2, 1 and 1 checks. VERIFIED the magic bytes against Kev's real backup (0xe9 at 0x2000, aa50 at 0x8000, 16777216 bytes). INFERRED: that the stub sends a 16-byte MD5 packet after each read (esptool's read_flash protocol); esptool-js 0.7.0 readFlash does not read it itself. NOT VERIFIED on a badge.
Revert: remove backup()/restore() from installer/app.js, the helpers from protocol.js and the two cards from index.html.

### DEC-2026-11-15-16: Game Boy speed: no core tweak kept; the display is the remaining lever
Context: Kev finds ~50 % speed cumbersome. Each variant was built, flashed and measured automatically: start the first Game Boy game over the console, 40-50 s of the emulator's 5 s stat lines, median of windows 2+ (same 1 MB colour-mode game every run, badge untouched).
Options and results (median emulated fps, target 59.7):
  baseline (Walnut-CGB, -O2): 28.5 (repeat after all trials: 27.9)
  display off (no present): 39.6
  gb_run_frame_dualfetch: 25.8
  WALNUT_GB_HIGH_LCD_ACCURACY 0: 27.6
  hot path in IRAM via linker fragment (~22 KB; CPU loop at 0x4080d7ac, verified in the map): 29.4, repeat 28.6 (core state fell to PSRAM)
  IRAM + core state in SRAM (SRAM_KEEP 32 KB): 28.7
  ROM bank 0 copied to SRAM: 27.8
  WALNUT_FULL_GBC_SUPPORT 0 (no colour): 30.5
  Peanut-GB instead of Walnut-CGB (no colour): 29.1
Decision: keep none of the core/memory variants (all within noise, or cost colour). The only large measured lever is the display (+39 % with it off), so the next step is drawing frames to the panel directly instead of through LVGL. Even with a free display the emulation alone measured ~40 fps, so full speed is not expected on this chip with these cores.
Evidence: VERIFIED, the numbers above from the badge console (scratchpad measure.py, trial.ps1). Run-to-run spread is about ±1.5 fps (min/max per run in the session log).
Revert: n/a (no code change kept).
