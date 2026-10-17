# Testing

## Host tests (no ROM; CI runs them on Linux, macOS and Windows)

```sh
python tools/xbox/rom_leak_check.py     # the tree holds nothing from a ROM
python tools/xbox/test_pad_combo.py     # the exit-combo detector (xhw_combo.c's core)
python tools/xbox/test_splash.py        # the loading screen's layout, palette, STILL (needs nxdk's font)
python tools/xbox/test_settings.py      # settings.ini parse and format
python tools/xbox/test_n64rx.py         # the tool: ROMs, packs, messages, cache, output folder, doctor, CLI
python tools/xbox/test_recomp_h.py      # our recomp.h vs upstream's: x86-64, i686 (CC32), 32- vs 64-bit registers
python tools/xbox/test_syms.py          # every committed symbol file against the function table's rules
python tools/xbox/test_scan64.py        # the register-width scan on hand-assembled functions (rabbitizer)
python tools/xbox/test_time.py          # the counter follows the ACPI clock; nothing else reads the TSC
python tools/xbox/test_mmio.py          # the RCP register model through the interpreter: KSEG1 RDRAM, PI DMA, SI's PIF block, AI, VI_ORIGIN, the SP task by DMEM, MI, the cartridge, RDRAM past its end
python tools/xbox/test_interp.py        # the MIPS interpreter: each instruction class vs expected results, 64- and 32-bit registers, the learn log's spans, the TLB (tlb.c, its page-table side recorded)
python tools/xbox/test_asp_hle.py       # the audio HLE: every command of every ABI vs a reference, SIMD = C, native = word-swapped RDRAM (hash: aspMain, hash2: nead + n_aspMain)
python tools/xbox/test_xbe_rom.py       # the ROM inside the XBE: xbe_sections' $ROM, found and checked by xhw_rom.c's parser
python tools/xbox/test_romcache.py      # the ROM page cache's core: chunk spans, word reads, the LRU and its cap, pins, failed fills
python tools/xbox/test_saves.py         # saves: pak CRCs and PIF round trips, the blank format, Rumble, osPfs*, Flash, SRAM by PI DMA
python tools/xbox/test_f3dex.py         # the display-list interpreter on a fake back end: Fast3D, F3DEX (old and new), F3DEX2, the ucode switch, CI textures
python tools/xbox/test_fbfx.py          # framebuffer readback's bookkeeping: colour-image slots, stale/hot marks, the RGBA5551/8888 conversion
python tools/xbox/test_yaz0.py          # Yaz0 ROMs on a synthetic ROM: the decoder's every code path, the DMA table found by behaviour, files at their vrom, the table rewritten
python tools/xbox/test_entry.py         # the analyser's entry-code shapes on hand-assembled entries (SDK, start-to-end + jal, empty clears, the stack and j/jal, refusals)
python tools/xbox/test_analyze.py       # the analyser against every decomp + ROM pair present (skips without ROMs: CI); N64RX_ROMS=<folder>
```

CI (`.github/workflows/build.yml`) is manual-only, for the maintainer's free
Actions minutes: `gh workflow run build.yml --ref dev` when a milestone needs
the three-OS check, `-f image=true` to rebuild and push the SDK image.

A test build of a game: `N64RX_CFLAGS=-DXHW_AUTOPAD=1 n64rx build <rom> --out out-test`
(the flags reach both triples). Test builds log `[OS] stall:` (what every N64
thread waits on, once, after 60 retraces without a graphics task) and a `[VI]`
line every 30 s.

The C tests build with `tools/xbox/hostcc.py`: `$CC`, else `cc`, `clang` or
`gcc`, else MSYS2's clang. `test_splash.py` saves what it checked as
`build-host/splash_*.png`.

