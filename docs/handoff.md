# Handoff

One entry per milestone: what was verified, how, and where ("xemu" results
are emulator-only; timing, audio, memory and HDD latency are judged on the
console).

## Where things stand (2026-10-16, end of the parallel round)

The newest state first; the entries below are the record, oldest first.

| game (US) | how built | xemu | console (red) |
|---|---|---|---|
| Pilotwings 64 | its pack, or the ROM alone (`--analyze`): the same | title to results, saves, sound, 720p + hor+, the BACK menu | `vnrx1`: plays, sound, EEPROM saves, the BACK menu, the combos; 53-59 fps |
| Mario Kart 64 | the ROM alone | title, menus, races (F3DEX), sound (nead), Controller Pak, 720p 16:9 | `vmk2`/`vmk4`: races at 29.9 fps (display lists 2-3 ms), sound, the ROM inside the XBE, the BACK menu and its Paks page, EEPROM and pak files; the map screen's framebuffer effect is wrong |
| Super Mario 64 | the ROM alone + three `--learn` rounds | head screen, Lakitu intro, castle grounds (TLB aliasing); head shading off | not tried |
| Dr. Mario 64 | the ROM alone | links, boots, faults on PI_STATUS through KSEG1 | - |
| Pokémon Snap | the ROM alone | builds (overlays sharing a vram mapped by the last DMA); runs its intro (F3DEX2, FlashRAM), loads the main-menu overlay, then reads NULL+0xe under `omCreateProcess` | - |
| the library (246 games) | the ROM alone (`tools/xbox/sweep.py`) | 143 build an XBE, 17 present frames in 45 s | - |

Built and merged in the round (each with host tests and a kill switch; the
entries below say how each was verified): the ROM inside the XBE, the ROM
page cache (ROMs over 16 MB), the safety net and the learning loop
(`--learn`), the graphics microcode registry with F3DEX/F3DEX2, the audio
microcode registry with nead and `n_aspMain`, every save type (Controller
Pak, SRAM, FlashRAM), the BACK menu's Paks page, the TLB, the V track's
`decomp_check.py`/`test_analyze.py`, the library sweep. Merged since: `snap` (overlays sharing a vram, below). Agents still running
when this was written: `fbfx`
(framebuffer effects), `mmio` (the RCP registers and RDRAM through KSEG1).

**Open, in the order they block the most games** (counts from the sweep):
the RCP's registers reached through KSEG1 by libultra the analyser did not
name (75 games), entry code that is not the SDK's (71), TLB-mapped
addresses (23, before the TLB merge), no graphics task (14), boot segments
below the entrypoint (14), branches into another function's body (12),
KSEG1 RDRAM (9); then overlays sharing a vram (Snap, Paper Mario), Yaz0 and
gzip-compressed code (the Zeldas, Dr. Mario 64), S2DEX's own commands,
framebuffer effects, MusyX and other audio microcodes, M2's leftovers (720p
on the console, the 30-minute soak, Pilotwings' pack README), M3's
measurement gate.

## M0: skeleton, toolchain, the tool's shape (2026-10-16, branch `dev`)

Built on Windows 11 with MSYS2 (clang 21.1.8), nxdk `58427c0`, N64Recomp
`ffb39cd`; run in xemu 0.8.136 (slot 9, 64 MB, HDTV AV pack, 480i dashboard).

**Verified**

- `n64rx doctor` on this machine: native engine ready (MSYS2, LLVM 21,
  cmake, ninja, nxdk at the pin, N64Recomp), Docker "not installed" with the
  install hint. The "no Docker" verdict is also covered by `test_n64rx.py`.
- `n64rx build build-host/test/test.z64` (the test ROM, `make_test_rom.py`)
  through the native engine: ROM read and matched by SHA-1, N64Recomp (5
  functions), the register scan (32-bit), the compile (`--fast` and ThinLTO),
  the XBE pass (title ID `4E52FFFE`, `$$XTIMAGE`, `default.tbn`), the output
  folder. A second build with the same inputs skips the recompile.
- xemu, the test pack: loading screen, "Preparing memory", "Loading code",
  "Starting N64Recomp-X test", then **"Test ROM: passed", 34 checks, 0 failed**
  with 32-bit registers (`--fast`) and with 64-bit registers under ThinLTO
  (`--gpr 64`). `[RECOMP] window: 4 MB, physical 03a31000`.
- xemu, the platform alone: the loading screen and the "No game built in"
  card; `[FBDUMP]` baselines with `-DXHW_SPLASH_STILL=1` in
  `docs/look/loading-480i-still.png` and `docs/look/card-no-game-still.png`.
- xemu, scripted input (autopad, `t<ms>` events): BACK held during the
  loading screen -> `[SETTINGS] BACK held at boot: 480i 4:3 (saved)`, and the
  next boot reads 480i; BACK + START -> `[SETTINGS] reset at boot (BACK +
  START): defaults` plus 480i; L + R + BACK + BLACK on the card -> `[PAD] port
  1: L + R + BACK + BLACK`, dashboard, and the next boot logs `[BOOT] previous
  exit: quit to the dashboard: L + R + BACK + BLACK on port 1`; the same combo
  (300 ms press) on the loading screen; L + R + BACK + WHITE -> restart, and
  the next boot logs `previous exit: restart: L + R + BACK + WHITE on port 1`.
- Host tests on Windows (MSYS2 clang as the host compiler): `test_pad_combo`
  35 checks, `test_splash` 29, `test_settings` 16, `test_n64rx` 11 tests,
  `rom_leak_check` clean.
- `tools/xbox/setup.sh` on Windows (MSYS2 path): installs the missing
  pacman packages, finds nxdk and N64Recomp built.
