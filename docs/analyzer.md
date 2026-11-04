# The ROM analyser (as built)

`docs/PLAN.md` §3.14 is the design; this file is what exists. The goal it
serves is the project's: **someone gives `n64rx` their ROM and gets a
working XBE**, with no decomp and no game pack. A pack, when one exists,
only adds fixes on top.

```
n64rx build <rom>                  # no pack for this ROM: analysed from the ROM alone
n64rx build <rom> --analyze        # analysed even when a pack knows the ROM (how the analyser is checked)
build-xbox/<id>/analysis.txt       # what it found: segments, functions, the libultra names and the rule behind each
build-xbox/<id>/pack/              # the generated pack: syms.toml, recomp.toml, interp.txt (never committed)
```

Everything the analyser derives comes from the user's ROM on the user's
machine and stays in their build folder.

## The steps (`tools/n64rx/n64rx_lib/analyze/`)

| step | file | what it does |
|---|---|---|
| boot segment | `funcs.py` `parse_entry`, `__init__.py` | IPL3 copies 1 MB from ROM 0x1000 to the header's entrypoint (the 6103 and 6106 boot chips 1 or 2 MB below it: the entry code's own BSS and main addresses say which; the 7102's, Lylat Wars', to 0x80000480 whatever the header says, `N64RX_IPL3_7102`). The entry code is read for the BSS clear and the jump to main in the shapes the toolchains wrote: the SDK's (`lui/addiu t0` start, `lui/ori t1` size, a `sw zero` loop, `jr t2` or `j`); a start-to-end loop (`sw zero, -4(t0)` after the increment) and main called with `jal` (the entry spins after it; the entry function then ends after the call); empty clear loops skipped; the stack set and a `j`/`jal` at once with no clear (the boot segment is then IPL3's whole 1 MB). The BSS start is where the loaded segment ends. 245 of the library's 258 distinct games' entries parse; the rest are their own loaders (Acclaim's: TLB-mapped code at 0x00200000+, read by `funcs.tlb_entry`: a call to a TLB writer with the virtual and physical addresses, then a jump into the mapped code; with no even page, evenpaddr -1, the odd page half a pair above, Shadow Man's, `N64RX_TLB_ODD`). |
| functions | `funcs.py` `discover` | recursive descent from the entry and main over every `jal`; each function walked through its branches to its last reachable instruction; `j` onto a known start is a tail call; jump tables read from `lui`/`lw`/`jr` (the `lui` may sit in a delay slot) bounded by the `sltiu` before them, or, when GCC hoisted the base out of a loop, from the last `lw`/`addu`/`lui`+`addiu` writes before the `jr` (every entry must land in the function); then the gaps between functions, and the code before a segment's first function, tried as functions (clean walks only), again from what those call, until nothing new. Dead epilogues after endless loops (`lw ra` / `addiu sp` first, no branch reaches them) fold into the function before; so does unoptimised IDO's closing `jr ra; nop` after the return statement's own (only in a segment whose `b .+8; nop` count says it is -O0 code, and never when a call, a data word or a `lui`/`addiu` pair names the address). |
| segments | `segments.py` | every call whose arguments, by constant propagation (`lui`/`addiu`/`ori`, `addu`/`subu`, `and`/`or`, shifts, the stack slots, locals kept there by -O0 code read back), hold a ROM offset, a KSEG0 address and a size (or a ROM start and end: `dma_copy(start, end, dest)`, `dma_read(dest, start, end)`) is a candidate DMA of code; so is every (RAM, ROM start, ROM end) or (start, end, RAM) word triple anywhere in the ROM (a script's load command, an overlay table). A candidate becomes a segment when the known code calls into it, the call targets look like function starts (a frame opens, or the words before close one), and every walk from them is clean. A start and end next to each other win, then a size in its natural place (the argument after the two addresses). |
| features | `features.py` | per function, a straight-line value tracker: calls, COP0 and FPU control registers, cache ops, TLB ops, hardware registers (VI, AI, PI, SI, SP, DP, MI by address), loads and stores at argument + offset, through pointers read from globals (and what was stored), 64-bit ops, `ddiv`/`dmult` kinds. Delay slots run before the call clobbers. |
| libultra | `libultra.py` | names by behaviour, no fingerprints: COP0/cache anchors (`__osDisableInt`, `osGetCount`, `osWritebackDCache`...), hardware anchors (`osAiSetFrequency`, `osPiRawStartDma`, `__osSiRawStartDma`, `__osViSwapContext`...), then the call graph and libultra's struct offsets (OSMesgQueue, OSThread, OSIoMesg, OSTimer, the VI context behind `__osViNext`), IDO's 64-bit helpers by their one `ddiv`/`dmult`, the TLB writers by their arguments (`osMapTLB`, `osUnmapTLB`, `osUnmapTLBAll`, `osMapTLBRdb`). Rules run to a fixed point; a name is given only when exactly one candidate fits (the hardware rules try every function first). |
| liveness | `emit.py` `reachable` | the game's code reached from the entry, main, function addresses in data (in the segments and anywhere else in the ROM: scripts, display-list callbacks) and in code, with the edges out of the functions the runtime replaces cut. Dead code (libultra's internals only replaced functions reached) is not recompiled. |
| sizes | `emit.py` `_size` | a function is translated to its end plus a branch's delay slot, and a function whose path runs off its end (its last instruction, or the one its delay slot follows, not a `jr`, `j` or unconditional branch) takes the next function's body too, as the CPU runs on into it (`N64RX_FALLTHROUGH`): GoldenEye's hand-written `cosf` is three instructions that fall into `sinf`, and recompiled it returned with `$f0` unset (every cosine wrong: the Dam's sky and far geometry, Bond's gun, the pause watch). A handful a game elsewhere (Mario Party 2, Super Mario 64, Pokémon Snap and Smash Bros. 1 each, Mario Kart 64 none). |
| return addresses | `emit.py` `ra_hooks` | a function that reads `$ra` as a value (an offset from it, a load through it, arithmetic on it; not `jr $ra`, a store or a move) gets its return address at every `jal` from recompiled code, through an N64Recomp `[[patches.hook]]` that sets `ctx->r31` first (the generated code never sets it); an interpreted one enters through `xrc_interp_ra` with that ra (`N64RX_RA_HOOKS`). Jet Force Gemini's resolver for its relocatable code looks up the `jal` that called it at `ra - 8` (1 function, 497 calls); with the interpreter's sentinel there it found nothing and the game wrote through a null pointer at boot. |
| output | `emit.py`, `../romonly.py` | the symbol file, `recomp.toml` (ignored = dead + interpreted; a real pack's patches mapped by address), `interp.txt`, the report. Interpreted: hand-written code (a return through a register other than ra), live functions that reach the RCP's registers by a fixed address and are not replaced (`emit.hw_funcs`, `N64RX_MMIO_INTERP=0`), and functions with a branch or `j` into another function's body or to code no function holds (`emit.cross_funcs`, `N64RX_XBRANCH_INTERP=0`: C cannot jump to another function's label). Before N64Recomp runs, what it would crash on or refuse one run at a time (`emit.recomp_hazards`, `N64RX_RECOMP_PRECHECK=0`): a jump table outside the function's own segment that the patched recompiler cannot read either (`patches/recomp/0001` reads a table from the first section holding its vram; unpatched, N64Recomp took the table to be in the function's section and read the ROM past its end: Conker's TLB-mapped code with its tables in KSEG0 segfaulted it; the finder mirrors its `analysis.cpp`; a table passes when its first entry, read where the recompiler will, lands in the function, `emit._table_elsewhere`; captured KSEG0 RDRAM that differs from the ROM segment under it, a boot that decompressed over itself, is written first as sections with no functions for this, `learn._table_sources`: GoldenEye's 0x70000000 code reads its tables at 0x8002xxxx), every one with its reason in the pack's `hazards.txt`, a `jal` to an address with no function outside the caller's segment, an `mfc0`/`mtc0` of a cop0 register other than Status. The boot chip (`rom.cic`, the CRC-32 of IPL3) goes to `osCicId`: Conker starts its main thread only on 6105, Banjo-Kazooie checks 6103. Without a pack the title is the header's ASCII, the title ID `0x4E52` + a hash of the game code, the save type from a facts table or 4 Kbit when the game probes the EEPROM (SRAM and Flash decided at run time). Yaz0 games are inflated before all this (`yaz0.py`). |

