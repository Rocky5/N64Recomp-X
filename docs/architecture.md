# Architecture (as built)

`docs/PLAN.md` §3 is the design; this file is what exists, kept current.

## The tree

```
games/<id>/          game packs: game.toml, <id>.syms.toml, recomp.toml, icon.png, README.md
  test/              the ROM-free end-to-end test (tools/xbox/make_test_rom.py)
tools/recomp/        N64Recomp at ffb39cd (submodule, unmodified)
tools/n64rx/         the user's tool: n64rx.py + n64rx_lib/ (cli, rom, packs, pipeline, engines, doctor)
tools/xbox/          build (compile_game.py, thinlto_link.py, patch_pbkit.py, xbe_sections.py),
                     setup.sh, docker/, msys/, recomp_scan64.py, make_test_rom.py, make_logo.py,
                     xemu_run.sh, console.py, sym.py, fbdump_to_png.py, rom_leak_check.py, host tests
xbox/include/        xhw.h, xgfx.h, xrom.h (the structs both triples share), xbuiltin.h
xbox/include/xrc/    recomp.h (the generated code's contract), librecomp/sections.h, xrc_game.h, xrc_prelude.h
xbox/src/hw/         the platform (nxdk triple): Wings64-X's, plus xhw_combo.c and the new loading screen
xbox/src/rt/         the runtime (game triple): boot.c, settings.c, mem.c, ovl.c, hooks.c, nogame/
tests/xbox/          the host tests' C
```

## Boot (M0)

1. `xhw_main.c` `main_body`: RAM lock, E:, `boot.log`, previous exit, code
   check, watchdog, clock.
2. `xhw_splash_show`: `XVideoSetMode(640, 480, 32)` and the loading screen's
   thread (level 9, a frame every 33 ms: the aurora, sparks, wordmark, card,
   stage line, bar, video line).
3. The controllers come up (SDL, USB; ~1.5 s) behind it; the BACK window
   starts.
4. `xrc_boot` (`xbox/src/rt/boot.c`): `settings.ini`, the video line
   (`xhw_video_predict`), then by game:
   - none built in: the card says so and how to build one;
   - the test pack: the RDRAM window (`mem.c`), the function table (`ovl.c`),
     the recompiled entrypoint, a card with the result;
   - a game: M1.
5. `xhw_splash_finish`: the minimum hold and the BACK window; the latched
   hold goes to `xrc_settings_apply_hold`.

## The generated code's world

- `rdram` is the window's physical base P (`mem.c`): host address
  `0x80000000 + P` is RDRAM byte 0, so `rdram + (uint32_t)vaddr` needs no
  arithmetic for KSEG0.
- `get_function` (`ovl.c`): a page map over 8 MB of KSEG0 to per-page
  `uint16_t` index arrays; every section of `recomp_overlays.inl` is entered
  at its link address (overlays by DMA source: M5).
- `xrc_game` (written by n64rx into `gen/xrc_game.c`): id, title, title ID,
  RDRAM size, entrypoint, register width, the section table, the icon.

## The exit combos

`xhw_combo.c`: raw pad state, any port, both triggers past 200 + BACK +
BLACK (dashboard) or WHITE (restart). Fed by the loading screen's thread,
the error cards, the watchdog's tick (last resort), and from M1 the game's
reads and the VI thread, all under the pad lock (`xhw_pad_lock` /
`xhw_pad_trylock`).
