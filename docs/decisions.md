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
a Controller Pak until the game writes the Rumble Pak probe (0xFE or 0x80
to 0x8000) before it ever read the pak's file system; then it is a Rumble
Pak until a restart. Games that keep saves on the pak and probe for rumble
later keep their saves (a save matters more than a motor); rumble through
the replaced osMotor* works whatever the port holds. The BACK menu's Paks
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
