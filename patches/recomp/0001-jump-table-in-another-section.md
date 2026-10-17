# 0001: a jump table in another section

**File:** `src/analysis.cpp` (`analyze_function`, the jump-table sizing loop),
against the pin `ffb39cd`.

**What it changes.** N64Recomp reads a jump table's entries at the
function's own ROM offset (`vram + func.rom - func.vram`; its own comment:
"TODO this assumes that the jump table is in the same section as the
function itself"). When the table's vram lies outside the function's
section, the patch reads it from the first section whose vram range holds
it (that section's `rom_addr - ram_addr`) and records that section on the
table. A table whose ROM offset runs past the ROM ends there instead of
reading past the buffer. Tables in the function's own section are read as
before.

**Why.** Games that map their code through the TLB keep its rodata in
KSEG0: GoldenEye's code at 0x70000000 (the same physical memory as its
boot segment at 0x80000400) reads its switch tables at 0x8002xxxx; Conker's
does the same. Unpatched, N64Recomp read the wrong bytes (or past the ROM),
so n64rx's analyser kept 124 of GoldenEye's functions from it, its
inflater among them, and the runtime interpreted them (a level load ran at
interpreter speed). The analyser now lets such a function through when the
table's first entry, read where the patched recompiler reads it, lies in
the function (`analyze/emit.py`, `_table_elsewhere`).

**How it is applied.** `tools/xbox/recomp_src.py` copies the submodule to
`build-host/recomp-src` and applies `patches/recomp/*.patch` in name order;
n64rx builds the recompiler from there and keys its caches on the pin plus
the patches (`engines.recompiler_pin`). The SDK image's prebuilt recompiler
predates the patch: inside it n64rx builds its own in `build-host/recomp`.

**Upstream.** Not sent. Drop the patch when the pin carries an equivalent.
