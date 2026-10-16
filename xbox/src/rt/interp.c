/* interp.c - a MIPS interpreter for the code the recompiler cannot express
 * (PLAN.md 3.14): hand-written assembly that returns through a register
 * other than ra (`move s3, ra` ... `jr s3`), calls local subroutines inside
 * other functions with bal, or jumps through registers with no table. The
 * analyser marks such functions; n64rx gives each a body that calls
 * xrc_interp() and enters it in the function table, so direct calls and
 * function pointers both arrive here.
 *
 * It runs the function's MIPS from RDRAM (the code is there: the game
 * loaded it) on the same recomp_context as the recompiled code. ra is set
 * to a sentinel on entry; reaching the sentinel returns to the recompiled
 * caller. A call (jal, jalr, bal) to a function the table knows runs the
 * recompiled function and comes back; a call anywhere else (a local
 * subroutine inside another function's code) is interpreted. A jump
 * through a register to a known function is a tail call. Integer ISA
 * (64-bit with 64-bit registers); COP1 loads, stores, moves, branches and
 * the common S/D/W/L arithmetic, comparisons and conversions; COP0 Status;
 * cache and sync as no-ops. Anything else stops the game with the
 * instruction named, never a guess.
 *
 * DIVERGES: speed (each instruction decoded every time); the analysis
 * report lists the routines that run here. */
#include <math.h>
#include <stdio.h>

#include "rt.h"
#include "xhw.h"

#define SENTINEL 0x8FFFFFF0u        /* an address no code has */
#define STEP_LIMIT 2000000000u      /* a routine that never returns: stop and say so */

recomp_func_t* xrc_try_function(uint32_t vram);   /* ovl.c: NULL when no function starts there */

#define R(n) (((gpr*)&ctx->r0)[n])
#define SX32(v) ((gpr)(int64_t)(int32_t)(uint32_t)(v))

typedef struct {
    uint64_t hi, lo;
    int fcc;
} St;

typedef struct {
    int branch, taken, likely, link, jreg;
    uint32_t target;
} Br;

static void stop(const char* what, uint32_t pc, uint32_t w) __attribute__((noreturn));
static void stop(const char* what, uint32_t pc, uint32_t w) {
    xhw_logf("[INTERP] %s: %08x at %08x", what, (unsigned)w, (unsigned)pc);
    xrc_fatal_vram(what, pc, w);
}

static inline float fs_get(recomp_context* ctx, int n) {
    fpr* f = &ctx->f0;
    return (n & 1) ? f[n - 1].fh : f[n].fl;
}
static inline void fs_set(recomp_context* ctx, int n, float v) {
    fpr* f = &ctx->f0;
    if (n & 1) f[n - 1].fh = v;
    else f[n].fl = v;
}
static inline uint32_t fw_get(recomp_context* ctx, int n) {
    fpr* f = &ctx->f0;
    return (n & 1) ? f[n - 1].u32h : f[n].u32l;
}
static inline void fw_set(recomp_context* ctx, int n, uint32_t v) {
    fpr* f = &ctx->f0;
    if (n & 1) f[n - 1].u32h = v;
    else f[n].u32l = v;
}
static inline double fd_get(recomp_context* ctx, int n) { return (&ctx->f0)[n & ~1].d; }
static inline void fd_set(recomp_context* ctx, int n, double v) { (&ctx->f0)[n & ~1].d = v; }
static inline uint64_t fl_get(recomp_context* ctx, int n) { return (&ctx->f0)[n & ~1].u64; }
static inline void fl_set(recomp_context* ctx, int n, uint64_t v) { (&ctx->f0)[n & ~1].u64 = v; }

