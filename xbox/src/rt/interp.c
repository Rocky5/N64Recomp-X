/* interp.c - a MIPS interpreter for the code the recompiler did not
 * translate (PLAN.md 3.14):
 *
 *   - hand-written assembly the recompiler cannot express (a return through
 *     a register other than ra, bal into the middle of other code, a jump
 *     through a register with no table): the analyser marks such functions,
 *     n64rx gives each a body that calls xrc_interp() and enters it in the
 *     function table, so direct calls and function pointers both arrive here
 *   - code the analyser never found (the safety net, learn.c): a call to an
 *     address with no function runs here instead of stopping the game, and
 *     the address is logged for the next build (the learning loop, step 5)
 *
 * It runs the MIPS from RDRAM (the game put it there) on the same
 * recomp_context as the recompiled code. ra is set to a sentinel on entry;
 * reaching the sentinel returns to the recompiled caller. A call (jal, jalr,
 * bal) to a function the table knows runs that function and comes back; a
 * call anywhere else is interpreted in line. A jump through a register to a
 * known function is a tail call.
 *
 * The integer registers live in a local 64-bit file for the whole run,
 * copied from the context on entry and back before every call out and on
 * return: with -DXRC_GPR32=1 a register is the sign extension of its low
 * word (O32 code keeps them so), so 64-bit instructions work here even in a
 * 32-bit build and the recompiled code sees the low words it expects. The
 * FPRs are the context's, reached exactly as the generated code reaches
 * them (N64Recomp's cgenerator.cpp): .s on fN.fl, .d on fN.d, the 32-bit
 * moves and conversions (mtc1, lwc1, cvt.w ...) through f_odd for an odd
 * register, the 64-bit ones on fN.u64. The arithmetic is the generated
 * code's too (recomp.h's helpers, the same triple): the FCR31 rounding mode
 * is MXCSR and the x87 control word (set_cop1_cs), round.w is lround (half
 * away from zero, as upstream: DIVERGES from the VR4300's round-to-even,
 * so an interpreted and a recompiled run agree), the comparisons ignore
 * NaN-ness as upstream does. Integer divide by zero gives the VR4300's
 * results instead of an x86 fault.
 *
 * Every instruction of the VR4300's user ISA the recompiler handles, plus
 * ll/sc/lld/scd (one CPU: sc always succeeds), the traps (do_break when
 * taken), syscall, break; COP0 Status through the runtime, the other COP0
 * registers as a shadow and the TLB instructions through tlb.c (below); cache
 * and sync are no-ops.
 * Anything else stops the game with the instruction named, never a guess.
 * Loads and stores in KSEG1 reach RDRAM through KSEG0 and, past RDRAM, the
 * register model (mmio.c): the analyser sends the code that touches the
 * hardware here for that (romonly.py).
 * There is no decoded-block cache: each instruction is read from RDRAM when
 * it runs, so code an overlay replaces needs no invalidation here.
 *
 * DIVERGES: speed (each instruction decoded every time); the analysis
 * report lists the routines that run here, the learn log the rest. */
#include <math.h>
#include <stdio.h>

#include "rt.h"
#include "xhw.h"

#define SENTINEL 0x8FFFFFF0u        /* an address no code has */
#define STEP_LIMIT 2000000000u      /* a routine that never returns: stop and say so (not for the net:
                                       * a thread's entry it runs may loop for good) */
#define MAX_FRAMES 32               /* in-line calls tracked for the learn log */

recomp_func_t* xrc_try_function(uint32_t vram);   /* ovl.c: NULL when no function starts there */
void xrc_trace_vram(uint32_t vram);                /* trace.c */
/* learn.c: a function the analyser missed ran here (len: bytes it spanned) */
void xrc_learn_func(uint32_t vram, uint32_t len, uint32_t caller);

#define SX32(v) ((uint64_t)(int64_t)(int32_t)(uint32_t)(v))

typedef struct {
    uint32_t entry, ret, top, caller;
} Frame;

typedef struct {
    uint64_t r[32];
    uint64_t hi, lo;
    int fcc;
    int learning;          /* entered by the safety net: log the functions it runs */
    int exc;               /* the refill vector (xrc_interp_refill): eret returns */
    uint32_t epc_k0;       /* k0 when EPC was last written (__osDispatchThread: the thread it restores) */
    int nframes;
    Frame fr[MAX_FRAMES];
} St;

typedef struct {
    int branch, taken, likely, link, jreg, ret, abs;
    uint32_t target;
} Br;

/* ---- the register file ---- */
static void regs_in(St* st, const recomp_context* ctx) {
    const gpr* g = &ctx->r0;
    int i;
    for (i = 0; i < 32; i++) st->r[i] = XRC_GPR32 ? SX32(g[i]) : (uint64_t)g[i];
    st->r[0] = 0;
    st->hi = ctx->hi, st->lo = ctx->lo;
}
static void regs_out(const St* st, recomp_context* ctx) {
    gpr* g = &ctx->r0;
    int i;
    for (i = 1; i < 32; i++) g[i] = (gpr)st->r[i];
    ctx->hi = st->hi, ctx->lo = st->lo;
}

static void frames_report(St* st, int down_to) {
    while (st->nframes > down_to) {
        Frame* f = &st->fr[--st->nframes];
        xrc_learn_func(f->entry, f->top + 4 - f->entry, f->caller);
    }
}

static void stop(St* st, const char* what, uint32_t pc, uint32_t w) __attribute__((noreturn));
static void stop(St* st, const char* what, uint32_t pc, uint32_t w) {
    if (st) frames_report(st, 0);
    xhw_logf("[INTERP] %s: %08x at %08x", what, (unsigned)w, (unsigned)pc);
    xrc_fatal_vram(what, pc, w);
}

