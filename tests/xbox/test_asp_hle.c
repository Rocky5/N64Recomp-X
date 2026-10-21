/* test_asp_hle.c - host tests for xbox/src/hw/asp_hle.c (driver: tools/xbox/test_asp_hle.py).
 *
 * Every command against tests/xbox/asp_ref.c on synthetic data (no ROM
 * material: books, frames, tables and samples come from a PRNG or are built
 * here), plus the vectors of Birdman64's crates/pw64-audio/src/tests.rs and
 * pw64-audio-data/src/vadpcm.rs (MIT), ported. The command lists are built the
 * way the decomp's synthesizer builds them (abi.h packing, _decodeChunk's DMA
 * skew, the 160-sample chunks of alAudioFrame).
 *
 *   test_asp_hle            run the tests; prints "hash XXXXXXXX" over a
 *                           32-voice synthetic workload: the driver requires the
 *                           same hash from every build (MMX, -DASP_SCALAR, ...);
 *                           "hash2" likewise over nead and n_aspMain lists
 *                           (asp_abis.inc: the registry and the other ABIs)
 *   test_asp_hle --bench N  N frames (3 chunks x 32 voices + reverb), us per frame
 *
 * N64Recomp-X: built with -DASP_HLE_WORDSWAP=1 the test RAM is the
 * recompiled game's word-swapped image (halfwords at ^2, bytes at ^3) and
 * every RAM access here goes through RB / ram_putb / ram_put16 / ram16 in
 * N64 order; DMEM bytes through DMB (o ^ 1). "rhash" covers what reaches
 * RAM, read in N64 order: the driver requires it to be the same in the
 * native and the word-swapped builds. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#include "../../xbox/src/hw/asp_hle.h"
#include "asp_ref.h"

void xhw_logf(const char* fmt, ...);
void xhw_logf(const char* fmt, ...) { (void)fmt; }   /* asp_hle.c's log: silent here */

#if defined(ASP_HLE_RESAMPLE_PER_TAP) && ASP_HLE_RESAMPLE_PER_TAP
#define PER_TAP 1
#else
#define PER_TAP 0
#endif

/* ---- harness ---------------------------------------------------------------- */

static int g_fail, g_checks;
#define CHECK(c, ...)                                               \
    do {                                                            \
        g_checks++;                                                 \
        if (!(c)) {                                                 \
            if (g_fail++ < 25) {                                    \
                printf("FAIL %s:%d: ", __func__, __LINE__);         \
                printf(__VA_ARGS__);                                \
                printf("\n");                                       \
            }                                                       \
        }                                                           \
    } while (0)

#if defined(ASP_HLE_WORDSWAP) && ASP_HLE_WORDSWAP
#define RX_B 3u   /* an N64 byte's host offset in the image */
#define RX_H 2u   /* a halfword's */
#define DX_B 1u   /* a DMEM byte's */
#else
#define RX_B 0u
#define RX_H 0u
#define DX_B 0u
#endif

#define RAM_SIZE (1u << 20)
static uint8_t g_ram[RAM_SIZE];
#define RB(a) g_ram[(uint32_t)(a) ^ RX_B]
#define DMB(o) asp_hle_dmem()[(uint32_t)(o) ^ DX_B]
static void ram_putb(uint32_t a, const uint8_t* s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) RB(a + i) = s[i];
}
static void* addr_cb(uint32_t a, void* user)
{
    (void)user;
    return a + 0x1000 <= RAM_SIZE ? g_ram + a : NULL;
}

static uint32_t g_cmds[2 * 16384];
static int g_n;
static void C(uint32_t w0, uint32_t w1)
{
    g_cmds[2 * g_n] = w0, g_cmds[2 * g_n + 1] = w1;
    g_n++;
}
static void run(void)
{
    asp_hle_run(g_cmds, (uint32_t)g_n, addr_cb, NULL);
    g_n = 0;
}

/* abi.h's packing macros */
#define W0(op, f, lo) ((uint32_t)(op) << 24 | (uint32_t)((f)&0xFF) << 16 | ((uint32_t)(lo)&0xFFFF))
enum { SPNOOP, ADPCM, CLEARBUFF, ENVMIXER, LOADBUFF, RESAMPLE, SAVEBUFF, SEGMENT, SETBUFF, SETVOL,
       DMEMMOVE, LOADADPCM, MIXER, INTERLEAVE, POLEF, SETLOOP };
#define F_INIT 1
#define F_LOOP 2
#define F_LEFT 2
#define F_VOL 4
#define F_AUX 8
static void a_setbuff(int f, uint32_t i, uint32_t o, uint32_t c) { C(W0(SETBUFF, f, i), (o & 0xFFFF) << 16 | (c & 0xFFFF)); }
static void a_setvol(int f, int v, int t, int r) { C(W0(SETVOL, f, v), ((uint32_t)t & 0xFFFF) << 16 | ((uint32_t)r & 0xFFFF)); }
static void a_adpcm(int f, uint32_t s) { C(W0(ADPCM, f, 0), s); }
static void a_clearbuff(uint32_t d, uint32_t c) { C((uint32_t)CLEARBUFF << 24 | (d & 0xFFFFFF), c); }
static void a_envmixer(int f, uint32_t s) { C(W0(ENVMIXER, f, 0), s); }
static void a_loadbuff(uint32_t s) { C((uint32_t)LOADBUFF << 24, s); }
static void a_resample(int f, uint32_t p, uint32_t s) { C(W0(RESAMPLE, f, p), s); }
static void a_savebuff(uint32_t s) { C((uint32_t)SAVEBUFF << 24, s); }
static void a_segment(uint32_t s, uint32_t b) { C((uint32_t)SEGMENT << 24, (s & 0xFF) << 24 | (b & 0xFFFFFF)); }
static void a_dmemmove(uint32_t i, uint32_t o, uint32_t c) { C((uint32_t)DMEMMOVE << 24 | (i & 0xFFFFFF), (o & 0xFFFF) << 16 | (c & 0xFFFF)); }
static void a_loadadpcm(uint32_t c, uint32_t s) { C((uint32_t)LOADADPCM << 24 | (c & 0xFFFFFF), s); }
static void a_mixer(int g, uint32_t i, uint32_t o) { C(W0(MIXER, 0, g), (i & 0xFFFF) << 16 | (o & 0xFFFF)); }
static void a_interleave(uint32_t l, uint32_t r) { C((uint32_t)INTERLEAVE << 24, (l & 0xFFFF) << 16 | (r & 0xFFFF)); }
static void a_polef(int f, int g, uint32_t s) { C(W0(POLEF, f, g), s); }
static void a_setloop(uint32_t a) { C((uint32_t)SETLOOP << 24, a); }

static int16_t* dm16(void) { return (int16_t*)asp_hle_dmem(); }
static void put16(uint32_t off, const int16_t* s, int n) { memcpy(asp_hle_dmem() + off, s, (size_t)n * 2); }
static int16_t get16(uint32_t off) { return dm16()[off >> 1]; }
static void ram_put16(uint32_t a, const int16_t* s, int n)
{
    int i;
    for (i = 0; i < n; i++) memcpy(g_ram + ((a + 2u * (uint32_t)i) ^ RX_H), s + i, 2);
}
static int16_t ram16(uint32_t a) { int16_t v; memcpy(&v, g_ram + (a ^ RX_H), 2); return v; }

static uint32_t g_rng = 0x12345678u;
static uint32_t rnd(void)
{
    g_rng ^= g_rng << 13, g_rng ^= g_rng >> 17, g_rng ^= g_rng << 5;
    return g_rng;
}
static int32_t rnd_range(int32_t lo, int32_t hi) { return lo + (int32_t)(rnd() % (uint32_t)(hi - lo + 1)); }
static int16_t rnd_s16(void) { return (int16_t)rnd(); }

static void reset(void)
{
    asp_hle_reset();
    memset(g_ram, 0, sizeof g_ram);
    g_n = 0;
}

/* ---- resampler tables (synthetic) -------------------------------------------- */

static int32_t div128(int64_t v)   /* floor((v + 64) / 128) */
{
    int64_t a = v + 64;
    return (int32_t)(a >= 0 ? a / 128 : -((-a + 127) / 128));
}

/* Catmull-Rom 4-tap, phase centres t = (2p + 1) / 128, in integers (Q15 =
 * numerator / 128, rounded) so every compiler and FPU builds the same table;
 * mirrored so phase 63 - p is phase p reversed exactly: passes the same check
 * the ROM's table must pass. */
static void lut_cr(int16_t* lut, int sharp)
{
    int p, k;
    for (p = 0; p < 32; p++) {
        int64_t u = 2 * p + 1, u2 = u * u, u3 = u2 * u;
        int32_t q[4];
        q[0] = div128(-u3 + 256 * u2 - 16384 * u);
        q[1] = div128(3 * u3 - 640 * u2 + 4194304);
        q[2] = div128(-3 * u3 + 512 * u2 + 16384 * u);
        q[3] = div128(u3 - 128 * u2);
        if (sharp) {
            /* add (-a, a, a, -a): same sum, large |taps| -> the int64 path */
            int32_t a = 32767 - (q[1] > q[2] ? q[1] : q[2]);
            q[0] -= a, q[1] += a, q[2] += a, q[3] -= a;
        }
        for (k = 0; k < 4; k++) {
            lut[p * 4 + k] = (int16_t)q[k];
            lut[(63 - p) * 4 + 3 - k] = (int16_t)q[k];
        }
    }
}

static void lut_linear_ref(int16_t* lut)   /* Birdman64 lut::linear, the HLE's fallback */
{
    int p;
    memset(lut, 0, 512);
    for (p = 0; p < 64; p++) {
        int32_t f = (p * 0x8000 + 32) / 64;
        lut[p * 4 + 1] = (int16_t)(0x7FFF - f);
        lut[p * 4 + 2] = (int16_t)f;
    }
}

/* ---- DMA, CLEARBUFF, DMEMMOVE -------------------------------------------------- */

static void test_dma_birdman(void)   /* tests.rs clearbuff_dmemmove_and_dma_alignment */
{
    int i, ok;
    reset();
    for (i = 0; i < 32; i++) RB(0x100 + i) = (uint8_t)i;
    /* 13 bytes from 0x103: the DMA fetches 16 from 0x100 into DMEM 0x40 */
    a_setbuff(0, 0x44, 0, 13), a_loadbuff(0x103), run();
    for (ok = 1, i = 0; i < 16; i++) ok &= DMB(0x40 + i) == i;
    CHECK(ok && DMB(0x50) == 0, "LOADBUFF alignment");
    a_dmemmove(0x40, 0x80, 16), a_clearbuff(0x40, 3), run();
    for (ok = 1, i = 0; i < 16; i++) ok &= DMB(0x80 + i) == i && DMB(0x40 + i) == 0;
    CHECK(ok, "DMEMMOVE / CLEARBUFF rounds to 16");
    a_setbuff(0, 0, 0x80, 16), a_savebuff(0x200), run();
    for (ok = 1, i = 0; i < 16; i++) ok &= RB(0x200 + i) == i;
    CHECK(ok, "SAVEBUFF");
}

