/* test_mmio.c - the RCP register model (xbox/src/rt/mmio.c) through the
 * interpreter (xbox/src/rt/interp.c), on the host: small programs that reach
 * KSEG1 RDRAM, the PI, SI, AI, VI, SP and MI registers, the cartridge and
 * RDRAM past its end, with the runtime paths the model calls recorded.
 * Built by tools/xbox/test_mmio.py. Exit code 0 = passed. */
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include "rt.h"
#include "xhw.h"
#include "ultra/ultra.h"

/* ---- the runtime, stubbed and recorded ---- */
static uint8_t s_ram[0x10000];
uint8_t* xrc_rdram;
uint8_t* xrc_rdram_host = s_ram;
uint32_t xrc_rdram_size = sizeof s_ram;
static jmp_buf s_jb;
static const char* s_fatal;
static uint32_t s_ev, s_evfirst;   /* events posted, a bit each */
static uint32_t s_dma[3], s_si[2], s_ai[2], s_fb, s_gfx, s_aud, s_rate, s_pages;

void xhw_log(const char* s) { (void)s; }
void xhw_logf(const char* fmt, ...) { (void)fmt; }
uint64_t xhw_time_us(void) { return 0; }
void* xhw_alias_page(uint32_t va) { (void)va, s_pages++; return NULL; }
int xhw_alias_map(uint32_t va, uint32_t pa, uint32_t bytes) { (void)va, (void)pa, (void)bytes; return 1; }
void xhw_alias_unmap(uint32_t va, uint32_t bytes) { (void)va, (void)bytes; }
void xrc_fatal_vram(const char* what, uint32_t vram, uint32_t extra) {
    (void)vram, (void)extra;
    s_fatal = what;
    longjmp(s_jb, 1);
}
void cop0_status_write(recomp_context* ctx, gpr value) { ctx->status_reg = (uint32_t)value; }
gpr cop0_status_read(recomp_context* ctx) { return ctx->status_reg; }
void do_break(uint32_t vram) { (void)vram; }
void recomp_syscall_handler(uint8_t* rdram, recomp_context* ctx, int32_t vram) { (void)rdram, (void)ctx, (void)vram; }
void pause_self(uint8_t* rdram) { (void)rdram; }
recomp_func_t* xrc_try_function(uint32_t vram) { (void)vram; return NULL; }
void xrc_learn_func(uint32_t vram, uint32_t len, uint32_t caller) { (void)vram, (void)len, (void)caller; }
void xrc_trace_vram(uint32_t vram) { (void)vram; }
void xhw_sleep_ms(uint32_t ms) { (void)ms; }
int xu_fault_wait(uint32_t vaddr, int store) { (void)vaddr; (void)store; return 0; }   /* ultra/thread.c */
uint32_t xu_self_sp(void) { return 0; }
void xhw_crash_set_fixup(int (*fn)(uint32_t host, int write)) { (void)fn; }
void xrc_learn_kuseg(uint32_t va, int aliased) { (void)va, (void)aliased; }

void xu_post_event(int e) { s_ev |= 1u << e; }
void xu_post_event_first(int e) { s_evfirst |= 1u << e; }
uint32_t xu_pi_word(uint32_t dev) { return 0x12345678u ^ dev; }
uint32_t xu_pi_dma(uint32_t dev, uint32_t dram, uint32_t n) { s_dma[0] = dev, s_dma[1] = dram, s_dma[2] = n; return 0; }
int xu_pi_save_dma(uint32_t addr, uint32_t dram, uint32_t n, int write) { (void)addr, (void)dram, (void)n, (void)write; return 0; }
int xu_pi_save_io(uint32_t addr, uint32_t* word, int write) { (void)addr, (void)word, (void)write; return 0; }
void xrc_gfx_task(uint32_t task) { s_gfx = task; }
void xrc_audio_task(uint32_t task) { s_aud = task; }
#undef ARG
#define ARG(i) ((uint32_t)ctx->r##i)
void osAiSetNextBuffer_recomp(uint8_t* rdram, recomp_context* ctx) { (void)rdram, s_ai[0] = ARG(4), s_ai[1] = ARG(5); }
void osAiGetLength_recomp(uint8_t* rdram, recomp_context* ctx) { (void)rdram, ctx->r2 = 0x100; }
void osAiSetFrequency_recomp(uint8_t* rdram, recomp_context* ctx) { (void)rdram, s_rate = ARG(4); }
void osViSwapBuffer_recomp(uint8_t* rdram, recomp_context* ctx) { (void)rdram, s_fb = ARG(4); }
void __osSiRawStartDma_recomp(uint8_t* rdram, recomp_context* ctx) { (void)rdram, s_si[0] = ARG(4), s_si[1] = ARG(5); }