/* ---- the FPRs, as the generated code reaches them ---- */
static inline float* fS(recomp_context* ctx, int n) { return &(&ctx->f0)[n].fl; }
static inline double* fD(recomp_context* ctx, int n) { return &(&ctx->f0)[n].d; }
static inline uint64_t* fL(recomp_context* ctx, int n) { return &(&ctx->f0)[n].u64; }
static inline uint32_t* fW(recomp_context* ctx, int n) {
    return (n & 1) ? &ctx->f_odd[(n - 1) * 2] : &(&ctx->f0)[n].u32l;
}

/* fn 8..15 (round/trunc/ceil/floor, .l then .w) and 36/37 (cvt.w/.l) of .s
 * and .d: 1 when handled */
static int to_int(recomp_context* ctx, uint32_t fn, int fd, double v, int is_s) {
    float f = (float)v;
    switch (fn) {
    case 8: *fL(ctx, fd) = (uint64_t)(is_s ? llroundf(f) : llround(v)); return 1;
    case 9: *fL(ctx, fd) = (uint64_t)(is_s ? (int64_t)f : (int64_t)v); return 1;
    case 10: *fL(ctx, fd) = (uint64_t)(is_s ? (int64_t)ceilf(f) : (int64_t)ceil(v)); return 1;
    case 11: *fL(ctx, fd) = (uint64_t)(is_s ? (int64_t)floorf(f) : (int64_t)floor(v)); return 1;
    case 12: *fW(ctx, fd) = (uint32_t)(is_s ? (int32_t)lroundf(f) : (int32_t)lround(v)); return 1;
    case 13: *fW(ctx, fd) = (uint32_t)(is_s ? (int32_t)f : (int32_t)v); return 1;
    case 14: *fW(ctx, fd) = (uint32_t)(is_s ? (int32_t)ceilf(f) : (int32_t)ceil(v)); return 1;
    case 15: *fW(ctx, fd) = (uint32_t)(is_s ? (int32_t)floorf(f) : (int32_t)floor(v)); return 1;
    case 36: *fW(ctx, fd) = (uint32_t)(is_s ? do_cvt_w_s(f) : do_cvt_w_d(v)); return 1;
    case 37: *fL(ctx, fd) = (uint64_t)(is_s ? do_cvt_l_s(f) : do_cvt_l_d(v)); return 1;
    }
    return 0;
}

/* c.cond.fmt as upstream: f/un/sf/ngle false, eq/ueq/seq/ngl ==, olt/ult/lt/nge <, ole/ule/le/ngt <= */
#define COMPARE(a, b, fn) (((fn) >> 1 & 3) == 0 ? 0 : ((fn) >> 1 & 3) == 1 ? (a) == (b) : ((fn) >> 1 & 3) == 2 ? (a) < (b) : (a) <= (b))

static void cop1(recomp_context* ctx, St* st, uint32_t pc, uint32_t w, Br* br) {
    uint32_t rs = (w >> 21) & 31, rt = (w >> 16) & 31, rd = (w >> 11) & 31, fn = w & 63;
    int16_t imm = (int16_t)(w & 0xFFFF);
    int fs = (int)rd, ft = (int)rt, fd = (int)((w >> 6) & 31);
    switch (rs) {
    case 0: if (rt) st->r[rt] = SX32(*fW(ctx, fs)); return;                                      /* mfc1 */
    case 1: if (rt) st->r[rt] = *fL(ctx, fs); return;                                            /* dmfc1 */
    case 2:                                                                                      /* cfc1 */
        if (rt) st->r[rt] = SX32(rd == 31 ? get_cop1_cs() | ((uint32_t)st->fcc << 23) : rd == 0 ? 0x0B00u : 0);
        return;
    case 4: *fW(ctx, fs) = (uint32_t)st->r[rt]; return;                                          /* mtc1 */
    case 5: *fL(ctx, fs) = st->r[rt]; return;                                                    /* dmtc1 */
    case 6:                                                                                      /* ctc1 */
        if (rd == 31) set_cop1_cs((uint32_t)st->r[rt]), st->fcc = (int)((st->r[rt] >> 23) & 1);
        return;
    case 8:                                                                                      /* bc1f/t(l) */
        br->branch = 1, br->likely = (rt & 2) != 0, br->taken = (rt & 1) ? st->fcc : !st->fcc;
        br->target = pc + 4 + ((int32_t)imm << 2);
        return;
    case 16: {   /* .s */
        float a = *fS(ctx, fs), b = *fS(ctx, ft);
        switch (fn) {
        case 0: *fS(ctx, fd) = a + b; return;
        case 1: *fS(ctx, fd) = a - b; return;
        case 2: *fS(ctx, fd) = a * b; return;
        case 3: *fS(ctx, fd) = a / b; return;
        case 4: *fS(ctx, fd) = sqrtf(a); return;
        case 5: *fS(ctx, fd) = fabsf(a); return;
        case 6: *fS(ctx, fd) = a; return;
        case 7: *fS(ctx, fd) = -a; return;
        case 33: *fD(ctx, fd) = (double)a; return;
        }
        if (fn >= 48) { st->fcc = COMPARE(a, b, fn); return; }
        if (to_int(ctx, fn, fd, (double)a, 1)) return;
        break;
    }
    case 17: {   /* .d */
        double a = *fD(ctx, fs), b = *fD(ctx, ft);
        switch (fn) {
        case 0: *fD(ctx, fd) = a + b; return;
        case 1: *fD(ctx, fd) = a - b; return;
        case 2: *fD(ctx, fd) = a * b; return;
        case 3: *fD(ctx, fd) = a / b; return;
        case 4: *fD(ctx, fd) = sqrt(a); return;
        case 5: *fD(ctx, fd) = fabs(a); return;
        case 6: *fD(ctx, fd) = a; return;
        case 7: *fD(ctx, fd) = -a; return;
        case 32: *fS(ctx, fd) = (float)a; return;
        }
        if (fn >= 48) { st->fcc = COMPARE(a, b, fn); return; }
        if (to_int(ctx, fn, fd, a, 0)) return;
        break;
    }
    case 20:   /* .w */
        if (fn == 32) { *fS(ctx, fd) = (float)(int32_t)*fW(ctx, fs); return; }
        if (fn == 33) { *fD(ctx, fd) = (double)(int32_t)*fW(ctx, fs); return; }
        break;
    case 21:   /* .l */
        if (fn == 32) { *fS(ctx, fd) = (float)(int64_t)*fL(ctx, fs); return; }
        if (fn == 33) { *fD(ctx, fd) = (double)(int64_t)*fL(ctx, fs); return; }
        break;
    }
    stop(st, "Interpreter: a COP1 instruction it does not run", pc, w);
}

