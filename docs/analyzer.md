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
- **The PIF** (`ultra/si.c`): the game's own SI code (a Controller Pak
  driver, a game's own pad reader) gets real answers from
  `__osSiRawStartDma`: pad status and buttons, the EEPROM, no pak.
- **The whole reimplemented list** (`ultra/extra.c`): the `osEPi*` handle
  DMA family, the later libultras' init and debugger probes, the Rumble Pak
  on the pad's motors, no Controller Pak / Flash / VRU yet.

## How well it does (checked against decomps)

| game | functions found / the decomp's | segments | replaced libultra named / wrong | runs (xemu) |
|---|---|---|---|---|
| Pilotwings 64 (US) | 1952 / 1956 (the 4: libultra's exception handler) | boot + app, exact | 54 of 54 / 0 | title, menus, flight, results, saves, sound: the same as the decomp-symbol build |
| Mario Kart 64 (US) | 3203 (not yet compared) | boot + both race segments | 71 named | title screen drawn; races need F3DEX |

The validation track (PLAN.md §6, "V") adds a game at a time from the
decomps and keeps every result here.

## Known gaps

- Code loaded with computed addresses (overlay tables, compressed code) is
  only found when the game runs: the learning loop (§3.14 step 5) is next.
- Microcode families: Fast3D only (F3DEX/F3DEX2/S2DEX in M4); audio
  microcodes other than `aspMain` play silent.
- Controller Pak, Flash, SRAM: not yet.