static void test_dma_roundtrip(void)
{
    uint8_t want[4096];
    int i, ok;
    reset();
    for (i = 0; i < 4096; i++) asp_hle_dmem()[i] = (uint8_t)rnd();
    for (i = 0; i < 4096; i++) want[i] = DMB(i);
    /* 736 bytes from 0xE00: wraps past the end of DMEM */
    a_setbuff(0, 0, 0xE00, 736), a_savebuff(0x3000), run();
    for (ok = 1, i = 0; i < 736; i++) ok &= RB(0x3000 + i) == want[(0xE00 + i) & 0xFFF];
    CHECK(ok, "SAVEBUFF wraps DMEM");
    a_clearbuff(0, 4096), run();
    for (ok = 1, i = 0; i < 4096; i++) ok &= asp_hle_dmem()[i] == 0;
    CHECK(ok, "CLEARBUFF 4096");
    a_setbuff(0, 0x7F8, 0, 736), a_loadbuff(0x3000), run();
    for (ok = 1, i = 0; i < 736; i++) ok &= DMB(0x7F8 + i) == want[(0xE00 + i) & 0xFFF];
    CHECK(ok, "LOADBUFF round trip");
    /* odd-length DMEMMOVE rounds to 16; overlapping move keeps memmove semantics */
    a_dmemmove(0x7F8, 0x800, 33), run();
    for (ok = 1, i = 0; i < 48; i++) ok &= DMB(0x800 + i) == want[(0xE00 + i) & 0xFFF];
    CHECK(ok, "DMEMMOVE overlapping");
}

/* ---- ADPCM ------------------------------------------------------------------------ */

static void pack_frame(uint8_t* f, int header, const int8_t* nib)
{
    int i;
    f[0] = (uint8_t)header;
    for (i = 0; i < 8; i++) f[1 + i] = (uint8_t)(((nib[2 * i] & 15) << 4) | (nib[2 * i + 1] & 15));
}

/* One A_INIT call on `nf` frames at RAM 0x400; book (predictor 0 rows r0, r1). */
static void hle_decode(const int16_t* r0, const int16_t* r1, const uint8_t* frames, int nf, int16_t* out)
{
    int16_t book[128];
    int i;
    reset();
    memset(book, 0, sizeof book);
    memcpy(book, r0, 16), memcpy(book + 8, r1, 16);
    ram_put16(0x100, book, 128);
    ram_putb(0x400, frames, (size_t)nf * 9);
    a_loadadpcm(256, 0x100);
    a_setbuff(0, 0, 0, (uint32_t)(nf * 9 + 7) & ~7u), a_loadbuff(0x400);
    a_setbuff(0, 0, 0x400, (uint32_t)nf * 32), a_adpcm(F_INIT, 0x800);
    run();
    for (i = 0; i < 16 * nf; i++) out[i] = get16(0x420 + 2 * i);
}

static void test_adpcm_vectors(void)   /* vadpcm.rs tests, through the command list */
{
    int16_t z[8] = {0}, r0[8], r1[8], out[48];
    int8_t n[16] = {1, -1, 7, -8, 0, 3, -3, 2, 1, 1, 1, 1, -2, -2, -2, -2};
    uint8_t fr[27];
    int i, ok;

    for (i = 0; i < 8; i++) r0[i] = 100, r1[i] = -200;
    memset(fr, 0, sizeof fr);
    hle_decode(r0, r1, fr, 3, out);
    for (ok = 1, i = 0; i < 48; i++) ok &= out[i] == 0;
    CHECK(ok, "silence");

    pack_frame(fr, 0x00, n), hle_decode(z, z, fr, 1, out);
    for (ok = 1, i = 0; i < 16; i++) ok &= out[i] == n[i];
    CHECK(ok, "zero book = residual");
    pack_frame(fr, 0x30, n), hle_decode(z, z, fr, 1, out);
    for (ok = 1, i = 0; i < 16; i++) ok &= out[i] == n[i] * 8;
    CHECK(ok, "zero book, scale 3");

    {   /* in-group and cross-group prediction: c1[0] = 0.5 */
        int8_t m[16] = {0};
        int16_t h[8] = {1024, 0, 0, 0, 0, 0, 0, 0};
        m[0] = 4, m[7] = 4;
        pack_frame(fr, 0, m), hle_decode(z, h, fr, 1, out);
        CHECK(out[0] == 4 && out[1] == 2 && out[2] == 0 && out[7] == 4 && out[8] == 2 && out[9] == 0,
              "in-group / cross-group: %d %d %d %d %d %d", out[0], out[1], out[2], out[7], out[8], out[9]);
    }
    {   /* history crosses frames, floor(-1.5) = -2 */
        int8_t m[16] = {0}, zero[16] = {0};
        int16_t h[8] = {-1024, 0, 0, 0, 0, 0, 0, 0};
        m[14] = 3;
        pack_frame(fr, 0, m), pack_frame(fr + 9, 0, zero), hle_decode(h, z, fr, 2, out);
        CHECK(out[14] == 3 && out[16] == -2, "history floors: %d %d", out[14], out[16]);
    }
    {   /* scale 15 is capped at 12 (aspMain; Birdman64's decoder does not cap and would give 32767) */
        int8_t m[16];
        int16_t one[8];
        for (i = 0; i < 16; i++) m[i] = 7;
        pack_frame(fr, 0xF0, m), hle_decode(z, z, fr, 1, out);
        for (ok = 1, i = 0; i < 16; i++) ok &= out[i] == 7 << 12;
        CHECK(ok, "scale cap: %d", out[0]);
        for (i = 0; i < 8; i++) one[i] = 2048;   /* every residual leaks fully: saturates */
        hle_decode(z, one, fr, 1, out);
        CHECK(out[0] == 28672 && out[1] == 32767 && out[7] == 32767, "saturation: %d %d", out[0], out[1]);
    }
}

/* Synthesizer-shaped decode over several calls (state in RDRAM, DMA skew) vs ref_adpcm. */
static void test_adpcm_random(void)
{
    int trial;
    static int16_t ref[16 * 64];
    static uint8_t stream[9 * 64];
    for (trial = 0; trial < 400; trial++) {
        int16_t book[128], hist[16], init_state[16];
        int nf = rnd_range(1, 60), done = 0, i, k, cls = trial % 4, init = trial % 3 != 0;
        const uint32_t src0 = 0x10000 + (rnd() & 0xFF), state = 0x800;
        int32_t lim = cls == 0 ? 2048 : cls == 1 ? 7000 : cls == 2 ? 12000 : 32767;
        reset();
        for (i = 0; i < 128; i++) book[i] = (int16_t)rnd_range(-lim, lim);
        for (i = 0; i < 9 * nf; i++) stream[i] = (uint8_t)rnd();
        for (i = 0; i < nf; i++)   /* scales mostly 0..12, some 13..15; predictors 0..15 */
            stream[9 * i] = (uint8_t)(((rnd() % 5 ? rnd() % 13 : 13 + rnd() % 3) << 4) | (rnd() % 16));
        for (i = 0; i < 16; i++) init_state[i] = init ? 0 : rnd_s16();
        memcpy(hist, init_state, sizeof hist);
        ref_adpcm(book, stream, nf, hist, ref);
        ram_put16(0x100, book, 128);
        ram_putb(src0, stream, (size_t)nf * 9);
        if (!init) ram_put16(state, init_state, 16);
        while (done < nf) {
            int c = rnd_range(1, 20), first = done == 0;
            uint32_t src, skew, nbytes, count;
            int16_t prev[16];
            if (c > nf - done) c = nf - done;
            src = src0 + 9u * (uint32_t)done, skew = src & 7, nbytes = 9u * (uint32_t)c + skew;
            count = (uint32_t)c * 32 - (uint32_t)(rnd() % 32);   /* ceil(count / 32) frames */
            a_loadadpcm(256, 0x100);
            a_setbuff(0, 0, 0, nbytes + 8 - (nbytes & 7)), a_loadbuff(src - skew);
            a_setbuff(0, skew, 0x200, count), a_adpcm(first && init ? F_INIT : 0, state);
            run();
            if (first) memcpy(prev, init_state, sizeof prev);
            else memcpy(prev, ref + 16 * (done - 1), sizeof prev);
            for (k = 0; k < 16; k++) CHECK(get16(0x200 + 2 * k) == prev[k], "trial %d history %d", trial, k);
            for (k = 0; k < 16 * c; k++)
                if (get16(0x220 + 2 * k) != ref[16 * done + k]) break;
            CHECK(k == 16 * c, "trial %d (class %d) frame %d sample %d: %d vs ref %d", trial, cls, done + k / 16, k,
                  get16(0x220 + 2 * k), ref[16 * done + k]);
            done += c;
        }
        for (k = 0; k < 16; k++) CHECK(ram16(state + 2 * k) == hist[k], "trial %d state %d", trial, k);
    }
}

static void test_adpcm_loop(void)
{
    int16_t book[128], loop[16], hist[16], ref[32];
    uint8_t fr[18];
    int i, ok;
    reset();
    for (i = 0; i < 128; i++) book[i] = (int16_t)rnd_range(-3000, 3000);
    for (i = 0; i < 16; i++) loop[i] = (int16_t)(i * 50 - 300);
    for (i = 0; i < 18; i++) fr[i] = (uint8_t)rnd();
    fr[0] = 0x52, fr[9] = 0x87;
    ram_put16(0x100, book, 128), ram_put16(0xA00, loop, 16), ram_putb(0x400, fr, 18);
    memcpy(hist, loop, sizeof hist), ref_adpcm(book, fr, 2, hist, ref);
    /* load.c: SETLOOP to lstate, then A_ADPCM with A_LOOP (history from the loop, state still written) */
    a_loadadpcm(256, 0x100), a_setbuff(0, 0, 0, 24), a_loadbuff(0x400);
    a_setloop(0xA00), a_setbuff(0, 0, 0x200, 64), a_adpcm(F_LOOP, 0x800), run();
    for (ok = 1, i = 0; i < 16; i++) ok &= get16(0x200 + 2 * i) == loop[i];
    for (i = 0; i < 32; i++) ok &= get16(0x220 + 2 * i) == ref[i];
    for (i = 0; i < 16; i++) ok &= ram16(0x800 + 2 * i) == hist[i];
    CHECK(ok, "A_LOOP");
    /* count 0: the 16 history samples only, and the state = the loop state */
    a_setloop(0xA00), a_setbuff(0, 0, 0x300, 0), a_adpcm(F_LOOP, 0x900), run();
    for (ok = 1, i = 0; i < 16; i++) ok &= get16(0x300 + 2 * i) == loop[i] && ram16(0x900 + 2 * i) == loop[i];
    CHECK(ok, "A_LOOP count 0");
}

/* ---- RESAMPLE ------------------------------------------------------------------- */

static void test_resample_birdman(void)   /* tests.rs, linear table */
{
    int16_t input[64], whole[64], part[64];
    int i, ok, pass, sizes[2][3] = {{64, 0, 0}, {16, 32, 16}};
    for (i = 0; i < 64; i++) input[i] = (int16_t)(i * 37 - 900);
    for (pass = 0; pass < 2; pass++) {
        int pos = 0, n = 0, j;
        int16_t* out = pass ? part : whole;
        reset();
        for (j = 0; j < 3 && sizes[pass][j]; j++) {
            int c = sizes[pass][j];
            put16(0x100, input + pos, c);
            a_setbuff(0, 0x100, 0x300, (uint32_t)c * 2), a_resample(j == 0 ? F_INIT : 0, 0x8000, 0x40), run();
            for (i = 0; i < c; i++) out[n++] = get16(0x300 + 2 * i);
            pos += c;
        }
    }
    for (ok = whole[0] == 0 && whole[1] == 0 && whole[2] == 0, i = 3; i < 64; i++) ok &= whole[i] == input[i - 3];
    CHECK(ok, "unity pitch = 3-sample delay");
    CHECK(!memcmp(whole, part, sizeof whole), "state carries over 16+32+16");

    reset();
    for (i = 0; i < 16; i++) input[i] = (int16_t)(i * 1000);
    put16(0x100, input, 16);
    a_setbuff(0, 0x100, 0x300, 64), a_resample(F_INIT, 0x4000, 0x40), run();
    for (ok = 1, i = 3; i < 15; i++) {
        int32_t mid = get16(0x300 + 4 * i + 2) - (input[i - 3] + 500);
        ok &= get16(0x300 + 4 * i) == input[i - 3] && mid >= -1 && mid <= 1;
    }
    CHECK(ok, "half pitch interpolates");
    CHECK(ram16(0x40) == 12000 && ram16(0x42) == 13000 && ram16(0x44) == 14000 && ram16(0x46) == 15000 &&
              ram16(0x48) == 0, "half pitch state");
}