static void cop1(recomp_context* ctx, St* st, uint32_t pc, uint32_t w, Br* br) {
    uint32_t rs = (w >> 21) & 31, rt = (w >> 16) & 31, rd = (w >> 11) & 31, fd = (w >> 6) & 31, fn = w & 63;
    int16_t imm = (int16_t)(w & 0xFFFF);
    int fs = (int)rd, ft = (int)rt;
    switch (rs) {
    case 0: if (rt) R(rt) = SX32(fw_get(ctx, fs)); return;                       /* mfc1 */
    case 1: if (rt) R(rt) = (gpr)fl_get(ctx, fs); return;                        /* dmfc1 */
    case 2: if (rt) R(rt) = SX32(rd == 31 ? get_cop1_cs() : 0); return;          /* cfc1 */
    case 4: fw_set(ctx, fs, (uint32_t)R(rt)); return;                            /* mtc1 */
    case 5: fl_set(ctx, fs, (uint64_t)R(rt)); return;                            /* dmtc1 */
    case 6: if (rd == 31) set_cop1_cs((uint32_t)R(rt)); return;                  /* ctc1 */
    case 8:                                                                      /* bc1f / bc1t (likely) */
        br->branch = 1, br->likely = (rt & 2) != 0, br->taken = (rt & 1) ? st->fcc : !st->fcc;
        br->target = pc + 4 + ((int32_t)imm << 2);
        return;
    case 16: {   /* .s */
        float a = fs_get(ctx, fs), b = fs_get(ctx, ft);
        switch (fn) {
        case 0: fs_set(ctx, fd, a + b); return;
        case 1: fs_set(ctx, fd, a - b); return;
        case 2: fs_set(ctx, fd, a * b); return;
        case 3: fs_set(ctx, fd, a / b); return;
        case 4: fs_set(ctx, fd, sqrtf(a)); return;
        case 5: fs_set(ctx, fd, fabsf(a)); return;
        case 6: fs_set(ctx, fd, a); return;
        case 7: fs_set(ctx, fd, -a); return;
        case 13: fw_set(ctx, fd, (uint32_t)(int32_t)a); return;                  /* trunc.w */
        case 12: case 14: case 15: case 36: fw_set(ctx, fd, (uint32_t)do_cvt_w_s(a)); return;
        case 33: fd_set(ctx, fd, (double)a); return;
        case 48: case 49: case 56: case 57: st->fcc = 0; return;
        case 50: case 51: case 58: case 59: st->fcc = a == b; return;
        case 52: case 53: case 60: case 61: st->fcc = a < b; return;
        case 54: case 55: case 62: case 63: st->fcc = a <= b; return;
        }
        break;
    }
    case 17: {   /* .d */
        double a = fd_get(ctx, fs), b = fd_get(ctx, ft);
        switch (fn) {
        case 0: fd_set(ctx, fd, a + b); return;
        case 1: fd_set(ctx, fd, a - b); return;
        case 2: fd_set(ctx, fd, a * b); return;
        case 3: fd_set(ctx, fd, a / b); return;
        case 4: fd_set(ctx, fd, sqrt(a)); return;
        case 5: fd_set(ctx, fd, fabs(a)); return;
        case 6: fd_set(ctx, fd, a); return;
        case 7: fd_set(ctx, fd, -a); return;
        case 13: fw_set(ctx, fd, (uint32_t)(int32_t)a); return;
        case 12: case 14: case 15: case 36: fw_set(ctx, fd, (uint32_t)do_cvt_w_d(a)); return;
        case 32: fs_set(ctx, fd, (float)a); return;
        case 48: case 49: case 56: case 57: st->fcc = 0; return;
        case 50: case 51: case 58: case 59: st->fcc = a == b; return;
        case 52: case 53: case 60: case 61: st->fcc = a < b; return;
        case 54: case 55: case 62: case 63: st->fcc = a <= b; return;
        }
        break;
    }
    case 20:   /* .w */
        if (fn == 32) { fs_set(ctx, fd, (float)(int32_t)fw_get(ctx, fs)); return; }
        if (fn == 33) { fd_set(ctx, fd, (double)(int32_t)fw_get(ctx, fs)); return; }
        break;
    case 21:   /* .l */
        if (fn == 32) { fs_set(ctx, fd, (float)(int64_t)fl_get(ctx, fs)); return; }
        if (fn == 33) { fd_set(ctx, fd, (double)(int64_t)fl_get(ctx, fs)); return; }
        break;
    }
    stop("Interpreter: a COP1 instruction it does not run", pc, w);
}

