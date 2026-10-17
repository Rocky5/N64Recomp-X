/* jpeg.c - the JPEG decoder task (M_NJPEGTASK) as Ocarina of Time runs it
 * (z_jpeg.c in its decomp): the CPU Huffman-decodes up to four macroblocks
 * of DCT coefficients into RDRAM and the RSP turns each into 16x16 RGBA16
 * pixels in place, which the CPU copies to the screen (its pre-rendered
 * room backgrounds). Written here from the task's documented behaviour:
 * dequantize, un-zigzag, inverse DCT, YCbCr to RGB, RGBA5551.
 *
 * The task's data (big-endian words at data_ptr): +0 the macroblocks'
 * address, +4 their count, +8 the mode (0: 4:2:2, two Y blocks; 2: 4:2:0,
 * four), +0xC/+0x10/+0x14 the Y, U and V quantization tables (64 u16 each,
 * zigzag order like the coefficients). A macroblock is its Y blocks then U
 * then V, 64 s16 each; slots are 0x300 bytes apart in mode 2, 0x200 in
 * mode 0.
 *
 * The numbers (the s16 clamp and x16 of the dequantization, the float
 * transform's 16x scale, the colour constants around 2048, the 0..0xFF0
 * clamp) follow the RSP microcode's documented behaviour (the HLE plugins'
 * "PS" variant: Ocarina of Time, Pokemon Stadium 1 and 2).
 * DIVERGES: none meant. -DXRC_JPEG=0: the task is not run. */
#include <math.h>

#include "ultra/ultra.h"

#ifndef XRC_JPEG
#define XRC_JPEG 1
#endif

static const uint8_t k_zigzag[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

/* the 1D inverse transform's basis, unnormalized: 1 for the DC term, sqrt(2)
 * cos((2n + 1) k pi / 16) for the others; the 2D result is divided by 8 */
static float s_cos[8][8];
static int s_init;

static void init_tables(void) {
    int n, k;
    for (n = 0; n < 8; n++)
        for (k = 0; k < 8; k++)
            s_cos[n][k] = k ? 1.41421356f * cosf((float)((2 * n + 1) * k) * 3.14159265f / 16.0f) : 1.0f;
    s_init = 1;
}

static void block(uint32_t src, uint32_t q, int16_t out[64]) {
    float c[64], t[64];
    int i, x, y, u, v;
    for (i = 0; i < 64; i++) {
        int32_t p = (int32_t)(int16_t)rd16(src + 2u * i) * (int32_t)rd16(q + 2u * i);
        p = p > 32767 ? 32767 : p < -32768 ? -32768 : p;
        c[k_zigzag[i]] = (float)(int16_t)(uint16_t)(p << 4);
    }
    for (y = 0; y < 8; y++)   /* rows: v -> y */
        for (u = 0; u < 8; u++) {
            float s = 0.0f;
            for (v = 0; v < 8; v++) s += s_cos[y][v] * c[v * 8 + u];
            t[y * 8 + u] = s;
        }
    for (y = 0; y < 8; y++)   /* columns: u -> x */
        for (x = 0; x < 8; x++) {
            float s = 0.0f;
            for (u = 0; u < 8; u++) s += s_cos[x][u] * t[y * 8 + u];
            out[y * 8 + x] = (int16_t)(s * 0.125f);
        }
}

/* a colour channel at 16x scale around 2048: truncated, clamped to 0..0xFF0, its top five bits */
static unsigned ch5(float f) {
    int i = (int)(int16_t)(int)f;
    i = i < 0 ? 0 : i > 0xFF0 ? 0xFF0 : i;
    return (unsigned)i >> 7;
}

/* 1: the task was this decoder's and ran */
int xrc_jpeg_task(uint32_t task) {
    uint32_t data = rd32(task + TK_DATA_PTR) | 0x80000000u, addr, count, mode, qy, qu, qv, m;
    static unsigned s_logged;
    if (!XRC_JPEG) return 0;
    addr = rd32(data) | 0x80000000u;
    count = rd32(data + 4);
    mode = rd32(data + 8);
    qy = rd32(data + 12) | 0x80000000u;
    qu = rd32(data + 16) | 0x80000000u;
    qv = rd32(data + 20) | 0x80000000u;
    if ((mode != 0 && mode != 2) || count == 0 || count > 16 || ((addr & 0x1FFFFFFFu) + count * 0x300u) > xrc_rdram_size) {
        if (s_logged++ < 4)
            xhw_logf("[JPEG] task %08x: address %08x count %u mode %u: not a decoder task this runs", (unsigned)task,
                     (unsigned)addr, (unsigned)count, (unsigned)mode);
        return 0;
    }
    if (!s_init) init_tables();
    if (s_logged++ < 1) xhw_logf("[JPEG] decoder task: %u macroblocks, mode %u", (unsigned)count, (unsigned)mode);
    for (m = 0; m < count; m++) {
        uint32_t mb = addr + m * (mode == 2 ? 0x300u : 0x200u);
        unsigned ny = mode == 2 ? 4 : 2, rows = mode == 2 ? 16 : 8, b, x, y;
        int16_t yb[4][64], ub[64], vb[64];
        for (b = 0; b < ny; b++) block(mb + b * 128u, qy, yb[b]);
        block(mb + ny * 128u, qu, ub);
        block(mb + (ny + 1) * 128u, qv, vb);
        for (y = 0; y < rows; y++)
            for (x = 0; x < 16; x++) {
                const int16_t* yblk = yb[(y >= 8 ? 2 : 0) + (x >= 8 ? 1 : 0)];
                float Y = (float)yblk[(y & 7) * 8 + (x & 7)] + 2048.0f;
                unsigned ci = (mode == 2 ? (y >> 1) : y) * 8 + (x >> 1);
                float cb = (float)ub[ci], cr = (float)vb[ci];
                unsigned r = ch5(Y + 1.4025f * cr), g = ch5(Y - 0.3443f * cb - 0.7144f * cr), bl = ch5(Y + 1.7729f * cb);
                wr16(mb + 2u * (y * 16 + x), (uint16_t)(r << 11 | g << 6 | bl << 1 | 1));
            }
    }
    return 1;
}
