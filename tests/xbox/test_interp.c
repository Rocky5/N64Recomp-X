/* test_interp.c - the MIPS interpreter (xbox/src/rt/interp.c) against
 * expected results, instruction by instruction, on the host: small programs
 * assembled here into a fake RDRAM, run with xrc_interp / xrc_interp_learn,
 * registers and memory checked. Built twice by tools/xbox/test_interp.py
 * (64-bit registers and -DXRC_GPR32=1). Exit code 0 = passed. */
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "rt.h"
#include "xhw.h"

/* ---- the runtime the interpreter reaches, stubbed ---- */
static uint8_t s_ram[0x10000];
uint8_t* xrc_rdram;
uint8_t* xrc_rdram_host = s_ram;
uint32_t xrc_rdram_size = sizeof s_ram;
static jmp_buf s_jb;
static const char* s_fatal;
static uint32_t s_fatal_pc, s_breaks, s_syscalls;

void xhw_logf(const char* fmt, ...) { (void)fmt; }
void xrc_fatal_vram(const char* what, uint32_t vram, uint32_t extra) {
    (void)extra;
    s_fatal = what, s_fatal_pc = vram;
    longjmp(s_jb, 1);
}
void cop0_status_write(recomp_context* ctx, gpr value) { ctx->status_reg = (uint32_t)value; }
gpr cop0_status_read(recomp_context* ctx) { return ctx->status_reg; }
/* tlb.c's host side (xhw_alias.c): the calls, recorded */
static uint32_t s_alias[8][3], s_nalias, s_unalias[8][2], s_nunalias;
int xhw_alias_map(uint32_t va, uint32_t pa, uint32_t bytes) {
    if (s_nalias < 8) s_alias[s_nalias][0] = va, s_alias[s_nalias][1] = pa, s_alias[s_nalias][2] = bytes;
    s_nalias++;
    return 1;
}
void xhw_alias_unmap(uint32_t va, uint32_t bytes) {
    if (s_nunalias < 8) s_unalias[s_nunalias][0] = va, s_unalias[s_nunalias][1] = bytes;
    s_nunalias++;
}
/* mmio.c: tests/xbox/test_mmio.c has the model; here KSEG1 past RDRAM is nothing */
int xrc_mmio_is(uint32_t vaddr) { (void)vaddr; return 0; }
uint32_t xrc_mmio_read(uint32_t vaddr) { (void)vaddr; return 0; }
void xrc_mmio_write(uint32_t vaddr, uint32_t v) { (void)vaddr, (void)v; }
void do_break(uint32_t vram) { s_breaks++, s_fatal_pc = vram; }
void recomp_syscall_handler(uint8_t* rdram, recomp_context* ctx, int32_t vram) { (void)rdram, (void)vram, ctx->r2 = 0x5C, s_syscalls++; }
int xu_start_dispatched(uint32_t t) { (void)t; return 0; }
void pause_self(uint8_t* rdram) { (void)rdram; xrc_fatal_vram("pause_self", 0, 0); }

static void known_fn(uint8_t* rdram, recomp_context* ctx) { (void)rdram; ctx->r2 = (gpr)(ctx->r4 * 3); }
void xrc_interp(uint8_t* rdram, recomp_context* ctx, uint32_t entry);
/* a routine the runtime interprets, as a build's xrc_interp.c has it (t_inline) */
#define SUB_AT 0x80004000u
static void sub_stub(uint8_t* rdram, recomp_context* ctx) { xrc_interp(rdram, ctx, SUB_AT); }
typedef struct { uint32_t vram; recomp_func_t* fn; } XrcInterpEntry;
const XrcInterpEntry xrc_interp_table[] = {{SUB_AT, sub_stub}};
const unsigned xrc_interp_rows = 1;
/* a recompiled function, called by an interpreted run, that exits through another: t_sp_escape */
#define ESC_SUB 0x80003100u
#define ESC_EXIT 0x80005200u
static int s_esc_calls;
static void esc_sub(uint8_t* rdram, recomp_context* ctx) {
    s_esc_calls++;
    xrc_interp(rdram, ctx, ESC_EXIT);
    s_esc_calls += 100;   /* never: the exit does not come back here */
}
recomp_func_t* xrc_try_function(uint32_t vram) {
    return vram == 0x80003000u ? known_fn : vram == SUB_AT ? sub_stub : vram == ESC_SUB ? esc_sub : NULL;
}

