# Architecture (as built)

`docs/PLAN.md` §3 is the design; this file is what exists, kept current.
The ROM-only path has its own page: `docs/analyzer.md`.

## The tree

```
games/<id>/          game packs (optional since PLAN.md 3.14): game.toml, symbols, recomp.toml, icon.png
  test/              the ROM-free end-to-end test (tools/xbox/make_test_rom.py)
  pw64/              Pilotwings 64: the A/B game; its MIT decomp's symbols are the analyser's first answer key
tools/recomp/        N64Recomp at ffb39cd (submodule, unmodified)
tools/n64rx/         the user's tool: n64rx.py + n64rx_lib/: cli, rom, packs, pipeline (recompile, the
                     refusal retries, gen/ extras: xrc_interp.c, xrc_missing.c, the ultra table), engines,
                     doctor, romonly (the generated pack), learn (--learn), saves (libultra globals the
                     saves need), analyze/ (the ROM analyser: mips, funcs, segments, features, libultra, emit)
tools/xbox/          build: compile_game.py, thinlto_link.py, patch_pbkit.py, xbe_sections.py (title, icon,
                     $ROM), setup.sh, docker/, msys/
                     checks: decomp_check.py (the analyser against a decomp), sweep.py (every ROM in a
                     folder), rom_leak_check.py, recomp_scan64.py, syms_tools.py, make_test_rom.py
                     console + xemu: xemu_run.sh, scenarios/, console.py, sym.py, fbdump_to_png.py,
                     auddump_to_wav.py, eeprom_video.py, make_logo.py
                     host tests: test_*.py (docs/testing.md)
xbox/include/        xhw.h, xgfx.h, xrom.h (the structs both triples share), xbuiltin.h
xbox/include/xrc/    recomp.h (the generated code's contract), librecomp/sections.h, xrc_game.h, xrc_prelude.h
xbox/src/hw/         the platform (nxdk triple): Wings64-X's, plus xhw_combo.c (exit combos), the loading
                     screen and the BACK menu's card (xhw_splash_draw.c), xhw_rom.c ($ROM, the SHA-1),
                     xhw_romcache.c (the ROM page cache), xhw_alias.c (page-table aliases for the TLB),
                     asp_hle.c + asp_abis.inc (audio HLE: aspMain, nead, n_aspMain, on the word-swapped image)
xbox/src/gfx/        the display-list interpreter (game triple): Wings64-X's Fast3D reading RDRAM by N64
                     address, plus F3DEX/F3DEX2 (f3dex_gbi.h), the microcode registry (ucode.c), CI
                     textures (texconv.c), generic hor+, the BACK menu's held frame (docs/renderer.md)
xbox/src/rt/         the runtime (game triple): boot, settings, settings_menu (the BACK menu, the Paks
                     page), video (the probation), audio (the registry, the AI), gfx, mem (the RDRAM
                     window), ovl (the function table), interp (the MIPS interpreter), learn (the safety
                     net, code capture, the learn lines), guard (captured code checked against RDRAM),
                     tlb (the game's TLB, misses, KSEG2/3), mmio (the register model), strand (a game's
                     setjmp/longjmp as coroutines), trace (crash and stall traces), hooks, nogame/
xbox/src/rt/ultra/   libultra's surface: thread, time, vi, pi (ROM reads, domain 2), si (pads, the PIF,
                     paks), sp, ai, misc, extra (osEPi*, motor, Flash, probes), save + savedev + pfs
                     (every save type), more2 (the TLB family, __osEnqueueAndYield), more3 (internals
                     N64Recomp leaves bodiless)
tests/xbox/          the host tests' C
docs/look/           baseline shots: the loading screen (xemu, STILL); the BACK menu card and Paks page (host)
```

## Boot

1. `xhw_main.c` `main_body`: RAM lock, E:, `boot.log`, previous exit, code
   check, watchdog, clock.
2. `xhw_splash_show`: `XVideoSetMode(640, 480, 32)` and the loading screen's
   thread (level 9: the aurora, sparks, wordmark, game card, stage line,
   bar, video line).
3. The controllers come up (SDL, USB; ~1.5 s) behind it; the BACK window
   starts.