/* ---- ldl/ldr/sdl/sdr on the 64-bit file (recomp.h's take gpr, 32 bits wide in a GPR32 build) ---- */
static uint64_t ld64(uint8_t* rdram, uint32_t a) {
    return ((uint64_t)(uint32_t)MEM_W(0, a) << 32) | (uint32_t)MEM_W(4, a);
}
static void sd64(uint8_t* rdram, uint32_t a, uint64_t v) {
    MEM_W(0, a) = (int32_t)(uint32_t)(v >> 32);
    MEM_W(4, a) = (int32_t)(uint32_t)v;
}
static uint64_t ldl(uint8_t* rdram, uint64_t init, uint32_t a) {
    uint32_t m = (a & 7) * 8;
    return (init & ~(~0ull << m)) | (ld64(rdram, a & ~7u) << m);
}
static uint64_t ldr(uint8_t* rdram, uint64_t init, uint32_t a) {
    uint32_t m = 56 - (a & 7) * 8;
    return (init & ~(~0ull >> m)) | (ld64(rdram, a & ~7u) >> m);
}
static void sdl(uint8_t* rdram, uint32_t a, uint64_t v) {
    uint32_t m = (a & 7) * 8;
    sd64(rdram, a & ~7u, (ld64(rdram, a & ~7u) & ~(~0ull >> m)) | (v >> m));
}
static void sdr(uint8_t* rdram, uint32_t a, uint64_t v) {
    uint32_t m = 56 - (a & 7) * 8;
    sd64(rdram, a & ~7u, (ld64(rdram, a & ~7u) & ~(~0ull << m)) | (v << m));
}

static void div32(St* st, int32_t a, int32_t b) {
    if (b == 0) st->lo = SX32(a >= 0 ? -1 : 1), st->hi = SX32(a);
    else if (a == INT32_MIN && b == -1) st->lo = SX32(a), st->hi = 0;
    else st->lo = SX32(a / b), st->hi = SX32(a % b);
}
static void divu32(St* st, uint32_t a, uint32_t b) {
    if (b == 0) st->lo = ~0ull, st->hi = SX32(a);
    else st->lo = SX32(a / b), st->hi = SX32(a % b);
}

/* COP0 other than Status (Index, EntryHi, Wired ...): what was written reads
 * back. The TLB instructions go to tlb.c with Index (0), EntryLo0/1 (2, 3),
 * PageMask (5) and EntryHi (10), so osMapTLB / osUnmapTLBAll / __osProbeTLB
 * code the analyser did not name (the recompiler refuses it) maps for real.
 * DIVERGES: tlbwr's Random counts down from 31 to Wired, one step a write
 * (the CPU's counts every cycle). -DXRC_TLBWR_RANDOM=0: tlbwr writes the
 * entry at Wired, as before. */
#ifndef XRC_ERET_RETURN
#define XRC_ERET_RETURN 1
#endif
#ifndef XRC_TLBWR_RANDOM
#define XRC_TLBWR_RANDOM 1
#endif
static uint32_t s_cop0[32];
static uint32_t s_random = 31;

static uint32_t random_index(void) {
    uint32_t wired = s_cop0[6] & 31, i;
    if (!XRC_TLBWR_RANDOM) return wired;
    if (s_random < wired || s_random > 31) s_random = 31;
    i = s_random;
    s_random = s_random > wired ? s_random - 1 : 31;
    return i;
}

static void tlb_op(uint32_t fn) {
    switch (fn) {
    case 1:   /* tlbr */
        xrc_tlb_read(s_cop0[0] & 31, &s_cop0[5], &s_cop0[10], &s_cop0[2], &s_cop0[3]);
        return;
    case 2: case 6:   /* tlbwi, tlbwr */
        xrc_tlb_write((fn == 2 ? s_cop0[0] : random_index()) & 31, s_cop0[5], s_cop0[10], s_cop0[2], s_cop0[3]);
        return;
    case 8:   /* tlbp */
        s_cop0[0] = xrc_tlb_probe(s_cop0[10]);
        return;
    }
}

/* one instruction; a branch or jump only fills `br` (the caller runs the delay slot) */
/* a load or store in KSEG1 past RDRAM: the register model (mmio.c), a word
 * at a time; the narrower ones take their bytes from it big-endian */
