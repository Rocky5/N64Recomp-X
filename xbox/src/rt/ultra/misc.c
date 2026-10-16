/* misc.c - the rest of the libultra surface (PLAN.md 3.3): initialisation,
 * the caches, address conversion, the FPU control word, the O32 64-bit
 * helpers. */
#include "ultra.h"

/* osInitialize: the boot block is written by boot.c before the game runs
 * (what the IPL3 left there); nothing else to do */
ULTRA(osInitialize) {
    (void)rdram;
    (void)ctx;
    xhw_log("[OS] osInitialize");
}

/* Cart DMA is a CPU copy and the interpreter reads RDRAM itself: no N64
 * cache to keep coherent */
ULTRA(osWritebackDCache) {
    (void)rdram;
    (void)ctx;
}
ULTRA(osWritebackDCacheAll) {
    (void)rdram;
    (void)ctx;
}
ULTRA(osInvalDCache) {
    (void)rdram;
    (void)ctx;
}
ULTRA(osInvalICache) {
    (void)rdram;
    xrc_learn_inval(ARG(0), ARG(1));   /* learn.c: code there may be new */
}

ULTRA(osVirtualToPhysical) {
    (void)rdram;
    RET(ARG(0) & 0x1FFFFFFFu);
}

ULTRA(osPhysicalToVirtual) {
    (void)rdram;
    RET((ARG(0) & 0x1FFFFFFFu) | 0x80000000u);
}

ULTRA(osGetMemSize) {
    (void)rdram;
    RET(xrc_rdram_size);
}

/* __osSetFpcCsr(value): the FPU control word, the old one back; only the
 * rounding mode means anything here (MXCSR and the x87 together) */
static uint32_t s_fcsr = 0x01000800u;
ULTRA(__osSetFpcCsr) {
    uint32_t old = s_fcsr;
    (void)rdram;
    s_fcsr = ARG(0);
    set_cop1_cs(s_fcsr & 3);
    RET(old);
}
ULTRA(__osGetFpcCsr) {
    (void)rdram;
    RET(s_fcsr);
}

/* ---- the O32 64-bit helpers libultra carries (ll.c): arguments in
 * a0/a1 and a2/a3 (high word first), the result in v0/v1 ---- */
#define A64 xu_arg64(ctx, 0)
#define B64 xu_arg64(ctx, 2)

ULTRA(__ll_div) {
    int64_t a = (int64_t)A64, b = (int64_t)B64;
    (void)rdram;
    RET64(b ? (uint64_t)(a / b) : 0);
}
/* libultra's __ll_rem(unsigned long long, long long): C's conversions make
 * it an unsigned remainder (its code is a ddivu, the same as __ull_rem);
 * the signed one with the sign fix-up is __ll_mod */
ULTRA(__ll_rem) {
    uint64_t a = A64, b = B64;
    (void)rdram;
    RET64(b ? a % b : 0);
}
ULTRA(__ll_mod) {
    int64_t a = (int64_t)A64, b = (int64_t)B64, r;
    (void)rdram;
    r = b ? a % b : 0;
    if (r && ((r < 0) != (b < 0))) r += b;
    RET64((uint64_t)r);
}
ULTRA(__ll_mul) {
    (void)rdram;
    RET64(A64 * B64);
}
ULTRA(__ll_lshift) {
    (void)rdram;
    RET64(A64 << (B64 & 63));
}
ULTRA(__ll_rshift) {
    (void)rdram;
    RET64((uint64_t)((int64_t)A64 >> (B64 & 63)));
}
ULTRA(__ull_div) {
    uint64_t a = A64, b = B64;
    (void)rdram;
    RET64(b ? a / b : 0);
}
ULTRA(__ull_rem) {
    uint64_t a = A64, b = B64;
    (void)rdram;
    RET64(b ? a % b : 0);
}
ULTRA(__ull_rshift) {
    (void)rdram;
    RET64(A64 >> (B64 & 63));
}