static struct { uint32_t vram, len, caller; } s_learnt[8];
static int s_nlearnt;
void xrc_trace_vram(uint32_t vram) { (void)vram; }   /* trace.c: nothing to record on the host */
void xhw_sleep_ms(uint32_t ms) { (void)ms; }
xhw_thread* xhw_thread_self(void) { return (xhw_thread*)0; }
uint64_t xhw_time_us(void) { return 0; }
static uint64_t s_count;   /* ultra/time.c: a counter that runs (each read moves it on) */
uint64_t xu_count64(void) { return s_count += 100; }
void xu_count_set32(uint32_t v) { s_count = (s_count & ~0xFFFFFFFFull) | v; }
int xhw_perf_detail(void) { return 0; }
void xhw_perf_add(int bucket, uint64_t us) { (void)bucket, (void)us; }
int xu_fault_wait(uint32_t vaddr, int store) { (void)vaddr; (void)store; return 0; }   /* ultra/thread.c */
void xhw_crash_set_fixup(int (*fn)(uint32_t host, int write)) { (void)fn; }
int xrc_kseg1_heal(uint32_t host) { (void)host; return 0; }
int xrc_cart_fault(uint32_t host) { (void)host; return 0; }          /* mmio.c */
int xrc_gap_fault(uint32_t host) { (void)host; return 0; }           /* mmio.c */
void xrc_guard_kuseg(void) {}                                        /* guard.c */
static int s_guard_stale;
int xrc_guard_current(uint32_t vram) { return !(s_guard_stale && vram == 0x80003000u); }   /* guard.c */
/* strand.c: as if another context ran and something switched back to this stack */
static uint32_t s_sw_target, s_sw_sp, s_sw_mine;
static int s_sw_n;
void xrc_strand_switch(uint8_t* rdram, recomp_context* ctx, uint32_t target, uint32_t my_sp) {
    (void)rdram;
    s_sw_n++, s_sw_target = target, s_sw_sp = (uint32_t)ctx->r29, s_sw_mine = my_sp;
    ctx->r29 = (gpr)(int32_t)my_sp;
    ctx->r2 = 77;
}
void xrc_guard_kuseg_range(uint32_t a, uint32_t b) { (void)a, (void)b; }  /* guard.c */
uint32_t xrc_ovl_reloc_vram(uint32_t v) { return v; }                     /* ovl.c */
uint32_t xrc_ipl3_vec[4];                                            /* boot.c */
void xrc_learn_kuseg(uint32_t va, int aliased) { (void)va, (void)aliased; }
void xrc_learn_func(uint32_t vram, uint32_t len, uint32_t caller) {
    if (s_nlearnt < 8) s_learnt[s_nlearnt].vram = vram, s_learnt[s_nlearnt].len = len, s_learnt[s_nlearnt++].caller = caller;
}

void xrc_interp(uint8_t* rdram, recomp_context* ctx, uint32_t entry);
void xrc_interp_learn(uint8_t* rdram, recomp_context* ctx, uint32_t entry, uint32_t caller);

/* ---- an assembler, just enough ---- */
enum { zero, at, v0, v1, a0, a1, a2, a3, t0, t1, t2, t3, t4, t5, t6, t7, s0, s1, s2, s3, s4, s5, s6, s7, t8, t9, k0, k1, gp, sp, fp, ra };
#define RT(fn, rs, rt, rd, sa) ((uint32_t)(((rs) << 21) | ((rt) << 16) | ((rd) << 11) | ((sa) << 6) | (fn)))
#define IT(op, rs, rt, imm) ((uint32_t)(((uint32_t)(op) << 26) | ((rs) << 21) | ((rt) << 16) | ((imm) & 0xFFFF)))
#define JT(op, a) ((uint32_t)(((uint32_t)(op) << 26) | (((a) >> 2) & 0x3FFFFFF)))
#define FT(fmt, ft, fs, fd, fn) ((uint32_t)((17u << 26) | ((fmt) << 21) | ((ft) << 16) | ((fs) << 11) | ((fd) << 6) | (fn)))
#define C1(sub, rt, fs) ((uint32_t)((17u << 26) | ((sub) << 21) | ((rt) << 16) | ((fs) << 11)))
#define NOP 0u
#define JR_RA RT(8, ra, 0, 0, 0)
#define ADDIU(t, s, i) IT(9, s, t, i)
#define LUI(t, i) IT(15, 0, t, i)
#define ORI(t, s, i) IT(13, s, t, i)
#define S_ 16
#define D_ 17
#define W_ 20
#define L_ 21

static void put(uint32_t vram, const uint32_t* w, int n) {
    int i;
    for (i = 0; i < n; i++) *(uint32_t*)(s_ram + (vram & 0xFFFF) + 4 * i) = w[i];
}
static uint32_t rd32(uint32_t vram) { return *(uint32_t*)(s_ram + (vram & 0xFFFF)); }
static void wr32(uint32_t vram, uint32_t v) { *(uint32_t*)(s_ram + (vram & 0xFFFF)) = v; }

static recomp_context C;
static int s_fail;
#define ENTRY 0x80001000u
#define G(n) ((uint64_t)(int64_t)(int32_t)(uint32_t)(n))   /* a register as it should read, sign-extended */
#define REG(n) ((uint64_t)(XRC_GPR32 ? (uint64_t)(int64_t)(int32_t)((gpr*)&C.r0)[n] : (uint64_t)((gpr*)&C.r0)[n]))

static void fresh(void) {
    memset(&C, 0, sizeof C);
#if !XRC_FODD_ARRAY
    C.f_odd = &C.f0.u32h;
#endif
    C.r29 = (gpr)(int32_t)0x8000F000u;
    C.r31 = (gpr)(int32_t)0x80000ABCu;
    s_fatal = NULL, s_breaks = s_syscalls = 0, s_nlearnt = 0;
    set_cop1_cs(0);
}
static int run_prog(const uint32_t* w, int n) {
    put(ENTRY, w, n);
    if (setjmp(s_jb)) return 0;
    xrc_interp(xrc_rdram, &C, ENTRY);
    return 1;
}
#define RUN(...) do { static const uint32_t p_[] = {__VA_ARGS__}; run_prog(p_, (int)(sizeof p_ / 4)); } while (0)
#define CHECK(name, got, want) do { uint64_t g_ = (uint64_t)(got), w_ = (uint64_t)(want); \
    if (g_ != w_) { printf("  %s: got %016llx, want %016llx\n", name, (unsigned long long)g_, (unsigned long long)w_); s_fail++; } } while (0)
#define CHECKF(name, got, want) do { double g_ = (got), w_ = (want); \
    if (g_ != w_) { printf("  %s: got %g, want %g\n", name, g_, w_); s_fail++; } } while (0)