static void mmio(recomp_context* ctx, St* st, uint32_t pc, uint32_t w, uint32_t ea) {
    uint32_t op = w >> 26, rt = (w >> 16) & 31, a = ea & ~3u, sh = (3 - (ea & 3)) * 8, v;
    uint64_t t = st->r[rt];
    if (op >= 40 && op != 49 && op != 55) {   /* the stores */
        switch (op) {
        case 40: v = (xrc_mmio_read(a) & ~(0xFFu << sh)) | (((uint32_t)t & 0xFFu) << sh); break;
        case 41: sh &= 16, v = (xrc_mmio_read(a) & ~(0xFFFFu << sh)) | (((uint32_t)t & 0xFFFFu) << sh); break;
        case 43: v = (uint32_t)t; break;
        case 57: v = *fW(ctx, (int)rt); break;
        case 63: xrc_mmio_write(a, (uint32_t)(t >> 32)), v = (uint32_t)t, a += 4; break;
        default: stop(st, "Interpreter: a hardware register access it does not model", pc, w);
        }
        xrc_mmio_write(a, v);
        return;
    }
    v = xrc_mmio_read(a);
#define SETR(x) do { if (rt) st->r[rt] = (x); } while (0)
    switch (op) {
    case 32: SETR((uint64_t)(int64_t)(int8_t)(v >> sh)); return;
    case 33: SETR((uint64_t)(int64_t)(int16_t)(v >> (sh & 16))); return;
    case 35: SETR(SX32(v)); return;
    case 36: SETR((uint8_t)(v >> sh)); return;
    case 37: SETR((uint16_t)(v >> (sh & 16))); return;
    case 39: SETR(v); return;
    case 49: *fW(ctx, (int)rt) = v; return;
    case 55: SETR(((uint64_t)v << 32) | xrc_mmio_read(a + 4)); return;
    }
#undef SETR
    stop(st, "Interpreter: a hardware register access it does not model", pc, w);
}

#ifndef XRC_FETCH_TLB
#define XRC_FETCH_TLB 1
#endif
uint32_t xrc_tlb_resolve(uint32_t vaddr, int store);   /* tlb.c */

int xu_start_dispatched(uint32_t t);   /* ultra/thread.c */

