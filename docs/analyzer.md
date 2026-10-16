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
- **The PIF** (`ultra/si.c`): the game's own SI code (a Controller Pak
  driver, a game's own pad reader) gets real answers from
  `__osSiRawStartDma`: pad status and buttons, the EEPROM, no pak.
- **The whole reimplemented list** (`ultra/extra.c`): the `osEPi*` handle
  DMA family, the later libultras' init and debugger probes, the Rumble Pak
  on the pad's motors, no Controller Pak / Flash / VRU yet.

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
| Super Mario 64 (US, -O0) | main 3516 of 3573, engine 283 of 284, goddard 693 of 699 (by count; few addresses to pin) | boot + engine (DMA) + goddard (a level script's load) | 66 / 0 | title logo drawn, sound; stops when goddard maps 0x04000000 with `osMapTLB` (the runtime has no TLB) |
| Dr. Mario 64 (US) | boot 262 / 270 | boot; the main segment is gzip-compressed (not found) | 56 / 0; live set identical | not tried |
| Paper Mario (US, GCC) | 2443 / 2680 in the segments found | main, engine1, engine2, evt, engine4, one partner and one map (overlays sharing a vram: one each) | 56 / 0 (87 missing: GCC-built libultra, Flash, the Pfs family) | not tried |

## Known gaps

- Code loaded with computed addresses (overlay tables read at run time,
  compressed code) is only found when the game runs: the learning loop
  (§3.14 step 5) is next. Loads described by plain words in the ROM are
  found (Super Mario 64's goddard). Compressed code (Dr. Mario 64's gzip
  main segment) is not.
- Overlays that share a vram (Paper Mario's maps, battles, partners): the
  first one found takes the range; the rest are not tried yet.
- libultra built by GCC (Paper Mario) and the later 2.0I/2.0J layouts: many
  rules are IDO-shaped; Paper Mario misses `osContInit`, `osSetEventMesg`,
  `osEPiStartDma`, `osSpTaskLoad`, the Flash and Pfs families.
- The TLB: Super Mario 64's goddard maps its dynlists at 0x04000000; the
  runtime ignores `osMapTLB` (`ultra/more.c`) and the read faults.
- Microcode families: Fast3D only (F3DEX/F3DEX2/S2DEX in M4); audio
  microcodes other than `aspMain` play silent.
- Controller Pak, Flash, SRAM: not yet.
