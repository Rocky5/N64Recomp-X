# Architecture (as built)

`docs/PLAN.md` §3 is the design; this file is what exists, kept current.
The ROM-only path has its own page: `docs/analyzer.md`.

## The tree

```
games/<id>/          game packs (optional since PLAN.md 3.14): game.toml, symbols, recomp.toml, icon.png
  test/              the ROM-free end-to-end test (tools/xbox/make_test_rom.py)
  pw64/              Pilotwings 64: the A/B game; its MIT decomp's symbols are the analyser's first answer key
tools/recomp/        N64Recomp at ffb39cd (submodule, unmodified)
tools/n64rx/         the user's tool: n64rx.py + n64rx_lib/ (cli, rom, packs, pipeline, engines, doctor,
                     romonly: the generated pack) and n64rx_lib/analyze/ (the ROM analyser)
tools/xbox/          build (compile_game.py, thinlto_link.py, patch_pbkit.py, xbe_sections.py), setup.sh,
                     docker/, msys/, recomp_scan64.py, syms_tools.py, make_test_rom.py, make_logo.py,
                     xemu_run.sh, eeprom_video.py, console.py, sym.py, fbdump_to_png.py, auddump_to_wav.py,
                     rom_leak_check.py, scenarios/, the host tests
xbox/include/        xhw.h, xgfx.h, xrom.h (the structs both triples share), xbuiltin.h
xbox/include/xrc/    recomp.h (the generated code's contract), librecomp/sections.h, xrc_game.h, xrc_prelude.h
xbox/src/hw/         the platform (nxdk triple): Wings64-X's, plus xhw_combo.c, the loading screen and the
                     BACK menu's card (xhw_splash_draw.c), asp_hle.c on the word-swapped image
xbox/src/gfx/        the display-list interpreter (game triple): Wings64-X's Fast3D, reading RDRAM by
                     N64 address, plus F3DEX/F3DEX2 (f3dex_gbi.h) and the microcode registry (ucode.c;
                     docs/renderer.md), CI textures, generic hor+, the held frame for the BACK menu
xbox/src/rt/         the runtime (game triple): boot, settings, settings_menu (the BACK menu), video
                     (the probation's A and timeout), audio (asp_hle and the AI), gfx, mem, ovl (the
                     function table), interp (hand-written code), hooks, nogame/
xbox/src/rt/ultra/   libultra's surface: threads, queues, events, time, VI, PI (and the osEPi family), SI
                     (pads, EEPROM, the PIF), SP, AI, misc, extra (rumble, paks, Flash, VRU, probes)
tests/xbox/          the host tests' C
docs/look/           baseline shots: the loading screen (xemu, STILL), the BACK menu card (host)
```

## Boot

1. `xhw_main.c` `main_body`: RAM lock, E:, `boot.log`, previous exit, code
   check, watchdog, clock.
2. `xhw_splash_show`: `XVideoSetMode(640, 480, 32)` and the loading screen's
   thread (level 9: the aurora, sparks, wordmark, game card, stage line,
   bar, video line).
3. The controllers come up (SDL, USB; ~1.5 s) behind it; the BACK window
   starts.
4. `xrc_boot` (`rt/boot.c`): `settings.ini`, the video line, then: no game
   (a card), the test pack (its checks on a card), or a game: the ROM read
   and checked by SHA-1, the RDRAM window, the boot block (`osTvType` ...
   `osMemSize`), the boot copy (1 MB from ROM 0x1000), the function table,
   the mode switch (`xhw_video_boot`), the renderer, the recompiled
   entrypoint.
5. `xhw_splash_finish`: the minimum hold and the BACK window; the latched
   hold goes to `xrc_settings_apply_hold`.

## The generated code's world

- `rdram` is the window's physical base P (`mem.c`): host address
  `0x80000000 + P` is RDRAM byte 0. The image is word-swapped: words
  native, halfwords at `^2`, bytes at `^3`.
- `get_function` (`ovl.c`): a page map over 8 MB of KSEG0 to per-page
  `uint16_t` index arrays; every section of `recomp_overlays.inl` and every
  interpreted routine (`gen/xrc_interp.c`) is entered at its address.
- `xrc_game` (`gen/xrc_game.c`, written by n64rx): id, title, title ID,
  RDRAM size, entrypoint, register width, the section table, the icon,
  the save size.
- libultra's objects live in RDRAM in N64 layout; host state (kernel
  threads, events, timers) hangs off side tables keyed by N64 address.
  One N64 thread is one kernel thread (levels 2..14); one recursive lock
  stands for the interrupt mask.
- Graphics and audio tasks run on the calling thread (`sp.c`): the
  interpreter for graphics, `asp_hle` for audio; their done events are put
  ahead of retraces already queued.

## The BACK menu

A BACK tap (BACK let go with nothing else pressed) on any port opens it.
The VI thread stops posting retraces (the game waits at its retrace), the
controller reads go neutral, a graphics task that arrives waits. The menu's
thread holds the renderer and presents the held game frame, dimmed, with the
card (`xhw_panel_render`: the loading screen's aurora, sparks, light sweep
and green bar) composited on the GPU; the VI thread reads the pads. Close:
`settings.ini` saved when something changed.

## The exit combos

`xhw_combo.c`: raw pad state, any port, both triggers past 200 + BACK +
BLACK (dashboard) or WHITE (restart). Fed by the game's reads, the BACK
menu's reads, the VI thread when the game stops reading, the loading
screen, the error cards and the watchdog's tick, all under the pad lock.
A dirty save is flushed first; the next boot names the exit.