static void exec(uint8_t* rdram, recomp_context* ctx, St* st, uint32_t pc, uint32_t w, Br* br) {
    uint32_t op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31, rd = (w >> 11) & 31, sa = (w >> 6) & 31, fn = w & 63;
    int16_t imm = (int16_t)(w & 0xFFFF);
    uint32_t uimm = w & 0xFFFF, btarget = pc + 4 + ((int32_t)imm << 2);
    uint64_t s = st->r[rs], t = st->r[rt];
    uint32_t ea = (uint32_t)s + (uint32_t)(int32_t)imm;
#define SET(n, v) do { if (n) st->r[n] = (v); } while (0)
#define TRAP(c) do { if (c) do_break(pc); } while (0)
    if (op >= 32 && op != 47 && (ea >> 29) == 5) {   /* KSEG1 */
        if ((ea & 0x1FFFFFFFu) < xrc_rdram_size) ea -= 0x20000000u;   /* RDRAM: the same bytes through KSEG0 */
        else if (xrc_mmio_is(ea)) {
            mmio(ctx, st, pc, w, ea);
            return;
        }
    }
    /* KSEG2/3, and KUSEG where rdram + ea is the Xbox kernel's space (fetch):
     * through the game's TLB to the KSEG0 alias. -DXRC_FETCH_TLB=0: as before */
    if (XRC_FETCH_TLB && op >= 32 && op != 47 &&
        (ea >= 0xC0000000u || ((int32_t)ea >= 0 && (uintptr_t)rdram + ea >= 0x80000000u))) {
        int store = (op >= 40 && op <= 46) || op >= 56;
        uint32_t p = xrc_tlb_resolve(ea, store);
        if (p != ~0u && p < xrc_rdram_size) ea = 0x80000000u | p;
    }
    switch (op) {
    case 0:
        switch (fn) {
        case 0: SET(rd, SX32((uint32_t)t << sa)); return;
        case 2: SET(rd, SX32((uint32_t)t >> sa)); return;
        case 3: SET(rd, SX32((int32_t)(uint32_t)t >> sa)); return;
        case 4: SET(rd, SX32((uint32_t)t << (s & 31))); return;
        case 6: SET(rd, SX32((uint32_t)t >> (s & 31))); return;
        case 7: SET(rd, SX32((int32_t)(uint32_t)t >> (s & 31))); return;
        case 8: br->branch = br->taken = br->jreg = 1, br->target = (uint32_t)s; return;                    /* jr */
        case 9: br->branch = br->taken = br->jreg = 1, br->target = (uint32_t)s, br->link = (int)rd; return; /* jalr */
        case 12:   /* syscall: the generated code's, then return as it does */
            regs_out(st, ctx);
            recomp_syscall_handler(rdram, ctx, (int32_t)pc);
            regs_in(st, ctx);
            br->ret = 1;
            return;
        case 13: do_break(pc); return;
        case 15: return;                                                                                    /* sync */
        case 16: SET(rd, st->hi); return;
        case 17: st->hi = s; return;
        case 18: SET(rd, st->lo); return;
        case 19: st->lo = s; return;
        case 20: SET(rd, t << (s & 63)); return;
        case 22: SET(rd, t >> (s & 63)); return;
        case 23: SET(rd, (uint64_t)((int64_t)t >> (s & 63))); return;
        case 24: { int64_t p = (int64_t)(int32_t)s * (int32_t)t; st->lo = SX32(p); st->hi = SX32(p >> 32); return; }
        case 25: { uint64_t p = (uint64_t)(uint32_t)s * (uint32_t)t; st->lo = SX32(p); st->hi = SX32(p >> 32); return; }
        case 26: div32(st, (int32_t)s, (int32_t)t); return;
        case 27: divu32(st, (uint32_t)s, (uint32_t)t); return;
        case 28: { int64_t lo, hi; DMULT((int64_t)s, (int64_t)t, &lo, &hi); st->lo = (uint64_t)lo, st->hi = (uint64_t)hi; return; }
        case 29: DMULTU(s, t, &st->lo, &st->hi); return;
        case 30:
            if (t == 0) st->lo = (int64_t)s >= 0 ? ~0ull : 1, st->hi = s;
            else { int64_t q, r; DDIV((int64_t)s, (int64_t)t, &q, &r); st->lo = (uint64_t)q, st->hi = (uint64_t)r; }
            return;
        case 31:
            if (t == 0) st->lo = ~0ull, st->hi = s;
            else DDIVU(s, t, &st->lo, &st->hi);
            return;
        case 32: case 33: SET(rd, SX32((uint32_t)s + (uint32_t)t)); return;
        case 34: case 35: SET(rd, SX32((uint32_t)s - (uint32_t)t)); return;
        case 36: SET(rd, s & t); return;
        case 37: SET(rd, s | t); return;
        case 38: SET(rd, s ^ t); return;
        case 39: SET(rd, ~(s | t)); return;
        case 42: SET(rd, (uint64_t)((int64_t)s < (int64_t)t)); return;
        case 43: SET(rd, (uint64_t)(s < t)); return;
        case 44: case 45: SET(rd, s + t); return;
        case 46: case 47: SET(rd, s - t); return;
        case 48: TRAP((int64_t)s >= (int64_t)t); return;
        case 49: TRAP(s >= t); return;
        case 50: TRAP((int64_t)s < (int64_t)t); return;
        case 51: TRAP(s < t); return;
        case 52: TRAP(s == t); return;
        case 54: TRAP(s != t); return;
        case 56: SET(rd, t << sa); return;
        case 58: SET(rd, t >> sa); return;
        case 59: SET(rd, (uint64_t)((int64_t)t >> sa)); return;
        case 60: SET(rd, t << (sa + 32)); return;
        case 62: SET(rd, t >> (sa + 32)); return;
        case 63: SET(rd, (uint64_t)((int64_t)t >> (sa + 32))); return;
        }
        break;
    case 1:
        br->branch = 1, br->target = btarget;
        switch (rt) {
        case 0: br->taken = (int64_t)s < 0; return;
        case 1: br->taken = (int64_t)s >= 0; return;
        case 2: br->taken = (int64_t)s < 0, br->likely = 1; return;
        case 3: br->taken = (int64_t)s >= 0, br->likely = 1; return;
        case 16: br->taken = (int64_t)s < 0, br->link = 31; return;
        case 17: br->taken = (int64_t)s >= 0, br->link = 31; return;
        case 18: br->taken = (int64_t)s < 0, br->likely = 1, br->link = 31; return;
        case 19: br->taken = (int64_t)s >= 0, br->likely = 1, br->link = 31; return;
        }
        br->branch = 0;
        switch (rt) {
        case 8: TRAP((int64_t)s >= (int64_t)imm); return;
        case 9: TRAP(s >= (uint64_t)(int64_t)imm); return;
        case 10: TRAP((int64_t)s < (int64_t)imm); return;
        case 11: TRAP(s < (uint64_t)(int64_t)imm); return;
        case 12: TRAP((int64_t)s == (int64_t)imm); return;
        case 14: TRAP((int64_t)s != (int64_t)imm); return;
        }
        break;
    case 2: case 3:
        br->branch = br->taken = br->abs = 1, br->target = ((pc + 4) & 0xF0000000u) | ((w & 0x03FFFFFFu) << 2);
        if (op == 3) br->link = 31;
        return;
    case 4: case 20: br->branch = 1, br->taken = s == t, br->likely = op == 20, br->target = btarget; return;
    case 5: case 21: br->branch = 1, br->taken = s != t, br->likely = op == 21, br->target = btarget; return;
    case 6: case 22: br->branch = 1, br->taken = (int64_t)s <= 0, br->likely = op == 22, br->target = btarget; return;
    case 7: case 23: br->branch = 1, br->taken = (int64_t)s > 0, br->likely = op == 23, br->target = btarget; return;
    case 8: case 9: SET(rt, SX32((uint32_t)s + (uint32_t)(int32_t)imm)); return;
    case 10: SET(rt, (uint64_t)((int64_t)s < (int64_t)imm)); return;
    case 11: SET(rt, (uint64_t)(s < (uint64_t)(int64_t)imm)); return;
    case 12: SET(rt, s & uimm); return;
    case 13: SET(rt, s | uimm); return;
    case 14: SET(rt, s ^ uimm); return;
    case 15: SET(rt, SX32(uimm << 16)); return;
    case 16:   /* COP0: Status is the runtime's; the rest a shadow, the TLB tlb.c's */
        if (rs == 0) { SET(rt, SX32(rd == 12 && !st->exc ? (uint32_t)cop0_status_read(ctx) : s_cop0[rd])); return; }
        if (rs == 4) {
            if (rd == 12 && !st->exc) cop0_status_write(ctx, (gpr)t);
            else s_cop0[rd] = (uint32_t)t;
            if (rd == 14) st->epc_k0 = (uint32_t)st->r[26];
            return;
        }
        if (rs == 16 && (fn == 1 || fn == 2 || fn == 6 || fn == 8)) { tlb_op(fn); return; }   /* tlbr, tlbwi, tlbwr, tlbp */
        if (rs == 16 && fn == 24 && st->exc) { br->ret = 1; return; }   /* eret, from the refill vector */
        /* DIVERGES: eret outside the exception code (a game's own libultra
         * the analyser did not name: __osDispatchThread's restore of the next
         * thread) returns from the interpreted function: the runtime switches
         * threads itself. -DXRC_ERET_RETURN=0: an instruction it does not run. */
        if (rs == 16 && fn == 24 && XRC_ERET_RETURN) {
            static unsigned s_n;
            /* the thread __osDispatchThread restored: k0 when it wrote EPC from
             * the thread's context (k0 is reused for MI_INTR_MASK after) */
            int started = (st->epc_k0 & 0xE0000003u) == 0x80000000u &&
                                  (uint32_t)MEM_W(0x11C, st->epc_k0) == s_cop0[14]
                              ? xu_start_dispatched(st->epc_k0)
                              : 0;
            if (s_n++ < 4)
                xhw_logf("[INTERP] eret at %08x outside exception code: a return%s", (unsigned)pc,
                         started ? " (the thread it dispatched started)" : "");
            br->ret = 1;
            return;
        }
        break;
    case 17: cop1(ctx, st, pc, w, br); return;
    case 24: case 25: SET(rt, s + (uint64_t)(int64_t)imm); return;
    case 26: SET(rt, ldl(rdram, t, ea)); return;
    case 27: SET(rt, ldr(rdram, t, ea)); return;
    case 32: SET(rt, (uint64_t)(int64_t)MEM_B(0, ea)); return;
    case 33: SET(rt, (uint64_t)(int64_t)MEM_H(0, ea)); return;
    case 34: SET(rt, SX32(do_lwl(rdram, (gpr)t, 0, ea))); return;
    case 35: case 48: SET(rt, SX32(MEM_W(0, ea))); return;   /* lw, ll */
    case 36: SET(rt, (uint64_t)MEM_BU(0, ea)); return;
    case 37: SET(rt, (uint64_t)MEM_HU(0, ea)); return;
    case 38: SET(rt, SX32(do_lwr(rdram, (gpr)t, 0, ea))); return;
    case 39: SET(rt, (uint64_t)(uint32_t)MEM_W(0, ea)); return;
    case 40: MEM_B(0, ea) = (int8_t)t; return;
    case 41: MEM_H(0, ea) = (int16_t)t; return;
    case 42: do_swl(rdram, 0, ea, (gpr)t); return;
    case 43: MEM_W(0, ea) = (int32_t)t; return;
    case 44: sdl(rdram, ea, t); return;
    case 45: sdr(rdram, ea, t); return;
    case 46: do_swr(rdram, 0, ea, (gpr)t); return;
    case 47: return;   /* cache */
    case 49: *fW(ctx, (int)rt) = (uint32_t)MEM_W(0, ea); return;    /* lwc1 */
    case 52: case 55: SET(rt, ld64(rdram, ea)); return;            /* lld, ld */
    case 53: *fL(ctx, (int)rt) = ld64(rdram, ea); return;          /* ldc1 */
    case 56: MEM_W(0, ea) = (int32_t)t; SET(rt, 1); return;         /* sc: one CPU, always succeeds */
    case 57: MEM_W(0, ea) = (int32_t)*fW(ctx, (int)rt); return;     /* swc1 */
    case 60: sd64(rdram, ea, t); SET(rt, 1); return;               /* scd */
    case 61: sd64(rdram, ea, *fL(ctx, (int)rt)); return;           /* sdc1 */
    case 63: sd64(rdram, ea, t); return;                           /* sd */
    }
#undef SET
#undef TRAP
    stop(st, "Interpreter: an instruction it does not run", pc, w);
}