## What the runtime adds for ROM-only builds

- **The interpreter** (`xbox/src/rt/interp.c`): hand-written assembly the
  recompiler cannot express (a return through a register other than ra,
  `bal` into another function's code, a jump through a register with no
  table) and any live function N64Recomp refuses run here, from RDRAM, on
  the same context; calls to known functions go back to recompiled code.
  `n64rx` generates each one's body (`gen/xrc_interp.c`) and enters it in
  the function table.
- **The safety net** (`xbox/src/rt/learn.c`): a call to an address the
  function table does not know (a function pointer, a jal N64Recomp could
  only look up, a thread entry) runs the code there in the same
  interpreter instead of the fatal "No function at" card, when the
  address is in RDRAM and holds something. Each such address gets a
  22-byte x86 stub (`push vram; call xrc_safety_run`) entered in the table,
  so later lookups cost what any function does and any thread may call it;
  the stub interprets whatever RDRAM holds at the time (no decoded cache:
  an overlay that replaces the code needs no invalidation). The
  interpreter keeps a 64-bit register file for the run (so a GPR32 build
  runs 64-bit code correctly inside it) and reaches the FPRs exactly as the
  generated code does. `-DXRC_SAFETY_NET=0` brings the card back.
- **The PIF** (`ultra/si.c`): the game's own SI code (a Controller Pak
  driver, a game's own pad reader) gets real answers from
  `__osSiRawStartDma`: pad status and buttons, the EEPROM, the Controller
  Pak's read/write commands with their CRCs, the Rumble Pak.