- Console variants (PLAN.md 3.13), xemu: at 64 MB `[MEM] boot: 65536 KB of
  RAM, nothing to hold (64 MB console)`, `[CLOCK] cpu 733 MHz (TSC against
  the ACPI timer, 3375000 Hz)`. With `mem_limit = '128'` (`-m 128`) the
  kernel still gives the XBE 64 MB (this BIOS honours cxbe's 64 MB-limit
  flag): the same `[MEM]` line, the console runs as 64 MB. Not exercised:
  `xhw_ram_lock64`'s hold path (a kernel that hands out the upper half
  anyway) and an upgraded CPU (xemu has neither); the console runs of §3.13.

**Not verified (and why)**

- Linux and macOS: no such machine here. `setup.sh`'s apt and Homebrew paths,
  the native engine there, and the host tests with `cc` are written but
  unrun until CI runs them (`.github/workflows/build.yml`, three-OS matrix).
- CI: the workflow is written; it runs once `dev` is pushed (not pushed:
  pushing waits for the maintainer).
- Docker: not installed here. The Dockerfile (multi-arch, N64Recomp inside),
  the Docker engine and the GHCR push job are unbuilt; the image is not on
  GHCR yet.
- The watchdog's last-resort combo look: covered by `test_pad_combo` (the
  timer) and wired in `xhw_watchdog.c`, but no xemu scenario stalls every
  other look in M0 (there is no VI thread yet; M1).