/* A KUSEG address is reached on the host at rdram + vaddr, where the TLB's
 * pages are aliased and an unmapped one faults into the game's refill
 * (tlb.c). For vaddr past 0x80000000 - rdram that lands in the Xbox
 * kernel's own space, where an unmapped page may read as someone else's
 * memory instead of faulting: GoldenEye's paged code at 0x7F0CF000
 * fetched as garbage, a wild jump to 0x7F5C40EC. There the fetch goes
 * through the game's TLB (a refill on a miss) to the KSEG0 alias.
 * DIVERGES: none meant. -DXRC_FETCH_TLB=0: the host alias, as before. */
static uint32_t fetch(uint8_t* rdram, uint32_t pc) {
    if (XRC_FETCH_TLB && (int32_t)pc >= 0 && (uintptr_t)rdram + pc >= 0x80000000u) {
        uint32_t p = xrc_tlb_resolve(pc, 0);
        if (p != ~0u && p < xrc_rdram_size) return (uint32_t)MEM_W(0, 0x80000000u | p);
    }
    return (uint32_t)MEM_W(0, pc);
}

/* each thread's last interpreted instruction and its registers: the crash
 * report's (trace.c), for the thread that crashed */
typedef struct {
    void* thread;
    volatile uint32_t pc;
    St* volatile st;
} Last;
static Last s_last[24];
static Last* last_of(void) {
    void* me = xhw_thread_self();
    unsigned i;
    for (i = 0; i < 24; i++)
        if (s_last[i].thread == me) return &s_last[i];
    for (i = 0; i < 24; i++)
        if (!s_last[i].thread && __sync_bool_compare_and_swap(&s_last[i].thread, NULL, me)) return &s_last[i];
    return &s_last[23];
}
uint32_t xrc_interp_last_pc(void* thread);
uint32_t xrc_interp_last_pc(void* thread) {   /* the stall report's (ultra/thread.c) */
    unsigned i;
    for (i = 0; i < 24; i++)
        if (s_last[i].thread == thread) return s_last[i].pc;
    return 0;
}
void xrc_interp_crash_report(void);
void xrc_interp_crash_report(void) {
    void* me = xhw_thread_self();
    unsigned i;
    int k;
    for (i = 0; i < 24; i++) {
        St* st = s_last[i].st;
        if (s_last[i].thread != me || !s_last[i].pc) continue;
        xhw_logf("[INTERP] this thread's last pc %08x", (unsigned)s_last[i].pc);
        if (st)
            for (k = 0; k < 32; k += 4)
                xhw_logf("[INTERP]   r%-2d %08x  r%-2d %08x  r%-2d %08x  r%-2d %08x", k, (unsigned)st->r[k], k + 1,
                         (unsigned)st->r[k + 1], k + 2, (unsigned)st->r[k + 2], k + 3, (unsigned)st->r[k + 3]);
    }
}

