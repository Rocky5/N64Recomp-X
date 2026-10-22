# Testing

## Host tests (no ROM; CI runs them on Linux, macOS and Windows)

```sh
python tools/xbox/rom_leak_check.py     # the tree holds nothing from a ROM
python tools/xbox/test_pad_combo.py     # the exit-combo detector (xhw_combo.c's core)
python tools/xbox/test_splash.py        # the loading screen's layout, palette, STILL, the holds' lines, titles on two lines (needs nxdk's font)
python tools/xbox/make_fonts.py         # (not a test) Sora -> xbox/src/hw/xhw_fonts.inc, after a size or weight change
python tools/xbox/test_settings.py      # settings.ini parse and format
python tools/xbox/test_n64rx.py         # the tool: ROMs, packs, messages, cache, output folder, doctor, CLI
python tools/xbox/test_compat.py        # docs/compat.md's tiers against n64rx's table
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
python tools/xbox/test_jpeg.py          # the JPEG decoder task: flat macroblocks give the microcode's colours
python tools/xbox/test_thread.py        # the thread layer on pthreads: the run token's order (libultra's), one thread in game code, a host wait keeping the turn, interrupts off; a stop with interrupts off, osSetThreadPri in a critical section, a polling higher thread on its starter's level
python tools/xbox/test_dryboot.py       # the dry boot's CPU: make_test_rom.py's self-checking program (calls, a jump table, mult/div, unaligned memory, floats and rounding modes) passes in it; its learn files in the console's formats
python tools/xbox/test_leak_check.py    # rom_leak_check against made-up offenders: a fake header block zipped, gzipped, shifted, as hex and base64; generated code under tests/; saves and learn files; an unlisted image; a raster SVG; a large binary
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

## The app and its toolchain (no ROM)

```sh
cd app && npm install
npm test                 # node --test: toolchain download/verify/unpack (resume, sha, zip/tar links refused),
                         # JSON lines, paths (portable, translocated, AppImage), jobs (one at a time, cancel),
                         # settings (atomic, the password only through safeStorage), validate, the stage's leak scan
npm run test:smoke       # Playwright on the real window with a fake n64rx (test/fake-n64rx.js): ROM -> badge ->
                         # build -> Done, the ISO path, a failed build, Send to Xbox, a second instance