/* one instruction; a branch or jump only fills `br` (the caller runs the delay slot) */
static void exec(uint8_t* rdram, recomp_context* ctx, St* st, uint32_t pc, uint32_t w, Br* br) {
    uint32_t op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31, rd = (w >> 11) & 31, sa = (w >> 6) & 31, fn = w & 63;
    int16_t imm = (int16_t)(w & 0xFFFF);
    uint32_t uimm = w & 0xFFFF, btarget = pc + 4 + ((int32_t)imm << 2);
    gpr s = R(rs), t = R(rt);
#define SET(r, v) do { if (r) R(r) = (v); } while (0)
    switch (op) {
    case 0:
        switch (fn) {
        case 0: SET(rd, SX32((uint32_t)t << sa)); return;
        case 2: SET(rd, SX32((uint32_t)t >> sa)); return;
        case 3: SET(rd, SX32((int32_t)t >> sa)); return;
        case 4: SET(rd, SX32((uint32_t)t << (s & 31))); return;
        case 6: SET(rd, SX32((uint32_t)t >> (s & 31))); return;
        case 7: SET(rd, SX32((int32_t)t >> (s & 31))); return;
        case 8: br->branch = br->taken = br->jreg = 1, br->target = (uint32_t)s; return;                    /* jr */
        case 9: br->branch = br->taken = br->jreg = 1, br->target = (uint32_t)s, br->link = rd; return;     /* jalr */
        case 15: return;                                                                                    /* sync */
        case 16: SET(rd, (gpr)st->hi); return;
        case 17: st->hi = s; return;
        case 18: SET(rd, (gpr)st->lo); return;
        case 19: st->lo = s; return;
        case 24: { int64_t p = (int64_t)(int32_t)s * (int32_t)t; st->lo = (uint64_t)(int64_t)(int32_t)p; st->hi = (uint64_t)(int64_t)(int32_t)(p >> 32); return; }
        case 25: { uint64_t p = (uint64_t)(uint32_t)s * (uint32_t)t; st->lo = (uint64_t)(int64_t)(int32_t)p; st->hi = (uint64_t)(int64_t)(int32_t)(p >> 32); return; }
        case 26: if ((int32_t)t) { st->lo = (uint64_t)(int64_t)((int32_t)s / (int32_t)t); st->hi = (uint64_t)(int64_t)((int32_t)s % (int32_t)t); } return;
        case 27: if ((uint32_t)t) { st->lo = (uint64_t)(int64_t)(int32_t)((uint32_t)s / (uint32_t)t); st->hi = (uint64_t)(int64_t)(int32_t)((uint32_t)s % (uint32_t)t); } return;
        case 32: case 33: SET(rd, SX32((uint32_t)s + (uint32_t)t)); return;
        case 34: case 35: SET(rd, SX32((uint32_t)s - (uint32_t)t)); return;
        case 36: SET(rd, s & t); return;
        case 37: SET(rd, s | t); return;
        case 38: SET(rd, s ^ t); return;
        case 39: SET(rd, ~(s | t)); return;
        case 42: SET(rd, (gpr)((int64_t)s < (int64_t)t)); return;
        case 43: SET(rd, (gpr)((uint64_t)s < (uint64_t)t)); return;
#if !XRC_GPR32
        case 20: SET(rd, (gpr)((uint64_t)t << (s & 63))); return;
        case 22: SET(rd, (gpr)((uint64_t)t >> (s & 63))); return;
        case 23: SET(rd, (gpr)((int64_t)t >> (s & 63))); return;
        case 28: { __int128 p = (__int128)(int64_t)s * (int64_t)t; st->lo = (uint64_t)p; st->hi = (uint64_t)(p >> 64); return; }
        case 29: { unsigned __int128 p = (unsigned __int128)(uint64_t)s * (uint64_t)t; st->lo = (uint64_t)p; st->hi = (uint64_t)(p >> 64); return; }
        case 30: if ((int64_t)t) { st->lo = (uint64_t)((int64_t)s / (int64_t)t); st->hi = (uint64_t)((int64_t)s % (int64_t)t); } return;
        case 31: if ((uint64_t)t) { st->lo = (uint64_t)s / (uint64_t)t; st->hi = (uint64_t)s % (uint64_t)t; } return;
        case 44: case 45: SET(rd, s + t); return;
        case 46: case 47: SET(rd, s - t); return;
        case 56: SET(rd, (gpr)((uint64_t)t << sa)); return;
        case 58: SET(rd, (gpr)((uint64_t)t >> sa)); return;
        case 59: SET(rd, (gpr)((int64_t)t >> sa)); return;
        case 60: SET(rd, (gpr)((uint64_t)t << (sa + 32))); return;
        case 62: SET(rd, (gpr)((uint64_t)t >> (sa + 32))); return;
        case 63: SET(rd, (gpr)((int64_t)t >> (sa + 32))); return;
#endif
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
        break;
    case 2: case 3:
        br->branch = br->taken = 1, br->target = ((pc + 4) & 0xF0000000u) | ((w & 0x03FFFFFFu) << 2);
        if (op == 3) br->link = 31;
        return;
    case 4: case 20: br->branch = 1, br->taken = s == t, br->likely = op == 20, br->target = btarget; return;
    case 5: case 21: br->branch = 1, br->taken = s != t, br->likely = op == 21, br->target = btarget; return;
    case 6: case 22: br->branch = 1, br->taken = (int64_t)s <= 0, br->likely = op == 22, br->target = btarget; return;
    case 7: case 23: br->branch = 1, br->taken = (int64_t)s > 0, br->likely = op == 23, br->target = btarget; return;
    case 8: case 9: SET(rt, SX32((uint32_t)s + (uint32_t)(int32_t)imm)); return;
    case 10: SET(rt, (gpr)((int64_t)s < (int64_t)imm)); return;
    case 11: SET(rt, (gpr)((uint64_t)s < (uint64_t)(int64_t)imm)); return;
    case 12: SET(rt, s & uimm); return;
    case 13: SET(rt, s | uimm); return;
    case 14: SET(rt, s ^ uimm); return;
    case 15: SET(rt, SX32(uimm << 16)); return;
    case 16:   /* COP0: Status only */
        if (rs == 0) { SET(rt, rd == 12 ? cop0_status_read(ctx) : 0); return; }
        if (rs == 4) { if (rd == 12) cop0_status_write(ctx, t); return; }
        break;
    case 17: cop1(ctx, st, pc, w, br); return;
    case 32: SET(rt, (gpr)(int64_t)MEM_B(imm, s)); return;
    case 33: SET(rt, (gpr)(int64_t)MEM_H(imm, s)); return;
    case 34: SET(rt, do_lwl(rdram, t, imm, s)); return;
    case 35: SET(rt, (gpr)(int64_t)MEM_W(imm, s)); return;
    case 36: SET(rt, (gpr)MEM_BU(imm, s)); return;
    case 37: SET(rt, (gpr)MEM_HU(imm, s)); return;
    case 38: SET(rt, do_lwr(rdram, t, imm, s)); return;
    case 39: SET(rt, (gpr)(uint32_t)MEM_W(imm, s)); return;
    case 40: MEM_B(imm, s) = (int8_t)t; return;
    case 41: MEM_H(imm, s) = (int16_t)t; return;
    case 42: do_swl(rdram, imm, s, t); return;
    case 43: MEM_W(imm, s) = (int32_t)t; return;
    case 46: do_swr(rdram, imm, s, t); return;
    case 47: return;   /* cache */
    case 49: fw_set(ctx, (int)rt, (uint32_t)MEM_W(imm, s)); return;   /* lwc1 */
    case 57: MEM_W(imm, s) = (int32_t)fw_get(ctx, (int)rt); return;   /* swc1 */
    case 53: fl_set(ctx, (int)rt, (uint64_t)LD(imm, s)); return;      /* ldc1 */
    case 61: SD(fl_get(ctx, (int)rt), imm, s); return;                /* sdc1 */
#if !XRC_GPR32
    case 24: case 25: SET(rt, s + (gpr)(int64_t)imm); return;
    case 26: SET(rt, do_ldl(rdram, t, imm, s)); return;
    case 27: SET(rt, do_ldr(rdram, t, imm, s)); return;
    case 44: do_sdl(rdram, imm, s, t); return;
    case 45: do_sdr(rdram, imm, s, t); return;
    case 55: SET(rt, LD(imm, s)); return;
    case 63: SD(t, imm, s); return;
#endif
    }
#undef SET
    stop("Interpreter: an instruction it does not run", pc, w);
}

static unsigned s_logged;

void xrc_interp(uint8_t* rdram, recomp_context* ctx, uint32_t entry) {
    St st = {ctx->hi, ctx->lo, 0};
    gpr saved_ra = R(31);
    uint32_t pc = entry, steps = 0;
    if (s_logged < 16) s_logged++, xhw_logf("[INTERP] %08x: hand-written code, interpreted", (unsigned)entry);
    R(31) = SX32(SENTINEL);
    while (pc != SENTINEL) {
        Br br = {0}, ds = {0};
        uint32_t w = (uint32_t)MEM_W(0, (gpr)(int32_t)pc);
        if (++steps > STEP_LIMIT) stop("Interpreted code never returned", pc, entry);
        exec(rdram, ctx, &st, pc, w, &br);
        if (!br.branch) {
            pc += 4;
            continue;
        }
        if (br.link) R(br.link) = SX32(pc + 8);
        if (br.likely && !br.taken) {   /* a likely branch not taken skips its delay slot */
            pc += 8;
            continue;
        }
        exec(rdram, ctx, &st, pc + 4, (uint32_t)MEM_W(0, (gpr)(int32_t)(pc + 4)), &ds);
        if (ds.branch) stop("Interpreter: a branch in a delay slot", pc + 4, 0);
        if (!br.taken) {
            pc += 8;
            continue;
        }
        if (br.target == SENTINEL) break;
        if (br.link) {   /* a call */
            recomp_func_t* f = xrc_try_function(br.target);
            if (f) {
                f(rdram, ctx);
                pc += 8;
            } else {
                pc = br.target;   /* a local subroutine: interpreted */
            }
            continue;
        }
        if (br.jreg && br.target != entry) {   /* a jump through a register to a function: a tail call */
            recomp_func_t* f = xrc_try_function(br.target);
            if (f) {
                f(rdram, ctx);
                break;
            }
        }
        pc = br.target;
    }
    R(31) = saved_ra;
    ctx->hi = st.hi, ctx->lo = st.lo;
}