static void test_resample_random(void)
{
    static const uint32_t pitches[] = {1, 0x0800, 0x2000, 0x4000, 0x6123, 0x7FFF, 0x8000, 0x8001, 0x9999,
                                       0xC000, 0xF000, 0xFFFE /* MAX_RATIO 1.99996 */, 0xFFFF};
    static int16_t z[4 + 4096], ref[2048];
    int16_t lut[256];
    int t, pi, i;
    for (t = 0; t < 3; t++) {          /* linear fallback, Catmull-Rom, sharp (int64 path) */
        for (pi = 0; pi < (int)(sizeof pitches / sizeof pitches[0]); pi++) {
            uint32_t pitch = pitches[pi], step = pitch << 1, state = 0x600;
            int total = 0, rep;
            for (rep = 0; rep < 6; rep++) {
                int loud = rep & 1;
                reset();
                if (t == 0) {
                    lut_linear_ref(lut);
                } else {
                    lut_cr(lut, t == 2);
                    CHECK(asp_hle_set_resample_lut(lut) == 1, "table %d accepted", t);
                }
                memset(z, 0, 8);
                for (i = 4; i < 4 + 4096; i++)
                    z[i] = loud ? (int16_t)(rnd() & 1 ? 32767 - (rnd() & 255) : -32768 + (rnd() & 255)) : rnd_s16();
                ref_resample(z, 1024, pitch, 0, lut, PER_TAP, ref);
                total = 0;
                while (total < 1024) {
                    int n = 8 * rnd_range(1, 20), k;
                    uint64_t p = (uint64_t)total * step;
                    uint32_t b = (uint32_t)(p >> 16);
                    if (n > 1024 - total) n = 1024 - total;
                    put16(0x140, z + b + 4, 2 * n + 8);     /* AL_DECODER_OUT-like input */
                    a_setbuff(0, 0x140, 0x800, (uint32_t)n * 2);
                    a_resample(total == 0 ? F_INIT : 0, pitch, state);
                    run();
                    for (k = 0; k < n; k++)
                        if (get16(0x800 + 2 * k) != ref[total + k]) break;
                    CHECK(k == n, "lut %d pitch %#x out %d: %d vs ref %d", t, pitch, total + k,
                          get16(0x800 + 2 * k), ref[total + k]);
                    total += n;
                }
                {
                    uint64_t p = (uint64_t)1024 * step;
                    uint32_t b = (uint32_t)(p >> 16);
                    CHECK(ram16(state) == z[b] && ram16(state + 6) == z[b + 3] &&
                              (uint16_t)ram16(state + 8) == (uint16_t)p, "lut %d pitch %#x state", t, pitch);
                }
            }
        }
    }
    /* a count that is not a multiple of 8 samples: 8-sample groups */
    reset();
    for (i = 0; i < 64; i++) dm16()[0xA0 + i] = (int16_t)(i * 100);
    dm16()[0x400 + 13] = 0x5555, dm16()[0x400 + 16] = 0x5555;
    a_setbuff(0, 0x140, 0x800, 26), a_resample(F_INIT, 0x8000, 0x600), run();
    CHECK(get16(0x800 + 2 * 13) != 0x5555 && get16(0x800 + 2 * 16) == 0x5555 && ram16(0x600) == 1200,
          "13 outputs -> 16");
}

static void test_lut_api(void)
{
    int16_t lut[256], zero[256];
    uint8_t be[512];
    int i;
    reset();
    memset(zero, 0, sizeof zero);
    CHECK(!asp_hle_resample_lut_valid(zero), "zero table rejected");
    lut_linear_ref(lut);
    CHECK(!asp_hle_resample_lut_valid(lut), "linear table is not a ROM table");
    CHECK(asp_hle_set_resample_lut(zero) == 0 && asp_hle_stats()->rom_lut == 0, "rejected table not installed");
    lut_cr(lut, 0);
    for (i = 0; i < 256; i++) be[2 * i] = (uint8_t)((uint16_t)lut[i] >> 8), be[2 * i + 1] = (uint8_t)lut[i];
    CHECK(asp_hle_set_resample_lut_be(be) == 1 && asp_hle_stats()->rom_lut == 1, "BE table installed");
    {   /* unity pitch through the installed table equals the reference with it */
        int16_t x[40], ref[32];
        memset(x, 0, 8);
        for (i = 4; i < 40; i++) x[i] = rnd_s16();
        put16(0x100, x + 4, 36);
        a_setbuff(0, 0x100, 0x400, 64), a_resample(F_INIT, 0x8000, 0x40), run();
        ref_resample(x, 32, 0x8000, 0, lut, PER_TAP, ref);
        for (i = 0; i < 32; i++)
            if (get16(0x400 + 2 * i) != ref[i]) break;
        CHECK(i == 32, "BE table used");
    }
    asp_hle_reset();
    CHECK(asp_hle_stats()->rom_lut == 0, "reset restores the fallback");
}

/* ---- ENVMIXER ------------------------------------------------------------------- */

/* The whole DMEM, element-ordered reference for one ENVMIXER (Birdman64's
 * per-sample order: L, R, wet L, wet R). */
static void ref_envmix_dmem(int16_t* d, RefEnv* e, int16_t dry, int16_t wet, uint32_t in, const uint32_t* outs,
                            int nout, int n)
{
    static int16_t vl[4096], vr[4096];
    int i, k;
    ref_env_volumes(e, n, vl, vr);
    for (i = 0; i < n; i++) {
        int16_t g[4];
        int16_t x = d[((in >> 1) + i) & 0x7FF];
        g[0] = ref_gain(vl[i], dry), g[1] = ref_gain(vr[i], dry), g[2] = ref_gain(vl[i], wet), g[3] = ref_gain(vr[i], wet);
        for (k = 0; k < nout; k++) {
            int16_t* o = &d[((outs[k] >> 1) + i) & 0x7FF];
            ref_mix(o, &x, 1, g[k]);
        }
    }
}

static int32_t rate_for(double from, double to, int samples)   /* env.c _getRate's intent, in Q16.16 */
{
    double a = pow(to / from, 8.0 / samples);
    return (int32_t)(a * 65536.0);
}

static void test_envmixer_random(void)
{
    static int16_t want[2048];
    int trial;
    for (trial = 0; trial < 600; trial++) {
        int16_t vl = (int16_t)rnd_range(0, 32767), vr = (int16_t)rnd_range(0, 32767);
        int16_t tl = (int16_t)rnd_range(0, 32767), tr = (int16_t)rnd_range(0, 32767);
        int16_t dry = (int16_t)rnd_range(0, 32767), wet = (int16_t)(trial % 7 == 0 ? -32768 : rnd_range(0, 32767));
        int32_t rl, rr;
        int aux = trial % 5 != 0, calls = rnd_range(1, 5), c, layout = trial % 6, i;
        uint32_t in = 0, outs[4] = {1088, 1408, 1728, 2048}, state = 0x700;
        RefEnv e;
        switch (trial % 8) {
        case 0: rl = rr = 0x10000; break;                                   /* hold */
        case 1: rl = 0, rr = 0; break;                                      /* to zero in 8 */
        case 2: rl = 0x7FFFFFFF, rr = 0x7FFFFFFF; break;                    /* jump up */
        case 3: rl = 0x20000, rr = 0x8000; break;                           /* x2, x0.5 per 8 */
        default:
            rl = rate_for(vl ? vl : 1, tl ? tl : 1, rnd_range(8, 600));
            rr = rate_for(vr ? vr : 1, tr ? tr : 1, rnd_range(8, 600));
        }
        if (layout == 4) outs[1] = outs[0];                         /* same start: still commutes */
        if (layout == 5) outs[1] = outs[0] + 6, outs[3] = outs[2];  /* partial overlap: scalar path */
        reset();
        for (i = 0; i < 2048; i++) dm16()[i] = rnd_s16();
        ref_env_init(&e, vl, vr, tl, tr, rl, rr);
        for (c = 0; c < calls; c++) {
            int n = 8 * rnd_range(1, 20), j;
            for (j = 0; j < n; j++) dm16()[j] = rnd_s16();              /* fresh input at AL_RESAMPLER_OUT */
            memcpy(want, dm16(), sizeof want);
            ref_envmix_dmem(want, &e, dry, wet, in, outs, aux ? 4 : 2, n);
            a_setbuff(0, in, outs[0], (uint32_t)n * 2 - (uint32_t)(rnd() % 16));   /* rounds up to 8 */
            a_setbuff(F_AUX, outs[1], outs[2], outs[3]);
            if (c == 0) {
                a_setvol(F_LEFT | F_VOL, vl, 0, 0), a_setvol(F_VOL, vr, 0, 0);
                a_setvol(F_LEFT, tl, (int)((uint32_t)rl >> 16), rl & 0xFFFF);
                a_setvol(0, tr, (int)((uint32_t)rr >> 16), rr & 0xFFFF);
                a_setvol(F_AUX, dry, 0, wet);
                a_envmixer(F_INIT | (aux ? F_AUX : 0), state);
            } else {
                a_envmixer(aux ? F_AUX : 0, state);
            }
            run();
            for (j = 0; j < 2048; j++)
                if (dm16()[j] != want[j]) break;
            CHECK(j == 2048, "trial %d call %d (rates %#x %#x, layout %d): lane %d = %d vs ref %d", trial, c,
                  (unsigned)rl, (unsigned)rr, layout, j, dm16()[j & 2047], want[j & 2047]);
        }
    }
}

