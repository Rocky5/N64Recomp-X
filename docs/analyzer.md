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
| boot segment | `funcs.py` `parse_entry`, `__init__.py` | IPL3 copies 1 MB from ROM 0x1000 to the header's entrypoint (the 6103 and 6106 boot chips 1 or 2 MB below it: the entry code's own BSS and main addresses say which); the SDK's entry code clears the BSS (`lui/addiu t0`, `lui/ori t1`, a `sw zero` loop) and jumps to main (`jr t2` or `j`). The BSS start is where the loaded segment ends. |
| functions | `funcs.py` `discover` | recursive descent from the entry and main over every `jal`; each function walked through its branches to its last reachable instruction; `j` onto a known start is a tail call; jump tables read from `lui`/`lw`/`jr` (the `lui` may sit in a delay slot) bounded by the `sltiu` before them, or, when GCC hoisted the base out of a loop, from the last `lw`/`addu`/`lui`+`addiu` writes before the `jr` (every entry must land in the function); then the gaps between functions, and the code before a segment's first function, tried as functions (clean walks only), again from what those call, until nothing new. Dead epilogues after endless loops (`lw ra` / `addiu sp` first, no branch reaches them) fold into the function before; so does unoptimised IDO's closing `jr ra; nop` after the return statement's own (only in a segment whose `b .+8; nop` count says it is -O0 code, and never when a call, a data word or a `lui`/`addiu` pair names the address). |
| segments | `segments.py` | every call whose arguments, by constant propagation (`lui`/`addiu`/`ori`, `addu`/`subu`, `and`/`or`, shifts, the stack slots, locals kept there by -O0 code read back), hold a ROM offset, a KSEG0 address and a size (or a ROM start and end: `dma_copy(start, end, dest)`, `dma_read(dest, start, end)`) is a candidate DMA of code; so is every (RAM, ROM start, ROM end) or (start, end, RAM) word triple anywhere in the ROM (a script's load command, an overlay table). A candidate becomes a segment when the known code calls into it, the call targets look like function starts (a frame opens, or the words before close one), and every walk from them is clean. A start and end next to each other win, then a size in its natural place (the argument after the two addresses). |
| features | `features.py` | per function, a straight-line value tracker: calls, COP0 and FPU control registers, cache ops, TLB ops, hardware registers (VI, AI, PI, SI, SP, DP, MI by address), loads and stores at argument + offset, through pointers read from globals (and what was stored), 64-bit ops, `ddiv`/`dmult` kinds. Delay slots run before the call clobbers. |
| libultra | `libultra.py` | names by behaviour, no fingerprints: COP0/cache anchors (`__osDisableInt`, `osGetCount`, `osWritebackDCache`...), hardware anchors (`osAiSetFrequency`, `osPiRawStartDma`, `__osSiRawStartDma`, `__osViSwapContext`...), then the call graph and libultra's struct offsets (OSMesgQueue, OSThread, OSIoMesg, OSTimer, the VI context behind `__osViNext`), IDO's 64-bit helpers by their one `ddiv`/`dmult`, the TLB writers by their arguments (`osMapTLB`, `osUnmapTLB`, `osUnmapTLBAll`, `osMapTLBRdb`). Rules run to a fixed point; a name is given only when exactly one candidate fits (the hardware rules try every function first). |
| liveness | `emit.py` `reachable` | the game's code reached from the entry, main, function addresses in data (in the segments and anywhere else in the ROM: scripts, display-list callbacks) and in code, with the edges out of the functions the runtime replaces cut. Dead code (libultra's internals only replaced functions reached) is not recompiled. |
| output | `emit.py`, `../romonly.py` | the symbol file, `recomp.toml` (ignored = dead + interpreted; a real pack's patches mapped by address), `interp.txt`, the report; without a pack the title is the header's, the title ID `0x4E52` + a hash of the game code, the save type from a facts table or 4 Kbit when the game probes the EEPROM. |

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
  `__osSiRawStartDma`: pad status and buttons, the EEPROM, no pak.
- **The whole reimplemented list** (`ultra/extra.c`): the `osEPi*` handle
  DMA family, the later libultras' init and debugger probes, the Rumble Pak
  on the pad's motors, no Controller Pak / Flash / VRU yet.

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
| Mario Kart 64 (US) | 3157 / 3159 placed; main 2657 of 2676, racing 413 of 415, ending 133 of 133 | boot + both race segments | 66 / 0 | title screen drawn; races need F3DEX |
| Super Mario 64 (US, -O0) | main 3516 of 3573, engine 283 of 284, goddard 693 of 699 (by count; few addresses to pin) | boot + engine (DMA) + goddard (a level script's load) | 66 / 0 | title head, the interactive head screen, castle grounds (Lakitu intro) after three `--learn` rounds and the TLB; the head's shading is off |
| Dr. Mario 64 (US) | boot 262 / 270 | boot; the main segment is gzip-compressed (not found) | 56 / 0; live set identical | links; boots, the net runs 4 functions, then interpreted code reads PI_STATUS through KSEG1 (`a4600010`) and faults; its gzip main segment is not found |
| Paper Mario (US, GCC) | 2443 / 2680 in the segments found | main, engine1, engine2, evt, engine4, one partner and one map (overlays sharing a vram: one each) | 56 / 0 (87 missing: GCC-built libultra, Flash, the Pfs family) | not tried |

## Known gaps

- Code loaded with computed addresses (overlay tables, compressed code) is
  only found when the game runs: the safety net runs it and the learning
  loop recompiles it next build (above). Two overlays at one address are
  still one table entry each (M5): the net's stub is fine (it interprets
  what is there), a learnt segment that overlaps another is not yet.
- The net's interpreter is slow (each instruction decoded every time); a
  hot function it runs costs frame time until the rebuild.
- Loads described by plain words in the ROM are found statically (Super
  Mario 64's goddard). Compressed code (Dr. Mario 64's gzip main segment)
  is not.
- Overlays that share a vram (Paper Mario's maps, battles, partners): the
  first one found takes the range; the rest are not tried yet.
- libultra built by GCC (Paper Mario) and the later 2.0I/2.0J layouts: many
  rules are IDO-shaped; Paper Mario misses `osContInit`, `osSetEventMesg`,
  `osEPiStartDma`, `osSpTaskLoad`, the Flash and Pfs families.
- The TLB: mapped KUSEG pages are aliased in the Xbox page tables
  (`xhw_alias.c`, `rt/tlb.c`; Super Mario 64's goddard dynlists); code
  linked at TLB addresses, KSEG2/3 and the ASID are not handled.
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