static void t_int(void) {
    fresh();
    RUN(LUI(t0, 0x8000), ORI(t0, t0, 0x1234), ADDIU(t1, zero, -1), RT(33, t0, t1, t2, 0) /* addu */,
        RT(35, zero, t1, t3, 0) /* subu */, RT(0, 0, t1, t4, 4) /* sll */, RT(2, 0, t1, t5, 28) /* srl */,
        RT(3, 0, t0, t6, 4) /* sra */, RT(42, t1, zero, t7, 0) /* slt */, RT(43, t1, zero, s0, 0) /* sltu */,
        IT(10, t1, s1, 0) /* slti */, IT(11, zero, s2, -1) /* sltiu */, RT(39, zero, zero, s3, 0) /* nor */,
        IT(14, t0, s4, 0xFFFF) /* xori */, JR_RA, NOP);
    CHECK("lui/ori", REG(t0), G(0x80001234u));
    CHECK("addu", REG(t2), G(0x80001233u));
    CHECK("subu", REG(t3), G(1));
    CHECK("sll", REG(t4), G(0xFFFFFFF0u));
    CHECK("srl", REG(t5), G(0xF));
    CHECK("sra", REG(t6), G(0xF8000123u));
    CHECK("slt", REG(t7), 1);
    CHECK("sltu", REG(s0), 0);
    CHECK("slti", REG(s1), 1);
    CHECK("sltiu", REG(s2), 1);
    CHECK("nor", REG(s3), G(0xFFFFFFFFu));
    CHECK("xori", REG(s4), G(0x8000EDCBu));
    CHECK("ra kept", REG(ra), G(0x80000ABCu));

    fresh();   /* mult / div and the VR4300's divide by zero, no x86 fault */
    C.r8 = (gpr)(int32_t)-7, C.r9 = 3, C.r10 = (gpr)(int32_t)0x80000000u, C.r11 = (gpr)(int32_t)-1;
    RUN(RT(24, t0, t1, 0, 0), RT(18, 0, 0, s0, 0), RT(16, 0, 0, s1, 0) /* mult -21 */,
        RT(25, t0, t1, 0, 0), RT(18, 0, 0, s2, 0), RT(16, 0, 0, s3, 0) /* multu */,
        RT(26, t0, t1, 0, 0), RT(18, 0, 0, s4, 0), RT(16, 0, 0, s5, 0) /* div -7/3 */,
        RT(26, t2, t3, 0, 0), RT(18, 0, 0, s6, 0), RT(16, 0, 0, s7, 0) /* INT_MIN / -1 */,
        RT(27, t0, zero, 0, 0), RT(18, 0, 0, t4, 0), RT(16, 0, 0, t5, 0) /* divu by 0 */,
        RT(26, t0, zero, 0, 0), RT(18, 0, 0, t6, 0) /* div -7 by 0 */, JR_RA, NOP);
    CHECK("mult lo", REG(s0), G(-21));
    CHECK("mult hi", REG(s1), G(-1));
    CHECK("multu lo", REG(s2), G((uint32_t)(0xFFFFFFF9ull * 3)));
    CHECK("multu hi", REG(s3), G(2));
    CHECK("div lo", REG(s4), G(-2));
    CHECK("div hi", REG(s5), G(-1));
    CHECK("div min lo", REG(s6), G(0x80000000u));
    CHECK("div min hi", REG(s7), 0);
    CHECK("divu 0 lo", REG(t4), G(0xFFFFFFFFu));
    CHECK("divu 0 hi", REG(t5), G(-7));
    CHECK("div -x/0 lo", REG(t6), G(1));
}

static void t_64(void) {
    /* 64-bit work inside one run: in a GPR32 build too, through memory */
    fresh();
    RUN(ADDIU(t0, zero, 0x1234), RT(60, 0, t0, t0, 0) /* dsll32 */, ORI(t0, t0, 0x5678), ADDIU(t1, zero, -1),
        RT(45, t0, t1, t2, 0) /* daddu */, LUI(s0, 0x8000), ORI(s0, s0, 0x2000), IT(63, s0, t2, 0) /* sd */,
        IT(55, s0, t3, 0) /* ld */, RT(63, 0, t3, t4, 0) /* dsra32 */, RT(62, 0, t1, t5, 28) /* dsrl32 */,
        ADDIU(t6, zero, 3), RT(29, t3, t6, 0, 0) /* dmultu */, RT(18, 0, 0, t7, 0), IT(63, s0, t7, 8),
        RT(31, t3, t6, 0, 0) /* ddivu */, RT(18, 0, 0, s1, 0), IT(63, s0, s1, 16),
        IT(25, zero, s2, -2) /* daddiu */, RT(23, t6, s2, s3, 0) /* dsrav */, JR_RA, NOP);
    CHECK("sd hi", rd32(0x80002000), 0x1234);
    CHECK("sd lo", rd32(0x80002004), 0x5677);
    CHECK("dsra32", REG(t4), 0x1234);
    CHECK("dsrl32", REG(t5), 0xF);
    CHECK("dmultu hi word", rd32(0x80002008), 0x369C);
    CHECK("dmultu lo word", rd32(0x8000200C), 0x10365);
    CHECK("ddivu", ((uint64_t)rd32(0x80002010) << 32) | rd32(0x80002014), 0x0000123400005677ull / 3);
    CHECK("dsrav", REG(s3), G(-1));

    fresh();   /* unaligned: lwl/lwr, swl/swr, ldl/ldr, sdl/sdr */
    wr32(0x80002000, 0x00112233), wr32(0x80002004, 0x44556677), wr32(0x80002008, 0x8899AABB);
    wr32(0x8000200C, 0xCCDDEEFF);
    memset(s_ram + 0x2020, 0, 0x20);
    RUN(LUI(s0, 0x8000), ORI(s0, s0, 0x2000), IT(34, s0, t0, 1) /* lwl */, IT(38, s0, t0, 4) /* lwr */,
        IT(42, s0, t0, 0x21) /* swl */, IT(46, s0, t0, 0x24) /* swr */, IT(26, s0, t1, 3) /* ldl */,
        IT(27, s0, t1, 10) /* ldr */, IT(44, s0, t1, 0x33) /* sdl */, IT(45, s0, t1, 0x3A) /* sdr */,
        IT(63, s0, t1, 0x28) /* sd */, JR_RA, NOP);
    CHECK("lwl/lwr", REG(t0), G(0x11223344u));
    CHECK("swl", rd32(0x80002020), 0x00112233);
    CHECK("swr", rd32(0x80002024), 0x44000000);
    CHECK("ldl/ldr hi", rd32(0x80002028), 0x33445566);
    CHECK("ldl/ldr lo", rd32(0x8000202C), 0x778899AA);
    CHECK("sdl 0", rd32(0x80002030), 0x00000033);
    CHECK("sdl 1", rd32(0x80002034), 0x44556677);
    CHECK("sdr 0", rd32(0x80002038), 0x8899AA00);
    CHECK("sdr 1", rd32(0x8000203C), 0);
}

