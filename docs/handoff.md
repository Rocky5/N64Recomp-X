# Handoff

One entry per milestone: what was verified, how, and where ("xemu" results
are emulator-only; timing, audio, memory and HDD latency are judged on the
console).

## Where things stand (2026-10-17)

The newest state first; the entries below are the record, oldest first.
"xemu" is the emulator (slot runs, test builds); "red" is the maintainer's
console. Everything since `vmk4`/`vsnap1` is xemu only.

| game (US) | how built | xemu | console (red) |
|---|---|---|---|
| Pilotwings 64 | its pack, or the ROM alone (`--analyze`): the same | title to results, saves, sound, 720p + hor+, the BACK menu | `vnrx1`: plays, sound, EEPROM saves, the BACK menu, the combos; 53-59 fps |
| Mario Kart 64 | the ROM alone | title, menus, races (F3DEX), sound (nead), Controller Pak, 720p 16:9 | `vmk2`/`vmk4`: races at 29.9 fps (display lists 2-3 ms), sound, the ROM inside the XBE, the BACK menu and its Paks page, EEPROM and pak files; the map screen's TV (a framebuffer read) wrong |
| Super Smash Bros. | the ROM alone | intro, title, 1P character select, a 1P match (Link vs Link, Hyrule Castle) at 47-50 fps with scripted input | - |
| Mario Party | the ROM alone | Hudson logo, the intro's 3D scene, the story text at ~58 fps, audio tasks (53 a second); its processes run as coroutines | - |
| Mario Party 2 / 3 | the ROM alone | 60 fps from boot (intro, menus); 2: the audio thread dies some time after the game restarts it (open) | - |
| Super Mario 64 | the ROM alone + three `--learn` rounds | head screen, Lakitu intro, castle grounds; head shading off | not tried |
| Pokémon Snap | the ROM alone (overlays sharing a vram) | intro and main menu (s7: 1576 frames in 30 s) | `vsnap1` (before the thread-stack fix): the crash xemu had then |
| Banjo-Kazooie | the ROM alone + ~22 `--learn` rounds | title, file select, the new-game intro, Banjo's house (~28 fps); the sky black (open) | - |
| Blast Corps | the ROM alone + 15 `--learn` rounds | Rare logo, title, profile screen, the attract demo (50-56 fps) | - |
| Ocarina of Time 1.2 | the ROM alone, Yaz0 inflated (53 MB, page cache) | title, file select, name entry, a new file, the intro text, Kokiri Forest's HUD; the 3D world dark (open), no audio | - |
| Ridge Racer 64 | the ROM alone + one `--learn` round | F3DEX2; 1732 frames in 30 s (~44 fps) | - |
| Donkey Kong 64 | the ROM alone + `--learn` rounds | F3DEX2, ~790 frames, then its main thread runs on without sending tasks (open) | - |
| Diddy Kong Racing, Wave Race 64, WWF No Mercy, Dr. Mario 64 | the ROM alone | present frames for the whole 30 s sweep run (DKR's microcode not decoded: grey boxes) | - |
| Paper Mario, GoldenEye, Conker, Jet Force Gemini, Castlevania | the ROM alone + `--learn` rounds | boot and stop (each in the 2026-10-17 entry below) | - |
| the library (246 games) | the ROM alone (`tools/xbox/sweep.py`) | s7: 245 build, 139 present frames, 121 still presenting at 30 s (`docs/analyzer.md`) | - |

**What exists** (each piece with host tests and a kill switch; the entries
below say how each was verified): the ROM inside the XBE and its page cache;
Yaz0 games inflated at build time; the ROM analyser (entry shapes, segments,
libultra by behaviour, the N64Recomp precheck) and the learning loop
(`--learn`: the safety net's functions, DMAs, code captured from RDRAM and
checked against it by `guard.c`, TLB regions); the game's own TLB (aliases,
misses through its exception code and fault thread, KSEG2/3 through
`XRC_HIGH_TLB`); overlays sharing a vram; the RCP register model through
KSEG1; F3DEX/F3DEX2 and the microcode registries; audio HLE (aspMain, nead,
`n_aspMain`); every save type and the BACK menu's Paks page; coroutines for
a game's own setjmp/longjmp; task done events held for their thread; threads
stopped by others; the library sweep and the V track's `decomp_check.py`
(0 wrong libultra names on Paper Mario and Mario Kart 64). Diagnosis:
`-DXRC_WATCH` (a hardware write watch), `XRC_WATCH_QUEUE`/`XRC_WATCH_RECV`,
`N64RX_TRACE=1` (per-thread traces in crashes and stalls), the stall report,
`[INTERP] hot`.

