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

**Built: `$ROM` has no virtual range** (2026-10-16). The first version
gave the section a virtual address past the image and grew SizeOfImage
over it, as the format suggests; xemu's `[MEM] boot` showed 4 KB less free
for an 8 MB ROM (the kernel's page tables for the image range; up to 64 KB
for a 64 MB one). With VirtualSize 0 and SizeOfImage untouched the boot is
the same to the KB as with the ROM beside the XBE. Nothing maps the
section, so the range was never used. The runtime reads the section table
from the header the kernel mapped (no descriptor file to keep in step), and
a ROM given to a test build by name (`MX_ROM`) still wins over `$ROM`. A
bad hash on an embedded ROM says to copy `default.xbe` again (the build
checked the ROM on the PC). Kill switches: `n64rx build --rom-beside`
(no section, the file as before) and `-DXHW_ROM_IN_XBE=0`.

## The safety net and the learning loop (2026-10-16, PLAN.md 3.14 step 5)

**A miss interprets instead of stopping, through a stub per address.**
`get_function` returns a bare `recomp_func_t*`; a global "pending address"
would race between the game's preemptive threads, thread-local storage is
not dependable on the game triple, and the generated code does not set ra.
A 22-byte x86 stub per address (`push vram; push ctx; push rdram; call`)
is thread-safe, costs nothing after the first lookup, and keeps
interpreting whatever RDRAM holds, so an overlay that replaces the code
needs no invalidation. No decoded-block cache: correctness first.

**The interpreter follows the generated code, not the VR4300, where they
differ** (odd FPRs in .s ops, round.w as lround, comparisons that ignore
NaN): a function must behave the same interpreted on one build and
recompiled on the next, after `--learn`.

**Learnt facts become analysis inputs, not a pack.** A learnt function is
a root of the segment holding it; one outside every segment brings its DMA
in as a segment; both stay in the build folder. The runtime merges a DMA
that continues the last one, so a game that loads a segment in 4 KB chunks
(SM64) yields the whole segment.