static void test_envmixer_birdman(void)
{
    int16_t x[64];
    int i, ok;
    /* constant volume pans and sends */
    reset();
    for (i = 0; i < 16; i++) x[i] = (int16_t)((i - 8) * 1000);
    put16(0, x, 16);
    for (i = 0; i < 16; i++) dm16()[0x80 + i] = 100;
    a_setbuff(0, 0, 0x100, 32), a_setbuff(F_AUX, 0x200, 0x300, 0x400);
    a_setvol(F_LEFT | F_VOL, 0x4000, 0, 0), a_setvol(F_VOL, 0x2000, 0, 0);
    a_setvol(F_LEFT, 0x4000, 1, 0), a_setvol(0, 0x2000, 1, 0), a_setvol(F_AUX, 0x7000, 0, 0x1000);
    a_envmixer(F_INIT | F_AUX, 0x800), run();
    for (ok = 1, i = 0; i < 16; i++) {
        int g0 = (0x4000 * 0x7000 + 0x4000) >> 15, g1 = (0x2000 * 0x7000 + 0x4000) >> 15;
        int g2 = (0x4000 * 0x1000 + 0x4000) >> 15, g3 = (0x2000 * 0x1000 + 0x4000) >> 15;
        ok &= get16(0x100 + 2 * i) == 100 + ((x[i] * g0 + 0x4000) >> 15);
        ok &= get16(0x200 + 2 * i) == ((x[i] * g1 + 0x4000) >> 15);
        ok &= get16(0x300 + 2 * i) == ((x[i] * g2 + 0x4000) >> 15);
        ok &= get16(0x400 + 2 * i) == ((x[i] * g3 + 0x4000) >> 15);
    }
    CHECK(ok, "constant volume");

    /* ramps by rate per 8 samples and clamps: L 0x1000 -> 0x4000 x2, R 0x4000 -> 0x1000 x0.5 */
    reset();
    for (i = 0; i < 64; i++) x[i] = 0x7FFF;
    put16(0, x, 64);
    a_setbuff(0, 0, 0x100, 64), a_setbuff(F_AUX, 0x200, 0x300, 0x400);
    a_setvol(F_LEFT | F_VOL, 0x1000, 0, 0), a_setvol(F_VOL, 0x4000, 0, 0);
    a_setvol(F_LEFT, 0x4000, 2, 0), a_setvol(0, 0x1000, 0, 0x8000), a_setvol(F_AUX, 0x7FFF, 0, 0);
    a_envmixer(F_INIT | F_AUX, 0x800), run();
    {
        int l7 = get16(0x100 + 14), l15 = get16(0x100 + 30), r7 = get16(0x200 + 14);
        CHECK(abs(l7 - 0x2000) <= 2 && abs(l15 - 0x3FFF) <= 2 && abs(r7 - 0x2000) <= 2, "ramp: %#x %#x %#x", l7, l15, r7);
        for (ok = 1, i = 16; i < 32; i++)
            ok &= abs(get16(0x100 + 2 * i) - 0x3FFF) <= 1 && abs(get16(0x200 + 2 * i) - 0x1000) <= 1;
        for (i = 1; i < 32; i++) ok &= get16(0x100 + 2 * i) >= get16(0x100 + 2 * i - 2);
        CHECK(ok, "ramp clamps at the targets, monotonic");
    }
    /* continuing from the state keeps the reached targets */
    memset(asp_hle_dmem() + 0x100, 0, 0x400);
    a_setbuff(0, 0, 0x100, 32), a_setbuff(F_AUX, 0x200, 0x300, 0x400), a_envmixer(F_AUX, 0x800), run();
    for (ok = 1, i = 0; i < 16; i++)
        ok &= abs(get16(0x100 + 2 * i) - 0x3FFF) <= 1 && abs(get16(0x200 + 2 * i) - 0x1000) <= 1;
    CHECK(ok, "continue holds the targets");
}

/* ---- MIXER, INTERLEAVE ---------------------------------------------------------- */

static void test_mixer(void)
{
    static int16_t want[2048];
    int16_t a[4] = {30000, -30000, 1000, -1000}, b[4] = {10000, -10000, 1000, 1000};
    int i, trial;
    reset();
    put16(0x100, a, 4), put16(0x200, b, 4);
    a_setbuff(0, 0, 0, 8), a_mixer(0x7FFF, 0x100, 0x200), run();
    CHECK(get16(0x200) == 32767 && get16(0x202) == -32768 && get16(0x204) == 2000 && get16(0x206) == 0, "saturates");
    a_setbuff(0, 0, 0, 8), a_mixer(0xC000, 0x100, 0x300), run();
    CHECK(get16(0x300) == -15000 && get16(0x302) == 15000, "negative gain");
    for (trial = 0; trial < 500; trial++) {
        int n = rnd_range(1, 400), cnt = 2 * n, g, kind = trial % 4;
        uint32_t src = 2 * (uint32_t)rnd_range(0, 900), dst;
        int na = (int)(((uint32_t)cnt + 31) & ~31u) / 2;
        g = trial % 9 == 0 ? -32768 : trial % 9 == 1 ? 0x7FFF : rnd_s16();
        dst = kind == 0 ? src : kind == 1 ? src + 2 * (uint32_t)rnd_range(1, 7) : 2 * (uint32_t)rnd_range(1024, 1500);
        if (kind == 3) src = 2 * (uint32_t)rnd_range(0, 1000);
        reset();
        for (i = 0; i < 2048; i++) dm16()[i] = trial % 3 ? rnd_s16() : (int16_t)(rnd() & 1 ? -32768 : 32767);
        memcpy(want, dm16(), sizeof want);
        for (i = 0; i < na; i++) ref_mix(&want[((dst >> 1) + i) & 0x7FF], &want[((src >> 1) + i) & 0x7FF], 1, (int16_t)g);
        a_setbuff(0, 0, 0, (uint32_t)cnt), a_mixer(g, src, dst), run();
        CHECK(!memcmp(want, dm16(), sizeof want), "trial %d kind %d gain %d", trial, kind, g);
    }
}

static void test_interleave(void)
{
    int16_t l[4] = {1, 2, 3, 4}, r[4] = {-1, -2, -3, -4};
    static int16_t L[400], R[400], want[800];
    int i, ok, trial;
    reset();
    put16(0x440, l, 4), put16(0x580, r, 4);
    a_setbuff(0, 0, 0, 8), a_interleave(0x440, 0x580), run();
    for (ok = 1, i = 0; i < 4; i++) ok &= get16(4 * i) == l[i] && get16(4 * i + 2) == r[i];
    CHECK(ok, "interleave L first");
    for (trial = 0; trial < 50; trial++) {
        /* AL_MAIN_L/R_OUT hold 160 samples; longer runs from separate buffers */
        int n = trial & 1 ? rnd_range(1, 160) : rnd_range(161, 368);
        uint32_t lo = n <= 160 ? 1088 : 0x800, ro = n <= 160 ? 1408 : 0xB00;
        reset();
        for (i = 0; i < n; i++) L[i] = rnd_s16(), R[i] = rnd_s16();
        put16(lo, L, n), put16(ro, R, n);
        ref_interleave(want, L, R, n);
        a_setbuff(0, 0, 0, (uint32_t)n * 2), a_interleave(lo, ro), run();
        CHECK(!memcmp(dm16(), want, (size_t)n * 4), "interleave n=%d", n);
    }
}

/* ---- POLEF ---------------------------------------------------------------------- */

/* drvrnew.c _init_lpfilter for a cutoff value fc (ALLowPass.fc). */
static int16_t lp_table(int32_t fc_in, int16_t table[16])
{
    const int32_t SCALE = 16384;
    int32_t fc = (fc_in * SCALE) >> 15;
    double ffc = (double)fc / SCALE, f = ffc;
    int i;
    memset(table, 0, 32);
    table[8] = (int16_t)fc;
    for (i = 9; i < 16; i++) f *= ffc, table[i] = (int16_t)(f * SCALE);
    return (int16_t)(SCALE - fc);   /* fgain */
}

static void test_polef(void)
{
    int16_t table[16], gain, x[48], y[2], ref[48];
    int i, ok;
    /* impulse response vs the reference, then vs the one-pole recursion */
    reset();
    gain = lp_table(0x5000, table);
    ram_put16(0, table, 16);
    memset(x, 0, sizeof x);
    x[0] = 0x4000;
    put16(0x100, x, 48);
    a_loadadpcm(32, 0), a_setbuff(0, 0x100, 0x100, 96), a_polef(F_INIT, gain, 0x40), run();
    y[0] = y[1] = 0, ref_polef(table, gain, x, 48, y, ref);
    for (ok = 1, i = 0; i < 48; i++) ok &= get16(0x100 + 2 * i) == ref[i];
    CHECK(ok, "impulse vs ref");
    for (ok = get16(0x100) > 0, i = 1; i < 48; i++) ok &= get16(0x100 + 2 * i) <= get16(0x100 + 2 * i - 2);
    CHECK(ok, "impulse response decays");
    CHECK(ram16(0x44) == ref[46] && ram16(0x46) == ref[47], "state");

    /* tests.rs polef_is_one_pole_lowpass: two calls, state carried */
    reset();
    ram_put16(0, table, 16);
    for (i = 0; i < 48; i++) x[i] = (int16_t)(i % 12 < 6 ? 12000 : -8000);
    put16(0x100, x, 24);
    a_loadadpcm(32, 0), a_setbuff(0, 0x100, 0x100, 48), a_polef(F_INIT, gain, 0x40), run();
    for (i = 0; i < 24; i++) ref[i] = get16(0x100 + 2 * i);
    put16(0x100, x + 24, 24);
    a_loadadpcm(32, 0), a_setbuff(0, 0x100, 0x100, 48), a_polef(0, gain, 0x40), run();
    for (i = 0; i < 24; i++) ref[24 + i] = get16(0x100 + 2 * i);
    {
        double yy = 0;
        int16_t want[48];
        int32_t fc = table[8];
        y[0] = y[1] = 0, ref_polef(table, gain, x, 48, y, want);
        for (ok = 1, i = 0; i < 48; i++) {
            yy = (gain * (double)x[i] + fc * yy) / 16384.0;
            ok &= fabs(ref[i] - yy) <= 8.0 && ref[i] == want[i];
        }
        CHECK(ok, "one-pole lowpass over two calls");
    }

    /* PW64 (VERSION_D aPoleFilter) packs fgain & 0xFF: 0x1800 -> 0, the section is silent */
    reset();
    gain = lp_table(0x2000, table);
    ram_put16(0, table, 16);
    for (i = 0; i < 48; i++) x[i] = rnd_s16();
    put16(0x100, x, 48);
    a_loadadpcm(32, 0), a_setbuff(0, 0x100, 0x100, 96), a_polef(F_INIT, 0x1800 & 0xFF, 0x40), run();
    for (ok = 1, i = 0; i < 48; i++) ok &= get16(0x100 + 2 * i) == 0;
    CHECK(ok, "8-bit-packed gain is silent");

    /* random tables and gains, several calls */
    {
        int trial;
        for (trial = 0; trial < 200; trial++) {
            int16_t tab[16], st[2] = {0, 0}, xx[160], want[160];
            int16_t g = rnd_s16();
            int c;
            reset();
            for (i = 0; i < 16; i++) tab[i] = rnd_s16();
            ram_put16(0, tab, 16);
            for (c = 0; c < 3; c++) {
                int n = 8 * rnd_range(1, 20);
                for (i = 0; i < n; i++) xx[i] = rnd_s16();
                put16(0x200, xx, n);
                ref_polef(tab, g, xx, n, st, want);
                a_loadadpcm(32, 0), a_setbuff(0, 0x200, 0x600, (uint32_t)n * 2), a_polef(c ? 0 : F_INIT, g, 0x40), run();
                for (i = 0; i < n; i++)
                    if (get16(0x600 + 2 * i) != want[i]) break;
                CHECK(i == n, "polef trial %d call %d", trial, c);
            }
        }
    }
}

/* ---- segments, addresses, setvol --------------------------------------------------- */

static void test_segments(void)
{
    int i, ok;
    reset();
    for (i = 0; i < 16; i++) DMB(0x80 + i) = (uint8_t)(0xA0 + i);
    a_segment(3, 0x1000), a_setbuff(0, 0, 0x80, 16), a_savebuff(3u << 24 | 0x20), run();
    for (ok = 1, i = 0; i < 16; i++) ok &= RB(0x1020 + i) == 0xA0 + i;
    CHECK(ok, "segment 3 base");
    /* K0 address: segment 0 + the low 24 bits, as the RSP resolves it */
    a_setbuff(0, 0, 0x80, 16), a_savebuff(0x80002000u), run();
    for (ok = 1, i = 0; i < 16; i++) ok &= RB(0x2000 + i) == 0xA0 + i;
    CHECK(ok, "K0 address");
    /* the segment table is the task's: cleared at the next task */
    a_setbuff(0, 0, 0x80, 16), a_savebuff(3u << 24 | 0x3000), run();
    CHECK(RB(0x3000) == 0xA0, "segments reset per task");
    /* an unmapped address is skipped and counted; its state reads as zeros */
    {
        uint32_t bad0 = asp_hle_stats()->bad_addr;
        a_setbuff(0, 0, 0x80, 16), a_savebuff(0x00FFFFF0u), run();
        a_setbuff(0, 0, 0x200, 0), a_adpcm(0, 0x00FFF000u), run();
        for (ok = 1, i = 0; i < 16; i++) ok &= get16(0x200 + 2 * i) == 0;
        CHECK(ok && asp_hle_stats()->bad_addr == bad0 + 3, "bad address: %u", asp_hle_stats()->bad_addr - bad0);
    }
    a_setbuff(0, 0, 0, 0), C(0x1F000000u, 0), C(0, 0), run();
    CHECK(asp_hle_stats()->cmds[32] == 1 && asp_hle_stats()->cmds[SPNOOP] == 1, "opcode counters");
}