void xrc_interp(uint8_t* rdram, recomp_context* ctx, uint32_t entry);

/* ---- an assembler, just enough ---- */
enum { zero, at, v0, v1, a0, a1, a2, a3, t0, t1, t2, t3, t4, t5, t6, t7, s0, s1, s2, s3 };
#define IT(op, rs, rt, imm) ((uint32_t)(((uint32_t)(op) << 26) | ((rs) << 21) | ((rt) << 16) | ((imm) & 0xFFFF)))
#define JR_RA ((uint32_t)((31u << 21) | 8u))
#define LUI(t, i) IT(15, 0, t, i)
#define ORI(t, s, i) IT(13, s, t, i)
#define LW(t, s, i) IT(35, s, t, i)
#define SW(t, s, i) IT(43, s, t, i)
#define LBU(t, s, i) IT(36, s, t, i)
#define LHU(t, s, i) IT(37, s, t, i)
#define LI(t, v) LUI(t, (uint32_t)(v) >> 16), ORI(t, t, (v) & 0xFFFF)

static recomp_context C;
static int s_fail;
#define ENTRY 0x80001000u
#define REG(n) ((uint32_t)((gpr*)&C.r0)[n])
static uint32_t* ram(uint32_t a) { return (uint32_t*)(s_ram + (a & 0xFFFF)); }

static int run_prog(const uint32_t* w, int n) {
    memset(&C, 0, sizeof C);
    C.f_odd = &C.f0.u32h;
    C.r31 = (gpr)(int32_t)0x80000ABCu;
    memcpy(ram(ENTRY), w, 4 * (size_t)n);
    s_fatal = NULL;
    if (setjmp(s_jb)) return 0;
    xrc_interp(xrc_rdram, &C, ENTRY);
    return 1;
}
#define RUN(...) do { static const uint32_t p_[] = {__VA_ARGS__}; run_prog(p_, (int)(sizeof p_ / 4)); } while (0)
#define CHECK(name, got, want) do { uint32_t g_ = (uint32_t)(got), w_ = (uint32_t)(want); \
    if (g_ != w_) { printf("  %s: got %08x, want %08x\n", name, g_, w_); s_fail++; } } while (0)

