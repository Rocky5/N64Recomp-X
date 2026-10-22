/* test_recomp_h.c - our recomp.h against upstream's, on the host (PLAN.md 3.2).
 *
 * One program, built by tools/xbox/test_recomp_h.py against either header
 * (-I picks it): it drives every macro and helper the generated code uses
 * with pseudo-random inputs from a fixed seed and prints one line per
 * result. The .py compares the lines: upstream (x86-64) against ours on
 * x86-64 and on i686 (the Pentium III's ISA: SSE1 floats, x87 doubles), and
 * ours with 32-bit registers against ours with 64-bit ones on the cases
 * whose inputs are sign-extended 32-bit values (all an O32 game makes).
 *
 * Covered: ADD32/SUB32/SIGNED, MEM_W/H/B/HU/BU loads and stores, SD/LD,
 * lwl/lwr/swl/swr, ldl/ldr/sdl/sdr (64-bit registers only), the RDRAM image
 * after every store sequence, DMULT/DMULTU/DDIV/DDIVU, the conversions
 * under all four rounding modes (set_cop1_cs / get_cop1_cs), the truncations,
 * round.w (to the nearest, a half to even, as the VR4300; upstream's
 * lroundf rounds a half away, so the reference is lrintf under mode 0). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef XRC_TEST_UPSTREAM
#include <math.h>
#endif
#include "recomp.h"

#if defined(XRC_RECOMP_H) && XRC_GPR32
#define HAS_GPR64 0
#define WIDTH "gpr32"
#else
#define HAS_GPR64 1
#define WIDTH "gpr64"
#endif

#define MEM (1u << 20)
static uint8_t* s_buf;
static uint8_t* rdram;

static uint64_t s_rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) {
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 7;
    s_rng ^= s_rng << 17;
    return s_rng;
}
/* a register value as an O32 game makes them: sign-extended 32 bits */
static gpr r32(void) { return (gpr)(int32_t)(uint32_t)rnd(); }
static uint32_t addr(uint32_t align) { return 0x80000000u + ((uint32_t)rnd() % (MEM - 64)) / align * align; }

static uint32_t image_hash(void) {
    uint32_t h = 2166136261u, i;
    for (i = 0; i < MEM; i++) h = (h ^ s_buf[i]) * 16777619u;
    return h;
}

static void ints(void) {
    int i;
    for (i = 0; i < 2000; i++) {
        gpr a = r32(), b = r32();
        printf("32 add %08x %08x -> %08x\n", (uint32_t)a, (uint32_t)b, (uint32_t)ADD32(a, b));
        printf("32 sub %08x %08x -> %08x\n", (uint32_t)a, (uint32_t)b, (uint32_t)SUB32(a, b));
        printf("32 slt %08x %08x -> %d\n", (uint32_t)a, (uint32_t)b, SIGNED(a) < SIGNED(b));
        printf("32 sltu %08x %08x -> %d\n", (uint32_t)a, (uint32_t)b, (uint32_t)a < (uint32_t)b);
    }
    for (i = 0; i < 2000; i++) {
        uint64_t a = rnd(), b = rnd(), lo, hi;
        int64_t sl, sh, q, r;
        DMULTU(a, b, &lo, &hi);
        printf("64 dmultu %016llx %016llx -> %016llx %016llx\n", (unsigned long long)a, (unsigned long long)b,
               (unsigned long long)lo, (unsigned long long)hi);
        DMULT((int64_t)a, (int64_t)b, &sl, &sh);
        printf("64 dmult -> %016llx %016llx\n", (unsigned long long)sl, (unsigned long long)sh);
        if (b) {
            DDIVU(a, b, &lo, &hi);
            printf("64 ddivu -> %016llx %016llx\n", (unsigned long long)lo, (unsigned long long)hi);
            DDIV((int64_t)a, (int64_t)b, &q, &r);
            printf("64 ddiv -> %016llx %016llx\n", (unsigned long long)q, (unsigned long long)r);
        }
    }
    {
        int64_t q, r;
        DDIV((int64_t)0x8000000000000000ull, -1, &q, &r);
        printf("64 ddiv min/-1 -> %016llx %016llx\n", (unsigned long long)q, (unsigned long long)r);
    }
}