- The console (red, gold): nothing of M0 has run on hardware. Worth checking
  first there: the loading screen's frame cost (`[BOOT] loading screen: N ms a
  frame` in a test build), the controllers' bring-up time, interlace flicker
  on the light sweep and the sparks at 480i.

**Known gaps carried into M1**

- `xhw_rom.c` is still Wings64-X's (Pilotwings 64's size and table); the
  runtime does not read a ROM yet. M1 generalises it to the pack's table.
- No libultra entry points, no VI thread, no boot block or boot copy (M1).
- `test_recomp_h` (our header against upstream's on the host) is M1's.

## M1: the runtime core on Pilotwings 64, first frame (2026-10-16, branch `dev`)

Windows 11 + MSYS2 (clang 21.1.8), nxdk `58427c0`, N64Recomp `ffb39cd`; xemu
0.8.136 slot 9; the maintainer's own US ROM (`ec771aed...`), never in the tree.

**Verified**

- `n64rx build "Pilotwings 64 (U) [!].z64"`: the pack matches by SHA-1; 1956
  functions in the symbol file, 1899 recompiled into 37 files; the scan finds
  no 64-bit instruction: 32-bit registers; the generated code compiles under
  the game triple with our `recomp.h` (implicit declarations stay errors:
  n64rx's `xrc_extern.h` declares the 58 libultra entry points and the other
  calls `funcs.h` lacks); ThinLTO and `--fast` both link.
- xemu, release build (ThinLTO): `[ROM] verified`, `[RECOMP] 1899 functions
  in 3 sections`, `[RECOMP] code: 783 KB of MIPS -> about 1778 KB of x86
  (x2.2)`, `[RECOMP] entrypoint 80200050, 32-bit registers`; `[OS]` threads 1,
  0, 4, 6, 3 at levels 9, 13, 11, 8, 10 (N64 priorities 12, 250, 127, 10, 110);
  the title screen drawn by the interpreter (FBDUMP, compared by eye with the
  game's own title: logo, the six pilots, the menu, the copyright line; the
  shot stays outside the tree); the intro and attract demos for 4 minutes,
  `[PERF]` ~50 fps in xemu (xemu timing, not the console's), no crash, no
  `crash.log`.
- `test_recomp_h`: upstream's header vs ours on x86-64 (85,015 lines, none
  differ), ours on i686 with `-march=pentium3 -msse -mfpmath=sse` and the
  Xbox's x87 control word (none differ), 32-bit registers vs 64-bit on the
  68,013 sign-extended cases (none differ).
- Host tests: `test_syms`, `test_scan64`, `test_time` (7 checks; no TSC read
  outside the clock module), plus M0's, with clang and with GCC.
- `[MEM] boot: 65536 KB of RAM, nothing to hold`, `[CLOCK] cpu 733 MHz` (xemu).

**Not verified (and why)**

- The console: no hardware run yet. Audio: tasks run but `asp_hle` is not
  wired (M2); `[PERF]`'s "audio 0 tasks" says so.
- Wings64-X's `[FBDUMP]` of the same frame side by side: compared with the
  game's look instead (the title screen is unambiguous); M2 runs Wings64-X's
  scenarios with shots on both builds.
- CI for M1: manual-only now (the maintainer's free minutes). The last
  automatic run (before M1's tests) had Linux, macOS and Windows through the
  toolchain, host tests, platform XBE and n64rx tests, failing only at the
  end-to-end step on a missing folder, since fixed; unrun since.
- `test_f3d` (synthetic display lists per family): not written. The F3D
  interpreter is Wings64-X's, hardware-tested there; the test comes with
  F3DEX in M4.

**Fixed on the way (docs/decisions.md):** `f_odd` must point at f0's high
word; PI device addresses are cartridge offsets; synchronous tasks' done
events go to the front of their queue; `_uvScDlistRecover`'s SP_STATUS
store is a pack instruction patch.

## M2 (in progress): Pilotwings 64 complete on the recompiled path (2026-10-16, branch `dev`)

### The first console run (red, build `vnrx1`, commit `6ad6148`)

ThinLTO test build (`-DXHW_TEST_BUILD=1`), XBE SHA-1 `f523245693c2`, in
`F:\Applications\N64Recomp-X-PW64\`, title ID `4E520001` (apart from
Wings64-X's folder and `57580001`, untouched). Red: 480i composite
dashboard (AV pack 1, no HD flags), one Duke on port 1.

**Verified (console)**

- The maintainer flew it: "works flawlessly ... even better than the native
  port". Sound on the AC97 (`[AUDIO] AC97 polled, 22047 Hz in -> 48 kHz`;
  the resampler table found at data+0c0; 0 bad addresses, 0 unknown
  commands; 6 AI underruns in 135 s, 0 dropped).
- `[MEM] boot: 65536 KB of RAM, nothing to hold (64 MB console)`, `[CLOCK]
  cpu 733 MHz (TSC against the ACPI timer, 3375000 Hz)`; 26 MB free after
  pbkit; `[RECOMP] code: 783 KB of MIPS -> about 1783 KB of x86 (x2.2)`.
- `[PERF]` 53-59 fps presented, 16.9-18.8 ms a frame, display lists
  2.4-6.7 ms, audio 0.7-1.1 ms a frame (646-1079 us a task), the rest idle;
  7306 frames presented in 7658 retraces.
- The loading screen at 11 ms a frame.
- EEPROM: the first boot found none (`blank EEPROM`), the game wrote
  `pw64.eep` three times; the next boot read it (`2048 bytes`) and the
  maintainer's progress was there. The image (466 bytes not 0xFF) is kept
  outside the tree (`C:\xemu\hw\saves\nrx1-pw64.eep`).
- The BACK menu over the game (Texture filter changed at once, saved on
  close, `settings.ini` read back); L + R + BACK + BLACK quit to the
  dashboard both times, named by the next boot and `lastexit.txt`.

**Not verified yet**: 720p and widescreen on the console (red's dashboard
is 480i only: needs an HD AV pack), a save moved to or from Wings64-X
(same byte order by construction), the 30-minute soak.

### ROM-only builds (2026-10-16, commits `8928df4`, `38e479c`)

**Verified**

- Pilotwings 64 with `--analyze` (its pack's patches mapped on top): the
  analyser finds 1952 of the decomp's 1956 functions (the 4 missing are
  libultra's exception handler, which the runtime replaces), the app
  segment exactly (ROM `0x51E30` at `802CA900`, `0x8C8F0` bytes), all 54
  replaced libultra functions named, none wrong; 32-bit registers. xemu:
  the Wings64-X menus scenario (title, file select, vehicle and class,
  pilot, briefing, flight, results) shot for shot the same as the
  decomp-symbol build; the EEPROM written and read; sound. One miss on the
  way (a jump table whose `lui` sat in a delay slot) crashed with "No
  function at this address" and was fixed in the analyser.
- Mario Kart 64 with no pack (US, `579c48e2...`): 3203 functions in the
  boot segment and both race segments, 71 libultra functions named, one
  hand-written routine interpreted (`80040C54`), 32-bit registers. xemu:
  the title screen drawn correctly. Deployed to the console (red) as
  `vmk1` (superseded by `vmk2`/`vmk4`, below).
- Runtime additions for ROM-only builds: the interpreter, the PIF, the
  `osEPi*` family, rumble, the remaining reimplemented names; `__ll_rem`
  corrected to libultra's unsigned remainder.

**Not verified / gaps**

- (All closed by the parallel round below: Mario Kart 64's races, sound and
  Controller Pak; the learning loop; `decomp_check.py`/`test_analyze.py`;
  the ROM inside the XBE.)

## The parallel round: ROM-only toward the library (2026-10-16)

Agents on the maintainer's console API credit, one git worktree and xemu
slot each (slots 9-14), merged into `dev` by the orchestrating session after
review; the orchestrator's own merges and fixes are noted where they
happened.

### The ROM inside the XBE (2026-10-16, branch `agent/romxbe`)

Windows 11, xemu slot 12; the maintainer's own ROMs, never in the tree.

**Verified**

- `n64rx build` writes `default.xbe` with the normalised ROM as `$ROM`
  (flags inserted + head/tail read-only, no preload, VirtualSize 0, raw
  offset 4 KB aligned, the last thing in the file); the folder is
  `default.xbe`, `default.tbn`, `TitleImage.xbx`, `TitleMeta.xbx`, the
  README. Host: `test_xbe_rom.py` (a synthetic XBE and the platform XBE,
  the test ROM, found by the runtime's parser, `xrom_verify` on the
  embedded bytes, a flipped bit caught) and `test_n64rx.py` (the folder,
  `--rom-beside`).
- xemu, `--fast` builds staged as the XBE alone: Pilotwings 64 (`[ROM]
  $ROM in D:\default.xbe at 0x2e6000, 8388608 bytes`, SHA-1 ok, 8 MB read
  in ~60 ms, the game running ~33 fps) and Mario Kart 64 from its ROM
  alone (12 MB, SHA-1 ok, running). `[MEM]` the same to the KB as the
  `--rom-beside` builds: `[MEM] boot: 65536 KB of RAM, nothing to hold`;
  Pilotwings 57212 KB free at boot, 26152 KB after nv2a; Mario Kart 57000 /
  21712 KB. The first try gave `$ROM` a virtual range (SizeOfImage grown
  over it): 4 KB less free at boot for 8 MB (page tables), hence
  VirtualSize 0.
- Failure cards in xemu: `-DXHW_ROM_IN_XBE=0` with no file beside: "No ROM
  found"; a flipped byte inside: "Bad or modified ROM dump" (message: copy
  `default.xbe` again); an XBE cut 1 MB short: "default.xbe is cut short"
  (the kernel still starts it: `$ROM` is never loaded).

**Not verified / gaps**

- The console (a retail kernel; modded BIOSes): `$$XTIMAGE`, also not
  preloaded and unsigned, runs there; `$ROM` has not. `[CLOCK] cpu 733 MHz
  (TSC against the ACPI timer, 3375000 Hz)` (xemu; nothing timed changed).
- An XBE over a DVD-R (`n64rx xiso`): one big file, not tried.
- `n64rx deploy` uploads the folder's files as before (now without a ROM
  file) and refuses a folder with no ROM inside or beside; not run against
  a console here.

### The safety net and the learning loop (2026-10-16, branch `agent/learn`)

**Done, verified how**

- `xbox/src/rt/interp.c` made complete for game code: a 64-bit register
  file per run (GPR32 builds run 64-bit code inside), COP1 as the
  generated code reaches it (all formats, round/trunc/ceil/floor/cvt, the
  FCR31 rounding mode, compare and likely branches), traps, syscall,
  ll/sc, VR4300 divide by zero, `b .` -> `pause_self`, COP0 shadow + TLB
  no-ops. `tools/xbox/test_interp.py` (new): passes with 64- and 32-bit
  registers.
- `xbox/src/rt/learn.c` (new): the safety net (a miss in `get_function`
  that holds code runs interpreted through a per-address stub), the
  `[LEARN]` log and `E:\UDATA\<title id>\learn.txt`, the DMA ring
  (`ultra/pi.c`), `osInvalICache` relogs (`ultra/misc.c`).
  `-DXRC_SAFETY_NET=0` restores the card.
- `n64rx build --learn` (`n64rx_lib/learn.py`; hooks in `romonly.py`,
  `pipeline.py`, `cli.py`); `n64rx log --pull` fetches `learn.txt`;
  `N64RX_WITHHOLD` test switch; pipeline's refusal retries 40 -> 400.
- xemu (emulator only, slot 14): Pilotwings 64 ROM-only with its app
  thread withheld runs on the net (35 fps), the `--learn` rebuild logs
  nothing (38.5 fps); Super Mario 64 learns its two run-time segments
  (`docs/analyzer.md`); Mario Kart 64 to its title: no net hits.
- `[MEM]`/`[CLOCK]`: not re-measured (no console run; the net adds 512
  stubs x 24 B, a 1,024-entry DMA ring, a 48 KB learn text).

**Not verified / gaps**

- No console run of the net (the stubs execute from .bss: fine on the
  P3, which has no NX; xemu agrees).
- SM64 after `--learn` faults in recompiled goddard code (a read of
  `04000650`: goddard maps `0x04000000` through the TLB, which the runtime
  does not have); Dr. Mario 64 does not link (`__osEnqueueAndYield_recomp`
  undefined); Banjo-Kazooie is not analysed (its boot is compressed):
  none of these reached the net.
- Interpreted code is slow (no decoded-block cache); a learnt segment that
  overlaps another overlay at the same address is not handled (M5).

### The ROM page cache (2026-10-16, branch `agent/romcache`)

Windows 11, xemu slot 12; the maintainer's own ROMs, never in the tree.
PLAN.md §3.5 "page cache", §3.10; `docs/decisions.md` has the design.

**Verified**

- Host: `test_romcache.py` (the core on a fake file and a fake
  reservation: chunk boundaries, DMAs over 3 and 11 chunks from odd
  offsets, word reads across a boundary, a partial last chunk, out-of-range
  requests, the LRU order and its cap, a DMA bigger than the cap, pins and
  their half-cap limit, failed fills and commits handing over zeros and
  retrying, the counters, v64/n64 files normalised per chunk, 20000 random
  reads against the image, the streamed SHA-1 equal to the whole one);
  `test_n64rx.py` (`[rom] pin`, `[rom] resident`); `test_xbe_rom.py`.
- xemu, Pilotwings 64 with the cache forced (`N64RX_CFLAGS="-DXHW_AUTOPAD=1
  -DXRC_ROM_RESIDENT_MB=0"`, scenario `tools/xbox/scenarios/flight`):
  title, menus, a flight to the results card, as with the resident ROM.
  `[ROM] 8192 KB streamed and hashed in 464 ms`; `[MEM] after nv2a free
  33132 KB` (resident: 26152 KB); `[PI]` the first second 45 misses
  (2880 KB, stall 127 ms), then about one miss a second; 4.6 MB held after
  100 s, nothing evicted.