npm start                # dev mode on this checkout (system Python, n64rx's own engine choice)
npm run dist             # stage (leak-scanned) + this OS's packages in app/dist/
python tools/xbox/pack_toolchain.py                 # the toolchain archive from this machine's setup.sh install
python tools/xbox/pack_toolchain.py --install-to DIR --selftest-after   # unpack, then build the test ROM with it alone
```

The smoke tests need the window to themselves: a running copy of the app
(`npm start`) holds the single-instance lock and fails the second-instance
test. A build with every path holding a space (the source copy, the builds,
the toolchain: the app's `N64Recomp-X Data`) was run by hand on Windows
(2026-10-18, `--fast` and ThinLTO): the test ROM to an XBE.

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

`--learn R` (with `--run`) runs the learning loop R rounds, as a user's
console would: each game whose boot learnt something (`[LEARN]` lines) and
presented no frames is built again with every boot log it has kept
(`learn/<code>-<n>.log`, passed as `--learn`) and booted again; the rows
keep each round's summary (`rounds`). `--learn-all` takes the games that
present frames too. On a sweep that ran without it, the first round boots
each game once more to have a log.

`tools/xbox/sweep_diff.py <old results.json> <new results.json> [--all]`
compares two sweeps: how many of the games both ran present frames in
each, and which ones changed (with their learning rounds).

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
`SHOT` for an `[FBDUMP]`. Each event logs when it takes hold (`[AUTOPAD] event <n> on at frame <f> (<ms> ms)`, the same clock as `[WATCH]`). A dump over COM1 stalls the game for seconds in xemu: SHOTs closer than that all land on one frame, and presses during it are missed; events only reach a game while it reads its pads. M0's scenarios:

| script | expect |
|---|---|
| `t300 L R for 1500` | `[SETTINGS] L + R held at boot: 480i 4:3 (saved)` |
| `t300 L R START for 1500` | `[SETTINGS] reset at boot (L + R + START): defaults` |
| `t300 BACK for 1500` | `[MENU] before the game (BACK held at boot)`, then the menu's rows; B: `[MENU] closed before the game` (`build-host/scen/preboot`: Texture filter changed, saved, the game starts; `build-host/preboot-shot.sh` with `-DXHW_SPLASH_DUMP` dumps the menu over the screen) |
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
| `-DXHW_SPLASH_DUMP` | off | `[FBDUMP]` of the loading screen as it ends, of a card, and of the menu before the game |
| `-DXHW_SPLASH_CLASSIC=1` | 0 | the first loading screen (unscii, aurora) instead of the deep-space title card |
| `-DXHW_SPLASH_MENU=0` | 1 | BACK held at boot does nothing (no menu before the game) |
| `N64RX_J_LATER=0` | 1 | the analyser decides a far `j` at once (a tail call) instead of after the walk |
| `N64RX_MESG_NODIV=0` | 1 | the message-queue rules want IDO's divide (no compare-wrapped osSendMesg/osRecvMesg) |
| `N64RX_DRYBOOT=1` (env, n64rx) | 0 | the dry boot (`tools/dryboot/dryboot.c`, `n64rx_lib/dryboot.py`, `docs/roadmap.md` B2): before a ROM-only build, the ROM's boot runs on this PC in a CPU-only N64 for `N64RX_DRYBOOT_FRAMES` frames (600: 10 s of game time, about a second here), and what it wrote (`<build>/dryboot/learn.txt` and `learn_code.bin`, the console's formats) goes in ahead of any `--learn`. Dr. Mario 64: 998 of its gzip main segment's 1,006 functions on the first build (0 before), `send_mesg` in the boot segment, the libultra names unchanged (decomp_check `--learn`) |
| `N64RX_NAME_UNIQUE=1` | 0 | (analyser) a libultra name the probe pass finds a second, different function fitting goes to the fit nearest the other libultra names; otherwise the first fit keeps it, as before. Either way `analysis.txt` lists each such name (`libultra: <name> fits ...`; review 4.1). In 27 library games: second copies of `__osDisableInt`/`__osRestoreInt` (Star Fox 64, Blast Corps), the EEPROM writers (Kirby 64, DK64, Majora's Mask), `osMapTLB` (Castlevania LoD), `osContStartReadData` (Yoshi's Story), `__osSetSR` and `osWritebackDCacheAll` (Excitebike 64) |
| `N64RX_NAME_BITS=0` | 1 | (analyser) `osViBlack` without its state bit (0x20: `osViRepeatLine`'s 0x40 fit too and came first in Star Fox 64), `osMapTLBRdb` without its 0xC0000000 entry (GoldenEye's and Conker's own fixed TLB writers fit), `osEepromProbe` with no call besides the SI access pair (Dr. Mario 64's `osContSetCh` fit), as before |
| `-DXRC_COMBOS=0` | 1 | no exit combos (diagnosis builds only) |
| `-DXRC_COMBO_WATCHDOG=0` | 1 | no last-resort combo look from the watchdog |
| `-DXRC_GPR32=1` | per pack | 32-bit registers (n64rx sets it from the scan or the pack) |
| `-DXRC_MEM_MASK=1` | 0 | every memory access masked to the window (KSEG1-heavy games) |
| `-DXRC_FUNC_PLAIN=1` | 0 | `RECOMP_FUNC` without weak + noinline (M3 measures it) |
| `-DXRC_CHECKS=1` | 0 | `NAN_CHECK` and `CHECK_FR` as logged fatals |
| `-DXRC_VIDEO_DEFAULT=<n>` | 0 (auto) | the default output (`n64rx build --video`) |
| `-DXHW_AUTOPAD=1` | 0 | a test build with scripted input |
| `-DXHW_PERF_DETAIL=1` | 0 | a `[PERF2]` line beside each `[PERF]`: GPU waits inside display lists, the interpreter's time (inclusive) and calls, texture hashing's time and bytes, the runtime lock's takes and the interrupts-off boost's level changes a frame (the M3 gate's counters, `docs/roadmap.md` B6). `[PERF]` itself now leaves a present made inside a list out of DL, the 5 s log write out of PRESENT and `[DLBENCH]`'s runs out of TEX, and counts waits at the frame's start apart (review 2.2); `tools/xbox/ab_report.py a.log b.log` compares boots by these lines |
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
| `-DXRC_TIMER_CATCHUP=0` | 1 | a periodic timer that fell more than one interval behind keeps its deadline in the past, as before: the timer thread's wait underflowed (~31 days) and every timer stopped; otherwise it is re-armed from now (as libultra re-arms it from the interrupt) and a past deadline waits 0 ms (`test_time.py`) |
| `N64RX_SP_CHECK=1` | 0 | diagnosis: every call in the generated code checks that $sp comes back as it went; `[SPCHECK]` names the call site, the callee (and a function pointer's target) the first 32 times (`pipeline.py write_sp_check`, `trace.c`) |
| `N64RX_INTERP_FUNCS=<vram>,...` | unset | diagnosis: those functions run in the interpreter (its test-build check logs one that returns with a callee-saved register or sp changed) |
| `N64RX_DIAG_HOOKS="<vram>=<C>\|\|..."` | unset | diagnosis: C put before the instruction at `<vram>` in the function that holds it (an N64Recomp hook), e.g. a register checked and logged |
| `-DXGFX_VTX_OUT_CACHE=0` | 1 | each triangle works out its vertices' screen position, depth and colour again, as before; otherwise once per vertex while the viewport and the output map stay (`f3d_interp.c` to_out; the same numbers) |
| `-DXGFX_VTX_WORDS=0` | 1 | a vertex is read halfword by halfword, as before; otherwise as four words of the image |
| `-DXRC_DLDUMP=<retrace>`, `-DXRC_DLDUMP_N=<n>` | 0, 6 | test builds: n graphics tasks after that retrace and the RDRAM they read over COM1, for `tools/xbox/bench_dl.py` (below) |
| `-DXRC_DLBENCH=<n>`, `-DXRC_DLBENCH_RUNS=<k>` | 0, 8 | every n retraces one task run k more times with and without the back end, timed: `[DLBENCH]` (below) |
| `-DXHW_SHOT_FREEZE=0` | 1 | the game's clock (osGetTime/osGetCount) runs on while a screenshot is written, as before: a 10 s `[FBDUMP]` looked like an RCP hang to Ocarina of Time (its 3 s timer) and it stopped after every xemu shot; otherwise it stands still for the dump |
| `-DXRC_RCP_OWNER=0` | 1 | a held done event goes out when any thread blocks (and at every retrace), as before; otherwise when the thread that started the task blocks, or at a retrace once that thread is blocked or two retraces on (Mario Party's nnsched: the graphics thread took the audio task's SP done and both waited for good) |
| `-DXRC_RCP_ROOM=0` | 1 | a held done event is posted even into a full queue (and dropped there), as before; otherwise it waits, with those after it, until its queue has room (Ocarina of Time's intermittent title stall: the scheduler's queue full of retrace messages, its SP done dropped) |
| `-DXHW_AUTOPAD_GAME_CLOCK=0` | 1 | autopad's `t<ms>` times run on the wall clock, as before; otherwise on the game's clock, which stops while a SHOT dumps (10-15 s each over serial): a 15-shot Pokémon Snap script had run only ~50 game frames between its shots |
| `-DXRC_MAX_THREADS=32` | 64 | N64 thread records (each started one a host thread with a committed 128 KB stack), as before 32; Pokémon Snap's object threads on the Beach course passed 32 at once ("Too many N64 threads"). Only a game that makes them pays the memory |
| `-DXRC_WATCH_QUEUE=<N64 address>` | unset | diagnosis: every message into that queue (the first 64, then every 600th) and every one taken out (the first 64), with the N64 thread |
| `-DXRC_WATCH_QUEUE_ALL=1` | 0 | with `XRC_WATCH_QUEUE`: every message into and out of that queue (the first 20000 each way) |
| `-DXRC_WATCH_RECV=<N64 address>` | unset | diagnosis: the first 300 times a thread blocks on that queue: its N64 thread, sp and 64 stack words (the saved return addresses); the trace (`N64RX_TRACE=1`) the first two times |
| `-DXRC_DP_ON_FULLSYNC=0` | 1 | every graphics task raises DP done, as before; otherwise only one whose display list issued a FullSync, as the RDP does (Blast Corps' scheduler asserts on a DP done with no RDP task) |
| `-DXRC_ERET_RETURN=0` | 1 | an `eret` the interpreter meets outside the game's exception code stops the game, as before; otherwise it returns from the interpreted function (a game's own unnamed libultra dispatching a thread; the sweep's commonest stop) |
| `-DXRC_TLBWR_RANDOM=0` | 1 | `tlbwr` writes the entry at Wired, as before; otherwise Random counts down from 31 to Wired, one step a write (`interp.c`) |
| `-DXRC_TLB=0` | 1 | the TLB's entries are kept (tlbp, tlbr, `osVirtualToPhysical` still see them) but nothing is aliased: a TLB-mapped access faults as before `tlb.c` |
| `-DXRC_KSEG1=0` | 1 | the window is not aliased at KSEG1 (`mem.c`): a recompiled access to uncached RDRAM (0xA0000000 + x) faults as before (the interpreter still goes through KSEG0) |
| `-DXRC_KSEG1_HEAL=0` | 1 | a fault on a KSEG1 alias page is a crash, as before; otherwise a page whose PTE has gone is aliased again and the access retried (`[MEM] KSEG1 page ... lost`), and one the kernel now holds is logged with its PDE/PTE (`... taken`). Majora's Mask on the console (`vmm9`): the whole 8 MB alias was made at boot, then its first uncached write faulted on a page of it |
| `-DXHW_ALIAS_OWN_TABLES=0` | 1 | a page table already in the page directory past the physical map (the KSEG1 alias, the RCP pages) is used as it is, and the tables made stay when the XBE is left, as before (red: an earlier XBE's table on a reused page, `docs/decisions.md`) |
| `-DXHW_ALIAS_ADOPT_COPY=1` | 0 | an adopted table's entries are copied into ours and the old entry handed back on leaving, as the first version did; otherwise it starts empty (`[MEM] alias: a page table found ...`) and is never handed on |
| `-DXRC_KSEG1_HEAL_MAX=<n>` | 64 | heals of one page in a row before the access faults to the crash card |
| `-DXRC_STRAND_GC=0` | 1 | a parked strand no saved context still points into is never ended (Mario Party's process contexts leaked until the board froze) |
| `-DXRC_NULL_PAGE=0` | 1 | the N64's page 0 through KUSEG faults, as before; otherwise it reads as zeros (Star Fox 64's audio heap reads an uninitialised `entry`) |
| `-DXRC_START_DEFER=1` / `=2` | 0 | a thread started by a higher N64 priority waits until its starter sleeps (or lowers its priority, or 200 ms): 1 only where the two share a kernel level, 2 whatever the levels (Star Fox 64's boot race; the race one level apart under `XRC_RANK_LEVELS`) |
| `-DXRC_RANK_LEVELS=1` | 0 | each N64 priority in use gets its own kernel level (up to 12), not the fixed map's shared ones (Star Fox 64's scheduler lag; with `XRC_START_DEFER=2`) |
| `-DXRC_RUN_TOKEN=1` | 0 | the run token (`docs/roadmap.md` B1): one game thread runs game code at a time, the highest-priority runnable one, FIFO among equals; it hands the token on when it blocks on an N64 object (a queue, a stop, a fault, the idle loop), never on a host wait; a thread readied that outranks the holder takes it (at the holder's next runtime call, at once where the levels preempt), and with interrupts off the switch waits for them; `XRC_START_DEFER` and `XRC_SEND_YIELD` do nothing then; `[OS] stall:` reports the holder and the hand-offs (host test `test_thread.py`) |
| `-DXRC_STOP_UNLOCK=0` | 1 | a thread that stops or destroys itself with interrupts off keeps the runtime's lock while it waits, and starts over with them still off, as before (the VI thread and every queue waited for good: review 5b.5) |
| `-DXRC_PRI_LOCKED=0` | 1 | `osSetThreadPri` edits the records unlocked and moves a thread holding interrupts off off level 14 at once, as before (Kirby 64's deadlock for a game that re-prioritises inside a critical section: review 5b.7) |
| `-DXRC_VI_RESERVE_SMALL=0` | 1 | a retrace queue of `XRC_VI_RESERVE` slots or fewer gets no `osViSetEvent` message, as before (a 1- or 2-slot queue waited on each frame hung: review 5b.1) |
| `-DXRC_RCP_ORDER=0` | 1 | two tasks' held done events flushed together come out newest first, as before (the scheduler paired the audio done with the graphics task: review 5b.3) |
| `-DXRC_PI_BOUNDS=0` | 1 | a PI DMA's length is not cut to 16 MB, a destination past RDRAM's end wraps onto physical 0, a DMA across the ROM's end zero-fills all of it, and the guard is told `dram \| 0x80000000` (the wrong range for TLB-mapped code), as before (review 5b.2, 5b.23) |
| `-DXRC_LEARN_TEXT_KB=<n>` | 160 | `learn.txt`'s size in KB (48 before: Ocarina of Time's learning stopped at the cap, unlogged); twice this in BSS; a larger file is kept as it is and nothing is added (`[LEARN] learn.txt full`) |
| `-DXRC_STUB_REENTER=0` | 1 | a safety-net stub an overlay switch took off the function table stays off it, as before (every later call went through the slow lookup: review 5a.4); the table has room for every net address (`XRC_MAX_LEARN`), not 512 |
| `-DXRC_SAVE_COPY=0` | 1 | the save writer writes under the save lock and a failed write is not tried again, as before; otherwise each dirty image is copied under the lock and the copy written outside it, a failure marks it dirty again and retries after 1, 2, 4 ... 30 s (the exit combos' flush at once) (review 5b.9, 5d.3) |
| `-DXHW_FIXUP_STORM=<n>` | 256 | the same fault (thread, instruction, address) "fixed" this many times within half a second is a crash with its card; 0: retried for ever, as before (a store to the read-only cartridge map spun at 100 % CPU: review 5a.2) |
| `-DXGFX_DL_BUDGET=<n>` / `-DXGFX_DL_BOUNDS=0` | 1000000 / 1 | a display-list task ends after n commands (a list that calls itself), and when its commands run off RDRAM's end, each logged once; 0: no budget / no end check (review 5c.3) |
| `-DXRC_FODD_ARRAY=0` | 1 | `recomp_context.f_odd` is a pointer set to `&f0.u32h`, as upstream; otherwise an array over the FPR block (the same words, no pointer loaded from the context: what `N64RX_RESTRICT` needs) |
| `N64RX_RESTRICT=1` (env, n64rx) | 0 | every generated function's `rdram` and `ctx` are `restrict`, with a compiler barrier before each loop's backward `goto` (a spin on a word another thread sets still ends); Pilotwings 64 `--fast`: 7.6 % fewer instructions, 12.3 % fewer with a memory operand (static count); on by default only after the M3 gate measures it on a console (`docs/roadmap.md` B6) |
| `XBOX_CMAKE_ARGS=-DXBOX_PBKIT_O2=OFF` | ON | pbkit built with no `-O`, as before; otherwise `-O2 -march=pentium3`, as the platform layer (review section 2) |
| `-DXRC_DIV_VR4300=0` | 1 | DIV / DIVU in generated code divide as N64Recomp emitted them (a zero divisor traps, #DE); otherwise the VR4300's results (`patches/recomp/0003`) |
| `-DXRC_RECAPTURE_SPAN=0` | 1 | a guard miss recaptures the page holding the function's start only, as before; otherwise every page the function covers, as one group record (`learn.c`) |
| `N64RX_LEARN_GROUPS=0` | 1 | (n64rx) versions of captured code built page by page only; otherwise a group record's pages are one version, built first (`learn.py`) |
| `N64RX_ANALYSIS_CACHE=0` | 1 | (n64rx) the ROM is analysed on every build; otherwise the analysis is kept in the build folder (`analysis.cache`) for the next build of the same ROM by the same n64rx source with the same `N64RX_*` settings (Mario Party: ~6 minutes saved a build) |
| `-DXGFX_VTX_SSE=0` | 1 | the vertex transform in scalar sums, as before; otherwise four lanes at once in SSE, the same bits (bench_dl's pictures match for GoldenEye's Dam and Kirby 64's tutorial; the gain not measured on a console) |
| `-DXGFX_TLUT_HASH_CACHE=0` | 1 | a CI texture's palette hashed for the cache key at every bind, as before; otherwise again only after a LOADTLUT or for another palette (the same keys; on GoldenEye's Dam the binds' share of the host profile did not move, 11.1% -> 10.7%) |
| `-DXGFX_TEX_UNREAD=0` | 1 | a triangle's texture is looked up and bound whenever G_TEXTURE is on, as before; otherwise only when the combiner reads a texel (or in copy/fill mode). Mario Party's board intro drew shade-only quads with the texture on: 36 "texture miss" lines of an unset tile 0 |
| `-DXGFX_RARE_DLINMEM=0` | 1 | Fast3D's opcode 0x07 counted as unknown, as before; otherwise Rare's: n commands run from memory (Jet Force Gemini's texture rectangles) |
| `-DXGFX_DKR=0` | 1 | a microcode with no version text read as Fast3D throughout, as before; otherwise Rare's DKR geometry (slot matrices, 10-byte vertices, G_TRIN) once it sends 0x05 or 0x07 (Diddy Kong Racing) |
| `N64RX_PROBE=0` | 1 | (n64rx) functions only from calls, gaps and roots, as before; otherwise also from the jals aimed into a run no function covers, when enough land on a function's start (Virtual Pool 64, Starshot) |
| `N64RX_TLB_ODD=0` | 1 | (n64rx) a TLB entry code's even-page argument taken as it is, as before; otherwise with no even page (-1) the odd page maps the code (Shadow Man) |
| `N64RX_IPL3_7102=0` | 1 | (n64rx) the header's entrypoint for the 7102 boot chip, as before; otherwise 0x80000480, where its IPL3 loads (Lylat Wars) |
| `-DXRC_THREAD_GP=0` | 1 | a new thread and a strand start with gp 0, as before; otherwise with the creator's (Starshot's gp-relative stores) |
| `N64RX_FUNC_DMA=0` | 1 | (n64rx) a learnt function goes to the overlay of the newest DMA over its address, as before; otherwise to the DMA logged with it, or none (the captured bytes) |
| `N64RX_BOOT_PLAIN=0` | 1 | (n64rx) the boot segment keyed as an overlay when a learnt segment overlaps it, as before (Paper Mario's build then failed) |
| `-DXRC_HIGH_SAFETY=0` | 1 | a call to KSEG2/3 code with no function ends the game, as before; otherwise the safety net runs it through the TLB and learns it (Paper Mario's effects) |
| `N64RX_HIGH_SEGS=0` | 1 | (n64rx) a function learnt in KSEG2/3 gets no segment from the ROM's load tables, as before |
| `-DXHW_AUDIO_APU_IDLE_ALWAYS=1` | 0 | the boot stops the APU whatever state it is found in, as before; otherwise only an APU found running (its sample counter or a DSP FIFO moving) is stopped. Smash Bros. on the console froze the whole Xbox during the audio boot after a stuck boot, with the APU found idle; the boot now logs `[AUDIO] step:` lines to name the step |
| `-DXRC_MMIO=0` | 1 | no RCP register model (`mmio.c`): no register pages, and the interpreter's KSEG1 accesses past RDRAM fault as before |
| `N64RX_MMIO_INTERP=0` (env, n64rx) | 1 | the analyser does not send functions with constant hardware addresses to the interpreter (`emit.hw_funcs`): they are recompiled, as before |
| `-DXRC_NO_MISSING_STUBS=1` | 0 | no weak bodies for the calls nothing provides (`gen/xrc_missing.c`, `more3.c`): such a game fails to link and the link names them |
| `-DXRC_MISSING_FATAL=1` | 0 | the first call to such a function is the fatal card (default: `[ULTRA] missing <name>` once, returns 0) |
| `-DXRC_SAFETY_NET=0` | 1 | a call to an address with no function is the fatal card again (no interpreting, no `[LEARN]`, no `learn.txt`) |
| `-DXRC_OVERLAYS=0` | 1 | sections that share vram are not switched on the PI DMA that loads them (ovl.c): the section entered last keeps the address |
| `-DXRC_HW_SHADOW=0` | 1 | no shadow pages for the RCP registers through KSEG1 (mem.c): a game's own access there faults with the crash card again |
| `-DXRC_HIGH_TLB=1` (n64rx sets it from a `kuseg` learn line at 0xC0000000 and up; `N64RX_HIGH_TLB=0/1` overrides) | 0 | every generated memory access at 0xC0000000 and up goes through the game's TLB to its KSEG0 alias (`recomp.h` `XRC_ADDR`, `tlb.c`): the host cannot alias KSEG2/3 (the Xbox kernel's system space). Paper Mario's effects at 0xE0200000 |
| `-DXRC_THREAD_HALT=0` | 1 | osStopThread of another thread is ignored and osDestroyThread leaves it running, as before; otherwise it stops at its next block (woken there if blocked) until it is started again (Mario Party 2's audio thread, destroyed and made again) |
| `-DXRC_THREAD_REUSE=0` | 1 | every new OSThread gets a new record and host thread (32 at most); otherwise a destroyed (or returned) thread's record, parked once stopped, serves the next new OSThread (Pokémon Snap's object threads) |
| `-DXRC_VI_K0BASE=0` | 1 | osViGetCurrentFramebuffer/osViGetNextFramebuffer return 0 before the first swap, as before; otherwise K0BASE (0x80000000), as libultra's __osViInit leaves them |
| `-DXRC_LOOKUP_TLB=0` | 1 | a call through a pointer into a KUSEG page the TLB does not map is "no function", as before; otherwise the game's refill code and fault path page it in first, as the CPU's miss would (GoldenEye's evicted 0x7F000000 pages) |
| `-DXRC_FAULT_RUNQ=0` | 1 | a faulted thread let go by the game's fault thread stays on the game's `__osRunQueue`; otherwise it is taken off as libultra's dispatcher would (`XRC_OS_RUNQUEUE`, or 12 bytes below `__osFaultedThread`, checked by `__osThreadTail`'s -1) |
| `-DXRC_DISPATCH_START=0` | 1 | an eret outside the game's exception code only returns; otherwise, when it is the game's own `__osDispatchThread` restoring an OSThread the runtime does not run (k0 when EPC was written), that thread is started from its context (pc, sp, a0, priority), and a boot that returns after that parks instead of the "game returned" card |
| `-DXRC_VI_ON_EVENT=0` | 1 | the retrace thread starts only with the runtime's `osCreateViManager`; otherwise also when a game registers `OS_EVENT_VI` |
| `-DXGFX_DESWIZZLE32=0` | 1 | an RGBA32 LOADBLOCK with dxt 0 deswizzled with the 16-bit rule (32-bit halves), as before; otherwise its odd rows swap texel pairs (TMEM's two banks) |
| `-DXGFX_LOAD32=0` | 1 | a LOADBLOCK counts texels in SETTIMG's size always, as before; otherwise 4 bytes each when its tile is 32-bit under a 16-bit image (Mario Party's title logo) |
| `-DXGFX_FILL32=0` | 1 | a fill rectangle's colour is RGBA5551 always, as before; otherwise RGBA8888 when the colour image is 32-bit, as the RDP takes it |
| `-DXGFX_TRI4=0` | 1 | Fast3D's 0xB1 unhandled, as before; otherwise Rare's G_TRI4 (GoldenEye, Perfect Dark): four triangles of 4-bit indices, one of three 0s skipped |
| `-DXHW_VIDEO_720P_KB=<kb>` | 24576 | free memory 720p asks for before pbkit starts (32768: the plan's first rule, which turned Smash Bros. and Banjo-Kazooie down to 480) |
| `-DXGFX_HIRES=0` | 1 | the screen is 320x240 N64 pixels always, as before (a wider buffer's left part shown); otherwise a buffer the game swaps to sets it: its width, and 3/4 of that or the VI mode's rows (GoldenEye's 440x330 front end, 640x480 games; `[GFX] screen WxH` when it changes) |
| `-DXGFX_TRACE_FRAME=<n>` / `-DXGFX_TRACE_MS=<ms>` / `-DXGFX_TRACE_PRIM=0xRRGGBB` | 0 | test builds: frame n's draws, the first frame that long after the first one, or the frame after the first draw with that prim colour, as `[DRAW]` lines |
| `-DXGFX_TEXLOG=<n>` | unset | debug: the first n textures made, as `[TEX]` lines (format, size, line, palette and TLUT mode, the first two palette entries, block or tile, the source, the first texel) |
| `-DXGFX_TEXRECT_SNAP=0` | 1 | a 1/2-cycle texrect's lower-right edge as given (10.2), as before; otherwise it is rounded up to whole pixels, as the RDP draws every row a subscanline of it touches (((y + 1) << 2) - 1 fills row y): GoldenEye's line-by-line backgrounds without gaps when upscaled) |
| `-DXGFX_TILE_WRAP=0` | 1 | a tile's size from its 10.2 coordinates unwrapped, as before; otherwise they wrap in 12 bits as the RDP's (a scrolling tile with lrs below uls; its mask gives the size) |
| `-DXRC_MAX_LEARN=<n>` | 4096 | addresses the safety net remembers (512 before: Paperboy's vtable-only functions ran past it, and a lookup past the table was the "no function" card); a full table is logged once (`[LEARN] the net's table is full`) |
| `-DXRC_PRESENT_ON_SWAP=0` | 1 | a frame presented at the end of every graphics task, as before; otherwise, once the game calls osViSwapBuffer, at the swap to its buffer or when a task starts drawing into another swapped buffer (`gfx.c`, `f3d_interp.c`; Mario Party draws a frame in several tasks) |
| `-DXRC_SWAP_TASKS=<n>` | 16 | tasks without a present on a swap before frames go back to the end of every task (a game whose swaps never name its buffers) |
| `-DXRC_SWAP_STALE=<n>` | 6 | retraces an open frame waits (no task, no swap to its buffer) before the VI thread presents it as it is (a game that draws once and waits) |
| `-DXRC_IPL3_LEFT=0` | 1 | low RDRAM left zero at boot; otherwise the part of the ROM's IPL3 that copies itself to RDRAM 0 is put there, as the console leaves it (`boot.c`; Banjo-Kazooie's anti-tamper reads 0xA00001D8) |
| `-DXRC_SP_BY_TYPE=0` | 1 | every task that is not graphics goes to the audio HLE, as before; otherwise only `M_AUDTASK` does, and another type (a JPEG or HVQ decoder) is logged once and not run (Ocarina of Time's JPEG task came first and silenced its audio) |
| `-DXRC_JPEG=0` | 1 | an `M_NJPEGTASK` is not run; otherwise one whose data looks like Ocarina of Time's decoder task (mode 0 or 2, 1-16 macroblocks in RDRAM) is decoded in place (`jpeg.c`: its pre-rendered room backgrounds; host test `tools/xbox/test_jpeg.py`) |
| `-DXRC_TLB_KSEG2=0` | 1 | the runtime's RDRAM helpers (DMA destinations, message queues, task fields) translate KUSEG only; otherwise KSEG2/3 too, through the game's TLB (Paper Mario DMAs its effects to 0xE0200000) |
| `-DXRC_FETCH_TLB=0` | 1 | the interpreter fetches a KUSEG address at rdram + vaddr even where that lands in the Xbox kernel's space (vaddr ≥ 0x80000000 − rdram, about 0x7C5CF000); otherwise there it goes through the game's TLB, its refill and fault path (`xrc_tlb_resolve`), to the KSEG0 alias; the interpreter's loads and stores too, and those to KSEG2/3 |
| `-DXRC_LOG_SP_STARTS=<n>` | unset | diagnosis: the first n SP task starts, through `osSpTaskStartGo` (`[SP] start`: type, task, N64 thread, retrace) or the register model (`[MMIO] SP start`). Test builds also report once (up to three times) when audio tasks stop for 300 retraces after running (`[OS] stall: audio tasks stopped`) |
| `-DXRC_INTERP_TRACE_FROM=<vram>` | unset | diagnosis: from that pc on, each interpreted call and register jump with sp, ra and whether the target is recompiled (`[ITRACE]`, up to 20000) |
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
| `-DXGFX_TEX_RECHECK=0` | 1 | a block load over the full-hash size is checked by its 16 samples alone, as before; otherwise it is also hashed whole every 8th frame it is used (staggered by cache slot) and every frame once its address has been seen to change (Pokémon Snap types its dialogue into a 3952-byte RGBA16 strip: the samples kept old glyphs; hashing every big texture every frame cost its lab 2.5 ms a frame in xemu) |
| `-DXGFX_MASK0_CLAMP=0` | 1 | a texture axis wraps or mirrors by its clamp bit alone, as before; otherwise an axis whose mask is 0 clamps, as the RDP's (Ocarina of Time's moon: a 64x64 tile on a quad whose coordinates run to 128 drew four moons) |
| `-DXGFX_CLAMP_EXTENT=0` | 1 | a clamp bit clamps at the texture's edge, as before; otherwise an axis with a mask whose tile is wider than 2^mask texels repeats (or mirrors): the RDP clamps at the tile's extent, not the mask (Pokémon Snap's Beach cliffs, rails and track: 64-texel textures on 768-texel clamped tiles drew flat grey) |
| `-DXGFX_RGBA_LOWBIT=0` | 1 | RGBA at 4 or 8 bits (no such format) is a texture miss, as before; otherwise it goes through the palette when the TLUT is on (as CI) and reads as I4/I8 when it is off, as the RDP's. Banjo-Kazooie's START menu: all 48 logged misses were RGBA 4-bit |
| `-DXGFX_PROBE=0` | 1 | a small copy of the frame being drawn (a LOADTILE of the colour image, then a COPY texrect into an image at most 32 wide) draws on the screen, as before; otherwise a software grid of that rectangle follows the frame's triangles with their depths and the copy writes, per pixel, which copy it was last drawn before (Pokémon Snap's photo check counts the pixels each Pokémon changed) |
| `-DXGFX_TILE_LINEW=0` | 1 | a tile wider than its line (Mario Party's board: 0-64 tiles over 64-texel rows, for the filter's last column) is a texture miss, as before; otherwise it is cut to the line's texels and the edge clamped (the RDP reads that column from the next row) |
| `-DXGFX_DXT32=0` | 1 | an RGBA32 LOADBLOCK whose dxt does not divide its line is used as loaded, as before; otherwise each 8-byte texel pair goes where the RDP's line counter puts it in TMEM and is read back as the sampler does (the 16-bit rule carried over; no game seen needing it yet: Mario Party's logo was `XGFX_LOADTILE32`) |
| `-DXGFX_LOADTILE32=0` | 1 | a LOADTILE into a 32-bit tile counts its rows at the tile's line, as before (half the bytes: Mario Party's title logo, 188x5 RGBA32 strips, came out 2 rows tall); otherwise twice it, TMEM's two banks |
| `-DXGFX_TLUT_OFFSET=0` | 1 | LOADTLUT reads the palette from the image's start, as before; otherwise SL/TL place it in the image and SH - SL counts it (GoldenEye keeps each palette right after its texels: its CI textures took their own texels as colours: the purple sky layer, green blocks) |
| `-DXGFX_RDP_TRI=0` | 1 | raw RDP triangles sent as G_RDPHALF_1/G_RDPHALF_CONT (and a closing G_RDPHALF_2) pairs are ignored, as before; otherwise their edge, shade, texture and z coefficients are drawn as the walker's two trapezoids (GoldenEye's cloud layer) |
| `-DXGFX_S2DEX_BG=0` | 1 | S2DEX's G_BG_COPY and G_BG_1CYC are skipped, as before; otherwise the background image is drawn in 64-texel chunks through the texture cache (host test `s2dex_bg`) |
| `-DXGFX_S2DEX_OBJ=0` | 1 | S2DEX's object commands are skipped, as before; otherwise its texture loads, rectangles and sprites are drawn (Yoshi's Story; host test `s2dex_obj`) |
| `-DXGFX_TILE_ROWHASH=0` | 1 | a LOADTILE texture's change check hashes its first and last rows, as before; otherwise every row (16 spread rows past the full-hash size) |
| `-DXGFX_TMEM_WITHIN=0` | 1 | a tile whose TMEM address no load started at is a texture miss, as before; otherwise the latest LOADBLOCK that covers it, from the tile's offset |
| `-DXHW_LOG_BOOST=0` | 1 | the log lock's holder keeps its own priority; otherwise it runs at 15 while holding it (an idle-priority N64 thread logging was preempted for good and stopped every thread that logs, the VI thread among them: Donkey Kong 64's retraces froze at 300) |
| `N64RX_XBRANCH_INTERP=0` (env, n64rx) | 1 | a function with a branch or j into another function's body is recompiled anyway (the compile then fails: "use of undeclared label") instead of interpreted |
| `-DXRC_THREAD_SP16=0` | 1 | threads start at the sp the game gives instead of 16 bytes below it, as libultra does |
| `-DXRC_WATCH=<N64 address>` | unset | diagnosis: every write to that word logged with the writer's EIP and the new value (`[WATCH]`, x86 DR0; `xhw_crash.c`). The full KSEG0 address (`0x800F1A10u`), not an offset; armed on every thread started after it (the debug registers are per thread). A "no function at this address" fatal also logs the N64 thread's registers and stack top (`[CRASH] N64 thread`). Only changes are logged (with the time in ms), and `-DXHW_WATCH_IGNORE=<mask>` leaves out changes in those bits only (a flag word that toggles a bit every frame) |
| `-DXRC_AI_STATUS=0` | 1 | AI_STATUS always 0 (not busy/full from the virtual DAC) |
| `-DXRC_AI_EVENTS=0` | 1 | OS_EVENT_AI only when the whole backlog ran out, from inside osAi* calls (not per buffer and every retrace) |
| `-DXRC_INTMASK_LOCK=1` | 0 | osSetIntMask(OS_IM_NONE) holds the runtime's lock like __osDisableInt (stops Mario Party) |
| `-DXRC_GUARD_KUSEG_EXACT=0` | 1 | every DMA, invalidation and TLB write unverifies every KUSEG guard entry |
| `-DXRC_PI_RAW_EVENT=0` | 1 | no OS_EVENT_PI after the runtime's raw PI DMA starts |
| `-DXRC_PAK_BOTH=0` | 1 | AUTO turns Rumble Pak at the motor probe (not both paks at once) |
| `-DXGFX_BLEND_MEM=0` | 1 | FORCE_BL alone blends, even when the one-cycle blender never reads the frame |
| `N64RX_KUSEG_GLOBALS=0` / `N64RX_KUSEG_WANT=0` / `N64RX_KUSEG_TABLES=0` / `N64RX_GAP_DATA=0` | 1 | the analyser: KUSEG code's globals, its want-list pages, KUSEG jump-table bases, a data word skipped before a prologue (each off) |
| `N64RX_RELOC_OVL=all` / `=0` | learnt | the Zelda engine's relocated overlays: every one recompiled / none (no hook; the safety net) |
| `-DXRC_RELOC_OVL=0` | 1 | the runtime maps no relocated overlay (the safety net runs them) |
| `N64RX_RELOC_CAPTURES=1` | 0 | captured pages where the game relocated an overlay kept in the build |
| `N64RX_THREADS_TOGETHER=0` | 1 | __osRunningThread taken as the getters found it even where __osFaultedThread's neighbour differs |
| `-DXRC_PEEK_EVERY=<n>` | 30 | with `XRC_PEEK`: retraces between peeks; 1: every retrace the first word changes |
| `-DXRC_PI_LOG_ALL=<n>` | unset | diagnosis: the first n PI DMAs of any destination (`[PI] DMA ROM`) |
| `-DXRC_TRACE_COUNT=func_...` | unset | trace builds: that function's entries (the first 8, then every 100th) |
| `-DXRC_MEMBENCH=1` | unset | diagnosis: RDRAM's read speed (64 KB at a few offsets) at boot and at audio tasks 2 and 200, with the page table entries (`[MEMB]`; xemu's slow physical memory) |
| `-DXHW_RDRAM_HIGHEST=<physical>` | 0x02FFFFFF in test builds, 0 in release | an 8 MB RDRAM window no higher than that when it fits (xemu reads the usual place's low half ~100x slower); 0: the top of the 64 MB |
| `-DXRC_RDRAM_PAD_MB=<n>` | unset | diagnosis: RDRAM placed n MB lower in physical memory |
| `-DXRC_DIAG_EVTQ=<vram>` / `-DXRC_DIAG_POST=<vram>` | unset | diagnosis, with `N64RX_CAPTURED_INTERP` on that function: libaudio's alEvtqNextEvent queue dumped once it spins (`[EVTQ]`), alEvtqPostEvent's posts counted per retrace and by whom (`[POST]`) |
| `-DASP_HLE_OPTIME=1` | unset | test builds: the time each audio command kind takes, every 256 tasks (`[ASPT]`) |
| `-DXRC_INTS_BOOST=0` | 1 | a thread with its interrupts off (`__osDisableInt`, cop0 Status IE clear: the runtime's lock) keeps its level, as before; otherwise it runs at level 14, above every game thread, until it turns them on (Kirby 64: a priority-0 thread holding the lock was preempted by a spinning one, the VI thread blocked on the lock and the game stopped, about one boot in two) |
| `-DXRC_TV_FROM_REGION=0` | 1 | osTvType 1 (NTSC) for every game, as before; otherwise from the ROM header's region byte (PAL for D, F, H, I, L, P, S, U, W, X, Y; MPAL for B). Kirby 64 (E) told NTSC stops at its lockout screen |
| `-DXRC_PAL_50HZ=0` | 1 | a PAL game gets the console's 60 retraces a second (it runs 20% fast); otherwise every sixth vblank is skipped, 50 a second |
| `-DXRC_PEEK=<N64 address>` | unset | diagnosis: 32 bytes at that address logged twice a second from the VI thread (`[PEEK]`, `ultra/vi.c`); `-DXRC_PEEK2=<address>` adds a second |
| `-DXHW_WATCH_RW=1` | 0 | with `XRC_WATCH`: reads too; each code address that touches the word is logged once (the first 64), whatever the value. In xemu (2026-10-18, Pokémon Snap's pad record) it logged nothing, not even the per-frame writer: judge it on the console |
| `-DXRC_CONTROLS=0` | 1 | the default layout whatever settings.ini's controls say (`ultra/si.c`); the Controls page still edits the file |
| `-DXHW_PAD_LAYER=0` | 1 | the Controls page draws every shape every frame, as before (14 fps on red); otherwise the controller and the callouts are cached in a layer drawn again only when the pad, the selection or a mapping changes, and only the pulses are drawn each frame |
| `-DXRC_TLB_HELPERS=0` | 1 | the runtime's RDRAM helpers mask a KUSEG address to physical instead of translating it through the game's TLB |
| `N64RX_TRACE=1` (env, n64rx) | unset | diagnosis: N64Recomp's `trace_mode`; the last 64 functions entered (recompiled by name, interpreted by address) are logged with a crash (`[TRACE]`, `xbox/src/rt/trace.c`) |
| `N64RX_GUARD=0` (env, n64rx) | 1 | functions recompiled from captured code run without checking that RDRAM still holds the bytes they were recompiled from (`guard.c`) |
| `N64RX_STRANDS=0` (env, n64rx) | 1 | the game's own setjmp/longjmp (found by shape) are recompiled as they are instead of run as coroutines on host threads (`strand.c`); Mario Party's processes then corrupt the main loop's registers |
| `N64RX_CAPTURED_INTERP=1` or `=<lo>-<hi>` (env, n64rx) | 0 | diagnosis: every function in captured code (or in that vram range) is interpreted; Banjo-Kazooie past its crash with its overlay region interpreted |
| `N64RX_WALK_RDRAM=0` (env, n64rx) | 1 | the function walker reads jump tables from the ROM only, as before; otherwise also from captured RDRAM (learn.py `_table_sources`): GoldenEye's 0x7F000000 functions with tables at 0x8005xxxx recompile (its level's interpreted instructions 8.4 million per 300 retraces down to 42 thousand, 26 to 30 fps in xemu). Its black level and modelless watch with this on were its `cosf` falling into `sinf` (`N64RX_FALLTHROUGH`) |
| `N64RX_FALLTHROUGH=0` (env, n64rx) | 1 | a function is translated up to the next function's start, as before; otherwise one whose path runs off its end (its last instruction, or the one its delay slot follows, not a jr, j or unconditional branch) takes the next one's body too: GoldenEye's hand-written `cosf` is three instructions that fall into `sinf`, and recompiled it returned with `$f0` unset (the Dam's dark sky and far geometry, Bond's gun and hand missing) |
| `N64RX_TLB_ROM_RUN=0` (env, n64rx) | 1 | a ROM-backed TLB segment spans its 1 MB region from the first page that matched the ROM to the last and 64 KB on, as before; otherwise it never covers a captured page of the region that differs from the ROM at its offset (Perfect Dark's first pages at 0x70000000 match the ROM's start, its other code there does not: 0x70003EE0 was compiled from unrelated ROM bytes) |
| `N64RX_XBRANCH_COND=0` / `N64RX_XBRANCH_OVL=0` (env, n64rx) | 1 | the cross-branch check (`emit.cross_funcs`) lets a conditional branch to another function's start through and checks the functions of overlays sharing a vram by vram alone (never live there), as before; otherwise both send the function to the interpreter: N64Recomp only makes a `j` or an unconditional branch to a start a tail call, a conditional one stays a goto ("use of undeclared label": Ocarina of Time's learnt overlays). The check also scans the translated size (`_size`, with a fallen-into neighbour) |
| `N64RX_RA_HOOKS=0` (env, n64rx) | 1 | nothing sets `$ra` at a call, as before (N64Recomp's generated code never does); otherwise each `jal` in recompiled code to a function that reads `$ra` as a value (`emit._reads_ra`: an offset from it, a load through it, arithmetic on it) gets an N64Recomp hook that sets `ctx->r31` to its return address first, and such a function the interpreter runs starts with that ra instead of its sentinel and returns on a jump back to it (`xrc_interp_ra`): Jet Force Gemini's call-site resolver finds its caller's `jal` at `ra - 8` (497 calls) |
| `N64RX_TLB_ROM=0` (env, n64rx) | 1 | captured KUSEG pages stay captures (`code_<vram>`); otherwise pages the ROM holds as they are become one ROM-backed segment (`tlb_<vram>`, learn.py `_rom_backed`) |
| `N64RX_RECOMP_PRECHECK=0` (env, n64rx) | 1 | what N64Recomp would crash on (a jump table outside the function's segment: it reads the ROM past its end) or refuse one run at a time (a jal to no function, a cop0 register other than Status) is left to it instead of interpreted up front |
| `-DXRC_LEARN_CODE=0` | 1 | the safety net does not save the RDRAM block of code no ROM DMA put there (`learn_code.bin`, `[LEARNCODE]`) |
| `-DXRC_STATUS_INIT=<value>` | 0x2000FF01 | the COP0 Status a thread starts with (libultra's: COP1 usable, every interrupt line unmasked, IE) |
| `-DXGFX_HOLD_EVICT=0` | 1 | the BACK menu never drops the game's texture cache when its held frame or card finds the texture pool full (Mario Kart 64 on red: the card was missing) |
| `-DXGFX_UCODE_F3DEX=0` | 1 | F3DEX 0.9x/1.x microcodes (and F3DLX, F3DLP, L3DEX, .NoN/.Rej) read as Fast3D, the build before the registry (`docs/renderer.md`) |
| `-DXGFX_UCODE_F3DEX2=0` | 1 | F3DEX2 microcodes (F3DZEX, F3DLX2, .NoN/.Rej) read as Fast3D |
| `-DXGFX_UCODE_S2DEX=0` | 1 | S2DEX 1.x/2.x read as Fast3D (its own commands are skipped either way; not drawn yet) |
| `-DXGFX_FB_READBACK=1` | 0 | framebuffer readback (docs/renderer.md "Framebuffer effects"): each frame copied N64-size to a cached stage; frames whose pages the game touched (x86 accessed bits) converted into RDRAM every frame for 2 s. 2: every frame, no watch. 3: staged every frame, converted only when the game invalidates the data cache over at least half a frame (`osInvalDCache`) or textures from it: Banjo-Kazooie's jigsaw transition (it copies its frame into the pieces' textures) right, 2 conversions per 600 frames, Smash Bros. at 720p unchanged (xemu). `-DXGFX_FB_INVAL_SHOT=n` (debug): a screenshot n frames after each such conversion. Off by default: Pilotwings 64 went hot every ~2 s and ran at ~14 fps in xemu with 1 |
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

### A game's own frame, replayed on the host (`tools/xbox/bench_dl.py`)

A test build made with `-DXRC_DLDUMP=<retrace>` dumps the first
`XRC_DLDUMP_N` (6) graphics tasks after that retrace over COM1 (`rt/gfx.c`):
each task's words (`[DLTASK]`), the whole RDRAM image once (`[DLRAM]`,
deflated), then for each later task only the 4 KB pages that changed
(`[DLPAGES]`). COM1 carries about 10 KB a second in xemu: the image takes a
minute or two, a later task seconds; run long enough for it to end (the
`[DLRAM] END` line). `python tools/xbox/bench_dl.py <serial.log>` builds
`tests/xbox/bench_dl.c` with the real interpreter and runs the tasks over
their images: `--png out.png` draws the first run in software (the
combiner's one or two cycles, point sampling, depth, alpha test, blending,
fog, the scissor: enough to see what a list draws, not the RDP's exact
pixels), `--draws` lists every draw's state as `[BDRAW]` lines, `--trace N`
gives the interpreter's own `[DRAW]` trace of task N, `--runs N` times the
lists over a back end that only counts and, on Windows, samples the pc
every millisecond and lists the hottest functions (the host's CPU, not the
Pentium III: shares, not milliseconds), `-D NAME=value` builds with a
switch. The image is the game's (ROM-derived): the tool keeps it in a
temporary folder; keep the logs in an ignored folder
(`build-host/dumps/`) and the pictures out of the tree.

On the console: `-DXRC_DLBENCH=<n>` (any build) runs one graphics task
every n retraces `XRC_DLBENCH_RUNS` (8) more times before its real run,
first with the back end left out, then with it, and logs
`[DLBENCH] retrace R, list L: A us a run with no back end, B us with it`:
the interpreter's own share of `[PERF]`'s "display lists" against the
NV2A's (push buffer, waits). The game sees one long frame each time.
`-DXHW_PROF=1` (`xhw_prof.c`) samples every thread's pc on the console
(`tools/xbox/sym.py --map <build's map> --prof boot.log`; statics fold into
the public symbol before them).
