# Decisions

What was decided while building, with the reason, newest last. `docs/PLAN.md`
is the plan; where a decision here departs from it, the entry says so and the
code carries `DIVERGES:`.

## M0 (2026-10-16)

**The platform layer comes from Wings64-X `2871557`, not `83f1258`.** The
plan named `83f1258`; the checkout had four more commits, and the only one
touching `xbox/src/hw` is a 20-line `nv2a.c` fix for the 720p 4:3 box and
the pillarbox clears (hardware-verified there). Taking the newer tree costs
nothing and keeps the fix.

**The ROM-free test ROM is assembled by `tools/xbox/make_test_rom.py`, not
built with libdragon** (PLAN.md 4.6 named a libdragon ROM). A ~150-line MIPS
assembler in the script builds a self-checking program, so CI needs no MIPS
toolchain on three OSes, and the image is the same byte for byte everywhere:
its SHA-1 (`a182648693f416078e0bff023243d552ad46fe16`) can sit in the test
pack like any game's. It covers what the plan wanted from libdragon (the
recompiler, our header, the window, the build) and more (jump tables, the
rounding modes), and like libdragon it never touches libultra.

**The test pack's XBE runs its code**, not just links (the plan's M0 asked for
a linking XBE). The runtime's test boot sets up the RDRAM window, builds the
function table and calls the recompiled entrypoint; the card shows the
program's own checks. This puts `recomp.h`'s memory model, rounding and
`LOOKUP_FUNC` on the target in M0, at both register widths.

**The loading screen's thread runs one level above the boot thread (9), not
at 3** (PLAN.md 5.5; `DIVERGES` in `xhw_splash.c`). At level 3 it starved
whenever the boot thread worked: a frame took "52 ms" in xemu (the thread was
preempted mid-frame), and BACK or an exit combo pressed during the
settings.ini write was never seen. At level 9 a frame costs 4 ms in xemu and
the boot gets the rest. To re-measure on the console: `[BOOT] loading screen:
N ms a frame` in test builds.

**The controllers come up behind the loading screen, and the BACK window
counts from there.** SDL and USB take ~1.45 s to answer (xemu). Opening them
before the screen (Melee-X's order) would leave the TV black that long;
opening them after the screen is up keeps it animated, and
`xhw_splash_finish` waits for both the minimum hold (2 s from the first
frame) and 1 s after the controllers answer, so a BACK held from power-on
latches (it needs 500 ms held). The splash thread only try-locks the pads,
so it never waits for the bring-up.

**The holds latch.** BACK held 500 ms (neither trigger past 100: L + R + BACK
starts the exit combos) latches safe video for this boot; BACK + START held
500 ms latches safe video and the defaults. Any other button skips the
minimum hold, never the BACK window. The plan said "while BACK is held";
latching means a press that ends before the screen does still counts, which
is what someone hunting for a picture expects.

**The exit combos live in the platform (`xhw_combo.c`), with Wings64-X's
release wait.** PLAN.md 3.9 says the action runs 200 ms after detection
"once the controllers are free". Wings64-X's console runs 1-6 showed that
leaving with L, R, BACK and BLACK still down hangs the console on the way to
the dashboard, so the action waits for the buttons to be let go (at most
3 s), then 200 ms. The combo's exit reason is final (`xhw_exit_reason_final`):
a card that comes up meanwhile can't rename it (xemu showed "card: no game
built in" as the previous exit before this). A thread that finds another one
already carrying the combo out parks for good: the error card's loop used
to spin and starve the loading screen's thread, which was carrying it out.

**Error cards before the mode switch are drawn in the loading screen's dress**
(§5.5 asks for it; Wings64-X used pbkit's debug screen). After the switch
the debug screen remains, as before.

**`xgfx_shutdown` returns when pbkit never started.** The "No game built in"
card quits to the dashboard before any `pb_init`; the shutdown dereferenced
pbkit state and crashed (xemu, `_pb_busy+0xb`).

**Title IDs:** `4E520000` for the platform alone, `4E52FFFE` for the test
pack; games from `4E520001` (PLAN.md §8 Q5's default). The ID reaches both
triples through `xrc_build.h` (CMake writes it) and the certificate through
`xbe_sections.py`.

**The game side never gets `-ftrivial-auto-var-init` or `-funsigned-char`.**
Wings64-X used both for the decomp's C; CLAUDE.md forbids the first on game
code, and the generated code spells its own signedness.

**The generated `lookup.cpp` is not compiled.** It only holds the entrypoint
and the ROM name, which the pack already gives; `xrc_game.c` (written by
n64rx) carries the facts the runtime needs and includes
`recomp_overlays.inl` for the section table.

**Our `librecomp/sections.h` defines `nullptr`** for C: the generated
`recomp_overlays.inl` writes `.relocs = nullptr` for sections without
relocations.

**n64rx's Docker engine runs the whole pipeline inside the image**, with the
checkout at `/src`, the ROM's folder read-only at `/rom` and the output at
`/out`, as the user's own uid on Linux and macOS. The image builds N64Recomp
from the checkout's submodule at its pin and is tagged by both pins
(`tools/xbox/docker/IMAGE`).