**Open, roughly by how much they block** (details in the entries below and
`docs/analyzer.md`):

1. From the first console runs of `vssb1`/`vmp1`/`vbk1`/`vmk5`: Smash's
   blurry textures, Mario Kart 64's drift boosts; Mario Party's flicker and
   Banjo-Kazooie's near/far clipping fixed in xemu, the console to confirm
   (the last entry below).
2. The s7 categories: NULL pointers through the TLB path (42 games), Rare's
   compressed boots before learning rounds (18), no frames (42).
3. Per game: Ocarina of Time's world and audio; Paper Mario's copy-protection
   chain and its flash code (no `osFlash*` rule); GoldenEye's wild jump in
   paged code; Jet Force Gemini's init reached only through decompressed
   data; Donkey Kong 64's main thread; Mario Party 2's audio thread;
   Castlevania's dispatch after a TLB miss.
4. The 13 entries that are their own loaders (Acclaim's Turok 1-3,
   Armorines, All-Star Baseball; Army Men; two Star Wars); gzip code.
5. Renderer: S2DEX's own commands, F3DDKR, framebuffer effects by default
   (MK64's TV, Snap's photos); MusyX and the other audio microcodes.
6. M2's leftovers (720p on the console, the 30-minute soak, Pilotwings'
   pack README); M3's measurement gate.

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

### Pokémon Snap on the console, and its stop (2026-10-16, orchestrator)

