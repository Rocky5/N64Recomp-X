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
python tools/xbox/test_interp.py        # the MIPS interpreter: each instruction class vs expected results, 64- and 32-bit registers, the learn log's spans
python tools/xbox/test_asp_hle.py       # the audio HLE: every command of every ABI vs a reference, SIMD = C, native = word-swapped RDRAM (hash: aspMain, hash2: nead + n_aspMain)
python tools/xbox/test_xbe_rom.py       # the ROM inside the XBE: xbe_sections' $ROM, found and checked by xhw_rom.c's parser
python tools/xbox/test_romcache.py      # the ROM page cache's core: chunk spans, word reads, the LRU and its cap, pins, failed fills
python tools/xbox/test_saves.py         # saves: pak CRCs and PIF round trips, the blank format, Rumble, osPfs*, Flash, SRAM by PI DMA
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
`SHOT` for an `[FBDUMP]`. M0's scenarios:

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
Controller Pak screen, its pages free).

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
| `-DXRC_ROM_RESIDENT_MB=<n>` | 16 | ROMs up to n MB are read whole into RAM; bigger ones go through the page cache (`xhw_romcache.c`). `0`: the page cache for every ROM (a test of it on Pilotwings 64); `64`: every ROM resident (no cache; a 32 MB+ ROM then does not fit) |
| `-DXRC_ROM_CACHE_MB=<n>` | 16 | the page cache's cap (chunks held, pins included) |
| `-DXRC_ROM_CACHE_720P_MB=<n>` | 12 | the same when the boot predicts 720p |
| `-DXHW_ROM_IN_XBE=0` | 1 | ignore `default.xbe`'s `$ROM` section: the ROM is looked for next to the XBE (the build-side switch is `n64rx build --rom-beside`, which leaves the section out and writes the file) |
| `-DXRC_SAFETY_NET=0` | 1 | a call to an address with no function is the fatal card again (no interpreting, no `[LEARN]`, no `learn.txt`) |
| `N64RX_WITHHOLD=<vram>,...` (env, n64rx) | unset | test: those functions left out of the analysis, so the safety net runs them (the learning loop's demo) |
| `-DXRC_SAVE_PAK=0` | 1 | no pak in any port: the PIF's status says none, pak commands get no answer, osPfs* return `PFS_ERR_NOPACK` (the old behaviour) |
| `-DXRC_SAVE_CART=0` | 1 | no SRAM or FlashRAM: domain 2 reads zeros and drops writes, osFlash* fail |
| `-DXRC_CONT_MAX_ADDR=<a>` | per build | libultra's `__osMaxControllers`, which the runtime's osContInit sets to 4 (n64rx finds it: `saves.py`); unset, a game's own pak code sees no pak |
| `-DASP_HLE_WORDSWAP=0` | 1 on the console | `asp_hle` on a native-order RDRAM (Wings64-X's layout; the host tests' first group) |
| `-DASP_HLE_NEAD=0` | 1 | the nead audio microcodes (Mario Kart 64, SM64 EU, Zelda) read as unknown: silent |
| `-DASP_HLE_NAUDIO=0` | 1 | `n_aspMain` (Paper Mario, Smash Bros.) reads as unknown: silent |
| `-DXGFX_HOLD_EVICT=0` | 1 | the BACK menu never drops the game's texture cache when its held frame or card finds the texture pool full (Mario Kart 64 on red: the card was missing) |
| `-DXGFX_UCODE_F3DEX=0` | 1 | F3DEX 0.9x/1.x microcodes (and F3DLX, F3DLP, L3DEX, .NoN/.Rej) read as Fast3D, the build before the registry (`docs/renderer.md`) |
| `-DXGFX_UCODE_F3DEX2=0` | 1 | F3DEX2 microcodes (F3DZEX, F3DLX2, .NoN/.Rej) read as Fast3D |
| `-DXGFX_UCODE_S2DEX=0` | 1 | S2DEX 1.x/2.x read as Fast3D (its own commands are skipped either way; not drawn yet) |

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