4. `xrc_boot` (`rt/boot.c`): `settings.ini`, the video line, then no game
   (a card), the test pack (its checks on a card), or a game:
   - **the ROM** (`xhw_rom.c`): `default.xbe`'s own `$ROM` section, found in
     the image header the kernel mapped at 0x10000 and read from
     `D:\default.xbe` at its raw offset (a ROM file beside the XBE when the
     build has none), checked by SHA-1. Up to 16 MB it is read whole into
     RAM (resident); a bigger one is hashed through a 256 KB buffer and then
     served by the page cache (`xhw_romcache.c`: the ROM's size reserved,
     64 KB chunks committed and read on first touch, an LRU over 16 MB, 12
     at 720p, the pack's `[rom] pin` ranges held). Every reader of the
     image (PI DMA, the `osEPi*` family, `osPiReadIo`, the boot copy) goes
     through `xhw_rom_read` (`xrom.h`; `pi.c`'s `xu_rom_to_rdram` /
     `xu_rom_read`);
   - the RDRAM window, the boot block (`osTvType` ... `osMemSize`), the
     boot copy (1 MB from ROM 0x1000), the function table, the mode switch
     (`xhw_video_boot`), the renderer, the recompiled entrypoint.
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
- KSEG1 (`mem.c`, `mmio.c`): the window is aliased a second time at
  `rdram + 0xA0000000` (host P + 0xA0000000), so uncached RDRAM is the same
  bytes (`-DXRC_KSEG1=0`). When the range is refused as a whole (on red,
  8 MB windows at 0xA3631000: a page there is the kernel's), the free
  pages are aliased one by one and the first refused one is logged with
  its PDE/PTE (`xhw_alias_why`). Past RDRAM, the RCP's registers, the PIF and the
  cartridge are a register model: the analyser sends every live function
  with a constant hardware address that the runtime does not replace to the
  interpreter (`emit.hw_funcs`), whose KSEG1 loads and stores go to
  `xrc_mmio_read`/`_write`; writes with an effect go through the runtime's
  own paths (PI_WR_LEN -> `xu_pi_dma` + the PI event, SI_PIF_ADDR_* ->
  the PIF in `si.c`, AI_LEN/AI_DACRATE -> `ai.c`, VI_ORIGIN -> the swap,
  SP DMA into a DMEM/IMEM model and SP_STATUS's clear-halt -> the task at
  DMEM 0xFC0). Each register block's 4 KB page is also a page of ours
  aliased at its host address (`xhw_alias_page`), holding the registers, so
  code still recompiled reads idle values (the VI thread rewrites the
  status words each retrace; its writes have no effect). `-DXRC_MMIO=0`.
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
- libultra functions N64Recomp leaves without a body and nothing else
  provides get a weak body in `gen/xrc_missing.c` that logs `[ULTRA]
  missing <name>` once and returns 0; the runtime's own (`ULTRA(...)`)
  replaces it. The runtime's libultra is also entered in the function
  table at the game's addresses (`gen/xrc_interp.c`'s ultra table), so
  interpreted code calling it by address reaches the replacement.
- Graphics and audio tasks run on the calling thread (`sp.c`): the
  interpreter for graphics, `asp_hle` for audio. Their done events are held
  for the thread that started the task and go out (ahead of retraces already
  queued) when it next blocks, or at a retrace once it is blocked or two
  retraces on; DP done only for a list that ended with a FullSync
  (`docs/decisions.md`: "A task's done events belong to its thread").
- Threads (`ultra/thread.c`): one kernel thread per N64 thread; a blocking
  call sleeps on the thread's event under the lock. osStopThread /
  osDestroyThread of another thread stop it at its next block; created and
  started again, it starts over at its entry. A game's own setjmp/longjmp
  (Mario Party's processes) run as coroutines (`strand.c`): each context a
  host thread of its own, the same N64 thread to the runtime, one running
  at a time; n64rx turns each call to setjmp into a host `__builtin_setjmp`
  in the caller (`XRC_SETJMP`). A destroyed or returned thread's record
  and host thread serve the next new OSThread (`XRC_THREAD_REUSE`).
- Boot: the ROM's IPL3 copy of itself is put in low RDRAM as the console
  leaves it (`boot.c`, `XRC_IPL3_LEFT`: Banjo-Kazooie's and Donkey Kong
  64's anti-tamper checks read it).
- RSP tasks by type (`ultra/sp.c`): graphics to the interpreter, audio to
  the HLE, Ocarina of Time's JPEG decoder task to `jpeg.c`; others logged
  and not run. Frames are presented at the game's swap once it swaps
  (`gfx.c`, `XRC_PRESENT_ON_SWAP`: a frame may span several tasks).
- The VI (`ultra/vi.c`): `osViGetCurrentFramebuffer`/`NextFramebuffer`
  answer K0BASE until the first swap, as libultra's `__osViInit` leaves
  them (`XRC_VI_K0BASE`); `xu_vi_rows` gives the renderer the mode's rows.
- A call through a pointer into a KUSEG page the TLB does not map runs the
  game's refill code and fault path first, as the CPU's miss would
  (`XRC_LOOKUP_TLB`, `learn.c`; GoldenEye's evicted 0x7F000000 pages).
- Code the TLB maps: a miss (a fault at a KUSEG host address) runs the
  game's own refill vector or `__osException` in the interpreter, then
  libultra's fault path (the game's fault thread pages it in); KSEG2/3
  (Paper Mario's 0xE0200000) cannot be aliased on the host, so a game that
  maps it is built with `XRC_HIGH_TLB` (learnt): `XRC_ADDR` sends addresses
  at 0xC0000000 and up through the TLB.
- Code captured from RDRAM (`learn.c`, the `--learn` rounds) is
  recompiled with a hash of the bytes it came from (`guard.c`): a function
  whose RAM now holds another overlay runs interpreted and is captured
  again.
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
and green bar) composited on the GPU; the VI thread reads the pads. When the
texture pool has no room for the card (1 MB) or the held frame, the
interpreter's texture cache is dropped (the game is frozen; its textures
reload from RDRAM after; `-DXGFX_HOLD_EVICT=0`). Close: `settings.ini`
saved when something changed.

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

## Overlays in the function table (ovl.c)

Sections that share vram are found at init (`s_ovl`); each keeps the table
indices it got there. `pi.c` reports every DMA to `xrc_ovl_dma`; a DMA from
inside an overlay's ROM range to its own RAM position maps that overlay:
its RAM range is cleared in the page arrays, the interpreted routines there
entered (they run what is in RAM), then its functions. A function of the
overlay that was there before then misses and goes to the safety net.
`[OVL]` lines log the switches. Kill switch `-DXRC_OVERLAYS=0`.

HAL's object manager (Snap) runs object threads with osStopThread(NULL) /
osStartThread each frame and recreates them on the same OSThread: thread.c
waits a stopped thread on its event, restarts a recreated one at its new
entry (`__builtin_setjmp` in `thread_body`), and has osStartThread of a
higher N64 priority on the same kernel level wait until it stops or blocks.