- **The whole reimplemented list** (`ultra/extra.c`, `pfs.c`, `save.c`,
  `more2.c`, `more3.c`): the `osEPi*` handle DMA family (with SRAM and
  FlashRAM on domain 2), the later libultras' init and debugger probes,
  `osPfs*` and `osMotor*` on the same pak images, `osFlash*`, the TLB
  family, `__osEnqueueAndYield`; a game whose own recompiled pak code counts
  ports by `__osMaxControllers` gets it set (`n64rx_lib/saves.py` finds it
  in the game's `osContInit`).
- **Bodies for what nothing provides**: libultra internals N64Recomp leaves
  bodiless get a weak body in `gen/xrc_missing.c` that logs `[ULTRA]
  missing <name>` once (`-DXRC_MISSING_FATAL=1`: the card instead).
- **The TLB** (`rt/tlb.c`, `hw/xhw_alias.c`): a mapped KUSEG page is
  aliased in the Xbox page tables at the host address the generated code
  computes (`docs/architecture.md`).

## The learning loop (PLAN.md 3.14 step 5)

1. The net logs each function it ran, each one that code called that the
   table does not know, and each one any interpreted code reached with a
   `jal` (a refused function's call into a segment the analyser missed;
   a hand-written routine's `bal` into another routine is not logged), as `[LEARN] func <vram> <bytes> from <caller>`
   (once as it starts, with 0 bytes: a thread's entry may never return; once
   more with the span it reached when it returns; the caller is the call
   site in interpreted code, or the recompiled function the call came
   from), and before it the newest PI DMA that covered the address,
   `[LEARN] dma <rom> <ram> <size>` (`ultra/pi.c` records every DMA in a
   ring of 1,024; a later DMA or `osInvalICache` over a logged address logs
   it again). The same lines go to `E:\UDATA\<title id>\learn.txt`,
   appended across boots.
2. `n64rx log --pull <ip>` fetches it (and saves it as
   `<folder>/debug/learn.txt`); a serial log works as well.
3. `n64rx build <rom> --learn learn.txt` (`n64rx_lib/learn.py`, implies
   `--analyze`): a learnt function inside a known segment becomes a root
   of that segment, which is walked again (`funcs.discover`); one outside
   every segment brings in the DMA that covered it as a segment
   `learn_<ram>`. The learnt functions and what they call are live (dead
   code is otherwise left out). The merged facts stay in
   `build-xbox/<id>/pack/learned.txt`; the cache key follows the pack
   folder, so the rebuild recompiles.

4. **Code no ROM DMA put there** (decompressed: Rare's games, gzip and the
   like): the first time the net runs code in a 64 KB block of RDRAM that
   no logged DMA covers, `learn.c` saves the block as it is
   (`E:\UDATA\<id>\learn_code.bin`: records of vram, size, the bytes in N64
   order; a `code` line in learn.txt; test builds also send it over COM1 as
   `[LEARNCODE]` base64). `--learn` makes each captured block a segment
   `code_<vram>`, appended to the build's own copy of the ROM
   (`rom_learnt.z64`, what N64Recomp reads; the XBE still holds the user's
   ROM); every function found in captured code is live. The game's own
   second copy of libultra (beside a boot loader's) is named separately: a
   name the loader's copy already has becomes `<name>__2`, left out of the
   recompile and forwarded to the runtime's by `gen/xrc_alias.c`.
   Captured blocks that follow each other in RDRAM are one segment (a
   function crossing a 64 KB boundary is whole); a call from one captured
   segment into another where no function starts yet is a root there. A
   function interpreted because its jump table is in RDRAM no capture holds
   asks for it: its interpreter stub calls `xrc_learn_want` the first time
   it runs (`want.txt`), and the console saves the blocks from the code to
   the table. Code captured at KUSEG asks the same way for a table in a
   KUSEG page (its page and the next: the patched recompiler reads a table
   from another segment), for a jr's table whose bytes no segment holds,
   and for the page of a callee no capture holds yet (All-Star Baseball
   2000's game, linked at 0x00100000: its audio synthesis ran interpreted
   until its tables came; `N64RX_KUSEG_WANT`, `N64RX_KUSEG_TABLES`). Its
   globals are read in the same 16 MB as the code, so libultra's getters
   and `__osRunningThread` are found there too (`N64RX_KUSEG_GLOBALS`).
   The register width is decided on what N64Recomp reads (the
   ROM plus the captured code): Banjo-Kazooie's `bkmemcpy64` copies with
   `ld`/`sd`, and on 32-bit registers every `bk_realloc` lost half of each
   doubleword (the heap block found reused under its spawnable-actor list).
   Captured KUSEG pages the ROM holds as they are (code a game pages in
   through its TLB straight from the ROM: GoldenEye's 0x7F000000, `romCopy`
   from its game segment's ROM start plus the page's offset,
   `tlb_manage.c` in its decomp) that agree on one ROM offset become one
   ROM-backed segment `tlb_<vram>` instead, from the 1 MB base to the last
   such page and 64 KB on: the whole of it is found from the ROM, not a page
   a learning round (GoldenEye: 173 pages, 2463 functions; `N64RX_TLB_ROM=0`
   keeps the captures). It counts as captured code for the game's own
   libultra and for liveness. TLB-mapped code whose jump table is in KSEG0
   RDRAM no capture holds asks for that table's page the same way.
   Code a game pages in with its own TLB (Conker's 0x15000000) is saved a
   4 KB page at a time, read through the TLB, keyed by its virtual page
   (neighbouring virtual pages sit in any frames). The console also writes
   down the KUSEG regions its TLB maps (`kuseg <va> <1 if the host could
   not alias it>`); when one could not be, the next build puts RDRAM's
   KSEG0 at a user address that holds them all (`-DXRC_RDRAM_USER`,
   `learn.rdram_user`). A game that compares osMemSize with 8 MB gets 8 MB
   (Donkey Kong 64's "Expansion Pak" screen; `romonly._wants_8mb`).
   Open: a captured block can be saved before all of it is loaded (Donkey
   Kong 64 then calls a "function" of stale bytes); captured code needs a
   check of its bytes before its recompiled form is trusted (now
   `guard.c`). Call targets inside captured code are weak starts: other
   code jals into whichever overlay is loaded there at the time
   (`funcs.discover weak_calls`).

5. **Several versions at one address** (Camelot's Mario Tennis, 2026-10-19;
   `docs/decisions.md` "Mario Tennis from its ROM alone"): a game that
   inflates each scene's code to the same RAM (Mario Tennis: 0x800E2000 to
   0x8012A000 holds the intro, the menus, the match) was captured a page at
   a time in different scenes. Every capture of a page is counted
   (`learn._choose`): the bytes seen most often win (a page record over a
   64 KB block's), and other versions that differ in a quarter of their
   words or more are kept; a run of pages with such versions becomes one
   overlay per version at that vram (`code_<vram>_v<n>`, appended to the
   learnt ROM, keyed by that offset like Pokémon Snap's levels). A learnt
   start is a root in a version only where its bytes look like a start
   (`_plausible`: a frame opened, or `jr ra` / `j` and its delay slot just
   before). The runtime's guard runs the version RDRAM holds (`guard.c`),
   checks versioned entries again each retrace, and checks captured code at
   TLB-mapped addresses again after every TLB write. Captured functions that
   run out of their segment (KUSEG pages captured with gaps) and callers of
   a static N64Recomp made inside captured code are interpreted. Mario
   Tennis (xemu, the ROM alone + 24 learning rounds, rounds 19 on with
   input): intro, title, main menu, play mode, character select, a match.

`N64RX_WITHHOLD=<vram>,...` (a test switch) takes functions out of the
analysis as if it had missed them; a function the learn file names is never
withheld. Pilotwings 64 (xemu): its app thread's entry withheld, the net ran
it and the 138 functions only it reached (left out as dead), the game ran
at 35 fps; the rebuild with the 145 learnt functions logs nothing and runs
at 38.5 fps.

Super Mario 64 (xemu), the real case: the analyser finds one segment;
184 functions jal into the engine segment the game loads with linker
symbols, N64Recomp refuses them, they run interpreted. The first run logs
the engine and goddard segments' DMAs (`ultra/pi.c` merges SM64's 4 KB
chunks: ROM `0xF5580` -> `80378800`, `0x13490` bytes; `0x21F4C0` ->
`8016F000`, `0x4A9E0`); with `--learn` the analysis has 5,051 functions in
3 segments and 4 refused. The second run reaches the net through goddard's
function pointers, then faults in recompiled goddard code (a read of the
address `04000650`, frame ~225): goddard maps `0x04000000` through the TLB
(its renderer's `osMapTLB` calls, read in the decomp) and the runtime has
no TLB. Not the net: the runtime needs TLB mapping for SM64's head screen.
With the TLB (`tlb.c`, `docs/decisions.md` "The TLB") the same build
draws Mario's head and PRESS START; two more learn rounds (437, then ~470
learnt functions: goddard's and the engine's function pointers) and START,
START, A, A reach the castle grounds' Lakitu intro (xemu, emulator only).
The analyser names osMapTLB, osUnmapTLBAll and osMapTLBRdb (`_tlb` rule);
goddard's dynlists are read through `0x04000000` -> 64 KB pages aliased in
the page tables.

Interpreted code reaches the runtime's libultra by address:
`gen/xrc_interp.c` carries `xrc_ultra_table` (each `ULTRA(name)` of
`xbox/src/rt` at the address the symbol file gives that name), entered in
the function table before the recompiled sections.

6. **Overlays the game relocates itself** (the Zelda engine, 2026-10-19;
   `analyze/zovl.py`): a file of the inflated image's DMA table that ends
   in a relocation section (text, data, rodata and bss sizes, then
   R_MIPS_32/26/HI16/LO16 entries), linked at the address the game's own
   overlay tables pair with its vrom range, becomes a relocatable section
   at that address with its text's relocations; the game's relocation
   routine (found by its constants) is hooked and the runtime maps the
   section wherever it is loaded (`ovl.c xrc_ovl_relocate`). The console
   logs `reloc <vram>` for each overlay a run relocated and the next build
   recompiles those (`N64RX_RELOC_OVL=all`: every one). Ocarina of Time:
   468 overlays, all link addresses as the decomp's; Majora's Mask: 612.

## The dry boot: the learning loop's first rounds on the PC (`tools/dryboot/`, `N64RX_DRYBOOT=auto`, the default)

`docs/roadmap.md` B2; `docs/decisions.md` has the choices. Before a ROM-only
build, `n64rx_lib/dryboot.py` builds `tools/dryboot/dryboot.c` with the PC's
compiler (once per source; the app's toolchain bundle carries it built), runs the ROM's boot in it for
`N64RX_DRYBOOT_FRAMES` (1800: 30 s of game time, a few seconds here, START
and A pressed on a schedule), and analyses again with its learn file
(`<build>/dryboot/learn.txt`, `learn_code.bin`) ahead of any `--learn`. The
dry boot is a CPU-only N64: the VR4300 with COP0, the TLB and the FPU,
RDRAM, and an RCP model that answers as the hardware would and finishes
every task at once. It runs the game's own entry code, libultra, threads and
exception handler. For each function a call (or a thread's first dispatch)
enters that the analysis lacks, it writes what the console's safety net
would: the PI DMA under it, or its page when the CPU made it or the TLB
maps it.
With `--rdram 4` uncached RDRAM stops at 4 MB as the console's KSEG1 alias
does (a load past it reads 0, a store goes nowhere; KSEG0 keeps 8 MB):
F-Zero X's own osGetMemSize found 8 MB and ran its code past 4 MB until
2026-10-22 (`--window keep`: as before).

First builds from the ROM alone (functions in the analysis; libultra names):

| game | without | with the dry boot | the console's learn rounds |
|---|---|---|---|
| Dr. Mario 64 | 263, its gzip main segment missed | 998 of the main segment's 1,006 (decomp), boot 263/270, 74 names (unchanged) | 3 rounds |
| GoldenEye 007 | 665 in 2 segments | 3,797 in 7, 119 names | 30 rounds: 3,821 in 7, 118 |
| Banjo-Kazooie | (boot loader only) | 6,567 in 5, 107 names | 22 rounds: 6,999 in 2, 108 |
| Mario Tennis | - | 1,346 in 14, 81 names | 32 rounds: 2,375 in 29 |
| Blast Corps | - | 1,508 in 10, 96 names | 15 rounds |

Across the library (the sweep's 246 ROMs, 600 frames each, 8 MB, the
16 Kbit EEPROM where `romonly.EEP16K` has the game, the TV type of the
header's region as the pipeline passes it): 239 reach their RSP tasks, 227
of them 50 or more in the 600 frames; 12 stop under 50 (a wait on something
the model does not answer yet: Mario Golf, Ken Griffey's two, Top Gear
Rally 2, Rainbow Six, Road Rash, Blues Brothers 2000, Nuclear Strike,
Hercules, Derby Stallion 64, Robotron 64, CyberTiger). The 7 with none:
F-Zero X (CIC 6106; Yoshi's Story, the other 6106, runs), BattleTanx and
Global Assault (a NULL list node at boot), A Bug's Life and Toy Story 2
(Traveller's Tales' probe of `0xC0001000`), Xena (a NULL pointer), NBA
Courtside (every thread waits, no pad read: its scheduler gets its retraces
and the main thread waits on the scheduler). Banjo-Tooie (E) and F1 World
Grand Prix II (E) were NTSC in the first scans and stopped; with PAL they
run 286 and 445 tasks.

The FPU with Status.CU1 clear traps (coprocessor unusable), as the VR4300
does: libultra marks a thread as an FPU user in that trap's handler and
only then saves its FP registers at a switch. Before (2026-10-22) no thread
was marked, a switch clobbered FP registers, and Perfect Dark (4 tasks, an
address error in its inflater), Banjo-Kazooie (the bad pointer at frame
293), Pilotwings 64 and Top Gear Overdrive stopped early; with it Perfect
Dark's first build has 2,286 functions in 38 segments against 2 without
the dry boot. CyberTiger went the other way (569 tasks to 3): its thread 2
loses a race the trap's few hundred instructions shift, and with PAL frame
timing (`--tv 0`) it runs as before, so a timing difference in the model,
not the trap. `-v` adds what the float mode needs (`fpu:` FPU operations
with FR 1, with FR 0 and on an odd register under FR 0; `FR n at pc`: the
Status writes that change it), `--watch addr bytes` logs the CPU's stores
there with the pc and frame, and `--snap n file` writes RDRAM as graphics
task n starts (its display list as the CPU left it; scratch only: the
game's bytes).

Mario Tennis's matches: with the console's own scenario script
(`N64RX_DRYBOOT_SCRIPT`, 9,000 frames, under two minutes here) the dry boot
plays into a match; `tools/xbox/dryboot_frame.py` draws a `--snap` of it with
the host renderer (Mario serving to Luigi at graphics task 4,300). Its menu
scenes are versions of the same pages the match replaced: kept since the
versions rule (`N64RX_DRYBOOT_VERSIONS`, `docs/testing.md`), the first build
has 2,280 functions in 16 segments (the console's 32 rounds: 2,375 in 29).

`decomp_check.py --learn <file>` scores a learn file against a decomp.
Open: nothing here is checked on a console yet.

## How well it does (checked against decomps)

`tools/xbox/decomp_check.py` runs the analyser on a ROM and compares it with
a decomp's answer key; `tools/xbox/test_analyze.py` runs every pair present
and fails on a wrong libultra name, on code the decomp's names keep live that
the analysis leaves dead, or on a result below the pair's floor
(`docs/testing.md`). The keys: Pilotwings 64's symbol file (addresses and
sizes); Paper Mario's and Dr. Mario 64's splat configs and symbol files (addresses); Super
Mario 64's and Mario Kart 64's link order (their `.ld` and each file's
functions in source order, no build: lined up against the analysis between
the addresses the key knows and the libultra names, one to one where the
counts agree; "placed" below).

| game | functions found / the decomp's | segments | replaced libultra right / wrong | runs (xemu) |
|---|---|---|---|---|
| Pilotwings 64 (US) | 1952 / 1956 (the 4: libultra's exception handler) | boot + app, exact | 75 / 0; live set identical to the decomp names' | title, menus, flight, results, saves, sound: the same as the decomp-symbol build |
| Mario Kart 64 (US) | 3157 / 3159 placed; main 2657 of 2676, racing 413 of 415, ending 133 of 133 | boot + both race segments | 66 / 0 | title, menus, races (F3DEX), sound (nead), the Controller Pak; **on the console (red)**: races at 29.9 fps, the BACK menu and its Paks page |
| Super Mario 64 (US, -O0) | main 3516 of 3573, engine 283 of 284, goddard 693 of 699 (by count; few addresses to pin) | boot + engine (DMA) + goddard (a level script's load) | 66 / 0 | title head, the interactive head screen, castle grounds (Lakitu intro) after three `--learn` rounds and the TLB; the head's shading is off |
| Dr. Mario 64 (US) | boot 262 / 270 | boot; the main segment is gzip-compressed (not found) | 56 / 0; live set identical | links; boots, the net runs 4 functions, then interpreted code reads PI_STATUS through KSEG1 (`a4600010`) and faults; its gzip main segment is not found |
| Paper Mario (US, GCC) | 2443 / 2680 in the segments found | main, engine1, engine2, evt, engine4, one partner and one map (overlays sharing a vram: one each) | 56 / 0 (87 missing: GCC-built libultra, Flash, the Pfs family) | not tried |

### The whole library (`tools/xbox/sweep.py`, 2026-10-16)

246 games (one ROM each from a 276-file folder), `n64rx build --fast`, a
pack where one matches (Pilotwings 64, Mario Kart 64), the ROM alone for the
rest. s1 is before the sweep's fixes, s2 after (`docs/decisions.md`, "The
sweep and what it fixed").

| stage reached | s1 | s2 |
|---|---|---|
| XBE built | 54 | 143 |
| link (undefined libultra internals) | 78 | 0 |
| compile (clang) | 19 | 12 |
| recompile (N64Recomp stopped) | 6 | 6 |
| analyse (n64rx stopped) | 89 | 85 |

Where s2 stops, by cause (for the analyser):

| cause | games | examples |
|---|---|---|
| "the entry code is not the SDK's (no BSS clear...)" | 71 | Armorines, Bust-A-Move '99, BattleTanx, A Bug's Life, Forsaken 64 |
| "boot segment would be -0x..." | 14 | |
| compile: `use of undeclared label L_x`: a branch into another function's body (function bounds) | 12 | Mario Party 1-3, Mario Golf, Vigilante 8 1-2, Earthworm Jim 3D, Bangai-O |
| N64Recomp: "Unhandled branch in recomp_entrypoint ... to 0x84001064" (the entry jumps into SP IMEM) | 5 | Aidyn Chronicles, Ready 2 Rumble, South Park Rally |
| N64Recomp: "Unhandled cop0 register in mtc0: 14" (EPC: an exception handler, no function named) | 1 | Pokémon Puzzle League |

The 143 XBEs booted in xemu for up to 45 s each (`--run 45`; xemu, not
hardware). Crash addresses are mapped back to the N64's:

| in xemu | games |
|---|---|
| crash: game code reads or writes the RCP's registers through KSEG1 (0xA41-0xA48xxxxx) | 75 |
| crash: a KUSEG (TLB-mapped) address | 23 |
| frames presented | 17 |
| no frames (threads up, no graphics task) | 14 |
| crash: KSEG1 RDRAM (`-DXRC_MEM_MASK=1` territory) | 9 |
| fatal: no function at an address | 2 |
| fatal: the boot code returned without starting threads | 2 |
| crash: the PIF through KSEG1 | 1 |

The first is libultra the analyser did not name (Ogre Battle 64:
`osCartRomInit`'s later version polling `PI_STATUS`): its code was
recompiled instead of replaced, and its register accesses have nowhere to
land. Naming more libultra versions (or a register trap in the memory
model) moves most of the library on. Presenting frames: Mace, Mystical
Ninja, Bio F.R.E.A.K.S., Deadly Arts, Hexen, ISS 64, Automobili
Lamborghini, Rampage, Tetris 64, Tetrisphere, Wayne Gretzky's 3D Hockey,
Stunt Racer 64, World Driver Championship, Air Boarder 64, Body Harvest,
Pilotwings 64, Mario Kart 64 (frames presented, not judged playable).

### The RCP's registers through KSEG1 (agent `mmio`, 2026-10-16)

The analyser now sends every live function with a constant hardware
address that the runtime does not replace to the interpreter
(`emit.hw_funcs`, `N64RX_MMIO_INTERP=0` to turn it off), and the runtime
models the registers (`xbox/src/rt/mmio.c`) and aliases KSEG1 RDRAM
(`docs/decisions.md` "Hardware registers through KSEG1"). Marked per game:
Mario Kart 64, Super Mario 64, Dr. Mario 64 none; Pilotwings 64 one (its
RSP-hang recovery); Ogre Battle 64 seven (PI, SI, SP); FIFA 64 its boot
loader (`0x80000F00`, which DMAs the game in through PI). Rebuilt and booted
in xemu for 45 s (not hardware), 18 games of the KSEG1 categories plus the
two references: **none stops on KSEG1 any more**; every register page and
the KSEG1 alias were mapped (no `[MMIO]`/`[MEM] KSEG1` line).

| in xemu (the 18) | games |
|---|---|
| frames presented | 4: Bottom of the 9th (1163), Chameleon Twist 2, Cruis'n USA, Ogre Battle 64 (1 each: the first frame) |
| fatal: `eret` (`0x42000018`) in code the safety net interprets (the game's own thread switch, unnamed libultra) | 4: FIFA 64, NHL 99, Blast Corps, Mario Tennis |
| crash: a KUSEG address near 0 (a null pointer, or the TLB) | 4: Triple Play 2000, Bomberman Hero, F-1 World Grand Prix, Jet Force Gemini |
| no frames | 5: Chameleon Twist, Donkey Kong 64, Killer Instinct Gold, Hydro Thunder, Dr. Mario 64 |
| fatal: no function at an address | 1: Rampage 2 |

Mario Kart 64 (1165 frames) and Pilotwings 64 (1616) present as before.

### The library again: s4 to s5 (2026-10-17, xemu)

The same 246 games, built from the ROM alone (no learning rounds), booted
for 45 s each. s5 was built before that night's eret, RCP-event and guard
fixes, so its 20 "an instruction it does not run" stops are all `eret`
(now a return, `XRC_ERET_RETURN`).

| in xemu | s4 | s5 |
|---|---|---|
| frames presented | 92 | 117 |
| no frames | 36 | 43 |
| crash at a KUSEG address (mostly NULL + an offset) | 32 | 38 |
| fatal: `eret` interpreted | 15 | 20 |
| fatal: no function at an address | - | 11 |

Presenting frames in s5 among the popular: Super Smash Bros. (2291 frames:
the intro, the title, the character select), International Superstar
Soccer 64, Mystical Ninja, Pokémon Snap, Kirby 64, Diddy Kong Racing
(grey boxes: its microcode), Mario Kart 64. Stopping early: Ocarina of Time
and F-Zero X (2-3 frames), Mario Party (none: its older libultra's SI
functions were not named; `__osSiRawStartDma` with the status check inline
and controller reads with no SI access queue are named now). Mario Party
draws since (2026-10-17): the analyser finds a game's own setjmp/longjmp
by shape and the runtime runs the contexts they switch as coroutines
(`strand.c`, `docs/decisions.md`).

### The library again: s7 (2026-10-17, xemu, commit `fbe2fa1`)

Built in a git worktree pinned to the commit (s6 was built from a working
tree that changed under it and is not comparable). 245 of 246 build; 139
present frames (s5: 130), 121 still presenting when the 30 s run ends.
Against s5: 12 gained (Donkey Kong 64, Rat Attack, Paperboy, Donald Duck,
Mario Party among them), 3 lost (Offroad Challenge, Robotron 64, Mario no
Photopie: open).

| in xemu (s7) | games |
|---|---|
| frames | 121 |
| no frames | 42 |
| crash: read/write KUSEG (TLB-mapped): mostly a NULL pointer | 42 |
| fatal: the boot code came back (Rare's compressed boots: the learning rounds take them further) | 18 |
| fatal: no function at an address | 9 |
| crash: KSEG0/KSEG1/PIF and the rest | 13 |

Popular titles in s7: Ocarina of Time 627 frames (its title demo), Super
Smash Bros. 1501, Super Mario 64 927, Mario Kart 64 913, Mario Party 1853,
Pokémon Snap 1576, Diddy Kong Racing 857, WWF No Mercy 849, F-Zero X 42,
Pokémon Stadium 20, Kirby 64 9. After s7 (not in it): the overlay
precheck (Mario Party 3 builds), XRC_HIGH_TLB, the controller query names
(Ocarina of Time's title sees the controller).

**s8 and after (2026-10-17).** s8: 128 still presenting at 30 s (s7: 121);
KUSEG NULL reads 30 (42), the boot code returning 19 (the dispatch start
takes Blast Corps on), no frames in the first round 52. Since: the VI
framebuffers at K0BASE before the first swap (Airboarders 64 presents); a
false `osContGetReadData` corrected: the rule matched Mario Party's own
ring of recorded pads (4 shorts a call, within 0x100 of
`osContStartReadData`'s command byte) and the runtime replaced it, while
the game's real one read a PIF buffer nothing filled (no buttons, on red).
The SI buffer now also comes from the pack routine `osContStartReadData`
calls, a halfword written at arg0 + 2 rules a candidate out (an OSContPad
has bytes there), and the reader of that buffer wins a tie: Mario Party
1-3 name the one beside `osContStartReadData`; Mario Kart 64, Super Mario
64, Smash Bros. and Pilotwings 64 name what they did. GoldenEye: TLB-paged
code found verbatim in the ROM becomes one ROM-backed segment, and
`patches/recomp/0001` with RDRAM table sections takes its switch tables
(134 -> 7 functions the recompiler refuses).

**s9 (2026-10-18, at `6a00bd1`).** 132 still presenting at 30 s (s8: 128).
Gained: Paper Mario, Glover, Wetrix, Paperboy, Rat Attack, Powerpuff Girls,
Asteroids Hyper 64, Blues Brothers 2000 and four that present a frame or
three. Lost: Bomberman Hero and PGA European Tour (both run clean at
`e62de64` in a rerun: the sweep's host was loaded), Snowboard Kids 2 (a
boot hang at retrace 0, host load; at HEAD it presents, then reads a
garbage pointer later than s8's 30 s), Rampage 2 (timing-dependent: 481
frames by 10 s in one run, 9 in 40 s in another), Aidyn Chronicles,
CyberTiger, Wipeout 64, NTWJ (not rerun). The KUSEG NULL reads are still
the biggest class; three looked at: Chopper Attack's task switcher
(stack-switching coroutines), Jet Force Gemini's call-site resolver (fixed
since: `ra_hooks`), Body Harvest and Pokémon Puzzle League (no cause yet).

**s10 (2026-10-19, at `bd9ac1c`).** 144 still presenting at 30 s (s9:
132); 152 present at least one frame (s9: 143). Gained: Mario Golf,
Bomberman 64, Bomberman Hero, Goemon's Great Adventure, Hexen, Forsaken,
Jet Force Gemini, Robopon 64, Top Gear Overdrive, Snowboard Kids 2,
Rampage 2, Aidyn Chronicles, CyberTiger, PGA European Tour, NTWJ. Lost:
Jeopardy! (a KUSEG read), Fighter's Destiny (a KUSEG write), Vigilante 8
(an integer divide by zero after presenting); not yet looked at. The
KUSEG reads are still the biggest class (24), then the games that never
present (55). NF2P, NF9E and NFBE shared their xemu slot with another
test during this sweep; their rows are to be run again.

**s11 and s12 (2026-10-19/20, overnight; `tools/xbox/sweep_diff.py`).**
s11 (first boots only, the tree of 2026-10-19 ~23:40): 147 present at
30 s. s12 (the overnight tree, `--learn 2`: 61 games that learnt
something built again with their logs, 37 a second time): **163 of
246**, none lost against s10. New since s10: Air Boarder 64, Beetle
Adventure Racing, Buck Bumble, Donald Duck Goin' Quackers, Duck Dodgers,
F-1 World Grand Prix 1 and 2, Killer Instinct Gold (one frame), Pokémon
Stadium 2, Rayman 2, Rugrats in Paris, Spider-Man, Starshot, Tony Hawk's
Pro Skater 1 to 3, Vigilante 8, Virtual Pool 64, World Driver
Championship. What got them there: a zero divisor's VR4300 result
(patches/recomp/0003), code no walk reached found from the calls aimed
at it (`_probe`), a new thread's gp, page 0 read as zeros, the learning
loop's rounds. Outside the sweep (its runs use the default switches):
Lylat Wars needs Star Fox 64's thread switches; Shadow Man boots to a
TLB miss; Diddy Kong Racing renders whole (Rare's DKR microcode).

**2026-10-18.** Two rules from games that failed: a far `j` inside a big
function is decided after the walk (`N64RX_J_LATER`; s9's six "undeclared
label" compile failures, Mario Golf's 19 KB function), and a libultra
built without IDO's divide has its osSendMesg/osRecvMesg named
(`N64RX_MESG_NODIV`; Mario Tennis: past its crash, to a stall at pc
80031254, `docs/handoff.md`).

**2026-10-22.** A far `j` left for after the walk whose target the walk
then reached by another branch was still a tail call: `discover` started a
function there and split the one it sits in (review 4.6). Now only an
unreached target is a tail (`N64RX_J_SEEN`). Paper Mario's check: 15
extra functions, was 17, none lost; nine other games' counts unchanged.
The register check (`emit.hw_funcs`) now reads an overlay's function with
the focus on its segment (review 4.3; `N64RX_HW_FOCUS`), and the memory
probe rule wants a MB boundary (`N64RX_PROBE_MB`): with the first alone,
Smash Bros.' overlay display-list builders (`lui 0xA078`) were taken for
probes. Both: Pokémon Snap interprets 4 functions, was 17; Paper Mario 37,
was 38; Smash, Ocarina, Majora, F-Zero X, Bassmasters, Mario Tennis and
GoldenEye unchanged.

**s15 and s16 (2026-10-22/23; the dry boot and the shipped learnt facts,
120 s boots on six xemus at once, `sweep.py --slots`).** s15's runs 78-242
had all read one game's stale serial.log (fixed: 0da4c65); run again it is
**183 of 242**. s16 (the dry boot no longer learning `send_mesg` from
`__osException`, the TMEM wrap, learnt DMAs from halfword offsets copied,
a stale "entry interpreted" mark dropped): **all 246 build, 192 present**.
Gained against s15: Super Smash Bros., Blast Corps, Diddy Kong Racing,
Kirby 64, Pokémon Snap, All-Star Baseball 2001, Duck Dodgers, F1 World
Grand Prix II, Indy Racing 2000, Jeopardy!, Brunswick Bowling, San
Francisco Rush 2049, Spider-Man, Tony Hawk's 2 and 3 (most of them the
`send_mesg` fault, which stopped them before their first graphics task).
The "losses" against s15 were s15's shorter runs ending before a later
crash (Dual Heroes at retrace ~540 in both; Turok, NHL 99, WWF No Mercy,
Bio F.R.E.A.K.S. deeper than before), or a loading-screen stall run again.
Left: 20 present nothing, 23 crash on a KUSEG access near NULL, 4 fatal.

## Known gaps

- **One bad walk fails a whole overlay** (`segments.find`: `not any(f.bad ...)`, even for a load a
  ROM table describes with its text range). WWF WrestleMania 2000 loses its match overlay this way
  (ROM 0xD2720: several unresolved `jr`s, data walked as code) and would run the menu overlay's code
  on its data; WWF No Mercy did until its one `jr` resolved (`N64RX_TABLE_SPILL`).
- **The debug libultra** (libultra_d: each call checks its arguments and
  calls `__osError(code, n, ...)`; -O0 code, arguments spilled to the
  stack). Mischief Makers ships it: the rules, written for the optimised
  build (leaf functions, sizes), miss its osCreateMesgQueue,
  osSetEventMesg and osContInit (its main thread waits for good on the SI
  event its own osSetEventMesg set up), and its `__osError` (0x20 bytes
  at 0x800A5AC0, a jump through a handler pointer) was named
  `__osEnqueueAndYield` with the exception assembly after it counted as
  its body. One of the 28 games of s12 that run, learn nothing and never
  present; the others not looked at yet. Next: find `__osError` by its
  shape and read past its calls (and the -O0 sizes) in the rules.
- The 25 other games of that group name their libultra. One looked at:
  Star Wars Episode I Racer (8 MB, from `_wants_8mb`) clears its three
  640x480 framebuffers above 4 MB, sets its video mode, then its main
  thread wakes on every retrace and never makes a graphics task, with
  only two of its threads started (open).
- Code loaded with computed addresses (overlay tables, compressed code) is
  only found when the game runs: the safety net runs it and the learning
  loop recompiles it next build (above). Two overlays at one address are
  still one table entry each (M5): code captured from RDRAM is checked
  against what it was recompiled from (`guard.c`) and runs interpreted
  when another overlay is loaded there; recompiling each variant is next.
- The net's interpreter is slow (each instruction decoded every time); a
  hot function it runs costs frame time until the rebuild.
- Loads described by plain words in the ROM are found statically (Super
  Mario 64's goddard). Yaz0 games (the Zeldas) are inflated before the
  analysis (`n64rx_lib/yaz0.py`: the DMA table found by behaviour, every
  file at its vrom, the table rewritten to "not compressed"; Ocarina of
  Time 1.2: 1456 files, 32 -> 53 MB; Majora's Mask: 1513, 32 -> 47 MB);
  the console checks the inflated image's SHA-1. Other compressed code
  (Dr. Mario 64's gzip main segment, Rare's) is captured from RDRAM as it
  runs (the learning loop, step 4).
- Overlays that share a vram are kept apart by key and switched on their
  DMA (below); a jal from one into an address no section has a function
  at is interpreted up front (the precheck, by key since 2026-10-17:
  Mario Party 3).
- libultra built by GCC (Paper Mario) and the later 2.0I/2.0J layouts: many
  rules are IDO-shaped. Paper Mario now gets 70 names right and none wrong,
  but `osSpTaskLoad`, the `osFlash*` family (no rule yet: the runtime has
  them) and the Pfs family stay the game's own. A rule that depends on
  another name must come after it (osContInit before the read/query split).
- A libultra function the runtime replaces in part must be named in full:
  the runtime's `osSetTimer` keeps its own timer list, so a game's own
  `osStopTimer` unlinked a timer that list never held and wrote through
  stale pointers (Ocarina of Time: its graph thread's RCP-timeout timer on
  the stack; the store landed in the game-state table and the next state's
  init became 0xFFF7B4E8). Rule since 2026-10-17: arg0 only, writes +0 and
  +4 of it, calls `__osDisableInt`, `__osRestoreInt` and `__osSetCompare`.
- The TLB: mapped KUSEG pages are aliased in the Xbox page tables
  (`xhw_alias.c`, `rt/tlb.c`); a miss runs the game's exception code and
  fault thread; KSEG2/3 goes through the TLB in the generated code when
  learnt (`XRC_HIGH_TLB`). The ASID is not handled.
- Microcode families: graphics Fast3D, F3DEX, F3DEX2 (S2DEX's own commands
  skipped; `docs/renderer.md`). Audio: `aspMain`, the nead family (Mario
  Kart 64, the Zelda microcode) and `n_aspMain` are recognised at run time
  from the microcode's jump table (`docs/architecture.md`); others (MusyX,
  MP3, DK64/Conker's variants) play silent with an `[AUDIO]` line saying so.
- Saves: EEPROM, the Controller Pak, SRAM and FlashRAM are built (SRAM and
  Flash host-tested only).

## Overlays that share vram (Pokemon Snap)

A game may load several segments at one address from different ROM ranges
(Snap: every level's code at 0x802Bxxxx-0x802Exxxx, the scenes at
0x800E18A0, 0x801DC8C0 ...). The analyser keeps them apart:

- `segments.py`: a load whose vram lies in a known segment counts when its
  ROM range is new (never over the boot segment); a call is "into the
  unknown" when no known segment has a function at its target. A load
  descriptor that names its text, `(start, end, RAM, text start, text end,
  ...)` (HAL's overlay tables), is a segment even when no known code calls
  it: its text starts with a function and the gaps run to the text's end.
  Call targets inside such a range may be another overlay's: the ones that
  do not look like starts are dropped (score 0.75 for load tables), and a
  root a function's own branches run past is not a start (`discover`'s
  `weak` roots). Kill switch `N64RX_OVERLAYS=0`.
- `Image.focus`: while a segment is walked its own bytes are read where
  overlays overlap.
- Keys and names: a function in a segment that overlaps another is keyed
  `vram | ROM offset << 32` (`Analysis.key`, `by_vram`, `seg_of`) and named
  `func_<vram>_<ROM offset>`, so N64Recomp sees each overlay as its own
  section with unique names; the pipeline's refusal handling works per name
  and so per (section, function). N64Recomp itself already resolves a `jal`
  to an address several sections hold as a lookup through `get_function`
  (no change to it). Liveness follows a call into the caller's own overlay
  when it has a function there, else into every overlay at that address;
  data pointers are looked for outside each segment's own bodies.
- The runtime switches the function table on the PI DMA that loads an
  overlay (`docs/architecture.md`).

| ROM | functions vs key | segments | libultra right / wrong | runs |
|---|---|---|---|---|
| Pokemon Snap (US) | 702 / 835 (the key names few; 133 missed are mostly data symbols typed as functions) | all 14 code segments the key names, 32 in all (every level and scene) | 82 / 0 (osCartRomInit, osEPiStartDma 2.0L, __osEPiRaw{Read,Write}Io, osEPi{Read,Write}Io, osStopThread rules added) | xemu: intro scene runs (F3DEX2, aspMain, FlashRAM), switches to the main menu overlay, then faults in omCreateProcess (NULL GObj) |
