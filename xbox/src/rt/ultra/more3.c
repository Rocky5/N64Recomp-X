/* more3.c - libultra the sweep found missing (tools/xbox/sweep.py, PLAN.md
 * 3.14): the internals of N64Recomp's ignored list that some games call
 * from their own code, and the landing for every call nothing provides.
 *
 * xrc_missing: n64rx gives every function the generated code calls and
 * nothing declares a weak body (gen/xrc_missing.c, pipeline.py) that lands
 * here, so a game that reaches one more libultra internal links and says
 * which at run time ("[ULTRA] missing <name>", once each, v0 = 0) instead
 * of failing to link. A runtime or pack body of the same name replaces the
 * weak one. -DXRC_MISSING_FATAL=1: the fatal card at the first call
 * instead; -DXRC_NO_MISSING_STUBS=1: no weak bodies (the link names them).
 * DIVERGES: a missing function returns 0 and does nothing. */
#include <stdio.h>

#include "ultra.h"

#ifndef XRC_MISSING_FATAL
#define XRC_MISSING_FATAL 0
#endif

void xrc_missing(const char* name, uint8_t* rdram, recomp_context* ctx) {
    static const char* seen[64];
    static unsigned nseen;
    unsigned i;
    (void)rdram;
    if (XRC_MISSING_FATAL) {
        char msg[96];
        snprintf(msg, sizeof msg, "%s (called from %08x)", name, (unsigned)(uint32_t)ctx->r31);
        xhw_fatal("A libultra function is missing", msg);
    }
    for (i = 0; i < nseen; i++)
        if (seen[i] == name)
            break;
    if (i == nseen) {
        if (nseen < 64)
            seen[nseen++] = name;
        xhw_logf("[ULTRA] missing %s (ra %08x): returns 0", name, (unsigned)(uint32_t)ctx->r31);
    }
    RET(0);
}

/* ---- the RSP's status register: games that drive the RSP themselves
 * (their own copy of osSpTaskStartGo's checks) poll it. Tasks run to the
 * end inside osSpTaskStartGo (sp.c), so the RSP always reads halted and
 * broken with "task done" (signal 2) up; the other signals are what the
 * game last wrote. DIVERGES: no RSP to start or stop. ---- */
enum { SPS_HALT = 0x1, SPS_BROKE = 0x2, SPS_SIG0 = 0x80, SPS_SIG2 = 0x200 };
static uint32_t s_sp_sigs;

ULTRA(__osSpSetStatus) {
    uint32_t w = ARG(0);
    int i;
    (void)rdram;
    for (i = 0; i < 8; i++) {   /* SP_CLR_SIGn = bit 9 + 2n, SP_SET_SIGn = bit 10 + 2n */
        if (w & (1u << (9 + 2 * i)))
            s_sp_sigs &= ~(SPS_SIG0 << i);
        if (w & (1u << (10 + 2 * i)))
            s_sp_sigs |= SPS_SIG0 << i;
    }
}

ULTRA(__osSpGetStatus) {
    (void)rdram;
    RET(s_sp_sigs | SPS_HALT | SPS_BROKE | SPS_SIG2);
}

ULTRA(__osSpDeviceBusy) {
    (void)rdram;
    RET(0);
}

/* ---- CP0 Count/Compare: the runtime's timers (time.c) do not use the
 * compare interrupt; a game's own timer code writing it is ignored ---- */
ULTRA(__osSetCompare) {
    (void)rdram;
    (void)ctx;
}

/* ---- the VI manager's context swap: vi.c owns the retrace; a game's own
 * copy of the manager calling it changes nothing ---- */
ULTRA(__osViSwapContext) {
    (void)rdram;
    (void)ctx;
}

/* ---- IDO's signed 64-bit conversions (the unsigned ones are extra.c's):
 * the integer in a0:a1 or v0:v1, the float in f12 or f0; C's truncation,
 * as cvt.l.d / trunc.l.d on the round-to-zero libultra sets ---- */
ULTRA(__ll_to_d) {
    (void)rdram;
    ctx->f0.d = (double)(int64_t)xu_arg64(ctx, 0);
}
ULTRA(__ll_to_f) {
    (void)rdram;
    ctx->f0.fl = (float)(int64_t)xu_arg64(ctx, 0);
}
ULTRA(__d_to_ll) {
    (void)rdram;
    RET64((uint64_t)(int64_t)ctx->f12.d);
}
ULTRA(__f_to_ll) {
    (void)rdram;
    RET64((uint64_t)(int64_t)ctx->f12.fl);
}
ULTRA(__d_to_ull) {
    double d = ctx->f12.d;
    (void)rdram;
    RET64(d <= 0.0 ? 0 : d >= 18446744073709551616.0 ? ~0ull : (uint64_t)d);
}
ULTRA(__f_to_ull) {
    float f = ctx->f12.fl;
    (void)rdram;
    RET64(f <= 0.0f ? 0 : f >= 18446744073709551616.0f ? ~0ull : (uint64_t)f);
}