/* ---- synthetic 32-voice workload (hash across builds, benchmark) ---------------------- */

#define NV 32
static int g_bench_build;   /* workload_frame leaves the list in g_cmds instead of running it */
static struct {
    uint32_t pitch, frac, fpos, rom;
    int16_t vol[2], tgt[2], dry, wet;
    int32_t rate[2];
} g_v[NV];

static void workload_setup(int sharp_lut)
{
    int v, i;
    int16_t lut[256], book[128], pole[16];
    reset();
    lut_cr(lut, sharp_lut), asp_hle_set_resample_lut(lut);
    for (v = 0; v < NV; v++) {
        int32_t lim = v == 7 ? 30000 : 4000;   /* voice 7: a book outside the int32 bound */
        g_v[v].pitch = (uint32_t)rnd_range(0x3000, 0xFFFE);
        /* the waves at 0x60000.. (Wings64-X had them at 0x10000.., over the states and the output) */
        g_v[v].frac = 0, g_v[v].fpos = 0, g_v[v].rom = 0x60000 + (uint32_t)v * 0x2000 + (rnd() & 31);
        g_v[v].vol[0] = (int16_t)rnd_range(0, 0x7FFF), g_v[v].vol[1] = (int16_t)rnd_range(0, 0x7FFF);
        g_v[v].tgt[0] = (int16_t)rnd_range(0, 0x7FFF), g_v[v].tgt[1] = (int16_t)rnd_range(0, 0x7FFF);
        /* integer rates (no libm in the hashed workload): towards the target, x0.8 .. x1.25 per 8 samples */
        g_v[v].rate[0] = g_v[v].tgt[0] >= g_v[v].vol[0] ? rnd_range(0x10000, 0x14000) : rnd_range(0xCCCC, 0x10000);
        g_v[v].rate[1] = g_v[v].tgt[1] >= g_v[v].vol[1] ? rnd_range(0x10000, 0x14000) : rnd_range(0xCCCC, 0x10000);
        g_v[v].dry = (int16_t)rnd_range(0x2000, 0x7FFF), g_v[v].wet = (int16_t)rnd_range(0, 0x3000);
        for (i = 0; i < 128; i++) book[i] = (int16_t)rnd_range(-lim, lim);
        ram_put16(0x8000 + (uint32_t)v * 256, book, 128);
        for (i = 0; i < 0x2000; i++) RB(g_v[v].rom + (uint32_t)i) = (uint8_t)rnd();
        for (i = 0; i < 0x2000 / 9; i++) RB(g_v[v].rom + 9u * (uint32_t)i) = (uint8_t)((rnd() % 13) << 4 | rnd() % 8);
    }
    memset(pole, 0, sizeof pole);   /* an integer _init_lpfilter-like table: fc^(i+1), Q14 */
    for (pole[8] = 0x2000, i = 9; i < 16; i++) pole[i] = (int16_t)((pole[i - 1] * 0x2000) >> 14);
    ram_put16(0x9000, pole, 16);
}

/* One alAudioFrame: 368 samples in chunks of 160, 160, 48 (AL_MAX_RSP_SAMPLES). */
static void workload_frame(int frame)
{
    static const int chunks[3] = {160, 160, 48};
    int ci, v, off = 0;
    for (ci = 0; ci < 3; ci++) {
        int n = chunks[ci];
        int first = frame == 0 && ci == 0;
        a_segment(0, 0);
        a_clearbuff(1088, (uint32_t)n * 2), a_clearbuff(1408, (uint32_t)n * 2);
        a_clearbuff(1728, (uint32_t)n * 2), a_clearbuff(2048, (uint32_t)n * 2);
        for (v = 0; v < NV; v++) {
            uint32_t step = g_v[v].pitch << 1, st = 0x20000 + (uint32_t)v * 256;
            uint32_t incount = (uint32_t)(((uint64_t)g_v[v].frac + (uint64_t)n * step) >> 16);
            uint32_t nf = (incount + 15) / 16 + 1, src, skew, nbytes;
            g_v[v].frac = (uint32_t)(((uint64_t)g_v[v].frac + (uint64_t)n * step) & 0xFFFF);
            if (g_v[v].fpos + nf > 0x2000 / 9 - 2) g_v[v].fpos = 0;
            src = g_v[v].rom + 9 * g_v[v].fpos, skew = src & 7, nbytes = 9 * nf + skew;
            g_v[v].fpos += nf - 1;
            a_loadadpcm(256, 0x8000 + (uint32_t)v * 256);
            a_setbuff(0, 0, 0, nbytes + 8 - (nbytes & 7)), a_loadbuff(src - skew);
            if (v % 5 == 4 && !first) a_setloop(st + 192);
            a_setbuff(0, skew, 320, nf * 32), a_adpcm(first ? F_INIT : (v % 5 == 4 ? F_LOOP : 0), st);
            a_setbuff(0, 352, 0, (uint32_t)n * 2), a_resample(first ? F_INIT : 0, g_v[v].pitch, st + 64);
            a_setbuff(0, 0, 1088, (uint32_t)n * 2), a_setbuff(F_AUX, 1408, 1728, 2048);
            if (first || (frame % 7 == 3 && ci == 1 && v % 3 == 0)) {
                a_setvol(F_LEFT | F_VOL, g_v[v].vol[0], 0, 0), a_setvol(F_VOL, g_v[v].vol[1], 0, 0);
                a_setvol(F_LEFT, g_v[v].tgt[0], (int)((uint32_t)g_v[v].rate[0] >> 16), g_v[v].rate[0] & 0xFFFF);
                a_setvol(0, g_v[v].tgt[1], (int)((uint32_t)g_v[v].rate[1] >> 16), g_v[v].rate[1] & 0xFFFF);
                a_setvol(F_AUX, g_v[v].dry, 0, g_v[v].wet);
                a_envmixer(F_INIT | F_AUX, st + 128);
            } else {
                a_envmixer(F_AUX, st + 128);
            }
        }
        /* reverb-shaped: reverb.c's mixes, a delay line, a lowpass, the main bus */
        a_setbuff(0, 0, 0, (uint32_t)n * 2);
        a_mixer(0xDA83, 1728, 1728), a_mixer(0x5A82, 2048, 1728);
        a_setbuff(0, 0, 1728, (uint32_t)n * 2), a_savebuff(0x50000 + (uint32_t)((frame * 368 + off) % 4096) * 2);
        a_clearbuff(2048, (uint32_t)n * 2);
        a_setbuff(0, 0, 0, (uint32_t)n * 2), a_loadbuff(0x50000 + (uint32_t)((frame * 368 + off + 1500) % 4096) * 2);
        a_setbuff(0, 0, 0, (uint32_t)n * 2), a_mixer(0x4000, 0, 320);
        a_setbuff(0, 320, 320, (uint32_t)n * 2), a_loadadpcm(32, 0x9000), a_polef(first, 0x2000, 0x9100);
        a_setbuff(0, 0, 0, (uint32_t)n * 2), a_mixer(0x3000, 320, 2048);
        a_dmemmove(2048, 1728, (uint32_t)n * 2);
        a_setbuff(0, 0, 0, (uint32_t)n * 2), a_mixer(0x7FFF, 1728, 1088), a_mixer(0x7FFF, 2048, 1408);
        a_setbuff(0, 0, 0, (uint32_t)n * 2), a_interleave(1088, 1408);
        a_setbuff(0, 0, 0, (uint32_t)n * 4), a_savebuff(0x40000 + (uint32_t)off * 4);
        off += n;
    }
    if (!g_bench_build) run();
}

static uint32_t fnv(uint32_t h, const uint8_t* p, size_t n)
{
    while (n--) h = (h ^ *p++) * 16777619u;
    return h;
}

/* RAM halfwords in N64 order, as native s16: the same bytes in either layout */
static uint32_t fnv_ram(uint32_t h, uint32_t a, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i += 2) {
        int16_t v = ram16(a + i);
        h = fnv(h, (const uint8_t*)&v, 2);
    }
    return h;
}

static uint32_t g_rhash;   /* RAM only: the same in the native and the word-swapped builds */

static uint32_t workload_hash(void)
{
    uint32_t h = 2166136261u, r = 2166136261u;
    int pass, f;
    for (pass = 0; pass < 2; pass++) {
        g_rng = 0xC0FFEEu + (uint32_t)pass;
        workload_setup(pass);
        for (f = 0; f < 40; f++) {
            workload_frame(f);
            r = fnv_ram(r, 0x40000, 368 * 4);
        }
        r = fnv_ram(r, 0x20000, NV * 256);
        r = fnv_ram(r, 0x50000, 8192);
        h = fnv(h ^ r, asp_hle_dmem(), 4096);
    }
    g_rhash = r;
    return h;
}

static void test_workload(void)
{
    uint32_t a = workload_hash(), b = workload_hash();
    int i, loud = 0;
    CHECK(a == b, "workload deterministic");
    for (i = 0; i < 368 * 2; i++) loud |= ram16(0x40000 + 2 * (uint32_t)i) != 0;
    CHECK(loud, "workload produces sound");
    printf("hash %08x\n", a);
    printf("rhash %08x\n", g_rhash);
}

/* ---- the registry and the other ABIs (asp_abis.inc) --------------------------------- */

/* A synthetic data segment head: the constants block (or none), the jump
 * table (entry i = 0x1000 + 0x40 i, or opcode 0's when i is not in
 * `present`), the nibble masks. */
static uint32_t data_head(uint8_t* d, int consts, int entries, uint32_t present)
{
    static const uint8_t masks[8] = {0xF0, 0x00, 0x0F, 0x00, 0x00, 0xF0, 0x00, 0x0F};
    uint32_t base = consts ? 0x10 : 0, i;
    memset(d, 0, 0x80);
    if (consts) d[3] = 1, d[5] = 2, d[6] = d[7] = 0xFF;
    for (i = 0; i < (uint32_t)entries; i++) {
        uint32_t v = (i && (present >> i & 1)) ? 0x1000 + 0x40 * i : 0x1118;
        d[base + 2 * i] = (uint8_t)(v >> 8), d[base + 2 * i + 1] = (uint8_t)v;
    }
    memcpy(d + base + 2 * (uint32_t)entries, masks, 8);
    return 0x80;
}

