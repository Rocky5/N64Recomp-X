# 0003: DIV and DIVU through the runtime's macros

**Files:** `src/cgenerator.cpp` (`CGenerator::emit_muldiv`, the `cpu_div`
and `cpu_divu` cases), against the pin `ffb39cd` with 0001 and 0002
applied.

**What it changes.** `div rs, rt` was emitted as
`lo = S32(S64(S32(rs)) / S64(S32(rt))); hi = S32(S64(S32(rs)) % S64(S32(rt)));`
and `divu` as the same in `U32`. Both are now one call to a macro the
runtime's `recomp.h` defines: `DIV32(rs, rt, lo, hi);`,
`DIVU32(rs, rt, lo, hi);`. Nothing else in the output changes.

**Why.** The VR4300 never traps on a zero divisor: DIV gives lo = -1 (1
for a negative dividend) and hi = the dividend, DIVU lo = all ones and
hi = the dividend. Code built without zero-division checks relies on it;
the host's integer divide raises #DE instead. The library sweep (s10) had
Vigilante 8 and World Driver Championship crash that way
(`c0000094`, frames 1592 and 1351). The runtime's macros
(`xbox/include/xrc/recomp.h`) give the VR4300's results and divide in 32
bits (the 64-bit division N64Recomp used against 0x80000000 / -1 is a
libgcc call on the Pentium III; that case is handled explicitly).
`-DXRC_DIV_VR4300=0` defines the macros as the old expressions. The
interpreter (`interp.c` `div32` / `divu32`) already did this; DDIV and
DDIVU (the runtime's inline functions) got the same zero cases.

**Upstream.** N64ModernRuntime's own `recomp.h` is GPL and was not read for
this; the behaviour is the VR4300 manual's.