**The ROM page cache: a reservation, committed chunks, one lock**
(2026-10-16, PLAN.md §3.5, M5). A ROM over `XRC_ROM_RESIDENT_MB` (16) is
streamed once for its SHA-1 through a 256 KB buffer and nothing is kept;
the cache then reserves the ROM's size (address space only, MEM_RESERVE)
so image byte `i` always lives at `base + i`, and commits a 64 KB chunk
(MEM_COMMIT) right before the ReadFile that fills it, on the reading
thread: never a fault inside the kernel's file system. Eviction is
MEM_DECOMMIT of the LRU tail when the cap (16 MB, 12 at 720p, pins
included) is reached. Melee-X's `xhw_reserve_lazy` (commit on fault) is
not in this tree and is not needed: every reader goes through
`xhw_rom_read`, which touches chunks before handing them over, so nothing
ever faults on the reservation. Readers get the bytes through a callback
(`xrom_sink`) under the cache's lock, a chunk at a time, so a chunk can't
be evicted by another N64 thread under the copy; `xu_rom_ptr` is now
resident-only (NULL with the cache) for that reason. Pins (`[rom] pin`)
may take at most half the cap, so a pack can't starve the LRU. A failed
fill hands over zeros, logs and retries next time (the DMA's own "outside
the ROM" zeros are the same policy). Kill switches: `-DXRC_ROM_RESIDENT_MB`
(0: the cache for every ROM, 64: none), the pack's `[rom] resident =
always | never`.

**Audio microcodes are told apart by their jump table's shape, not a hash.**
Every libultra-lineage audio microcode keeps its command table in its data
segment right before the ADPCM nibble masks; where it sits, how long it is
and which opcodes point at the no-op handler name the ABI and its variant
(asp_abis.inc). A text hash would need a list of every microcode build
(a fingerprint per ROM family) and would miss revisions; the table rule
reads behaviour, holds no bytes, and an unknown shape plays silence with a
log line instead of noise. The nead variants follow from which opcodes
exist (no ADDMIXER = the first nead's envelope; a 32-entry table with
opcode 14 = the old INTERLEAVE and POLEF; 24 entries = FILTER, DUPLICATE,
HILOGAIN), checked against the maintainer's ROMs and the known variants.

## Saves (2026-10-16, PLAN.md 3.6)

**Which pak a port holds: AUTO by default** (`savedev.c`). A port answers as
both: a Controller Pak below 0x8000 (its file system), a Rumble Pak from
0x8000 up (the probe reads 0x80 back, the motor at 0xC000), so a game that
probes the motor first still finds its saves (All-Star Baseball 2000 said
"no Controller Pak found" under the rule before, 2026-10-19,
`-DXRC_PAK_BOTH=0` restores it: a Controller Pak until the game writes the
Rumble Pak probe before it ever read the file system, then a Rumble Pak
until a restart). Rumble through the replaced osMotor* works whatever the
port holds. The BACK menu's Paks
page (below) sets it per port: `[portN] pak = auto|controller|rumble|none`.

**One `.mpk` per port** (`<id>.mpk` for port 1, `<id>-2.mpk`..`<id>-4.mpk`),
32 KB in the pak's order, so port 1's file trades with emulators' single-pak
files. A pak with no file starts formatted and empty (libultra's layout,
our own ID: device bit set, one bank) and gets a file on the game's first
write. **SRAM and FlashRAM share one 128 KB buffer** (a cartridge has one)
and are told apart on the first domain-2 access: a command to the Flash's
register (0x08010000) means Flash, else an existing `.fla`, else SRAM. No
pack field and no analyser rule are needed for it. The Flash says it is a
Macronix part libultra treats as NEW_FLASH (page addresses in bytes), and
programming only clears bits, as the chip does. Memory: 258 KB static.

**libultra's controller count** (`tools/n64rx/n64rx_lib/saves.py`). The real
osContInit sets `__osMaxControllers` (4); ours is the runtime's. A game
whose pak code was recompiled (its osPfs* not named: Mario Kart 64 from its
ROM alone) loops over that byte and saw no pak. n64rx finds the store of the
constant 4 in the game's own osContInit (the pack's symbols, the ROM's
code) and passes its address as `-DXRC_CONT_MAX_ADDR`; it is derived per
build, never kept. The save writer holds the lock while it writes a file
(no 128 KB copy; a game that saves again meanwhile waits for the disk).

## The BACK menu's Paks page (2026-10-16, PLAN.md 5.2)

**Four paks per game, and the files they already had.** The maintainer
asked for the pak to be managed from the BACK menu after testing Mario Kart
64. Paks A-D are `<id>.mpk`, `<id>-2.mpk`, `-3`, `-4`: the per-port files
the saves already used, so a save made before the page is Pak A in port 1,
untouched. **Each pak is always in exactly one port** (a real pak is in one
controller): choosing a pak another port holds swaps the two, so the four
port images stay four distinct files with no shared buffer, and nothing has
to decide what a pak in two ports means. A set in settings.ini that has a
pak twice reads as A-D in order. Copying a note to another of the game's
paks is built (it lands in the target's lowest free pages, as
osPfsAllocateFile would put it); a pak shared by every game (moving a real
pak between games) is not.

**A change is a hot-swap, not a reboot.** The game is frozen while the menu
is open, so the swap lands between two of its pak accesses, as a hand would.
Two signals, both the hardware's: the controller's status byte has
CONT_CARD_PULL (0x02) once after the swap (libultra's `__osPfsGetStatus`
says `PFS_ERR_NEW_PACK`), and the pak's ID area is another pak's (a format
from the menu writes a new random ID), which the runtime's osPfs* check
against the OSPfs's copy as libultra's `__osCheckId` does. A delete also
raises the pull bit: the pak was "out" while it was edited, and a game that
re-reads it sees the new directory. Neither signal reaches a game that does
not ask; nothing is forced on it.

**The VI thread asks, the menu's thread acts.** The pad handling runs on the
VI thread, which must never wait on the disk; deletes, formats and swaps
take the save lock (the writer may be writing a file), so they run on the
menu's own thread between frames, which also re-reads the note table every
frame (16 entries: microseconds). Deletes and formats ask first (A again,
anything else keeps), on the card's warm note line.

## The renderer's microcode families (2026-10-16, PLAN.md 3.7, `docs/renderer.md`)

**The microcode is named by its version text, not by a hash.** PLAN.md
3.7 had "the task's ucode hash -> the family's opcode table". A table of
hashes would be data taken from ROMs (and one entry per microcode build);
the text every graphics microcode carries in its data segment ("RSP Gfx
ucode F3DEX fifo 1.23 ...") is the microcode's own behaviour, names the
family and its variant in one rule, and needs nothing per ROM. No text:
Fast3D, the build before the registry (Pilotwings 64 unchanged).

**One interpreter, a translation layer, not one per family.** F3DEX and
F3DEX2 share the RDP, the texture loads, the matrix stack and the vertex
pipeline with Fast3D; only numbering and a few layouts differ. `ex_cmd`
rewrites a command into the Fast3D one it equals, or does the few that
have no equal, so the shadow volumes, the widescreen rules, the held frame
and every fix made for Pilotwings 64 apply to every family at once. The
state stays in Fast3D's terms.

**CI textures came with the F3DEX work.** Mario Kart 64's textures are
mostly CI4/CI8 (Pilotwings 64 used none): G_LOADTLUT and a TLUT path in
texconv.c, the palette in the texture cache key.

**A matching decomp with no addresses is read by its link order** (the V
track's `decomp_check.py`). Super Mario 64 and Mario Kart 64 need IDO and
a MIPS toolchain to build; their `.ld` lists the objects in order and each
file's functions are in source order (the host clang's preprocessor with
the version's defines, generated headers stubbed). Lined up against the
analysis between the addresses the key does know (`func_XXXXXXXX`) and the
analysis's own libultra names (the longest run in the same order on both
sides), the functions map one to one where the counts agree; a name only
counts as right when its neighbours line up too. No build, any OS.

**Whether code is unoptimised is read from the code** (`funcs.py`
`_referenced`): IDO at -O0 closes ifs and returns with `b .+8; nop`, which
the optimiser never leaves (Super Mario 64 ~0.8 per `jr ra`, the -O2 games
under 0.02). Only there is a bare `jr ra; nop` after another one folded into
the function before; elsewhere it is an empty function.

## The TLB (2026-10-16)

**Mapped pages are aliased in the Xbox page tables; the hot path does not
change.** The generated code computes `rdram + vaddr` with `rdram` = P,
the window's physical base, so a TLB-mapped vaddr V (KUSEG) is host
`P + V`, in user space (SM64's `0x04000650` was `07a31650`). When the game
maps V onto N64 physical X, `xhw_alias.c` reserves the 4 MB regions around
that host range from the kernel (so it never commits there or shares a
page table with us), then maps each host page onto Xbox physical `P + X`,
the frame the window already shows at `rdram + 0x80000000 + X`, by writing
the x86 page tables (ring 0, one address space, self-mapped at
`0xC0000000`/`0xC0300000`). A region already in use refuses the mapping.
Weighed against: (b) relocating TLB-linked segments at build
time, which needs relocations a ROM does not carry (SM64's goddard dynlists
hold absolute `0x04xxxxxx` pointers); (c) translating in `MEM_*`, a compare
on every load and store of every game for the few that map; copying the
mapped bytes to a private buffer, which the RSP (reading the physical
pages) would not see. Limits: a vaddr whose host range the XBE or the heap
already uses, or KSEG2/3 (host addresses in the kernel's system space),
cannot be aliased (logged as `[TLB] ... not aliased`); the ASID is not compared; code linked at TLB addresses is
not in the function table. SM64 needs none of these: goddard's code is
KSEG0, only its dynlist data is read at `0x04000000` (64 KB pages onto a
heap buffer). Kill switch `-DXRC_TLB=0`.

**The TLB routines run where they are found.** Named by the analyser
(`_tlb` in `libultra.py`: they write Index, EntryHi, EntryLo0/1 and run
`tlbwi`; arguments and PageMask tell osMapTLB, osUnmapTLB, osUnmapTLBAll
and osMapTLBRdb apart), the runtime's `ultra/more2.c` replaces them;
unnamed, N64Recomp refuses them (mtc0 EntryHi) and the interpreter's
`tlbwi`/`tlbp`/`tlbr` reach the same `tlb.c`.

**`__osEnqueueAndYield` is a 1 ms pause with the interrupt lock let go.**
Libultra code the analyser did not name (Dr. Mario 64's osYieldThread
before the `_tlb` rule named it) calls it; the runtime's threads are the
kernel's, so there is no run queue to join. Every libultra caller rechecks
its condition in a loop, so returning early is a spurious wake-up; the
millisecond keeps a busy caller from starving lower kernel priorities.

## The sweep and what it fixed (2026-10-16, `tools/xbox/sweep.py`, agent `breadth`)

**Calls nothing provides link as weak bodies** (`pipeline.py`
`write_missing_c`, `xbox/src/rt/ultra/more3.c`). N64Recomp renames every
function of its built-in *ignored* list (libultra internals: `__osSpSetStatus`,
`__osEnqueueAndYield`, the TLB and CP0 helpers, IDO's 64-bit conversions)
to `<name>_recomp` and emits no body; the analyser stops its reachability
there too. A game whose own code calls one failed to link: 78 of 246 games
in the first sweep. n64rx now writes `gen/xrc_missing.c`, a weak body per
called-but-undeclared name that logs `[ULTRA] missing <name>` once and
returns 0; a strong body (the runtime's, a pack's, another agent's
`more2.c`) replaces it at the link (the game side is linked as objects, not
an archive, so the weak body never hides one). The common ones got real
bodies in `more3.c` (the RSP status register as a halted RSP with "task
done" up, the CP0 compare and VI context writes ignored, the signed 64-bit
conversions). Switches: `-DXRC_NO_MISSING_STUBS=1` (the old link errors),
`-DXRC_MISSING_FATAL=1` (a card at the first call).

**A load into `$zero`** comes out of the pinned N64Recomp as
`0 = MEM_W(...);`, which C rejects (Mace and two other games). n64rx rewrites it to
`(void)(MEM_W(...));` after the recompile: the read happens (it may be a
probe), the result is dropped, as on the CPU. No change to `tools/recomp/`.

**`floor.*`/`ceil.*`**: the generator calls libm's `floorf`/`ceilf`/`floor`/
`ceil`, which `recomp.h` did not declare (Jet Force Gemini). They map to
`__builtin_*` (pdclib's, the interpreter's already).

**Titles**: the header's name is its printable ASCII (Shift-JIS titles
broke n64rx's own output on a cp1252 console: a traceback); all-Japanese
headers become "N64 game <code>". cxbe's title passes CMake's post-build
shell line unquoted, so an apostrophe ("Cruis'n USA", "Majora's Mask")
broke the link step; it gets a safe subset and `xbe_sections.py` writes the
real title afterwards.

## The BACK menu on a full texture pool (2026-10-16, Mario Kart 64 on red)

**The menu drops the game's texture cache when its card finds no room**
(`f3d_present_held`, `hold_frame`; `-DXGFX_HOLD_EVICT=0`). The card is a
512x512 texture (1 MB) made each frame it shows; Mario Kart 64's races
leave the pool with ~1 MB free in pieces, so on the console the menu opened
with no card. Reserving 1 MB for the card for the whole game was the other
choice: it costs every game a megabyte of texture pool for something shown
only while the game is frozen. Dropping the cache costs nothing while the
game runs; after the menu closes the textures reload from RDRAM (a few
frames of uploads).

## Working in parallel (2026-10-16)

**Agents work in git worktrees on their own branches, never commit, and
the orchestrating session reviews and merges** (the maintainer's direction:
"create these subagents and ... review their work and then merge it back
in"). Each agent ran headless (`claude -p`, auto permission mode: the
classifier refused agents with permission checks skipped) on the
maintainer's console API credit with a hard `--max-budget-usd` cap, kept
`AGENT_PROGRESS.md` current so a cap never lost work, owned a named set of
files and an xemu slot of its own (slots 9-14 with separate HDD images), and
noted every touch outside its files. Docs conflicts were merged as the
union of both sides; code conflicts by hand, then the host tests and a
no-game build before each merge commit, and Mario Kart 64 / Pilotwings 64
builds after the larger ones.

## Overlays keyed by ROM offset, switched on the DMA (2026-10-16)

Functions of segments that share vram are named `func_<vram>_<ROM offset>`
and keyed `vram | ROM offset << 32` in the analyser, rather than giving
N64Recomp relocatable sections: the game's overlays are linked at fixed
addresses, and N64Recomp already turns an ambiguous `jal` into a lookup.
The runtime maps the overlay the last PI DMA loaded (the source ROM range
decides, not the bytes), which is cheap (one pass over the section's
range) and needs no hashing of RAM. KSEG1 register reads get shadow pages
rather than per-site patches, so a ROM-only build needs no pack for them.

## Hardware registers through KSEG1 (2026-10-16, agent `mmio`)

**KSEG1 RDRAM is aliased; the RCP's registers are a model the interpreter
reaches, with register pages as a fallback for recompiled code.** The sweep
stopped 75 ROM-only games on loads and stores at 0xA4xxxxxx (libultra
versions the rules did not name, recompiled instead of replaced) and 9 on
uncached RDRAM. Three options were weighed:

- *Trap the fault and decode the x86 instruction* (heavy: every addressing
  mode and register of the generated code, in an exception handler, on two
  triples): not taken.
- *Map a page at each register block and accept writes with no effect*:
  cheap, and it stops the faults, but a PI DMA, a PIF block or an audio
  buffer written to the registers never happens, so the game reads stale
  memory a little later instead of stopping here. Kept only as the fallback
  (the pages, below).
- *Interpret the code that touches the hardware, with a register model*:
  taken. The analyser already records every constant hardware address a
  function uses (`features.py`); `emit.hw_funcs` lists the live ones the
  runtime does not replace and `romonly.py` adds them to `interp.txt`, so
  the pipeline gives each an interpreter body. The interpreter's loads and
  stores in KSEG1 go to KSEG0 for RDRAM and to `mmio.c` past it, where a
  write with an effect calls the runtime's own path (the same code the
  named libultra functions use), so a raw DMA behaves exactly like a named
  one. These are small, cold routines (Ogre Battle 64: seven; Mario Kart
  64, Super Mario 64: none; Pilotwings 64: one, its RSP-hang recovery), so
  interpreting them costs nothing visible. A function the rules name is
  still replaced; the safety net's code runs in the same interpreter and so
  reaches the model too (Dr. Mario 64's PI poll).

KSEG1 RDRAM takes the TLB's page-table aliasing (`xhw_alias.c`): the
window's frames mapped again at host P + 0xA0000000, so the generated
code's ordinary load works there with no extra instruction. Past the
window's size, KSEG1 reads 0 and keeps nothing (an N64 with no Expansion
Pak; `osGetMemSize`'s probe sees the size the pack gives). Each register
block's 4 KB page is a page of ours aliased at its host address, so a
register reached through a pointer the analyser could not follow reads the
idle machine instead of faulting; the VI thread puts the status words back
each retrace. The PIF page sits at host P + 0xBFC00000, above the page
tables, in kernel space the kernel did not map at boot (the sweep's faults
there show it); if a page cannot be had it is logged and that block faults
as before. Kill switches: `-DXRC_KSEG1=0`, `-DXRC_MMIO=0`,
`N64RX_MMIO_INTERP=0`.

## A TLB miss runs the game's own exception code (2026-10-16, Rare's games)

Rare's games page in on demand: GoldenEye's refill vector at 0x80000000
fills EntryLo from a page table through Context and tlbwr, and a miss on
an invalid page goes through libultra's fault path to its fault thread
(`crashMain`), which loads the page from the ROM; Conker's
`__osException` sends a miss in 0x15000000-0x151FC000 to its own handler,
which decompresses the page and maps it. On the Xbox the access faults at
rdram + vaddr. Chosen: the crash handler asks the runtime first
(`xhw_crash_set_fixup`); `tlb.c` runs the game's own exception code in the
interpreter as the CPU would (BadVAddr, Context, EntryHi, Cause set; the
vector if the game wrote one, else its `__osException`, found by n64rx
through `__osExceptionPreamble`) until its eret, on a scratch register
file with the stack below the thread's; if the page is still not valid,
libultra's fault path (`thread.c`: `__osFaultedThread`, the thread
stopped and flagged, OS_EVENT_FAULT out, wait until the game makes it
runnable). Then the page is aliased and the instruction runs again.
Rejected: emulating each game's paging in C (game knowledge in the
runtime; the analyser finds the libultra pieces by their shape instead).
While that code runs, the game's own `__osRunQueue` (found by n64rx in
`__osException`'s priority check, `-DXRC_OS_RUNQUEUE`) points at a
sentinel of priority -1, so its dispatch returns to the running thread:
the runtime's threads never fill that queue (Conker read its empty head).
Open: a KUSEG page whose host address lands on the kernel's map
(GoldenEye's 0x7F000000 with this RDRAM window) cannot be aliased; it
needs translated loads and stores for that game. The 6105 boot chip's
values are the same kind of fact: `osCicId` from the ROM's IPL3 (Conker
starts its main thread only on 6105, Banjo-Kazooie breaks its models
unless 6103) and IPL3's word at RDRAM 0x2FE1C0 (Donkey Kong 64 waits for
it).

## Captured code is checked before it runs (2026-10-16, `guard.c`)

Code captured from RDRAM was recompiled from the bytes one run saw, but
decompressed overlays share addresses (Banjo-Kazooie's levels load at
0x803863F0; Donkey Kong 64's and Conker's do the same). Chosen: n64rx
hashes each captured function's own bytes as recompiled (`gen/xrc_guard.c`;
a page's hash changed with the data beside the code: Banjo-Kazooie's core
code shares its first page with data, and every call there missed) and
wraps each function with a check of its state; an unverified function is
hashed in RDRAM on its next call, and one whose bytes differ runs in the
interpreter and its page is captured again. A page is
unverified again when the game invalidates the instruction cache over it
or a PI DMA writes it. Rejected for now: recompiling every variant of a
page (the overlay sections would share vram; N64Recomp resolves such jals
through lookups, which the runtime would then pick by hash): the next step
once variants are captured. Kill switch `N64RX_GUARD=0`.

## Coroutines through setjmp and longjmp (2026-10-17, `strand.c`)

Mario Party 1-3 run every game object as a process (Hudson's HuPrc) that
switches stacks with the game's own setjmp/longjmp: the scheduler setjmps
and longjmps into a process at its entry and stack; a process setjmps its
own buffer and longjmps back. Recompiled, each N64 call is a host call, so
the longjmp returned into the wrong host frames and the main loop's $s2
came back wrong. Chosen: each context runs on a host thread of its own (a
strand), all the same N64 thread to the runtime, one running at a time.
n64rx finds the pair by shape (sp, ra, s0-s7, fp, f20-f31 at arg0) and
routes it to the runtime; each call to setjmp in the generated C becomes
`XRC_SETJMP`, a host `__builtin_setjmp` in the caller kept per N64 buffer.
A longjmp to a buffer whose setjmp is still as it wrote it wakes that
strand, which host-longjmps back into its live frame with the buffer's
registers; any other longjmp (a new process, or a dying one's buffer
rewritten) starts a strand at the buffer's ra and sp and ends the one that
waited there. Rejected: copying host stacks (the recompiled frames are not
relocatable) and interpreting the processes (all game logic runs in them).
DIVERGES: a context that returns from its entry waits for good. Kill switch
`N64RX_STRANDS=0`.

## A task's done events belong to its thread (2026-10-17, `sp.c`, `thread.c`)

Tasks run inside `osSpTaskStartGo` and their done events wait until the
starting thread blocks (Blast Corps, above). Two holes showed with Mario
Party's nnsched, whose audio and graphics threads share one SP queue:
the thread that blocked flushed the events into the queue it was about to
wait on and then slept anyway (the check came before the flush): now it
goes back to the caller's check. And any thread's block, or a retrace,
posted every held event: the graphics thread, waiting for the audio thread
to pass on the done of the graphics task the audio thread had yielded,
took the audio task's own done, and both threads waited for good (one run
in two). Chosen: each held event names the thread that started the task;
that thread's block posts its own, and a retrace posts one whose thread is
blocked already or that is two retraces old (a thread that polls). A
thread back from a flush leaves the message to a thread that blocked on
that queue first at its priority or above, as libultra hands it to the
first waiter. Kill switch `-DXRC_RCP_OWNER=0`.


## Another thread stopped stops at its next block (2026-10-17, `thread.c`)

osStopThread of another thread was ignored and osDestroyThread of another
left it running. Mario Party 2 destroys its audio thread, closes the audio
library and makes the thread again; the old one woke on a retrace and read
the freed globals. On the N64 the target is not running at that moment
(one CPU): it is ready or blocked. Chosen: the target is flagged, woken if
it is blocked, and stops at its next block with no lock held; created and
started again, it starts over at its entry, as a thread stopped by itself
already did (HAL's object threads). Rejected: suspending the host thread
at once (it may hold the log lock, the runtime's lock or a kernel lock).
DIVERGES: a target busy in its own code runs on until it blocks. Kill
switch `-DXRC_THREAD_HALT=0`.

## KSEG2/3 through the game's TLB (2026-10-17, `XRC_HIGH_TLB`)

The TLB's pages are aliased on the host at `rdram + vaddr`; for KSEG2/3
(0xC0000000 and up) that is the Xbox kernel's system space, where a page of
ours would race the kernel's own allocator. Paper Mario runs its effects at
0xE0200000. Chosen: the runtime logs such a mapping as a learn line and
the next `--learn` build compiles with `XRC_HIGH_TLB`: the generated code's
`XRC_ADDR` sends an address at 0xC0000000 and up through the game's TLB to
its KSEG0 alias (one compare a memory access, the branch rarely taken; a
miss runs the refill code and the fault path). Only games that map KSEG2/3
pay. Rejected: decoding the faulting x86 instruction in the crash handler
and emulating it (every load and store form the compiler emits). Override
`N64RX_HIGH_TLB=0/1`.

## KSEG2/3 addresses in the runtime's helpers go through the TLB (2026-10-17, `XRC_TLB_KSEG2`)

The generated code reaches KSEG2/3 through the game's TLB since
`XRC_HIGH_TLB`, but the runtime's own helpers (`xu_phys`: DMA
destinations, queues, task fields) only translated KUSEG and masked the
rest. Paper Mario DMAs its effect code to 0xE0200000 (mapped to physical
0x1A6000): masked, it landed on physical 0x200000, over the obfuscated
boot code still running at 0x80200000, and the interpreter returned into
the effect's bytes (a store to address 0). Chosen: `xu_phys` sends 0xC0000000
and up through the TLB entries as well (nothing changes for games without
a valid entry: the lookup returns at once). Found from the decomp: its
`effect_globals` segment at vram 0xE0200000, the DMA's ROM offset in a
`[WATCH]` on the overwritten word.

## Frames presented at the swap (2026-10-17, `gfx.c`, `XRC_PRESENT_ON_SWAP`)

The interpreter presented a frame at the end of every graphics task. Mario
Party draws one frame in several tasks (its swap-chain thread sends tasks
until the last one's flag, then calls osViSwapBuffer: `src/engine/graphics.c`
in the decomp), so every other picture was part of a frame on black: on
the console it flickered too badly to play (`vmp1`). Chosen: once the game
has called osViSwapBuffer, a task's frame stays open after it; it is
presented at the swap to the buffer it drew into, or when a task starts
drawing into another buffer the game swaps to (a scheduler that swaps at
the retrace after the task, as Ocarina of Time's, can run the next frame's
task first). A game that never swaps, or 16 tasks with no present on a
swap, goes back to a present per task. Rejected: presenting from the VI
thread at the retrace that latches the swap (the frame would wait up to a
retrace more, and the back end's present is not the VI thread's to make).

## IPL3's copy of itself left in low RDRAM (2026-10-17, `boot.c`, `XRC_IPL3_LEFT`)

The IPL3 ends by copying its last part from SP DMEM to RDRAM 0 and running
it there; the console then keeps those words below the boot segment, save
the few that osInitialize overwrites with its exception preamble.
Banjo-Kazooie checks one (`viewport.c`: the word at 0xA00001D8 must be
0xAC2B0010) and otherwise draws with near 750 and far 1250 (the console's
"serious near and far clipping", `vbk1`; the sky, drawn with 5 and 15000,
disappeared with it). Chosen: at boot the runtime finds the copy in the
ROM's own IPL3 (`addiu t0,t0,X` and `addiu t3,t3,Y` before `lui t1,0xA000`)
and puts ROM X..Y at RDRAM D (`addiu t1,t1,D` before the loop: 4 in the
6105's, which Donkey Kong 64 checks at 0xA00002E8, 0 elsewhere). All 268
ROMs of the library have the copy, in six variants (0x4C0..0x774 in 240 of
them). No game's bytes or addresses
are in the runtime: the ROM supplies them. Rejected: running the IPL3 in
the interpreter (it initialises RDRAM and the RCP through registers the
runtime does not model).

## 720p from 24 MB free (2026-10-17, `xhw_video.c`, `XHW_VIDEO_720P_KB`)

PLAN.md 3.10 gated 720p on 32 MB free before pbkit starts. Measured in
xemu, 720p costs about 3 MB over 480 (Mario Kart 64: 21020 KB free after
nv2a at 480i, 17964 at 720p; the ROM cache also takes its 720p cap), and
the rule turned down Smash Bros. (31376 KB free at that point) and
Banjo-Kazooie (29600), which run with 14-15 and 13 MB free at 480.
Chosen: 24 MB, which leaves them 10-12 MB at 720p. To confirm on the
console (`[MEM]` and `[BEAT]` free lines at 720p). `-DXHW_VIDEO_720P_KB=32768`:
the old rule.

## The screen's size from the buffer the game swaps to (2026-10-17, `f3d_interp.c`, `XGFX_HIRES`)

The renderer mapped N64 coordinates as 320x240 always. GoldenEye's front
end draws into 440x330 buffers (its decomp's `fr.c`: the VI's xScale and
yScale set from the buffer's size), so the right part of every menu
(the folders, the legal text) fell off the screen. Chosen: when the game
sets a colour image it swaps to (osViSwapBuffer's addresses), its width is
the screen's, and the height 3/4 of it unless the VI mode's rows (field 0's
yScale over its lines) say otherwise (640x240). A 320-wide game maps as
before. The colour image's width is what the RDP draws to; the VI mode
only settles the height.

## N64Recomp patched at build time (2026-10-17, `patches/recomp`, `tools/xbox/recomp_src.py`)

The recompiler had been used unmodified at its pin. GoldenEye's code at
0x70000000 and 0x7F000000 reads its switch tables in KSEG0, and N64Recomp
reads a table at the function's own ROM offset (its own TODO), so 124 of
its functions had to be interpreted. Chosen: patches in `patches/recomp/`
(one `.md` each), applied to a copy of the submodule at build time by a
strict applier of our own; the recompiler and the generated code's cache
are keyed on the pin plus the patches. The submodule is never edited and
moving the pin stays deliberate (a patch that no longer applies stops the
build with that message). Rejected: a fork of N64Recomp (a second
repository to keep in step), working around it in the analyser (it cannot
change where the recompiler reads).

## The Controls page: a drawn controller, not capture-the-next-button (2026-10-18, `settings_menu.c`, `xhw_panel_pad.c`)

PLAN.md 5.2 listed a Buttons sub-page per port that captures the next
button pressed. The maintainer asked for a customised control screen with
the Xbox controller drawn on it (2026-10-18). Built: the BACK menu's
"Controls" row (per port) opens a page that draws the controller on the
card (a shaded body from distance fields, its buttons in their colours, the
sticks leaning with the real pad), every Xbox input with a callout naming
the N64 control it gives; the selected one pulses, what is held lights up
live, changed callouts carry a dot. The mapping goes the other way from
capture: an Xbox input is picked (Up / Down) and what it gives is chosen
(Left / Right): N64 buttons are things the Xbox controller cannot press,
so capturing "the next N64 button" has nothing to capture, and choosing
per Xbox input shows the whole layout at once. A button gives one N64
button or nothing; the d-pad and the sticks give a direction set (the
stick, the C buttons, the N64 d-pad, nothing). Stored per game and port in
settings.ini (`[portN] a = a ...`; one key per input, PLAN.md 5.2's form);
`si.c` maps through it, the default table is the old fixed layout
(`-DXRC_CONTROLS=0` forces it). BACK stays the menu's and is shown as such;
the exit combos read the raw pad, so no layout can lock a user out (and
BACK + START at boot resets every setting).

## Interrupts off runs above every game thread (2026-10-18, Kirby 64)

On the N64 a thread with its interrupts off is never preempted: nothing
can run until it turns them on. Here "interrupts off" is the runtime's
lock (`ints_off` in `ultra/thread.c`), and the game's threads are kernel
threads at fixed levels, so a low thread holding it could be preempted by
a higher one that never blocks. Kirby 64 did exactly that: its idle
thread (N64 priority 0, level 2) took the lock, the main thread (level
10) spun on its music player's state (0x800A77F8) waiting for the audio
thread, the VI thread blocked on the lock, no retrace reached the
scheduler, and nothing moved again. The holder now runs at level 14 while
it holds the lock and drops back when it lets go (`XRC_INTS_BOOST`). The
other way, a timeslice yield in the spinner, would need a hook in
generated code; this keeps the N64's rule instead. Costs a kernel call at
each interrupt-off and -on of game code (libultra's own are HLE'd).

## osTvType from the ROM's region; 50 retraces for PAL (2026-10-18)

The boot block said NTSC for every game; a PAL ROM checks and stops at
its lockout screen (Kirby 64 (E)). The region byte decides now (PAL for
D, F, H, I, L, P, S, U, W, X, Y; MPAL for B), and a PAL game gets 50
retraces a second from the console's 60 Hz vblank by skipping every
sixth, so its speed and music are right (the frame pacing is a little
uneven: 5 frames in 6 vblanks). Switches `XRC_TV_FROM_REGION`,
`XRC_PAL_50HZ`. The output stays the dashboard's mode (60 Hz on an NTSC
console).

## Three tiers of game status (maintainer, 2026-10-18)

Each game is **Gold** (fully working), **Silver** (playable: bugs, none
game-breaking) or **Experimental** (everything else). `docs/compat.md`
holds the tiers and the rules. Only a console run promotes a game, because
xemu is not hardware: an xemu-only game is Experimental however well it
runs there, a staged fix promotes nothing until the console run with it,
and a console regression demotes at once. One row per ROM (region and
revision), since another version is another build. The first table
(2026-10-18): Gold Super Mario 64, Pilotwings 64 and Super Smash Bros.
(named by the maintainer); Silver Mario Kart 64, GoldenEye, Banjo-Kazooie,
Ocarina of Time; the rest Experimental.


## A desktop app over n64rx, with a prebuilt toolchain (maintainer, 2026-10-18)

PLAN.md §8 Q4 asked whether the CLI gets a GUI. Answer: yes, an Electron
app (`app/`) a non-technical person downloads, drops a ROM on, and gets the
Xbox folder from, in the port's galaxy look. It is a shell over `n64rx`:
it runs `n64rx info --json` when a ROM is loaded (the game, its tier and
its known problems from `compat.py`) and `n64rx --events json build`
(JSON lines, `docs/app.md`); no build step lives in JavaScript. The
toolchain is a prebuilt per-OS download (`tools/xbox/pack_toolchain.py`:
Python, LLVM 21, nxdk, N64Recomp, a trimmed MSYS2 on Windows), not Docker
(Docker Desktop's WSL2 and virtualisation steps are the hard part for the
people the app is for) and not `setup.sh` (an hour of building on the
user's machine). Docker and the native toolchain stay for the CLI and for
the app in dev mode (`N64RX_TOOLCHAIN=system`). Every piece builds locally
with one command; CI only calls the same commands. Electron over pywebview
(the OS webview is missing on many Linux installs) and Pygame (no real
widgets, drag-drop or HiDPI).

`compat.py` now lists every row of `docs/compat.md` with a ROM, the
Experimental ones too, each with its "keeps it from the next tier" in a
player's words: the known problems the app shows. The rule in
`docs/compat.md` changed with it.

## The deep-space title card (maintainer, 2026-10-18, `xhw_splash_draw.c`)

The maintainer: the first loading screen "says Searching for ROM when the
ROM may already be in the XBE" and "shows the logo of the app, almost as if
it's the game's logo". Four mockups (title card, stage rail, halo, tiles)
on a design canvas; A was picked with "more space theming", A2 approved,
then its console render approved. What changed and why:

- **No game art, no app logo as a game's.** The card is the game's title
  (Sora Bold, 46 px, 32 px or two lines when long) and facts the runtime
  has: the header's code, region and revision, the size and where the ROM
  is ("built into this XBE" or beside it), "SHA-1 verified" when it is. The
  platform is a small mark and name top right; "TechProGabe Presents"
  stays, as a label (the family's line, no longer a 3x wordmark).
- **Type the CPU can afford:** Sora (SIL OFL 1.1) baked once by
  `tools/xbox/make_fonts.py` into 4-bit coverage tables (170 KB in the
  XBE, Latin-1), blended per pixel; no FreeType on the console.
- **Fast on a P3:** red's first build drew the whole sky every frame
  (~70 ms) at level 9 over the boot thread (level 8): the controllers took
  10 s and the SHA-1 never finished (the watchdog's hang card). Now the
  sky that does not move (nebula, constellation, planet) is drawn once
  into a 1.2 MB cache (freed at the mode switch); a frame copies it and
  adds ~120 stars (drifting at three depths, twinkling, two lines tall for
  480i), a shooting star and the text: red `vsm12` 6 ms a frame (the first
  64 ms). The splash thread also sleeps at least 12 ms a frame however
  long a frame took, so the boot always gets the CPU.
- **Nothing pops in:** the screen fades up from black (400 ms), the title
  fades and settles 10 px (350 ms) and is set on the first frame
  (`xrc_splash_early`, before the controllers' 1.6-2.9 s bring-up), the
  chips fade (300 ms), the stage label fades (180 ms), the bar eases to
  where the stage is and never jumps.
- `-DXHW_SPLASH_CLASSIC=1` draws the first screen (kept: it is small).

## The boot holds swapped: L + R is safe video, BACK the menu (maintainer, 2026-10-18)

"Hold triggers for settings ... literally the same menu from BACK", then
"flip flop: safe video L + R, the menu BACK; either one, you have to hold
it". Both latch after 500 ms held: L + R (both triggers past 100, no BACK)
is safe video, with START also the defaults; BACK with no trigger opens the
BACK menu before the game (`xrc_menu_preboot`). L + R + BACK is the exit
combos' start, so neither latches while all three are down, and a combo
finished on the loading screen quits before any hold is applied (nothing
is saved). The menu before the game is the same code and rows
(`settings_menu.c`, `s_pre`): drawn by the CPU into two 512x416 images
(allocated for it, freed after) that the splash thread lays over its frozen,
dimmed screen with a 200 ms fade; the controllers are read by the boot
thread at 30 a second (the splash thread stops reading them). The video
mode is not set yet, so a Video output change is used as the game starts
(no `*`, no restart), "Return" reads "Start the game", Restart and Exit
leave as in the game. The four ways back to a picture are unchanged in
kind: L + R, L + R + START, the init fallback, the probation.

## A far `j` is decided after the walk (2026-10-18, `funcs.walk`, `N64RX_J_LATER`)

Six games of the s9 sweep failed to compile with `goto` to an undeclared
label (Mario Golf, A Bug's Life, Clayfighter SC, CyberTiger, Fighting
Force, Toy Story 2): a `j` forward inside a big function was taken for a
tail call because the walk had not yet reached that far (Mario Golf's
19 KB function at 0x8003327C jumps 0x4000 ahead early in a depth-first
walk), its target became a function start, and the functions around it
were cut short, so their branches pointed past their end. A `j` forward
beyond the end so far is now kept aside and walked once the rest is
known, if the function reaches within `INTRA_J` of it by then; only the
rest are tail calls. Host-checked on Mario Golf's analysis; the sweep's
next run says how many of the six now build.

## libultra built without IDO's divide (2026-10-18, Mario Tennis, `N64RX_MESG_NODIV`)

Mario Tennis's own libultra (captured from RDRAM) has an osJamMesg that
divides like IDO's but an osSendMesg and osRecvMesg that wrap the ring by
a compare and end in `j __osRestoreInt` (a tail call). The queue rule
wanted the divide, so neither was named, the game's own osSendMesg ran on
the runtime's queues and read `*mtqueue` = NULL (round 2 onwards crashed
at once). The rule now also takes, beside IDO's shape, a function without
a divide that calls `__osDisableInt`, calls or tail-jumps to
`__osRestoreInt`, reads and writes validCount and reads msgCount and the
ring; osRecvMesg by "writes `first`, reads the fullqueue (+4), not the
mtqueue (+0)" when its arg1 is spilled to the stack. Features carry a
function's tail calls (`Feat.tails`). With it Mario Tennis runs on to a
stall (below, `docs/handoff.md`).

## Mario Tennis from its ROM alone: what the analyser and runtime learnt (2026-10-18/19, xemu)

Camelot's games inflate their code into RDRAM and keep scenes' code at one
address. Each change below has a kill switch; the default is the good build.

- **A second copy of a libultra function is named too** (`libultra.py`
  `Namer.twin`, `N64RX_ULTRA_TWINS`). Mario Tennis links osStartThread
  twice (one with the `__osRunningThread == NULL` dispatch, one without);
  only the first was named, the game started its main thread through the
  other on its own run queue, and thread 1 idled at priority 0 with
  nothing else running. The second becomes `osStartThread__2`, a body
  that calls the runtime's (`gen/xrc_alias.c`). Other rules skip twins.
- **osVirtualToPhysical with the probe inside** (`N64RX_KSEG_PROBE`):
  a `tlbp` function that also returns arg0's low 29 bits for KSEG0/KSEG1
  is osVirtualToPhysical, not `__osProbeTLB` (whose runtime version, true
  to libultra's, returns -1 for KSEG0). Mario Tennis mapped 0x02001000
  onto physical 0 through it.
- **Camelot's D-cache routines** (index-load-tag then hit-writeback,
  cache ops 5 and 25) are osWritebackDCacheAll / osWritebackDCache;
  interpreted before (their `mfc0 TagLo`), 1.5 million instructions a
  run.
- **A TLB miss runs 0x80000000 only when the game wrote a handler there**
  (`interp.c`, `boot.c xrc_ipl3_vec`, `-DXRC_REFILL_IPL3=1` to run it
  anyway): the runtime's osInitialize never writes the game's vectors, so
  0x80000000 held the IPL3's leftover code, whose checksum loop ends in a
  `bal` to itself.
- **A `jal` into another function's body interprets the caller**
  (`emit.cross_funcs`, `N64RX_XJAL_INTERP`): N64Recomp makes the target a
  static function to the next symbol, whose branches past it do not compile.
- **Several versions of captured code at one address** (`learn.py`
  `_choose`, `_versions`; `N64RX_LEARN_MAJORITY`, `N64RX_CODE_VERSIONS`):
  a captured page's bytes are the version seen most often (one round caught
  0x80042000 mid-load); other versions that differ in a quarter of their
  words or more are kept, and a run of pages with such versions becomes
  one overlay per version at the same vram (`code_<vram>_v<n>`, keyed by
  the ROM offset they are appended at, as Pokémon Snap's levels are).
  The guard (`guard.c`, `-DXRC_GUARD_VERSIONS`) runs the version whose
  bytes RDRAM holds: a miss tries the entries beside it at that vram; each
  version's body is in `xrc_guard_body`. Versioned entries are checked
  again each retrace (`-DXRC_GUARD_RETRACE`): the CPU writes a scene's
  code with no DMA the guard sees.
- **A learnt start is a root only where the bytes look like one starts**
  (`learn._plausible`, `N64RX_LEARN_STARTS`): a frame opened there, or the
  code before ending in `jr ra` / `j` and its delay slot. The menu's
  learnt starts on the intro's bytes at the same address split the intro's
  functions, and the recompiled intro code wrote a display list through a
  pointer of 0x80 as the menu loaded.
- **A captured function whose walk runs out of its segment is
  interpreted** (`romonly.py`, `N64RX_RUNOUT_INTERP`): pages captured with
  gaps between them (Mario Tennis's KUSEG 0x50000000, 0x50002000,
  0x50005000) recompiled it cut short, and it returned mid-way.
- **Captured code at TLB-mapped addresses is checked again** after every
  TLB write and every invalidation (`xrc_guard_kuseg`,
  `-DXRC_GUARD_KUSEG`): the game invalidates by the KSEG0 address and
  remaps the TLB over other pages.
- **An interpreted routine at a KUSEG address is mapped before it runs**
  (`interp.c map_entry`, `-DXRC_INTERP_MAP`), through the game's own miss
  path, as the safety net's lookup does: Mario Tennis pages its module at
  0x10000000 in on demand, and a fault in the middle of the interpreter's
  fetch ran the game's pager nested in that run (its tables came out
  wrong, a read through 0x10001 in its archive walker).

## The cartridge through KSEG1 for recompiled code (2026-10-19, `mmio.c xrc_cart_fault`, `-DXRC_CART_WINDOW`)

A Bug's Life and Toy Story 2 (Traveller's Tales) read their ROM with plain
loads at 0xB0000000 + offset; the interpreter had the model for it, the
recompiled code faulted at host P + 0xB0xxxxxx. The crash guard's fixup
now maps a page of our own there, filled from the ROM in RDRAM's word
order, and the load runs again; 64 such pages at most, reused oldest
first. DIVERGES: a store to the ROM lands in the page until it is reused.

## The whole ROM at the cartridge's KSEG1 addresses (2026-10-19, `xhw_rom_cart_map`, `-DXRC_CART_WHOLE`)

A Bug's Life reads its data in place from ROM addresses (0xB08D180C passed
as a struct pointer, megabytes of it): the page window (above) filled its
1 MB and reusing pages bugchecked (a page unmapped in our own tables and
touched again is a 0xA in the kernel). A ROM held whole in RAM is now
mapped there whole, read only, on the first such fault: the resident image
(page-aligned now) is put in RDRAM's word order once, under a guard that
lets no reader in mid-swap, and `xhw_rom_read` hands out big-endian bytes
from it as before (a piece at a time); `xhw_rom_resident` returns NULL from
then on. Past the ROM's end the bus answers with the address (16 shared
open-bus pages). A ROM in the page cache keeps the page window. DIVERGES:
a store to the ROM through KSEG1 faults.

## Audio: NULL saves dropped, a microcode per task (2026-10-19, Castlevania: Legacy of Darkness)

A watch on the game's own code at 0x80000864 (found zero when called) named
the audio HLE: aspMain lists that end `06000000 00000000` (or a save to
0x280) wrote the output buffer over RDRAM 0..0xA00, where this game's code
starts (0x80000400). A SAVEBUFF to address word 0 is dropped
(`ASP_HLE_NULL_SAVE=1`: written, as before); each task's microcode data is
looked up and a new one identified as the first was, four kept
(`-DXRC_AUDIO_MULTI_UC=0`: the first task's for all). The saves to 0x280
come and go between boots: they are a symptom of the thread order below.

## N64 priorities that share a host level, kept apart on request (2026-10-19, `rank_update`, `-DXRC_RANK_LEVELS`, `-DXRC_SEND_YIELD`)

The fixed map (`fixed_level`) puts priorities 1-10 on one level and 13-110
on one; Castlevania runs threads at 9, 10, 12, 100, 110, 120, 130 and 150,
and its boots ended differently each time (NULL reads, a callee-saved
register lost, its scheduler starved): where libultra runs the higher of
two threads alone, the kernel shared the CPU between them. The priorities
in use are now ranked each time a thread starts or changes priority: each
keeps its fixed level unless a lower one took it, then moves up one (and
back down from 14), so up to 12 distinct priorities get distinct levels; a
game whose priorities never collide keeps the fixed map exactly. A send
that wakes a higher-priority thread on the sender's own level also yields
to it until it sleeps again (20 ms at most; never with interrupts off).

**Both are off by default** (`-DXRC_RANK_LEVELS=1`, `-DXRC_SEND_YIELD=1` to
use them): the console-proven games have colliding priorities too (Super
Mario 64, Smash Bros., Mario Kart 64 each moved a level), and Castlevania's
variance was the AI backlog (below), the same with or without them.

## Castlevania: Legacy of Darkness from its ROM alone: what it took (2026-10-19, xemu)

Every boot ended differently (NULL reads, a pointer read from a sample,
the game's code wiped) until these; each has a kill switch.

- **AI_LEN is never more than the buffer playing** (`ultra/ai.c`,
  `-DXRC_AI_LEN_CLAMP`): the backlog can hold more than two buffers when a
  game queues ahead after a hitch, and osAiGetLength returned all of it but
  the queued one. The game sizes its next frame as target - length / 4 +
  0x100 into a halfword: past one buffer it wrapped, the frame overran its
  buffer into the next buffer's header, and its audio manager read a sample
  there as a pointer. This was the root of the variance.
- **No audio save below RDRAM 0x1000** on the console build (`asp_hle.c`
  `ASP_HLE_LOW_GUARD`; host tests keep 0) nor to address word 0: with the
  overrun the lists saved to 0x280 and 0, over the exception vectors and
  the game's own code at 0x80000400.
- **A ROM range loaded over a section that is alone at its address unmaps
  its functions there** (`ovl.c foreign`, `-DXRC_OVL_FOREIGN`): the game
  DMAs each actor's code to 0x801CAEA0 and 0x802E3B70; the one learnt and
  recompiled ran for every actor (a call through NULL). The section's own
  DMA maps them again; captured code keeps the guard.
- **TLB-mapped pages that hold other code at other times are left to the
  safety net** (`learn.c` `kuseg_versions` lines, `learn._drop_kuseg_multi`,
  `N64RX_KUSEG_MULTI`): it pages each actor in at 0x0E000000 / 0x0F000000;
  the whole 1 MB region is dropped once one page in it is seen changed.
- **A tail call from code run in line returns to its `ra`** (`interp.c`,
  `-DXRC_TAIL_RA`), not out of the whole run (which handed the run's caller
  an inner frame's sp and saved registers); and a jump through any register
  to where `ra` points is a return, never a tail call (Majora's Mask
  returns with `jr $t9` into code the net runs: the table knew that
  address, it was called and then run again, a NULL write at frame 104).
- **The cartridge through KSEG0 too** (0x90000000: it reads 0x90000866),
  the same window as KSEG1's.

## The framebuffer probe, narrowed (2026-10-19, `f3d_interp.c`, `XGFX_PROBE_MAXW`, `XGFX_PROBE_ROWS`, `XGFX_PROBE_CLIP`)

The probe (Pokémon Snap's photo check: small copies of the frame being
drawn answered from a software grid) also caught Mario Tennis capturing a
32x32 picture of its frame in strips: 128 copies a frame from 61x3 and
85x3 regions into a 32-wide image. Its marks went a row past that image
into the game's archive handle (the attract demo crashed at frame 1207,
every time), and every triangle of a frame scanned the whole region (the
match ran at 12 fps in xemu). The probe now takes copy targets up to 16
wide and regions of two rows or more, writes only what the copy rectangle
covers and the scissor allows, and rasterises a triangle over its bounding
box alone: the demo runs, the match is 25-26 fps in xemu, and Snap's 8x8
case (the host test) is unchanged.

## All-Star Baseball 2000: a game linked in KUSEG (2026-10-19)

Acclaim's loader (at 0x80300000) maps KUSEG 0x00100000 onto physical 0
with a 1 MB TLB entry, inflates the game there and jumps in; the game
carries its own libultra and pages more code in through 4 KB TLB entries
from its own fault thread. What it took, each with its switch:

- **libultra's globals in KUSEG** (`features.py`, `saves.py`,
  `N64RX_KUSEG_GLOBALS`): a function linked in KUSEG has its globals in
  the same 16 MB, so the getters (`osGetThreadPri`), `__osRunningThread`,
  `__osFaultedThread` and `__osRunQueue` are found at 0x0012xxxx; the
  runtime reads and writes them through the game's TLB, and leaves them
  alone while unmapped (`interp.c os_global`, `ultra.h xu_mapped`).
- **osStartThread lets a faulted thread go** (`thread.c`, under
  `XRC_THREAD_RESUME`): the game's pager maps the page, leaves the fault
  flag set and calls osStartThread; libultra's makes the thread runnable,
  and now so does the runtime's.
- **The guard checks KUSEG entries by what changed** (`guard.c`,
  `-DXRC_GUARD_KUSEG_EXACT`): a TLB write unverifies the entries in the
  virtual ranges it unmapped and mapped, a DMA or an invalidation those
  over its physical pages; before, every one unverified every KUSEG
  function, the whole game here.
- **AI_STATUS from the virtual DAC** (`ai.c`, `-DXRC_AI_STATUS`): busy
  while a buffer plays, full with another queued. The game fills the FIFO
  until it reads full; always 0, it synthesised ~200 frames a second.
- **OS_EVENT_AI per buffer, unasked** (`ai.c`, `vi.c`, `-DXRC_AI_EVENTS`):
  the event comes when the buffer playing ends (the queued one starts) as
  well as when all of them have, and the VI thread looks every retrace, as
  the interrupt comes whether or not the game asks the AI anything. Its
  audio thread waits on that event alone: it made ~6 frames a second, the
  sound events its game thread posted meanwhile overflowed the player's
  queue (libaudio drops a post with no free item: the player's own tick
  went) and the audio thread spun on the empty queue when a game loaded.
  Now it plays (batting, pitches) at ~22 fps in xemu. Castlevania's audio
  counts are the same with the switch off.
- **osSetIntMask(OS_IM_NONE) as a critical section was tried and is off**
  (`thread.c`, `-DXRC_INTMASK_LOCK=1`): holding the lock like
  `__osDisableInt` stopped Mario Party at its second frame (it switches
  coroutines under that mask and a parked strand kept the lock).
- **Captured code asks for its tables and callees** (`emit.py`,
  `funcs.py`, `romonly.py`, `N64RX_KUSEG_WANT`, `N64RX_KUSEG_TABLES`): a
  jump table in a KUSEG page no capture holds, a jr whose table bytes no
  segment holds, and a jal into such a page each name the page(s) in
  `want.txt`; the function's interpreter stub saves them the first time it
  runs (`learn.c xrc_learn_want`), and the next round recompiles it. The
  linear table finder takes a KUSEG base for code linked there. Its audio
  synthesis ran in the interpreter before.
- **Test builds put RDRAM low in physical memory** (`xhw_sys.c`,
  `-DXHW_RDRAM_HIGHEST`, default 0x02FFFFFF in test builds, the top in
  release builds): xemu reads physical 0x03631000 to ~0x03880000 (the
  usual place of an 8 MB window's low half) about 100 times slower than
  the rest, already at boot and slower later (`-DXRC_MEMBENCH` measures
  it). The game's audio tasks took 12 ms there instead of 0.3 ms and its
  game thread never got the CPU; Majora's Mask was slow at boot there too.
  xemu only: the console's memory is uniform, so release builds keep the
  top. The cause inside xemu is not known.

## A one-cycle blender that never reads the frame draws opaque (2026-10-19, `f3d_interp.c`, `XGFX_BLEND_MEM`)

FORCE_BL alone turned NV2A alpha blending on, with the combiner's alpha.
The RDP blends with the frame only where the blender formula reads it
(MEM as its P or M input); `G_RM_OPA_SURF` (0x0F0A4000: IN * 0 + IN * 1)
writes its input as it is. All-Star Baseball 2000 draws its sky and
grass that way with a combiner alpha of 0: they came out the clear colour.
In one-cycle mode, with no MEM in either cycle's P or M fields, the draw
is now opaque (alpha compare as before). The mode is common (Super Mario
64, Pilotwings 64, Smash Bros., Mario Kart 64, Ocarina of Time, Majora's
Mask, GoldenEye, Mario Tennis and Kirby 64 log it); A/B shots of the three
Gold games differ only in timing. Test builds log each render mode it
changes once. `-DXGFX_BLEND_MEM=0`: FORCE_BL blends, as before.

## Overlays the game relocates itself, recompiled (2026-10-19, `analyze/zovl.py`, `romonly.py`, `ovl.c`)

The Zelda engine links each overlay (an actor, a game state, the pause
screens, the player) at an address of its own above the RAM and loads it
where its allocator has room, then patches it in RDRAM from the relocation
table at the file's end (Overlay_Relocate). That code ran in the safety
net, interpreted: Ocarina of Time's scene-load hitches, the next big item
for its pacing. Now, from the ROM alone:

- **Found by what they are** (`zovl.py`): in the inflated image's DMA
  table, a file whose last word leads back to a five-word header (text,
  data, rodata, bss, relocation count) right after its rodata, with valid
  relocations (section, R_MIPS_32/26/HI16/LO16, offset in range); its link
  address from the game's overlay tables in the code ((vrom start, vrom
  end) then (vram start, vram end) spanning the file plus bss). Ocarina of
  Time: 468 overlays, every link address the decomp's (only the data-only
  `ovl_map_mark_data` has none); Majora's Mask: 612.
- **Recompiled as relocatable sections** at the link address, with the
  text's relocations in the symbol file (HI16 paired with the LO16 that
  reads its register, as the game pairs them; sorted, as N64Recomp walks
  them with the instructions): its own references read `section_addresses`
  through RELOC_HI16/LO16. Jump tables switch on the index and need no
  relocation. The functions their data points at are roots; their callees
  in the code are live.
- **Mapped where the game puts them** (`ovl.c xrc_ovl_relocate`): a hook
  at the start of the game's relocation routine (found by its constants:
  0x0F000000 and 0x02/04/05/06 << 24; Majora's Mask has two) maps the
  section at the address it is relocated to, clears what the table had
  there, unmaps its last place and points `section_addresses` there. An
  overlay routine left to the interpreter is entered where it is loaded.
- **Only the ones a run used** (`learn.c` `reloc` lines, `learn.py`):
  every one doubled Ocarina's image (9 MB -> 17 MB of a 64 MB console);
  the hook is there from the first build so the console reports the
  overlays it relocated, and the next build recompiles those.
  `N64RX_RELOC_OVL=all` takes every one, `0` none (no hook, the safety
  net as before); `-DXRC_RELOC_OVL=0` maps nothing at run time. The
  `reloc` line also says where the overlay landed and how long it is; the
  pages the safety net captured inside such a place were that overlay's
  code and are left out of the next build (`learn._without_relocated`,
  `N64RX_RELOC_CAPTURES=1` keeps them).
- **A relocation clears the place whole**: the range its header gives,
  and every overlay that overlapped it, all of that one's functions (a
  first version left the rest of a bigger overlay mapped under a smaller
  one, and an overlay not yet learnt cleared nothing: Ocarina stalled in
  Link's house running a stale overlay's code).

Result (xemu, `scen/ootgame`, 120 s): Ocarina of Time presented 3137
frames with 15 learnt overlays recompiled (938 functions) against 1950
before; Majora's Mask 2040 in 85 s with 45 (1578 functions) against 1887
in 105 s. Image 11.9 MB and 11.2 MB.

## The uncached alias's page tables are ours, and leave with us (2026-10-19, `xhw_alias.c`, `XHW_ALIAS_OWN_TABLES`, `xhw_alias_shutdown`)

Set 16 on red: Majora's Mask crashed after the Nintendo logo (`vmm14`)
and Mario Tennis froze on a white screen (`vmt2`), both on uncached
(KSEG1) accesses whose alias pages had "lost" their PTEs. The PDE of
0xA3800000 pointed at a table at physical 0x03A30000 in both: Mario
Tennis's run had made that table (the page just under its 4 MB window),
the XBE was left, and the page directory entry outlived the quick reboot
while its page went back to the kernel. Majora's Mask's 8 MB window then
held that very page: its own stores became PTEs (the "lost" PTEs read
80762388, 8075ecbc: N64 pointers) until an uncached store found a page
"taken". xemu's kernel never showed a table there.

- A table already in the page directory in kernel space past the physical
  map (the KSEG1 alias, the RCP's register pages) is never used as it
  is: its entries are copied into a page of ours and the PDE pointed at
  it (`table_for_ex`'s adopt; logged once per region). Our tables are
  tracked by their physical page, so a PDE the kernel changed is adopted
  again on the next heal.
- Leaving the XBE (the dashboard combo, a relaunch) takes every table we
  made where there was none out of the page directory, and gives a region
  that had an entry (a split large page, an adopted table) that entry
  back (`xhw_alias_shutdown`).
- A page healed 64 times in a row stops being healed: the access faults
  with the crash card instead of the console freezing in the heal
  (`XRC_KSEG1_HEAL_MAX`).

Kill switch `-DXHW_ALIAS_OWN_TABLES=0` (the table there is used, as
before). Not yet on red.

2026-10-20: an adopted table starts empty, its present entries counted
for the log line, not copied: the only tables found there on red were an
earlier XBE's, on a page since reused, so their "entries" are whatever
that page holds now (copied, they read as pages in use, or as aliases onto
random frames: a likely cause of Kirby 64's textures on red). And leaving
gives a split large page its entry back but never an adopted table's: it
would hand the stale entry on to the next XBE. `-DXHW_ALIAS_ADOPT_COPY=1`:
copied and handed back, as the first version did.

## Mario Party's processes: ended contexts are reclaimed (2026-10-19, `strand.c`, `XRC_STRAND_GC`)

Hudson's process manager ends a process by freeing it, never by a longjmp
that starts another context on the same buffer, so the strand behind each
ended process (a host thread with a 128 KB stack) stayed parked for good:
xemu stopped at 96 with "Too many coroutines" on the way to a board, and
red (`vmp15`) froze at the board's start with 11 MB left. A parked strand
(not an N64 thread's own first one) none of whose buffers still holds the
sp and ra its setjmp wrote there can never be resumed: when fewer than 24
strands are free, such strands are ended (their threads unwind and free
their slots). xemu: 43 ended a cycle, the board reached ("Welcome to the
Board!", "Then, the Game starts"). Kill switch `-DXRC_STRAND_GC=0`.

## osSetEventMesg: the message before the queue (2026-10-19, `ultra/thread.c`)

The event's queue was stored, then (for OS_EVENT_VI) the runtime's VI
thread started, then the message: a retrace between them posted a NULL
message. Mario Tennis's own VI manager read its type through it at its
first retrace and died at frame 2 in release builds (the test builds'
logging moved the race), the game running on without retraces handled
(`vmt2`'s white screen may be this or the alias above). The message is
stored first, then the queue, under the lock, then the retraces start.

## Calls into code loaded at run time go through the lookup (2026-10-19, `patches/recomp/0002`, `N64RX_LOOKUP_JAL`)

See `patches/recomp/0002-lookup-jal-targets.md`. The analyser's three
cases that sent a whole caller to the interpreter (a jal to no function,
to another overlay's function, into a function's body in captured code)
now list the target in `[patches] lookup_jal`, and the patched recompiler
calls it through `get_function`: Mario Tennis's interpreted functions 385
-> 129. Kill switch `N64RX_LOOKUP_JAL=0`.

## A zero divisor gives the VR4300's result, not a host trap (2026-10-20, `patches/recomp/0003`, `recomp.h`, `XRC_DIV_VR4300`)

See `patches/recomp/0003-div-by-zero.md`. DIV and DIVU are emitted as the
runtime's `DIV32` / `DIVU32`; DDIV / DDIVU got the same zero cases. The
VR4300 never traps there (lo -1 or 1, hi the dividend), code built without
zero-division checks relies on it, and the sweep's Vigilante 8 and World
Driver Championship crashed on the host's #DE. The macros also divide in
32 bits where N64Recomp divided in 64 (a libgcc call on the P3).
`-DXRC_DIV_VR4300=0`: the old expressions.

## A thread started by a higher priority on its level waits (2026-10-19, `ultra/thread.c`, `XRC_START_DEFER`, off)

Star Fox 64 (US 1.1, `NFXE`) starts its graphics thread (40) from its
main thread (100), both on level 10, then makes the message queues; the
graphics thread ran at once, sent its first DMA's message into
`gDmaMesgQueue` before the queue was made again over it, and waited on it
for good (the sf64 decomp read as the key). `-DXRC_START_DEFER=1` makes a
thread started by a higher N64 priority sharing its level wait until that
thread has slept once (200 ms at most). Off by default until it has been
run across the regression set.

Later the same night: a level apart is not enough either. With
`XRC_RANK_LEVELS=1` (every priority its own level: main 13, audio 12,
timer 11, graphics 10, serial 9) the boot race came back (0 frames), and a
new log line in `osCreateMesgQueue` ("queue made again with N64 thread
... asleep on it") named three threads (graphics, timer, serial) asleep on
queues main then made again: a host block in the starter (a log line, the
runtime's lock) lets any lower level run, where libultra never would.
`-DXRC_START_DEFER=2` holds a thread started by a higher N64 priority until
that thread sleeps, whatever their levels. With both, Star Fox 64 plays its
intro past the scene change where the fixed map's lag overflowed
`sNewAudioTasks[]` (xemu: 1716 frames at 105 s, no stall; the old build
stopped at 135).

## A learnt start is used only where the bytes can start one, in every segment (2026-10-19, `learn.py`, `N64RX_LEARN_STARTS_ALL`)

Castlevania: Legacy of Darkness crashed ~70 s into play on red (`vcv2`)
and at frame ~10800 of `scen/cvlod` in xemu: `func_8001AB28`'s s0 came
back 0 from a call. `N64RX_SP_CHECK=1` (below) named the call: the
function pointer at 0x800029D8 (its object teardown) to `func_802E54B4`
came back with sp 0x18 lower. The learn logs held a start at 0x802E5500,
a function of another area overlay the game loads at 0x802E3B70 (the
safety net logged it while that one was there); applied to this overlay's
bytes it is the delay slot of `func_802E54B4`'s jalr, so the analyser cut
the function there and its recompiled half returned without its epilogue.
The plausibility check captured code had (a frame opened there, or the
code before it ends a function) now applies to the learnt starts of every
segment: an implausible one is weak (walked over, never a split). With it
the function is whole (0x70 bytes), no call changes sp, and the scenario
runs to its end (6605 frames in 185 s, against 3908 in 260 s and a
crash). Kill switch `N64RX_LEARN_STARTS_ALL=0`.

## The learnt DMA overlays are guarded too (2026-10-19, `pipeline.py write_guard`, `guard.c`, `N64RX_GUARD_LEARN`)

Star Fox 64 decompresses a scene's code over the last one's with the CPU
(MIO0, no DMA to the place), so the runtime's overlay switch (by DMA) kept
the intro's recompiled code at 0x80187520 under the title's. `learn_*`
segments' functions are now wrapped by the guard like captured code and
checked again every retrace (`xrc_guard_retrace_each`): a mismatch runs
the bytes RDRAM holds in the interpreter and recaptures the page. Star
Fox 64 then played its opening to "Great Fox: Message from General
Pepper" (1268 frames) where it stopped at 611 before. Kill switch
`N64RX_GUARD_LEARN=0`.

## A changed function is recaptured whole, its pages kept together (2026-10-20, `learn.c`, `guard.c`, `learn.py`, `XRC_RECAPTURE_SPAN`, `N64RX_LEARN_GROUPS`)

Mario Tennis's match ran 1.1-1.6 million MIPS instructions every 300
retraces in the interpreter, round after round of learning, in the same
six functions of its scene code (0x8010CF2C, 0x14F4 bytes, the most). Two
gaps: a guard miss recaptured only the page holding the function's start,
so the next build had that version cut short at the page's end ("runs out
of the segment": interpreted again); and the versions of a span were built
page by page from each page's n-th other version, which can pair one
scene's page with another's (a function over both never matches RDRAM).
The guard now passes the function's size, and learn.c saves it from its
own start to its last page's end as one record with GROUP set in the size
word (KSEG0; through the TLB, a page each as before). learn.py makes each
such record a version of that function alone (`code_<vram>_g<n>`, rooted at
its start and what it calls there, up to 24 per span), beside the
page-by-page versions of the whole span; the record's whole pages count as
page captures too. Two first cuts failed: a group as a whole-span version
(Mario Tennis's scene span is 0x62000 bytes; eight of them took the image
from 12.7 to 21 MB, 3.5 MB free after the NV2A's share in xemu), and a
group of whole pages with the starts the analyser guessed in them (the
game hung at boot, a thread spinning in one of them).
Kill switches `-DXRC_RECAPTURE_SPAN=0` (the first page only) and
`N64RX_LEARN_GROUPS=0` (page by page only). An old n64rx reading a new
learn file stops at the first group record (its size reads as huge).

## The analysis is kept between builds (2026-10-20, `romonly.py _analyze_cached`, `N64RX_ANALYSIS_CACHE`)

`analyze()` is most of a ROM-only build's first step (Mario Party: 359 of
428 s), and a test build that changes only C flags, or a learning round,
ran it again. It is now pickled into the build folder (`analysis.cache`,
35 MB for Mario Party, 0.1 s to read), keyed by the ROM's SHA-1, every
`n64rx_lib` source file and every `N64RX_*` variable; the learning loop's
additions are applied to a fresh copy each time. It holds facts derived
from the user's ROM and stays in their build folder, as `analysis.txt`
does. Kill switch `N64RX_ANALYSIS_CACHE=0`.

## The 7102 boot chip loads at 0x80000480 (2026-10-20, `analyze/__init__.py`, `N64RX_IPL3_7102`)

Lylat Wars (Star Fox 64 PAL, the only game on CIC-7102) came back from its
boot code with three functions found. Its header's entrypoint says
0x80000400, but its code is linked at 0x80000480: read at the header's
address its jal targets fell 0x80 bytes past their functions (5316 of them
land on a prologue at 0x80000480, a few hundred at any other base). The
7102's IPL3 (CRC 0x009E9EA3, the PAL twin of 6101's) loads there whatever
the header says; the analyser now takes that address for that boot chip
(a fact about the IPL3, not the game). From the ROM alone with Star Fox
64's thread switches (`-DXRC_START_DEFER=2 -DXRC_RANK_LEVELS=1`): 2907
frames in 120 s in xemu. Kill switch `N64RX_IPL3_7102=0`.

## A TLB entry code that maps only the odd page (2026-10-20, `analyze/funcs.py tlb_entry`, `N64RX_TLB_ODD`)

Shadow Man's entry code calls osMapTLB with no even page (evenpaddr -1),
the odd page at physical 0 and 1 MB pages, then jumps to 0x00103060: its
code runs at 0x00100000 + its physical address. The analyser took the -1
as the even page's address, put the code at base 1, and found none ("the
boot code came back"). With no even page, the odd page (oddpaddr, the
stack argument at 0x10) half a pair above vaddr is the mapping: 457
functions, and the game boots in xemu (no frames yet: a read past RDRAM
through its TLB, learning rounds next). Kill switch `N64RX_TLB_ODD=0`.

## Code no walk reaches, found from the calls aimed at it (2026-10-20, `analyze/funcs.py _probe`, `N64RX_PROBE`)

Virtual Pool 64's analysis found 58 of its ~1900 functions and Starshot's
77 of ~1600: their game code sits in the boot segment past a data blob or
after the last function found, reached only through pointers, and the
gap scan looks between functions and gives up on a gap whose first walk
hits data. Now, when nothing new turns up, each run of a ROM segment that
no function covers (4 KB or more) is read for its jals aimed inside it:
when at least 8 of them, and 60% or more, land on a function's start (a
prologue, `addiu sp, sp, -n`, or the word after a `jr ra` and its delay
slot), those targets are walked as starts (Virtual Pool: 1420 of 1480).
Not for code captured from RDRAM (other overlays' calls aim there).
Virtual Pool 64: 1839 functions, 2791 frames in 70 s in xemu (none
before); Starshot: 1558. The decomp check (`test_analyze.py`, six pairs)
gives the same results with and without it. Kill switch `N64RX_PROBE=0`.

## A new thread starts with its creator's gp (2026-10-20, `ultra/thread.c`, `strand.c`, `XRC_THREAD_GP`)

Starshot's entry code sets gp (0x800B8500) and calls main; its first
thread then stored through `0xFC0($gp)` and the runtime's thread had gp 0:
a write to KUSEG page 0. gp is one value for a whole N64 program and
libultra leaves it alone in a new thread's context, so the thread kept the
gp the entry set. osCreateThread now keeps the creator's gp for the new
thread, and a strand (strand.c) takes the last one a thread was created
with. Starshot then runs (no frames yet in 90 s, learning next). Kill
switch `-DXRC_THREAD_GP=0`.

## The boot segment keeps its plain keys when a learnt segment overlaps it (2026-10-20, `analyze/__init__.py overlays`, `N64RX_BOOT_PLAIN`)

Paper Mario DMAs code into part of its own boot segment (a learnt segment
at 0x8006C000, 0xB90 bytes). Every segment whose vram another shares was
keyed as an overlay (vram | ROM offset << 32), the boot included: its
functions lost their libultra names (70 -> 0, the rule only names plain
keys) and N64Recomp refused the build ("func_80025C00_1000 is set as
ignored ... but does not exist"). The boot is never an overlay now; the
segment over it keeps its overlay keys, so the two stay apart. Kill switch
`N64RX_BOOT_PLAIN=0`.

## A learnt function goes to the overlay it ran in (2026-10-20, `learn.py`, `N64RX_FUNC_DMA`)

learn.py put a learnt function in the overlay of the newest DMA over its
address anywhere in the log, but learn.c logs, with each function, the
DMA that put its code there when it ran (`dma ...` the line before
`func ...`), or none when no DMA did (the CPU wrote it: a decompressed
map). Paper Mario loads hundreds of maps at 0x80240000, decompressed by
the CPU; a start learnt in one went to another's ROM segment and split a
function there (a crash at frame 691). Now a function goes to the DMA
logged with it; with none, to the captured bytes, or to the segment
holding its address only when that is the one code there. Paper Mario:
back to ~2390 frames (stopping at an effect mapped through its TLB at
0xE006C298, open: its DMA is logged by physical address). GoldenEye,
Banjo-Kazooie and Mario Tennis analyse the same; Castlevania's area
overlays (sharing 0x801CAEA0 and 0x802E3B70) get their own learnt
segments. Kill switch `N64RX_FUNC_DMA=0`.

## KSEG2/3 code goes through the safety net (2026-10-20, `learn.c holds_code`, `interp.c fetch`, `learn.py`/`segments.py high_load`, `XRC_HIGH_SAFETY`, `N64RX_HIGH_SEGS`)

Paper Mario maps each of its 128 effects at its own vram (0xE0xxxxxx)
through its TLB onto wherever it loaded it. A call to one ended the game
("No function at this address: e006c298"): the safety net only took KSEG0
and TLB-mapped KUSEG. Now a KSEG2/3 address the TLB maps is run by the
interpreter (its fetch goes through the TLB there too) and learnt by its
physical page; the next build looks for the one ROM table entry (start,
end, vram) whose vram range holds it and recompiles that as a segment
(`high_load`; table words at KSEG2/3 addresses are used for nothing else:
most are other words; an entry counts only with a page-aligned vram, 1 MB
at most, and a function start at the learnt address in its bytes: Paper
Mario's table names each effect's code and its graphics data at one
vram). Paper Mario: round 8 recompiles six effects (the code entries at
0xE0068000, 0xE006A000, 0xE006C000, 0xE0078000, 0xE008E000, 0xE00AE000),
runs 3139 frames in 70 s; its effect globals (0xE0200000, loaded another
way) still run in the safety net. Kill switches `-DXRC_HIGH_SAFETY=0`,
`N64RX_HIGH_SEGS=0`.

## Rare's 0x07: n commands from memory (2026-10-20, `f3d_interp.c`, `XGFX_RARE_DLINMEM`)

Jet Force Gemini's microcode (no version text, read as Fast3D) uses
opcode 0x07, reserved in Nintendo's Fast3D: `07 nn llll` with an address
runs n commands from there and comes back (`07060030 000a4f78`: 6
commands, 0x30 bytes). JFG puts each texture rectangle's SETTILE,
LOADBLOCK, SETTILESIZE and RDPHALF words there; unread, tile 0 was never
set, every rectangle missed its texture, and its menus' text and
backgrounds drew as grey boxes (`vjfg1` on red: "a broken menu"). Now
run in place (Fast3D family only, one level deep), and the main menu
draws whole in xemu (PLAY, MULTI, OPTIONS with their icons; no texture
miss). Kill switch `-DXGFX_RARE_DLINMEM=0`.

## Rare's DKR microcode: matrices, vertices and triangles from memory (2026-10-20, `f3d_interp.c` `dkr_*`, `XGFX_DKR`)

Diddy Kong Racing (and Jet Force Gemini, a later one) runs a microcode
with no version text whose geometry is not Fast3D's; the encodings are the
DKR decomp's `include/f3ddkr.h` (CC0, read as the answer key). G_MTX loads
a whole MVP (the CPU's, Nintendo's integer/fraction layout) into one of
four slots and selects it (slot in w0 bits 22-23; JFG's in 16-19);
G_MOVEWORD 0x0A selects a slot, 0x02 turns billboarding on; G_VTX loads
10-byte vertices (s16 x, y, z, RGBA) transformed by that slot alone,
from 0 or appended (w0 bit 16), a billboard vertex added to vertex 0's
clip position; G_TRIN (0x05) draws up to 16 triangles from memory (flags
0x40 = both faces, three indices, u, v per vertex in 10.5; w0 bit 16
textures them); JFG's 0xBF sets the bases its addresses are relative to.
On once a microcode with no version text sends 0x05 or 0x07, which
Nintendo's Fast3D never does. Diddy Kong Racing's intro and title in
xemu: whole (the character intro on the beach, the title's 3D flyby
behind the logo), where it drew grey boxes; host replay of its title
frame: 933 draws, 4285 triangles (6 draws without). Jet Force Gemini:
its menu background now, its 3D letters not yet (its matrices hold
values the DKR layout does not explain: open). Kill switch
`-DXGFX_DKR=0`.

## A learnt DMA that is a found segment, longer, grows it (2026-10-20, `learn.py apply`)

Mortal Kombat 4's sweep learning round failed to link ("duplicate symbol
_func_80132000_51400__g"): the analyser found its load at 0x80132000 from
ROM 0x51400 as 0x884A0 bytes, the console saw the same DMA 0xD98A0 bytes
long and ran code past the first's end, and learn.py added a second
segment with the same ROM offset and vram: their functions' overlay names
(vram + ROM offset) were the same. Such a DMA now grows the segment the
analyser found. Mortal Kombat 4 then builds and runs 70 s in xemu with no
crash (no frames yet; it crashed at boot in s10 and s11).

## The audio HLE's RDRAM span stops at RDRAM's end (2026-10-20, `rt/audio.c asp_addr`, `XRC_ASP_SPAN_END`)

The audio HLE asks for each address as a 4 KB span (DMEM's size: no
transfer is longer); Diddy Kong Racing saves its audio buffers at
0x3FF0C0, under the top of 4 MB, so the span ran past RDRAM's end and
every one of those SAVEBUFFs was dropped as a "bad address" (1.7 a task,
its music and effects with holes). Near the end the span is now what is
left of RDRAM: 0 bad addresses. The regression set's 13 games had none
before or after. The first four bad addresses are now logged with their
command (`[AUDIO] bad address ...`). Kill switch `-DXRC_ASP_SPAN_END=0`.

## The run token: libultra's one-thread-at-a-time, off until a console set (2026-10-22, `ultra/thread.c`, `XRC_RUN_TOKEN`)

`docs/roadmap.md` B1. Every thread switch the record added (`XRC_START_DEFER`,
`XRC_SEND_YIELD`, `XRC_RANK_LEVELS`, `XRC_INTS_BOOST`) patched one case of
one fact: the kernel time-slices threads that share a level and runs any
lower level while a thread waits on a host lock, where libultra runs the
highest-priority runnable thread alone until it blocks. The token makes
that the rule: one game thread holds it at a time (the highest-priority
ready one, FIFO among equals), it is handed on only at N64 blocks (a
queue, a stop, a fault wait, the idle loop's `pause_self`, a polled
`__osEnqueueAndYield`), never at a host wait, and a thread readied that
outranks the holder takes it, waiting for interrupts to come on when they
are off. The runtime's own threads are the interrupts and never hold it.
The kernel's levels stay: they still preempt, and a holder preempted by
an interrupt's wake-up runs on to its next runtime call only where it
shares a level with the new holder (the one residual difference, marked
DIVERGES). Rejected: demoting a preempted holder to the lowest level (a
kernel call from the VI thread per preemption, and priority inversion on
any host lock the demoted thread held). Off by default: it changes every
game's threading and only a console set says it is the better build;
`tests/xbox/test_thread.c` checks the order on pthreads.

## The analyser's first fit stays; a probe pass reports a second fit (2026-10-22, `analyze/libultra.py`, `N64RX_NAME_UNIQUE`, `N64RX_NAME_BITS`)

Review 4.1 found the per-function rules name the first function that fits.
Deciding at a pass's end instead (every fit collected, named only when one)
moved names in 7 of 27 library games, because the rules read each other's
names within a pass: Mario Party's `osSetEventMesg` went to
`__osSetHWIntrRoutine` once `osSendMesg` was named a rule later. So the
first fit keeps the name, and a probe pass after the names settle runs every
rule with nothing named and lists each name that a second function of a
different shape fits (`analysis.txt`). What the probe found was fixed in the
rules themselves (`osViBlack`'s bit 0x20 against `osViRepeatLine`'s 0x40,
`osMapTLBRdb`'s 0xC0000000 entry against games' own TLB writers,
`osEepromProbe`'s call besides the SI access pair against `osContSetCh`).
`N64RX_NAME_UNIQUE=1` gives an ambiguous name to the fit nearest the other
libultra names; unnaming was rejected (second copies of `__osDisableInt` in
Star Fox 64 and Blast Corps would lose their name).

## The dry boot: a CPU-only N64 on the PC, not a console round (2026-10-22, `tools/dryboot/dryboot.c`, `n64rx_lib/dryboot.py`, `N64RX_DRYBOOT`)

`docs/roadmap.md` B2. Code a game makes at run time (Rare's and Camelot's
decompressed segments, Dr. Mario 64's gzip main segment, GoldenEye's code
paged in behind the TLB) needed the console, round after round, before a
user's build was whole. The dry boot runs the ROM's own boot on the PC: a
VR4300 written here (not `interp.c`, a function-level interpreter that
recurses on the host stack and cannot follow libultra's `eret` thread
switches), RDRAM and an RCP that finishes every task at once, the game's
own libultra and exception handler running as they would on the console.
It writes the console safety net's formats, so nothing new is taught to
the analyser and what is learnt stays in the build folder (a page the CPU
wrote is the same RDRAM capture the console's net saves). Choices: a page
counts as the CPU's when most of its words were written since a DMA (the
exception vectors and a patched word leave a boot page the ROM's); a page
is saved again only when its code changed, not data beside it; a page's
superseded versions are dropped unless an older one comes back (a loader
replaced for good made learn.py keep Banjo-Kazooie's block twice, as an
overlay, whose libultra copy is never named); TLB-mapped code is saved by
its virtual page, as the console's net does; thread entries are `eret`
targets that never ran, were no exception's EPC and follow no call; START
and A are pressed on a schedule so menus give way to the code they load.
Off by default until a console set runs builds made with it; on 2026-10-23
the maintainer made `N64RX_DRYBOOT=auto` the default without that set (set 30,
staged): the dry boot runs on every ROM-only build and its learning is used
when it found 16 functions or more, so the games that need it (GoldenEye,
Banjo-Kazooie, Mario Tennis, Donkey Kong 64: the first three never start
without it) build from the ROM alone and the ones that learn nothing (Super
Mario 64, Smash, DKR, Mario Kart 64, Ocarina) build as before. The app's
toolchain bundle carries the dry boot prebuilt (`dryboot/<source hash>/`,
`N64RX_DRYBOOT_DIR`): its MSYS2 has clang without a C library.

## Status.FR per context: odd FPRs follow each thread's float mode (2026-10-22, `recomp.h XRC_FODD`, `ultra/thread.c`, `saves.thread_fr`, `pipeline.py write_fodd`, `XRC_FR`, `N64RX_FR`)

N64Recomp's generator spells an odd FPR two ways: its arithmetic on
`ctx->fN.fl` (a register of its own) and its moves through `f_odd`, the
even register's high word (FR = 0, as O32 code wants; upstream's
`uses_mips3_float_mode` is parsed but not used). A game whose threads run
with Status.FR set (MIPS III's 32 independent FPRs) breaks on that split:
Jet Force Gemini's view matrix code is hand-written with odd singles, and
its stores wrote other registers' words (`docs/renderer.md`). FR is per
thread, from the Status word osCreateThread stores (libultra's 0xFF03; FR
for every thread in Acclaim's Turok engine games, Conker, Shadow Man and
Perfect Dark; Rare's in Jet Force Gemini clears it for a negative id, which
it stores negated), and the dry boot ran both modes in one game (JFG, DK64),
so a build-wide FR = 1 would have broken the FR = 0 threads' double moves.
Chosen: the odd word is indexed from f0 by `1 + mips3_float_mode` (an
offset from ctx, so `restrict` holds; one extra load per odd access, only
in a game that needs it: n64rx rewrites the generated code and passes
`-DXRC_FR` only when the ROM's osCreateThread sets FR), each context's mode
set where its thread starts and by `cop0_status_write`. The rule is found
by the store's shape (the Status word at 0x118 of the context, made by
`ori 0xFF03` after `lui 0x0400`), libultra's struct, no game's address.
Not chosen: patching the generator (patches/recomp/: the same effect, and
a pin move). `N64RX_FR=0`: FR = 0 everywhere, as before.

## Gold means fully playable with no major issues (2026-10-22, the maintainer; `docs/compat.md`, `compat.py`)

The maintainer redefined the tiers: "Gold means the game is fully playable
with no major issues. Silver means playable but with issues." Before,
Gold was "no known bug a player would notice". So a Gold row may now list
minor issues (the app shows them, `test_compat` checks both sides agree),
and Silver is a game with issues of which at least one is major, none
game-breaking. Only a console run still promotes; the rows stay as the
runs left them until the maintainer reclassifies, since what counts as
major is theirs to say. Candidates whose listed issues read as minor:
GoldenEye 007 (road-marker flicker, dips to ~40 fps in intense scenes),
Castlevania: Legacy of Darkness (texture issues), Mario Kart 64 (the map
TV, drift boosts in question), Banjo-Kazooie (the intro's texture errors,
shadows now and then orange); Ocarina of Time's scene-load hitches and
audio skips are the likeliest to count as major.

## The app carries its build tools; the zip is the download (2026-10-22, the maintainer; `app/scripts/stage-toolchain.js`, `app/scripts/dist.js`, `paths.shippedBundle`)

The maintainer: "fix the tools dependency in the app ... download run and
go". The first-run download was never live (no archive published, so
`toolchain.json` held placeholders and every user had to install the tools
themselves). Chosen: `npm run dist` puts the toolchain bundle, as
`pack_toolchain.py` stages it, inside the app (`resources/toolchain/`,
already unpacked), and the app uses it before anything else: no download,
no unpack, no install, no network to build. The order is the shipped
bundle, then one unpacked in the data folder, then the download (kept for
`npm run dist:no-tools` and a future published archive);
`N64RX_TOOLCHAIN=system` still forces the system's. The Windows artifact
is the zip of the unpacked folder only: the portable single-file exe was
dropped, since it unpacks itself to `%TEMP%` at every start and with the
tools inside that is more than half a gigabyte each launch. The bundle's
nxdk loses the upstream test suites and IDE projects the build never reads
(`NXDK_PRUNE`): libcxx's tests had 176-character paths, which break
Windows' unzip (260) once the folder is a few levels deep; the longest is
now 103. Not chosen: a self-extracting first run (the same wait the user
complained about) or an installer (the app is meant to be one folder to
delete). Cost: a bigger download (the zip against a 125 MB archive plus the
app); macOS and Linux get the same layout but were not built here.

## Per-ROM patches: functions written in C for one ROM (2026-10-23, `games/patches/`, `N64RX_PATCHES`)

The runtime stays game-agnostic, and a fix that only one game's code needs
had nowhere to live in a ROM-only build: the full packs (`games/<id>/`) hold
symbols and configs, and Donkey Kong 64 has none. Its audio loop
(0x80601D24) assumes it keeps up with a timer its scheduler sets every
second retrace; in xemu it does not (the scheduler runs display lists inside
its own task start), a stale start message reaches it, and it waits for ever.
Runtime switches (done events sooner, ranked levels, the run token, timers
not queued twice) narrowed the gap and did not close it. A patch keyed by the
ROM's SHA-1, like `games/learned/`, replaces that one function with a C
version that drops a start message finding no task waiting (`DIVERGES`),
which is safe because only the loop itself starts the game's audio tasks.
The patch is MIT code written here from the decomp's description; it holds
addresses and C, no bytes of the game. Kill switch: `N64RX_PATCHES=0`.

## The first build is the best build: recipes for the learnt code, not its bytes (2026-10-23, maintainer)

Set 31 on red: GoldenEye built from the ROM alone ran at 12-34 fps (123
routines interpreted) where the learn-log build that earned Silver
interprets 34. The maintainer: this cannot ship; a user gets the best
performance by opening the app and building, and it is the next run's top
priority. The learn logs' code bytes cannot be committed (they are the
game's code; the repository is public and CLAUDE.md's first rule forbids
it), and a learning loop the user runs is a later addition, not the answer.
So what the console learnt is turned into recipes that make the same bytes
from the user's own ROM at build time: unpacking facts (ROM offset,
algorithm, destination), dry-boot input routes into each game's levels, and
checksums of the pages the console captured to prove the build recreated
them; plus analyser fixes for the routines a rule, not missing bytes, keeps
interpreted. Tiers describe the ROM-only build (`docs/compat.md`).
`docs/roadmap.md` section 0 has the order of work.

## The build's training asks again rather than ship more facts (2026-11-04)

The gap between a ROM-only build and a learn-log build was mostly pages the analysis
could name but nobody had saved. So the dry boot runs again with the analysis's own
`want.txt`, on the user's PC, from the user's ROM: nothing new is committed and it works
on ROMs the project has never seen, where recipes per game would not. Routes stay the way
to reach what a boot never runs, and are not on by default for any game until one proves
itself in a full build. Raising TLB misses to page in wanted pages was built, measured
(2 pages of 232 across 28 games) and removed rather than kept as an untested path.