/* test builds: what the interpreter spends its time on, every 5 s (300
 * retraces): instructions run per entry address, the top 6 ([INTERP] hot) */
unsigned xu_retrace(void);   /* ultra/vi.c */
#define HOT_N 64
static struct { uint32_t entry; unsigned steps, calls; } s_hot[HOT_N];
static volatile long s_hot_busy;
static unsigned s_hot_at;

static void hot_note(uint32_t entry, unsigned steps) {
    unsigned i, h = (entry >> 2) % HOT_N, now;
    if (__sync_lock_test_and_set(&s_hot_busy, 1)) return;   /* another thread logs: this one is dropped */
    for (i = 0; i < HOT_N; i++) {
        unsigned k = (h + i) % HOT_N;
        if (s_hot[k].entry == entry || !s_hot[k].entry) {
            s_hot[k].entry = entry, s_hot[k].steps += steps, s_hot[k].calls++;
            break;
        }
    }
    now = xu_retrace();
    if (now - s_hot_at >= 300) {
        unsigned total = 0, n, j;
        for (i = 0; i < HOT_N; i++) total += s_hot[i].steps;
        xhw_logf("[INTERP] hot: %u instructions in %u retraces", total, now - s_hot_at);
        for (n = 0; n < 6; n++) {   /* the top 6, taken out one at a time */
            unsigned best = HOT_N;
            for (j = 0; j < HOT_N; j++)
                if (s_hot[j].entry && (best == HOT_N || s_hot[j].steps > s_hot[best].steps)) best = j;
            if (best == HOT_N) break;
            xhw_logf("[INTERP]   %08x: %u instructions, %u calls", (unsigned)s_hot[best].entry, s_hot[best].steps,
                     s_hot[best].calls);
            s_hot[best].entry = 0;
        }
        memset(s_hot, 0, sizeof s_hot);
        s_hot_at = now;
    }
    __sync_lock_release(&s_hot_busy);
}

static void run(uint8_t* rdram, recomp_context* ctx, uint32_t entry, int learning, uint32_t caller, int exc) {
    St st;
    gpr saved_ra = ctx->r31;
    uint32_t pc = entry, steps = 0;
    Last* last = last_of();
    St* outer = last->st;
    regs_in(&st, ctx);
    st.fcc = 0, st.learning = learning, st.nframes = 0, st.exc = exc, st.epc_k0 = 0;
    st.r[31] = SX32(SENTINEL);
    if (learning) st.fr[st.nframes++] = (Frame){entry, SENTINEL, entry, caller};
    xrc_trace_vram(entry);   /* trace.c: what the crash report names */
    while (pc != SENTINEL) {
        Br br = {0}, ds = {0};
        uint32_t w;
        last->pc = pc, last->st = &st;
        w = fetch(rdram, pc);
        if (++steps > STEP_LIMIT && !learning) stop(&st, "Interpreted code never returned", pc, entry);
        if (st.nframes && pc > st.fr[st.nframes - 1].top && pc - st.fr[st.nframes - 1].entry < 0x10000u)
            st.fr[st.nframes - 1].top = pc;
        exec(rdram, ctx, &st, pc, w, &br);
        if (br.ret) break;
        if (!br.branch) {
            pc += 4;
            continue;
        }
        if (br.link) st.r[br.link] = SX32(pc + 8);
        if (br.likely && !br.taken) {   /* a likely branch not taken skips its delay slot */
            pc += 8;
            continue;
        }
        exec(rdram, ctx, &st, pc + 4, fetch(rdram, pc + 4), &ds);
        if (ds.branch) stop(&st, "Interpreter: a branch in a delay slot", pc + 4, 0);
        if (st.nframes && pc + 4 > st.fr[st.nframes - 1].top && pc + 4 - st.fr[st.nframes - 1].entry < 0x10000u)
            st.fr[st.nframes - 1].top = pc + 4;
        if (!br.taken) {
            pc += 8;
            continue;
        }
#ifdef XRC_INTERP_TRACE_FROM
        {   /* diagnosis: from that pc on, every call and register jump with sp and ra */
            static int s_tr = -1;
            if (s_tr < 0 && pc + 4 >= (uint32_t)(XRC_INTERP_TRACE_FROM) && pc <= (uint32_t)(XRC_INTERP_TRACE_FROM) + 0x400u) s_tr = 20000;
            if (s_tr > 0 && (br.link || br.jreg)) {
                s_tr--;
                xhw_logf("[ITRACE] %08x %s %08x sp %08x ra %08x frames %d%s", (unsigned)pc, br.link ? "call" : "jr",
                         (unsigned)br.target, (unsigned)st.r[29], (unsigned)st.r[31], st.nframes,
                         br.link && xrc_try_function(br.target) ? " (recompiled)" : "");
            }
        }
#endif
        if (br.target == SENTINEL) break;
        if (br.target == pc && !br.link && !fetch(rdram, pc + 4)) pause_self(rdram);   /* b . (an idle loop), as the recompiler does */
        if (br.link) {   /* a call */
            recomp_func_t* f = xrc_try_function(br.target);
            xrc_trace_vram(br.target);
            if (f) {
                regs_out(&st, ctx);
                f(rdram, ctx);
                regs_in(&st, ctx);
                pc += 8;
            } else {     /* code with no function: interpreted in line; logged when the net
                          * runs it, or when it is a jal (a function the analyser missed, not
                          * hand-written code's bal into another routine) */
                if ((learning || br.abs) && st.nframes < MAX_FRAMES) {
                    st.fr[st.nframes++] = (Frame){br.target, pc + 8, br.target, pc};
                    xrc_learn_func(br.target, 0, pc);
                }
                pc = br.target;
            }
            continue;
        }
        if (br.jreg && br.target != entry) {   /* a jump through a register to a function: a tail call */
            recomp_func_t* f = xrc_try_function(br.target);
            if (f) {
                regs_out(&st, ctx);
                f(rdram, ctx);
                regs_in(&st, ctx);
                break;
            }
        }
        if (st.nframes > (learning ? 1 : 0) && br.jreg && br.target == st.fr[st.nframes - 1].ret)
            frames_report(&st, st.nframes - 1);
        pc = br.target;
    }
    frames_report(&st, 0);
    if (XHW_TEST_BUILD) hot_note(entry, steps);
    regs_out(&st, ctx);
    ctx->r31 = saved_ra;   /* back as the caller had it */
    last->st = outer;      /* an enclosing run's registers, not this frame's gone ones */
}