`vsnap1` (commit `1361903`, ThinLTO, the ROM inside,
`F:\Applications\N64Recomp-X-SNAP\`, title ID `4E520FB8`) crashes on red
exactly where xemu does: frame ~100, a read of RDRAM `0x8000000E`, right
after the main-menu overlay (ROM `a08e30`) is mapped; the intro, F3DEX2,
aspMain at 32 kHz and FlashRAM seen on the way. Traced in xemu
(`-DXRC_THREAD_TRACE=1`; a small map reader, since `tools/xbox/sym.py`
misreads this map format): object thread `10000004` (`func_800E455C`) ->
`func_800E4AF8` -> `omCreateProcess` -> `omGetStackOfSize`
(`func_80007F24`) walks the object manager's free list of thread stacks and
follows a corrupt link (`0x8000000E`). The manager (thread 4, N64 priority
110) and the object threads (51) share kernel level 10; the
create/start/stop/destroy order in the trace looked right, and turning the
kernel's wait boost off (`-DXHW_THREAD_NOBOOST=1`) changed nothing. Next:
find who writes that free list (a write watch on its head, or running the
om functions in the interpreter), and compare the recompiled overlay's
functions with the decomp's.

### The RCP's registers through KSEG1 (2026-10-16, branch `agent/mmio`)

**Done.** KSEG1 RDRAM is the window aliased again at host P + 0xA0000000
(`mem.c`, `xhw_alias.c`; `-DXRC_KSEG1=0`). The RCP's registers, the PIF and
the cartridge past RDRAM are a register model (`xbox/src/rt/mmio.c`;
`-DXRC_MMIO=0`): the analyser sends live, unreplaced functions with a
constant hardware address to the interpreter (`emit.hw_funcs`, used by
`romonly.py`; `N64RX_MMIO_INTERP=0`), whose KSEG1 accesses go to the model;
PI/SI/AI/VI/SP writes act through the runtime's own paths. Each register
block's page is a page of ours aliased at its host address
(`xhw_alias_page`) so recompiled code reads the idle machine; the VI thread
refreshes the status words (`ultra/vi.c`, one line).

**Verified how.** Host: `test_mmio.py` (new; 64- and 32-bit registers),
`test_interp.py`, `test_n64rx.py`. xemu (slot 13, 45 s each, not
hardware): 18 games of the sweep's KSEG1 categories rebuilt from the ROM
alone: none stops on KSEG1; 4 present frames (Bottom of the 9th 1163; three
show one frame), the others stop further on (`docs/analyzer.md`). Mario Kart
64 and Pilotwings 64 still present frames (1165, 1616). The whole category
(85 games) is in `build-host/sweep/mm2/` when its run finishes.

**Not verified.** No console run: whether the kernel ever hands out
0xA0000000 + P or the PIF page's 0xC0000000+ range on hardware is unproven
(each mapping is checked free first; refused ones are logged). The SP task
by DMEM, AI by registers and VI_ORIGIN paths are host-tested only; no game
in the 18 was seen using them on screen.

The 85-game run built 83: 64 Oozumou 2 and Ridge Racer 64 now fail to
compile (`use of undeclared label`), probably because a function sent to
the interpreter is left out of the recompile while a neighbour branches
into it; unconfirmed (check with `N64RX_MMIO_INTERP=0`).

**Gaps carried on.** `eret` in safety-net code (the game's own thread
switch; 4 of 18); null-ish KUSEG reads (4); a recompiled store to a
register still has no effect (only the interpreter's do). The Expansion
Pak probe (`0xA0400000`: Zelda, Hybrid Heaven) now reads 0 instead of
faulting, so those games see 4 MB: their packs need `rdram_mb = 8`.

### The orchestrator's solo work after the agents (2026-10-16, evening)

Done on `dev`, each with a kill switch, host tests passing, Mario Kart 64
and Pilotwings 64 rebuilt and booted in xemu after the runtime changes:

- **Yaz0** (`n64rx_lib/yaz0.py`, `test_yaz0.py`): the Zelda DMA table found by
  behaviour, every file inflated to its vrom, the table rewritten. Ocarina of
  Time 1.2 builds from its ROM alone (53 MB, page cache) and boots in xemu.
- **The analyser**: entries that clear from a start to an end, call main with
  jal, clear empty ranges, or set the stack and jump at once (245 of 258
  distinct games' entries parse, from 184); jal-main entries end after the
  call; functions that branch into another's body or to code no function
  holds are interpreted (Mario Party, 64 Oozumou 2, Ridge Racer 64,
  Forsaken 64 compile); the later `osSetEventMesg` (with PRENMI) and the
  `osCreatePiManager` that calls it (Ridge Racer 64 ran once named).
- **The runtime**: threads start at `sp - 16` (Pokémon Snap past its main
  menu's load; found with the new `-DXRC_WATCH` hardware write watch);
  `MEM_WU` in `recomp.h`; the stall report also for games that never send a
  graphics task; the mmio agent's register model takes over snap's shadow
  pages (the RSP memories' boot values kept).
- **Not verified on a console**: everything in this entry.

### Rare's games and code the TLB maps (2026-10-16, night, orchestrator)

The maintainer: "Rare games should be a focus"; "focus on games people
actually wanna play".

- **Code captured from RDRAM** (`docs/analyzer.md`, the learning loop's
  step 4) and the game's own libultra copies (`<name>__2`). Banjo-Kazooie
  (xemu, `build-host/learnloop.sh`): nine rounds to a fixed point, 4838
  functions, audio (`n_aspMain`); stops on a heap block reused while still
  listed in `sSpawnableActorList` (found with `-DXRC_WATCH`; the decomp,
  `build-host/decomps/banjo`, names the functions by their addresses).
  Threads now start with libultra's Status (Banjo halts on purpose
  without IM4).
- **Code a game maps with its own TLB**: the function table is two-level
  over the whole address space; the runtime's RDRAM helpers translate a
  KUSEG address through the TLB (physical addresses are passed as KSEG0);
  the safety net and the capture take TLB-mapped code; `osUnmapTLBAll`
  keeps entry 31 (libultra's NTLBENTRIES); a refused entry is interpreted
  through `xrc_entry_interp` with no entrypoint given to N64Recomp. Turok 2
  runs its main and stops past the 2 MB its TLB entry maps; Conker
  (0x10000000) and GoldenEye (0x70000000) build, map their code and stop
  on its interpreted libultra (an eret), the learning loop's next rounds.
- **Analyser**: entries that jr to a loader or call main before a break;
  the TLB template first; the entry and main never named libultra; the
  memory-size probes (KSEG1 RDRAM) interpreted; osGetMemSize,
  osGetThreadId/osGetThreadPri, the later osSetEventMesg and
  osCreatePiManager named; a function's trailing delay slot kept; the
  jump-table addend N64Recomp declares twice deduplicated.
- **Diagnosis**: `N64RX_TRACE=1` (N64Recomp's trace_mode; the last 64
  functions, recompiled by name and interpreted by address, logged with a
  crash or a stall), `-DXRC_WATCH=<address>` (x86 DR0 write watch).
- **Games**: Ocarina of Time gets past its probes and presents 2 frames,
  then its threads all wait (the IRQ manager's retrace queue empty: open);
  F-Zero X stalls with no frames; Diddy Kong Racing runs but draws grey
  boxes (its F3DDKR microcode is not decoded: renderer work); Ridge Racer
  64 draws (F3DEX2) but takes no A press.
- Not verified on a console: everything in this entry.

### Rare's games, second pass (2026-10-16, night)

All xemu; nothing new on the console.

- **Banjo-Kazooie**: the heap corruption was the register width: the
  64-bit scan read the ROM without the captured code, so the build had
  32-bit registers and `bkmemcpy64` (ld/sd) broke every `bk_realloc`.
  osCicId is now the chip's (6103: its model code checks it). 7032
  functions; it now crashes in `anSeq_update` on an actor whose animation
  sequence pointer holds code bytes (open; the crash report now carries
  the N64 registers).
- **Conker**: osCicId 6105 starts its main thread; `__osRunningThread` kept
  in RDRAM; N64Recomp's segfault (a jump table outside the function's
  segment) avoided by the precheck; its code at 0x15000000 pages in
  through its own `__osException` handler on a TLB miss (`tlb.c`), and is
  captured a page at a time. Stalls before its first graphics task (the
  learning loop goes on).
- **GoldenEye**: its refill vector and its fault thread (`crashMain`) map
  0x7F000000 pages (xemu aliased them at the default layout); then a read
  of 0x00002024 (a NULL pointer, open).
- **Donkey Kong 64**: the 6105 boot magic at 0x2FE1C0, 8 MB, into its main
  code; then a captured block's stale bytes (a `syscall`): captured code
  needs a content check (open, `docs/analyzer.md`).
- **Blast Corps** presents frames (clears only) and stalls; **Jet Force
  Gemini** no longer writes through NULL (running thread), stalls before
  its first graphics task.
- Not verified on a console: everything in this entry; whether the kernel
  map at a KUSEG host address can be aliased on a 64 MB console (xemu runs
  128 MB).

## 2026-10-17: ROM-only games in xemu (orchestrator)

All xemu, test builds, on slots 9-12; nothing new on the console this day.
`[MEM]`/`[CLOCK]`: not recorded (xemu).

### Games

- **Banjo-Kazooie** (~22 `--learn` rounds): its intro with Banjo and
  Kazooie drawn (F3DEX 1.21), the title screen (the logo, Banjo dancing;
  30 fps), the file select in Banjo's house ("Game 1: Empty", 28 fps), the
  new-game intro in the lair, then Banjo's house. The sky draws black
  (open). What it took: calls into captured code are weak starts
  (`funcs.discover weak_calls`: Banjo's core code jals into its level
  overlays at addresses that are mid-function in the captured cutscene
  overlay; found by interpreting halves of the region,
  `N64RX_CAPTURED_INTERP=<lo>-<hi>`); captured code checked before it runs
  (`guard.c`); faces textured (a tile inside a larger LOADBLOCK); the log
  lock's holder above the game's threads (`XHW_LOG_BOOST`).
- **Blast Corps** (15 rounds): the Rare logo, the title, the profile
  screen ("Amber"; its portrait frame empty: a framebuffer effect), the
  attract story text and demo at 50-56 fps. It had asserted on a DP done
  with no RDP task: DP done now only for a list that ends with a FullSync
  (`XRC_DP_ON_FULLSYNC`), done events held until the starting thread
  blocks (`XRC_RCP_DEFER`). Its glyphs were stale (T for G: reused 512-byte
  slots): textures up to 2 KB are hashed whole.
- **Super Smash Bros.** (no rounds): intro, title, 1P character select, and
  a 1P match on Hyrule Castle (Link against a CPU Link) at 47-50 fps; the
  scripted stick moves Link. "A does not drop the token" had been the
  script: the game reads no pads while it loads, a screenshot over COM1
  freezes it for seconds, and a second A picks the token up again
  (`docs/testing.md`).
- **Mario Party 1-3** (no rounds): Mario Party: the Hudson logo, the
  intro's 3D scene, the story text, 58-59 fps, 53 audio tasks a second,
  three 60 s runs without a freeze. Its game objects are Hudson processes
  that switch stacks with the game's own setjmp/longjmp: n64rx finds the
  pair by shape (the same in all three), the runtime runs each context as
  a coroutine on a host thread of its own (`strand.c`). Mario Party 2: 60
  fps; it destroys and remakes its audio thread, which now stops when
  destroyed and restarts, then dies later on a stale message pointer in
  one run of two (open; the game keeps presenting). Mario Party 3: 3377
  frames in 35 s at 59 fps, after the overlay precheck and its
  development-board probe at `0xBFF00000` ("KMC", read as 0 by the model).
- **Donkey Kong 64**: 8 MB, the 6105 boot magic, captured code and its own
  osViGetCurrentMode named: F3DEX2, ~790 frames to 42 s, then its main
  thread runs on inside recompiled `func_8063A968` (called from captured
  code) without blocking (open).
- **Ocarina of Time**: the title demo (Link riding in Hyrule Field) and
  the logo at ~25 fps; "NO CONTROLLER" until the controller-query names
  were fixed (below). After START the graph thread waits on its stack
  queue `0x8011F9B4` for the frame's task-done message (`0x29A`, every
  frame until then) and it never comes (open: the scheduler's framebuffer
  and swap state next; its moon also draws as a flat rectangle).
- **Paper Mario**: its effects run at `0xE0200000` through the TLB; KSEG2/3
  cannot be aliased on the host, so a learnt build sets `XRC_HIGH_TLB`.
  With it the boot goes on into its copy-protection chain (code at
  `0x80200000`: `create_audio_system_obfuscated`, `load_obfuscation_shims`,
  a hash over ROM words read through the PI picking a jump target) and
  stores to address 0 (open). Its flash code runs interpreted (the runtime
  has `osFlash*`; the analyser has no rule for them yet).
- **GoldenEye** (open): boots into `bossInitMainthreadData` (interpreted:
  it calls the paged `0x7F` code); the crashing thread's last entry is the
  safety net at `0x7F0CFF30` (never captured), from where an untraced jump
  reaches `0x7F5C40EC`, past the game segment.
- **Jet Force Gemini** (open): its main thread writes through the pointer
  at `0x80102DB0`, still NULL; only `func_8006B8C4` sets it
  (`malloc(0x580)`) and nothing in the ROM calls it or holds its address:
  the call comes through a table in decompressed data.
- **Castlevania** (open): 27 frames, then garbage after the dispatch
  `eret` in its own `__osException` run for a TLB miss.

### The runtime, for every game

- **Done events** (`sp.c`, `thread.c`, decisions.md): a thread that flushed
  its held SP done into the queue it was about to wait on slept anyway
  (the check came first); and any thread's block posted every thread's
  events, so nnsched's graphics thread took the audio task's done. Now
  the events belong to the starting thread, and a flush goes back to the
  caller's check unless a thread that blocked there first at its priority
  or above takes the message (`XRC_RCP_OWNER`).
- **Threads stopped by others** stop at their next block and restart when
  made again (`XRC_THREAD_HALT`). Pokémon Snap, Mario Kart 64 and Super
  Mario 64 run as before.
- **KSEG2/3 through the TLB** (`XRC_HIGH_TLB`, learnt), **coroutines**
  (`N64RX_STRANDS`).
- Checked after these: Mario Kart 64 (30 fps), Blast Corps (59), Super
  Mario 64 (30), Smash (42), Banjo-Kazooie (19), Pokémon Snap: present
  frames as before, 45 s each.

### The analyser and n64rx

- The precheck missed every overlay function (it looked them up by vram
  while `live` keys them with their ROM offset): Mario Party 3's boards
  call where no section has a function, and N64Recomp refused them one a
  run until n64rx died on a missing `funcs.h`. Now 1639 are interpreted up
  front; the 400-run limit ends in an error that says what to do.
- osContInit is named before the read/query split (Ocarina of Time's
  osContStartQuery and osContStartReadData had come out swapped), and
  osContGetQuery is named. `decomp_check.py`: Paper Mario 70 right,
  Mario Kart 64 67 right, 0 wrong.
- A missing `tools/recomp` stops n64rx with what to do (it had written a
  config N64Recomp refused); a relative `N64RX_BUILD_ROOT` is made
  absolute.

### The sweep (s7)

Built in a git worktree pinned to `fbe2fa1` (`docs/testing.md`): 245 of
246 build, 139 present frames, 121 still presenting at 30 s. s6, built from
a working tree that changed under it, lost 30 games to half-made states and
is not comparable. Tables in `docs/analyzer.md`.

### Not verified

On a console: everything in this section. In xemu: the sound itself
(tasks run; not listened to), Mario Party past its story text, Smash past
the first seconds of a match. xemu slot 10 sometimes stalls reading the
ROM at boot (the same XBE boots on another slot): an emulator matter.

### On the console (2026-10-17, commit `e9c907c`, deployed to red, not yet run)

Release builds (ThinLTO, `-O2`, no test flags), the ROM inside each XBE,
staged in `C:/xemu/hw/stage-<name>` with the map at
`C:/xemu/hw/n64recomp_x.<name>.map`; each booted 30 s in xemu first, no
crash. Deploy each with `MX_HW=/c/xemu/hw MX_FTP_HOST=192.168.158.113
MX_TITLE_ID=<id> MX_APP=<folder> python tools/xbox/console.py deploy <name>`
(`pull vmk4` first for Mario Kart 64: deploy deletes old logs), then `pull
<name>` after playing.

| build | game | xbe sha1 | title ID / folder | what to check |
|---|---|---|---|---|
| `vmk5` | Mario Kart 64 | c3f3894592e2 | 4E520DF7, `/F/Applications/N64Recomp-X-MK64` | regression after the done-event and thread changes: a GP race still 29.9 fps, sound, BACK menu + Paks page, EEPROM/pak saves |
| `vssb1` | Super Smash Bros. | 71aba9fe63b4 | 4E520101, `/F/Applications/N64Recomp-X-SSB` | first console run: intro, menus, a 1P match and a 4-player VS (fps, sound, all four pads), SRAM save of the settings |
| `vmp1` | Mario Party | 25a187ec999f | 4E52376A, `/F/Applications/N64Recomp-X-MP` | first console run of the coroutines (a host thread per process on the real kernel): intro, a board turn, a mini-game, fps, sound, EEPROM save |
| `vbk1` | Banjo-Kazooie | 8d1bae170588 | 4E52E3F7, `/F/Applications/N64Recomp-X-BK` | first Rare game on the console (~22 learning rounds, captured code, the guard): title, file select, a new game into Spiral Mountain, fps, the sky |

From each `boot.log`: the `[MEM]` and `[CLOCK]` lines, `[PERF]`, any
`[CRASH]`/card, and whether the 64 MB budget holds (free KB in `[BEAT]`).

**Console results (red, 2026-10-17; the maintainer's play and the pulled
logs in `C:/xemu/hw/logs<name>`).** All four: `[CLOCK]` 733 MHz (TSC
against ACPI), 64 MB console, nothing held; the ROM verified by SHA-1.

| build | maintainer | log |
|---|---|---|
| `vmk5` | the same as `vmk4`; drift boosts (mini-turbos) seem not to work (open: the stick's wiggle through the dead zone, or the boost itself) | races 29.9 fps (display lists 2.6-5 ms, longest frame 34-38 ms), nead audio 59 tasks/s, free 19 MB; quit with L+R+BACK+BLACK |
| `vssb1` | plays well; textures look blurry (open: filtering or LOD) | **59.9 fps** (display lists 2.4-2.8 ms, game logic 12.7 ms), `n_aspMain` 59 tasks/s, free 12.5 MB; quit with L+R+BACK+BLACK |
| `vmp1` | plays into the intro; flickers so badly it was switched off (open: console only, xemu showed no flicker; the swap/present timing first) | 60 fps, present 12.7-15.3 ms in GPU and flip waits, page cache 4 MB held, free 23.5 MB; the log ends at 30 s (power off) |
| `vbk1` | works; serious near and far clipping (open: the depth range and clip planes) | 30 fps, dips to 5 fps on loads (one 5.6 s frame), free 13 MB; the log ends at 75 s |

No crash, card or stall in any log.

## 2026-10-17: the console's first results, from the decomps

Work on the games with a decomp to read (`build-host/decomps`), starting
from the console results above.

- **Mario Party's flicker**: the decomp's swap-chain thread
  (`src/engine/graphics.c`) draws one frame in several graphics tasks and
  swaps once; the interpreter presented at every task's end, so every
  other picture was part of a frame on black. Frames are now presented at
  the game's swap (`docs/decisions.md` "Frames presented at the swap",
  `XRC_PRESENT_ON_SWAP`). xemu, test builds, 60 s each: Mario Party 29-30
  presents a second (one a frame; 60 before, two half frames), its odd-frame
  log down to 1 line; Mario Kart 64 and Smash unchanged (30 and 53-59 fps).
  Host tests `test_f3dex`, `test_fbfx` pass. Console: not yet.
- **Banjo-Kazooie's near/far clipping and its black sky**: the decomp's
  `viewport.c` forces near 750 and far 1250 unless RDRAM 0x1D8 holds a word
  of the IPL3, which the console leaves there (the IPL3 copies its last part
  to RDRAM 0 and runs it). The runtime now puts that part of the ROM's own
  IPL3 at RDRAM 0 (`docs/decisions.md` "IPL3's copy of itself",
  `XRC_IPL3_LEFT`). xemu, test build: `[GFX] perspective` (a new test-build
  line: each loaded perspective's near and far, once) now gives the
  decomp's values (sky 5/15609, the world far/128, the transition
  63.6/500) instead of 750/1250; the lair's walls behind Gruntilda draw
  (black before); triangles rejected beyond far in a 600-frame window 0
  (694 of 789 before). The `[GFX] frame` line now splits rejected and
  clipped triangles by far and near. Console: not yet.
- **Donkey Kong 64** checks the same IPL3 copy (the decomp's `heap.c`:
  0xA00002E8 must hold 0xC86E2000, else its timed objects are never freed);
  the 6105's copy lands at RDRAM 4, which the boot now reads from the IPL3
  too. xemu, test build, 90 s: still presenting at 80 s (1684 frames; it
  stopped near 790 before), no audio tasks (MusyX, open). Paper Mario and
  Jet Force Gemini crash writing RDRAM 0 as in their earlier learning runs
  (unchanged).
- **Mario Kart 64's drift boosts** (the decomp's `player_controller.c`,
  read, nothing changed yet): a spark level needs the stick past -10/+10
  after MK64's own dead zone (12 of the raw value, then clamped to 53) for
  18 game frames, then back; two levels and R released give the boost.
  The port maps full tilt to 80 after its own 20 % radial dead zone.

**On the console (deployed to red 2026-10-17, the code of `5803b8b`,
ThinLTO release, the ROM inside; each booted 40 s in xemu first):**

| build | game | xbe sha1 | title ID / folder | what to check |
|---|---|---|---|---|
| `vmp2` | Mario Party | e71ad68e37d5 | 4E52376A, `/F/Applications/N64Recomp-X-MP` | the flicker gone; the intro and a board at an even 30 fps |
| `vbk2` | Banjo-Kazooie | 175d519fb4ac | 4E52E3F7, `/F/Applications/N64Recomp-X-BK` | no near/far clipping; the sky drawn in Spiral Mountain |

**Ocarina of Time (1.2), the same day (xemu, test builds, scripted START
and A presses).** It now gets past START: the file select, the name entry
screen, a new file, the intro's text ("Navi... Navi, where art thou?"),
and Kokiri Forest's HUD (hearts, the minimap, Navi's "Next" button).
Found on the way, each from the decomp:
- Present on swap (above) and the IPL3 copy: after START the next scene
  draws (it never did before; which of the two did it was not split out).
- The game's own `osStopTimer` ran on a timer the runtime's `osSetTimer`
  never put in the game's list, and wrote the graph thread's `gfxCtx`
  over Play's entry in the game-state table: starting the file called
  0xFFF7B4E8. Found with `XRC_WATCH` on that word (now armed on every
  thread) and the new register dump; fixed by an analyser rule
  (`docs/analyzer.md`).
- Its second microcode reads `RSP Gfx[Safe] S2DEX fifo 2.05`; unrecognised,
  its lists ran as Fast3D and the scheduler's task never ended. Read now
  (S2DEX2's own commands are still not drawn).
- `G_RDPSETOTHERMODE` was not handled (about 19000 a 600-frame window).
Open: the 3D world shows dark or flat grey behind the HUD and text (the
traced frame draws the sky, the world with depth on, and the game's own
full-screen dimming fills: whether the result is right is not judged
yet); no audio task at all (its audio thread waits); interpreted overlay
code is hot (up to 11 million instructions a 300-retrace window); runs
with a 640x480 SHOT stop logging in xemu.

