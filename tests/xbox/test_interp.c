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
void do_break(uint32_t vram) { s_breaks++, s_fatal_pc = vram; }
void recomp_syscall_handler(uint8_t* rdram, recomp_context* ctx, int32_t vram) { (void)rdram, (void)vram, ctx->r2 = 0x5C, s_syscalls++; }
void pause_self(uint8_t* rdram) { (void)rdram; xrc_fatal_vram("pause_self", 0, 0); }

static void known_fn(uint8_t* rdram, recomp_context* ctx) { (void)rdram; ctx->r2 = (gpr)(ctx->r4 * 3); }
recomp_func_t* xrc_try_function(uint32_t vram) { return vram == 0x80003000u ? known_fn : NULL; }

static struct { uint32_t vram, len, caller; } s_learnt[8];
static int s_nlearnt;
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
    C.f_odd = &C.f0.u32h;
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
    CHECK("round.w 2.5 (lround, as upstream)", REG(s1), 3);
    CHECK("cvt.w.s -3.5", REG(s2), G(-4));
    CHECK("cvt.w.s toward zero", REG(s3), 2);
    CHECK("cfc1 rm", REG(s4), 1);
    CHECK("cvt.l.d", REG(s5), 3);
    CHECK("sdc1 hi", rd32(0x80002008), 0x40080000);
    CHECKF("cvt.s.d", C.f14.fl, 3.14f);
    CHECK("bc1f not taken", REG(s6), 1);
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

    fresh();   /* an instruction it does not run stops with its address */
    RUN(NOP, 0x4A000000u /* COP2 */, JR_RA, NOP);
    CHECK("stop", s_fatal != NULL, 1);
    CHECK("stop pc", s_fatal_pc, ENTRY + 4);
}

int main(void) {
    xrc_rdram = s_ram - 0x80000000u;
    t_int();
    t_64();
    t_branch();
    t_fpu();
    t_misc();
    printf("test_interp (%s registers): %s\n", XRC_GPR32 ? "32-bit" : "64-bit", s_fail ? "FAILED" : "passed");
    return s_fail != 0;
}
