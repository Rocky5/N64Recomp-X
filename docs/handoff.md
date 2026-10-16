# Handoff

One entry per milestone: what was verified, how, and where ("xemu" results
are emulator-only; timing, audio, memory and HDD latency are judged on the
console).

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
  `vmk1`; not yet reported.
- Runtime additions for ROM-only builds: the interpreter, the PIF, the
  `osEPi*` family, rumble, the remaining reimplemented names; `__ll_rem`
  corrected to libultra's unsigned remainder.

**Not verified / gaps**

- Mario Kart 64 past its title: races use F3DEX (the interpreter is Fast3D
  only: M4); its audio microcode is not identified (silent); no Controller
  Pak; the console run.
- The learning loop for code loaded with computed addresses (PLAN.md 3.14
  step 5) is not built.
- The analyser has no host test yet: `test_analyze.py` (synthetic MIPS
  fixtures) and `decomp_check.py` are the V track's first files.
- The ROM inside the XBE: built, see below.

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


## The TLB and more libultra for ROM-only games (2026-10-16, branch `agent/tlb`)

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
