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
                     function table), interp (hand-written code), learn (the safety net, the learn log), hooks, nogame/
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
   (`xhw_rom.c`: `default.xbe`'s own `$ROM` section, found in the image
   header at 0x10000 and read from `D:\default.xbe` at its raw offset; a
   ROM file beside the XBE when the build has none) and checked by SHA-1:
   up to 16 MB read whole into RAM (resident), bigger streamed through a
   256 KB buffer for the hash and then served by the page cache
   (`xhw_romcache.c`: the ROM's size reserved, 64 KB chunks committed and
   read on first touch, an LRU over 16 MB, 12 at 720p; the pack's
   `[rom] pin` ranges held). Every reader of the image (PI DMA, the
   `osEPi*` family, `osPiReadIo`, the boot copy) goes through
   `xhw_rom_read` (`xrom.h`; pi.c's `xu_rom_to_rdram`/`xu_rom_read`),
   the RDRAM window, the boot block (`osTvType` ...
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
  interpreted routine (`gen/xrc_interp.c`) is entered at its address. A
  miss goes to the safety net (`learn.c`): code in RDRAM is interpreted
  through a per-address stub entered in the table, and logged for
  `n64rx --learn`; anything else is the fatal card.
- The TLB (`tlb.c`, `hw/xhw_alias.c`): 32 entries as the game wrote them;
  a valid entry's pages are mapped a second time in the x86 page tables at
  the host address `rdram + vaddr` gives, onto the frames that hold the
  N64 pages, so the generated code reads TLB-mapped data with its ordinary
  loads. The runtime's osMapTLB family (`ultra/more2.c`) and the
  interpreter's `tlbwi`/`tlbp`/`tlbr` both write it; `osVirtualToPhysical`
  and `__osProbeTLB` translate through it. Kill switch `-DXRC_TLB=0`.
- `xrc_game` (`gen/xrc_game.c`, written by n64rx): id, title, title ID,
  RDRAM size, entrypoint, register width, the section table, the icon,
  the save size.
- libultra's objects live in RDRAM in N64 layout; host state (kernel
  threads, events, timers) hangs off side tables keyed by N64 address.
  One N64 thread is one kernel thread (levels 2..14); one recursive lock
  stands for the interrupt mask.
- Saves (`ultra/save.c`, `savedev.c`, `pfs.c`): every image (EEPROM, a
  Controller Pak per port, SRAM or FlashRAM) lives in `save.c` with its
  file in `E:\UDATA\<title id>\` (`<id>.eep`, `<id>.mpk`, `<id>-2.mpk`..,
  `<id>.sra` / `<id>.fla`, the cartridge's byte order), read on first use,
  written by one writer thread within a second of a write and by the exit
  combos' flush, under one recursive lock. `savedev.c` holds the protocols
  with no platform: the pak's PIF commands 0x02/0x03 with libultra's CRCs,
  the Rumble Pak's probe and motor, SRAM and the Flash's commands on PI
  domain 2. The replaced osPfs* (`pfs.c`), osFlash* and osEPi* (`extra.c`)
  work on the same images as a game's own recompiled libultra reaching
  them through the PIF (`si.c`) or the handle family. The SRAM-or-Flash
  question is answered on the game's first domain-2 access.
- Graphics and audio tasks run on the calling thread (`sp.c`): the
  interpreter for graphics, `asp_hle` for audio; their done events are put
  ahead of retraces already queued.
- The audio microcode registry (`audio.c` at the first task,
  `asp_hle_identify` in `xbox/src/hw/asp_abis.inc`): the microcode's data
  segment holds its command jump table right before the ADPCM nibble masks
  `f000 0f00 00f0 000f`; its place, its length and the entries that point
  at opcode 0's handler pick the ABI: `aspMain` (ABI1: table at +0x10, 16
  entries), the nead family (+0x10, 32 or 24 entries; variant flags from
  which opcodes exist) or `n_aspMain` (+0x00, 16). `asp_hle_run` decodes
  that ABI from then on; an unknown microcode is logged and plays silence.
  Nothing from any microcode is stored.

## The BACK menu

A BACK tap (BACK let go with nothing else pressed) on any port opens it.
The VI thread stops posting retraces (the game waits at its retrace), the
controller reads go neutral, a graphics task that arrives waits. The menu's
thread holds the renderer and presents the held game frame, dimmed, with the
card (`xhw_panel_render`: the loading screen's aurora, sparks, light sweep
and green bar) composited on the GPU; the VI thread reads the pads. Close:
`settings.ini` saved when something changed.

The "Controller Pak" row (per port) opens the **Paks page** on the same
card (`docs/look/paks-page-host.png`): Port, In the port (Auto, Controller
Pak, Rumble Pak, Nothing), Pak (A-D), the pak's notes in a window of six
(`XHW_PANEL_ITEM` rows: A acts on them, no `< >`), Format pak, Back. The VI
thread only asks; the menu's thread does the work under the save lock and
reads the note table again every frame (`savedev.c` `xu_pak_notes`). Paks
A-D are the files `<id>.mpk`, `<id>-2.mpk`, `-3`, `-4` (the names saves had
before the page, one per port); each pak is always in exactly one port, so
taking another port's pak swaps the two (`save.c` `xu_pak_insert`). A change
is a hot-swap: the port's next status has CONT_CARD_PULL (0x02) once
(`xu_pak_pull`, `xu_pak_status`), and the runtime's osPfs* compare the
OSPfs's ID with the pak's as libultra's `__osCheckId` does, so a game sees
`PFS_ERR_NEW_PACK` and initialises the pak again. A delete
(`xu_pak_note_delete`: osPfsDeleteFile's work), a copy to another of the
game's paks (`xu_pak_note_copy`: X on a note, Left / Right pick the pak) and
a format (a new ID) mark the pak dirty, so the save writer and the exit combos' flush write them.

## The exit combos

`xhw_combo.c`: raw pad state, any port, both triggers past 200 + BACK +
BLACK (dashboard) or WHITE (restart). Fed by the game's reads, the BACK
menu's reads, the VI thread when the game stops reading, the loading
screen, the error cards and the watchdog's tick, all under the pad lock.
A dirty save is flushed first; the next boot names the exit.
