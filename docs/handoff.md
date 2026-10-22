# Handoff

One entry per milestone: what was verified, how, and where ("xemu" results
are emulator-only; timing, audio, memory and HDD latency are judged on the
console).

## Where things stand (2026-10-18)

Each game's tier (Gold fully working, Silver playable, Experimental the
rest; only a console run promotes) is in `docs/compat.md`; a console
result here moves its row there in the same change.

**Newest (2026-10-19, xemu only; nothing of it on red yet):**
- Mario Tennis: intro, menus, character select, matches (~25 fps), the
  attract demo, from the ROM alone plus learning rounds.
- Castlevania: Legacy of Darkness: through to Foggy Lake (~30 fps).
- Acclaim's KUSEG-linked games: All-Star Baseball 2000, 2001 and '99 play
  a game; South Park: Chef's Luv Shack reaches its menus (logo drawn
  wrong); Jeremy McGrath Supercross 2000 draws 5 frames (needs its process
  switcher modelled).
- Ocarina of Time and Majora's Mask recompile the overlays they relocate
  themselves (from the ROM alone, the ones a learning run used): Ocarina
  3137 frames in 120 s against 1950, into the opening cutscenes.
- Runtime: AI busy/full status and its event per buffer and every
  retrace; a blender that never reads the frame draws opaque; the PI event
  after raw DMAs; AUTO paks answer as both; the guard's KUSEG checks by
  what changed; test builds put an 8 MB RDRAM where xemu reads it fast.
- The 12-game regression set presents frames with no crash after every
  change; host tests pass; test_analyze 0 failures.

**Open, in order:** Pokémon Snap's camera lock (the Beach tutorial
waits for three photos with a Pokémon in focus; in xemu the photo check
now finds one and the tutorial prompts "Press Z to aim": next, the three
photos and the unlock shown in a run, then red with a new save); Jeremy
McGrath's process switcher (`strand.c` taught its context save/load and
the return to main); Chef's Luv Shack's logo; the sweep s10's entries
NF2P, NF9E, NFBE run again (a test ran on their slot) and its three
losses; set 15 rebuilt at HEAD before red (it predates all of the above;
red was unreachable all session). The entries at the end
have the details.

**Before that (2026-10-18, late):** the loading screen is the deep-space title
card the maintainer approved on red (`vsm13`: Super Mario 64 through it to
the end of Bob-omb Battlefield, 9 ms a frame, the game at 29.9 fps); the boot holds swapped (L + R safe video, BACK
the BACK menu before the game); the analyser keeps far `j`s inside their
function (six compile failures of s9) and names a libultra built without
IDO's divide (Mario Tennis past its crash, to a stall). The entry at the
end has the details. Mario Tennis is the maintainer's next priority.

The newest state first; the entries below are the record, oldest first.
"xemu" is the emulator (slot runs, test builds); "red" is the maintainer's
console (480i: its dashboard allows no 480p or 720p). Red ran the
`e7ad98f` set and then the Controls set on 2026-10-18 (the entries at the
end); set 14 (every game here plus Kirby 64, with the fixes those runs led
to) is staged and not yet on red (`build-host/deploy-set14.sh`). Anything
not marked red is xemu only.

| tier (`docs/compat.md`) | games |
|---|---|
| Gold | Super Mario 64, Pilotwings 64, Super Smash Bros. |
| Silver | Mario Kart 64, GoldenEye, Banjo-Kazooie, Ocarina of Time 1.2 |
| Experimental | Mario Party (its board: fix staged), Pokémon Snap (its camera lock: the tutorial's unlock, open), Majora's Mask (boot crash: fix staged), Jet Force Gemini, Kirby 64 (E) (staged), Mario Tennis, Castlevania: Legacy of Darkness, All-Star Baseball 2000 / 2001 / '99, South Park: Chef's Luv Shack, the rest |

| game (US) | how built | xemu | console (red) |
|---|---|---|---|
| Pilotwings 64 | its pack, or the ROM alone (`--analyze`): the same | title to results, saves, sound, 720p + hor+, the BACK menu | `vnrx1`: plays, sound, EEPROM saves, the BACK menu, the combos; 53-59 fps |
| Mario Kart 64 | the ROM alone | title, menus, races (F3DEX), sound (nead), Controller Pak, 720p 16:9 | `vmk2`/`vmk4`: races at 29.9 fps (display lists 2-3 ms), sound, the ROM inside the XBE, the BACK menu and its Paks page, EEPROM and pak files; the map screen's TV (a framebuffer read) wrong; drift boosts in question |
| Super Smash Bros. | the ROM alone | intro, title, 1P character select, a 1P match (Link vs Link, Hyrule Castle) at 47-50 fps with scripted input; 720p 16:9 at 45-57 fps (11.7 MB free) | `vssb6`: perfect, 59.9 fps; portraits sharp. `vssb7`: AC97 stuck on one boot, the next boot froze the Xbox in the audio boot; `vssb8` (step lines, an idle APU left alone): two clean boots |
| Mario Party | the ROM alone | Hudson logo, the intro's 3D scene, the story text; one present a frame (its frames are several tasks; presented at the swap), audio; its processes run as coroutines; 720p at 29-30 fps; START past the title to Mushroom Village (`0e5dc19`) | `vmp5`: flicker gone, 29.9 fps. `vmp11`: stable to a board; the logo blocky, the board black (both fixed since: `vmp12` staged) |
| Mario Party 2 / 3 | the ROM alone | intro, menus; 2: its audio thread, destroyed and made again by the game, plays on (58 tasks a second, 2026-10-17) | - |
| Super Mario 64 | the ROM alone (+ three `--learn` rounds before) | head screen, Lakitu intro, castle grounds and Lakitu's text drawn right (scripted, 2026-10-17); 720p hor+ at 29-30 fps (the skybox stays in the 4:3 box) | `vsm6`, `vsm7`: works perfectly, 29.9 fps |
| Pokémon Snap | the ROM alone (overlays sharing a vram) | intro, title, the photographer card, Professor Oak's lab and his whole briefing, every glyph right; the Beach course whole to the goal gate, back to the lab and out again at 29.9 fps (2026-10-18, 64 thread records); its cliffs, rails and track textured since the clamp-extent rule | `vsnap5`: runs well, 29.7 fps on the Beach; the camera can't be moved (the first run's tutorial: three photos with a Pokémon in focus; in xemu the photo check finds one and the tutorial prompts Z, then A; the unlock not yet shown) |
| Banjo-Kazooie | the ROM alone + ~22 `--learn` rounds | title, file select, the new-game intro with the lair drawn whole, Banjo's house (~28 fps); near/far now the game's (the IPL3 copy); 720p hor+ at ~28 fps, 10 MB free (geometry the game culls shows as void at the far edges) | `vbk12`: the jiggy transition right. `vbk13`: textures much better (RGBA 4-bit); static on the START screen (open) |
| Blast Corps | the ROM alone + 15 `--learn` rounds | Rare logo, title, profile screen, the attract demo (50-56 fps) | - |
| Majora's Mask | the ROM alone | title, file select, name entry, audio (2026-10-17) | `vmm9`: still crashes at its first uncached write (a KSEG1 alias page lost after boot; healed at the fault since, not yet on red) |
| Ocarina of Time 1.2 | the ROM alone, Yaz0 inflated (53 MB, page cache) | title, file select, name entry, a new file, the intro text, Kokiri Forest's HUD, the Deku Tree's intro drawn, music (nead); its intermittent title stall (a dropped SP done) fixed 2026-10-18 (8 of 8 boots) | `vzel13`: lock-ups up to 2.6 s and HDD noise (learn.txt per line: fixed). `vzel14`: steadier, quieter; its own 20 fps, hitches at scene loads (relocated overlays interpreted), small audio skips, a rumble buzz at scene changes (`[PAK]` log staged) |
| Ridge Racer 64 | the ROM alone + one `--learn` round | F3DEX2; 1732 frames in 30 s (~44 fps) | - |
| Donkey Kong 64 | the ROM alone + `--learn` rounds | its file screen ("GAME 1"), drawing on (the IPL3 copy at RDRAM 4); audio stops once graphics start (open) | - |
| Diddy Kong Racing, Wave Race 64, WWF No Mercy, Dr. Mario 64 | the ROM alone | present frames for the whole 30 s sweep run (DKR's microcode not decoded: grey boxes) | - |
| Paper Mario | the ROM alone + `--learn` rounds | past its obfuscated boot (KSEG2 DMA), 321 frames, then a map's textures (open) | - |
| GoldenEye | the ROM alone + `--learn` rounds (30) | logos, gun barrel, file select, mission select, briefing, the Dam in-game with its HUD and the pause watch; menus 37-50 fps; 7 functions refused by the recompiler (134 before); since 2026-10-18 the Dam right (cosf, palettes, the RDP-triangle sky), Bond's gun and the pause watch, ~30 fps; 720p 16:9 the same at ~30 fps, 18.6 MB free | `vge5`: works very well, 59.5 fps in menus; some frame-rate dips |
| Jet Force Gemini | the ROM alone | Rare logo, a menu screen by 70 s (its text drawn as boxes: open) since `$ra` is set at its resolver's calls (2026-10-18) | `vjfg1`: boots to the Rare logo, a broken menu, slow, silent (Rare's own GBI and audio microcode: open) |
| Kirby 64 (E) | the ROM alone | the intro, the Copy-ability question, the tutorial rooms (since osTvType from the region and the interrupt-off boost, 2026-10-18); one HUD icon garbled | `vk1` staged |
| Conker, Castlevania | the ROM alone + `--learn` rounds | boot and stop (the entries below) | - |
| the library (246 games) | the ROM alone (`tools/xbox/sweep.py`) | s9: 132 still presenting at 30 s (s8: 128); Jet Force Gemini past its boot since (`docs/analyzer.md`) | - |

**What exists** (each piece with host tests and a kill switch; the entries
below say how each was verified): the ROM inside the XBE and its page cache;
Yaz0 games inflated at build time; the ROM analyser (entry shapes, segments,
libultra by behaviour, the N64Recomp precheck; TLB-paged code the ROM
holds as one ROM-backed segment, captured RDRAM read for jump tables) and
N64Recomp's own patches (`patches/recomp`, applied at build time), and the learning loop
(`--learn`: the safety net's functions, DMAs, code captured from RDRAM and
checked against it by `guard.c`, TLB regions); the game's own TLB (aliases,
misses through its exception code and fault thread, KSEG2/3 through
`XRC_HIGH_TLB`); overlays sharing a vram; the RCP register model through
KSEG1; F3DEX/F3DEX2 and the microcode registries; audio HLE (aspMain, nead,
`n_aspMain`); every save type and the BACK menu's Paks and Controls pages; coroutines for
a game's own setjmp/longjmp; task done events held for their thread and
for room in their queue; `$ra` set at calls to code that reads it (N64Recomp
hooks); up to 64 N64 threads; threads
stopped by others; the library sweep and the V track's `decomp_check.py`
(0 wrong libultra names on Paper Mario and Mario Kart 64). Diagnosis:
`-DXRC_WATCH` (a hardware write watch), `XRC_WATCH_QUEUE`/`XRC_WATCH_RECV`,
`N64RX_TRACE=1` (per-thread traces in crashes and stalls), the stall report,
`[INTERP] hot`.

**Open, roughly by how much they block** (details in the entries below and
`docs/analyzer.md`):

1. From red (2026-10-17): Banjo-Kazooie's jiggy and START-menu
   textures; Ocarina of Time's textures (its two moons fixed in xemu);
   Mario Party's first-boot audio hang (the AC97 bring-up,
   `boot_prev.log`); whether `vzel12`/`vbk11`/`vmp9`/`vmm8`/`vsnap4`/`vge4`
   fix what they are meant to; 720p on the
   console (red's dashboard has it off); Mario Kart 64's drift boosts.
2. GoldenEye: on the console (`vge4`); the watch model and the walker's
   RDRAM tables are fixed in xemu (cosf), the rest of the missions unseen.
3. Ocarina of Time's overlays run interpreted (also at `vzel5`'s commit);
   its title stall (a dropped SP done) is fixed in xemu, not yet on red.
4. The s8 categories: NULL reads through KUSEG (23; Chopper Attack's is a
   hand-written task switcher, which needs strands like Mario Party's),
   Rare's compressed boots before learning rounds (19), no frames in the
   first round (52).
5. Per game: Paper Mario's map textures, Donkey Kong 64's audio, Bomberman
   64's loader, Perfect Dark (past its boot: a TLB miss through its own
   exception code, round 8), Jet Force Gemini's menu text (boxes), Mario Party's 720p title logo.