int main(void) {
    xrc_rdram = s_ram - 0x80000000u;
    xrc_mmio_init();
    CHECK("pages asked for", s_pages, 13);

    /* KSEG1 RDRAM is KSEG0's bytes; past RDRAM reads 0 and keeps nothing */
    RUN(LI(t0, 0xA0002000u), LI(t1, 0xCAFEF00Du), SW(t1, t0, 4), LI(t2, 0x80002000u), LW(t3, t2, 4),
        LI(t4, 0xA0400000u), SW(t1, t4, 0), LW(t5, t4, 0), JR_RA, 0);
    CHECK("KSEG1 store, KSEG0 load", REG(t3), 0xCAFEF00Du);
    CHECK("past RDRAM", REG(t5), 0);
    CHECK("no stop", s_fatal == NULL, 1);

    /* PI: idle status, a ROM DMA by PI_WR_LEN, the event */
    RUN(LI(t0, 0xA4600000u), LW(t1, t0, 0x10), LI(t2, 0x3000), SW(t2, t0, 0), LI(t2, 0x10001000u), SW(t2, t0, 4),
        ORI(t2, zero, 0xFF), SW(t2, t0, 0xC), JR_RA, 0);
    CHECK("PI_STATUS", REG(t1), 0);
    CHECK("PI dev", s_dma[0], 0x10001000u);
    CHECK("PI dram", s_dma[1], 0x80003000u);   /* physical, passed as KSEG0: never through a TLB */
    CHECK("PI len", s_dma[2], 0x100u);
    CHECK("PI event", (s_ev >> OS_EVENT_PI) & 1, 1);

    /* SI: the PIF block written, read back */
    RUN(LI(t0, 0xA4800000u), LI(t1, 0x4000), SW(t1, t0, 0), SW(zero, t0, 0x10), LW(t2, t0, 0x18), JR_RA, 0);
    CHECK("SI dir", s_si[0], 1);
    CHECK("SI dram", s_si[1], 0x80004000u);
    CHECK("SI_STATUS", REG(t2), 0);
    RUN(LI(t0, 0xA4800000u), SW(zero, t0, 4), JR_RA, 0);
    CHECK("SI read dir", s_si[0], 0);

    /* AI: a buffer, the rate (NTSC), the length */
    *ram(0x300) = 1;
    RUN(LI(t0, 0xA4500000u), LI(t1, 0x5000), SW(t1, t0, 0), LI(t1, 0x1000), SW(t1, t0, 4), LW(t2, t0, 4),
        LW(t3, t0, 0xC), ORI(t1, zero, 2207), SW(t1, t0, 0x10), JR_RA, 0);
    CHECK("AI buf", s_ai[0], 0x80005000u);
    CHECK("AI len", s_ai[1], 0x1000u);
    CHECK("AI_LEN", REG(t2), 0x100u);
    CHECK("AI_STATUS busy", REG(t3), 0x40000000u);
    CHECK("AI rate", s_rate, 48681812u / 2208u);

    /* VI: the origin less one line is the framebuffer */
    RUN(LI(t0, 0xA4400000u), ORI(t1, zero, 0x3202), SW(t1, t0, 0), ORI(t1, zero, 320), SW(t1, t0, 8),
        LI(t1, 0x00100280u), SW(t1, t0, 4), LW(t2, t0, 4), JR_RA, 0);
    CHECK("VI fb", s_fb, 0x80100000u);
    CHECK("VI_ORIGIN reads back", REG(t2), 0x00100280u);

    /* SP: the task block DMA'd to DMEM 0xFC0, halt cleared: the task runs */
    *ram(0x6000) = 1;   /* M_GFXTASK */
    *ram(0x6004) = 0x1234;
    RUN(LI(t0, 0xA4040000u), ORI(t1, zero, 0xFC0), SW(t1, t0, 0), LI(t1, 0x6000), SW(t1, t0, 4), ORI(t1, zero, 0x3F),
        SW(t1, t0, 8), LI(t2, 0xA4000FC0u), LW(t3, t2, 4), ORI(t1, zero, 0x125), SW(t1, t0, 0x10), LW(t4, t0, 0x10),
        LW(t5, t0, 0x1C), LW(t6, t0, 0x1C), JR_RA, 0);
    CHECK("DMEM word", REG(t3), 0x1234u);
    CHECK("gfx task", s_gfx, 0x80006000u);
    CHECK("SP done events", (s_evfirst >> OS_EVENT_SP) & (s_evfirst >> OS_EVENT_DP) & 1, 1);
    CHECK("SP_STATUS after", REG(t4) & 0x203u, 0x203u);
    CHECK("semaphore first", REG(t5), 0);
    CHECK("semaphore then", REG(t6), 1);

    /* MI: the version, the mask's clear/set pairs; bytes and halves big-endian */
    RUN(LI(t0, 0xA4300000u), LW(t1, t0, 4), ORI(t2, zero, 0xAA), SW(t2, t0, 0xC), ORI(t2, zero, 0x01),
        SW(t2, t0, 0xC), LW(t3, t0, 0xC), LBU(t4, t0, 7), LHU(t5, t0, 4), LBU(t6, t0, 4), JR_RA, 0);
    CHECK("MI_VERSION", REG(t1), 0x02020102u);
    CHECK("MI_INTR_MASK", REG(t3), 0xEu);
    CHECK("lbu", REG(t4), 0x02u);
    CHECK("lhu", REG(t5), 0x0202u);
    CHECK("lbu hi", REG(t6), 0x02u);

    /* the cartridge through KSEG1 */
    RUN(LI(t0, 0xB0000010u), LW(t1, t0, 0), JR_RA, 0);
    CHECK("ROM word", REG(t1), 0x12345678u ^ 0x10000010u);

    /* a register access it does not model stops, named */
    RUN(LI(t0, 0xA4600000u), IT(34, t0, t1, 0x10) /* lwl */, JR_RA, 0);
    CHECK("lwl stops", s_fatal != NULL, 1);

    printf("test_mmio: %s\n", s_fail ? "FAILED" : "passed");
    return s_fail ? 1 : 0;
}