static void t_branch(void) {
    fresh();   /* a loop, likely branches, a link */
    RUN(ADDIU(t0, zero, 5), ADDIU(v0, zero, 0), ADDIU(v0, v0, 2), ADDIU(t0, t0, -1), IT(5, t0, zero, -3) /* bne */,
        NOP, IT(21, zero, zero, 2) /* bnel not taken: skips its slot */, ADDIU(v1, zero, 9), ADDIU(a0, zero, 1),
        IT(20, zero, zero, 2) /* beql taken */, ADDIU(a1, zero, 4), ADDIU(a2, zero, 99), IT(1, zero, 1, 2) /* bgez */,
        NOP, ADDIU(a3, zero, 1), JR_RA, NOP);
    CHECK("loop", REG(v0), 10);
    CHECK("bnel slot", REG(v1), 0);
    CHECK("after bnel", REG(a0), 1);
    CHECK("beql slot", REG(a1), 4);
    CHECK("beql skipped", REG(a2), 0);
    CHECK("bgez skipped", REG(a3), 0);

    fresh();   /* a call to a known function (its delay slot first), and a call into code with none */
    put(0x80002000, (const uint32_t[]){ADDIU(v1, a0, 100), JR_RA, ADDIU(v1, v1, 1)}, 3);
    RUN(RT(37, ra, zero, s7, 0) /* or s7, ra */, JT(3, 0x80003000), ADDIU(a0, zero, 7), ADDIU(s0, v0, 0),
        JT(3, 0x80002000), NOP, RT(8, s7, 0, 0, 0) /* jr s7 */, NOP);
    CHECK("known call", REG(s0), 21);
    CHECK("in-line call", REG(v1), 7 + 100 + 1);

    fresh();   /* the safety net's log: each function once returned, with its span and caller */
    put(0x80002000, (const uint32_t[]){ADDIU(v0, v0, 5), JR_RA, NOP}, 3);
    put(ENTRY, (const uint32_t[]){ADDIU(sp, sp, -8), IT(43, sp, ra, 0), JT(3, 0x80002000), NOP, IT(35, sp, ra, 0),
                                  JR_RA, ADDIU(sp, sp, 8)}, 7);
    if (!setjmp(s_jb)) xrc_interp_learn(xrc_rdram, &C, ENTRY, 0x80000500u);
    CHECK("learn result", REG(v0), 5);
    CHECK("learn n", s_nlearnt, 3);
    CHECK("learn inner starts", s_learnt[0].vram, 0x80002000u);
    CHECK("learn inner start len", s_learnt[0].len, 0);
    CHECK("learn inner", s_learnt[1].vram, 0x80002000u);
    CHECK("learn inner len", s_learnt[1].len, 12);
    CHECK("learn inner caller", s_learnt[1].caller, ENTRY + 8);
    CHECK("learn outer", s_learnt[2].vram, ENTRY);
    CHECK("learn outer len", s_learnt[2].len, 28);
    CHECK("learn outer caller", s_learnt[2].caller, 0x80000500u);
}

/* mfc0 Count reads the runtime's counter, which runs (review 5b.4: a frozen shadow spun a delay loop) */
#define C0(sub, rt, rd) ((uint32_t)((16u << 26) | ((sub) << 21) | ((rt) << 16) | ((rd) << 11)))
static void t_count(void) {
    fresh();
    RUN(C0(0, t0, 9) /* mfc0 t0, Count */, C0(0, t1, 9), C0(4, t2, 9) /* mtc0 t2, Count */, C0(0, t3, 9), JR_RA, NOP);
    CHECK("Count runs", REG(t1) != REG(t0), 1);
    CHECK("mtc0 Count sets it", (uint32_t)REG(t3) - (uint32_t)REG(t2) <= 1000u, 1);
}