- The same with a 1 MB cap (`-DXRC_ROM_CACHE_MB=1`): plays to the same
  results card; 14-26 misses and evictions a second in the busy seconds,
  stall up to 45 ms a second (xemu's file reads), never a wrong byte seen.
- Rayman 2 (32 MB, from its ROM alone): `[ROM] 32768 KB streamed and
  hashed in 1429 ms`, `[PI] cache: 32 MB ROM reserved ..., LRU over 16 MB`,
  `[MEM] rom page cache free 55800 KB`, `after nv2a 35108 KB`, the boot copy
  through the cache; the game then stops in the interpreter on `tlbwi`
  (`42000002` at `80003d60`: TLB mapping, not the ROM). Ridge Racer 64
  (32 MB) the same: 1204 ms for the hash, 35156 KB free after nv2a, then
  `tlbwi` at `8000ed60`.
- `[MEM] boot: 65536 KB of RAM, nothing to hold (64 MB console)`;
  `[CLOCK] cpu 733 MHz (TSC against the ACPI timer, 3375000 Hz)`, retrace
  58.55 Hz (xemu); nothing timed changed.

**Not verified / gaps**

- The console: the HDD's ~10 ms seeks against xemu's host file reads; the
  `[PI]` stall numbers above are emulator-only. Audio DMAs that miss stall
  the audio thread (pins are the answer; no pack has any yet).
- No 32 MB ROM runs far from the ROM alone yet: Paper Mario, Kirby 64,
  Pokemon Stadium stop in the analyser ("the boot segment would be
  -0x..."); Turok 2/3, Spider-Man, Road Rash 64, Shadow Man, Battle for
  Naboo, Nuclear Strike, Indiana Jones, SW Racer ("the entry code ... is
  not the SDK's"); Mario Party 1-3, Mario Golf, Pokemon Puzzle League,
  Mega Man 64, Hydro Thunder, No Mercy, Gex 3, Banjo-Tooie (E) fail in
  N64Recomp or the compile; Ogre Battle 64 (40 MB) stops n64rx with a
  Python traceback (`gen/funcs.h` missing after the recompiler); OoT and
  Majora's Mask need Yaz0 (M5). Rayman 2 and Ridge Racer 64 need `tlbwi`
  in the interpreter.
- Prefetch hints and `[rom] dvd_cache_mb` are not built; the cache cap at
  720p follows the boot's prediction (a BACK hold to 480i keeps the 12 MB).
- `xu_rom_ptr` is resident-only now (NULL with the cache): new readers use
  `xu_rom_read`/`xu_rom_to_rdram`.

- Decided, to build: the ROM inside the XBE (`$ROM`, PLAN.md §3.5); the
  output folder still holds the ROM next to `default.xbe`.

### The audio microcode registry (2026-10-16, branch `agent/audio`)

**Why Mario Kart 64 was silent:** its synthesizer (SM64 EU's, "nead")
speaks another audio ABI (`A_ENVMIXER` 19, `A_LOADBUFF` 20, ...); every
command fell into asp_hle's unknown slot (xemu: 36604 unknown commands in
10 s, peak 0). Also the first AI buffer came before the first task, so the
microcode was never logged and its resampler table never searched.

**Verified**

- Host: `test_asp_hle.py`, 14 builds (MMX, scalar, -O0, the pertap
  variant, word-swapped, 32-bit pentium3 gcc) pass 42845 checks: the
  registry on synthetic tables; every nead command (DMA with its own
  offsets, the Q16 envelope with and without the first nead's limits,
  MIXER / ADDMIXER, both INTERLEAVEs, DMEMMOVE2, DOWNSAMPLE_HALF, FILTER,
  DUPLICATE, HILOGAIN, RESAMPLE_ZOH, 2-bit and 4-bit ADPCM, RESAMPLE) and
  `n_aspMain`'s (the fixed buffers, the linear envelope over three calls
  through its RAM state, ADPCM, RESAMPLE into N_MAIN2) against
  `asp_ref.c`; `hash2` / `rhash2` the same in every build and layout.
  aspMain's workload hash is unchanged (`8cb5eefe`).
- The registry's rules against the maintainer's ROMs (a host scan, not in
  the tree): Pilotwings 64, SM64 US, Pokemon Snap: aspMain; Mario Kart 64:
  nead, 32 entries, no ADDMIXER (the first nead); Ocarina of Time (its
  `code` file, Yaz0-decompressed by the scan): nead, 24 entries; Paper
  Mario, Smash Bros.: `n_aspMain`.
- xemu (not hardware), Mario Kart 64 from its ROM alone: `nead (32-entry
  table at data+10, opcodes 007ffda7, flags 3)`, the resampler table found
  at data+100, 0 unknown commands, peak 28316; 3 s from second 20 (title
  and attract): RMS 4360, L/R correlation 0.71, 98 % of the energy below
  4 kHz (white noise ~30 %). Pilotwings 64 (pack): still aspMain, RMS
  2979, 98 %.

**Not verified / gaps**

- SM64 US (aspMain), Paper Mario and Smash Bros. (`n_aspMain`) could not
  be run: SM64's ROM-only build stops after N64Recomp (`gen/funcs.h`
  missing), Paper Mario's and Smash Bros.'s analysis refuses the boot
  segment. `n_aspMain` is host-tested only.
- Not implemented (counted as unknown, skipped): S8DEC and UNK_25 (SM64
  Shindou), `n_aspMain`'s opcode-14 filters (Donkey Kong 64, Conker),
  MusyX and the MP3 microcode. nead's FILTER (the Zelda microcode's note
  filter) is host-tested only; it leaves the game's table alone where an
  emulator writes the averaged taps back to it.