The end-to-end check (CI's last step):

```sh
python tools/xbox/make_test_rom.py build-host/test/test.z64
python tools/n64rx/n64rx.py build build-host/test/test.z64       # -> out/N64Recomp-X test/
```

On the console or in xemu it must end on **"Test ROM: passed"** with 34
checks and none failed. `--gpr 64` (hidden) builds it with 64-bit registers.

## ROM-only builds (`docs/analyzer.md`)

```sh
n64rx build <rom>                     # no pack: from the ROM alone
n64rx build <rom> --analyze           # even with a pack (Pilotwings 64: the check against its decomp)
cat build-xbox/<id>/analysis.txt      # segments, functions, the libultra names and their rules
```

The check against a decomp is `tools/xbox/decomp_check.py` (the V track,
PLAN.md §6; first file of V): functions, segments and libultra names
against the decomp's lists, then the XBE in xemu.

```sh
python tools/xbox/decomp_check.py <rom> games/pw64/pw64.us.syms.toml        # a symbol file
python tools/xbox/decomp_check.py <rom> <decomps>/papermario/ver/us        # a splat version folder
python tools/xbox/decomp_check.py <rom> <decomps>/sm64 -v                  # a matching decomp's link order
N64RX_ROMS=<roms> N64RX_DECOMPS=<decomps> python tools/xbox/test_analyze.py [pw64 pm sm64 mk64 drmario64]
```

It reports functions missed / extra / mis-sized, the decomp's code
segments found or not, the libultra names the runtime replaces right /
wrong / missing, and the live set against the one the decomp's names give.
A matching decomp with no addresses (sm64, mk64) is read through the host
clang's preprocessor (generated headers stubbed under
`build-host/decomp_check/`) and lined up by order; `-v` lists the runs where
the counts differ, which is where the analysis split or missed functions.
`test_analyze.py` runs every pair present (ROMs in `N64RX_ROMS`, default
`build-host/roms`; decomps in `N64RX_DECOMPS`, default
`build-host/decomps`) and fails on any wrong libultra name, on code the
decomp's names keep live that the analysis leaves dead, or on a result
below the pair's floor; with neither it skips (CI). Nothing it reads or
writes is committed. A crash card that says
"No function at this address" names a function the analysis missed; the
card's second number is the host return address (`tools/xbox/sym.py --map
<folder>/debug/n64recomp_x.map 0x...` names the recompiled caller).
`[INTERP]` lines name the hand-written routines that run interpreted.

## The sweep: every ROM in a folder (`tools/xbox/sweep.py`)

```sh
export N64RX_RECOMP=<checkout>/build-host/recomp            # one N64Recomp for every worktree
python tools/xbox/sweep.py "<ROM folder>" --list            # which file stands for each game
python tools/xbox/sweep.py "<ROM folder>" --tag s2 --jobs 5 # n64rx build --fast over all of them
python tools/xbox/sweep.py "<ROM folder>" --tag s2 --only "NKTE|Mario"   # some again (code or file name)
. /c/xemu/nrx-env<N>.sh                                     # your xemu slot
python tools/xbox/sweep.py "<ROM folder>" --tag s2 --run 45 # boot each XBE that built
```

One ROM per game (the game code without its region letter): `[!]` first,
then USA, Europe, Japan; `[b]`, `[h]`, `[t]`, `[o]`, `[f]`, betas, hacks
and kiosks are skipped. Each build gets a timeout (`--timeout`, 1500 s).
`build-host/sweep/<tag>/` (never tracked) gets `results.json`, `table.md`
(stage counts, categories, one row per game with the first error line),
`logs/<code>.txt` (n64rx's output; the tool's own log is still
`build-xbox/<id>/n64rx.log`), `out/<code>/` (the folders), `run/<code>.txt`
(the serial log's interesting lines and tail). A second run resumes:
games already in `results.json` are skipped unless `--redo`; `--rescan`
reads the logs again (new categories without rebuilding). Stages:
analyse, recompile, compile, link, xbe; `--run` adds what the boot reached
(crash with its address, fatal card, frames presented, `[LEARN]` lines,
`[ULTRA] missing` names, the graphics microcode) and stops a boot early at
a crash, a card or 30 s of beats. xemu is not hardware: a run says where
a game stops in xemu, nothing about timing.

Run a sweep from a worktree pinned to a commit, never from the checkout
being worked in (every build compiles the runtime from the tree it runs
in; s6 caught half-made edits and lost 30 games):

```sh
git worktree add --detach ../N64Recomp-X-sweep <commit>
cd ../N64Recomp-X-sweep && git submodule update --init tools/recomp   # n64rx reads its symbol lists
export N64RX_BUILD_ROOT=$PWD/build-sweep N64RX_RECOMP=<checkout>/build-host/recomp
. /c/xemu/nrx-env<N>.sh   # a slot no other run uses
python -u tools/xbox/sweep.py "<ROM folder>" --jobs 5 --run 30 --tag s7
```

A one-off test build beside a sweep or a learning loop takes its own
`N64RX_BUILD_ROOT` (`build-rg`, `build-sweep`), or the two compile over each
other in `build-xbox/`.

Pokemon Snap's scenario (`tools/xbox/scenarios/snap_title`): the intro, the
title, START and A through the menus, shots along the way (build with
`N64RX_CFLAGS=-DXHW_AUTOPAD=1`).

## The learning loop (`docs/analyzer.md`)

```sh
N64RX_WITHHOLD=8022E920 n64rx build pw64.z64 --analyze --fast --out out-wh   # PW64's app thread entry withheld
# run it: the serial log has "[LEARN] safety net: 8022e920 ..." and "[LEARN] func ..." lines
grep -a '\[LEARN\]' serial.log > out-wh/learn.txt                               # or n64rx log --pull <ip>
N64RX_WITHHOLD=8022E920 n64rx build pw64.z64 --fast --out out-learnt --learn out-wh/learn.txt
# run it: no [LEARN] lines
```

## xemu (`tools/xbox/xemu_run.sh`)

```sh
MX_RUN=/c/xemu/nrx-run9 MX_XEMU=/c/xemu/xemu/xemu.exe \
MX_XISO=/c/xdev/nxdk/tools/extract-xiso/build/extract-xiso.exe \
MX_XEMU_ARGS="-config_path C:/xemu/slots/9/xemu.toml" \
MX_XBE="out/N64Recomp-X test/default.xbe" tools/xbox/xemu_run.sh 40 'card:'
```

| variable | meaning |
|---|---|
| `MX_XBE` | the XBE to stage (default `build-xbox/xbe/default.xbe`) |
| `MX_ROM` | a ROM to pack next to it (yours; never committed). Not needed for an n64rx build: its ROM is inside `default.xbe` (`$ROM`, PLAN.md 3.5); only a `--rom-beside` build or the platform XBE wants one. In a test build (`-DXHW_TEST_BUILD`) an `MX_ROM` env set by autopad wins over `$ROM` |
| `MX_RUN` | work folder: `stage/`, `game.xiso`, `serial.log` |
| `MX_STAGE_EXTRA` | a folder whose files are packed too (`autopad.txt`) |
| `MX_XISO` | a native extract-xiso instead of the image's |
| `MX_XEMU`, `MX_XEMU_ARGS` | the binary and its arguments (`-config_path` for a slot) |
| `MX_SHOTS` | stop after this many complete `[FBDUMP]`s |

The run's output is COM1 (`serial.log`); `tools/xbox/fbdump_to_png.py
serial.log out` turns `[FBDUMP]` lines into PNGs. xemu is not hardware. A
screenshot over COM1 stalls the game for seconds (a 720p one ~10 s):
scripted presses keep the ACPI clock's time meanwhile, so scenarios space
their shots. Stray processes left by an interrupted run (a `grep` on a
pipe) can slow xemu to a crawl: check the task list when the frame rate
drops for no reason.

## Scripted input (test builds: `-DXHW_AUTOPAD=1`)

`D:\autopad.txt`, one event a line: `<frame> <buttons> [for <frames>]`, or
`t<ms> <buttons> [for <ms>]` counted from the moment the controllers are up
(the loading screen presents no frames). Buttons: `A B X Y START BACK WHITE
BLACK UP DOWN LEFT RIGHT`, triggers `L R`, sticks `SL SR SU SD CL CR CU CD`,
`SHOT` for an `[FBDUMP]`. A dump over COM1 stalls the game for seconds in xemu: SHOTs closer than that all land on one frame, and presses during it are missed; events only reach a game while it reads its pads. M0's scenarios:

| script | expect |
|---|---|
| `t300 BACK for 1500` | `[SETTINGS] BACK held at boot: 480i 4:3 (saved)` |
| `t300 BACK START for 1500` | `[SETTINGS] reset at boot (BACK + START): defaults` |
| `t800 L R BACK BLACK for 300` | `[PAD] port 1: L + R + BACK + BLACK`; next boot: `previous exit: quit to the dashboard: ...` |
| `t6000 L R BACK BLACK for 300` | the same from the "No game built in" card |
| `t800 L R BACK WHITE for 300` | `[PAD] port 1: L + R + BACK + WHITE`, a restart; next boot: `previous exit: restart: ...` |

Video modes in xemu: `tools/xbox/eeprom_video.py <copy of eeprom.bin> 480p,720p,widescreen`
and a slot config pointing at the copy (`MX_XEMU_ARGS="-config_path .../xemu-hd.toml"`);
the HDTV AV pack is xemu's default. M2's scenarios (game builds):
`combo_flight` (restart while the game reads the pads), `combo_menu` (the
dashboard from inside the menu), `video_auto` (the menu's Video output to
Auto, Restart), `hd` (A keeps 720p, shots with Widescreen Auto and after a
change). `tools/xbox/scenarios/menu/autopad.txt`: `[MENU] opened`,
`[MENU] Widescreen: ...`, `[MENU] closed (settings.ini saved)`, four shots.
A COM1 `[FBDUMP]` takes seconds in xemu, so scenarios space their shots.
Timed events follow the ACPI clock. A `SHOT` due before the game's first
frame stalls the boot: put shots late. Saves: `pak_menu` (Mario Kart 64:
START at the title; expect `[SI] port 1: the game's own code reaches its
pak`, `[SAVE] ...mpk`), `pak_boot` (START held at boot: the game's own
Controller Pak screen, its pages free), `pak_page` (the BACK menu's Paks
page: `[MENU] Paks page, port 1`, `[SAVE] port 1: Pak B, port 2: Pak A`
and back, `[MENU] Pak A formatted: no notes`, `[SAVE] ...mpk written`;
then `pak_boot` shows the game's own pak screen agreeing).

## Switches

| switch | default | effect |
|---|---|---|
| `-DXHW_NO_SPLASH` | off | no loading screen: straight to the mode switch (the controllers still get 1.5 s) |
| `-DXHW_SPLASH_MS=<n>` | 2000 | the loading screen's minimum hold |
| `-DXHW_SPLASH_STILL=1` | 0 | background frozen, wordmark settled, no highlight: comparable shots |
| `-DXHW_SPLASH_DUMP` | off | `[FBDUMP]` of the loading screen as it ends, and of a card |
| `-DXRC_COMBOS=0` | 1 | no exit combos (diagnosis builds only) |
| `-DXRC_COMBO_WATCHDOG=0` | 1 | no last-resort combo look from the watchdog |
| `-DXRC_GPR32=1` | per pack | 32-bit registers (n64rx sets it from the scan or the pack) |
| `-DXRC_MEM_MASK=1` | 0 | every memory access masked to the window (KSEG1-heavy games) |
| `-DXRC_FUNC_PLAIN=1` | 0 | `RECOMP_FUNC` without weak + noinline (M3 measures it) |
| `-DXRC_CHECKS=1` | 0 | `NAN_CHECK` and `CHECK_FR` as logged fatals |
| `-DXRC_VIDEO_DEFAULT=<n>` | 0 (auto) | the default output (`n64rx build --video`) |
| `-DXHW_AUTOPAD=1` | 0 | a test build with scripted input |
| `-DXRC_AUDIO=0` | 1 | audio tasks counted and dropped: silent |
| `-DXRC_AUDIO_DUMP=<s>` | 0 | test builds: 3 s of the output from second `<s>` as `[AUDDUMP]` lines (`tools/xbox/auddump_to_wav.py serial.log out.wav`: the WAV and its numbers, RMS, L/R correlation, the energy below 4 kHz; the WAV stays outside the tree) |
| `-DXRC_MENU=0` | 1 | no BACK menu (settings.ini still works) |
| `-DXRC_MENU_PAKS=0` | 1 | no Controller Pak row and no Paks page in the BACK menu (`[portN] pak`, `pak_file` in settings.ini still apply) |
| `-DXRC_ROM_RESIDENT_MB=<n>` | 16 | ROMs up to n MB are read whole into RAM; bigger ones go through the page cache (`xhw_romcache.c`). `0`: the page cache for every ROM (a test of it on Pilotwings 64); `64`: every ROM resident (no cache; a 32 MB+ ROM then does not fit) |
| `-DXRC_ROM_CACHE_MB=<n>` | 16 | the page cache's cap (chunks held, pins included) |
| `-DXRC_ROM_CACHE_720P_MB=<n>` | 12 | the same when the boot predicts 720p |
| `-DXHW_ROM_IN_XBE=0` | 1 | ignore `default.xbe`'s `$ROM` section: the ROM is looked for next to the XBE (the build-side switch is `n64rx build --rom-beside`, which leaves the section out and writes the file) |
| `-DXRC_TLB_MISS=0` | 1 | an access to an unmapped KUSEG page is a crash, as before; otherwise the crash handler's fixup runs the game's own exception code (its vector at 0x80000000, or `__osException`) and libultra's fault path, then retries (`tlb.c`) |
| `-DXRC_RCP_DEFER=0` | 1 | a task's SP/DP done events go out inside `osSpTaskStartGo`, as before; otherwise when the starting thread next blocks, or at the next retrace |
| `-DXRC_RCP_OWNER=0` | 1 | a held done event goes out when any thread blocks (and at every retrace), as before; otherwise when the thread that started the task blocks, or at a retrace once that thread is blocked or two retraces on (Mario Party's nnsched: the graphics thread took the audio task's SP done and both waited for good) |
| `-DXRC_WATCH_QUEUE=<N64 address>` | unset | diagnosis: every message into that queue (the first 64, then every 600th) and every one taken out (the first 64), with the N64 thread |
| `-DXRC_WATCH_RECV=<N64 address>` | unset | diagnosis: the first 300 times a thread blocks on that queue: its N64 thread, sp and 64 stack words (the saved return addresses); the trace (`N64RX_TRACE=1`) the first two times |
| `-DXRC_DP_ON_FULLSYNC=0` | 1 | every graphics task raises DP done, as before; otherwise only one whose display list issued a FullSync, as the RDP does (Blast Corps' scheduler asserts on a DP done with no RDP task) |
| `-DXRC_ERET_RETURN=0` | 1 | an `eret` the interpreter meets outside the game's exception code stops the game, as before; otherwise it returns from the interpreted function (a game's own unnamed libultra dispatching a thread; the sweep's commonest stop) |
| `-DXRC_TLBWR_RANDOM=0` | 1 | `tlbwr` writes the entry at Wired, as before; otherwise Random counts down from 31 to Wired, one step a write (`interp.c`) |
| `-DXRC_TLB=0` | 1 | the TLB's entries are kept (tlbp, tlbr, `osVirtualToPhysical` still see them) but nothing is aliased: a TLB-mapped access faults as before `tlb.c` |
| `-DXRC_KSEG1=0` | 1 | the window is not aliased at KSEG1 (`mem.c`): a recompiled access to uncached RDRAM (0xA0000000 + x) faults as before (the interpreter still goes through KSEG0) |
| `-DXRC_MMIO=0` | 1 | no RCP register model (`mmio.c`): no register pages, and the interpreter's KSEG1 accesses past RDRAM fault as before |
| `N64RX_MMIO_INTERP=0` (env, n64rx) | 1 | the analyser does not send functions with constant hardware addresses to the interpreter (`emit.hw_funcs`): they are recompiled, as before |
| `-DXRC_NO_MISSING_STUBS=1` | 0 | no weak bodies for the calls nothing provides (`gen/xrc_missing.c`, `more3.c`): such a game fails to link and the link names them |
| `-DXRC_MISSING_FATAL=1` | 0 | the first call to such a function is the fatal card (default: `[ULTRA] missing <name>` once, returns 0) |
| `-DXRC_SAFETY_NET=0` | 1 | a call to an address with no function is the fatal card again (no interpreting, no `[LEARN]`, no `learn.txt`) |
| `-DXRC_OVERLAYS=0` | 1 | sections that share vram are not switched on the PI DMA that loads them (ovl.c): the section entered last keeps the address |
| `-DXRC_HW_SHADOW=0` | 1 | no shadow pages for the RCP registers through KSEG1 (mem.c): a game's own access there faults with the crash card again |
| `-DXRC_HIGH_TLB=1` (n64rx sets it from a `kuseg` learn line at 0xC0000000 and up; `N64RX_HIGH_TLB=0/1` overrides) | 0 | every generated memory access at 0xC0000000 and up goes through the game's TLB to its KSEG0 alias (`recomp.h` `XRC_ADDR`, `tlb.c`): the host cannot alias KSEG2/3 (the Xbox kernel's system space). Paper Mario's effects at 0xE0200000 |
| `-DXRC_THREAD_HALT=0` | 1 | osStopThread of another thread is ignored and osDestroyThread leaves it running, as before; otherwise it stops at its next block (woken there if blocked) until it is started again (Mario Party 2's audio thread, destroyed and made again) |
| `-DXRC_PRESENT_ON_SWAP=0` | 1 | a frame presented at the end of every graphics task, as before; otherwise, once the game calls osViSwapBuffer, at the swap to its buffer or when a task starts drawing into another swapped buffer (`gfx.c`, `f3d_interp.c`; Mario Party draws a frame in several tasks) |
| `-DXRC_SWAP_TASKS=<n>` | 16 | tasks without a present on a swap before frames go back to the end of every task (a game whose swaps never name its buffers) |
| `-DXRC_IPL3_LEFT=0` | 1 | low RDRAM left zero at boot; otherwise the part of the ROM's IPL3 that copies itself to RDRAM 0 is put there, as the console leaves it (`boot.c`; Banjo-Kazooie's anti-tamper reads 0xA00001D8) |
| `-DXRC_THREAD_RESUME=0` | 1 | osStopThread(self) parks for good, osCreateThread over a stopped thread does not restart it, osStartThread never waits for a higher priority on the same kernel level (thread.c) |
| `N64RX_OVERLAYS=0` (env, n64rx) | unset | the analyser's old segment rule: no segment whose vram overlaps a known one, no load descriptors without a caller (segments.py) |
| `N64RX_WITHHOLD=<vram>,...` (env, n64rx) | unset | test: those functions left out of the analysis, so the safety net runs them (the learning loop's demo) |
| `-DXRC_SAVE_PAK=0` | 1 | no pak in any port: the PIF's status says none, pak commands get no answer, osPfs* return `PFS_ERR_NOPACK` (the old behaviour) |
| `-DXRC_SAVE_CART=0` | 1 | no SRAM or FlashRAM: domain 2 reads zeros and drops writes, osFlash* fail |
| `-DXRC_CONT_MAX_ADDR=<a>` | per build | libultra's `__osMaxControllers`, which the runtime's osContInit sets to 4 (n64rx finds it: `saves.py`); unset, a game's own pak code sees no pak |
| `-DASP_HLE_WORDSWAP=0` | 1 on the console | `asp_hle` on a native-order RDRAM (Wings64-X's layout; the host tests' first group) |
| `-DASP_HLE_NEAD=0` | 1 | the nead audio microcodes (Mario Kart 64, SM64 EU, Zelda) read as unknown: silent |
| `-DASP_HLE_NAUDIO=0` | 1 | `n_aspMain` (Paper Mario, Smash Bros.) reads as unknown: silent |
| `N64RX_YAZ0=0` (env, n64rx) | 1 | a ROM-only build of a Yaz0 game (the Zeldas) uses the ROM as it is instead of the inflated image |
| `-DXRC_THREAD_TRACE=1` | 0 | diagnosis: the first 400 osCreateThread / osStartThread / osStopThread / osDestroyThread calls logged with the calling thread |
| `-DXHW_THREAD_NOBOOST=1` | 0 | the runtime's threads at absolute levels run without the kernel's wait boost (experiment; not on a console) |
| `-DXGFX_TEX_FULLHASH_MAX=64` | 2048 | the texture cache's change check samples 16 words of every texture over 64 bytes, as before; otherwise it hashes every word of one up to 2 KB (Blast Corps writes its font's glyphs into reused slots: the samples missed the change) |
| `-DXGFX_TMEM_WITHIN=0` | 1 | a tile whose TMEM address no load started at is a texture miss, as before; otherwise the latest LOADBLOCK that covers it, from the tile's offset |
| `-DXHW_LOG_BOOST=0` | 1 | the log lock's holder keeps its own priority; otherwise it runs at 15 while holding it (an idle-priority N64 thread logging was preempted for good and stopped every thread that logs, the VI thread among them: Donkey Kong 64's retraces froze at 300) |
| `N64RX_XBRANCH_INTERP=0` (env, n64rx) | 1 | a function with a branch or j into another function's body is recompiled anyway (the compile then fails: "use of undeclared label") instead of interpreted |
| `-DXRC_THREAD_SP16=0` | 1 | threads start at the sp the game gives instead of 16 bytes below it, as libultra does |
| `-DXRC_WATCH=<N64 address>` | unset | diagnosis: every write to that word logged with the writer's EIP and the new value (`[WATCH]`, x86 DR0; `xhw_crash.c`). The full KSEG0 address (`0x800F1A10u`), not an offset; armed on every thread started after it (the debug registers are per thread). A "no function at this address" fatal also logs the N64 thread's registers and stack top (`[CRASH] N64 thread`) |
| `-DXRC_TLB_HELPERS=0` | 1 | the runtime's RDRAM helpers mask a KUSEG address to physical instead of translating it through the game's TLB |
| `N64RX_TRACE=1` (env, n64rx) | unset | diagnosis: N64Recomp's `trace_mode`; the last 64 functions entered (recompiled by name, interpreted by address) are logged with a crash (`[TRACE]`, `xbox/src/rt/trace.c`) |
| `N64RX_GUARD=0` (env, n64rx) | 1 | functions recompiled from captured code run without checking that RDRAM still holds the bytes they were recompiled from (`guard.c`) |
| `N64RX_STRANDS=0` (env, n64rx) | 1 | the game's own setjmp/longjmp (found by shape) are recompiled as they are instead of run as coroutines on host threads (`strand.c`); Mario Party's processes then corrupt the main loop's registers |
| `N64RX_CAPTURED_INTERP=1` or `=<lo>-<hi>` (env, n64rx) | 0 | diagnosis: every function in captured code (or in that vram range) is interpreted; Banjo-Kazooie past its crash with its overlay region interpreted |
| `N64RX_RECOMP_PRECHECK=0` (env, n64rx) | 1 | what N64Recomp would crash on (a jump table outside the function's segment: it reads the ROM past its end) or refuse one run at a time (a jal to no function, a cop0 register other than Status) is left to it instead of interpreted up front |
| `-DXRC_LEARN_CODE=0` | 1 | the safety net does not save the RDRAM block of code no ROM DMA put there (`learn_code.bin`, `[LEARNCODE]`) |
| `-DXRC_STATUS_INIT=<value>` | 0x2000FF01 | the COP0 Status a thread starts with (libultra's: COP1 usable, every interrupt line unmasked, IE) |
| `-DXGFX_HOLD_EVICT=0` | 1 | the BACK menu never drops the game's texture cache when its held frame or card finds the texture pool full (Mario Kart 64 on red: the card was missing) |
| `-DXGFX_UCODE_F3DEX=0` | 1 | F3DEX 0.9x/1.x microcodes (and F3DLX, F3DLP, L3DEX, .NoN/.Rej) read as Fast3D, the build before the registry (`docs/renderer.md`) |
| `-DXGFX_UCODE_F3DEX2=0` | 1 | F3DEX2 microcodes (F3DZEX, F3DLX2, .NoN/.Rej) read as Fast3D |
| `-DXGFX_UCODE_S2DEX=0` | 1 | S2DEX 1.x/2.x read as Fast3D (its own commands are skipped either way; not drawn yet) |
| `-DXGFX_FB_READBACK=1` | 0 | framebuffer readback (docs/renderer.md "Framebuffer effects"): each frame copied N64-size to a cached stage; frames whose pages the game touched (x86 accessed bits) converted into RDRAM every frame for 2 s. 2: every frame, no watch. Off by default: Pilotwings 64 went hot every ~2 s and ran at ~14 fps in xemu |
| `-DXGFX_FB_DUMP=<n>` | 0 | debug: `[FBDUMP]` of the RDRAM frame after the n-th readback |

Wings64-X's platform switches (`XHW_*`, `XGFX_*`) carry over; its
`docs/testing.md` lists them.

## The renderer on the host (`docs/renderer.md`)

`python tools/xbox/test_f3dex.py`: the real interpreter (`f3d_interp.c`,
`texconv.c`, `ucode.c`) over a fake RDRAM and a back end that records
`xgfx_draw`'s vertices. Display lists built in the test in each GBI
(Fast3D, F3DEX, F3D_OLD's QUAD and CULLDL, F3DEX2 with its MTX, MOVEMEM,
MOVEWORD and GEOMETRYMODE layouts, the 32-entry buffer, CULLDL, BRANCH_Z,
POPMTX, a segmented G_VTX, G_LOAD_UCODE), CI4/CI8 through a TLUT, and the
microcode version texts; no ROM. Scenarios for the F3DEX games:
`tools/xbox/scenarios/mk64_race` (Mario Kart 64: the menus to a Luigi
Raceway race, A held, shots), `mk64_hd` (the same at 720p 16:9: hor+;
needs the 720p/widescreen slot config above) and `pm_title` (Paper Mario's title, once
its ROM-only build gets that far); both use frame events, since the shots
stall the game for seconds.