static void t_fpu(void) {
    fresh();
    C.r8 = 7, C.r9 = (gpr)(int32_t)0xC0300000u /* -2.75f */;
    RUN(C1(4, t0, 2) /* mtc1 f2 */, FT(W_, 0, 2, 4, 32) /* cvt.s.w f4 */, C1(4, t1, 6), FT(S_, 6, 4, 8, 0) /* add.s */,
        FT(S_, 6, 4, 10, 2) /* mul.s */, FT(S_, 0, 8, 12, 33) /* cvt.d.s */, FT(D_, 12, 12, 14, 0) /* add.d */,
        FT(S_, 0, 6, 16, 13) /* trunc.w.s */, C1(0, t2, 16) /* mfc1 */, FT(S_, 0, 6, 18, 14) /* ceil.w */,
        C1(0, t3, 18), FT(S_, 0, 6, 20, 15) /* floor.w */, C1(0, t4, 20), FT(S_, 4, 6, 0, 60) /* c.lt.s */,
        IT(17, 8, 1, 2) /* bc1t */, NOP, ADDIU(t5, zero, 1), FT(S_, 6, 4, 0, 60), IT(17, 8, 3, 2) /* bc1tl not taken */,
        ADDIU(t6, zero, 1), ADDIU(t7, zero, 1), C1(4, t0, 1) /* mtc1 f1: the odd half of f0 */, C1(0, s0, 1), JR_RA, NOP);
    CHECKF("cvt.s.w", C.f4.fl, 7.0f);
    CHECKF("add.s", C.f8.fl, 4.25f);
    CHECKF("mul.s", C.f10.fl, -19.25f);
    CHECKF("add.d", C.f14.d, 8.5);
    CHECK("trunc.w", REG(t2), G(-2));
    CHECK("ceil.w", REG(t3), G(-2));
    CHECK("floor.w", REG(t4), G(-3));
    CHECK("bc1t taken", REG(t5), 0);
    CHECK("bc1tl slot", REG(t6), 0);
    CHECK("after bc1tl", REG(t7), 1);
    CHECK("f1 is f0's high word", C.f0.u32h, 7);
    CHECK("mfc1 f1", REG(s0), 7);
#if XRC_FR
    fresh();   /* FR = 1 (recomp.h XRC_FODD): an odd FPR is a register of its own, for moves and .s alike */
    C.mips3_float_mode = 1;
    C.r8 = 0x40200000u /* 2.5f */;
    RUN(C1(4, t0, 3) /* mtc1 f3 */, FT(S_, 3, 3, 5, 0) /* add.s f5, f3, f3 */, C1(0, s0, 5) /* mfc1 f5 */, JR_RA, NOP);
    CHECKF("FR 1: mtc1 f3 is f3", C.f3.fl, 2.5f);
    CHECK("FR 1: f2 untouched", C.f2.u32h, 0);
    CHECK("FR 1: mfc1 f5 reads add.s's", REG(s0), 0x40A00000u);
#endif

    fresh();   /* the FCR31 rounding mode, round.w, the 64-bit moves and conversions */
    C.r8 = 0x40200000u /* 2.5f */, C.r9 = 1 /* toward zero */, C.r10 = (gpr)(int32_t)0xC0600000u /* -3.5f */;
    C.r11 = 0x80002000u;
    wr32(0x80002000, 0x40091EB8), wr32(0x80002004, 0x51EB851F);   /* 3.14 */
    RUN(C1(4, t0, 2), FT(S_, 0, 2, 4, 36) /* cvt.w.s nearest even */, C1(0, s0, 4), FT(S_, 0, 2, 4, 12) /* round.w */,
        C1(0, s1, 4), C1(4, t2, 6), FT(S_, 0, 6, 4, 36), C1(0, s2, 4), C1(6, t1, 31) /* ctc1 */,
        FT(S_, 0, 2, 4, 36), C1(0, s3, 4), C1(2, s4, 31) /* cfc1 */, C1(6, zero, 31),
        IT(53, t3, 8, 0) /* ldc1 f8 */, FT(D_, 0, 8, 10, 37) /* cvt.l.d */, C1(1, s5, 10) /* dmfc1 */,
        FT(L_, 0, 10, 12, 33) /* cvt.d.l */, IT(61, t3, 12, 8) /* sdc1 */, FT(D_, 0, 8, 14, 32) /* cvt.s.d */,
        FT(D_, 8, 8, 0, 50) /* c.eq.d */, IT(17, 8, 0, 2) /* bc1f */, NOP, ADDIU(s6, zero, 1), JR_RA, NOP);
    CHECK("cvt.w.s 2.5", REG(s0), 2);
    CHECK("round.w 2.5 (to even, as the VR4300)", REG(s1), XRC_ROUND_EVEN ? 2 : 3);
    CHECK("cvt.w.s -3.5", REG(s2), G(-4));
    CHECK("cvt.w.s toward zero", REG(s3), 2);
    CHECK("cfc1 rm", REG(s4), 1);
    CHECK("cvt.l.d", REG(s5), 3);
    CHECK("sdc1 hi", rd32(0x80002008), 0x40080000);
    CHECKF("cvt.s.d", C.f14.fl, 3.14f);
    CHECK("bc1f not taken", REG(s6), 1);
    set_cop1_cs(0);

    fresh();   /* FCSR.FS: a denormal single result flushed to zero (recomp.h XRC_FTZ), and cfc1 reads FS back */
    C.r8 = 0x0DA24260u /* 1e-30f */, C.r9 = 0x2EDBE6FFu /* 1e-10f */, C.r10 = 0x01000000u /* FS */;
    RUN(C1(6, t2, 31) /* ctc1 FS */, C1(4, t0, 2), C1(4, t1, 4), FT(S_, 4, 2, 6, 2) /* mul.s: 1e-40 */,
        C1(0, s0, 6), C1(2, s1, 31) /* cfc1 */, C1(6, zero, 31), FT(S_, 4, 2, 8, 2), C1(0, s2, 8), JR_RA, NOP);
    CHECK("FS: the denormal flushed", REG(s0), XRC_FTZ ? 0u : 0x000116C2u);
    CHECK("FS read back", REG(s1), XRC_FTZ ? 0x01000000u : 0u);
    CHECK("no FS: the denormal kept", REG(s2), 0x000116C2u);
    set_cop1_cs(0);
}