- The nead and `n_aspMain` envelopes are scalar C (no MMX path yet); the
  cost on the P3 is unmeasured ([PERF] audio on the console; xemu's 300 to
  1200 us per task is not a number). The exact rounding of the nead
  envelope and the MIXER follows emulator behaviour and the aspMain
  oracle, not a disassembly of the nead microcode.
- The console run.
### Saves: every type (2026-10-16, branch `agent/saves`)

**Verified**

- Host: `test_saves.py` (216 checks): libultra's pak CRCs against a second
  formulation; the blank pak's format as libultra checks it (ID sums, the
  inode sum 0x71, the mirror); PIF read/write round trips with CRCs, the
  bank register, no pak; the Rumble Pak's probe and motor, the AUTO rule;
  the runtime's osPfs* (init, allocate, write, read, state, find, number of
  files, free pages, delete, the checker on a broken entry, a zeroed pak
  repaired, no pak, a Rumble Pak); the Flash's ID, sector and chip erase,
  page program (NEW_FLASH's second 0xB4), read array, status and its clear,
  the kind from a command or a file; SRAM by PI DMA, its mirror, words.
- xemu (slot 13, emulator only), Mario Kart 64 from its ROM alone, test
  build: the game's own (recompiled) pak code reaches the pak through the
  PIF (`[SI] port 1: the game's own code reaches its pak`), the blank pak is
  created and written (`[SAVE] ...nkte.mpk written`); with START held at
  boot the game's "N64 Controller Pak Data" screen shows the pak: no notes,
  123 pages free; the next boot reads the 32 KB file back. Its EEPROM
  (`nkte.eep`) still loads.

**Not verified / gaps**

- No ghost or record written to the pak in a race (MK64 races need F3DEX).
- SRAM and FlashRAM only on the host: Ocarina of Time, Paper Mario and
  Pokémon Snap were not booted here (graphics and the analyser come
  first). The runtime's osFlash* and the domain-2 handle family compile but
  ran on no game.
- The Rumble Pak at the PIF level ran on no game; the console not touched.
- `-DXRC_CONT_MAX_ADDR` needs osContInit named (the analyser names it in
  every ROM so far); without it a recompiled pak path sees no pak.
### The microcode registry and the F3DEX family (M4 work, agent `f3dex`, 2026-10-16)

**Verified**

- `python tools/xbox/test_f3dex.py`: the real interpreter over a fake
  RDRAM and back end; Fast3D, F3DEX, F3D_OLD, F3DEX2 lists built in the
  test (TRI2, QUAD, CULLDL, BRANCH_Z, POPMTX, segmented G_VTX, the 32-entry
  buffer, back-face culling with F3DEX2's bits, G_LOAD_UCODE switching the
  family), CI4/CI8 through a TLUT, the version texts; passes, and fails when
  the F3DEX triangles are dropped (mutation check).
- Mario Kart 64 from its ROM alone, xemu (slot 10), `scenarios/mk64_race`:
  `[GFX] ucode RSP Gfx ucode F3DEX 0.95 ... F3DEX, 32 vertices, old`; the
  title, game select, player select, map select and a Luigi Raceway race
  (HUD, the karts, the track, the course map) drawn; no texture misses (CI
  textures came with G_LOADTLUT), no unhandled commands; A held from the
  countdown, the kart drives (5th place, an item picked up). At 720p 16:9
  (`scenarios/mk64_hd`, a slot config with `eeprom_video.py
  480p,720p,widescreen`): 720p kept with A, the race hor+ with the HUD in
  the 4:3 box.
- Pilotwings 64 (pack build), xemu, `scenarios/menu`: `[GFX] ucode RSP SW
  Version: 2.0D ... Fast3D`; the BACK menu over the held frame as before.

**Not verified / gaps**

- No F3DEX2 game runs from its ROM alone yet: Paper Mario, Smash Bros.,
  Pokemon Snap and Banjo-Kazooie stop in the analyser ("the boot segment
  would be -0x... bytes"), Dr. Mario 64 at the link (undefined
  `__osEnqueueAndYield_recomp`). F3DEX2 is host-tested only.
- S2DEX's own commands are skipped, not drawn (`docs/renderer.md`).
- Mario Kart 64 on the console; a white translucent puff over the driver's
  back in races (particles or their depth; `docs/renderer.md` gaps).
- `[MEM]`/`[CLOCK]`: unchanged by this work (no allocation added; the
  vertex buffer grew from 16 to 80 entries, ~3 KB of BSS).
- Decided, to build: the ROM inside the XBE (`$ROM`, PLAN.md §3.5); the
  output folder still holds the ROM next to `default.xbe`.

### The V track's first files: decomp_check, test_analyze (2026-10-16, branch `agent/analyzer`)

**Verified** (host; xemu is not hardware)

- `tools/xbox/decomp_check.py` and `tools/xbox/test_analyze.py` on four
  pairs, 0 wrong replaced-libultra names in each (`docs/analyzer.md`'s
  table): Pilotwings 64 1952/1956, 75 right, live set identical; Mario
  Kart 64 3157/3159 placed, 66 right; Super Mario 64 main 3516 of 3573,
  engine 283 of 284, goddard 693 of 699, 66 right; Paper Mario 2443/2680
  in the segments found, 56 right; Dr. Mario 64 boot 262/270, 56 right
  (its main segment is compressed).
- Analyser fixes they drove: the 6103/6106 entrypoint offset (Paper
  Mario), `(start, end, dest)` and `(dest, start, end)` DMAs, -O0 locals
  read back from the stack, loads described in ROM data (goddard), GCC
  jump tables, the code before a segment's first function, -O0's closing
  `jr ra` folded, function addresses anywhere in the ROM kept live, the
  hardware rules naming only a sole candidate (`osAiSetFrequency` had
  named Paper Mario's `__osInitialize_common`), the TLB writers,
  `osCreatePiManager` no longer taken by `osCreateScheduler`.
- Super Mario 64 (US) from its ROM alone, `--fast`, xemu slot 9: boots,
  sound, the title logo drawn; 2 hand-written routines interpreted before
  the TLB names (now none). `[MEM] boot free 56768 KB of 65536 KB`;
  `[CLOCK] cpu 733 MHz (TSC against the ACPI timer)` (xemu).

**Not verified / gaps**

- Super Mario 64 past the logo: goddard calls `osMapTLB(0, .., 0x04000000,
  ..)` and reads its dynlists through that mapping; the runtime has no TLB
  (`ultra/more.c` ignores it, DIVERGES) and the read faults (access
  violation at a host address, `[CRASH] read of address 07a31650`). Needs
  KUSEG translation in the runtime's memory model.
- Overlays sharing a vram (Paper Mario's maps and partners), GCC-built
  libultra rules (Paper Mario: osContInit, osSetEventMesg, osEPiStartDma,
  Flash, Pfs); compressed code (Dr. Mario 64's main segment).
### The TLB and more libultra for ROM-only games (2026-10-16, branch `agent/tlb`)

**Done**

- `xbox/src/rt/tlb.c` (new): the 32 TLB entries; a valid entry's pages are
  aliased at the host address the generated code computes (`rdram + V` =
  P + V, user space) by `xbox/src/hw/xhw_alias.c` (new, nxdk triple): it
  reserves the 4 MB regions from the kernel, makes page tables, writes PTEs
  onto the window's frames. `-DXRC_TLB=0` keeps the entries, aliases
  nothing. `docs/decisions.md` "The TLB" has the options weighed.
- `interp.c`: tlbwi/tlbwr/tlbr/tlbp go to `tlb.c` (osMapTLB code the
  recompiler refuses runs there). `ultra/more2.c` (new): osMapTLB,
  osUnmapTLB, osUnmapTLBAll, osSetTLBASID, osMapTLBRdb, __osProbeTLB, the
  __osGetTLB* readers, __osEnqueueAndYield (a 1 ms pause with the interrupt
  lock let go, `thread.c`'s new `xu_pause_unlocked`). `ultra/misc.c`:
  osVirtualToPhysical translates TLB addresses.
- `libultra.py`: `_tlb` rule (osMapTLB, osUnmapTLB, osUnmapTLBAll,
  osMapTLBRdb by their COP0 writes and arguments; osYieldThread by its three
  calls). PW64's osMapTLBRdb `80243650` matches the decomp's symbols.
- Host: `test_interp.py` (a TLB case: map, tlbp, tlbr, translate, unmap,
  the page-table side recorded), `test_n64rx.py`, `test_time.py` pass.
- xemu (emulator only, slot 14): Super Mario 64 from its ROM alone, three
  `--learn` rounds: the title's head, the interactive head screen, the
  castle grounds' Lakitu intro (`[TLB] 0: 04000000-0400ffff -> 00390000`
  ...). Dr. Mario 64 links and boots (see its row in `docs/analyzer.md`).
- `[MEM]`/`[CLOCK]`: not re-measured on a console; xemu: free 25,288 KB
  in SM64's level (each aliased 4 MB region costs one 4 KB page table).

**Not verified / gaps**

- No console run: the page-table writes are checked in xemu only (QEMU's
  MMU). The kernel's reaction to PTEs it did not write, inside a region it
  reserved for us, is assumed benign (it never commits there).
- The ASID is ignored; KSEG2/3 mappings and code linked at TLB addresses
  (not in the function table) are not supported; a host range already in
  use is logged `not aliased` and faults.
- SM64: the head's shading is wrong (graphics); later rounds still log
  `[LEARN]` lines (`out-sm64/serial5.log` holds 37 more).
- Dr. Mario 64: interpreted code reads PI registers through KSEG1; its main
  code segment is not found or learnt.
### The BACK menu's Paks page (2026-10-16, branch `agent/pakmenu`)

**Verified**

- Host: `test_saves.py` (245 checks): the note table read from an image
  (names in the pak's character set, extensions, the code and company,
  pages from the inode chain, free pages), a delete as osPfsDeleteFile does
  (pages free, the inode sum and mirror; libultra's checker then finds
  nothing to fix; osPfsFreeBlocks agrees), an unused entry refused, the
  status's pull bit once (and kept while the port is empty),
  `__osPfsGetStatus`'s `PFS_ERR_NEW_PACK` once, a format with a new ID
  making the old OSPfs stale (`PFS_ERR_NEW_PACK`) until osPfsInitPak; a
  note copied to another pak (its pages' bytes in order, the target's sum
  and mirror; already there: -2; a full pak: -3).
  `test_settings.py` (19): `[portN] pak`, `pak_file` round trips, a lower
  case letter, a pak in two ports falling back to A-D. `test_splash.py`
  (40): the 11-row card, the Paks page's rows, info and note fit; previews
  `docs/look/menu-card-host.png` (updated), `docs/look/paks-page-host.png`.
- xemu (slot 13, emulator only), Mario Kart 64 from its ROM alone
  (`--fast`, `-DXHW_AUTOPAD=1`), `tools/xbox/scenarios/pak_page`: the menu
  opens over the title menus, "Controller Pak" opens the Paks page (Pak A,
  no notes, 123 pages free), Pak B and back (`[SAVE] port 1: Pak B, port 2:
  Pak A`, the files loaded in turn), Format pak with its confirm, the file
  written by the save writer, the menu closes. Then `pak_boot`: the game's
  own "N64 Controller Pak Data" screen shows the formatted pak (no notes,
  123 pages free).

**Not verified / gaps**

- Delete and copy on a real note in a game: MK64 makes its note only for a ghost in
  Time Trials (a race); the host test covers delete. No game was watched
  re-reading after a mid-game swap (the pull bit and ID check are tested on
  the host only).
- The Rumble Pak / Nothing choices ran in xemu only as rows; no game that
  probes rumble was booted.
- Not on the console. `[MEM]`: +2 KB static (the note table, the page's
  state); `[CLOCK]`: unchanged.
- A pak shared across games (stretch) is not built.
### The sweep: every ROM in the library (2026-10-16, branch `agent/breadth`)

**Done.** `tools/xbox/sweep.py` builds every ROM in a folder (one per game,
5 at a time, a timeout each) and boots each XBE in xemu (`--run`); results
stay in `build-host/sweep/<tag>/` (`docs/testing.md`). Fixed what was not
the analyser's: weak bodies for libultra internals nothing provides
(`gen/xrc_missing.c` -> `xbox/src/rt/ultra/more3.c`, which also has real
`__osSpSet/GetStatus`, `__osSetCompare`, `__osViSwapContext`, the signed
64-bit conversions), loads into `$zero` rewritten after the recompile,
`floorf`/`ceilf` in `recomp.h`, ASCII titles from Shift-JIS headers, an
apostrophe in the title breaking cxbe's post-build line, n64rx's output on a
cp1252 pipe (`docs/decisions.md`).

**Verified how.** Two sweeps of 246 games: XBE built 54 -> 143, link
failures 78 -> 0 (table in `docs/analyzer.md`). `--run 45` over the 143 in
xemu (slot 11): 17 present frames, 75 stop on RCP register access through
KSEG1, 23 on a TLB-mapped address. Host tests: `test_n64rx.py` (new: the
Shift-JIS title, the `$zero` fixup, the weak bodies), `test_recomp_h.py`.

**Not verified.** No console run. xemu only, and no game judged beyond
"frames presented". The weak bodies' runtime path (`[ULTRA] missing`) never
fired in the 45 s runs; `__osEnqueueAndYield` (66 games) waits for the
`tlb` agent's body.

**Gaps carried on.** For the analyser: 71 entry-code and 14 boot-segment
refusals, 12 function-bounds compile errors, 5 entry points jumping into SP
IMEM, 1 `mtc0 EPC`; unnamed libultra versions that touch the RCP registers
(75 runtime stops). For `tlb`: 23 KUSEG stops. Next: name more libultra
versions (`osCartRomInit`, the raw PI/SI/SP I/O) or trap KSEG1 registers.
`[MEM]`/`[CLOCK]` (xemu, Ogre Battle 64): 64 MB console, 34368 KB free after
nv2a with a 40 MB ROM on the page cache; cpu 734 MHz (ACPI), TSC unused.

### Mario Kart 64 on the console (red, `vmk2` then `vmk4`)

ThinLTO release builds from the ROM alone, the ROM inside `default.xbe`, in
`F:\Applications\N64Recomp-X-MK64\`, title ID `4E520DF7`.

**Verified (console)**

- `vmk2` (commit `ec17492`): `[ROM] $ROM in D:\default.xbe at 0x324000,
  12582912 bytes, not preloaded`, read in 686 ms, SHA-1 in 1582 ms (the
  ROM inside the XBE works on the retail kernel). Races at 29.9 fps,
  display lists 2-3 ms, audio 2.5-3.5 ms a frame at 59 tasks a second, the
  rest idle; the maintainer: "game is stable, sound works". The BACK menu
  opened (the game froze and dimmed) but showed no card: 141 failed
  texture-pool allocations, the 1 MB card had no room.
- The fix (commit `1545bc3`, `-DXGFX_HOLD_EVICT`): the menu drops the
  interpreter's texture cache when the card or the held frame finds no
  room. xemu over a race with 788 KB free: the card drawn, the race's
  textures back after closing.
- `vmk4` (commit `408d0b3`, + the Paks page): `[GFX] menu card: texture
  pool full, the game's texture cache dropped (card shown)`; the Paks page
  opened on port 1; Exit to dashboard from the menu named by
  `lastexit.txt`; EEPROM (512 bytes) and `nkte.mpk` loaded; mostly 29.9 fps
  (the slower seconds and 500-690 ms frames are the boot's and menus'
  loads). The maintainer: "worked fine".
- `[MEM]` 21.3 MB free after nv2a; `[CLOCK] cpu 733 MHz`.

**Not verified / gaps**

- The map screen's framebuffer effect is wrong (agent `fbfx`); drift boost
  unconfirmed. MK64 rewrites `nkte.mpk` at every boot (probably the game
  repairing the pak ID it reads); the menu's card texture is rebuilt every
  frame it shows (the `[NV2A]` texture counter inflates).
### Pokémon Snap from its ROM alone, overlays sharing a vram (agent `snap`, 2026-10-16)

Verified: `test_analyze.py` (6 pairs with Snap: 0 wrong libultra names, floors held); `test_n64rx.py`, `test_interp.py` pass;
Snap builds (`--fast`), 32 segments, 3793 functions recompiled; in xemu
(slot 14, emulator only) its intro scene runs (F3DEX2 identified, aspMain
audio, FlashRAM 1 Mbit save found), the overlay switch to the main menu
works (`[OVL] 800e18a0: ROM a08e30 mapped`), then it faults reading NULL+0xe
in func_80007F24 under omCreateProcess (main menu's func_800E4AF8: its
omAddGObj seems to return NULL). MK64 ROM-only and PW64 (its pack) build and draw in xemu, no fault, no stall.
Not verified: SM64 boot after these changes, real hardware, the
title screen and Beach. `[MEM]`/`[CLOCK]`: xemu 64 MB, 733 MHz (not hardware).

### Framebuffer effects (agent fbfx, 2026-10-16; xemu only)

Built behind `-DXGFX_FB_READBACK=1` (default 0): the CPU-read frame path for
Mario Kart 64's big screens (docs/renderer.md "Framebuffer effects").
Verified: host tests (`test_fbfx.py`, `test_f3dex.py` test_readback); in
xemu, MK64's race touches its frames and the readback runs; an RDRAM dump
(`-DXGFX_FB_DUMP`) of the N64 logo frame is correct. Not verified: the big
screen on screen (no scripted shot reached it), any console run, 720p.
Gap carried on: Pilotwings 64 stays hot and slows to ~14 fps in xemu with
it on; render to texture in the same frame is not done.