6. Renderer: S2DEX's own commands, F3DDKR, framebuffer effects by default;
   hor+ for orthographic backgrounds (Super Mario 64's sky); MusyX and the
   other audio microcodes.
7. M2's leftovers (the 30-minute soak, Pilotwings' pack README); M3's
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
- **Mario Kart 64's drift boosts, measured** (xemu, a scripted 50cc race,
  `XRC_WATCH` on player 1's `effects` at 0x800F6A4C and on its
  `driftStateCounter`/`driftState` word at 0x800F6BB8, offsets from the
  decomp's `Player`): R with the stick into a right turn hops (HOP) and
  drifts (DRIFTING); the counter runs while the stick is held outward,
  about two counts a presented frame; a 10-frame outward tap gives
  counter 19, then the return gives `driftState` 1 and counter 10, as the
  decomp's `update_drift_state_counter` says. Every scripted drift then
  ended 0.1 s to 0.4 s later (counter and state cleared, DRIFTING off: the
  heading within 6 degrees of the travel), before a second level, so no
  MINI_TURBO. Not known yet whether a player on a real N64 keeps the drift
  with the same input; nothing in the input path is wrong (full tilt 80,
  MK64's own dead zone and clamp to 53 after it). Next: a reference run
  of the same input, or the heading maths (`unk_0C0`) compared.
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
- No audio: every task that was not graphics went to the audio HLE, and
  the first one it sees names the audio microcode. Ocarina of Time's first
  such task is its JPEG decoder's (type 4, a 32-byte "microcode"):
  "unknown audio microcode", every audio task after it skipped. Tasks now
  go by type (`XRC_SP_BY_TYPE`): `nead (24-entry table)`, 45-55 audio
  tasks a second, peak 15000-19000 (xemu). The JPEG task is not run
  (prerendered room backgrounds, open).
The world draws: at 180 s of scripted A presses, the Great Deku Tree's
face, Navi and the "Fly, Navi, fly!" text box (the darker frames before
were the intro's own night and its full-screen dimming fills). Open:
interpreted overlay code is hot (up to 11 million instructions a
300-retrace window: the actors' relocated code); runs with a 640x480
SHOT stop logging in xemu some seconds later.

**Staged for the console (not yet deployed: red's FTP did not answer):**
`vzel1`, Ocarina of Time 1.2, the code of `9d51f31` (ThinLTO release, the
inflated ROM inside, 57 MB XBE), xbe sha1 7a99522d5478, title ID 4E52B837,
folder `/F/Applications/N64Recomp-X-ZEL` (both folders made by hand on the
first deploy). 45 s in xemu first: no crash, 1141 frames. To check: the
title, a new file, the intro, Kokiri Forest with Link under control, music,
fps, the free memory (`[BEAT]`).

**Paper Mario, the same day (xemu, test build, its `--learn` logs).** The
store to address 0 in its boot was not the copy-protection chain itself
(its hashes give `load_engine_data` 0x80026DDC and `create_audio_system`
0x8004AB00 from the ROM's bytes, as the decomp says): `load_engine_data`
DMAs the effect code to 0xE0200000, and the runtime masked that to
physical 0x200000, over the chain's own code at 0x80200000; the
interpreter returned into the new bytes. KSEG2/3 now go through the TLB in
the runtime's helpers (`docs/decisions.md`). It now presents 321 frames
(1 before), then reads address 4 in `load_texture_impl` (from
`mdl_load_all_textures`, a map's textures; open).

**Also staged (red still off):** `vsm1`, Super Mario 64 (US), the code of
`3248b8c` (ThinLTO release, ROM inside, 11.6 MB XBE), xbe sha1
ea2ad2ee555d, title ID 4E52B0E2, folder `/F/Applications/N64Recomp-X-SM64`
(make both folders on the first deploy). xemu: castle grounds with the
Lakitu text box drawn right (test build, scripted); the release booted
35 s, no crash. To check: the intro, the castle grounds, a course, fps,
sound, the EEPROM save.

**GoldenEye, the same day (xemu, test build, its `--learn` logs).** Its
main code runs at 0x7000xxxx through the TLB (all interpreted, "hand-written
code": the analysis has it at 0x80000400) and its game code is paged in at
0x7F000000 by its own refill (`tlb_resolve.s` in the decomp: an invalid
entry first, then its fault path loads the page from the ROM). The wild
jump to 0x7F5C40EC was the host side: a KUSEG address is reached at rdram +
vaddr, and from about 0x7C5CF000 up that is the Xbox kernel's space, where
the unmapped page 0x7F0CF000 read as someone else's memory instead of
faulting into a miss. The interpreter's fetch there now goes through the
game's TLB (`XRC_FETCH_TLB`): the miss at 0x7F0CFF30 is taken, then the
game's fault path stalls (its fault thread shows stopped; open). Not yet:
the same for the interpreter's and the generated code's data accesses in
that band, and recompiling the 0x7000xxxx code at its own addresses.

**Donkey Kong 64, the same day (xemu, test builds).** With the IPL3 copy it
reaches its file screen ("GAME 1", drawn right). Its audio (n_aspMain,
identified) stops: `XRC_LOG_SP_STARTS` shows 30 audio tasks during boot,
then only graphics tasks. Its scheduler (the decomp's `code_131B0.c`)
starts an audio task when the RSP is idle (`func_8060F208`) or, with a
graphics task current, asks it to yield (`func_8060FA5C`: state 3,
`osSpTaskYield`) and starts the audio task when that task's SP done comes
(`func_8060F54C`). The audio thread then waits on its done queue
0x8076D6D0 for good, and the scheduler on 0x80767A40 with nothing queued.
Next: where the order of the graphics task's SP/DP done (held for the
scheduler's thread, `sp.c`) and the audio thread's 0x29E request breaks
that path.

**Pokémon Snap, the same day (xemu, test build, scripted START/A).** It
stopped at 20 s with "Too many N64 threads" (32): it makes and destroys
object threads by the dozen and a destroyed thread's record was never
freed. A destroyed or returned thread's record and host thread, parked in
`stop_wait`, now serve the next new OSThread (`XRC_THREAD_REUSE`). It now
runs on into Professor Oak's lab (the menu, Oak, his dialogue) at 60 s.
Open: a run of glyphs missing in Oak's text ("t____").

**Dr. Mario 64, the same day.** The analysis misses its main segment
(`test_analyze.py`: 262 of 270 boot functions, main_segment missed), so
most of it ran through the safety net. Three `--learn` rounds (xemu slot
13, `build-host/drm-learn*.log`, never committed): 263 → 560 → 751
functions, the last round learning 3 code ranges and 4 functions. Round 3
presents for ~28 s, then sends no graphics task (its main thread busy;
open).

**Smash Bros.'s blurry textures (the console report), found in xemu.** On
the 1P character select the portraits and names were striped and garbled:
RGBA32 textures loaded with LOADBLOCK dxt 0 (kept in TMEM order) were
deswizzled with the 16-bit rule. RGBA32's odd rows swap texel pairs (TMEM's
two banks); `texconv_deswizzle32` (host-tested, `XGFX_DESWIZZLE32`). The
portraits and names are sharp now (xemu, scripted). Any game with RGBA32
dxt-0 loads gains the same. Console: to confirm (a new Smash build).

**Staged for the console at `6ae7cf0` (red off; these replace `vzel1` and
`vsm1`, never deployed):** ThinLTO releases, ROM inside, each booted 35 s
in xemu with no crash.

| build | game | xbe sha1 | title ID / folder | to check |
|---|---|---|---|---|
| `vssb2` | Super Smash Bros. | 6c0f23177dfd | 4E520101, `/F/Applications/N64Recomp-X-SSB` | the character portraits sharp (the RGBA32 fix), the rest as `vssb1` |
| `vzel2` | Ocarina of Time 1.2 | 41935c861b7d | 4E52B837, `/F/Applications/N64Recomp-X-ZEL` (new: make both folders) | title, new file, the intro, Kokiri Forest, music, fps, free memory |
| `vsm2` | Super Mario 64 | eb5eb4bf6999 | 4E52B0E2, `/F/Applications/N64Recomp-X-SM64` (new: make both folders) | intro, castle grounds, a course, fps, sound, EEPROM save |

**Ocarina of Time's texture misses.** Its scrolling tiles (2-cycle TEXEL1,
the file select's boxes) set `lrs`/`lrt` below `uls`/`ult`; the
interpreter's size came out 0 and the tile was dropped (`texture miss
(size)`, hundreds a run; the name-entry boxes drew solid cyan). Tile sizes
now wrap in 12 bits as the RDP's (`XGFX_TILE_WRAP`): 0 misses in the same
run.
Banjo-Kazooie had the other miss: a tile over a load shorter than one row
(48 bytes under a 64-byte line: height 0, dropped). Such a load now counts
as one row (with `XGFX_TILE_WRAP`); its run has 0 misses (48 before).
Whether this was its black sky and tree is for the console to show.

**Staged for the console, superseding `vssb2`/`vzel2`/`vsm2` (red still
off).** ThinLTO releases, ROM inside, each booted 30-35 s in xemu with no
crash. `vssb3`, `vzel3`, `vsm3` at `e4c575d` (the tile wrap); `vbk3`,
`vmk6` at `5380e59` (also the short-load row).

| build | game | xbe sha1 | title ID / folder |
|---|---|---|---|
| `vssb3` | Super Smash Bros. | 24bb66cd8c22 | 4E520101, `/F/Applications/N64Recomp-X-SSB` |
| `vzel3` | Ocarina of Time 1.2 | 5045824eef87 | 4E52B837, `/F/Applications/N64Recomp-X-ZEL` (new folders) |
| `vsm3` | Super Mario 64 | 1f9b64ca010c | 4E52B0E2, `/F/Applications/N64Recomp-X-SM64` (new folders) |
| `vbk3` | Banjo-Kazooie | 27c01714cf60 | 4E52E3F7, `/F/Applications/N64Recomp-X-BK` (replaces `vbk2`) |
| `vmk6` | Mario Kart 64 | 00005430f511 | 4E520DF7, `/F/Applications/N64Recomp-X-MK64` |

**Sweep s8 (2026-10-17, the worktree at `f6b0150`, xemu slot 14, 30 s
runs; built while other builds shared the CPU, so slow interpreted boots
read low).** 246 games: 128 still presenting at 30 s (121 in s7), 136
presented at all (139), KUSEG crashes 30 (42). New: Majora's Mask (564
frames), Mario Party 3 (836). Stopped presenting: Paperboy (512 → 0) and
six with 1-15 frames before, Goemon (NGME, almost wholly interpreted at
boot: the shared CPU). Paperboy was the safety net's table (512 addresses):
it is C++, its virtual functions reached only through vtables, and the
521st lookup returned nothing ("no function at 80029d30"). The table holds
4096 now, with a hash index (`XRC_MAX_LEARN`); Paperboy presents 1447
frames in 35 s at ~50 fps.
A mini sweep at `4769cad` (s8b, 12 games): Paperboy presents (1147 frames,
2026 learn lines); the others fail as before for their own reasons
(Bomberman 64 and AeroGauge "no function", Buck Bumble, Donald Duck,
Pokémon Stadium 2 no frames, Goemon a KUSEG write, Big Mountain 2000 a
divide by zero, Rayman 2 a NULL read). Clay Fighter 63⅓ and Top Gear
Overdrive draw a frame or two and wait: a frame left open with no task and
no matching swap is now presented by the VI thread after 6 retraces
(`XRC_SWAP_STALE`); Clay Fighter shows its frame again.

**Bomberman 64 (open, the same before today: s7, s8, s8b).** "No function
at ade00008": its loader `func_8000083C(index, dest, arg)` DMAs a code
segment (ROM index << 17) to `dest` and calls `dest(arg)`; on thread 3 the
call came with `dest` = 0x80020668, thread 3's own OSThread, so the DMA
wrote code over the thread's stack and the callback read back an
instruction word. Where that argument comes from (its handler table,
`func_80001A30`) is next.

**Majora's Mask (US), the same day (xemu, test build, the ROM alone, no
`--learn`).** Title, file select and the name-entry screen drawn right
(the name box textured since the tile wrap), audio (nead family, ~54 tasks
a second, peak 16497), ~41 fps on the menus. First time looked at.

**Staged for the console, the current set (red still off; supersedes every
earlier staged build).** ThinLTO releases, ROM inside, each booted 25-35 s
in xemu with no crash. All but `vmm1` at `521a389`; `vmm1` at `80a7fd5`
(docs only after it).

| build | game | xbe sha1 | title ID / folder |
|---|---|---|---|
| `vssb4` | Super Smash Bros. | d02ebd103ae7 | 4E520101, `/F/Applications/N64Recomp-X-SSB` |
| `vzel5` | Ocarina of Time 1.2 | 177d064477d6 | 4E52B837, `/F/Applications/N64Recomp-X-ZEL` (new folders; at `2d3b29d`, with the JPEG task; its title demo at OoT's own 20 fps in xemu) |
| `vsm4` | Super Mario 64 | 3ea43f35bc49 | 4E52B0E2, `/F/Applications/N64Recomp-X-SM64` (new folders) |
| `vbk4` | Banjo-Kazooie | b3f0c538d696 | 4E52E3F7, `/F/Applications/N64Recomp-X-BK` |
| `vmk7` | Mario Kart 64 | 3ce0973d05e2 | 4E520DF7, `/F/Applications/N64Recomp-X-MK64` |
| `vmp3` | Mario Party | 3e59f743f4dc | 4E52376A, `/F/Applications/N64Recomp-X-MP` |
| `vmm1` | Majora's Mask | ee9fd2560cda | 4E52510E, `/F/Applications/N64Recomp-X-MM` (new folders) |

**Ocarina of Time's JPEG task (its pre-rendered room backgrounds:
Hyrule Market, the shops).** From the decomp's `z_jpeg.c`: the CPU
Huffman-decodes four macroblocks (4:2:0, six 8x8 blocks, 0x300 bytes each)
into RDRAM and an `M_NJPEGTASK` turns each into 16x16 RGBA5551 in place.
`jpeg.c` does that task (`XRC_JPEG`), its numbers from the HLE plugin's
documented behaviour (read, not copied: `LICENSE.md`), host-tested on flat
macroblocks. The type-4 task OoT runs at boot (a 32-byte microcode, not
the decoder) is rejected and logged. Not yet seen on a pre-rendered room:
the scripts do not reach one.

**GoldenEye draws (xemu, test build, its `--learn` logs).** After the
fetch fix its third page fault stalled: its fault thread (`crashMain` in
the decomp's `crash.c`) loads the page, then puts the faulted thread on
`__osRunQueue` with `__osEnqueueThread` and yields, for libultra's
dispatcher to pop. The runtime never popped it: the second fault queued
thread 3 after itself, the third enqueue walked that cycle for good. The
thread is now taken off the game's run queue when the fault is let go
(`XRC_FAULT_RUNQ`; the queue's address from libultra's layout, 12 bytes
below `__osFaultedThread`, matching the decomp's 0x80027728). It now
presents 1047 frames in 55 s (0 before), ~14 fps in xemu with its main
code all interpreted.
A scripted run (START/A, shots at 50 s and 90 s) shows a dark background
with the red aim cursor, the same at both shots; every other line of the
background is dark (open: a row stride or copy-mode rule in its large
background draws, or the menu waiting on input it does not get).

**Mario Party 2's audio after its restart (xemu, 60 s, the regression run
after the thread change).** The game destroys its audio thread and makes
it again (`[OS] thread 0 started again`); audio now runs on after it:
3518 tasks by the end, 58 a second, peak 30893 (the old open item, "dies
some time after the game restarts it", not seen).

**"The boot code came back" (19 games in s8, mostly Rare's compressed
boots), first boot from the ROM alone.** Blast Corps: the analysis sees 54
functions before the boot decompresses the game; the game's own
`osStartThread` then runs interpreted and its `__osDispatchThread` ends in
an `eret` into the new thread, which the interpreter took as a return, so
no game thread ever started. Now the thread being dispatched (k0 when EPC
was written from its context) is started by the runtime from that context
(`XRC_DISPATCH_START`), and a boot returning after that parks. Blast Corps
now starts its main thread and its PI manager on the first boot; it then
waits on its own (unnamed) VI manager, whose events go through its own
libultra, which the learning rounds name. The retrace thread now also
starts when a game registers `OS_EVENT_VI` (`XRC_VI_ON_EVENT`).

**Perfect Dark, five learning rounds (xemu slot 13, `pd-learn*.log`).**
Round 1 from the ROM alone: 2 functions, "the boot code came back" (its
boot returns before any dispatch). Rounds 2-5: 181 functions, 67 libultra
names, then a fetch at 0x700016CC (its code, mapped at 0x70000000 through
the TLB as GoldenEye's) before any TLB entry maps it (no `[TLB]` line):
the crash. Its learn build places RDRAM's window at 0x91000000, so KUSEG
0x70000000 sits at host 0x01000000 (the alias region); the interpreter's
kernel-band test now computes in 64 bits (it wrapped there). Open: where
its boot maps 0x70000000.


**GoldenEye's front end (xemu, test builds, 19-20 learning rounds,
2026-10-17 evening).** Four fixes, each from its decomp:
- The striped background: `titleRenderFolderMenuBackgroundLines`
  (`title2.c`) draws its 440x299 backdrop as 299 one-row texrects, `(y <<
  2)` to `((y + 1) << 2) - 1`. The RDP fills the row whole; upscaled, each
  left a quarter-row gap. 1/2-cycle texrects now cover every pixel row and
  column they touch (`XGFX_TEXRECT_SNAP`).
- Its menus draw at 440x330 (`fr.c` sets the VI's scales from the buffer);
  the renderer mapped 320x240, so the folders and the legal text were off
  the right edge. The screen's size now comes from the buffer the game
  swaps to (`XGFX_HIRES`; `[GFX] screen 440x330`).
- Its microcode (Fast3D 2.0G) has Rare's `G_TRI4` at 0xB1 (four triangles
  of 4-bit indices, `gbi_extension.h`): every model drew one triangle in
  ten (a frame: 9 triangles, now 1018). `XGFX_TRI4`, host test `tri4`.
- It evicts its 0x7F000000 code pages at random (90 frames of 8 KB,
  `tlb_manage.c`) and calls into them through pointers; the runtime's
  lookup said "no function at 7f01f53c". A call into an unmapped KUSEG page
  now runs the game's refill code and fault path first (`XRC_LOOKUP_TLB`).
Seen: the Nintendo logo, the Rare logo (fading), the gun barrel and its
blood, the legal screen, the file select with four folders, A into the
mode select ("1. SELECT MISSION", "2. MULTIPLAYER"). 46-58 fps with
round 19's code (14 fps before it). Not yet: a mission. `[MEM]` free 20956
KB; `[CLOCK]` cpu 733 MHz (xemu).

**720p on the working games (xemu slot 10, its HD eeprom: 480p, 720p,
widescreen; test builds at `42a1fd3`, `build-host/hd720.sh`).** 720p asked
for 32 MB free before pbkit; Smash Bros. (31376 KB) and Banjo-Kazooie
(29600) fell back to 480p. It costs about 3 MB (Mario Kart 64: 21020 KB
free after nv2a at 480i, 17964 at 720p), so the gate is now 24 MB
(`XHW_VIDEO_720P_KB`, decisions.md). At 720p 16:9:

| game | free at runtime | fps (xemu) | picture |
|---|---|---|---|
| Super Smash Bros. | 11.7 MB | 45-57 | title in the 4:3 box, its fill to the edges |
| Mario Kart 64 | 17.0 MB | 25-30 | title in the box |
| Banjo-Kazooie | 10.1 MB | ~28 | hor+; geometry the game culls shows as void at the far edges |
| Mario Party | 20.1 MB | 29-30 | the letterboxed intro in the box |
| Super Mario 64 | 21.1 MB | 29-30 | head screen in the box; the attract demo hor+, its skybox (an ortho draw) only in the box |
| Ocarina of Time | 18.7 MB | 20 | title hor+ with two moons and skybox seams (open) |

Ocarina of Time stops presenting when A is pressed during its opening
(at frame 36 or 231 of the boot, before the logo has settled): at 480p
too, with the evening's switches off, and at `vzel5`'s commit
(`2d3b29d`), so not a regression. One thread then loops through seven
interpreted functions (8007e64c, 80218194, 8008fe7c, 8008faf8, 8008f408,
800902e8) and no graphics task goes out. START at 40 s, once the logo
has settled, reaches the name entry as before (`ootlate`). Open.

**GoldenEye in the Dam, and most of it recompiled (xemu, test builds,
learning rounds 21-27, 2026-10-17 evening).** From the briefing, START
loads the Dam: the ground, the barriers, the ammo counter (7 | 93) drawn,
the sky a dark purple (open) and one block's texture garbled (open). What
got its code out of the interpreter, each from the decomp:
- Its jump tables for the 0x70000000 code sit in KSEG0 data its boot
  decompresses (0x8002xxxx): N64Recomp read a table at the function's own
  ROM offset. `patches/recomp/0001` reads it from the section that holds
  it; `learn.py` makes captured KSEG0 RDRAM unlike the ROM under it into
  function-less sections listed first (`rdram_<vram>`).
- `tlb_manage.c` copies each 0x7F000000 page straight from the ROM: the
  captured pages match the ROM at 0x34B30, so the analyser now makes one
  ROM-backed segment of them (`tlb_7F000000`, 2463 functions; the boot's
  0x70000000 copy likewise). Its own libultra copy lives there; it counts
  as captured code for naming (left out, the boot stalled).
- The walker read tables at 0x8005xxxx from the compressed ROM bytes and
  gave up ("jr without a table", 93 functions, interpreted): it now reads
  them from the RDRAM sections too.
Functions interpreted: 158 (round 20) -> 26; refused by the recompiler:
134 -> 8 (cop0 and `trunc.l` only). Interpreted instructions in the
menus: 7.5 million per 300 retraces (round 26) -> about 4000 (round 27);
the menus at 45-50 fps. But in the level that build drew a black world
under a frozen "PP7 (SILENCED)" banner, or (paused) the watch without its
model. Bisected by interpreting ranges (`N64RX_CAPTURED_INTERP`, now over
`tlb_` segments too; `build-host/gebis.sh`): the black world goes with
0x7F038000-0x7F054000, the watch model with 0x7F054000-0x7F05B000, outside
0x7F0568F4-0x7F056F08 (`get_status_of_objective` at 0x7F057238 alone is
not it). Every switch checked there has its `sltiu` bound's case count
and a `nop` delay slot. The walker's RDRAM tables are off by default for
now (`N64RX_WALK_RDRAM=1` turns them on): GoldenEye builds as round 26,
its level code partly interpreted. Round 30 (off) still shows the paused
watch without its model, so that one is older than the walker change
(interpreting 0x7F054000-0x7F05B000 brings the model back); the black
world was seen only with it on. `[MEM]` free 18608 KB;
`[CLOCK]` cpu 733 MHz (xemu).

**Staged for the console, the current set (red still off; supersedes
every earlier staged build).** ThinLTO releases at `bec34b3`-`7b05f79`
(the analyser's later changes touch only games with captured RDRAM: of
these, Banjo-Kazooie, built last), each booted 35-40 s in xemu with no
crash (`build-host/hwsmoke.sh`). New since the last set: 720p from 24 MB
free (Smash Bros. and Banjo-Kazooie now get it), the texrect, hi-res,
TRI4 and fill fixes (GoldenEye's, harmless elsewhere), the VI framebuffer
at K0BASE before the first swap. Mario Party's first build of this set
(`vmp4`) called a wild pointer at 15 s (a jump table read from another
overlay); fixed before `vmp5`.

| build | game | xbe sha1 | xemu (40 s, 480i) | title ID / folder |
|---|---|---|---|---|
| `vssb6` | Super Smash Bros. | 0ca60d9d9f33 | 42 fps, 14.7 MB free | 4E520101, `/F/Applications/N64Recomp-X-SSB` |
| `vbk6` | Banjo-Kazooie | b9dac8872136 | 20 fps, 12.9 MB | 4E52E3F7, `/F/Applications/N64Recomp-X-BK` |
| `vmk9` | Mario Kart 64 | ec2677a6e263 | 28 fps, 19.9 MB | 4E520DF7, `/F/Applications/N64Recomp-X-MK64` |
| `vmp5` | Mario Party | 01b62e7f20f8 | 28 fps, 23.2 MB | 4E52376A, `/F/Applications/N64Recomp-X-MP` |
| `vsm6` | Super Mario 64 | 3d4735b2627f | 29 fps, 24.0 MB | 4E52B0E2, `/F/Applications/N64Recomp-X-SM64` (new folders) |
| `vzel7` | Ocarina of Time 1.2 | cefa56962924 | 20 fps, 21.6 MB | 4E52B837, `/F/Applications/N64Recomp-X-ZEL` (new folders) |
| `vmm3` | Majora's Mask | e83c420d2b36 | 17 fps, 18.1 MB | 4E52510E, `/F/Applications/N64Recomp-X-MM` (new folders) |

Deployed to red 2026-10-17 20:27 (each XBE's size checked on the
console); its results are the "Red, the set's first run" entry below.
To test on red: 720p on Smash Bros. and Banjo-Kazooie (the `[MEM]` and
`[BEAT]` free lines; the 24 MB rule is xemu-measured); Mario Party's
flicker and Banjo's clipping (the earlier fixes, never confirmed); Smash's
portraits; in Ocarina of Time, wait for the title logo to settle before
START (an earlier press stalls it, open).

**Red, the set's first run (2026-10-17 evening; logs in
`C:\xemu\hw\logs<build>`).** Red's dashboard allows neither 480p nor
720p (`AV pack 1, 480p 0, 720p 0`): everything ran 480i, 720p untested.
`[CLOCK]` cpu 733 MHz on each.

| build | the maintainer | the log |
|---|---|---|
| `vssb6` Smash Bros. | perfect, 60 fps | 59.9 fps, display lists 3.5 ms |
| `vsm6` Super Mario 64 | works perfectly | 29.9 fps |
| `vbk6` Banjo-Kazooie | works well, widescreen right; textures wrong as the jiggies come in and in the START menu | 29.9 fps, display lists 2 ms |
| `vmk9` Mario Kart 64 | - | no log (not run) |
| `vmp5` Mario Party | flicker gone; the logo's texture wrong; the controls dead (START does nothing); the first boot's audio stuck, fine after a reboot | 29.9 fps; `boot_prev.log` has the stuck AC97 |
| `vzel7` Ocarina of Time | textures wrong, low frame rate, two moons on the title | 19.9 fps (the game's own 20) |
| `vmm3` Majora's Mask | crashes | a write to host a38af402 (KSEG1 0xA027E402) at its first frame |

Found and fixed the same night (`0e5dc19`): Mario Party's controls (the
analyser named the game's own pad ring `osContGetReadData`; Mario Party
2 and 3 the same), its title logo (32-bit LOADBLOCKs counted at 16 bits),
and the KSEG1 alias of 8 MB windows, refused on the console as a whole
(`[MEM] KSEG1 not aliased (host a3631000 in use)` in OoT's log too): now
aliased page by page, the first refused page logged. Open: Banjo's jiggy
and START-menu textures, Ocarina of Time's textures and two moons, the
first-boot audio hang.

**Staged: the fix round (red off; `build-host/deploy-set.sh` deploys
these four after pulling the logs of the builds they replace).** ThinLTO
releases at `0e5dc19`, each booted 35 s in xemu with no crash.

| build | game | xbe sha1 | what it should fix |
|---|---|---|---|
| `vmp6` | Mario Party | 8ad0ef65b4ae | the dead controls, the title logo |
| `vmm5` | Majora's Mask | 2044b8ad67d3 | the crash at its first uncached write (or the log names the page in the way) |
| `vzel9` | Ocarina of Time 1.2 | 9e2f9f402b59 | the KSEG1 alias (uncached RDRAM), 32-bit block loads: perhaps its textures |
| `vbk7` | Banjo-Kazooie | 5999609195a9 | 32-bit block loads: perhaps the jiggy and START-menu textures |

## 2026-10-18: Pokémon Snap's dialogue, Ocarina of Time's moon, timers after a hitch (overnight, xemu)

**Pokémon Snap now (xemu slot 14, test build, scripted input; emulator
only).** From the ROM alone at `d4a122a`: intro, title, the photographer
card, Professor Oak's lab and his whole briefing (the ZERO-ONE, the PKMN
Report, "Shall I repeat this information?") with every glyph right; ~58 fps
in the menus, 30 in the lab; no crash or stall in 6-minute runs. `[MEM]`
64 MB, 14.7 MB free in the lab; `[CLOCK]` 733 MHz (xemu). The scripted
input has not yet reached a course (Beach): blind timing through the
dialogue is slow.

**Its glyphs.** Snap types its dialogue into a 152x13 RGBA16 strip (3952
bytes, one LOADBLOCK) at a fixed address. The texture cache's change check
sampled 16 words of anything over 2 KB, so most new glyphs kept the strip's
old contents ("touna everywn" for "found everywhere"). A block load over
2 KB is now also hashed whole every 8th frame it is used (staggered by
cache slot) and every frame once its address has been seen to change
(`XGFX_TEX_RECHECK`); LOADTILE textures hash every row, not the first and
last (`XGFX_TILE_ROWHASH`). Hashing every big texture every frame was tried
first and cost Snap's lab 2.5 ms of display-list time a frame in xemu; the
recheck costs ~0.4 ms (8.4 against 8.0).

**Ocarina of Time's two moons (red's report) reproduced and fixed in
xemu.** `-DXGFX_TRACE_PRIM=0xf0ffb4` traced the frame with the moon's prim
colour: one draw, 2 triangles, a 64x64 IA8 tile with clamp bits clear and
mask 0, texture coordinates 0..2. The RDP clamps an axis with mask 0
whatever its clamp bit (no wrap without a mask), so only one moon shows;
the interpreter repeated it (four moons, two on screen). Fixed for every
game (`XGFX_MASK0_CLAMP`). The moon itself was not caught on screen again
(the title's day/night timing varies run to run); the trace shows the
quad's coordinates and the wrap mode now clamp.

**Timers after a long hitch.** A periodic `osSetTimer` more than one
interval behind kept a deadline in the past; the timer thread's wait
(`next - now`) underflowed to ~31 days and every timer stopped until a new
`osSetTimer`. Re-armed from now, a past deadline waits 0 ms
(`XRC_TIMER_CATCHUP`; `test_time.py` reproduces the 2,688,506,306 ms wait
with it off). Any hitch longer than a timer's interval on the console (an
HDD spin-up, a long load) could do this.

**Ocarina of Time stopped after every xemu screenshot: its own RCP-hang
check.** An autopad `SHOT` (an `[FBDUMP]` over COM1, ~10 s inside
`osViSwapBuffer` on the scheduler's thread) outlasted the 3 s timer the
graphics thread sets on each task (`Graph_TaskSet00`): `0x29A` arrived on
its queue (`XRC_WATCH_QUEUE=0x8011f9b4`), the game took it for "RCP is HUNG
UP!!" and stopped, as on an N64. The game's clock (osGetTime/osGetCount)
now stands still while a shot is written (`xhw_game_time_us`,
`XHW_SHOT_FREEZE`): three shots in a row and the title runs on. Not the
console's early-press stall (no shot there).

**Regressions checked (xemu 720p, slot 10, the hd720 scenario):** Smash's
1P menu, Banjo's intro (Spiral Mountain), Super Mario 64's file select and
Mario Kart 64's title (its checkered flag, compared with the clamp off) draw
as before. Mario Party's title logo, still blocky at 720p, is worse with
the three switches off (striped), so better, not right: its 188-wide RGBA32
strips load with a dxt that does not divide the line (open). Mario Kart
64's game-select panels show thin dark lines at strip seams in one run;
not compared with the clamp off (the scripted START did not reach the menu
again).

GoldenEye with the three switches on and off (slot 13, its 30 learn
logs): the Dam the same either way (the green block left of the start and
the speckled props are older); one run without the ammo counter was
GoldenEye's armed/unarmed input timing (the watch screen draws the bullet
icon and digits with the clamp on). Test builds now log, once per texture,
a draw whose coordinates leave a mask-0 axis's loaded extent by more than a
texel (`[GFX] mask-0 clamp:`): none in that run.

Host: `test_f3dex.py` (row hash, the recheck, mask-0 clamp: each fails with
its switch off), `test_time.py`, `test_fbfx.py`; `rom_leak_check.py` clean.

**Staged for red (supersedes every earlier staged build; `build-host/deploy-set.sh`
and `build-host/watch-red.sh` deploy these, pulling the logs of `vzel7`, `vbk6`,
`vmp5`, `vmm3`, `vsnap1` first, and verify each by size).** ThinLTO releases at
`3ff8766` (the 2026-10-18 renderer, timer, palette and fall-through fixes;
held done events waiting for queue room; 64 thread records). The set
before (`vzel11`, `vbk10`, `vmp8`, `vmm7`, `vsnap3`, `vge3`, at `93c7fb5`)
never reached red and is replaced.

| build | game | xbe sha1 | what it should show |
|---|---|---|---|
| `vsnap4` | Pokémon Snap | 91b56619fcf3 | past the main menu (vsnap1 crashed there), Oak's lab with its text right, the Beach course whole (in xemu; `vsnap3` would have stopped at its start: 32 threads) |
| `vzel12` | Ocarina of Time 1.2 | 14b314f210b2 | one moon; the KSEG1 and 32-bit load fixes; no title stall (a dropped SP done) |
| `vbk11` | Banjo-Kazooie | 2d6f11e34f44 | the jigsaw transition's pieces carrying the picture (built with `-DXGFX_FB_READBACK=3`: the frame converted when the game invalidates it); the START-menu textures |
| `vmp9` | Mario Party | 8908b636184a | the controls and the title logo |
| `vmm8` | Majora's Mask | 30575cb4b885 | the first uncached write no longer crashes it |
| `vge4` | GoldenEye (new: `/F/Applications/N64Recomp-X-GE`, 4E52481C; deploy-set makes the folders) | a45fee186588 | first console run (its 30 learn logs): logos, menus, the Dam with its textures, sky, cliffs, Bond's gun and the pause watch right in xemu |

### GoldenEye's palettes and its sky (2026-10-18, xemu)

GoldenEye keeps each CI texture's palette right after its texels and loads
it with LOADTLUT, SL = the texel count, in an image of width 1
(`gDPLoadTLUT07`, `tex.c`); the interpreter read every palette from the
image's start, so CI textures took their own texels as colours. With SL/TL
honoured (`XGFX_TLUT_OFFSET`) the Dam's start is right in xemu: the green
block is a rusty container, the barriers red and white, the hazard stripes
yellow and black. Its cloud layer is drawn as raw RDP triangle commands
built on the CPU (`sky.c skyRenderTri`, G_RDPHALF_1/G_RDPHALF_CONT pairs
ending in G_RDPHALF_2): now decoded and drawn (`XGFX_RDP_TRI`; a mid-frame
dump shows it). The purple was those mis-paletted textures; the top of the
start view is now the dark far geometry the game draws there without fog.
Perfect Dark shares GoldenEye's texture code. Host: `test_f3dex.py`
(`tlut_offset`, `rdp_tri`). Not checked: the watch model (its own open
item), the cloud layer's look against the N64's.

### GoldenEye's cosine (2026-10-18, xemu)

Interpreting 0x7F054000-0x7F05B000 made the Dam right, so the recompiled
code there was wrong. Bisected by interpreting halves with a pixel check
(blue sky at the top-left of a level shot; `build-host/gebisect.sh`):
0x7F057800-0x7F057F00, where `math_sincos.s` puts `cosf` at 0x7F057EA0:
three instructions (add pi/2) that fall into `sinf` with no return. The
analyser sized `cosf` up to `sinf`'s start, so the recompiled `cosf` returned
with `$f0` unset: every cosine in the game was garbage. A function whose
path runs off its end now takes the next function's body too
(`emit._size`, `N64RX_FALLTHROUGH`; only `cosf` changed size in GoldenEye:
0xC to 0x12C). In xemu the Dam is right from the ROM alone: the cliffs,
the cloudy blue sky (the RDP-triangle layer), the tunnel, and Bond's hand
and PP7, which were missing. `test_analyze.py` 6 pairs, 0 failures. The
pause watch's model (the same range) is still to see (the scripted pause
missed); the earlier "dark sky" was this, not the palettes alone.

Then with the walker's RDRAM jump tables on (`N64RX_WALK_RDRAM`, off since
the black level): the level is right and the pause watch draws its whole
model (the face, the health and armour arcs, the PP7), so both old faults
were `cosf`. The walker's tables are on by default again: the level's
interpreted instructions fall from 8.4 million to 42 thousand per 300
retraces, 26 to 30 fps in the Dam (xemu).

### Perfect Dark past its boot (2026-10-18, xemu slot 13)

Its 0x70000000 code is mapped through the TLB as GoldenEye's, and three
things stood in the way: (1) the RDRAM window had moved
(`XRC_RDRAM_USER`) so 0x70000000 lands at host 16 MB, but something else
was there by the time its TLB mapped it: the learnt KUSEG span's host range
is now held at the very start of boot (`XRC_KUSEG_HOLD`,
`xhw_alias_hold`; `[MEM] KUSEG 70000000-704fffff at host 01000000: held`);
(2) it maps 0x70400000 onto physical 0x400000, the Expansion Pak's half, in
a 4 MB window: the TLB logs `[LEARN] rdram 8` and the next build gets 8 MB;
(3) its first two pages at 0x70000000 match the ROM's start, so the
analyser made a ROM-backed segment of the whole region and compiled
0x70003EE0 from unrelated ROM bytes (`lw $ra, 0x16FE($s3)`): a ROM-backed
TLB segment now never covers a captured page of its region that differs
from the ROM (`N64RX_TLB_ROM_RUN`; GoldenEye's 0x7F segment unchanged).
Rounds 6-8 (`pd-learn6..8.log`): no crash for 100 s, no frame yet; round 8
ends in a TLB miss at 0xD2D2D3A2 (a stack-fill pattern used as a pointer)
through the game's exception code, then a NULL read in 0x70003D14. Rounds
9 and 10 were starved by the sweep's builds (retrace 5; logs removed).
`[MEM]` 8 MB window, 29 MB free; `[CLOCK]` 733 MHz (xemu).

### Ocarina of Time in game (2026-10-18, xemu slot 13)

A new-file scenario (`build-host/scen/ootgame`: START, file 1, END on the
name, start, A through the intro; screenshots no longer stop it) reaches
the Great Deku Tree's intro, Link's bed with Navi's "You finally woke up!"
and the house, all textured, at the game's 20 fps. One later frame (going
outside) showed the view covered by flat PRIM-only polygons: a near green
one (z 0.11) and a far blue one drawn first, the textured room geometry
after them. Not seen again (the run's timing varies); it may be a
transition or the "texture issues" red reported. Its S2DEX2 `G_BG_COPY`
(0x0A, the pre-rendered backgrounds) is still unhandled (`[GFX] unhandled op
0A`).

### Banjo-Kazooie's jigsaw transition (2026-10-18, xemu)

Red's "textures wrong as the jiggies come in": the transition copies the
frame into 80 32x32 textures of the jigsaw model with the CPU right after
`osInvalDCache` over it (`model_copyFramebufferToTextures`,
`gc/transition.c`), and our frames never reach RDRAM. Readback mode 3
(`-DXGFX_FB_READBACK=3`) converts a staged frame when the game invalidates
at least half of it: the pieces now show the picture (the guitar scene,
the title logo, `-DXGFX_FB_INVAL_SHOT=20` shots). Mode 1 also did it, but
its page watch went hot in the levels (60-120 conversions per 600 frames,
15 fps). Default still 0; `vbk10` is built with 3 for red.

**Ocarina of Time's title stall is not the press (xemu).** A run with the
new-file scenario froze at frame 421, before the first scripted press (every
autopad event then fired at frame 421): all threads waiting, the graphics
thread on its task-done queue (`0x8011F9B4`), the scheduler idle on its own,
the fault path's input loop running (0x8007E64C, 0x80218194, 0x8008FE7C...).
So the "early press" stall is an intermittent lost task completion at the
title; the press only coincided. Next: `XRC_LOG_SP_STARTS` plus
`XRC_WATCH_QUEUE=0x8011D978` over repeated title runs to catch which done
event goes missing.

**Ocarina of Time interprets its overlays.** At the title, 7.7 million
instructions per 300 retraces run interpreted (0x801E88A4 alone 5.8 million
in 100 calls; 0x803A1854, 0x801E7248, 0x803A119C): relocated overlay code
the ROM-only build cannot place, and OoT has had no learning rounds. A
likely part of red's "low FPS"; learning rounds (`--learn`, the captured
code checked by `guard.c`) are the next thing to try for it.

**Learning Ocarina of Time's overlays does not work yet.** Round 1 (title,
file select, the intro, the house: `zel-learn1.log`, 1410 functions)
built into round 2 only after two analyser fixes (`emit.cross_funcs`: a
conditional branch to another function's start is a goto N64Recomp cannot
compile, and overlays sharing a vram were never checked: their functions
are keyed with their ROM), and round 2 then never presented a frame: its
`learn_` segments compile the overlays' ROM bytes, but OoT relocates each
overlay as it loads it, so the recompiled code has the unrelocated
addresses. Learning a relocating game needs the captured, relocated bytes
(the `code_` path) for those overlays, with `guard.c` checking each load.

## 2026-10-18: Ocarina of Time's title stall found and fixed; Jet Force Gemini boots (overnight, xemu)

**The stall.** `build-host/ootstall.sh` (title boots, no input, with
`-DXRC_WATCH_QUEUE=0x8011D978 -DXRC_WATCH_QUEUE_ALL=1
-DXRC_LOG_SP_STARTS=100000`) caught it on its third run. The scheduler's
thread fell a few retraces behind (the host busy with the library sweep);
its 8-slot queue filled with the IRQ manager's retrace messages; then the
retrace's flush of the held SP done (`sp.c`, two retraces old) posted into
the full queue and `post()` dropped it ("event 4 dropped: queue 8011d978
full (8)"). The graphics task never completed and the graphics thread
waited for good. Held done events now wait for room in their queue (both
slots when SP and DP done share one), in order, and the next flush tries
again (`XRC_RCP_ROOM`, default 1). On the N64 the scheduler drains its
queue long before eight messages build up. Verified: 8 of 8 title boots
since, under the same sweep load, with no drop and no stall (two brief
60-retrace loading pauses in one run, also in the logs from before).
`[MEM]` 21.4 MB free at the title; `[CLOCK]` 733 MHz (xemu).

**Jet Force Gemini past its boot (xemu, test build, slot 13).** It wrote
through a null pointer at boot in every sweep (s8, s9). Its call-site
resolver (`0x80054530`, hand-written, interpreted) finds the `jal` that
called it at `ra - 8` in the relocation tables of its relocatable code,
loads that code, and jumps to the function. N64Recomp's generated code never
sets `ctx->r31` at a call, and the interpreter starts with a sentinel in
`$ra`, so it found nothing. The analyser now finds the functions that read
`$ra` as a value (`emit._reads_ra`) and gives every `jal` to them in
recompiled code an N64Recomp `[[patches.hook]]` that sets `ctx->r31` first
(JFG: 1 function, 497 calls). Such a function, when interpreted, enters
through `xrc_interp_ra` with that ra and returns on a jump back to it
(`N64RX_RA_HOOKS`, default 1). JFG then shows the Rare logo right and
reaches a menu screen (a panel drawn, its text as boxes) by 70 s, 1091
frames in 55 s. The relocatable code runs interpreted inline (4.7 million
instructions per 300 retraces at its busiest). `[MEM]` 32.5 MB free after
nv2a; `[CLOCK]` 733 MHz (xemu). Not verified: past the menu, and the
console.

**Pokémon Snap's Beach course (xemu, test build, slot 14, scripted).**
Two script problems first. Autopad's `t<ms>` times ran on the wall clock,
so each SHOT (10-15 s over serial) ate the script's time: they now run on
the game's clock, which the dump holds (`XHW_AUTOPAD_GAME_CLOCK`). And N64 B
is the Duke's X (the Duke's B is C-right): the script had answered Oak's
"Shall I repeat this information?" with C-right and looped the briefing
(`build-host/scen/snap19`: A through the name and the briefing, A then X
5 s apart until 117 s, then A alone). The course then stopped at its start
with "Too many N64 threads" (32): HAL's object manager runs a thread per
object, and the Beach has more than 32 at once. The records are now 64
(`XRC_MAX_THREADS`); the run went through the whole course (Pikachu,
Snorlax, Butterfree, Eevee, Pidgey drawn), past the goal gate, back to
Oak's lab ("Welcome back!") and onto the course again, with no stall.
29.9 fps on the course, 53-55 in the lab. `[MEM]` 11.0 MB free on the course
(17.0 after nv2a); `[CLOCK]` 733 MHz (xemu). Open: some cliff and tunnel
walls draw flat grey. Not verified: the console, the other courses, the
photo review.

**Pokémon Snap's flat grey cliffs: the RDP clamps at the tile, not the
mask (xemu, test build, slot 14).** A traced course frame
(`-DXGFX_TRACE_MS=208000`, `build-host/scen/snap20`; the bind line now
prints the tile's shifts and the `G_TEXTURE` scale) showed the cliff
draws: a 64x64 CI4 texture, mask 6, clamp set on both axes, on a tile 768
texels wide, at coordinates 324..383. The RDP wraps by the mask inside the
tile and clamps only past its extent; the interpreter clamped at the
texture's edge, one flat colour. An axis with a mask and a clamped tile
wider than 2^mask now repeats (or mirrors) (`XGFX_CLAMP_EXTENT`, default
1; host test `test_clamp_extent`). The Beach's cliffs, rails and wooden
track are textured since (they were flat grey and smeared). Verified:
Snap's course (xemu); Super Mario 64's head screen the same with and
without it. Not verified: the A/B run over Mario Kart 64, Banjo-Kazooie,
GoldenEye, Ocarina of Time and Smash Bros. was stopped before it ran
(`build-host/clampab.sh` runs it); the staged `vsnap4` was built before
this rule.

## 2026-10-18: red runs the `e7ad98f` set; the Controls page (console + xemu)

**Red ran ten builds at `e7ad98f`** (ThinLTO, the ROM inside, 480i; logs
pulled to `C:\xemu\hw\logs<name>`; `vmp10` was not run). `[CLOCK]` 733 MHz
on every boot. The maintainer's notes, then what the logs say:

| build | maintainer | log |
|---|---|---|
| `vsnap5` Pokémon Snap | runs well; the camera can't be moved on the Beach | 29.7 fps on the course; `[MEM]` 17.1 MB free after nv2a; no thread or stack crash |
| `vzel13` Ocarina of Time | much better; low fps, texture pop-in, a blocky title, 20 fps with random lock-ups; much HDD activity | 19.9 fps (its own 20), frames up to 2.6 s; 4574 `[LEARN]` lines, 2208 different, each new one rewrote learn.txt whole (below); 180 overlay functions run by the safety net (relocated overlays) |
| `vge5` GoldenEye | works very well, some fps issues | 59.5 fps in menus |
| `vjfg1` Jet Force Gemini | HDD churn at boot, the Rare logo, a glitched menu, very slow | 23.7-29.9 fps; graphics microcode with no version text read as Fast3D: ops 05 and 07 unhandled ~15000 times a run, 48 "no load" texture misses; its audio microcode unknown (silent) |
| `vssb7` Smash Bros. | first boot: the audio-stuck notice, then fine; second boot (from the dashboard): locked | boot 1: `AC97 stuck ... after 6 restarts and 1 cold resets`, 52.9 fps; boot 2: the whole Xbox froze between `[AUDIO] found: codec` and the idle step (no more `[BEAT]`; the log is flushed per line before the first frame) |
| `vmk10` Mario Kart 64 | as before, fine | 29.9 fps |
| `vsm7` Super Mario 64 | great, to Bob-omb Battlefield | 29.9 fps |
| `vmm9` Majora's Mask | crashes at boot | a write to host a38af402 (KSEG1 0xA027E402) at frame 15, the same as `vmm3`; no `[MEM] KSEG1` line, so the 8 MB alias was made whole at boot and lost a page later |
| `vbk12` Banjo-Kazooie | the jiggy transition fixed; the START screen still wrong | all 48 logged texture misses RGBA at 4 bits, from 115 s (the START menu) |

**Fixed since (xemu and host tests; not yet on red):**

- learn.txt is written by its own thread every 3 s, outside every lock,
  and a line is logged once a boot (`XRC_LEARN_WRITE_MS`, `learn.c`): each
  new line had rewritten the whole file through the atomic path (temp
  file, flush, read back, rename) on the game's thread. Ocarina of Time's
  lock-ups and HDD noise, and likely Jet Force Gemini's.
- A KSEG1 alias page found missing at a fault is aliased again and the
  access retried; one the kernel holds is logged with its PDE/PTE
  (`XRC_KSEG1_HEAL`, `mem.c` via the TLB fixup). Majora's Mask; not
  reproducible in xemu (its alias never fails there).
- RGBA at 4 or 8 bits reads through the palette with the TLUT on, as I4/I8
  with it off (`XGFX_RGBA_LOWBIT`). Banjo-Kazooie's START menu.
- The audio boot logs `[AUDIO] step:` before each step and leaves an idle
  APU alone (`XHW_AUDIO_APU_IDLE_ALWAYS`). Smash Bros.'s frozen second
  boot: the next log names the step if it happens again.
- A graphics microcode with no version text is guessed from its display
  lists, task after task (`XGFX_UCODE_GUESS`). Jet Force Gemini's vote
  Fast3D: its 05/07 are Rare's own commands (the Perfect Dark-era GBI), so
  its menu stays wrong until that GBI is decoded (open).
- `-DXRC_PEEK=<addr>`: 32 bytes logged twice a second (diagnosis).

**The Controls page (maintainer request, 2026-10-18).** The BACK menu's
"Controls" row (per port) opens a page with the Xbox controller drawn on
the card and a callout per input naming the N64 control it gives
(`docs/look/controls-page-host.png`; `docs/architecture.md`;
`docs/decisions.md` "The Controls page"). Stored in settings.ini per port
(`a = a`, ... `right_stick = c_buttons`), `si.c` maps through it; the
default table is the old fixed layout (`XRC_CONTROLS`). Verified: host
tests (`test_settings`: defaults, round trip, values an input can't give;
`test_splash`: the page renders and its selection shows), and xemu (Super
Mario 64, slot 12, `build-host/scen/menuctl`): BACK, down to Controls, A
opens the page drawn through the NV2A; X changed to Z then L by Right and
A, logged `[MENU] port 1 X: Z`. `[MEM]` unchanged (the body layer is 219 KB
of static memory). Not verified: the console; ports 2-4 with pads in them.

**Pokémon Snap's camera (xemu, open).** The stick reaches the game:
`osContGetReadData` gives -80/+80, and the game's own pad records at
0x80048A08/0x80048A88 hold it (`XRC_PEEK`), Z (the left trigger) held
brings up the camera view. The stick moves nothing in xemu either, in the
normal view or the camera view (`build-host/scen/snap24`: LT held, the
stick left 2.5 s); the routine at 0x800AA3A0 that turns the stick into floats
(stick / 80.0, FR=0 pairs, checked) stops running when the course starts,
so the course reads the stick elsewhere. A read watch on 0x80048A90
(`XHW_WATCH_RW`) logged nothing in xemu, not even the writer, so the
reader is still to find (on the console, or by the ROM's references to
0x80048A88: two in the 0x8009A8C0 segment besides the copier).

**On red now (2026-10-18, deployed and size-checked; built from `e7ad98f`
plus the work above, ThinLTO, the ROM inside; `vsm8`, `vssb8`, `vzel14`,
`vbk13` booted 40 s in xemu with no crash).** The logs of the set they
replace were pulled first.

| build | game | xbe sha1 | what to look at |
|---|---|---|---|
| `vsnap6` | Pokémon Snap | 3f186b31273d | the Controls page; the camera (still open) |
| `vzel14` | Ocarina of Time 1.2 | dd8a6d8cc8ba | no lock-ups, less HDD noise (learn.txt batched) |
| `vge6` | GoldenEye | bbecb45fed5c | as `vge5` |
| `vjfg2` | Jet Force Gemini | b94c420f7526 | boot HDD noise (learn.txt); the menu stays wrong |
| `vssb8` | Smash Bros. | 53f8c2fe63b0 | boots after a stuck-audio boot; if it freezes, the last `[AUDIO] step:` line |
| `vmk11` | Mario Kart 64 | a28aa0292f91 | the controls through the table (no change meant) |
| `vsm8` | Super Mario 64 | 5125deb1e596 | the Controls page: a change, then play |
| `vmp11` | Mario Party | 9c9d74e83783 | its controls and logo (`vmp10` was never run) |
| `vmm10` | Majora's Mask | e0199fe8cd01 | boots past its first uncached write, or `[MEM] KSEG1 page ...` names the page |
| `vbk13` | Banjo-Kazooie | a9f0f56f44e6 | the START screen's textures (FB_READBACK=3) |

## 2026-10-18 (later): red runs the Controls set; the Controls page's speed, Mario Party's logo and board, Snap's camera lock (console + xemu)

Red ran `vbk13`, `vssb8`, `vzel14`, `vmp11` (logs in
`C:/xemu/hw/logs{bk13,ssb8,zel14,mp11}`; the other six left no logs, not
run). `[CLOCK] cpu 733 MHz` on every boot; `[MEM] after nv2a` free: Banjo
13812 KB, Smash 15620 KB, Ocarina 28248 KB, Mario Party 28356 KB.

| build | maintainer | the log |
|---|---|---|
| `vbk13` Banjo-Kazooie | textures much better; the START screen still shows static | 0 texture misses (RGBA4 fixed); readback mode 3 resolved 2 frames in 11.7 ms; the Controls page used (D-pad cycled, saved) at 14 fps |
| `vssb8` Smash Bros. | worked fine | two clean boots in a row: every `[AUDIO] step:` line passed, the APU left alone (idle) |
| `vzel14` Ocarina of Time | steadier, less HDD noise; audio skips a little; the pad rumbles a bit at scene changes; the frame rate still low | 19.9 fps (the game's own 20: display lists 6-13 ms, audio 6-7 ms); hitches of 443-665 ms at scene loads, each one a run of `safety net` lines (relocated actor overlays interpreted the first time) |
| `vmp11` Mario Party | stable, played to a board; the title logo blocky; the board black | 29.9 fps; 48 texture misses (the log's cap), all `fmt 0 siz 2 line 16 s 0-64`: a 65-texel tile over a 64-texel line |
| Controls page | "awesome" | 14 fps while open on Banjo (the rows page runs ~30) |

**Fixed since (xemu or host verified):**

- The Controls page's cost. Host timing (`padbench`, scratch): the page
  took 2.34 ms a frame against the rows page's 1.02; the controller's
  shapes were the difference (leader lines walked their whole bounding
  boxes with a 64-bit divide and a square root a pixel, the blend divided
  by 255 three times a pixel). Now the shapes skip what they cannot
  cover, the blend has no divide, and the controller and the callouts
  live in a cached premultiplied layer drawn again only when the pad,
  the selection or a mapping changes (`XHW_PAD_LAYER`): 1.10 ms, within
  3 levels of the old picture. `test_splash` checks a press shows and a
  release puts the picture back. Not verified: red's frame rate.
- Mario Party's title logo: its 188x5 RGBA32 strips come by LOADTILE,
  whose rows were counted at the 32-bit tile's line (one TMEM bank, half
  a row): two rows, stretched (`XGFX_LOADTILE32`). Right in xemu (title,
  test build); `test_f3dex` checks a 5-row strip.
- Mario Party's black board: a tile one texel wider than its line is cut
  to the line instead of refused (`XGFX_TILE_LINEW`). Host-tested; the
  scripted run did not reach a board in xemu: red will tell.
- `XGFX_DXT32` (an RGBA32 LOADBLOCK whose dxt does not divide its line):
  written for the logo before the trace showed it was a LOADTILE; kept
  behind its switch, no game known to need it.
- Ocarina's rumble: each motor change is logged now (`[PAK] port N:
  motor on/off at T ms`, the first 64), to tell the game's own rumble at a
  scene change from a short pulse held through a hitch.

**Pokémon Snap's camera (xemu, found; not fixed).** On the Beach the stick
reaches the game (0x80049758 = +80 / -80 under the autopad's SR / SL,
`-DXRC_PEEK=0x80049750u`, `XRC_PEEK2` is new). The course's camera code
(segment 0x80350200, ROM 0x4F0610) turns the view only while the flag
0x80382D0C is 0; it is 1 on our runs. The flag is set at the course's
start when byte 0x803AE515 is 1, which is when progress bit 0x100 is clear
(set once `func_800BF864()` returns 4 or more: a progress count), and a
process (0x803554EC) clears it once `func_8009BBF4()` (the word
0x800AE27C) reaches 3. That word is added to by `func_8009C9E8` from the
course (0x803537F8, when bit 4 of 0x803AE774 is set, with a subject
pointer). In xemu 28 photos with Butterfree in view left it at 0 and the
stick locked. Red's Snap save is blank (`npfe.fla: none yet`), so every
run there was a first run.

Why the count never moves (ROM read, not yet tried): the photo check is
the RDP's. Each frame the course (0x80359080) appends to its display
list a call to 0x80382DF0, which LOADTILEs the 8x8 texels at the
screen's centre (156..163 x 116..123, under the crosshair) of a 320-wide
RGBA16 image at segment 0xF, then a SETCIMG of an 8-wide RGBA16 image
(from a table at 0x803AE578) and a call to 0x80382E38, which copies
the block there with a COPY-mode texrect (scissor 8 wide); the CPU reads
the small image afterwards (0x80358E98 copies the result 0x803AE758..764
to 0x803AE768..774 under the interrupt mask; bit 4 of 0x803AE774 means a
subject). Here neither half reaches RDRAM: Snap builds without
framebuffer readback, so segment 0xF's picture is not in RDRAM, and an
off-screen colour image is drawn on the screen, never into RDRAM. Next:
segment 0xF is the colour image being drawn (logged: it alternates
with the two framebuffers). 0x80359244 then compares each pair of
consecutive copies and calls the Pokémon drawn between them the subject
when 32 or more of the 64 pixels changed. Started (not finished): a
software grid of the copied rectangle follows the frame's triangles with
their depths and each copy writes, per pixel, the copy it was last drawn
before (`XGFX_PROBE`, on by default; `test_f3dex` checks a triangle
between two copies changes all 64 and nothing changes none). In xemu the
probe turns on ("the game copies 8x8 at 156,116"), but 28 photos with
Butterfree in view still left 0x800AE27C at 0 and the camera locked.
Next: the test-build line `[GFX] probe copy N: n of 64 pixels drawn
since the last` (added, not yet run) says whether the Pokémon's
triangles reach the grid; then where the objects are drawn relative to
the copies. Not in staged set 14 (built before it).

**Banjo's START screen static:** not reproduced. The xemu scripts reach
the file select but not the game (`scen/bk3`..`bk6`: on slot 13 A picks
game 1 and the next A leaves "Press A to play" as it was; `bkgame` reached
the intro in an earlier session; red's `vbk13` got into the game with the
same input code, so this is the slot, not the pad). One run stalled
reading the ROM with three emulators at once.

Open from this round: Ocarina's scene-load hitches (its relocated actor
overlays: recompiling them is the cure; the next big item for its frame
pacing and the audio skips), Banjo's pause static, Snap's camera lock,
Jet Force Gemini's GBI.

**Staged for red (not deployed: its FTP did not answer).** Set 13
(`vsnap7`..`vbk14`, `deploy-set13.sh`) was replaced before it reached the
console by set 14 (`build-host/hwbuild14.sh`, `deploy-set14.sh`: makes
Kirby's folders, pulls the logs of the builds each replaces, deploys):
ThinLTO releases of `e7ad98f` plus this round and the Kirby round below
(the interrupt boost and the PAL region are in every one), each booted
40 s in xemu (`hwsmoke.sh`):

| build | game | xbe sha1 | what to look at |
|---|---|---|---|
| `vsnap8` | Pokémon Snap | 00c327bb20da | as before (the camera lock needs the photo check: open) |
| `vzel16` | Ocarina of Time 1.2 | 527ed07a425d | `[PAK]` lines against the scene-change buzz |
| `vge8` | GoldenEye | 1b4a4e3218c1 | as before |
| `vjfg4` | Jet Force Gemini | b199f3de7dcb | as before |
| `vssb10` | Smash Bros. | 3c809bff6b39 | as before (Gold: a regression check of the interrupt boost) |
| `vmk13` | Mario Kart 64 | a78936bde17c | as before |
| `vsm10` | Super Mario 64 | 957937fba34b | the Controls page's frame rate (Gold: a regression check) |
| `vmp13` | Mario Party | 367d3601ebaa | the title logo, the board |
| `vmm12` | Majora's Mask | 814eb5e26015 | boot (not run on red since `vmm9`) |
| `vbk15` | Banjo-Kazooie | 2459564a9855 | the START screen; the Controls page's frame rate |
| `vk1` | Kirby 64 (E) | f9adc6e2f628 | first console run (new folders `N64Recomp-X-K64`, UDATA 4E52F739): boot, the intro, the tutorial, 50 Hz pacing on a 60 Hz console |

## 2026-10-18 (night): Kirby 64 from the ROM alone (xemu)

Kirby 64 - The Crystal Shards (E) (`NK4P`, 32 MB, F3DEX2 2.04H + S2DEX2,
n_aspMain, EEPROM 4 Kbit) built as a test build with no learn logs
(`build-host/k64-shot.sh`, slot 14, `scen/k64`). The sweep had it at 5
frames, all threads waiting.

- **Its region lockout.** The boot block told every game NTSC; the PAL
  ROM showed "This game is not designed for use on this system" and
  looped there. osTvType comes from the header's region byte now, and a
  PAL game gets 50 retraces a second (`XRC_TV_FROM_REGION`,
  `XRC_PAL_50HZ`, `docs/decisions.md`). Every PAL ROM of the library
  was told NTSC before this: the next sweep should move.
- **A deadlock, about one boot in two.** After the intro the main thread
  (N64 priority 50) spins on its music player's state
  (`while (func_80020DB8(0))` at 0x800A77F8) waiting for the audio
  thread; the stall log showed the scheduler with an empty VI queue while
  retraces counted on: the VI thread was blocked on the runtime's lock,
  held by a priority-0 thread with its interrupts off that the spinner
  had preempted. A thread with interrupts off now runs at level 14 until
  it turns them on (`XRC_INTS_BOOST`). Before: 2 of 3 runs stalled
  (1331 frames); after: 4 of 4 ran on (2774-3007 frames by 75-80 s).
- **What it shows now** (xemu, 480i): the Dream Land intro (in its
  letterboxed window), Ribbon in space, "Do you know how to use Kirby's
  Copy ability?", the tutorial rooms (Kirby, a Waddle Dee, the move
  names). Open: a HUD icon garbled in one tutorial room ("Lift"); one CI4
  texture drawn with the TLUT off (the intro; read through its palette
  here, which the RDP may not do); the letterbox's vertical place under
  PAL's 288 lines; speed and sound on the console.
- Test builds can log every texture made: `-DXGFX_TEXLOG=<n>` (`[TEX]`,
  format, size, palette mode, the first two palette entries, the source).
  `XRC_PEEK2` peeks a second address. `osViSetEvent` is logged.

## 2026-10-18: the desktop app, the prebuilt toolchain, the logo (orchestrator + agents)

Maintainer requests of 2026-10-18: a downloadable app, "download, click and
run", no installer; a galaxy look; a game's tier and known problems shown
when its ROM is loaded; ask where to save; XBE or ISO before the build;
Xbox login `xbox` / `xbox` by default; a new logo. No console run (no
`[MEM]` / `[CLOCK]` lines: nothing here runs on the Xbox but the icon).

- **n64rx**: `info <rom> [--json]` (the header, its pack, `compat.report()`:
  tier, meaning, known problems, another region's row); `--events json`
  (step / note / done / log / result / error JSON lines; an internal error
  is an event, exit 3); `build --iso FILE` (a sixth step, the staging folder
  removed); `N64RX_HOME` (the cache, the portable app's data folder);
  `N64RX_FTP_PASS` (deploy's password off the command line);
  `N64RX_RECOMPILER_KEY` (`engines.py`: a bundle's prebuilt recompiler
  accepted without `tools/recomp`). `compat.py` lists every row of
  `docs/compat.md` with its problems in a player's words. The estimate is
  "about 1 to 3 minutes": measured SM64 38 s, OoT 41 s, an SM64 rebuild 17 s
  (Ryzen 7 5700X3D; `docs/app.md`). Host tests: `test_n64rx.py` 21,
  `test_compat.py` 12 rows.
- **Paths with spaces**: `xbox/CMakeLists.txt`'s `-include` and
  `RULE_LAUNCH_LINK` were unquoted (every build under a folder with a space
  failed: the app's data folder has one); quoted. `compile_game.py` read
  `.d` files split on spaces (every rebuild recompiled everything): parses
  make's `\ ` now. The test ROM built with the source, builds and toolchain
  all under spaced paths (`--fast` and ThinLTO); a plain checkout build is
  unchanged.
- **The toolchain bundle** (`tools/xbox/pack_toolchain.py`, Windows x64
  verified): python-build-standalone 3.12 + rabbitizer + Pillow, MSYS2
  trimmed by an allowlist and PE imports, nxdk (its wrappers' `${NXDK_DIR}`
  quoted in the staged copy only, `DIVERGES`), N64Recomp with its key;
  644 MB staged, 124 MB `.tar.zst`; unpacked to a path with spaces it built
  the test ROM with PATH cut to the bundle and System32 (18-21 s). Linux and
  macOS packing is written, not run (`.github/workflows/toolchain.yml`,
  manual, not run).
- **The app** (`app/`, Windows verified): the packaged app downloaded the
  local archive (`N64RX_TOOLCHAIN_URL=file://`), checked and unpacked it
  (59-65 s) and built the test ROM through its UI (20.6 s). The portable exe
  makes `N64Recomp-X Data` next to itself; the zip too. A code review (16
  findings: a screen race, a double-click double build, the FTP password in
  argv and logs, no single-instance lock, PYTHON* leaking into the bundle,
  archive links, ...) was fixed and tested; `npm test` 67 + 1 Linux-only
  skip, `npm run test:smoke` 8. Shots: `docs/look/app-*.png` (the fake).
- **The logo** ("Constellation X", `docs/look/logo/`): original art; the
  first draft's four same-handed side spurs read as a pinwheel and were
  removed (arms straight on the diagonals, mirror-symmetric).
  `tools/xbox/render_logo.js` renders `xbox/assets/logo.png` (replaces
  `make_logo.py`) and the app's icon; checked as the XBE stores it (128x128,
  1-bit alpha) and at 32 px.

Not verified: macOS and Linux (the app and the bundle); Send to Xbox and an
ISO against red; a real game built through the app (the CLI built SM64 and
OoT); the README's steps on a fresh machine per OS (CLAUDE.md, before a
release). Before a release: the repository (or a releases repository)
public, so the toolchain and the app can be downloaded; the toolchain's
GPL parts (MSYS2) and their source offer; unsigned builds (Gatekeeper's
right-click Open, SmartScreen); the portable exe unpacks to `%TEMP%` at
every start (the zip does not).

## 2026-10-18 (late): the deep-space loading screen, the holds swapped, Mario Golf's jumps, Mario Tennis's queues (console + xemu)

Maintainer requests, in order: a new loading screen (four mockups; A with
more space; A2 approved, then its console render approved); "hold
triggers for settings: the same menu as BACK, before the game"; then
"flip flop: L + R safe video, BACK the menu, both held"; the menu faster
and nothing popping in; Mario Tennis working well (next round).

| build | where | result |
|---|---|---|
| `vsm11` (2354cad07d90) | red | the first deep-space build: the loading screen starved the boot (controllers up in 10059 ms, the SHA-1 never ended, the watchdog's hang card) |
| `vsm12` (f6c90a1b4c9b) | red | the sky cached, the boot never starved: `[BOOT] loading screen: 6 ms a frame (30 frames; the first 64 ms)`, controllers up in 2915 ms, the SHA-1 896 ms, the game ran ("thats better and the game worked"); the title faded in late |
| `vsm13` (60c209582c16) | red | **approved by the maintainer**: the title on the first frame, played to the end of Bob-omb Battlefield ("worked fine"); `[BOOT] loading screen: 9 ms a frame (30 frames; the first 64 ms)`, controllers up in 3055 ms, the screen 4876 ms; the game 29.9 fps (display lists 3.5-6.4 ms); `[MEM] boot` free 53096 KB, `[CLOCK] cpu 733 MHz` |

`[MEM] boot` free 53100 KB (vsm11; image 9524 KB: the font tables add
~170 KB), `[CLOCK] cpu 733 MHz` on red.

**The screen** (`xhw_splash_draw.c`, `docs/decisions.md` "The deep-space
title card"): host render `docs/look/loading-space-host.png`; `test_splash`
49 checks (safe area for every hold and the settings states, titles on
two lines, the stage line with its step count, UTF-8 Latin-1, a cool dark
sky). xemu (`build-host/preboot-shot.sh`, slot 11, scenario
`build-host/scen/preboot`): BACK held -> `[BOOT] loading screen: BACK held
(the settings before the game)`, `[MENU] before the game`, Texture filter
changed, B -> `[MENU] closed before the game (settings.ini saved)`, the game
started; the dump shows the menu over the dimmed screen with "Start the
game". Not verified: L + R / L + R + START on red (xemu scenario lines are
in `docs/testing.md`, not run), the menu before the game on red, 720p after
a pre-boot change, the error cards in the new dress on red.

**The analyser** (`docs/decisions.md`): far `j`s decided after the walk
(Mario Golf's 0x8003327C stays whole on the host analysis; the six s9
compile failures are not rebuilt yet); osSendMesg/osRecvMesg without a
divide (Mario Tennis). `test_analyze` against the decomp pairs: see the
commit's notes (run with `N64RX_ROMS` at the NAS folder).

**Mario Tennis (`NM8E`, xemu, slot 10, `build-host/learnloop2.sh ... mt 10`):**
a Camelot boot at 0x80300000 that inflates the game into RDRAM; its
libultra is captured (`code_80031000`, `code_8003A000`, ...). Round 1 learnt
82 functions and 11 blocks; rounds 2-6 crashed at once (the queue rule
above); with the fix rounds 2-6 run to `[OS] stall: no graphics task at all
after 300 retraces`: thread 1 (pri 0) runs, last interpreted pc 80031254,
round 3 on learn nothing new (`code 1 func 0`). Next: what 80031254 spins
on (likely a value an interpreted or unnamed libultra routine should set:
the VI or SP), 258 functions in 5 segments, 70 libultra names.

## 2026-10-18/19 (night): Mario Tennis to a match from its ROM alone; captured code in versions; the six far-j games build (xemu)

The seed's priority: Mario Tennis (`NM8E`) working well. Everything here is
xemu only (slots 10 and 11); nothing new ran on red.

**Mario Tennis** (`build-host/mtshot.sh`: a test build with every
`mt-learn*.log`, scenario `build-host/scen/mt`: START, START, A through the
menus, then A to serve and hit; `LEARN=n` keeps the log as round n):

| round | what changed | result |
|---|---|---|
| 6 (start) | - | `[OS] stall: no graphics task at all`: thread 1 idles at priority 0, nothing else started |
| 7-8 | the second osStartThread named (`osStartThread__2`) | threads 0, 17-19, 4, 6, 3 start; audio (n_aspMain) |
| 13-14 | osVirtualToPhysical named (Camelot's has the probe inside); the refill skips the IPL3's leftovers at 0x80000000; Camelot's D-cache routines named | 45 frames, then a stall in a load |
| 18 | (the learn loop's stall stop was ending runs during loads; long runs: `STOP`, `SECS`) | intro (the blimp, a rally), the title at 30 fps, 1207 frames |
| 19 | input (the scenario) | Main Menu and Play Mode at 30 fps |
| 20-23 | versions of captured code, plausible starts, KUSEG guard reset, run-out functions interpreted, the KUSEG entry mapped before it runs (`docs/decisions.md`) | Character Select, a match (Mario v Luigi) |
| 24-25 | static callers in versions interpreted | to the match each run, no crash; the match ~12.6 fps in xemu (display lists 55-58 ms a frame: xemu's GPU, judge on red), one 15 s frame as the match loads |

The learn logs (`build-host/mt-learn*.log`, never committed) hold the
game's captured code; `build-host/mt-aside/` holds the rounds left out
(19 with its KUSEG captures, 20, 21's originals).

**The s9 compile failures** (`N64RX_J_LATER`): all six now build
(`build-host/sweep/s10j`); in xemu CyberTiger presents frames (1012-1058 in
30 s), Mario Golf 70-125 then stops (the Camelot engine: next, with the
Tennis fixes and its own learning rounds, `build-host/gameshot.sh mg`),
Clayfighter SC and Fighting Force no frames, A Bug's Life and Toy Story 2
read their ROM through KSEG1 (the cartridge window, `docs/decisions.md`;
A Bug's Life reads its data in place from ROM addresses, 1 MB and more:
a page window cannot hold that, and it bugchecked (0xA) when pages were
reused; it needs the whole ROM visible at 0xB0000000, open).

**Acclaim's loaders** (All-Star Baseball 2000, Jeremy McGrath; the s9
"No function at 80000400" class): their osMapTLB stack arguments sit at
0x80400000, past 4 MB, and the runtime read garbage there (the code at
0x00100000 mapped onto 0x1C0BB000). Those builds now get 8 MB
(`romonly._wants_8mb`, `N64RX_STACK_8MB`); not yet run.

**Host tests:** every `tools/xbox/test_*.py` passes (`test_interp` and
`test_mmio` got stubs for the new symbols), `test_analyze` against the six
decomp pairs: 0 failures. `test_compat`: 13 rows (Mario Tennis added,
Experimental).

**Later the same night (xemu):**

- **Mario Tennis**: the attract demo (no input) crashed at frame 1207 in
  every run: the framebuffer probe (Pokémon Snap's photo check) took its
  32x32 capture of the frame, 128 strip copies a frame into a 32-wide
  image, and wrote its marks a row past it into the archive handle beside
  it. The probe now takes only targets up to 16 wide and regions of two
  rows or more (`XGFX_PROBE_MAXW`, `XGFX_PROBE_ROWS`), clips to the copy
  rectangle and the scissor, and rasterises only a triangle's bounding box
  (`XGFX_PROBE_CLIP`): the demo runs (3101 frames, 110 s), and the match is
  25-26 fps in xemu (display lists 58 ms -> 19 ms).
- **Castlevania: Legacy of Darkness** (`ND4E`, the maintainer's request:
  "see if we can get that game working"): every boot ended differently
  until the AI_LEN clamp and the five other fixes in `docs/decisions.md`
  ("Castlevania: Legacy of Darkness from its ROM alone"); now four boots at
  once run the same: intro, title, Game Start, the new-game opening, and
  Foggy Lake with Cornell, the HUD and the pause menu (3 learning rounds,
  `build-host/gameshot.sh cvlod`, scenario `build-host/scen/cvlod`).
- **A Bug's Life** reads its whole ROM through KSEG1: mapped there now (no
  bugcheck, no fault); it stalls before its first frame (thread 4 in an
  interpreted loop at 0x8006F0A0: open).
- **Regression sweep** (`build-host/regress.sh`, 70 s each, no input):
  Pilotwings 64, Super Mario 64, Smash Bros., Mario Kart 64, Ocarina of
  Time, Mario Party, Pokémon Snap, GoldenEye, Banjo-Kazooie, Kirby 64 all
  present frames with no crash; Majora's Mask crashed at frame 104: the
  tail-call change, fixed (a `jr` to where `ra` points is a return; 1251
  frames, no crash).
- **Set 15** (`build-host/hwbuild15.sh`, `deploy-set14.sh`'s successor
  `deploy-set15.sh`): set 14's games at HEAD plus Mario Tennis (`vmt1`,
  `N64Recomp-X-MT`, `4E5247BD`) and Castlevania: Legacy of Darkness (`vcv1`,
  `N64Recomp-X-CV2`, `4E522C29`).

**Set 15 staged** (`/c/xemu/hw/stage-*`, built at the working tree after
these changes; red was off, nothing deployed: `bash build-host/deploy-set15.sh`
when it is on): vsnap9 `3d86890969d0`, vzel17 `da3dbcb1186b`, vge9
`4c0b3432e51e`, vjfg5 `c9a4f555569e`, vssb11 `ccaee183d9e5`, vmk14
`99e557677b9c`, vsm14 `b05a94114a26`, vmp14 `dcd31f981075`, vmm13
`142d1e3ab0fb`, vbk16 `b3067a106858`, vk2 `20cfebf6facb`, vmt1
`ffc95659d1c6`, vcv1 `0db24b0ebfea`. The release XBEs of vmt1 and vcv1 boot
in xemu (1564 and 1700 frames in 45 s); `[MEM] boot` free 53240 / 53592 KB,
after nv2a 14028 / 14124 KB, `[CLOCK] cpu 733 MHz` (xemu).

**Not verified:** anything on red; Mario Tennis's sound in a match; the
cartridge window on any game that reaches frames; the 8 MB rule on
Acclaim's games.


### Acclaim's KUSEG games, and xemu's slow memory (2026-10-19, later; xemu only)

**All-Star Baseball 2000 (`NBEE`)**, from the ROM alone plus 26 learning
rounds: Acclaim's loader maps KUSEG 0x00100000 onto physical 0, inflates
the game there and jumps in; the game has its own libultra and pages more
code in through 4 KB TLB entries from its fault thread. It now shows its
legal screens, the title ("Expansion Pak detected"), Main Setup,
Controllers and the line-up screen at ~20-22 fps in xemu, and plays a game
(batting, pitches; ~22 fps). It froze when a game loaded: its audio
thread, woken only by OS_EVENT_AI, got the event only from inside osAi*
calls and made ~6 frames a second; the sound events its game thread
posted overflowed the player's queue, the player's own tick was dropped
and the audio thread spun on the empty queue (found with a test-build dump
of the queue, `-DXRC_DIAG_EVTQ`). The AI event now comes per buffer and
every retrace. The stadium's sky and grass drew black: drawn under
`G_RM_OPA_SURF` with a combiner alpha of 0, which the renderer blended
(FORCE_BL alone turned blending on); a one-cycle blender that never reads
the frame now draws opaque (`XGFX_BLEND_MEM`, `docs/decisions.md`).

What it took (`docs/decisions.md` "All-Star Baseball 2000: a game linked
in KUSEG"): libultra's globals found in KUSEG and used through the TLB;
osStartThread lets a faulted thread go (its pager); the guard rechecks only
the KUSEG entries a TLB write or DMA reaches; AI_STATUS busy/full from the
virtual DAC (it filled the FIFO forever); captured code asks the console
for its jump tables' and callees' pages (`want.txt`); a gap function no
longer starts on a data word before a prologue; a replaced libultra
function is never also "interpreted"; the Controller Pak is found (AUTO
answers as both paks).

**xemu's slow memory.** In xemu, reads of physical 0x03631000 to about
0x03880000 (where an 8 MB window's low half lands) ran ~100 times slower
than the rest already at boot, and slower later (`-DXRC_MEMBENCH`: 8-16 ms
per 64 KB against 50 us). All-Star Baseball's audio tasks took 12 ms there
and its game thread never ran; Majora's Mask was slow at boot there.
Test builds now place an 8 MB window under 48 MB physical
(`xhw_sys.c`, `-DXHW_RDRAM_HIGHEST`); release builds keep the top, as the
console's memory is uniform. The cause inside xemu is not known. A 4 MB
window stays at the top (Mario Kart 64 and Banjo-Kazooie read past their
4 MB into what the low place left unmapped: crashes, fixed).

**Jeremy McGrath Supercross 2000 (`NCOE`):** the same loader; 9 learning
rounds. Its own PI manager waits for the PI interrupt after each raw DMA
(the runtime's raw starts now post it, `-DXRC_PI_RAW_EVENT`); it then
draws 5 frames and crashes: its game objects are cooperative processes
switched by its own routines (`0x0025E9D0` saves a context, `0x0025E94C`
loads one and jumps, `0x0025E8E8` yields, `0x0025E8C0`/`0x0025E8D4` save
and restore the main stack), Mario Party's setjmp/longjmp in another
shape. Recompiled, a switch returns into the wrong host frame (s0 came
back as 0x474500B1). Needs the strands (`strand.c`) taught this shape,
including the return to main. Open.

Regression (xemu, slots 11-14, test builds with all of the above): Super
Mario 64, Pilotwings 64, Super Smash Bros., Mario Kart 64, Ocarina of Time,
Majora's Mask (1178 frames, was 104 before the jr-to-ra fix), Pokémon Snap,
Mario Party, GoldenEye, Banjo-Kazooie, Mario Tennis, Kirby 64 all present
frames with no crash (Mario Party and Smash Bros. rechecked after the
int-mask lock was switched off: 1551 and 2964 frames in 60 s, as before). Host tests pass; test_analyze 0 failures.
[MEM]/[CLOCK]: xemu only, nothing new on the console.


### Relocated overlays recompiled; more Acclaim games (2026-10-19, later; xemu only)

**The Zelda engine's overlays** (`docs/decisions.md` "Overlays the game
relocates itself, recompiled"; `analyze/zovl.py`): found from the ROM
alone (Ocarina of Time 468, every link address the decomp's; Majora's
Mask 612), recompiled as relocatable sections, mapped by a hook at the
game's relocation routine (`ovl.c xrc_ovl_relocate`). Only the overlays
a learning run reported (`reloc` lines) are recompiled: all of them made
Ocarina's image 17 MB. In xemu with one learning round: Ocarina 3137
frames in 120 s (15 overlays, 938 functions) against 1950, through the
title, file select, the opening narration, Ganondorf's nightmare and the
Deku Tree calling Navi in 200 s; Majora's Mask 2040 frames in 85 s (45
overlays) against 1887 in 105 s. A first version stalled Ocarina in
Link's house: a smaller overlay loaded over part of a bigger one left the
rest of the old one mapped, and an overlay not yet learnt cleared nothing;
now each relocation clears the range its header gives and unmaps every
overlay that overlapped it, whole. Not verified: the console; frame
pacing at scene loads measured on the console (the reason for this).
Open: the learnt RAM captures of relocated code from earlier rounds stay
in the build beside the sections (harmless: the relocation clears them).

**More Acclaim games** (the same loader at 0x80300000): All-Star Baseball
2001 (14 rounds) and '99 (6 rounds) play a game; South Park: Chef's Luv
Shack (9 rounds) reaches Who's Playing and its menus (its logo draws
wrong); a lower RDRAM place first put its KUSEG aliases on the heap
(test builds now use under 48 MB physical: 0x02800000); its
exception code's `__osRunningThread` now comes from beside
`__osFaultedThread` (its getters named another word, `saves.py
consistent_threads`). Jeremy McGrath Supercross 2000 still needs its
process switcher.

**The library sweep s10** (`build-host/sweep/s10`): 144 of 246 still
presenting at 30 s (s9: 132), 15 gained, 3 lost (`docs/analyzer.md`).


### Pokémon Snap's camera lock, narrowed (2026-10-19, late; xemu only)

With the decomp as the key (`app_level/pokemon_detect.c`, `sys/sched.c`):
the detector is enabled with 7 regions a frame (`NumPokemons`), the probe
answers every copy (`[GFX] probe copy N: n of 64 pixels drawn since the
last`), `scPostProcessFunc` (0x80048890) is set to
`PokemonDetector_PostProcessImage` once the course loads and stays set,
and that function is entered (`-DXRC_TRACE_COUNT=func_803589D0_4F8DE0`).
Yet `PokemonDetector_HasPokemonInFocus` (0x803AE758) stays 0 and its
message queue (0x803AEF48) stays full. Next: the compare in
`FindPokemonInFocus` (regions `i - 1` against `i`, 32 of 64 differing):
whether the regions the probe wrote are the ones it reads after its
`osInvalDCache` (the runtime's cache invalidation may hand the RDP's
picture back over them: `f3d_fb_inval`), and why the queue is never
drained (its `osRecvMesg` NOBLOCK). New diagnostics: `-DXRC_PEEK_EVERY`,
`-DXRC_PI_LOG_ALL`, `-DXRC_TRACE_COUNT` (`docs/testing.md`).

### Pokémon Snap's camera lock is the tutorial; the photo check works (2026-10-19, night; xemu only)

Correction to the entry above: the detector's queue (0x803AEF48) is not
full. Its peek reads `validCount` 0, `msgCount` 2 (the 2 was the
capacity), so `PokemonDetector_PostProcessImage` gets each frame's
message and runs `FindPokemonInFocus`.

The lock is the game's own: on a new save the Beach starts the tutorial
(`updateTutorialMain`, `IsInputDisabled = true`: no stick, no dash, no
C-buttons) until `D_800AE27C`, the photos taken with a Pokémon in focus,
reaches 3. When a Pokémon is in focus the tutorial stops the course and
prompts "Press Z to aim", then "Press A". Without a detection there is no
prompt and the camera never unlocks.

Four runs, xemu slot 14, the same test build (`-DXRC_PEEK=0x803AE758u
-DXRC_PEEK_EVERY=1`), the same input to frame 6103 (`scen/snap27`'s route
through the lab, then A every 2 s on the Beach):
- run 1: at frame ~6044, 39 s into the course,
  `PokemonDetector_HasPokemonInFocus` = 1, Snorlax (id 0x8F, flags 0x3D:
  `POKEMON_FLAG_4`, a photo subject). The tutorial process (0x803552B0)
  starts and the course holds on "Press Z to aim" to the end of the run.
  The photo check works end to end in the port.
- runs 2 and 3 (Z+A pressed later; film 60 to 40): no Pokémon in focus,
  the photo count stays 0, the camera stays locked, and the course ends
  back at the lab.
- run 4 (run 1's input again, with the new summary line): Pidgey (id
  0x10, flags 0x25) in focus at frame ~5070, 7 s into the course. Between
  frames 4992 and 5082 the Pokémon copies reached half or more 22 times
  (up to 64 of 64). After that the tutorial holds with one Pokémon at 56
  of 64 every frame.

So the probe gives the game what it compares, and the tutorial finds its
subject. The same input finds a different Pokémon at a different time
(Pidgey at 7 s, Snorlax at 39 s, or none in runs 2 and 3) because the
Pokémon move on the game's timing. The first 40 lines of the earlier
runs (8 of 64) were the course's first frames, not the whole run.
Left to show: the unlock itself. Once the prompt is up, Z held and A
pressed take the tutorial's 3 photos (`D_800AE27C` to 3), then the stick
turns the view. That takes a scenario that waits for the prompt, or a
player: on red, follow the prompts. New test-build line: `[GFX] probe
frames A-B: n later copies, the most m of 64, k at half or more` every 30
frames. A frame's first copy now has its own 8 lines and no longer uses
up the 40. Run scenarios: `build-host/scen/snapd` (A only), `snapz`,
`snapz2` (Z held with A); untracked.

## 2026-10-19 (evening): set 16 on red, its failures, set 17 fixes (red + xemu)

**Red ran set 16** (`build-host/hwbuild16.sh`, `hwbuild16k.sh`,
`deploy-set16.sh`; the working tree, not xemu-smoked first; logs pulled to
`C:/xemu/hw/logs{k4,mt2,ge10,mp15,mm14,cv2,sm15,ssb12}`). The maintainer:

| build | result | cause, fix |
|---|---|---|
| `vk4` Kirby 64 (USA, `NK4E`, new) | runs; "major texture failure" | open: nothing in the log, not seen in xemu (the same intro draws right there). A likely cause: the stale page-directory entry below (left by an earlier XBE run) pointing at a page that is now Kirby's texture pool or RDRAM, our KSEG1 PTEs written into it; set 17's own tables avoid that. Next time BACK + WHITE screenshots (`screenshots = 1` in settings.ini) |
| `vmt2` Mario Tennis (new) | a white screen, frozen | two causes: its uncached RDRAM's page table overwritten (the log stops at the 16th KSEG1 heal) and, in release builds only, a NULL message from `osSetEventMesg`'s race killing its own VI manager at frame 2 (reproduced in xemu with the release XBE); both fixed (`docs/decisions.md`), `vmt4` staged, 55 s clean in xemu |
| `vge10` GoldenEye | fine; dips to ~40 fps in intense scenes | the 0x70020000 page learnt (round 31): interpreted instructions 1.5M -> 53K per 5 s |
| `vmp15` Mario Party | froze at the board's start; textures shuffled on the board | the strands of ended processes never freed (fixed: `XRC_STRAND_GC`, `vmp16` staged; xemu reaches the board); the shuffle open (`[GFX] texture miss (no load)`: a tile with line 0, at the board's first frames) |
| `vmm14` Majora's Mask | black after the Nintendo logo, crash | a page table left by the XBE run before (Mario Tennis's) inside its RDRAM window (fixed: own tables, cleared on exit; `vmm15` staged) |
| `vcv2` Castlevania LoD | crashed ~70 s in | reproduced in xemu (frame 10584, `func_8001AB28`'s s0 0 after a call into its object teardown): open, being bisected |
| loading screen | the ID chip had odd characters | a double-encoded middle dot in `xhw_rom.c` (fixed) |

`[MEM]`/`[CLOCK]` on red (`vmt2`): boot free 50188 KB, after nv2a 12204
KB, cpu 733 MHz.

**Set 17 staged** (`build-host/hwbuild17.sh`, `deploy-set17.sh`; rebuilt
2026-10-20 ~05:30 with everything in the overnight entry below, Diddy
Kong Racing added; ThinLTO release; all 15 smoke-booted 45 s in xemu,
every one presenting, no crash; not deployed, no hardware testing
tonight): vsnap11 `d39ec2e4f69c`, vzel19 `a8de229c6897`, vge11
`008633e4d249`, vjfg7 `733f5ca22015`, vssb13 `3f62dade2070`, vmk16
`40727e8b598c`, vsm16 `8b0728448c81`, vmp17 `c51a68523f9f`, vmm16
`23e9ddfdba3f`, vbk18 `f8b13d58c401`, vk5 `a3a6e896d3c1` (Kirby 64 USA),
vmt5 `d84a91eb3730`, vcv3 `66e770db821c`, vsf1 `64659b4bdcf8` (Star Fox
64 US 1.1, new: `-DXRC_START_DEFER=2 -DXRC_RANK_LEVELS=1`, title ID
4E5260B9), vdkr1 `632f65a16d48` (Diddy Kong Racing US 1.1, new: Rare's
DKR microcode, title ID 4E52E856). To look at: Mario Tennis boot
and a match (`vmt5`), Majora's Mask past the Nintendo logo (`vmm16`), Mario
Party's board (`vmp17`), Castlevania past a minute of play (`vcv3`),
Kirby's textures with BACK + WHITE screenshots (`screenshots = 1` in its
settings.ini), Star Fox 64 past its intro (`vsf1`), Diddy Kong Racing
(`vdkr1`), Jet Force Gemini's menus (`vjfg7`), the loading screen's
ID chip, the Gold games as a regression check, and a relaunch between
games (the stale-table fix). (`vmp16`, `vmm15`, `vmt3`, `vmt4` were interim builds.)

**Also today (xemu):** the display-list replay (`tools/xbox/bench_dl.py`,
`docs/testing.md`), `-DXRC_DLBENCH`; GoldenEye's per-frame interpreter cost
~8-10 ms in xemu against ~1.5 ms for the back end; patches/recomp/0002
(lookup jals: Mario Tennis 385 -> 129 interpreted functions); Kirby 64
(USA) from the NAS (`Kirby 64 - The Crystal Shards (U) [!].z64`): its
opening story at ~50 fps in xemu; Star Fox 64 (US 1.1) from a boot hang to
its space intro (start deferral, `osSpTaskLoad`'s task for a raw SP start,
page 0 read as zeros), then a stall at its first scene load (open). The
regression set (14 games) presents frames with no crash after all of it.

### Star Fox 64 (US 1.1, `NFXE`) from its ROM alone (2026-10-19, night; xemu only, open)

The sweep skipped it (a duplicate of Lylat Wars (E), which stops at boot).
With the sf64 decomp as the key:
- boot hang: its graphics thread (40) ran beside its main thread (100) on
  level 10 and sent a DMA message into `gDmaMesgQueue` before main made the
  queue: `-DXRC_START_DEFER=1` (off by default) lets it reach frames;
- no task ever ran: the debug monitor's `__rmonStepRCP` made
  `__osSpSetStatus` ambiguous, so `osSpTaskStartGo` went unnamed and its own
  copy started the SP through SP_STATUS with no task block in DMEM: the
  rule now wants the written value to be the argument (both named), and
  the runtime's `osSpTaskLoad` leaves its task for a raw SP start
  (`XRC_SP_LOAD_TASK`), run as `osSpTaskStartGo` runs one;
- its audio heap's uninitialised `entry` read (a decomp `@bug`) crashed the
  audio thread: page 0 through KUSEG now reads as zeros (`XRC_NULL_PAGE`);
- its scenes are decompressed over each other by the CPU: the learnt DMA
  overlay at 0x80187520 is guarded now (`N64RX_GUARD_LEARN`).
It then plays its intro and the opening cinematic ("Great Fox", General
Pepper's message) in xemu, and stops at a scene change: its main thread
(the scheduler, 100) lags on level 10 beside four lower threads, the audio
thread queues three tasks between two of its drains, and the game's
`sNewAudioTasks[]` overflows into `sNewGfxTasks[]` (the graphics task is
lost; the N64 never lets the scheduler lag). `XRC_RANK_LEVELS=1` would keep
them apart but brings the boot race back (0 frames): open.

2026-10-20: the race with ranked levels was the same race one level
apart. `osCreateMesgQueue` now logs a queue made again over a sleeper, and
named graphics, timer and serial asleep on queues main then made: a host
block in main (a log line, the lock) let the lower levels run.
`-DXRC_START_DEFER=2` holds a started thread of lower N64 priority until
its starter sleeps (or lowers its priority, or 200 ms), whatever the
levels. With `-DXRC_START_DEFER=2 -DXRC_RANK_LEVELS=1` Star Fox 64 plays
its intro and the story past the scene change (1716 frames at 105 s,
xemu under load at 5-7 fps, no stall). The regression set with both (14
games, 70 s, xemu): every one presents, no crash, most a little ahead of
the baseline (Pilotwings 3125 against 2430; Mario Tennis 1322 against
1679, no input scripted). Both stay off by default: the console-proven
games move levels under them (Super Mario 64, Smash Bros., Mario Kart 64),
and only a console run decides that. Set 17 carries a Star Fox 64 build
with both (`vsf1`, title ID 4E5260B9).

## 2026-10-20 (overnight, no hardware): stale tables, Star Fox's threads, division, learning (xemu only)

- **Adopted page tables start empty** and are never handed on at exit
  (`docs/decisions.md`; `XHW_ALIAS_ADOPT_COPY=1` restores copying). The
  likeliest common cause of set 16's console-only failures: Majora's Mask,
  Mario Tennis's white screen, and perhaps Kirby 64's and Mario Party's
  textures (both draw right in xemu, both ran after another XBE on red).
  Not verifiable without the console: xemu leaves no tables behind.
  Majora's Mask in xemu with it: 621 frames, the quit combo at 40 s.
- **Star Fox 64** past its scene change with `-DXRC_START_DEFER=2
  -DXRC_RANK_LEVELS=1` (see its section above); off by default.
- **Integer division by zero** gives the VR4300's result
  (`patches/recomp/0003`, `XRC_DIV_VR4300`): Vigilante 8 ran 2415 frames
  past the frame (1592) where the sweep saw it fault.
- **Learning:** a changed function is recaptured whole and its pages kept
  together (`XRC_RECAPTURE_SPAN`, `N64RX_LEARN_GROUPS`). Mario Tennis's
  match: interpreted instructions per 300 retraces 1.06-1.58M -> ~770K
  (rounds 30-32 from `scen/mt`, a match each), image 12.9 MB, 11.7 MB free
  after the NV2A (xemu). What is left (0x800F02B4, its jump table 300 KB
  away in another capture; 0x10000000, a branch past its segment) is ~1%
  of a 733 MHz CPU.
- **The analysis is cached** in the build folder (`N64RX_ANALYSIS_CACHE`):
  Mario Party's 6-minute analysis once per ROM and n64rx version.
- **The sweep learns** (`tools/xbox/sweep.py --learn R`): rebuilds each game
  that learnt something with its boot logs and boots it again.
- Regression set (14 games, 70 s each, xemu, with rank levels and
  start-defer on): all present, none crash.
- Mario Party's board in xemu: right. Its 36 "texture miss" lines (an
  all-zero tile 0, the board's intro) were shade-only quads (combiner
  `fcffffff fffe793c`) drawn with G_TEXTURE on: harmless, and no longer
  looked up (`XGFX_TEX_UNREAD`). Test builds now log the 16 commands
  before the first misses; `-DXGFX_TRACE_MISS=1` (trace the frame after a
  miss) did not fire there (not looked into further).
- Kirby 64's console textures: its texture pool is healthy in xemu (3.6 MB
  free, no failed allocation); nothing more without console screenshots.
- **Lylat Wars (Star Fox 64 PAL)**: its 7102 boot chip loads at
  0x80000480, not the header's 0x80000400 (`N64RX_IPL3_7102`): from 3
  functions found to 1701, and 2907 frames in 120 s in xemu (with the
  Star Fox thread switches).
- **Shadow Man**: its entry maps only an odd TLB page (`N64RX_TLB_ODD`):
  from no code found to 457 functions; it boots and then takes a TLB miss
  at 0x00800004 (from 0x00117730) before its exception code is in (the
  vectors still hold IPL3's): open. Three learning rounds learnt nothing
  more.
- The sweep's games with a big plain boot segment and few functions
  (Virtual Pool 64: 58 of ~1900; Starshot: 77) copy their code out of the
  boot image with the CPU (no DMA for the analyser to find): the learning
  loop's captures are the way in.
- **Code the walks missed** (`N64RX_PROBE`): Virtual Pool 64 from 58 to
  1839 functions and 2791 frames in 70 s (none in s10/s11); Starshot from
  77 to 1558. The six decomp pairs are unchanged.
- **gp for new threads** (`XRC_THREAD_GP`): Starshot no longer faults on
  its first gp-relative store; after two learning rounds it presents
  frames (3028 in 50 s of game time, xemu; s10/s11: a crash at boot).
- **Sweep s11** (the tree of 2026-10-19 ~23:40: page 0 as zeros, the
  strands' GC, the osSetEventMesg order, start-defer off; first boots only,
  30 s): 147 of 246 present frames (s10: 144): Buck Bumble, Donald Duck
  Goin' Quackers, Rugrats in Paris newly. Five games now fault reading N64
  0x00001000 where they read page 0 before (Air Boarder 64, Chopper
  Attack, Extreme-G, Mortal Kombat 4, Iggy's Reckin' Balls: compressed
  loaders whose own dispatch goes wrong; learning first). **Sweep s12**
  (tonight's tree, `--learn 2`) is running from `../nrx-s12`, slot 10.
- **Paper Mario**: round 4 reached 2257 frames (stopping at an effect at
  0xE006C298 with no function); its log adds a DMA of code into the boot
  segment, which broke the next build (`N64RX_BOOT_PLAIN` fixes that).
  Rounds 5-6 then crash at frame 691 in `func_80243FC4_164824`: a start
  learnt in another map's overlay applied to this one (Paper Mario loads
  hundreds of maps at 0x80240000; learnt starts are not tied to the
  overlay they came from). Fixed (`N64RX_FUNC_DMA`): round 5 again runs
  ~2390 frames and stops at its first effect (0xE006C298, mapped through
  its TLB at 0xE0xxxxxx; learn.c logs the effect's DMA by physical
  address, so nothing ties it to that address yet). Then
  (`XRC_HIGH_SAFETY`): its effects run in the safety net; rounds 6-7 reach
  ~2475 frames and stop at a null-pointer store in its battle code
  (`func_80263C60`, frame ~5166). Round 8 recompiles six effects from the
  ROM's effect table (`high_load`) and runs 3139 frames in 70 s with no
  crash (xemu; the battle store not reached or gone: unknown).
- **Jet Force Gemini's menus** (`XGFX_RARE_DLINMEM`): Rare's opcode 0x07
  (n commands from memory) carried every texture rectangle's tile setup;
  its menu text and backgrounds drew as grey boxes. The main menu now
  draws whole in xemu (PLAY / MULTI / OPTIONS, their icons); no texture
  miss in 100 s. Its audio (Rare's microcode) is still to look at.
- **Diddy Kong Racing renders** (`XGFX_DKR`: Rare's DKR microcode, the
  encodings from the DKR decomp's `f3ddkr.h`, CC0): its intro (the
  characters on the beach) and the title's 3D flyby draw whole in xemu,
  where they were grey boxes; its Game Select and initials screens (the
  3D letter carousel) right, and on into a 3D scene of the game (night
  sky, terrain, plants) with scripted input (`scen/dkrplay`). Not on the
  console: `vdkr1` added to set 17 (title ID 4E52E856). Its audio lost
  every SAVEBUFF near the top of RDRAM (the HLE's 4 KB span ran past 4
  MB: `XRC_ASP_SPAN_END`); fixed, `vdkr1` rebuilt (`632f65a16d48`,
  `hwbuild17-dkr.sh`).
- **Regression set on the final tree** (2026-10-20 ~05:45; 14 games, 70 s,
  xemu, default switches): every one presents (Kirby 64 (E) once stuck
  at "Reading the ROM" with four xemus running, 2825 frames on a rerun);
  no crash.

## 2026-10-22: a roadmap and a code review of dev at ba03cd1 (host only, no hardware, no xemu)

- **`docs/roadmap.md`** (new): from seven games to the top 100 and 25
  Gold. The record's failures sorted into seven cross-game buckets ranked
  by games moved per week of work (B1 a run token for libultra's thread
  semantics, B2 a build-time dry boot for code the analyser cannot see, B3
  microcodes, B4 framebuffer effects, B5 code on demand, B6 the M3
  measurement and the `restrict` lever, B7 unnamed libultra); six phases
  with exit checks; the 25 Gold candidates and the gap for each; the
  top-100 appendix (the record, the bucket, the No-Intro and mupen64plus
  facts, engine notes to verify); questions for the maintainer.
- **`docs/review-2026-10.md`** (new): the code review of `dev` at
  `ba03cd1`, each item CONFIRMED or PLAUSIBLE with file:line: the host
  tests, the generated code's aliasing cost, the PC tool and the app
  (security, supply chain, robustness), the analyser, the runtime core,
  the libultra layer, the renderer, the platform layer, licences, docs
  debt, test coverage. The highs: 3.1 (`n64rx` deletes an existing folder
  named like the ROM's header title), 1.1 and 1.2 (host tests red on
  Linux), 5b.1 (no retrace message to a 1- or 2-slot VI queue), 5a.1
  (`section_addresses` overrun), 4.1 (the naming rules' uniqueness check);
  the roadmap's section 8 ranks the top 13.
- Verified how: the host tests run on Linux (18 of 22 entries green;
  `test_mmio`'s link failure and `test_f3dex`'s GCC `-Werror` failures are
  review items 1.1 and 1.2; `test_splash` needs nxdk, `test_analyze`
  checked 0 pairs with no ROM); a codegen experiment with the project's own
  flags and `recomp.h` on the generated code's contract (with `restrict` on
  the two parameters a four-instruction MIPS fill loop goes from 6 x86
  instructions with 4 memory operands at 32-bit registers, 15 with 8 at
  64-bit, to 4 register-only ones in both: roadmap B6); the display-list
  interpreter fuzzed
  on the host (the real `f3d_interp.c` and `texconv.c` over an RDRAM window
  with guard pages, 12,600 lists: three overrun classes, no output-contract
  violation; review 5c); SHA-1 and the XBE section finder checked on the
  host (review 5d.1); host harnesses linking the real runtime and libultra
  sources against stand-ins (review 5a, 5b).
- Not verified: nothing ran on a console or in xemu; no ROM (synthetic
  ROMs and hand-written MIPS only); LLVM 21.1.8 and nxdk are not on the
  host (clang 18 and GCC 13 stood in). No code was changed.
- Gaps: the renderer, platform layer and audio HLE reviews are shallower
  than the rest (their reviewers were stopped: review 5c, 5d, 9); the
  `nv2a_cc.c` combiner cross-check against an RDP model was lost and must
  be redone. The top-100 facts come from the No-Intro DAT and the
  mupen64plus database, not from carts; the engine column waits for the
  sweep's microcode columns (roadmap B3).

## 2026-10-22 (night, no hardware): the roadmap's phase 0 and phase 1, the dry boot, the float mode (host + xemu only)

The maintainer merged the roadmap PR and asked for it to be worked through
overnight. Their answers before going: commit locally, never push; the
run token (B1) and the dry boot (B2) built behind switches that default to
off; packs may ship learnt facts (addresses and sizes, no bytes); xemu and
full sweeps allowed; no hardware; no subagents. Commits 28dacab..6c96027
on `dev`, none pushed.

- **Phase 0 (review fixes)**: the highs and most mediums of
  `docs/review-2026-10.md`, each behind a switch where it changes behaviour
  (`docs/testing.md`'s table):
  - 3.1: `n64rx` only replaces a folder carrying its own `.n64rx-output`
    marker.
  - 5b.1: `XRC_VI_RESERVE_SMALL`. 5a.1: `section_addresses` sized by the
    largest index.
  - 5b.2, 5b.23: `XRC_PI_BOUNDS`. 5b.3: `XRC_RCP_ORDER`. 5b.4:
    `XRC_INTERP_COUNT`. 5b.5-7: `XRC_STOP_UNLOCK`, the bounded start wait,
    `XRC_PRI_LOCKED`.
  - 5b.8, 5b.12: the EEPROM and Pfs lengths. `XRC_SAVE_COPY`: images
    written outside the lock.
  - 5b.15: 4 KB of slack before the register shadow. 5b.19: a dispatched
    thread's gp. 5b.22: rank_update's bound. 5a.2: `XHW_FIXUP_STORM`.
  - 5c.1-4: the display-list bounds and budget.
  - 1.1, 1.2: the host tests under GCC and Linux.
  - 3.r1-r9: `n64rx` robustness.
  - `rom_leak_check` hardened (archives, shifted and encoded headers,
    generated code, images by hash) with a pre-commit hook.
  - `[PERF]` split by phase and a `[PERF2]` line (`XHW_PERF_DETAIL`), with
    `ab_report.py`.
  - `restrict` on the generated code (`N64RX_RESTRICT=1`, opt-in): 7.6 %
    fewer instructions and 12.3 % fewer memory operands, counted statically
    in Pilotwings' code.
  - L3DEX/L3DEX2 lines drawn.
  - The analyser's probe pass for second fits (`N64RX_NAME_UNIQUE`,
    `N64RX_NAME_BITS`, opt-in).
- **B1, the run token** (`XRC_RUN_TOKEN=1`, off): libultra's one thread at
  a time in `ultra/thread.c`; host harness `test_thread` (pthreads, both
  ways). xemu: all 14 regression games present with it, no crash.
- **B2, the dry boot** (`N64RX_DRYBOOT=1`, off; `tools/dryboot/`,
  `docs/analyzer.md`). It is a CPU-only N64 on the PC that writes the
  console's learn files before the first build. First builds from the ROM
  alone: GoldenEye 3,797 functions in 7 segments (30 console rounds:
  3,821); Banjo-Kazooie 6,567 in 5; Dr. Mario 64's gzip main segment 998
  of 1,006; Blast Corps 1,508; Perfect Dark 2,286 in 38 (2 without).
  - xemu, dry-boot build against the console-learnt build, 90 s each:
    every build presents with no crash; at boot they learn 11/0 (Banjo),
    13/6 (GoldenEye), 9/2 (Blast Corps), 4/4 (Dr. Mario) and 262/6 (Mario
    Tennis, whose matches the dry boot never reaches).
  - Library: 237 of 246 ROMs reach their RSP tasks.
  - The CU1 trap (4c8b6e3) is what moved Perfect Dark, Banjo-Kazooie's
    frame-293 fault and Mario Party 3. CyberTiger is a timing race in the
    model: it runs with PAL timing.
- **Learnt facts** (042ff2c): `games/learned/` holds addresses and sizes
  from the console's rounds for 22 ROMs, read by a ROM-only build of that
  exact ROM (`N64RX_LEARNED=0`: not). Ocarina 1.2's first build: 5,328
  functions in 57 segments against 3,302 in 3. Mario Tennis gains nothing:
  its code is CPU-made.
- **The float mode** (6c96027, on by default for the games it finds):
  Jet Force Gemini's NaN matrices (renderer.md's open question) were our
  FPU code, not the game's. The dry boot made them right at the same
  addresses. Rare's osCreateThread gives threads Status.FR, and its
  hand-written matrix code uses odd FPRs as singles; the generated code's
  moves treated them as FR = 0 halves. Now odd FPRs follow each context's
  FR (`XRC_FR`, from the ROM by `saves.thread_fr`: JFG, the Turok-engine
  five, Conker, Shadow Man, Perfect Dark). xemu: 0 of 2 bad matrices per
  list against 2 of 2 with `N64RX_FR=0`, the matrix word for word the dry
  boot's.
- Verified how: the 24 CI host tests pass after each commit (clang, plus
  GCC for `test_f3dex`). The 14-game xemu regression set ran at the
  baseline, after phase 0 and with the token; all present with no crash.
  `test_analyze`: unchanged, 0 wrong names. The A/B runs above in xemu
  (slot 11; this machine's xemu runs about 2 fps, so frame counts are
  noise).
- Not verified: **nothing ran on a console.** The `[MEM]` and `[CLOCK]`
  lines are xemu's only (JFG's test build: 54,224 KB free at boot, 30,316
  KB after nv2a; `[CLOCK] cpu 733 MHz`), not red's. Set 18 is scripted
  (`build-host/hwbuild18.sh`, `deploy-set18.sh`) and not deployed. Whether
  JFG's letters and 3D now draw is not checked (xemu here reaches only its
  Nintendo logo in 150 s). The other FR games' first builds are blocked
  before the float mode matters: Turok 2 and Shadow Man crash the same with
  and without it, Conker presents nothing either way (A/B in xemu, 120 s).
- **Turok 2's TLB misses** (d56c20b): its image holds three copies of
  libultra's preamble and `__osException`; `saves.os_exception` took the
  last, but the game installs the one at `0x0029BFF0` (the dry boot's
  vectors), whose code pages its world in. Now the copy a segment loads
  where it is linked wins. xemu: the KUSEG-0 write crash of s12 is gone; it
  stops at the first paged-in function (`0x00425604`), the learning loop's
  or the dry boot's to supply.
- Also: a function's size stops at the recompiler image's end (f3df9f8:
  Indiana Jones's dry-boot build was refused whole); `bench_dl` links again
  (f17ad7f).
- Gaps: Mario Tennis's matches in the dry boot; Banjo-Tooie's CIC 6105;
  BattleTanx's NULL node; Traveller's Tales' `0xC0001000` probe; F1 World
  Grand Prix II and NBA Courtside's waits; CyberTiger's race.

## 2026-10-22 (night, continued; no hardware): the review's runtime items, Turok 2's fault path, the dry boot's versions (host + xemu only)

Commits 10786f5..da84afb on `dev`, none pushed; nothing deployed to red.

- **Build failures from the dry-boot sweep (s13db)**: a branch out of its
  function becomes a tail call through the lookup (`fix_stray_goto`:
  Bust-A-Move '99 builds again); ll/lld/sc/scd send their function to the
  interpreter (N64Recomp stopped the whole run on one: Ken Griffey's);
  a function with a `bal`/`bgezal`/`bltzal` into a function's body is
  interpreted (`N64RX_XBAL`). N64Recomp made a static there and could not
  place its branches ("Unhandled branch in static_2_800B6844"): one
  hand-written routine with several entry points, the same in Resident
  Evil 2 and both Ken Griffey games (Angel Studios'), which also reads `ra`
  as a value after a `bltzal $zero` (compiled code never sets `ra`).
- **The dry boot's versions** (71c3a90): a page's version superseded after
  the first graphics task is kept (`--versions boot`, the default). Mario
  Tennis with its scenario script: 2,280 functions in 16 segments (was
  1,372 in 14; the console's 32 rounds: 2,375 in 29), its match reached;
  Banjo-Kazooie 6,806 in 6; GoldenEye unchanged. `--snap` writes the
  task's OSTask too and `tools/xbox/dryboot_frame.py` turns a snapshot into
  a DLDUMP log for `bench_dl.py --png` (scratch only: the picture is the
  game's). Jet Force Gemini's name entry drawn whole that way.
- **Turok 2 presents frames** (e88d6a7). The fault path (7e041d3,
  `__osFaultedThread` with its loads in either order: the six games of
  Acclaim's engine, nothing else changes) and `__osRunQueue` (809cdec,
  either order: 118 ROMs had none) are found in the `__osException` the
  game installs, and demand paging works (the fault thread maps each
  miss). Then every thread waited on a queue that read "0 of 0". A runtime
  write watch (`-DXRC_WATCH=0x8011ADE0`, an N64 address) caught
  `asp_hle_run` zeroing the scheduler's queue: an audio command list's
  addresses are physical, but the runtime took those below 0x80000000
  through the game's TLB, and Turok 2 maps KUSEG 0x00300000 to physical
  0x00100000, so its audio went 2 MB low. Now read as KSEG0
  (`XRC_ASP_PHYS`). xemu, dry boot plus one learning round, 150 s: 305
  frames presented and audio running (before: 0). What it draws is not
  judged (no screenshot kept: game frames).
- **The rest of Acclaim's engine**, the same fix (xemu, slot 11, 120 s,
  dry boot, no learnt facts; in s12 every one presented 0): Turok 3 238
  frames, Shadow Man 242, Turok: Rage Wars 249, South Park 229. A/B: South
  Park with `-DXRC_ASP_PHYS=0` presents 0 and crashes writing 0xEFFCEC7D.
  Armorines restarts silently (no card) right after the interpreter runs
  its hand-written 0x002A42F4, every boot.
- **Turok: Dinosaur Hunter presents** (da84afb, 238 frames): it inflates its
  code over its boot segment, and the learnt functions there were walked
  over the ROM's compressed bytes (499 functions, then a jump to 0). The
  build's copy of the ROM now takes RDRAM's bytes where the learnt starts
  look like starts only there (`N64RX_RDRAM_CODE`): 2,323 functions.
- **Ken Griffey's Slugfest and Major League Baseball Featuring Ken Griffey
  Jr. build and present** (eb7483b: 296 and 292 frames; s12: 0); Resident
  Evil 2's dry-boot build works and presents 2 (as s12 without it), then
  its threads sit at interpreted pcs with no graphics task.
- **The review's runtime items** (`docs/review-2026-10.md`), each with its
  switch and a host test where one can reach it:
  - 5a.10: `XRC_SETJMP`'s call is never a tail call.
  - 5a.15: round.w/round.l round a half to even (`XRC_ROUND_EVEN`).
    FCSR.FS flushes denormal results (`XRC_FTZ`, MXCSR.FZ).
  - 5a.9: a KSEG1 heal refused because another thread healed the page
    retries.
  - 5a.14: capture through the TLB never reads past RDRAM.
  - 5b.18: 64 timers, and osSetTime leaves pending timers alone.
  - 5b.20: the 64DD's windows are open bus, not the save
    (`XRC_SAVE_64DD`).
  - 5b.21: the OSIoMesg header filled as libultra's (`XRC_PI_HDR`).
- **The library's dry-boot scan with each region's TV type**: 239 of 246
  reach their tasks (Banjo-Tooie (E) and F1 World Grand Prix II now run;
  PAL ROMs had NTSC timing).
- **s13db is not a result**: 30 s runs with two xemus at once (this machine
  runs about 2 fps; Banjo, GoldenEye and Blast Corps need 90 s to show
  frames). Stopped at run 73 of 95; only its build failures are used above.
- Verified how: host tests after each commit; the 14-game regression set
  rebuilt at b70dc81 and booted one at a time in xemu (slot 11, 90 s,
  `head2`): all 14 present with no crash (Banjo-Kazooie, Mario Tennis and
  both Kirby 64s stalled at the loading screen while a build ran beside
  them and presented when booted again alone; Majora's Mask's audio stall
  line is in every baseline too). The audio, `bal` and decompressed-code
  fixes came after that build: the audio one changes only games whose TLB
  maps that low (none of the 14's logged entries), the `bal` one only
  builds with an N64Recomp static (none of the 14), the third replaces no
  span in any of the 14 (a dry run of the learn step with their logs).
- Not verified: **nothing ran on a console.** `[MEM]`/`[CLOCK]` are xemu's
  only (Super Mario 64's test build: 53,588 KB free at boot, `[CLOCK] cpu
  733 MHz`). Set 18 is staged, not deployed.
- Gaps: Resident Evil 2 after its first 2 frames; Armorines' silent
  restart; what the Acclaim games draw (not judged: no frames kept); NBA Courtside's scheduler messages stop;
  Conker's KUSEG-0 read; contiguous TLB pages want one section; JFG's 3D
  past its menus (xemu reaches only the Nintendo logo in 150 s).

## 2026-10-22 (afternoon, no hardware): Armorines, Resident Evil 2 and Conker present; the sweep measures what it says (xemu only)

The maintainer was back, asked for a push of the night's work (`dev` at
c4d06ed, 52 commits) and allowed one Opus subagent at a time. Commits
since the push: 6c53ced, 1472782; nothing deployed to red. Two of the
diagnoses below were the subagent's (host only: logs, the dry boot,
disassembly), each checked here in xemu before it went in.

- **Armorines** (c4d06ed, the last commit of the push): it reset the
  console with no card every boot (s12 too). A live trace
  (`-DXRC_TRACE_LOG`, new) ended in its heap memset: its boot bumps the
  word at 0x80400000 and reads it back to find the Pak, which on a 4 MB
  build is the runtime's own memory past RDRAM, so it "found" 8 MB and
  cleared to 0x80780000 over the host. Such a probe now gets 8 MB
  (`N64RX_PROBE_8MB`). xemu: 232 frames, one boot.
- **Resident Evil 2** (6c53ced): its DAC thread's 192-sample buffers
  (shorter than a retrace) left the AI model's backlog at or below 0, and
  the event never came again; its synthesizer, its audio tasks and then
  its scheduler's own RSP task waited on that. A buffer queued to a spent
  backlog posts at once (`XRC_AI_SPENT`, `test_ai`), and a graphics task
  with no display list (its own microcode) is not run, SP done only
  (`XRC_GFX_NO_LIST`; DIVERGES: what it draws is missing). xemu: 204
  frames (before: 2). Super Mario 64, Pilotwings 64 and Smash present
  with both, no crash.
- **Conker's Bad Fur Day** (1472782): its main maps the boot code at
  0x10000000 with its own tlbwi and jumps there; analysed at KSEG0 every
  call went through the safety net and its own osStartThread's eret ended
  the boot. Analysed where main maps it when the functions there cover
  most of the image (`N64RX_TLB_MAIN`; Perfect Dark has the same main but
  stays at KSEG0: an A/B showed its mapped span is inflated code). xemu,
  ROM alone with the learnt facts: threads start, 185 frames.
- **The sweep** (8a8d08e): `--run N` was cut at the 30 s heartbeat
  whatever N said, and a boot xemu stalled at the loading screen counted
  as "no frames". Both fixed (`SWEEP_RETRIES`). s14db (the dry boot, no
  learnt facts, 90 s, the 101 games s12 left under 30 frames) is running
  on slot 12; it was built before the fixes above, and my one-off boots
  on slot 11 ran beside it, so its "no frames" lines need a second look.
- **The recompile cache** (34be68e): `fix_stray_goto` changed gen/ without
  a GEN_VERSION bump (Bust-A-Move '99 and Forsaken failed again from kept
  code), and the key ignored the recompiler's own ROM copy; both fixed.
- Set 18 is staged (28 builds, `C:\xemu\hw\stage-*`), not deployed; it
  predates these fixes. `[MEM]`/`[CLOCK]` are xemu's only.
- Gaps: Body Harvest DMAs 0x1108 bytes of MIO0 data to RDRAM 0x80000000
  (a destination pointer of 0) and then runs it; NHL 99 and Bomberman 64:
  The Second Attack store through a NULL pointer (0x28, 0x38); RE2's own
  microcode is not drawn; NBA Courtside; JFG's 3D.

## 2026-10-22 (evening): set 19 on red, its verdicts, and the fixes after it (red + xemu)

Set 19 (29 release builds at fe59555, `build-host/hwbuild19.sh`) went to
red, size-verified (`deploy-set19.sh`; sets 17 and 18 never had). The
maintainer played seven before running out of time; logs pulled with
`build-host/pull-set19.py` to `C:\xemu\hw\logs<name>`.

- **Diddy Kong Racing** (`vdkr3`): works perfectly. **Gold.**
- **Castlevania: Legacy of Darkness** (`vcv5`): **Silver**, texture issues.
- **GoldenEye** (`vge13`): stays Silver; texture flicker on the road markers.
- **Banjo-Kazooie** (`vbk20`) and **Kirby 64 US** (`vk7`): texture issues,
  the same in both (Kirby's at the menu). Not seen in xemu. Ruled out from
  the logs: the texture pool (0 allocations failed) and GPU ordering
  (freed textures wait for idle; textures are never rewritten in place).
  What the issues look like is still to ask.
- **Mario Tennis** (`vmt7`): plays, then froze with the BACK menu on its
  Paks page: retraces stopped at 1480, the watchdog's hang card. The
  thread dump shows a deadlock (game scheduler with interrupts off, so
  holding the runtime lock, waiting for the renderer the menu holds; the
  VI thread, which feeds the menu, waiting for the runtime lock). Fixed in
  d41d8cb for every game (`XRC_GFX_LOCK_DROP`, `XRC_MENU_VI_FIRST`);
  xemu never showed the freeze either way. `vmt8` (the fix) is on red.
- **Pokémon Snap** (`vsnap13`): the camera still does not move, but the
  run had 8-10 s on the Beach; the tutorial's first focus came at 7-39 s
  in xemu (and not at all in two runs). `vsnap14` on red logs the focus
  flag and the photo count (`[PEEK]` 0x803AE758 / 0x800AE27C): stay on
  the Beach without touching the stick until "Press Z to aim", then LT
  and A three times.
- Not run: the Gold three (SM64, Pilotwings, Smash), Mario Kart, Ocarina,
  Mario Party, Majora's Mask, JFG, Star Fox and the 13 first-time games.

After it (xemu; host tests after each):
- **Body Harvest** (4a6db73): three code blocks at one address, one kept;
  now all, as overlays of one another. 247 frames (before: a DMA to
  address 0 and a fatal). Smash's play scenario unchanged with it.
- Earlier this evening (handoff above): NHL 99 (fe59555, e7fd013),
  Bomberman 64 TSA (bdc626a), Conker (1472782), RE2 (6c53ced).

`[MEM]`/`[CLOCK]` from red's logs: Banjo-Kazooie 50,112 KB free at boot,
11,772 KB after nv2a (`vbk20`); Kirby 64 50,964 / 27,804 KB (`vk7`); the
clock line `[CLOCK] cpu 733 MHz`.

## 2026-10-22 (night): three more red rounds, Banjo's START screen fixed (red + host)

Rounds on red after set 19, each a few builds (`build-host/hwbuild20.sh`
to `hwbuild24.sh`; logs in `C:\xemu\hw\logs<name>`). Then the maintainer
paused hardware testing until approved; `vsnap16` (`hwbuild25.sh`) is
staged, not deployed.

- **Mario Tennis** (`vmt8`): the BACK menu's Paks page no longer freezes
  on red (d41d8cb). New: a line at the bottom of the screen now and then,
  the right side cut off, the ball's textures. Its log shows a 384x15
  RGBA16 tile (11,520 bytes) missed as "too big": more than TMEM holds,
  so the game relies on TMEM's wrap; not changed blind.
- **Banjo-Kazooie's START screen: fixed on red** (`vbk22`, 527e2c1). The
  A/B (`vbk21a` magenta sentinel, `vbk21b` CPU readback, `vbk21c` tiles
  off; 9b9e3c1, own title IDs via `N64RX_TITLE_ID`, 6c42197) showed the
  readback stage holding the right picture. The decomp (code_8DC20.c)
  then showed the pause copying the frame into the depth buffer's memory
  with texrects and drawing it back: our texrects ignored the colour
  image. `XGFX_ZIMG_TEXRECT` writes such copies into RDRAM.
- **Banjo-Kazooie's orange shadows: not fixed.** The fog-blender rule
  (df4a16e) changed no Banjo draw on red (`vbk23` logged none), so it is
  opt-in now (a99447b). Also open: the intro's dark fog and its jigsaw
  pieces' textures.
- **Pokémon Snap** (`vsnap14`, `vsnap15a/b`): the camera stays locked;
  15b (vertex output cache and word reads off) behaves the same. The
  probe follows the detector's copies in the course's first seconds
  (`XGFX_PROBE_LOG`, 0fe5023: "6 / 12 at half or more"), yet the focus
  flag never set; those runs were 5-10 s of course. `vsnap16` peeks the
  detector's state over a whole course (protocol in `hwbuild25.sh`).
- The Gold three on set 19 (`vsm18`, `vpw2`, `vssb15`): ok.
- The s15 library sweep (the dry boot, the shipped learnt facts, 90 s
  runs, one xemu) is running; built at 5948f77 plus default-off
  diagnostics.