static void test_identify(void)
{
    uint8_t d[0x80];
    asp_ucode_t u;
    uint32_t n;
    n = data_head(d, 1, 16, 0xFFFF);
    CHECK(asp_hle_identify(d, n, &u) == ASP_ABI_AUDIO && u.entries == 16 && u.present == 0xFFFF, "aspMain");
    n = data_head(d, 1, 32, 0x007FFDA7u);   /* no ADDMIXER, POLEF at 14 */
    CHECK(asp_hle_identify(d, n, &u) == ASP_ABI_NEAD && u.flags == (ASP_NEAD_MK | ASP_NEAD_OLD_INTERL) &&
              u.present == 0x007FFDA7u, "nead, first: %x %x", (unsigned)u.abi, (unsigned)u.flags);
    n = data_head(d, 1, 32, 0x087FBDF7u);   /* ADDMIXER, no 14, FILTER at 27 */
    CHECK(asp_hle_identify(d, n, &u) == ASP_ABI_NEAD && u.flags == 0, "nead, new interleave: %x", (unsigned)u.flags);
    n = data_head(d, 1, 24, 0x007FFFF7u);
    CHECK(asp_hle_identify(d, n, &u) == ASP_ABI_NEAD && u.flags == ASP_NEAD_SHORT && u.entries == 24, "nead, 24");
    n = data_head(d, 0, 16, 0xFE7Fu);
    CHECK(asp_hle_identify(d, n, &u) == ASP_ABI_NAUDIO && u.present == 0xFE7Fu, "n_aspMain");
    n = data_head(d, 1, 20, 0xFFFFF);
    CHECK(asp_hle_identify(d, n, &u) == ASP_ABI_UNKNOWN, "20 entries: unknown");
    memset(d, 0x5A, sizeof d);
    CHECK(asp_hle_identify(d, sizeof d, &u) == ASP_ABI_UNKNOWN, "no masks: unknown");
    CHECK(asp_hle_identify(d, 0, &u) == ASP_ABI_UNKNOWN, "no data: unknown");
    /* unknown: nothing runs */
    reset();
    data_head(d, 1, 20, 0xFFFFF), asp_hle_identify(d, 0x80, &u), asp_hle_set_ucode(&u);
    dm16()[0x100] = 77;
    C((uint32_t)CLEARBUFF << 24 | 0x200, 16), run();
    CHECK(dm16()[0x100] == 77, "unknown ABI runs nothing");
}

static void use_ucode(uint32_t abi, uint32_t flags, uint32_t present)
{
    asp_ucode_t u;
    u.abi = abi, u.flags = flags, u.present = present, u.entries = abi == ASP_ABI_NEAD ? 32 : 16;
    asp_hle_set_ucode(&u);
}
#define NEAD_MK_OPS 0x007FFDA7u     /* what the first nead implements */
#define NEAD_ALL_OPS 0x07FFFFFFu
#define NEAD_MK (ASP_NEAD_MK | ASP_NEAD_OLD_INTERL)
#define NA_OPS 0xFE7Fu
enum { N_ADDMIXER = 4, N_ZOH = 6, N_COPYBLOCKS = 16, N_HALF = 17, N_ENVSETUP1 = 18, N_ENVMIXER = 19, N_LOADBUFF = 20,
       N_SAVEBUFF = 21, N_ENVSETUP2 = 22, N_HILOGAIN = 24, N_DUPLICATE = 26 };

static void test_nead_dma(void)
{
    static uint8_t src[4096];
    int trial, i, ok;
    for (trial = 0; trial < 200; trial++) {
        uint32_t cnt = (uint32_t)rnd_range(1, 0xFFF), dmem = (uint32_t)rnd_range(0, 0xFFF) & ~3u;
        uint32_t a = 0x10000 + ((uint32_t)rnd_range(0, 0x7FF) & ~7u), b = 0x30000 + ((uint32_t)rnd_range(0, 0x7FF) & ~7u);
        uint32_t n = (cnt + 7) & ~7u;
        reset(), use_ucode(ASP_ABI_NEAD, NEAD_MK, NEAD_MK_OPS);
        for (i = 0; i < 4096; i++) src[i] = (uint8_t)rnd();
        ram_putb(a, src, n);
        C((uint32_t)N_LOADBUFF << 24 | cnt << 12 | dmem, a), C((uint32_t)N_SAVEBUFF << 24 | cnt << 12 | dmem, b), run();
        for (ok = 1, i = 0; i < (int)n; i++) ok &= RB(b + (uint32_t)i) == src[i];
        CHECK(ok, "trial %d: LOADBUFF + SAVEBUFF %#x bytes at %#x", trial, (unsigned)cnt, (unsigned)dmem);
        CHECK(get16(dmem) == ram16(a), "trial %d: DMEM lanes hold RAM's halfwords", trial);
    }
}

static void test_nead_envmixer(void)
{
    static int16_t want[2048];
    int trial, i, k;
    for (trial = 0; trial < 400; trial++) {
        int mk = trial & 1, n = rnd_range(1, 255), swap = !mk && (trial & 2), neg[4], groups = (n + 7) & ~7;
        uint32_t in = 16 * (uint32_t)rnd_range(0, 15), outs[4], w0, w1;
        uint16_t env[3], env0[3], step[3], st[3];
        int16_t* o[4];
        reset();
        if (mk) use_ucode(ASP_ABI_NEAD, NEAD_MK, NEAD_MK_OPS);
        else use_ucode(ASP_ABI_NEAD, 0, NEAD_ALL_OPS);
        for (k = 0; k < 4; k++) outs[k] = 0x400 + 0x200 * (uint32_t)k + 16 * (uint32_t)rnd_range(0, 8);
        for (i = 0; i < 2048; i++) dm16()[i] = trial % 5 ? rnd_s16() : (int16_t)(rnd() & 1 ? 32767 : -32768);
        for (k = 0; k < 3; k++) env[k] = (uint16_t)rnd(), step[k] = (uint16_t)rnd();
        env[2] &= 0xFF00;
        memcpy(env0, env, sizeof env);
        neg[0] = (int)(rnd() & 1), neg[1] = (int)(rnd() & 1);
        neg[2] = !mk && (rnd() & 1), neg[3] = !mk && (rnd() & 1);
        memcpy(want, dm16(), sizeof want);
        o[0] = want + (outs[0] >> 1), o[1] = want + (outs[1] >> 1);
        o[2] = want + (outs[swap ? 3 : 2] >> 1), o[3] = want + (outs[swap ? 2 : 3] >> 1);
        st[0] = step[0], st[1] = step[1], st[2] = mk ? 0 : step[2];
        ref_envmix_nead(want + (in >> 1), groups, env, st, neg, o);
        C((uint32_t)N_ENVSETUP1 << 24 | (uint32_t)(env0[2] >> 8) << 16 | step[2], (uint32_t)step[0] << 16 | step[1]);
        C((uint32_t)N_ENVSETUP2 << 24, (uint32_t)env0[0] << 16 | env0[1]);
        w0 = (uint32_t)N_ENVMIXER << 24 | (in >> 4) << 16 | (uint32_t)n << 8 | (uint32_t)swap << 4 |
             (uint32_t)neg[2] << 3 | (uint32_t)neg[3] << 2 | (uint32_t)neg[0] << 1 | (uint32_t)neg[1];
        w1 = (outs[0] >> 4) << 24 | (outs[1] >> 4) << 16 | (outs[2] >> 4) << 8 | (outs[3] >> 4);
        C(w0, w1), run();
        CHECK(!memcmp(want, dm16(), sizeof want), "trial %d (mk %d, n %d, swap %d)", trial, mk, n, swap);
    }
}