static void t_misc(void) {
    fresh();   /* ll/sc, syscall (returns, as the generated code does), traps */
    C.r16 = 0x80002000u;
    wr32(0x80002000, 41);
    RUN(IT(48, s0, t0, 0) /* ll */, ADDIU(t0, t0, 1), IT(56, s0, t0, 0) /* sc */, RT(52, t0, t0, 0, 0) /* teq: taken */,
        RT(54, t0, t0, 0, 0) /* tne: not */, RT(12, 0, 0, 0, 0) /* syscall */, ADDIU(t5, zero, 1), JR_RA, NOP);
    CHECK("sc ok", REG(t0), 1);
    CHECK("sc stored", rd32(0x80002000), 42);
    CHECK("traps", s_breaks, 1);
    CHECK("syscall", s_syscalls, 1);
    CHECK("syscall v0", REG(v0), 0x5C);
    CHECK("syscall returns", REG(t5), 0);

    fresh();   /* COP0: Status through the runtime, EntryHi a shadow, tlbp misses */
    C.r8 = 0x1234, C.r13 = 0xFF01;
    RUN(IT(16, 4, t0, 10 << 11) /* mtc0 t0, EntryHi */, IT(16, 0, t1, 10 << 11) /* mfc0 */, 0x42000008u /* tlbp */,
        IT(16, 0, t2, 0) /* mfc0 Index */, 0x42000002u /* tlbwi */, IT(16, 4, t5, 12 << 11) /* mtc0 Status */,
        IT(16, 0, t4, 12 << 11), JR_RA, NOP);
    CHECK("EntryHi", REG(t1), 0x1234);
    CHECK("tlbp", REG(t2), G(0x80000000u));
    CHECK("Status", C.status_reg, 0xFF01);
    CHECK("mfc0 Status", REG(t4), 0xFF01);

    fresh();   /* the TLB (tlb.c): osMapTLB's sequence maps two 4 KB pages at 0x04000000, tlbp and
                * tlbr read the entry back, osUnmapTLB's sequence takes the aliases away */
    s_nalias = s_nunalias = 0;
    C.r8 = 3, C.r9 = 0x04000000u, C.r10 = ((0x4000u >> 12) << 6) | 0x1F, C.r11 = ((0x5000u >> 12) << 6) | 0x1F;
    RUN(IT(16, 4, t0, 0 << 11) /* mtc0 Index */, IT(16, 4, zero, 5 << 11) /* PageMask 4 KB */,
        IT(16, 4, t1, 10 << 11) /* EntryHi */, IT(16, 4, t2, 2 << 11) /* EntryLo0 */, IT(16, 4, t3, 3 << 11),
        0x42000002u /* tlbwi */, IT(16, 4, zero, 2 << 11), IT(16, 4, zero, 0 << 11),
        LUI(t4, 0x0400), ORI(t4, t4, 0x1234), IT(16, 4, t4, 10 << 11), 0x42000008u /* tlbp */,
        IT(16, 0, t5, 0 << 11), 0x42000001u /* tlbr */, IT(16, 0, t6, 3 << 11), JR_RA, NOP);
    CHECK("tlb aliases", s_nalias, 2);
    CHECK("tlb even host", s_alias[0][0], (uint32_t)(uintptr_t)(xrc_rdram + 0x04000000u));
    CHECK("tlb even phys", s_alias[0][1], ((uint32_t)(uintptr_t)s_ram & 0x7FFFFFFFu) + 0x4000);
    CHECK("tlb odd host", s_alias[1][0], (uint32_t)(uintptr_t)(xrc_rdram + 0x04001000u));
    CHECK("tlb odd phys", s_alias[1][1], ((uint32_t)(uintptr_t)s_ram & 0x7FFFFFFFu) + 0x5000);
    CHECK("tlb page", s_alias[1][2], 0x1000);
    CHECK("tlbp", REG(t5), 3);
    CHECK("tlbr", REG(t6), C.r11);
    CHECK("tlb to_phys", xrc_tlb_to_phys(0x04001234u), 0x5234);
    CHECK("tlb to_phys miss", xrc_tlb_to_phys(0x04002000u), 0xFFFFFFFFu);
    C.r9 = 0x80006000u, C.r10 = C.r11 = 1;
    RUN(IT(16, 4, t0, 0 << 11), IT(16, 4, t1, 10 << 11), IT(16, 4, t2, 2 << 11), IT(16, 4, t3, 3 << 11),
        0x42000002u, JR_RA, NOP);
    CHECK("tlb unaliases", s_nunalias, 2);
    CHECK("tlb unalias host", s_unalias[1][0], (uint32_t)(uintptr_t)(xrc_rdram + 0x04001000u));
    CHECK("tlb unmapped", xrc_tlb_to_phys(0x04001234u), 0xFFFFFFFFu);

    fresh();   /* an instruction it does not run stops with its address */
    RUN(NOP, 0x4A000000u /* COP2 */, JR_RA, NOP);
    CHECK("stop", s_fatal != NULL, 1);
    CHECK("stop pc", s_fatal_pc, ENTRY + 4);
}