/* tlb.c: a TLB miss runs the game's own exception code, as the CPU would,
 * with BadVAddr, Context, EntryHi and Cause set, until its eret: the refill
 * vector it put at 0x80000000 (GoldenEye's reads its page table through
 * Context into EntryLo0/1 and tlbwr), or else its __osException
 * (-DXRC_OS_EXCEPTION from n64rx: Conker's sends a miss in its paged range
 * to its own handler, which decompresses the page and maps it; libultra's
 * leaves the thread faulted, and thread.c's fault path follows). It runs
 * on a scratch register file with the stack below the thread's (its saves
 * and the dispatcher's restore touch nothing the runtime reads), Status a
 * shadow, and __osRunningThread put back after (the dispatcher pops the
 * game's run queue, which the runtime does not keep). One at a time.
 * 0: nothing to run. */
uint32_t xu_self_sp(void);   /* ultra/thread.c */
int xrc_interp_refill(uint32_t vaddr, int store) {
    static volatile long s_busy;
    uint8_t* rdram = xrc_rdram;
    recomp_context ctx;
    uint32_t start = fetch(rdram, 0x80000000u) ? 0x80000000u : 0, sp;
#ifdef XRC_OS_EXCEPTION
    if (!start) start = XRC_OS_EXCEPTION;
#endif
#ifdef XRC_RUNNING_THREAD
    uint32_t running;
#endif
#ifdef XRC_OS_RUNQUEUE
    uint32_t runq;
#endif
    if (!start) return 0;
    while (__sync_lock_test_and_set(&s_busy, 1)) xhw_sleep_ms(0);
    memset(&ctx, 0, sizeof ctx);
    ctx.f_odd = &ctx.f0.u32h;
    sp = xu_self_sp();
    ctx.r29 = (gpr)(int32_t)((sp ? sp : 0x80000400u) - 0x400u);
    s_cop0[8] = vaddr;                                                    /* BadVAddr */
    s_cop0[4] = (s_cop0[4] & 0xFF800000u) | ((vaddr >> 9) & 0x007FFFF0u);  /* Context: PTEBase | BadVPN2 */
    s_cop0[10] = (vaddr & 0xFFFFE000u) | (s_cop0[10] & 0xFFu);            /* EntryHi: VPN2 | ASID */
    s_cop0[13] = (store ? 3u : 2u) << 2;                                  /* Cause: TLBS / TLBL */
    s_cop0[12] = 0x2000FF03u;                                             /* Status: EXL, interrupts on */
    s_cop0[14] = 0;                                                       /* EPC: not known */
#ifdef XRC_RUNNING_THREAD
    running = (uint32_t)MEM_W(0, XRC_RUNNING_THREAD);
#endif
#ifdef XRC_OS_RUNQUEUE
    /* the game's run queue, which the runtime's threads never fill: a
     * sentinel of priority -1 below the handler's stack meanwhile, so the
     * dispatch after the handler returns to the running thread */
    runq = (uint32_t)MEM_W(0, XRC_OS_RUNQUEUE);
    MEM_W(0, (uint32_t)ctx.r29 - 0x200u) = 0;
    MEM_W(4, (uint32_t)ctx.r29 - 0x200u) = -1;
    MEM_W(0, XRC_OS_RUNQUEUE) = (int32_t)((uint32_t)ctx.r29 - 0x200u);
#endif
    {
        static unsigned s_n;
        if (s_n++ < 8) xhw_logf("[TLB] miss %08x: the game's exception code from %08x", (unsigned)vaddr, (unsigned)start);
    }
    run(rdram, &ctx, start, 0, 0, 1);
#ifdef XRC_RUNNING_THREAD
    MEM_W(0, XRC_RUNNING_THREAD) = (int32_t)running;
#endif
#ifdef XRC_OS_RUNQUEUE
    MEM_W(0, XRC_OS_RUNQUEUE) = (int32_t)runq;
#endif
    __sync_lock_release(&s_busy);
    return 1;
}

static unsigned s_logged;

void xrc_interp(uint8_t* rdram, recomp_context* ctx, uint32_t entry) {
    if (s_logged < 16) s_logged++, xhw_logf("[INTERP] %08x: hand-written code, interpreted", (unsigned)entry);
    run(rdram, ctx, entry, 0, 0, 0);
}

/* learn.c's safety net: code the analyser missed */
void xrc_interp_learn(uint8_t* rdram, recomp_context* ctx, uint32_t entry, uint32_t caller);
void xrc_interp_learn(uint8_t* rdram, recomp_context* ctx, uint32_t entry, uint32_t caller) {
    run(rdram, ctx, entry, 1, caller, 0);
}