static void test_nead_misc(void)
{
    static int16_t want[2048], L[512], R[512], il[1024];
    int trial, i, k;
    for (trial = 0; trial < 450; trial++) {
        int kind = trial % 9;
        uint32_t cnt = 16 * (uint32_t)rnd_range(1, 40), a = 2 * (uint32_t)rnd_range(0, 300);
        uint32_t b = 0x800 + 2 * (uint32_t)rnd_range(0, 300);
        int16_t g = rnd_s16();
        reset(), use_ucode(ASP_ABI_NEAD, (trial & 1) ? 0 : NEAD_MK, NEAD_ALL_OPS);
        for (i = 0; i < 2048; i++) dm16()[i] = rnd_s16();
        memcpy(want, dm16(), sizeof want);
        switch (kind) {
        case 0:   /* MIXER, the count in w0 */
            ref_mix(want + (b >> 1), want + (a >> 1), (int)cnt / 2, g);
            C((uint32_t)MIXER << 24 | (cnt >> 4) << 16 | (uint16_t)g, a << 16 | b);
            break;
        case 1:   /* ADDMIXER */
            for (i = 0; i < (int)cnt / 2; i++) {
                int32_t v = want[(b >> 1) + i] + want[(a >> 1) + i];
                want[(b >> 1) + i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
            }
            C((uint32_t)N_ADDMIXER << 24 | (cnt >> 4) << 16 | 0x7FFF, a << 16 | b);
            break;
        case 2:   /* INTERLEAVE: the old (SETBUFF's out and count) or the new (explicit) */
            for (i = 0; i < (int)cnt / 2; i++) L[i] = want[(a >> 1) + i], R[i] = want[(0x600 >> 1) + i];
            ref_interleave(il, L, R, (int)cnt / 2);
            memcpy(want + (b >> 1), il, cnt * 2);
            if (trial & 1) C((uint32_t)INTERLEAVE << 24 | (cnt >> 4) << 16 | b, a << 16 | 0x600);
            else a_setbuff(0, 0, b, cnt), C((uint32_t)INTERLEAVE << 24, a << 16 | 0x600);
            break;
        case 3: { /* DMEMMOVE2: blocks copied forward 32 bytes at a time, the source following */
            uint32_t reps = (uint32_t)rnd_range(0, 4), bs = 32 * (uint32_t)rnd_range(1, 4), si = a, so = a + bs, r;
            uint8_t* wb = (uint8_t*)want;
            for (r = reps ? reps : 1; r; r--)
                for (k = 0; k < (int)bs; k += 32, si += 32, so += 32) memmove(wb + so, wb + si, 32);
            C((uint32_t)N_COPYBLOCKS << 24 | reps << 16 | a, (a + bs) << 16 | bs);
            break;
        }
        case 4:   /* DOWNSAMPLE_HALF */
            for (i = 0; i < (int)cnt / 2; i++) want[(b >> 1) + i] = want[(a >> 1) + 2 * i];
            C((uint32_t)N_HALF << 24 | cnt / 2, a << 16 | b);
            break;
        case 5: { /* DUPLICATE: 128 bytes, c copies */
            uint32_t c = (uint32_t)rnd_range(0, 6);
            for (k = 0; k < (int)c; k++) memcpy(want + (b >> 1) + 64 * k, dm16() + (a >> 1), 128);
            C((uint32_t)N_DUPLICATE << 24 | c << 16 | a, b << 16 | 0x80);
            break;
        }
        case 6: { /* HILOGAIN: signed Q4.4, floored */
            int8_t q = (int8_t)rnd();
            for (i = 0; i < (int)cnt / 2; i++) {
                int32_t v = (int32_t)want[(a >> 1) + i] * q;
                v = v >= 0 ? v / 16 : -((-v + 15) / 16);
                want[(a >> 1) + i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
            }
            C((uint32_t)N_HILOGAIN << 24 | (uint32_t)(uint8_t)q << 16 | cnt, a << 16);
            break;
        }
        case 7: { /* RESAMPLE_ZOH: output j = input floor((fr + j * step) / 65536) */
            uint32_t pitch = rnd() & 0xFFFF, fr = rnd() & 0xFFFF;
            for (i = 0; i < (int)cnt / 2; i++)
                want[(b >> 1) + i] = dm16()[(a >> 1) + (uint32_t)(((uint64_t)fr + (uint64_t)i * (pitch << 1)) >> 16)];
            a_setbuff(0, a, b, cnt), C((uint32_t)N_ZOH << 24 | pitch, fr);
            break;
        }
        case 8:   /* CLEARBUFF (the exact count) and DMEMMOVE */
            memset((uint8_t*)want + a, 0, cnt - 2);
            memmove((uint8_t*)want + b, (uint8_t*)want + 0x600, cnt);
            C((uint32_t)CLEARBUFF << 24 | a, cnt - 2), C((uint32_t)DMEMMOVE << 24 | 0x600, b << 16 | cnt);
            break;
        }
        run();
        for (i = 0; i < 2048; i++)
            if (want[i] != dm16()[i]) break;
        CHECK(i == 2048, "trial %d kind %d: lane %#x %d vs %d", trial, kind, i, dm16()[i & 2047], want[i & 2047]);
    }
    /* an opcode the microcode lacks is skipped and counted */
    reset(), use_ucode(ASP_ABI_NEAD, NEAD_MK, NEAD_MK_OPS);
    dm16()[0x100] = 5, dm16()[0x600] = 7;
    C((uint32_t)N_ADDMIXER << 24 | 1u << 16, 0x600u << 16 | 0x200), C((uint32_t)30 << 24, 0), run();
    CHECK(dm16()[0x100] == 5 && asp_hle_stats()->cmds[32] == 2, "lacking opcodes skipped");
}

/* ADPCM (4- and 2-bit) and RESAMPLE through the nead encoding */
static void test_nead_adpcm_resample(void)
{
    static int16_t ref[16 * 64], z[4 + 2048], rout[1024];
    static uint8_t stream[9 * 64];
    int16_t book[128], lut[256];
    int trial, i, k;
    for (trial = 0; trial < 200; trial++) {
        int two = trial & 1, nf = rnd_range(1, 60), fb = two ? 5 : 9;
        int16_t hist[16];
        uint32_t state = 0x800, nbytes = (uint32_t)(nf * fb + 7) & ~7u;
        reset(), use_ucode(ASP_ABI_NEAD, NEAD_MK, NEAD_MK_OPS);
        for (i = 0; i < 128; i++) book[i] = (int16_t)rnd_range(-7000, 7000);
        for (i = 0; i < fb * nf; i++) stream[i] = (uint8_t)rnd();
        for (i = 0; i < nf; i++) stream[fb * i] = (uint8_t)((rnd() % 16) << 4 | (rnd() % 16));
        memset(hist, 0, sizeof hist);
        if (two) ref_adpcm2(book, stream, nf, hist, ref);
        else ref_adpcm(book, stream, nf, hist, ref);
        ram_put16(0x100, book, 128);
        ram_putb(0x10000, stream, (size_t)(nf * fb));
        C((uint32_t)LOADADPCM << 24 | 256, 0x100);
        C((uint32_t)N_LOADBUFF << 24 | nbytes << 12, 0x10000);
        a_setbuff(0, 0, 0x400, (uint32_t)nf * 32);
        C(W0(ADPCM, F_INIT | (two ? 4 : 0), 0), state), run();
        for (k = 0; k < 16 * nf; k++)
            if (get16(0x420 + 2 * (uint32_t)k) != ref[k]) break;
        CHECK(k == 16 * nf, "trial %d (2-bit %d): sample %d: %d vs ref %d", trial, two, k, get16(0x420 + 2 * (uint32_t)k),
              ref[k]);
        for (k = 0; k < 16; k++) CHECK(ram16(state + 2 * (uint32_t)k) == hist[k], "trial %d state %d", trial, k);
    }
    for (trial = 0; trial < 40; trial++) {
        uint32_t pitch = (uint32_t)rnd_range(1, 0xFFFE);
        reset(), use_ucode(ASP_ABI_NEAD, 0, NEAD_ALL_OPS);
        lut_cr(lut, trial & 1);
        asp_hle_set_resample_lut(lut);
        memset(z, 0, 8);
        for (i = 4; i < 4 + 2048; i++) z[i] = rnd_s16();
        ref_resample(z, 256, pitch, 0, lut, PER_TAP, rout);
        put16(0x140, z + 4, 1024);
        a_setbuff(0, 0x140, 0xC00, 512), C(W0(RESAMPLE, F_INIT, pitch), 0x600), run();
        for (k = 0; k < 256; k++)
            if (get16(0xC00 + 2 * (uint32_t)k) != rout[k]) break;
        CHECK(k == 256, "resample trial %d pitch %#x out %d", trial, (unsigned)pitch, k);
    }
}

/* FILTER (the Zelda microcode's opcode 7): an 8-tap FIR, taps = the average
 * of the table and the state's previous copy, history = the state's 8 samples */
static void test_nead_filter(void)
{
    static int16_t x[8 + 640], want[640];
    int trial, i, j;
    for (trial = 0; trial < 100; trial++) {
        int16_t tab[8], st[16], h[8];
        int n = 8 * rnd_range(1, 80), op = trial & 1 ? 27 : 7;
        uint32_t d = 0x200 + 16 * (uint32_t)rnd_range(0, 20), sa = 0x3000, ta = 0x3100;
        reset();
        if (op == 7) use_ucode(ASP_ABI_NEAD, ASP_NEAD_SHORT, 0x00FFFFFFu);
        else use_ucode(ASP_ABI_NEAD, 0, NEAD_ALL_OPS | 1u << 27);
        for (j = 0; j < 8; j++) tab[j] = (int16_t)rnd_range(-12000, 20000);
        for (j = 0; j < 16; j++) st[j] = j < 8 ? rnd_s16() : (int16_t)rnd_range(-12000, 20000);
        for (j = 0; j < 8; j++) h[j] = (int16_t)(((int32_t)tab[j] + st[8 + j]) >= 0 ? ((int32_t)tab[j] + st[8 + j]) / 2
                                                   : -((-((int32_t)tab[j] + st[8 + j]) + 1) / 2));
        memcpy(x, st, 16);
        for (i = 0; i < n; i++) x[8 + i] = dm16()[(d >> 1) + (uint32_t)i] = rnd_s16();
        for (i = 0; i < n; i++) {
            int64_t acc = 0;
            for (j = 0; j < 8; j++) acc += (int64_t)x[8 + i - j] * h[j];
            acc = (acc + 0x4000) >= 0 ? (acc + 0x4000) / 32768 : -((-(acc + 0x4000) + 32767) / 32768);
            want[i] = (int16_t)(acc > 32767 ? 32767 : acc < -32768 ? -32768 : acc);
        }
        ram_put16(ta, tab, 8), ram_put16(sa, st, 16);
        C((uint32_t)op << 24 | 2u << 16 | (uint32_t)n * 2, ta), C((uint32_t)op << 24 | d, sa), run();
        for (i = 0; i < n; i++)
            if (get16(d + 2 * (uint32_t)i) != want[i]) break;
        CHECK(i == n, "trial %d op %d: sample %d: %d vs %d", trial, op, i, get16(d + 2 * (uint32_t)i), want[i % n]);
        for (j = 0; j < 8; j++)
            CHECK(ram16(sa + 2 * (uint32_t)j) == x[n + j] && ram16(sa + 16 + 2 * (uint32_t)j) == h[j] &&
                      ram16(ta + 2 * (uint32_t)j) == tab[j], "trial %d state %d", trial, j);
    }
}

/* n_aspMain: fixed buffers relative to N_MAIN 0x4F0, 0x170 bytes (184 samples) a call */
#define NA_MAIN 0x4F0u
#define NA_CNT 184
static void test_naudio(void)
{
    static int16_t want[2048];
    int trial, i, k;
    for (trial = 0; trial < 120; trial++) {
        int16_t vl = rnd_s16(), vr = rnd_s16(), tl = rnd_s16(), tr = rnd_s16(), dry = rnd_s16(), wet = rnd_s16();
        int32_t rl = (int32_t)rnd() >> rnd_range(4, 16), rr = (int32_t)rnd() >> rnd_range(4, 16);
        RefLin e;
        int16_t* o[4];
        uint32_t state = 0x900;
        int pass;
        reset(), use_ucode(ASP_ABI_NAUDIO, 0, NA_OPS);
        e.value[0] = (int32_t)((uint32_t)(uint16_t)vl << 16), e.value[1] = (int32_t)((uint32_t)(uint16_t)vr << 16);
        e.target[0] = (int32_t)((uint32_t)(uint16_t)tl << 16), e.target[1] = (int32_t)((uint32_t)(uint16_t)tr << 16);
        e.step[0] = rl / 8, e.step[1] = rr / 8, e.dry = dry, e.wet = wet;
        for (pass = 0; pass < 3; pass++) {
            for (i = 0; i < 2048; i++) dm16()[i] = rnd_s16();
            memcpy(want, dm16(), sizeof want);
            o[0] = want + (0x9D0 >> 1), o[1] = want + (0xB40 >> 1), o[2] = want + (0xCB0 >> 1), o[3] = want + (0xE20 >> 1);
            ref_envmix_lin(want + (NA_MAIN >> 1), NA_CNT, &e, o);
            if (pass == 0) {
                C((uint32_t)SETVOL << 24 | (F_VOL | F_LEFT) << 16 | (uint16_t)vl, (uint32_t)(uint16_t)dry << 16 | (uint16_t)wet);
                C((uint32_t)SETVOL << 24 | F_VOL << 16 | (uint16_t)tr, (uint32_t)rr ^ 0x5A5A);
                C((uint32_t)POLEF << 24, (uint32_t)rr & 0xFFFF);   /* opcode 14: the low half of the right rate */
                C((uint32_t)SETVOL << 24 | (uint16_t)tl, (uint32_t)rl);
            }
            C((uint32_t)ENVMIXER << 24 | (uint32_t)(pass ? 0 : F_INIT) << 16 | (uint16_t)vr, state), run();
            for (i = 0; i < 2048; i++)
                if (want[i] != dm16()[i]) break;
            CHECK(i == 2048, "trial %d pass %d: lane %#x %d vs %d", trial, pass, i, dm16()[i & 2047], want[i & 2047]);
        }
    }
    /* MIXER, INTERLEAVE and the buffers at the fixed offsets */
    for (trial = 0; trial < 60; trial++) {
        static int16_t L[NA_CNT], R[NA_CNT], il[2 * NA_CNT];
        static uint8_t src[512];
        int16_t g = rnd_s16();
        reset(), use_ucode(ASP_ABI_NAUDIO, 0, NA_OPS);
        for (i = 0; i < 2048; i++) dm16()[i] = rnd_s16();
        memcpy(want, dm16(), sizeof want);
        ref_mix(want + ((NA_MAIN + 0x170) >> 1), want + ((NA_MAIN + 0x2E0) >> 1), NA_CNT, g);
        C((uint32_t)MIXER << 24 | (uint16_t)g, 0x2E0u << 16 | 0x170u), run();
        CHECK(!memcmp(want, dm16(), sizeof want), "trial %d: MIXER", trial);
        for (i = 0; i < NA_CNT; i++) L[i] = want[(0x9D0 >> 1) + i], R[i] = want[(0xB40 >> 1) + i];
        ref_interleave(il, L, R, NA_CNT);
        memcpy(want + (NA_MAIN >> 1), il, sizeof il);
        C((uint32_t)INTERLEAVE << 24, 0), run();
        CHECK(!memcmp(want, dm16(), sizeof want), "trial %d: INTERLEAVE", trial);
        for (i = 0; i < 512; i++) src[i] = (uint8_t)rnd();
        ram_putb(0x20000, src, 512);
        C((uint32_t)LOADBUFF << 24 | 0x170u << 12 | 0x170u, 0x20000);
        C((uint32_t)SAVEBUFF << 24 | 0x170u << 12 | 0x170u, 0x28000);
        C((uint32_t)CLEARBUFF << 24 | 0x170u, 0x20), run();
        for (k = 1, i = 0; i < 0x170; i++) k &= RB(0x28000 + (uint32_t)i) == src[i];
        CHECK(k && get16(NA_MAIN + 0x170) == 0 && get16(NA_MAIN + 0x190) == ram16(0x20020),
              "trial %d: LOADBUFF, SAVEBUFF, CLEARBUFF", trial);
    }
    {   /* ADPCM: state in w0, the fields of w1; RESAMPLE into N_MAIN2 */
        static int16_t ref[16 * 12], z[4 + 512], rout[NA_CNT];
        static uint8_t stream[9 * 12];
        int16_t book[128], hist[16], lut[256];
        reset(), use_ucode(ASP_ABI_NAUDIO, 0, NA_OPS);
        for (i = 0; i < 128; i++) book[i] = (int16_t)rnd_range(-4000, 4000);
        for (i = 0; i < 9 * 12; i++) stream[i] = (uint8_t)rnd();
        for (i = 0; i < 12; i++) stream[9 * i] &= 0xB7;
        memset(hist, 0, sizeof hist);
        ref_adpcm(book, stream, 12, hist, ref);
        ram_put16(0x100, book, 128), ram_putb(0x10000, stream, 9 * 12);
        C((uint32_t)LOADADPCM << 24 | 256, 0x100);
        C((uint32_t)LOADBUFF << 24 | 112u << 12 | 0x2E0u, 0x10000);
        C((uint32_t)DMEMMOVE << 24 | 0x2E0u, 0x8u << 16 | 112u);    /* the input 8 bytes in (the 4-bit field) */
        C((uint32_t)ADPCM << 24 | 0x800, 1u << 28 | 384u << 16 | 8u << 12 | 0x400u), run();
        for (k = 0; k < 16 * 12; k++)
            if (get16(NA_MAIN + 0x420 + 2 * (uint32_t)k) != ref[k]) break;
        CHECK(k == 16 * 12, "n_aspMain ADPCM sample %d", k);
        for (k = 0; k < 16; k++) CHECK(ram16(0x800 + 2 * (uint32_t)k) == hist[k], "n_aspMain ADPCM state %d", k);
        reset(), use_ucode(ASP_ABI_NAUDIO, 0, NA_OPS);
        lut_cr(lut, 0), asp_hle_set_resample_lut(lut);
        memset(z, 0, 8);
        for (i = 4; i < 4 + 512; i++) z[i] = rnd_s16();
        ref_resample(z, NA_CNT, 0x6000, 0, lut, PER_TAP, rout);
        put16(NA_MAIN + 0x800, z + 4, 512);
        C((uint32_t)RESAMPLE << 24 | 0x600, 1u << 30 | 0x6000u << 14 | 0x800u << 2 | 1u), run();
        for (k = 0; k < NA_CNT; k++)
            if (get16(0x660 + 2 * (uint32_t)k) != rout[k]) break;
        CHECK(k == NA_CNT, "n_aspMain RESAMPLE into N_MAIN2: out %d", k);
    }
}

/* nead and n_aspMain lists shaped like their synthesizers', hashed: the
 * same in every build of a group (SIMD = C), "rhash2" the same in both
 * RAM layouts */
static uint32_t g_rhash2;
static uint32_t abi2_hash(void)
{
    uint32_t h = 2166136261u, r = 2166136261u;
    int pass, f, v, i;
    int16_t book[128], lut[256];
    for (pass = 0; pass < 2; pass++) {
        g_rng = 0xA5A5u + (uint32_t)pass;
        reset();
        lut_cr(lut, pass), asp_hle_set_resample_lut(lut);
        for (i = 0; i < 128; i++) book[i] = (int16_t)rnd_range(-6000, 6000);
        ram_put16(0x100, book, 128);
        for (i = 0; i < 0x8000; i++) RB(0x10000 + (uint32_t)i) = (uint8_t)rnd();
        if (pass == 0) use_ucode(ASP_ABI_NEAD, NEAD_MK, NEAD_MK_OPS);
        else use_ucode(ASP_ABI_NEAD, 0, NEAD_ALL_OPS);
        for (f = 0; f < 20; f++) {
            C((uint32_t)CLEARBUFF << 24 | 0x400, 0x800);
            C((uint32_t)LOADADPCM << 24 | 256, 0x100);
            for (v = 0; v < 8; v++) {
                uint32_t st = 0x8000 + 0x100 * (uint32_t)v;
                C((uint32_t)N_LOADBUFF << 24 | 0x100u << 12, 0x10000 + 0x400 * (uint32_t)v + 0x90 * (uint32_t)f);
                a_setbuff(0, 0, 0x180, 0x200);
                C(W0(ADPCM, (f == 0 ? F_INIT : 0) | (v & 1 ? 4 : 0), 0), st);
                a_setbuff(0, 0x1A0, 0x380, 0x160);
                C(W0(RESAMPLE, f == 0 ? F_INIT : 0, 0x5000 + 0x900 * (uint32_t)v), st + 0x40);
                C((uint32_t)N_ENVSETUP1 << 24 | 0x40u << 16 | 0x0100, (uint32_t)(0x80 * v) << 16 | 0xFF80);
                C((uint32_t)N_ENVSETUP2 << 24, (uint32_t)(0x1000 * v + 0x800) << 16 | 0x9000);
                C((uint32_t)N_ENVMIXER << 24 | (0x380u >> 4) << 16 | 0xB0u << 8 | (uint32_t)(v & 0xF),
                  0x40u << 24 | 0x56u << 16 | 0x6Cu << 8 | 0x82u);
            }
            C((uint32_t)MIXER << 24 | (0x160u >> 4) << 16 | 0x5000, 0x6C0u << 16 | 0x400);
            a_setbuff(0, 0, 0xA00, 0x160);
            C((uint32_t)INTERLEAVE << 24 | (0x160u >> 4) << 16 | 0xA00, 0x400u << 16 | 0x560);
            C((uint32_t)N_SAVEBUFF << 24 | 0x2C0u << 12 | 0xA00, 0x40000 + 0x2C0 * (uint32_t)f);
            run();
            r = fnv_ram(r, 0x40000 + 0x2C0 * (uint32_t)f, 0x2C0);
        }
        asp_hle_reset(), use_ucode(ASP_ABI_NAUDIO, 0, NA_OPS);   /* RAM kept: the same wave data */
        lut_cr(lut, pass), asp_hle_set_resample_lut(lut);
        for (f = 0; f < 10; f++) {
            C((uint32_t)CLEARBUFF << 24 | 0x4E0u, 0x170 * 4);
            C((uint32_t)LOADADPCM << 24 | 256, 0x100);
            for (v = 0; v < 4; v++) {
                uint32_t st = 0x9000 + 0x100 * (uint32_t)v;
                C((uint32_t)LOADBUFF << 24 | 0xD8u << 12, 0x10000 + 0x800 * (uint32_t)v + 0xD8 * (uint32_t)f);
                C((uint32_t)ADPCM << 24 | st, (uint32_t)(f == 0) << 28 | 0x180u << 16 | 0x170u);
                C((uint32_t)RESAMPLE << 24 | (st + 0x40),
                  (uint32_t)(f == 0) << 30 | (0x6000u + 0x700 * (uint32_t)v) << 14 | 0x190u << 2);
                C((uint32_t)SETVOL << 24 | (F_VOL | F_LEFT) << 16 | 0x4000, 0x7000u << 16 | 0x2000);
                C((uint32_t)SETVOL << 24 | F_VOL << 16 | 0x6000, 0x30000);
                C((uint32_t)SETVOL << 24 | 0x5000, (uint32_t)-0x20000);
                C((uint32_t)ENVMIXER << 24 | (uint32_t)(f == 0) << 16 | 0x3000, st + 0x80);
            }
            C((uint32_t)INTERLEAVE << 24, 0);
            C((uint32_t)SAVEBUFF << 24 | 0x2E0u << 12, 0x60000 + 0x2E0 * (uint32_t)f);
            run();
            r = fnv_ram(r, 0x60000 + 0x2E0 * (uint32_t)f, 0x2E0);
        }
        h = fnv(h ^ r, asp_hle_dmem(), 4096);
    }
    g_rhash2 = r;
    return h;
}

static void test_abi2_workload(void)
{
    uint32_t a = abi2_hash(), b = abi2_hash();
    int i, loud = 0;
    CHECK(a == b, "nead/n_aspMain workload deterministic");
    for (i = 0; i < 0x2E0 / 2; i++) loud |= ram16(0x60000 + 2 * (uint32_t)i) != 0;
    CHECK(loud, "n_aspMain workload produces sound");
    printf("hash2 %08x\n", a);
    printf("rhash2 %08x\n", g_rhash2);
}

/* us per replay of a list: the best of 7 runs of `frames` replays (the host is shared) */
static double bench_list(const uint32_t* list, int n, int frames)
{
    double best = 1e30;
    int r, f;
    for (r = 0; r < 7; r++) {
        clock_t t0 = clock();
        double us;
        for (f = 0; f < frames; f++) asp_hle_run(list, (uint32_t)n, addr_cb, NULL);
        us = (double)(clock() - t0) * 1e6 / CLOCKS_PER_SEC / frames;
        if (us < best) best = us;
    }
    return best;
}

int main(int argc, char** argv)
{
    if (argc == 3 && !strcmp(argv[1], "--bench")) {
        /* One frame's list (after 4 warm-up frames, so states are continuing),
         * replayed; then replayed with one opcode turned into SPNOOP to split
         * the cost. Replays reuse the same input, so they time steady work. */
        static uint32_t list[2 * 16384], cut[2 * 16384];
        static const int ops[] = {ADPCM, RESAMPLE, ENVMIXER, MIXER, LOADBUFF};
        static const char* names[] = {"ADPCM", "RESAMPLE", "ENVMIXER", "MIXER", "LOADBUFF"};
        int frames = atoi(argv[2]), f, n, i, k;
        double full, part;
        g_rng = 0xBE7CBu;
        workload_setup(0);
        for (f = 0; f < 4; f++) workload_frame(f);
        g_bench_build = 1, workload_frame(4), g_bench_build = 0;
        n = g_n, g_n = 0;
        memcpy(list, g_cmds, sizeof(uint32_t) * 2 * (size_t)n);
        full = bench_list(list, n, frames);
        printf("bench %.1f us per 368-sample frame (32 voices, 3 chunks, reverb-shaped bus):", full);
        for (k = 0; k < 5; k++) {
            memcpy(cut, list, sizeof(uint32_t) * 2 * (size_t)n);
            for (i = 0; i < n; i++)
                if ((int)(cut[2 * i] >> 24) == ops[k]) cut[2 * i] = 0;
            part = bench_list(cut, n, frames);
            printf(" %s %.0f%%", names[k], 100.0 * (full - part) / full);
        }
        printf("\n");
        return 0;
    }
    test_dma_birdman();
    test_dma_roundtrip();
    test_adpcm_vectors();
    test_adpcm_random();
    test_adpcm_loop();
    test_resample_birdman();
    test_resample_random();
    test_lut_api();
    test_envmixer_birdman();
    test_envmixer_random();
    test_mixer();
    test_interleave();
    test_polef();
    test_segments();
    test_workload();
    test_identify();
    test_nead_dma();
    test_nead_envmixer();
    test_nead_misc();
    test_nead_adpcm_resample();
    test_nead_filter();
    test_naudio();
    test_abi2_workload();
    printf("%s: %d checks, %d failed\n", g_fail ? "FAIL" : "ok", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
