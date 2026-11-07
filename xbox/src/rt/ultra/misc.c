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
    xrc_gfx_inval(ARG(0), ARG(1));   /* a frame there the CPU will read: from the GPU first (../gfx.c) */
}
ULTRA(osInvalICache) {
    (void)rdram;
    xrc_learn_inval(ARG(0), ARG(1));   /* learn.c: code there may be new */
    xrc_guard_inval(ARG(0), ARG(1));   /* guard.c: recompiled code there checked again */
}

/* KSEG0/KSEG1 masked; a TLB-mapped address through tlb.c's entries (an
 * unmapped one masked as before, where libultra would return -1) */
ULTRA(osVirtualToPhysical) {
    uint32_t v = ARG(0), p = v - 0x80000000u < 0x40000000u ? 0xFFFFFFFFu : xrc_tlb_to_phys(v);
    (void)rdram;
    if (XHW_TEST_BUILD && (p != 0xFFFFFFFFu ? p : v & 0x1FFFFFFFu) < 0x1000u && v >= 0x1000u) {   /* diagnosis */
        static unsigned s_n;
        if (s_n++ < 8) xhw_logf("[ULTRA] osVirtualToPhysical(%08x) = %08x (ra %08x)", (unsigned)v,
                                (unsigned)(p != 0xFFFFFFFFu ? p : v & 0x1FFFFFFFu), (unsigned)ctx->r31);
    }
    RET(p != 0xFFFFFFFFu ? p : v & 0x1FFFFFFFu);
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
/* DIVERGES: a libultra built with its checks reports what fails to a development board. Nothing is there on a
 * retail console, and what they check (the VI manager's state, __osRunningThread) is not kept here for libultra's own
 * code, so a failure says nothing about the game. Logged, and on. */
ULTRA(xrc_osError) {
    static unsigned s_n;
    (void)rdram;
    if (s_n++ < 8) xhw_logf("[OS] libultra's check %d reports an error (a debug build): ignored", (int)(int16_t)ARG(0));
}

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
