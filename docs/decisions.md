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

## M1 (2026-10-16)

**Pilotwings 64's symbol file comes from the decomp's splat config, not its
ELF** (PLAN.md 4.2 said `--dump-context` on the decomp's IDO build). That
build needs IDO and MIPS binutils under WSL or Docker, and neither runs on
this machine (Hyper-V is off). `tools/xbox/syms_tools.py from-splat` runs the
decomp's own splat config (splat 0.39.1, its pin) once with every C file as
asm: spimdisasm's function bounds, the decomp's names where its symbol files
have them. The same facts, on any OS, with no compiler. Committed: the decomp
is MIT.

**`func_80231A10` (a handwritten mfc0/mtc0 Count helper) is `ignored`.** The
recompiler stops on COP0 Count; nothing in the game calls it.

**`ctx->f_odd` points at `f0.u32h` in every context** (`xrc_ctx_init`). The
generated code reaches an odd FPR as the high half of the even one below it
(FR = 0); left NULL, the first float op wrote through address 0x90.

**PI device addresses are cartridge offsets.** libultra ORs `osRomBase` in
itself, so `osPiStartDma`'s and `osPiReadIo`'s addresses are ROM offsets (the
app segment's 0x51E30); a full bus or KSEG1 address is still accepted.

**The boot code parks at its first `osStartThread`** (called from a thread
that is not an N64 thread): the N64 never returned to it either.

**Synchronous tasks post their done events at the front of the queue**
(`DIVERGES` in `ultra/sp.c`). With the task on the scheduler's own thread,
the retraces that queued while it ran sat ahead of its done events; the
scheduler took its task for still running, asked it to yield and recovered
from a task "that did not yield" (Pilotwings 64's `_uvScHandleRetrace`).

**`_uvScDlistRecover`'s SP_STATUS store is a nop** (`games/pw64/recomp.toml`,
an instruction patch; Wings64-X's patch 018 at the same site). The recovery's
messages are kept. PLAN.md 3.2.1: a KSEG1 CPU store is patched at its site.

**The interpreter reads by N64 address.** Display lists word by word from the
host image (words are native); vertices, matrices, viewports and lights
through `rd16`/`rd8`; texels copied out in N64 byte order, the order
Wings64-X's `texconv.c` already took (its textures stayed big-endian).

**CI is manual-only** (the maintainer's free Actions minutes): `gh workflow
run build.yml --ref dev`, `-f image=true` to rebuild the SDK image.

## M2 (2026-10-16)

**`asp_hle` runs on the word-swapped image** (`ASP_HLE_WORDSWAP`, the
console's default). Transfers between RDRAM and DMEM swap the halfwords of
each word, so DMEM's s16 lanes stay native and every MMX path is unchanged;
only the ADPCM decoder reads bytes, through `o ^ 1`. Everything the
microcode reads is the game's own N64 data, so no loader is patched (the
siblings converted the bank loader). `test_asp_hle` runs both layouts and
requires the same RAM results.

**The resampler table is found in the task's microcode data**, not read at a
pack-given ROM offset: the first audio task's data segment is searched for
512 bytes that pass `asp_hle`'s checks (PW64: data+0xC0). Generic, and
nothing from the ROM is stored.

**`osAiSetFrequency` returns the DAC's real rate** (video clock / divider:
22050 asked, 22047 given), as libultra does; Wings64-X returned the asked
rate. The synthesizer runs at what it is told.

**The BACK menu opens on a BACK tap** (BACK let go with nothing else pressed
meanwhile), not on the press. BACK + WHITE (screenshots, moved from BACK
alone) and the exit combos all start with BACK; opening on the press would
put the menu up on the way to every one of them.

**The BACK menu is a card in the loading screen's dress, composited on the
GPU** (the maintainer: "should look super cool and match the vibe"). The
platform renders it into a 512x416 ARGB image (`xhw_panel_render` in
`xhw_splash_draw.c`: the same aurora, sparks, light sweep and green bar as
the loading screen, a rounded body with a teal edge and glow in alpha); the
interpreter shows it over the held game frame, dimmed to 45 %, scaled with
the content box (1.5x at 720p, 3/4 as wide on an anamorphic 480). It fades
and slides in over 200 ms, out over 100 ms. The plan's "CPU text over the
frame" (xhw_overlay.c) could not blend: the framebuffer is write-combined.
Host preview: `docs/look/menu-card-host.png` (`test_splash` writes it over a
stand-in game frame).

**The menu has its own thread; the VI thread only reads the controllers.**
Drawn on the VI thread, a slow frame (xemu: 20-40 ms) swallowed short
presses. The menu thread (level 13) holds the renderer while the menu is up,
so a graphics task waits for it; the VI thread polls the pads every retrace
and changes the settings; the held-direction repeat is timed by the clock.

**Autopad times `t<ms>` events by the ACPI clock**, not `GetTickCount`: the
kernel tick fell behind under load in xemu and a 100 ms press lasted 300.
(Other `GetTickCount` uses in the platform are timeouts and the loading
screen's animation; they stay for now.)

**Widescreen offers Auto, On and Off.** Stretch needs interpreter work and
comes with M6.

**Generic hor+** (`XGFX_WORLD_GENERIC`, PLAN.md 5.4): until a pack emits a
world tag, every perspective draw under the full-screen scissor is the world
and is drawn across the whole width; rectangles and orthographic draws stay
in the 4:3 box. Wings64-X widened only inside its decomp patches' tags; here
no pack has patches on day one. The game's own culling still works to its
4:3 frustum (pop-in at the edges until a pack's culling patch).

**The video-mode probation's runtime half is `rt/video.c`** (M1 had none: A
could not keep a mode). A on any port, at the game's reads or from the VI
thread every 8 retraces, keeps the mode and saves `video_confirmed`; that A
does not reach the game until let go. The timeout restarts into 480i from
the VI thread.

## The ROM-only path (2026-10-16, PLAN.md 3.14)

**The end goal is a working XBE from the ROM alone; packs are optional.**
The maintainer: "someone downloads this toolkit, provides the ROM, and it
spits out an XBE". The decomp-based symbol file of M1 became the answer key
the analyser is checked against.

**libultra is recognised by behaviour, never by fingerprints.** A list of
instruction hashes taken from ROMs would be ROM-derived data in the tree;
rules on behaviour (COP0, hardware registers, struct offsets, the call
graph) hold no ROM material and carry across libultra versions. Pilotwings
64: all 54 replaced functions named, none wrong; Mario Kart 64 (another
libultra version): 71 named on the first run.

**Segments are found from constant DMA arguments, proved by the calls into
them.** No libultra name is needed: any call whose arguments evaluate to a
ROM offset, a RAM address and a size is a candidate; it becomes a segment
only when the known code calls into it and the bytes there look like
function starts. Pilotwings' app segment came out exact; Mario Kart's two
race segments too.

**Dead code is not recompiled.** Liveness from the entry, main and every
function address in data or code, with the edges out of replaced functions
cut: libultra's internals (exception handler, the VI and PI managers'
threads) only the replaced functions reach drop out, and with them the
calls to internals the runtime has no reason to provide.

**Hand-written code runs in an interpreter, not a hand port.** Mario Kart
64's decoders return through `s3` after `bal` into another function's
body; no recompiler expresses that. A decomp-based recomp ports such code
by hand; a ROM-only tool cannot, so the runtime interprets it (and any live
function N64Recomp refuses) from RDRAM on the same context.

**The PIF is emulated for the game's own SI code.** A ROM-only build runs
libultra code the rules did not name (the Controller Pak driver, a game's
own pad reader); they build PIF command blocks themselves, so
`__osSiRawStartDma` answers them as the PIF would.

**Without a pack, the title ID is `0x4E52` + a 16-bit hash of the game
code** (0x0100 and up; below is kept for packs), the title is the header's,
the save type comes from a facts table of 16 Kbit games (else 4 Kbit when
the game probes an EEPROM).

**The decomps are answer keys, read and never built or shipped** (the V
track). The maintainer: "between all the decomps that are there, we should
be able to get it working pretty stable". Their lists stay in `build-host/`;
the rules keep only what generalises.

**The ROM goes inside the XBE** (maintainer, 2026-10-16: "that way there is a
singular package for the user to manage"; decided, not built yet, PLAN.md
§3.5). A `$ROM` section the kernel never loads, read by the runtime from
`D:\default.xbe` at its raw offset: no RAM until read, the resident load and
the page cache unchanged, the SHA-1 check on the embedded bytes. Nothing
changes about publishing: a built XBE was never shareable (it holds the
game's recompiled code). Until then the ROM sits next to `default.xbe`.
