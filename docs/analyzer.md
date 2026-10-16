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
| boot segment | `funcs.py` `parse_entry` | IPL3 copies 1 MB from ROM 0x1000 to the header's entrypoint; the SDK's entry code clears the BSS (`lui/addiu t0`, `lui/ori t1`, a `sw zero` loop) and jumps to main (`jr t2` or `j`). The BSS start is where the loaded segment ends. |
| functions | `funcs.py` `discover` | recursive descent from the entry and main over every `jal`; each function walked through its branches to its last reachable instruction; `j` onto a known start is a tail call; jump tables read from `lui`/`lw`/`jr` (the `lui` may sit in a delay slot) bounded by the `sltiu` before them; then the gaps between functions tried as functions (clean walks only), again from what those call, until nothing new. Dead epilogues after endless loops (`lw ra` / `addiu sp` first, no branch reaches them) fold into the function before. |
| segments | `segments.py` | every call whose arguments, by constant propagation (`lui`/`addiu`/`ori`, `addu`/`subu`, `and`/`or`, shifts, the outgoing stack slots), hold a ROM offset, a KSEG0 address and a size is a candidate DMA of code. A candidate becomes a segment when the known code calls into it, the call targets look like function starts (a frame opens, or the words before close one), and every walk from them is clean. The size in its natural place (the argument after the two addresses) wins. |
| features | `features.py` | per function, a straight-line value tracker: calls, COP0 and FPU control registers, cache ops, TLB ops, hardware registers (VI, AI, PI, SI, SP, DP, MI by address), loads and stores at argument + offset, through pointers read from globals (and what was stored), 64-bit ops, `ddiv`/`dmult` kinds. Delay slots run before the call clobbers. |
| libultra | `libultra.py` | names by behaviour, no fingerprints: COP0/cache anchors (`__osDisableInt`, `osGetCount`, `osWritebackDCache`...), hardware anchors (`osAiSetFrequency`, `osPiRawStartDma`, `__osSiRawStartDma`, `__osViSwapContext`...), then the call graph and libultra's struct offsets (OSMesgQueue, OSThread, OSIoMesg, OSTimer, the VI context behind `__osViNext`), IDO's 64-bit helpers by their one `ddiv`/`dmult`. Rules run to a fixed point; a name is given only when exactly one candidate fits. |
| liveness | `emit.py` `reachable` | the game's code reached from the entry, main, function addresses in data and in code, with the edges out of the functions the runtime replaces cut. Dead code (libultra's internals only replaced functions reached) is not recompiled. |
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

Interpreted code reaches the runtime's libultra by address:
`gen/xrc_interp.c` carries `xrc_ultra_table` (each `ULTRA(name)` of
`xbox/src/rt` at the address the symbol file gives that name), entered in
the function table before the recompiled sections.

## How well it does (checked against decomps)

| game | functions found / the decomp's | segments | replaced libultra named / wrong | runs (xemu) |
|---|---|---|---|---|
| Pilotwings 64 (US) | 1952 / 1956 (the 4: libultra's exception handler) | boot + app, exact | 54 of 54 / 0 | title, menus, flight, results, saves, sound: the same as the decomp-symbol build |
| Mario Kart 64 (US) | 3203 (not yet compared) | boot + both race segments | 71 named | title screen drawn; races need F3DEX |

The validation track (PLAN.md §6, "V") adds a game at a time from the
decomps and keeps every result here.

## Known gaps

- Code loaded with computed addresses (overlay tables, compressed code) is
  only found when the game runs: the safety net runs it and the learning
  loop recompiles it next build (above). Two overlays at one address are
  still one table entry each (M5): the net's stub is fine (it interprets
  what is there), a learnt segment that overlaps another is not yet.
- The net's interpreter is slow (each instruction decoded every time); a
  hot function it runs costs frame time until the rebuild.
- Microcode families: Fast3D only (F3DEX/F3DEX2/S2DEX in M4); audio
  microcodes other than `aspMain` play silent.
- Controller Pak, Flash, SRAM: not yet.
