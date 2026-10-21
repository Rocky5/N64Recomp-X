# 0002: jal targets called through the lookup

**Files:** `include/recompiler/context.h` (`Context::lookup_jal_targets`),
`src/config.h` / `src/config.cpp` (`[patches] lookup_jal`), `src/main.cpp`
(the config into the context), `src/recompilation.cpp` (the jal), against
the pin `ffb39cd` with 0001 applied.

**What it changes.** A new optional config array, `[patches] lookup_jal =
[0x800F4054, ...]`: a `jal` to one of those addresses is emitted as
`LOOKUP_FUNC(0x800F4054)(rdram, ctx)`, the call N64Recomp already makes for
an ambiguous target, instead of a direct call or a new static function.
Every other jal is resolved as before; an empty or missing array changes
nothing.

**Why.** Two kinds of call n64rx's analyser could only handle by sending the
whole caller to the runtime's MIPS interpreter (`analyze/emit.py`,
`recomp_hazards`):

- a jal into captured code where this version of the code has no function
  start, but another overlay loaded at the same address has one (Mario
  Tennis's scenes at 0x800E0000-0x80110000). N64Recomp made a static
  function there from this version's bytes and split the function holding
  them; the runtime runs whichever overlay is loaded;
- a jal to an address no segment holds yet ("No function found for jal
  target": Banjo-Kazooie's captured code, ~150 callers). N64Recomp refuses
  the function.

Interpreted, Mario Tennis's match ran ~1.8 million MIPS instructions every
5 s in the safety net (xemu). Through the lookup the caller is compiled and
only the call goes through `get_function` (`ovl.c`): the overlay loaded
there, or the safety net, which runs the code and logs it for the next
learning round.

**Kill switch.** `N64RX_LOOKUP_JAL=0` (n64rx): the analyser writes no
targets and interprets those callers, as before.

**Toolchain bundle.** The app's prebuilt recompiler
(`tools/xbox/pack_toolchain.py`) must be built with this patch: a
recompiler without it ignores the array and compiles direct calls.

**Upstream.** Not sent. Drop the patch when the pin carries an equivalent.
