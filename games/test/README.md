# test: the ROM-free end-to-end test

Not a game. `tools/xbox/make_test_rom.py` assembles a small self-checking
MIPS program into a 1 MB image (SHA-1 `a182648693f416078e0bff023243d552ad46fe16`,
the same on every machine); `n64rx build` takes it through the recompiler,
our `recomp.h`, the runtime and the XBE pass like any game. On the console
(or xemu) the loading screen runs through "Preparing memory", "Loading code"
and "Starting N64Recomp-X test", then a card shows the checks' result:
`Test ROM: passed` with 34 checks and none failed.

```sh
python tools/xbox/make_test_rom.py build-host/test/test.z64
n64rx build build-host/test/test.z64
```

What it covers: a loop with a delay slot, direct calls, a call through a
function pointer (`LOOKUP_FUNC`), an IDO jump table, mult/div, shifts and
compares, byte, halfword and unaligned (`lwl`/`lwr`) memory over the
word-swapped image, single and double floats under all four rounding modes'
paths (MXCSR and the x87 control word). It uses no libultra, so it says
nothing about the OS layer.