/* a TLB miss (tlb.c xrc_tlb_miss): the game's refill vector at 0x80000000 runs
 * with Context set, fills EntryLo0/1 from its table and tlbwr; eret returns */
int xrc_interp_refill(uint32_t vaddr, int store);
uint32_t xu_self_sp(void) { return 0x8000F000u; }
static void t_refill(void) {
    static const uint32_t vec[] = {
        IT(16, 0, k0, 4 << 11) /* mfc0 k0, Context */, LUI(k1, 0x8000), ORI(k1, k1, 0x8000),
        IT(43, k1, k0, 0x10) /* sw k0, 0x10(k1) */, IT(35, k1, k1, 0) /* lw k1, 0(k1) */,
        IT(16, 4, k1, 2 << 11) /* mtc0 EntryLo0 */, ADDIU(k1, k1, 0x40), IT(16, 4, k1, 3 << 11) /* EntryLo1 */,
        IT(16, 4, zero, 5 << 11) /* PageMask */, 0x42000006u /* tlbwr */, 0x42000018u /* eret */, NOP};
    static const uint32_t preamble[] = {LUI(k0, 0x8000), ADDIU(k0, k0, 0x1000), RT(8, k0, 0, 0, 0) /* jr k0 */, NOP};
    fresh();
    s_nalias = 0;
    memset(s_ram, 0, 0x80);
    CHECK("refill: no vector", xrc_interp_refill(0x7F002000u, 0), 0);
    put(0x80000000u, vec, (int)(sizeof vec / 4));
    wr32(0x80008000u, ((0x6000u >> 12) << 6) | 0x1F);
    CHECK("refill: eret", xrc_interp_refill(0x7F002345u, 0), 1);
    CHECK("refill: Context", rd32(0x80008010u) & 0x7FFFF0u, (0x7F002345u >> 9) & 0x7FFFF0u);
    CHECK("refill: even page", xrc_tlb_to_phys(0x7F002345u), 0x6345);
    CHECK("refill: odd page", xrc_tlb_to_phys(0x7F003010u), 0x7010);
    CHECK("refill: aliased", s_nalias, 2);
    CHECK("refill: host", s_alias[0][0], (uint32_t)(uintptr_t)(xrc_rdram + 0x7F002000u));
    put(0x80000000u, preamble, 4);   /* libultra's preamble: into __osException (here an eret) */
    wr32(0x80001000u, 0x42000018u);
    CHECK("refill: through the preamble", xrc_interp_refill(0x7F004000u, 1), 1);
    CHECK("refill: nothing mapped", xrc_tlb_to_phys(0x7F004000u), 0xFFFFFFFFu);
    CHECK("refill: Status untouched", C.status_reg, 0);
    memset(s_ram, 0, 0x80);
}

/* hand-written code: a routine with one frame calls another interpreted one
 * that shares it, whose error path takes the caller's epilogue and so returns
 * from both (Perfect Dark's 0x70018680 and 0x70019634) */
static void t_inline(void) {
    enum { FRAME = 0x800, RA_AT = 0x7DC, EPI = 0x80001020u };
    static const uint32_t caller[] = {
        ADDIU(sp, sp, -FRAME), IT(43, sp, ra, RA_AT) /* sw ra */, JT(3, SUB_AT) /* jal */, NOP,
        ORI(v0, v0, 1) /* only when the callee came back */, NOP, NOP, NOP,
        /* EPI */ IT(35, sp, ra, RA_AT) /* lw ra */, JR_RA, ADDIU(sp, sp, FRAME)};
    static const uint32_t leave[] = {ORI(v1, zero, 7), JT(2, EPI) /* j the caller's epilogue */, NOP};
    static const uint32_t back[] = {ORI(v1, zero, 9), JR_RA, NOP};
    static const uint32_t sys[] = {0x0000000Cu /* syscall */, NOP};
    static const uint32_t tail[] = {LUI(t9, 0x8000), ORI(t9, t9, 0x3000), RT(8, t9, 0, 0, 0) /* jr t9 */, NOP};
    fresh();
    put(ENTRY, caller, (int)(sizeof caller / 4));
    put(SUB_AT, leave, 3);
    xrc_interp(xrc_rdram, &C, ENTRY);
    CHECK("inline: the callee ran", REG(v1), 7);
    CHECK("inline: both left", REG(v0), 0);
    CHECK("inline: one frame popped", REG(sp), G(0x8000F000u));
    CHECK("inline: the caller's ra", REG(ra), G(0x80000ABCu));
    CHECK("inline: no break", s_breaks, 0);
    fresh();
    put(SUB_AT, back, 3);
    xrc_interp(xrc_rdram, &C, ENTRY);
    CHECK("inline: a plain return", REG(v1), 9);
    CHECK("inline: the caller goes on", REG(v0), 1);
    CHECK("inline: its frame popped", REG(sp), G(0x8000F000u));
    fresh();   /* the callee ends in a tail call to recompiled code: the caller still goes on (Bomberman 64 2) */
    C.r4 = 5;
    put(SUB_AT, tail, 4);
    xrc_interp(xrc_rdram, &C, ENTRY);
    CHECK("inline: the tail call ran, then the caller", REG(v0), 15 | 1);
    CHECK("inline: its frame popped after a tail call", REG(sp), G(0x8000F000u));
    CHECK("inline: the caller's ra after a tail call", REG(ra), G(0x80000ABCu));
    fresh();   /* the callee is a syscall its handler has run: the caller still goes on (Banjo-Tooie's linker) */
    put(SUB_AT, sys, 2);
    xrc_interp(xrc_rdram, &C, ENTRY);
    CHECK("inline: after a syscall, the caller", REG(v0) & 1, 1);
    CHECK("inline: its frame popped after a syscall", REG(sp), G(0x8000F000u));
    CHECK("inline: the caller's ra after a syscall", REG(ra), G(0x80000ABCu));
    memset(s_ram + 0x1000, 0, sizeof caller);
    memset(s_ram + (SUB_AT & 0xFFFF), 0, sizeof tail);
}