static void memory(void) {
    int i;
    memset(s_buf, 0, MEM);
    for (i = 0; i < 4000; i++) {
        gpr base = (gpr)(int32_t)addr(1), v = r32();
        int16_t off = (int16_t)(rnd() & 0x1F);
        uint32_t w = addr(4), h = addr(2), b = addr(1);
        switch (rnd() % 6) {
        case 0: MEM_W(0, (gpr)(int32_t)w) = (int32_t)v; break;
        case 1: MEM_H(0, (gpr)(int32_t)h) = (int16_t)v; break;
        case 2: MEM_B(0, (gpr)(int32_t)b) = (int8_t)v; break;
        case 3: do_swl(rdram, off, base, v); break;
        case 4: do_swr(rdram, off, base, v); break;
        case 5: {
            uint32_t d = addr(8);
            SD((uint64_t)v << 32 | (uint32_t)r32(), 0, (gpr)(int32_t)d);
            break;
        }
        }
        if (i % 500 == 499) printf("32 image %08x\n", image_hash());
    }
    for (i = 0; i < 3000; i++) {
        uint32_t w = addr(4), h = addr(2), b = addr(1), u = addr(1);
        gpr init = r32();
        printf("32 lw %08x -> %08x\n", w, (uint32_t)MEM_W(0, (gpr)(int32_t)w));
        printf("32 lh %08x -> %08x lhu %08x\n", h, (uint32_t)(int32_t)MEM_H(0, (gpr)(int32_t)h), (uint32_t)MEM_HU(0, (gpr)(int32_t)h));
        printf("32 lb %08x -> %08x lbu %08x\n", b, (uint32_t)(int32_t)MEM_B(0, (gpr)(int32_t)b), (uint32_t)MEM_BU(0, (gpr)(int32_t)b));
        printf("32 lwl %08x %08x -> %08x\n", u, (uint32_t)init, (uint32_t)do_lwl(rdram, init, 0, (gpr)(int32_t)u));
        printf("32 lwr %08x %08x -> %08x\n", u, (uint32_t)init, (uint32_t)do_lwr(rdram, init, 0, (gpr)(int32_t)u));
    }
    printf("32 image end %08x\n", image_hash());
}

/* the doubleword forms: 64-bit registers only (an O32 game never uses them);
 * last, so the 32-bit build's random stream stays in step until here */
static void memory64(void) {
#if HAS_GPR64
    {
        int i;
        for (i = 0; i < 3000; i++) {
            uint32_t d = addr(8), u = addr(1), u2 = addr(1);
            uint64_t i64 = rnd(), v2 = rnd();
            printf("64 ld %08x -> %016llx\n", d, (unsigned long long)LD(0, (gpr)(int32_t)d));
            printf("64 ldl %08x -> %016llx\n", u, (unsigned long long)do_ldl(rdram, (gpr)i64, 0, (gpr)(int32_t)u));
            printf("64 ldr %08x -> %016llx\n", u, (unsigned long long)do_ldr(rdram, (gpr)i64, 0, (gpr)(int32_t)u));
            do_sdl(rdram, 0, (gpr)(int32_t)u, (gpr)i64);
            do_sdr(rdram, 0, (gpr)(int32_t)u2, (gpr)v2);
        }
        printf("64 image %08x\n", image_hash());
    }
#endif
}

static float rf(void) { return (float)((int32_t)(uint32_t)rnd() >> 3) / 256.0f + 0.5f * (float)(rnd() & 1); }
static double rd(void) { return (double)((int64_t)rnd() >> 24) / 1024.0 + 0.5 * (double)(rnd() & 1); }

/* floats as their bits: libc's %a differs between hosts */
static uint32_t fbits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}
static unsigned long long dbits(double d) {
    unsigned long long u;
    memcpy(&u, &d, 8);
    return u;
}

/* one rnd() per statement: the evaluation order of arguments is the compiler's */
static void floats(void) {
    uint32_t mode;
    int i;
    for (mode = 0; mode < 4; mode++) {
        set_cop1_cs(mode);
        printf("32 rm %u -> %u\n", mode, get_cop1_cs());
        for (i = 0; i < 1500; i++) {
            float f = rf();
            float g = rf();
            double d = rd();
            double e = rd() + 3.0;
            int32_t w = (int32_t)(uint32_t)rnd();
            printf("32 cvt.w.s %08x -> %d\n", fbits(f), CVT_W_S(f));
            printf("32 cvt.w.d %016llx -> %d\n", dbits(d), CVT_W_D(d));
            printf("32 trunc %08x %016llx -> %d %d\n", fbits(f), dbits(d), TRUNC_W_S(f), TRUNC_W_D(d));
            printf("32 cvt.l.d %016llx -> %lld\n", dbits(d), (long long)CVT_L_D(d));
            printf("32 cvt.s.w %d -> %08x\n", (int)w, fbits(CVT_S_W(w)));
            printf("32 mul.s %08x\n", fbits(MUL_S(f, g)));
            printf("32 div.d %016llx\n", dbits(DIV_D(d, e)));
        }
    }
    set_cop1_cs(0);
    for (i = 0; i < 3000; i++) {
        float f = rf();
        double d = rd();
#ifdef XRC_TEST_UPSTREAM
        printf("32 round %08x %016llx -> %ld %ld\n", fbits(f), dbits(d), (long)lrintf(f), (long)lrint(d));   /* to even (mode 0) */
#else
        printf("32 round %08x %016llx -> %ld %ld\n", fbits(f), dbits(d), (long)xrc_lroundf(f), (long)xrc_lround(d));
#endif
    }
}

int main(void) {
    s_buf = (uint8_t*)malloc(MEM + 64);
#ifdef XRC_RECOMP_H
    rdram = s_buf - 0x80000000u;   /* ours: the window's base, so rdram + KSEG0 address = byte */
#else
    rdram = s_buf;                 /* upstream: rdram is byte 0, KSEG0 subtracted per access */
#endif
#if defined(XRC_RECOMP_H) && defined(__i386__)
    {   /* the Xbox's x87 control word (Melee-X measured it): 53-bit doubles */
        uint16_t cw = 0x027F;
        __asm__ volatile("fldcw %0" ::"m"(cw));
    }
#endif
    ints();
    memory();
    floats();
    memory64();
    fprintf(stderr, "test_recomp_h (%s): done\n", WIDTH);
    return 0;
}