/* a run keeps ra and sp in memory and calls; below it, a run loads them back and returns: the first run ends there
 * (Mortal Kombat 4's objects). XRC_SP_ESCAPE */
static void t_sp_escape(void) {
    static const uint32_t saver[] = {
        ADDIU(sp, sp, -0x18), LUI(t0, 0x8000), ORI(t0, t0, 0x6000), IT(43, t0, ra, 0), IT(43, t0, sp, 8),
        (3u << 26) | ((ESC_SUB >> 2) & 0x3FFFFFFu), NOP,
        ORI(v0, 0, 99),   /* not reached: the exit returned for it */
        JR_RA, ADDIU(sp, sp, 0x18)};
    static const uint32_t leaver[] = {
        LUI(t0, 0x8000), ORI(t0, t0, 0x6000), IT(35, t0, sp, 8), IT(35, t0, ra, 0), ORI(v0, 0, 7), JR_RA, ADDIU(sp, sp, 0x18)};
    fresh();
    s_sw_n = 0, s_esc_calls = 0;
    put(ESC_EXIT, leaver, 7);
    run_prog(saver, 10);
    CHECK("exit: the function below ran once and did not go on", s_esc_calls, 1);
    CHECK("exit: not taken as a change of context", s_sw_n, 0);
    CHECK("exit: the saving run returned, with the exit's v0", REG(v0), 7);
    CHECK("exit: its stack back", REG(sp), G(0x8000F000u));
    CHECK("exit: the caller's ra", REG(ra), G(0x80000ABCu));
    wr32(0x80006000u, 0), wr32(0x80006008u, 0);
    memset(s_ram + (ESC_EXIT & 0xFFFF), 0, sizeof leaver);
}

static void t_stack_switch(void) {
    static const uint32_t lw_sp[] = {LUI(t0, 0x8000), ORI(t0, t0, 0x6000), IT(35, t0, sp, 0) /* lw sp */, IT(35, t0, t9, 4),
                                     RT(8, t9, 0, 0, 0) /* jr t9 */, ADDIU(sp, sp, 0x10)};
    static const uint32_t own[] = {LUI(t0, 0x8000), ORI(t0, t0, 0x6000), IT(35, t0, sp, 0), JR_RA, NOP};
    fresh();
    s_sw_n = 0;
    wr32(0x80006000u, 0x80008000u), wr32(0x80006004u, 0x80003000u);
    run_prog(lw_sp, 6);
    CHECK("switch: taken once", s_sw_n, 1);
    CHECK("switch: to the address the routine jumped to", s_sw_target, 0x80003000u);
    CHECK("switch: with the sp after its delay slot", s_sw_sp, 0x80008010u);
    CHECK("switch: this run's own stack", s_sw_mine, 0x8000F000u);
    CHECK("switch: back, with the registers of the context that came back", REG(v0), 77);
    CHECK("switch: back on its stack", REG(sp), G(0x8000F000u));
    CHECK("switch: the caller's ra", REG(ra), G(0x80000ABCu));
    fresh();   /* sp loaded back to what it is: a return */
    s_sw_n = 0;
    wr32(0x80006000u, 0x8000F000u);
    run_prog(own, 5);
    CHECK("switch: not to its own stack", s_sw_n, 0);
    CHECK("switch: returned", REG(sp), G(0x8000F000u));
    wr32(0x80006000u, 0), wr32(0x80006004u, 0);
}

static void t_tail_guard(void) {
    static const uint32_t tail[] = {LUI(t9, 0x8000), ORI(t9, t9, 0x3000), RT(8, t9, 0, 0, 0) /* jr t9 */, NOP};
    static const uint32_t there[] = {ORI(v1, zero, 5), JR_RA, NOP};
    put(0x80003000u, there, 3);
    fresh();   /* a function there that is what RDRAM holds: a tail call to it */
    s_guard_stale = 0;
    C.r4 = 5;
    run_prog(tail, 4);
    CHECK("tail guard: the function ran", REG(v0), 15);
    CHECK("tail guard: not the bytes", REG(v1), 0);
    fresh();   /* other code there now: the code goes on, in line */
    s_guard_stale = 1;
    C.r4 = 5;
    run_prog(tail, 4);
    CHECK("tail guard: not the stale function", REG(v0), 0);
    CHECK("tail guard: the bytes RDRAM holds", REG(v1), 5);
    s_guard_stale = 0;
    memset(s_ram + 0x3000, 0, sizeof there);
}

int main(void) {
    xrc_rdram = s_ram - 0x80000000u;
    t_inline();
    t_stack_switch();
    t_sp_escape();
    t_tail_guard();
    t_int();
    t_64();
    t_branch();
    t_count();
    t_fpu();
    t_misc();
    t_refill();
    printf("test_interp (%s registers): %s\n", XRC_GPR32 ? "32-bit" : "64-bit", s_fail ? "FAILED" : "passed");
    return s_fail != 0;
}
