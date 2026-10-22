/* f3d_interp.c - Fast3D (F3D GBI) display-list interpreter onto the NV2A
 * (PLAN.md 3.7). Wings64-X's xbox/src/gfx/f3d_interp.c (from wings64-dc's
 * gfx_f3d.c), moved onto the recompiled game's memory: display lists,
 * vertices, matrices, lights and textures are read from the word-swapped
 * RDRAM image by N64 address (words native, halfwords at ^2, bytes at ^3;
 * the texels are copied out in N64 byte order for texconv.c, the order
 * Wings64-X kept its textures in). Segment addresses resolve to N64
 * addresses, masked into the window.
 *
 * Runs synchronously on the thread that calls osSpTaskStartGo for a
 * graphics task (xbox/src/rt/ultra/sp.c); one task is one frame,
 * presented at its end. The CPU does the RSP's work: the display-list
 * walk, the 16-entry vertex cache, the matrix stack, lighting and texgen,
 * the fog factor, clipping (near, far, guard band) and culling. The NV2A
 * does the RDP's: the colour combiner (the G_SETCOMBINE words go to the
 * back end, nv2a_cc.c compiles them to register combiners), blending,
 * depth, alpha test, fog, two texture units. xgfx.h is the contract.
 *
 * Kept from the Dreamcast port, quirk by quirk (wings64-dc kb/traps.md):
 * the texrect encoding (E4 + B3 + B2), UVBT tile rows clamped to the loaded
 * bytes, RDP rectangles without shade, Mtx halfword order on a
 * little-endian host, far-plane clipping, shadow volumes, the SUBSCREEN
 * overlay stretch, the title fit, small scissors and viewports honoured,
 * TMEM deswizzle and the LOADBLOCK dxt rule. Dropped because the NV2A has
 * what the PVR lacked: the per-vertex combiner fit (register combiners do
 * it per pixel), painter-depth layers (the NV2A draws in submission
 * order), the baked sea product (two texture units), modifier volumes
 * (stencil).
 *
 * Conventions: N64 matrices are row-vector (v' = v M), stored here
 * row-major as float m[4][4]; "MUL" pre-multiplies (M = param x M). Output:
 * N64 320x240 viewport pixels mapped onto the framebuffer's content rect;
 * depth = (z/w + 1) / 2 of the back end's depth range, tested LEQUAL. */
#include <math.h>
#include <stdint.h>
#include <string.h>
#if defined(__SSE__)
#include <xmmintrin.h>
#endif

#include "xrc_widescreen.h"
#include "ultra/ultra.h"

#include "f3d_gbi.h"
#include "fbfx.h"
#include "f3d_interp.h"
#include "f3dex_gbi.h"
#include "texconv.h"
#include "xgfx.h"
#include "xhw.h"

/* Every RDRAM address here is physical (seg(), the colour images): the
 * runtime's helpers take it as KSEG0, so it is never read through a game's
 * TLB (ultra.h xu_phys: a KUSEG address below 0x80000000 is virtual, and
 * Turok 2 maps its code over physical 0x200000 +). */
#define XU_K0(a) (((uint32_t)(a)) | 0x80000000u)
#define rd8(a) rd8(XU_K0(a))
/* halfwords at even addresses: an odd one (a garbage list's matrix or
 * TLUT) read a byte past the window at its last byte, and in the
 * word-swapped image an odd halfword is not two adjacent bytes anyway
 * (docs/review-2026-10.md 5c.4) */
#define rd16(a) rd16(XU_K0(a) & ~1u)
#define rd32(a) rd32(XU_K0(a))
#define xu_host(a) xu_host(XU_K0(a))
#define xu_from_rdram(d, a, n) xu_from_rdram((d), XU_K0(a), (n))

/* ---- switches (docs/testing.md) ---- */
/* vertices and matrices read as words of the image (their halves and bytes in
 * N64 order within them); -DXGFX_VTX_WORDS=0: halfword by halfword, as before */
#ifndef XGFX_VTX_WORDS
#define XGFX_VTX_WORDS 1
#endif
#ifndef XGFX_FAR_CLIP
#define XGFX_FAR_CLIP 1        /* Fast3D clips at the far plane (z <= w): the sea stops short of the horizon */
#endif
#ifndef XGFX_OVERLAY_FULL
#define XGFX_OVERLAY_FULL 1    /* SUBSCREEN-sized no-Z untextured draws stretched to the screen edges */
#endif
#ifndef XGFX_TITLE_FIT
#define XGFX_TITLE_FIT 1       /* the title's SUBSCREEN picture fitted to the screen with a black margin */
#endif
#ifndef XGFX_SCISSOR
#define XGFX_SCISSOR 1         /* small scissors and viewports (photo album thumbnails) honoured */
#endif
#ifndef XGFX_Z_REL_STEP
#define XGFX_Z_REL_STEP (1.0f / 32768.0f)   /* per draw-order step; 2^-16 (wings64-dc's) still let the target flicker a little on the console (run 5) */
#endif
#ifndef XGFX_Z_REL
#define XGFX_Z_REL 1           /* the tie-break and decal bias as a fraction of distance; 0: polygon offset units */
#endif
#ifndef XGFX_Z_TIEBREAK
#define XGFX_Z_TIEBREAK 1      /* later coplanar draw states win depth ties, as on the RDP */
#endif
#ifndef XGFX_SHADOW_VOL
#define XGFX_SHADOW_VOL 1      /* shadow volumes through the stencil buffer; 0: both passes dropped */
#endif
#ifndef XGFX_TEX_MEMO
#define XGFX_TEX_MEMO 1        /* per-frame (load address, tile key) -> texture memo */
#endif
#ifndef XGFX_LOADTILE32
#define XGFX_LOADTILE32 1      /* LOADTILE into a 32-bit tile: a row is twice its line (both banks) */
#endif
#ifndef XGFX_LOAD32
#define XGFX_LOAD32 1          /* LOADBLOCK into a 32-bit tile counts 32-bit texels */
#endif
#ifndef XGFX_FILL32
#define XGFX_FILL32 1          /* a fill into a 32-bit colour image takes its colour as RGBA8888, as the RDP */
#endif
#ifndef XGFX_TRI4
#define XGFX_TRI4 1            /* Fast3D's 0xB1 as Rare's G_TRI4 */
#endif
#ifndef XGFX_HIRES
#define XGFX_HIRES 1           /* the screen's size from the swapped buffer's width and the VI mode */
#endif
#ifndef XGFX_TEXRECT_SNAP
#define XGFX_TEXRECT_SNAP 1    /* 1/2-cycle texrects cover every pixel row and column they touch */
#endif
#ifndef XGFX_S2DEX_OBJ
#define XGFX_S2DEX_OBJ 1       /* S2DEX's objects drawn: texture loads, rectangles, sprites (Yoshi's Story) */
#endif
#ifndef XGFX_S2DEX_BG
#define XGFX_S2DEX_BG 1        /* S2DEX's G_BG_COPY and G_BG_1CYC drawn (Ocarina of Time's pre-rendered backgrounds) */
#endif
#ifndef XGFX_TLUT_OFFSET
#define XGFX_TLUT_OFFSET 1     /* LOADTLUT's SL/TL offset the palette in the image (GoldenEye's palettes after their texels) */
#endif
#ifndef XGFX_RDP_TRI
#define XGFX_RDP_TRI 1         /* raw RDP triangles sent as G_RDPHALF_1/G_RDPHALF_CONT pairs (GoldenEye's sky) */
#endif
#ifndef XGFX_MASK0_CLAMP
#define XGFX_MASK0_CLAMP 1     /* an axis with mask 0 clamps, as the RDP's */
#endif
#ifndef XGFX_CLAMP_EXTENT
#define XGFX_CLAMP_EXTENT 1    /* a clamp bit clamps at the tile's extent: wider than the mask, the texture repeats */
#endif
#ifndef XGFX_TILE_LINEW
#define XGFX_TILE_LINEW 1      /* a tile wider than its line: the line's texels (the extra column for the filter) */
#endif
#ifndef XGFX_PROBE
#define XGFX_PROBE 1           /* small copies of the frame being drawn, from a software sample grid (Pokemon Snap's photo check) */
#endif
#ifndef XGFX_RGBA_LOWBIT
#define XGFX_RGBA_LOWBIT 1     /* RGBA at 4 or 8 bits: through the palette (TLUT on) or as I4/I8 */
#endif
#ifndef XGFX_TILE_ROWHASH
#define XGFX_TILE_ROWHASH 1    /* a LOADTILE's change check hashes every row, not the first and last */
#endif
#ifndef XGFX_TILE_WRAP
#define XGFX_TILE_WRAP 1       /* tile sizes wrap in 12 bits (a scrolling tile with lrs < uls) */
#endif
#ifndef XGFX_DESWIZZLE32
#define XGFX_DESWIZZLE32 1     /* LOADBLOCK dxt 0 of RGBA32: odd rows swap texel pairs (TMEM's two banks) */
#endif
#ifndef XGFX_DXT32
#define XGFX_DXT32 1           /* the same rule for RGBA32 (texel pairs) */
#endif
#ifndef XGFX_DXT_EXACT
#define XGFX_DXT_EXACT 1       /* LOADBLOCK dxt not sized for the line: the RDP's odd-line rule */
#endif

/* the game's screen in N64 pixels: 320x240, or the size of the buffer it
 * swaps to when that is another (screen_size: GoldenEye's 440x330 front
 * end, 640x480 games) */
static float s_n64w = 320.0f, s_n64h = 240.0f;
#define N64_W s_n64w
#define N64_H s_n64h
#define MTX_DEPTH 11           /* 10 pushes + current (Birdman64 gfx.md) */
#define DL_DEPTH 18
#define VC_MAX 80              /* the largest vertex buffer of the families (F3DLP); Fast3D uses 16 */
#define GUARD 2.0f             /* x/y clipped at +-GUARD*w: screen coordinates stay well inside the NV2A's range */
/* DIVERGES: triangles are trivially rejected beyond the drawn extent, not
 * at the viewport edge: the port draws past the SUBSCREEN viewport (10..310 x
 * 8..222) to the screen edges, where the RDP's scissor cut the picture, so a
 * terrain triangle wholly in that band (18 px at the bottom) was dropped and
 * terrain popped at the edges on fast turns. 1.2 > 160/150 and 125/107;
 * -DXGFX_REJECT_RATIO=1.0f is the old reject. */
#ifndef XGFX_REJECT_RATIO
#define XGFX_REJECT_RATIO 1.2f
#endif
/* x reject scale: 1, or with hor+ the drawn width over the 4:3 box's (4/3
 * at 16:9), so the reject follows the aspect (map_setup) */
static float s_rej_kx = 1.0f;
static uint32_t s_epoch = 1;   /* draw state epoch: DIRTY() makes the next draw rebuild its state */
#define DIRTY() (s_epoch++)
static void frame_cimg(uint32_t c);   /* a frame across tasks (f3d_swap) */
static void screen_size(uint32_t c, uint32_t w);

/* ------------------------------------------------------------------ state */

typedef struct {
    float x, y, z, w;          /* clip space */
    float s, t;                /* texel coordinates (s10.5 scaled by gSPTexture, /32) */
    float r, g, b, a;          /* shade 0..1 */
    float fog;                 /* 0..255 */
    uint32_t oc;               /* outcode: 1 x+, 2 x-, 4 y+, 8 y-, 16 near, 32 needs clipping, 64 far */
    /* to_out's work that does not change between the triangles sharing the
     * vertex (XGFX_VTX_OUT_CACHE): valid while ostamp == s_ostamp and the
     * fog bit is ofog */
    uint32_t ostamp, ofog;
    float ox, oy, oz;          /* framebuffer x before the HUD anchor, y, depth 0..1 before the bias */
    uint8_t orgba[4];
} Vtx;
#define VTX_LERP_FLOATS 11
/* the vertices' screen positions and colours are worked out once per vertex,
 * not once per triangle that uses it; s_ostamp moves on whenever what they
 * depend on changes (the viewport, the output map). -DXGFX_VTX_OUT_CACHE=0:
 * per triangle, as before (the same numbers) */
#ifndef XGFX_VTX_OUT_CACHE
#define XGFX_VTX_OUT_CACHE 1
#endif
static uint32_t s_ostamp = 1;

typedef struct {
    uint8_t fmt, siz, cms, cmt, masks, maskt, shifts, shiftt, palette;
    uint16_t line, tmem;
    uint16_t uls, ult, lrs, lrt;   /* 10.2 */
} Tile;

/* What TMEM holds at a tile's address: a LOADBLOCK (a run of bytes, odd
 * rows swapped unless dxt says otherwise) or a LOADTILE (a rectangle of the
 * texture image: `rows` rows of `row_bytes` at `stride`, unswapped as
 * sampled). */
typedef struct {
    uint32_t addr, bytes;
    uint16_t dxt;
    uint8_t valid, tile;
    uint16_t rows, row_bytes, stride;
    uint16_t tmem;   /* where the load started, in 64-bit words */
    uint32_t seq;    /* load order: the latest load over an address is what TMEM holds */
} TmemLoad;
static uint32_t s_load_seq;

static struct {
    uint32_t seg[16];
    float proj[4][4];
    float mv[MTX_DEPTH][4][4];
    float mvp[4][4];
    int mv_top, mvp_dirty;
    float vp_sx, vp_sy, vp_tx, vp_ty;   /* N64 pixels */
    float vp_sz, vp_tz;                 /* screen z units (G_MAXZ) */
    uint32_t geom, omh, oml;            /* geom in Fast3D's bits whatever the microcode */
    uint32_t geom2;                     /* F3DEX2: the RSP's own geometry word */
    uint32_t cc0, cc1;
    uint8_t prim[4], env[4], fogc[4], blend[4];
    uint8_t prim_lod_frac;
    uint32_t fill;
    int16_t fog_m, fog_o;
    int tex_on, tex_tile;
    float tex_ss, tex_st;
    Tile tile[8];
    TmemLoad loads[64];                 /* indexed by tmem >> 3 */
    uint16_t tlut[256];                 /* TMEM's upper half as G_LOADTLUT left it: one entry a 64-bit word */
    uint8_t timg_fmt, timg_siz;
    uint16_t timg_w;
    uint32_t timg_addr;
    uint32_t cimg, zimg;
    int num_lights;
    uint8_t light_col[9][3];
    int8_t light_dir[9][3];
    Vtx vc[VC_MAX + 4];   /* (+4: a line's quad, do_line) */
    uint32_t half1, half2;
} R;

/* ------------------------------------------------------------- addressing */

/* A display list's address -> the N64 address it means: a segmented one
 * (segment 1..15 with a base) through the segment table, as the RSP did;
 * KSEG0, KSEG1 and physical addresses as they are (the accessors mask them
 * into the window). */
static inline uint32_t seg(uint32_t a) {
    uint32_t s = (a >> 24) & 0x0F;
    if (a < 0x10000000u && s && R.seg[s]) a = R.seg[s] + (a & 0x00FFFFFFu);
    return a & 0x1FFFFFFFu;
}
/* its host address, for word-wise reads (display lists) and hashing */
static inline const uint8_t* host(uint32_t a) { return xu_host(seg(a)); }
/* n bytes from a, cut at the window's end: a texture near the top of RDRAM
 * was hashed past it (docs/review-2026-10.md 5c.2) */
static inline uint32_t span(uint32_t a, uint32_t n) {
    uint32_t off = (uint32_t)(host(a) - xrc_rdram_host);
    return off + (uint64_t)n > xrc_rdram_size ? xrc_rdram_size - off : n;
}

/* --------------------------------------------------------------- matrices */

/* N64 Mtx = { s16 i[4][4]; u16 f[4][4] }: halfword e of each half, read
 * from the image at N64 addresses */
static void mtx_from_n64(float out[4][4], uint32_t m) {
    int e;
    if (XGFX_VTX_WORDS && !(m & 3u) && (m & 0x1FFFFFFFu) + 64u <= xrc_rdram_size) {
        /* the image's words: each holds two halves in N64 order (the integer
         * parts first, then the fractions) */
        const uint32_t* q = (const uint32_t*)xu_host(m);
        for (e = 0; e < 16; e += 2) {
            uint32_t wi = q[e >> 1], wf = q[8 + (e >> 1)];
            out[e >> 2][e & 3] = (float)(int32_t)((wi & 0xFFFF0000u) | (wf >> 16)) * (1.0f / 65536.0f);
            out[e >> 2][(e & 3) + 1] = (float)(int32_t)((wi << 16) | (wf & 0xFFFFu)) * (1.0f / 65536.0f);
        }
        return;
    }
    for (e = 0; e < 16; e++)
        out[e >> 2][e & 3] = (float)(((int32_t)(int16_t)rd16(m + 2u * e) << 16) | (int32_t)rd16(m + 32u + 2u * e)) *
                             (1.0f / 65536.0f);
}

static void mtx_mul(float out[4][4], const float a[4][4], const float b[4][4]) {   /* out = a x b */
    float t[4][4];
    int i, j;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) t[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
    memcpy(out, t, sizeof t);
}

static void do_mtx(uint32_t w0, uint32_t w1) {
    float m[4][4];
    uint8_t p = (w0 >> 16) & 0xFF;
    mtx_from_n64(m, seg(w1));
    if (p & F3D_G_MTX_PROJECTION) {
        int persp = R.proj[2][3] != 0.0f;
        if (p & F3D_G_MTX_LOAD) memcpy(R.proj, m, sizeof m);
        else mtx_mul(R.proj, m, R.proj);
        if ((R.proj[2][3] != 0.0f) != persp) DIRTY();   /* hor+ extends perspective draws only (pixel_state) */
        if (XHW_TEST_BUILD && (p & F3D_G_MTX_LOAD) && m[2][3] != 0.0f) {
            /* a perspective loaded: its near and far planes (z/w = -1, 1) and
             * fovy, when they change (the game's clip distances, as drawn) */
            static float s_ln[32], s_lf[32];
            static unsigned s_nlog;
            float a = m[2][2] + m[2][3], b = m[2][2] - m[2][3];
            float n = a != 0.0f ? m[3][2] / a : 0.0f, f = b != 0.0f ? m[3][2] / b : 0.0f;
            unsigned k;
            if (n > f) { float t = n; n = f; f = t; }
            for (k = 0; k < s_nlog; k++)   /* each pair once */
                if (fabsf(n - s_ln[k]) <= 0.01f * fabsf(n) + 0.01f && fabsf(f - s_lf[k]) <= 0.01f * fabsf(f) + 0.01f) break;
            if (k == s_nlog && s_nlog < 32) {
                s_ln[s_nlog] = n;
                s_lf[s_nlog++] = f;
                xhw_logf("[GFX] perspective: near %d.%02d far %d fovy %d aspect %d/1000 (retrace %u)", (int)n,
                         (int)(fabsf(n - (float)(int)n) * 100.0f), (int)f,
                         m[1][1] != 0.0f ? (int)(2.0f * atanf(fabsf(m[2][3] / m[1][1])) * 57.29578f + 0.5f) : 0,
                         m[0][0] != 0.0f ? (int)(1000.0f * m[1][1] / m[0][0]) : 0, xu_retrace());
            }
        }
    } else {
        if ((p & F3D_G_MTX_PUSH) && R.mv_top < MTX_DEPTH - 1) {
            memcpy(R.mv[R.mv_top + 1], R.mv[R.mv_top], sizeof m);
            R.mv_top++;
        }
        if (p & F3D_G_MTX_LOAD) memcpy(R.mv[R.mv_top], m, sizeof m);
        else mtx_mul(R.mv[R.mv_top], m, R.mv[R.mv_top]);
    }
    R.mvp_dirty = 1;
}

/* --------------------------------------------------------------- vertices */

/* Lighting and spherical texgen (rare in PW64: G_LIGHTING only with
 * G_TEXTURE_GEN + DECALRGB; Birdman64 gfx.md). Eye-space normal. */
static void vtx_lit(Vtx* v, const uint8_t* cn) {
    const float(*m)[4] = R.mv[R.mv_top];
    float nx = (int8_t)cn[0], ny = (int8_t)cn[1], nz = (int8_t)cn[2];
    float ex = nx * m[0][0] + ny * m[1][0] + nz * m[2][0];
    float ey = nx * m[0][1] + ny * m[1][1] + nz * m[2][1];
    float ez = nx * m[0][2] + ny * m[1][2] + nz * m[2][2];
    float len = sqrtf(ex * ex + ey * ey + ez * ez);
    if (len > 1e-6f) { ex /= len; ey /= len; ez /= len; }
    if (R.geom & F3D_G_LIGHTING) {
        int amb = R.num_lights < 8 ? R.num_lights : 8, l;
        float r = R.light_col[amb][0], g = R.light_col[amb][1], b = R.light_col[amb][2];
        for (l = 0; l < R.num_lights && l < 8; l++) {
            float lx = R.light_dir[l][0], ly = R.light_dir[l][1], lz = R.light_dir[l][2];
            float ll = sqrtf(lx * lx + ly * ly + lz * lz), d;
            if (ll < 1e-6f) continue;
            d = (ex * lx + ey * ly + ez * lz) / ll;
            if (d > 0.0f) { r += d * R.light_col[l][0]; g += d * R.light_col[l][1]; b += d * R.light_col[l][2]; }
        }
        v->r = r > 255.0f ? 1.0f : r * (1.0f / 255.0f);
        v->g = g > 255.0f ? 1.0f : g * (1.0f / 255.0f);
        v->b = b > 255.0f ? 1.0f : b * (1.0f / 255.0f);
    } else {
        v->r = cn[0] * (1.0f / 255.0f);
        v->g = cn[1] * (1.0f / 255.0f);
        v->b = cn[2] * (1.0f / 255.0f);
    }
    if (R.geom & F3D_G_TEXTURE_GEN) {   /* spherical env map: (n_eye + 1) / 4 of the texture scale */
        v->s = (ex + 1.0f) * 0.25f * R.tex_ss * (1.0f / 32.0f);
        v->t = (ey + 1.0f) * 0.25f * R.tex_st * (1.0f / 32.0f);
    }
}

static uint32_t outcode(const Vtx* v) {
    uint32_t o = 0;
    float gw = GUARD * v->w, rw = XGFX_REJECT_RATIO * v->w;
    if (v->x > rw * s_rej_kx) o |= 1;   /* hor+: the drawn extent is wider (map_setup) */
    if (v->x < -rw * s_rej_kx) o |= 2;
    if (v->y > rw) o |= 4;
    if (v->y < -rw) o |= 8;
    if (v->z < -v->w) o |= 16;
    if (v->z + v->w < 0.0f || v->x > gw || v->x < -gw || v->y > gw || v->y < -gw) o |= 32;
    if (XGFX_FAR_CLIP && v->z > v->w) o |= 64 | 32;
    return o;
}

/* the vertex buffer of the microcode running (f3d_set_ucode) */
static const GfxUcode s_uc_f3d = { GFX_UC_F3D, 0, 16, 0, "" };
static const GfxUcode* s_uc = &s_uc_f3d;
static unsigned s_vmax = 16;

/* the vertex's clip position as four lanes at once (SSE: the P3 has it):
 * each lane's sum in the scalar code's order, ((x m0 + y m1) + z m2) + m3,
 * with no fused multiply-add, so the same bits (bench_dl's pictures match).
 * GoldenEye's vertices were a third of its display-list time on the host.
 * -DXGFX_VTX_SSE=0: the scalar sums, as before */
#ifndef XGFX_VTX_SSE
#define XGFX_VTX_SSE 1
#endif
#if !defined(__SSE__)
#undef XGFX_VTX_SSE
#define XGFX_VTX_SSE 0
#endif

static void do_vtx(uint32_t n, uint32_t v0, uint32_t addr) {
    uint32_t i, src;
#if XGFX_VTX_SSE
    __m128 m0, m1, m2, m3;
#endif
    src = seg(addr);
    if (R.mvp_dirty) {
        mtx_mul(R.mvp, R.mv[R.mv_top], R.proj);   /* row vectors: v MV P */
        R.mvp_dirty = 0;
    }
#if XGFX_VTX_SSE
    m0 = _mm_loadu_ps(R.mvp[0]), m1 = _mm_loadu_ps(R.mvp[1]), m2 = _mm_loadu_ps(R.mvp[2]), m3 = _mm_loadu_ps(R.mvp[3]);
#endif
    for (i = 0; i < n && v0 + i < s_vmax; i++, src += 16) {
        /* N64 Vtx: s16 ob[3], u16 flag, s16 tc[2], u8 cn[4] */
        int16_t ob[3], tc[2];
        uint8_t cn[4];
        Vtx* v = &R.vc[v0 + i];
        float x, y, z;
        v->ostamp = 0;
        if (XGFX_VTX_WORDS && !(src & 3u)) {   /* the image's words are native: four reads */
            const uint32_t* q = (const uint32_t*)xu_host(src);
            uint32_t w0, w1, w2, w3;
            /* the window's end tested before the four reads, not after them (docs/review-2026-10.md 5c.1) */
            if ((src & (xrc_rdram_size - 1)) + 16u > xrc_rdram_size) w0 = q[0], w1 = rd32(src + 4), w2 = rd32(src + 8), w3 = rd32(src + 12);
            else w0 = q[0], w1 = q[1], w2 = q[2], w3 = q[3];
            ob[0] = (int16_t)(w0 >> 16); ob[1] = (int16_t)w0; ob[2] = (int16_t)(w1 >> 16);
            tc[0] = (int16_t)(w2 >> 16); tc[1] = (int16_t)w2;
            cn[0] = (uint8_t)(w3 >> 24); cn[1] = (uint8_t)(w3 >> 16); cn[2] = (uint8_t)(w3 >> 8); cn[3] = (uint8_t)w3;
        } else {
            ob[0] = (int16_t)rd16(src); ob[1] = (int16_t)rd16(src + 2); ob[2] = (int16_t)rd16(src + 4);
            tc[0] = (int16_t)rd16(src + 8); tc[1] = (int16_t)rd16(src + 10);
            cn[0] = rd8(src + 12); cn[1] = rd8(src + 13); cn[2] = rd8(src + 14); cn[3] = rd8(src + 15);
        }
        x = ob[0]; y = ob[1]; z = ob[2];
#if XGFX_VTX_SSE
        _mm_storeu_ps(&v->x, _mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(_mm_set1_ps(x), m0), _mm_mul_ps(_mm_set1_ps(y), m1)),
                                                   _mm_mul_ps(_mm_set1_ps(z), m2)),
                                        m3));
#else
        const float(*m)[4] = R.mvp;
        v->x = x * m[0][0] + y * m[1][0] + z * m[2][0] + m[3][0];
        v->y = x * m[0][1] + y * m[1][1] + z * m[2][1] + m[3][1];
        v->z = x * m[0][2] + y * m[1][2] + z * m[2][2] + m[3][2];
        v->w = x * m[0][3] + y * m[1][3] + z * m[2][3] + m[3][3];
#endif
        v->oc = outcode(v);
        if (R.geom & (F3D_G_LIGHTING | F3D_G_TEXTURE_GEN)) {
            vtx_lit(v, cn);
        } else {
            v->r = cn[0] * (1.0f / 255.0f);
            v->g = cn[1] * (1.0f / 255.0f);
            v->b = cn[2] * (1.0f / 255.0f);
        }
        v->a = cn[3] * (1.0f / 255.0f);
        if (!(R.geom & F3D_G_TEXTURE_GEN)) {
            v->s = tc[0] * R.tex_ss * (1.0f / 65536.0f) * (1.0f / 32.0f);
            v->t = tc[1] * R.tex_st * (1.0f / 65536.0f) * (1.0f / 32.0f);
        }
        if ((R.geom & F3D_G_FOG) && v->w > 0.0f) {
            float f = v->z / v->w * R.fog_m + R.fog_o;
            v->fog = f < 0.0f ? 0.0f : f > 255.0f ? 255.0f : f;
        } else {
            v->fog = 0.0f;
        }
    }
}

/* ------------------------------------------------------------- output map */

/* N64 viewport pixels -> framebuffer pixels: x * s_sx + s_ox. The content
 * rect is the 4:3 box the N64's 320x240 maps onto, undistorted: the whole
 * framebuffer at 4:3, a centred box in a 16:9 picture (960x720 of 1280x720,
 * 1440x1080 of 1920x1080, 480x480 of an anamorphic 640x480).
 *
 * Widescreen (PLAN.md 3.2, wings64_widescreen.h): with hor+ on, the world
 * view's perspective draws keep this mapping and are scissored to the full
 * width instead of the box (s_wx0..s_wx1), so the 3D shows more left and
 * right at the same vertical field of view; untextured fills that span the
 * view (clears, fades, the letterbox bars) are stretched across it; the
 * rest of the 2D stays in the box, the flight HUD's gauges moved to the
 * edges by anchor tags. Off at 720p/1080i: the box with black bars. */
static float s_sx = 2.0f, s_sy = 2.0f, s_ox, s_oy;
static XgfxOutput s_out;
static float s_cx, s_cy, s_cw = 640.0f, s_ch = 480.0f;   /* content rect */
static float s_wx0, s_wx1 = 640.0f;   /* the drawn width: the box, or with hor+ the framebuffer */
static int s_title;   /* the title screen this frame (title fit) */
static volatile int s_ws_req;   /* f3d_set_widescreen */
static int s_hp;                /* hor+ this frame */
static int s_world;             /* inside a world view's tags (patches/050) */
static int s_tagged;            /* a world tag was seen: the pack marks its world views itself */
static int s_anchor = 'C';      /* the HUD anchor of the 2D draws that follow (patches/052) */
static int s_extend;            /* this draw state: a world view's perspective draw, scissored to s_wx0..s_wx1 */
static int s_side_drawn;        /* this frame drew into the pillarbox strips on purpose (side_clear) */
static float s_xoff;            /* this draw state: the anchor's shift in framebuffer pixels */
#define TITLE_MARGIN 12.0f   /* output pixels at 480; scaled with the content rect */

void f3d_set_widescreen(int on) { s_ws_req = on; }

static void map_setup(void) {
    float aspect, shown;
    int wide = s_ws_req;
    xgfx_output(&s_out);
    if (s_out.xscale < 1) s_out.xscale = 1;
    aspect = (float)s_out.fb_w / (float)s_out.xscale / (float)s_out.fb_h;   /* the framebuffer's, not the pixels' */
    /* the aspect the TV shows: 16:9 at 720p/1080i; at 480 16:9 with Widescreen
     * on (anamorphic: a 16:9 TV set to "full" undoes the squeeze), else 4:3 */
    shown = aspect > 1.4f ? aspect : wide ? 16.0f / 9.0f : 4.0f / 3.0f;
    s_ch = (float)s_out.fb_h;
    s_cw = (float)s_out.fb_w * (4.0f / 3.0f) / shown;
    if (wide && !XGFX_HORPLUS) s_cw = (float)s_out.fb_w;   /* kill switch: 16:9 as a plain stretch */
    /* whole pixels: 1280 x (4/3) / (16/9) came out as 960.0 with s_cx
     * 159.999, the (int) casts put the box at 159..1119 and the title's
     * right margin clear stopped one column short: the scene's last column
     * showed as a line at the box's right edge at 720p (console rc1, rc1b) */
    s_cw = (float)(int)(s_cw + 0.5f);
    s_cx = (float)(int)(((float)s_out.fb_w - s_cw) * 0.5f + 0.5f);
    s_cy = 0.0f;
    /* DIVERGES: hor+ (the title stays in its fitted box) */
    s_hp = wide && XGFX_HORPLUS && !s_title && s_cw < (float)s_out.fb_w - 1.0f;
    s_wx0 = s_hp ? 0.0f : s_cx;
    s_wx1 = s_hp ? (float)s_out.fb_w : s_cx + s_cw;
    s_rej_kx = s_hp ? (s_wx1 - s_wx0) / s_cw : 1.0f;
    if (XGFX_TITLE_FIT && s_title) {
        /* the title composes its picture inside SUBSCREEN (10..310 x 8..222);
         * DIVERGES: fitted to the content rect with a black margin, as the DC port */
        float m = TITLE_MARGIN * s_ch / 480.0f, mx = m * s_cw / (s_ch * 4.0f / 3.0f);   /* xscale, anamorphic 480 */
        s_sx = (s_cw - 2.0f * mx) / 300.0f;
        s_ox = s_cx + mx - 10.0f * s_sx;
        s_sy = (s_ch - 2.0f * m) / 214.0f;
        s_oy = s_cy + m - 8.0f * s_sy;
    } else {
        s_sx = s_cw / N64_W;
        s_sy = s_ch / N64_H;
        s_ox = s_cx;
        s_oy = s_cy;
    }
    s_ostamp++;
}

/* SUBSCREEN edges in N64 pixels, 2 pixels of slack (the HUD fade box lands
 * at 18.0x, the fade-out quad at 15.4; wings64-dc traps.md) */
#define SUB_X0 (12.0f * N64_W / 320.0f)
#define SUB_X1 (308.0f * N64_W / 320.0f)
#define SUB_Y0 (10.0f * N64_H / 240.0f)
#define SUB_Y1 (220.0f * N64_H / 240.0f)

/* ------------------------------------------------------------- draw state */

static XgfxState s_st;           /* the state the batch below is drawn with */
static float s_zrel;             /* XGFX_Z_REL: this state's depth bias, a fraction of the distance */
static uint32_t s_st_epoch;
static int s_textured, s_tex1, s_decal, s_overlay, s_shadow_pass2;
static float s_u_scale[2], s_u_off[2], s_v_scale[2], s_v_off[2];
static unsigned s_zorder;
static float s_scissor[4] = { 0.0f, 0.0f, 320.0f, 240.0f };   /* N64_W x N64_H at each frame's start */
static int s_sc_on;
static float s_sc[4];            /* the clip rectangle when s_sc_on, N64 pixels */

#define BATCH_TRIS 2048
static XgfxVtx s_batch[BATCH_TRIS * 3];
static uint32_t s_nb;

static int s_trace;   /* -DXGFX_TRACE_FRAME=<n>: frame n's draws as [DRAW] lines */
#ifndef XGFX_TRACE_MS     /* -DXGFX_TRACE_MS=<ms>: the first frame that long after the first, instead */
#define XGFX_TRACE_MS 0
#endif
#ifndef XGFX_TRACE_FRAME
#define XGFX_TRACE_FRAME 0
#endif
#ifndef XGFX_TRACE_PRIM   /* -DXGFX_TRACE_PRIM=0xRRGGBB: the frame after the first draw with that prim colour */
#define XGFX_TRACE_PRIM 0
#endif
#ifndef XGFX_TRACE_MISS   /* -DXGFX_TRACE_MISS=1: the frame after the first texture miss */
#define XGFX_TRACE_MISS 0
#endif
static int s_trace_next;

/* a draw that lands in the pillarbox strips without being a hor+ world
 * draw (side_clear): the first 16 are logged, in every build, to name the
 * one behind the line at 720p (console, rc1) */
static void log_side_leak(void) {
    static unsigned s_n;
    float x0 = 1e9f, x1 = -1e9f, bl = s_cx, br = s_cx + s_cw;
    uint32_t i;
    if (s_extend || (s_overlay && s_hp) /* a fill stretched to 16:9 on purpose */ || s_n >= 16 ||
        s_cw >= (float)s_out.fb_w - 1.0f)
        return;
    if (s_st.scissor[0] >= (int32_t)bl && s_st.scissor[2] <= (int32_t)br) return;   /* clipped to the box */
    for (i = 0; i < s_nb * 3; i++) {
        if (s_batch[i].x < x0) x0 = s_batch[i].x;
        if (s_batch[i].x > x1) x1 = s_batch[i].x;
    }
    if (x0 >= bl - 0.5f && x1 <= br + 0.5f) return;
    s_n++;
    xhw_logf("[GFX] draw past the 4:3 box (%u): state %d, title %d, hor+ %d, overlay %d, x %d..%d, box %d..%d, "
             "scissor %d..%d, %u tris, tex %u, cc %08x:%08x, depth %u",
             s_n, wx_game_state(), s_title, s_hp, s_overlay, (int)x0, (int)x1, (int)bl, (int)br,
             (int)s_st.scissor[0], (int)s_st.scissor[2], (unsigned)s_nb, (unsigned)s_st.tex[0],
             (unsigned)s_st.cc0, (unsigned)s_st.cc1, (unsigned)s_st.depth_func);
}

static float s_m0[2] = { -1.0f, -1.0f };   /* bind_unit: unit 0's mask-0 axes (its loaded extent, 0..1), or -1 */
static uint32_t s_m0_tile, s_m0_addr;
/* f3d_bench: the list run again with the back end left out (nothing drawn;
 * textures still looked up, made only if new) or in, untimed by [PERF] */
static int s_null_back, s_benching;
static int s_in_dl;
static uint64_t s_present_in_dl;
static void flush(void) {
    if (!s_nb) return;
    if (s_null_back) {
        s_nb = 0;
        return;
    }
    if (XHW_TEST_BUILD && s_st.tex[0] && (s_m0[0] >= 0.0f || s_m0[1] >= 0.0f)) {
        static unsigned s_said;
        uint32_t i;
        float lo[2] = { 2e9f, 2e9f }, hi[2] = { -2e9f, -2e9f };
        for (i = 0; i < s_nb * 3; i++) {
            if (s_batch[i].s0 < lo[0]) lo[0] = s_batch[i].s0;
            if (s_batch[i].s0 > hi[0]) hi[0] = s_batch[i].s0;
            if (s_batch[i].t0 < lo[1]) lo[1] = s_batch[i].t0;
            if (s_batch[i].t0 > hi[1]) hi[1] = s_batch[i].t0;
        }
        for (i = 0; i < 2 && s_said < 40; i++) {
            /* in texels: past the loaded extent by more than one (filtering's edge) */
            float n = (float)(i ? (s_m0_tile & 0xFF) : (s_m0_tile >> 8 & 0xFF)), k = s_m0[i] > 0.0f ? n / s_m0[i] : 0.0f;
            static uint32_t s_seen[40];
            unsigned j;
            if (s_m0[i] < 0.0f || n < 2.0f || (lo[i] * k > -1.5f && hi[i] * k < n + 1.5f)) continue;
            for (j = 0; j < s_said && s_seen[j] != (s_m0_addr ^ i); j++) {}
            if (j < s_said) continue;
            s_seen[s_said] = s_m0_addr ^ i;
            {
                s_said++;
                xhw_logf("[GFX] mask-0 clamp: axis %c of %08x (fmt %u siz %u %ux%u) runs %d..%d%% of its %d%%, %u tris at %d,%d",
                         i ? 't' : 's', (unsigned)s_m0_addr, (unsigned)(s_m0_tile >> 24), (unsigned)(s_m0_tile >> 16 & 0xFF),
                         (unsigned)(s_m0_tile >> 8 & 0xFF), (unsigned)(s_m0_tile & 0xFF), (int)(lo[i] * 100.0f),
                         (int)(hi[i] * 100.0f), (int)(s_m0[i] * 100.0f), (unsigned)s_nb, (int)s_batch[0].x, (int)s_batch[0].y);
                break;
            }
        }
    }
    log_side_leak();
    if (s_trace) {
        uint32_t i;
        float zlo = 2e9f, zhi = -2e9f;
        for (i = 0; i < s_nb * 3; i++) {
            if (s_batch[i].z < zlo) zlo = s_batch[i].z;
            if (s_batch[i].z > zhi) zhi = s_batch[i].z;
        }
        float slo = 2e9f, shi = -2e9f, tlo = 2e9f, thi = -2e9f;
        for (i = 0; i < s_nb * 3; i++) {
            if (s_batch[i].s0 < slo) slo = s_batch[i].s0;
            if (s_batch[i].s0 > shi) shi = s_batch[i].s0;
            if (s_batch[i].t0 < tlo) tlo = s_batch[i].t0;
            if (s_batch[i].t0 > thi) thi = s_batch[i].t0;
        }
        xhw_logf("[DRAW] z %d..%d (x1000) st0 %d..%d, %d..%d (x100) wrap %u/%u", (int)(zlo * 1000.0f / s_out.zmax),
                 (int)(zhi * 1000.0f / s_out.zmax), (int)(slo * 100.0f), (int)(shi * 100.0f), (int)(tlo * 100.0f),
                 (int)(thi * 100.0f), (unsigned)s_st.wrap_s[0], (unsigned)s_st.wrap_t[0]);
    }
    if (XGFX_TRACE_PRIM && !s_trace_next && s_st.prim[0] == ((XGFX_TRACE_PRIM >> 16) & 0xFF) &&
        s_st.prim[1] == ((XGFX_TRACE_PRIM >> 8) & 0xFF) && s_st.prim[2] == (XGFX_TRACE_PRIM & 0xFF))
        s_trace_next = 1;
    if (s_trace)
        xhw_logf("[DRAW] %u tris cc %06x:%08x cyc %u tex %u/%u blend %u aref %u depth %u/%u bias %u sten %u fog %u "
                 "prim %02x%02x%02x%02x env %02x%02x%02x%02x geom %06x oml %08x v0 %d,%d z %d a %u",
                 (unsigned)s_nb, (unsigned)s_st.cc0, (unsigned)s_st.cc1, (unsigned)s_st.two_cycle + 1,
                 (unsigned)s_st.tex[0], (unsigned)s_st.tex[1], (unsigned)s_st.blend, (unsigned)s_st.alpha_ref,
                 (unsigned)s_st.depth_func, (unsigned)s_st.depth_write, (unsigned)s_st.depth_bias,
                 (unsigned)s_st.stencil, (unsigned)s_st.fog, s_st.prim[0], s_st.prim[1], s_st.prim[2], s_st.prim[3],
                 s_st.env[0], s_st.env[1], s_st.env[2], s_st.env[3], (unsigned)R.geom, (unsigned)R.oml,
                 (int)s_batch[0].x, (int)s_batch[0].y, (int)(s_batch[0].z * 1000.0f / s_out.zmax), s_batch[0].rgba[3]);
    if (s_extend) s_side_drawn = 1;
    xgfx_draw(&s_st, s_batch, s_nb);
#ifdef XGFX_TRACE_DUMP_AT
    if (s_trace) {   /* -DXGFX_TRACE_DUMP_AT=<n>: the picture after the trace frame's n-th draw */
        static int s_ndraw;
        if (++s_ndraw == XGFX_TRACE_DUMP_AT) {
            xhw_logf("[DRAW] dump after draw %d", s_ndraw);
            xgfx_debug_dump();
        }
    }
#endif
    s_nb = 0;
}

/* --- textures: a cache keyed by RAM address, tile key and a content sample --- */
#define TEX_CACHE 1024
typedef struct {
    uint32_t addr, key, sample, frame;
    uint32_t full;   /* every word's hash of a sampled (big) block load; 0 otherwise */
    uint32_t hot;    /* its address was seen to change: rechecked every frame */
    uint32_t handle;
    uint16_t w, h, pw, ph;
} TexEntry;
static TexEntry s_texc[TEX_CACHE];
static uint32_t s_frame_no = 1;

/* every word of a texture up to this size, 16 samples of a larger one: a
 * game that writes its font's glyphs into the same slots changed bytes the
 * samples missed (Blast Corps' intro text: T for G). -DXGFX_TEX_FULLHASH_MAX=64:
 * as before. A larger one is also hashed whole now and then (XGFX_TEX_RECHECK). */
#ifndef XGFX_TEX_FULLHASH_MAX
#define XGFX_TEX_FULLHASH_MAX 2048
#endif
/* A block load bigger than that is hashed whole every 8th frame it is used
 * (staggered by cache slot), and every frame once its address has been seen
 * to change: Pokémon Snap types its dialogue into a 152x13 RGBA16 strip
 * (3952 bytes) and the 16 samples kept old glyphs; hashing every big texture
 * every frame cost Snap's lab 2.5 ms a frame (xemu). -DXGFX_TEX_RECHECK=0: the
 * samples alone, as before */
#ifndef XGFX_TEX_RECHECK
#define XGFX_TEX_RECHECK 1
#endif
static uint32_t full_hash(const uint8_t* p, uint32_t bytes) {
    uint32_t h = 2166136261u, g = 0x811C9DC5u ^ bytes, i, w, v;
    for (i = 0; i + 8 <= bytes; i += 8) {   /* two lanes, eight bytes a step (the multiplies overlap) */
        memcpy(&w, p + i, 4);
        memcpy(&v, p + i + 4, 4);
        h = (h ^ w) * 16777619u;
        g = (g ^ v) * 16777619u;
    }
    if (i + 4 <= bytes) {
        memcpy(&w, p + i, 4);
        h = (h ^ w) * 16777619u;
    }
    return (h ^ (g * 0x9E3779B1u) ^ bytes) | 1u;
}
static uint32_t sample_hash(const uint8_t* p, uint32_t bytes) {
    uint32_t h = 2166136261u, step = bytes > XGFX_TEX_FULLHASH_MAX ? bytes / 16 : 4, i, w;
    for (i = 0; i + 4 <= bytes; i += step) {
        memcpy(&w, p + (i & ~3u), 4);
        h ^= w;
        h *= 16777619u;
    }
    return h ^ bytes;
}

/* level load: the game reuses its heap, so everything goes (wings64-dc traps.md) */
void f3d_tex_reset(void) {
    int i;
    flush();
    for (i = 0; i < TEX_CACHE; i++)
        if (s_texc[i].handle) xgfx_tex_destroy(s_texc[i].handle);
    memset(s_texc, 0, sizeof s_texc);
}

static TexEntry* tex_evict_slot(unsigned slot) {
    TexEntry* e = &s_texc[slot];
    if (e->handle) xgfx_tex_destroy(e->handle);
    memset(e, 0, sizeof *e);
    return e;
}

#if XGFX_TEX_MEMO
#define MEMO 64
static struct { uint32_t frame, addr, key; TexEntry* e; } s_memo[MEMO];
#endif

/* a tile inside a bigger LOADBLOCK finds it (Banjo-Kazooie); -DXGFX_TMEM_WITHIN=0: a miss, as before */
#ifndef XGFX_TMEM_WITHIN
#define XGFX_TMEM_WITHIN 1
#endif
/* the last display list ended with a FullSync (the RDP raises its interrupt
 * there: a task without one gets no DP done) */
static int s_fullsync;
static void tex_miss(const char* why, int tidx);
static void fb_touch(uint32_t addr, uint32_t bytes);
static uint32_t s_cimg_w, s_cimg_siz, s_cimg_maxy;   /* the colour image's width and pixel size; the frame's lowest scissor row */


/* G_MOVEWORD G_MW_SEGMENT; test builds log segment 15's first values (Pokemon
 * Snap's photo check copies the crosshair's 8x8 from an image there) */
static void set_segment(unsigned n, uint32_t v) {
    if (XHW_TEST_BUILD && n == 15 && R.seg[15] != v) {
        static unsigned s_n15;
        if (s_n15++ < 8)
            xhw_logf("[GFX] segment 15 = %08x (cimg %08x zimg %08x)", (unsigned)v, (unsigned)R.cimg, (unsigned)R.zimg);
    }
    R.seg[n & 15] = v;
}
/* a palette's hash for the cache key, worked out again only when a
 * LOADTLUT has written TMEM's palette half since (s_tlut_gen) or another
 * palette is asked for: a CI8 texture hashed all 256 entries at every bind,
 * before the per-frame memo could answer (GoldenEye's binds were an eighth
 * of its display-list time on the host). The same keys.
 * -DXGFX_TLUT_HASH_CACHE=0: hashed at every bind, as before */
#ifndef XGFX_TLUT_HASH_CACHE
#define XGFX_TLUT_HASH_CACHE 1
#endif
static uint32_t s_tlut_gen = 1, s_th_gen, s_th_b, s_th_n, s_th_lut, s_th;

/* The texture tile `tidx` samples: converted, padded to powers of two,
 * uploaded once and cached. NULL when it can't be made. */
static TexEntry* texture_for_tile(int tidx) {
    static uint8_t raw[8192];
    static uint32_t conv[1024 * 64];
    Tile* t = &R.tile[tidx];
    TmemLoad* ld = &R.loads[(t->tmem >> 3) & 63];
    TmemLoad within;
    /* the tile's 10.2 coordinates wrap in 12 bits, as the RDP's: a scrolling
     * tile may have lrs below uls (Ocarina of Time's file select and water),
     * and its mask gives the size. -DXGFX_TILE_WRAP=0: unwrapped, as before */
    unsigned w = XGFX_TILE_WRAP ? ((((unsigned)t->lrs - t->uls) & 0xFFFu) >> 2) + 1 : (unsigned)(((t->lrs - t->uls) >> 2) + 1),
             h = XGFX_TILE_WRAP ? ((((unsigned)t->lrt - t->ult) & 0xFFFu) >> 2) + 1 : (unsigned)(((t->lrt - t->ult) >> 2) + 1);
    /* RGBA32 splits each texel across TMEM's two banks: SETTILE's line covers half a row */
    unsigned line = t->line * 8u * (t->siz == 3 ? 2u : 1u), bytes, pw, ph, slot, i, pal;
    uint32_t key, sample, full = 0;
    int big, hot = 0;
    uint64_t tex_t0, hash_t0;
    TexEntry* e;
    if (!ld->valid && XGFX_TMEM_WITHIN) {
        /* a tile inside a bigger LOADBLOCK that started lower in TMEM (Banjo-
         * Kazooie loads several textures, or a texture and its palette, with
         * one block and points tiles into it): that load, from the tile's
         * offset on */
        int k;
        const TmemLoad* best = NULL;
        for (k = ((t->tmem >> 3) & 63) - 1; k >= 0; k--) {
            const TmemLoad* c = &R.loads[k];
            if (!c->valid || c->tile || c->tmem > t->tmem || (uint32_t)(t->tmem - c->tmem) * 8u >= c->bytes) continue;
            if (!best || c->seq > best->seq) best = c;
        }
        if (best) {
            uint32_t off = (uint32_t)(t->tmem - best->tmem) * 8u;
            within = *best;
            within.addr += off;
            within.bytes -= off;
            within.tmem = t->tmem;
            ld = &within;
        }
    }
    if (!ld->valid || t->line == 0) { tex_miss("no load", tidx); return NULL; }
    if (t->masks && (1u << t->masks) < w) w = 1u << t->masks;
    if (t->maskt && (1u << t->maskt) < h) h = 1u << t->maskt;
    /* DIVERGES: a tile one texel wider than its line (Mario Party's board,
     * 64-texel rows under a 0-64 tile, for the filter's last column): the
     * RDP reads that column from the next row in TMEM; here the row's own
     * texels, the edge clamped. -DXGFX_TILE_LINEW=0: a miss, as before */
    if (XGFX_TILE_LINEW && w * (4u << t->siz) > line * 8u) w = line * 8u / (4u << t->siz);
    /* tiles may declare more rows than were loaded (UVBT, fonts): only loaded rows exist */
    if (h > ld->bytes / line) h = ld->bytes / line;
    /* a load shorter than one row (Banjo-Kazooie: 48 bytes under a 64-byte
     * line) still fills that row's start: one row, the rest as RDRAM has it */
    if (!h && ld->bytes && XGFX_TILE_WRAP) h = 1;
    if (w == 0 || h == 0 || w > 1024 || h > 1024) { tex_miss("size", tidx); return NULL; }
    if (XHW_TEST_BUILD && t->siz == 3) {   /* test builds: the first 32-bit tiles' shapes (Mario Party's logo) */
        static uint32_t s_seen[12];
        static unsigned s_n;
        unsigned k;
        for (k = 0; k < s_n && s_seen[k] != ld->addr; k++) {}
        if (k == s_n && s_n < 12) {
            s_seen[s_n++] = ld->addr;
            xhw_logf("[GFX] 32-bit tile %d: %ux%u (tile %u-%u x %u-%u), line %u words, load %08x %u bytes dxt %u %s",
                     tidx, w, h, t->uls >> 2, t->lrs >> 2, t->ult >> 2, t->lrt >> 2, t->line, (unsigned)ld->addr,
                     (unsigned)ld->bytes, ld->dxt, ld->tile ? "tile" : "block");
        }
    }
    bytes = line * h;
    if (bytes > sizeof raw) { tex_miss("too big", tidx); return NULL; }
    key = t->fmt | t->siz << 3 | (w - 1) << 5 | (h - 1) << 15 | (uint32_t)(line & 0xFF) << 24 | (uint32_t)(ld->dxt != 0) << 31;
    pal = t->fmt == F3D_G_IM_FMT_CI ||
          (XGFX_RGBA_LOWBIT && t->fmt == F3D_G_IM_FMT_RGBA && t->siz <= F3D_G_IM_SIZ_8b && f3d_omh_text_lut(R.omh));
    if (pal) {   /* the palette is part of the picture: in the key (equality only) */
        unsigned n = t->siz == F3D_G_IM_SIZ_4b ? 16u : 256u, b = t->siz == F3D_G_IM_SIZ_4b ? t->palette * 16u : 0u;
        uint32_t th = f3d_omh_text_lut(R.omh);
        if (XGFX_TLUT_HASH_CACHE && s_th_gen == s_tlut_gen && s_th_b == b && s_th_n == n && s_th_lut == th) {
            th = s_th;
        } else {
            uint32_t lut = th;
            for (i = 0; i < n; i++) th = (th ^ R.tlut[b + i]) * 16777619u;
            s_th_gen = s_tlut_gen, s_th_b = b, s_th_n = n, s_th_lut = lut, s_th = th;
        }
        key ^= th | 1u;
    }
#if XGFX_TEX_MEMO
    {
        unsigned m = ((ld->addr >> 3) ^ (key * 0x9E3779B1u >> 26)) & (MEMO - 1);
        if (s_memo[m].frame == s_frame_no && s_memo[m].addr == ld->addr && s_memo[m].key == key && s_memo[m].e &&
            s_memo[m].e->addr == ld->addr && s_memo[m].e->key == key)
            return s_memo[m].e;
    }
#endif
    if (fbfx_stale_count())   /* a frame the game draws with: RDRAM gets it first */
        fb_touch(ld->addr, ld->tile ? (ld->rows - 1u) * ld->stride + ld->row_bytes : bytes);
    /* the change check hashes the image as it is (its byte order does not matter for that) */
    {
        static int s_detail = -1;
        if (s_detail < 0) s_detail = xhw_perf_detail();
        hash_t0 = s_detail ? xhw_time_us() : 0;
    }
    if (ld->tile && XGFX_TILE_ROWHASH) {
        /* every row (16 spread rows past XGFX_TEX_FULLHASH_MAX bytes): Pokémon
         * Snap types its dialogue into a texture loaded with LOADTILE, and a
         * strip whose first and last rows stay blank kept its old middle
         * (ascenders lost: "touna" for "found") */
        unsigned r, nr = ld->rows, rs = (uint32_t)nr * ld->row_bytes > XGFX_TEX_FULLHASH_MAX && nr > 16 ? nr / 16u : 1u;
        sample = 0;
        for (r = 0; r < nr; r += rs)
            sample = (sample ^ sample_hash(host(ld->addr + r * ld->stride), span(ld->addr + r * ld->stride, ld->row_bytes))) * 16777619u;
        sample ^= sample_hash(host(ld->addr + (nr - 1u) * ld->stride), span(ld->addr + (nr - 1u) * ld->stride, ld->row_bytes));
    } else
    sample = sample_hash(host(ld->addr), span(ld->addr, ld->tile ? ld->row_bytes : bytes)) ^
             (ld->tile ? sample_hash(host(ld->addr + (ld->rows - 1u) * ld->stride),
                                     span(ld->addr + (ld->rows - 1u) * ld->stride, ld->row_bytes)) : 0);
    if (hash_t0)   /* [PERF2]: the samples' time and the bytes they cover */
        xhw_perf_add(XHW_PERF_TEXHASH, xhw_time_us() - hash_t0),
            xhw_perf_add(XHW_PERF_TEXBYTES, ld->tile ? (uint64_t)ld->rows * ld->row_bytes : bytes);
    slot = ((ld->addr >> 3) * 2654435761u ^ key * 40503u) % TEX_CACHE;
    big = XGFX_TEX_RECHECK && !ld->tile && bytes > XGFX_TEX_FULLHASH_MAX;
    for (i = 0; i < 8; i++) {   /* 8-way probe */
        e = &s_texc[(slot + i) % TEX_CACHE];
        if (!e->handle || e->addr != ld->addr || e->key != key) continue;
        if (e->sample != sample) { hot = 1; continue; }   /* the same place, changed */
        if (big && (e->hot || ((s_frame_no + slot + i) & 7u) == 0)) {
            full = full_hash(host(ld->addr), span(ld->addr, bytes));
            if (full != e->full) {   /* changed where the samples did not look */
                hot = 1;
                e = tex_evict_slot((slot + i) % TEX_CACHE);
                goto build;
            }
        }
        goto hit;
    }
    {   /* a free slot, else the least recently used of the eight */
        unsigned pick = slot;
        for (i = 0; i < 8; i++) {
            TexEntry* c = &s_texc[(slot + i) % TEX_CACHE];
            if (!c->handle) { pick = (slot + i) % TEX_CACHE; break; }
            if (c->frame < s_texc[pick].frame) pick = (slot + i) % TEX_CACHE;
        }
        e = tex_evict_slot(pick);
    }
build:
    tex_t0 = xhw_time_us();
    if (ld->tile) {   /* LOADTILE: the rectangle's rows, laid out at the tile's line */
        uint32_t src = seg(ld->addr);
        unsigned r, n = ld->row_bytes < line ? ld->row_bytes : line;
        memset(raw, 0, bytes);
        for (r = 0; r < h && r < ld->rows; r++) xu_from_rdram(raw + r * line, src + r * ld->stride, n);
    } else {
        xu_from_rdram(raw, seg(ld->addr), (bytes + 7) & ~7u);   /* in N64 byte order */
    }
    if (ld->tile) {
    } else if (ld->dxt == 0) {   /* stored in TMEM order */
        /* DIVERGES: none meant. RGBA32's odd rows swap texel pairs (the two
         * banks), not 32-bit words: Smash Bros.'s RGBA32 portraits came out
         * striped. -DXGFX_DESWIZZLE32=0: the 16-bit rule for every size. */
        if (t->siz == 3 && XGFX_DESWIZZLE32) texconv_deswizzle32(raw, line, bytes / line);
        else texconv_deswizzle(raw, line, bytes / line);
    }
#if XGFX_DXT_EXACT
    else if (t->siz != 3 && (uint32_t)ld->dxt * (line / 8) != 2048u)
        texconv_deswizzle_dxt(raw, line, bytes / line, ld->dxt);
    /* RGBA32 the same way, its texel pairs trading places (the rule carried
     * over from 16 bits; no game seen needing it yet). -DXGFX_DXT32=0: as loaded */
    else if (t->siz == 3 && XGFX_DXT32 && (uint32_t)ld->dxt * (line / 8) != 2048u)
        texconv_deswizzle32_dxt(raw, line, bytes / line, ld->dxt);
#endif
    pw = texconv_pow2(w);
    ph = texconv_pow2(h);
    if (pw * ph > sizeof conv / 4) { tex_miss("too big", tidx); return NULL; }
    /* RGBA at 4 or 8 bits (no such format: Banjo-Kazooie's start screen,
     * vbk12): the RDP reads it through the palette when the TLUT is on, as
     * CI; with it off, as intensity (I4, I8). -DXGFX_RGBA_LOWBIT=0: a miss */
    if (pal ? !texconv_ci_to_argb8(raw, line, w, h, (n64_siz_t)t->siz, R.tlut, t->palette * 16u,
                                   f3d_omh_text_lut(R.omh) == 3, conv, pw, ph)
            : !texconv_to_argb8(raw, line, w, h,
                                XGFX_RGBA_LOWBIT && t->fmt == F3D_G_IM_FMT_RGBA && t->siz <= F3D_G_IM_SIZ_8b
                                    ? (n64_fmt_t)F3D_G_IM_FMT_I
                                    : (n64_fmt_t)t->fmt,
                                (n64_siz_t)t->siz, conv, pw, ph)) {
        tex_miss("format", tidx);
        return NULL;
    }
#ifdef XGFX_TEXLOG   /* debug: -DXGFX_TEXLOG=<n>, the first n textures made (format, size, palette, source) */
#ifndef XGFX_TEXLOG_FROM
#define XGFX_TEXLOG_FROM 0   /* from this frame on */
#endif
    if (s_frame_no >= (unsigned)(XGFX_TEXLOG_FROM)) {
        static unsigned s_tl;
        if (s_tl++ < (unsigned)(XGFX_TEXLOG))
            xhw_logf("[TEX] %u: fmt %d siz %d %ux%u line %u pal %u tlut %u (%04x %04x) %s %08x %u bytes dxt %u, px0 %08x",
                     s_tl, t->fmt, t->siz, w, h, t->line, t->palette, (unsigned)f3d_omh_text_lut(R.omh),
                     R.tlut[t->palette * 16u], R.tlut[t->palette * 16u + 1u], ld->tile ? "tile" : "block",
                     (unsigned)ld->addr, (unsigned)ld->bytes, ld->dxt, (unsigned)conv[0]);
    }
#endif
    e->handle = xgfx_tex_create(pw, ph, conv);
    if (!e->handle) {   /* pool full: drop everything not used this frame and try once more */
        for (i = 0; i < TEX_CACHE; i++)
            if (s_texc[i].handle && s_texc[i].frame != s_frame_no && &s_texc[i] != e) tex_evict_slot(i);
        e->handle = xgfx_tex_create(pw, ph, conv);
        if (!e->handle) { tex_miss("pool full", tidx); return NULL; }
    }
    e->addr = ld->addr;
    e->key = key;
    e->sample = sample;
    e->full = big ? (full ? full : full_hash(host(ld->addr), span(ld->addr, bytes))) : 0;
    e->hot = hot;
    e->w = (uint16_t)w;
    e->h = (uint16_t)h;
    e->pw = (uint16_t)pw;
    e->ph = (uint16_t)ph;
    if (!s_benching) xhw_perf_add(XHW_PERF_TEX, xhw_time_us() - tex_t0);   /* ([DLBENCH] times its own runs) */
hit:
    e->frame = s_frame_no;
#if XGFX_TEX_MEMO
    {
        unsigned m = ((ld->addr >> 3) ^ (key * 0x9E3779B1u >> 26)) & (MEMO - 1);
        s_memo[m].frame = s_frame_no;
        s_memo[m].addr = ld->addr;
        s_memo[m].key = key;
        s_memo[m].e = e;
    }
#endif
    return e;
}

/* the first misses of each kind, for the log */
static uint32_t s_ring[16][2], s_ring_n;   /* test builds: run_dl's last 16 commands, as the list holds them */
static void tex_miss(const char* why, int tidx) {
    static unsigned s_n;
    const Tile* t = &R.tile[tidx & 7];
    const TmemLoad* ld = &R.loads[(t->tmem >> 3) & 63];
    if (XGFX_TRACE_MISS && !s_trace_next) s_trace_next = 1;
    if (s_n++ >= 48) return;
    xhw_logf("[GFX] texture miss (%s): tile %d fmt %d siz %d line %d tmem %d s %d-%d t %d-%d mask %d/%d | load %d "
             "addr %08x bytes %u dxt %d",
             why, tidx, t->fmt, t->siz, t->line, t->tmem, t->uls >> 2, t->lrs >> 2, t->ult >> 2, t->lrt >> 2, t->masks,
             t->maskt, ld->valid, (unsigned)ld->addr, (unsigned)ld->bytes, ld->dxt);
    if (s_n <= 4) {   /* what TMEM holds then */
        int k;
        for (k = 0; k < 64; k++)
            if (R.loads[k].valid)
                xhw_logf("[GFX]   load at tmem %u: addr %08x bytes %u %s", (unsigned)R.loads[k].tmem,
                         (unsigned)R.loads[k].addr, (unsigned)R.loads[k].bytes, R.loads[k].tile ? "tile" : "block");
        if (XHW_TEST_BUILD && s_n <= 2)   /* and the commands before it, oldest first */
            for (k = 0; k < 16; k++) {
                const uint32_t* c = s_ring[(s_ring_n + (unsigned)k) & 15];
                xhw_logf("[GFX]   cmd -%02d: %08x %08x", 16 - k, (unsigned)c[0], (unsigned)c[1]);
            }
    }
}

static float shift_scale(int sh) {
    if (sh == 0) return 1.0f;
    if (sh <= 10) return 1.0f / (float)(1 << sh);
    return (float)(1 << (16 - sh));
}

/* the RDP clamps an axis whose mask is 0 whatever its clamp bit says (no
 * wrap without a mask): Ocarina of Time's moon is a 64x64 tile on a quad
 * whose coordinates run to 128, and repeating drew four moons.
 * -DXGFX_MASK0_CLAMP=0: the clamp bit alone, as before */
/* The RDP clamps at the tile's extent (SH - SL), not at the mask: an axis
 * with its clamp bit set and a tile wider than 2^mask texels repeats (or
 * mirrors) the masked texture across the tile and clamps only past its
 * edge. Pokemon Snap's Beach cliffs are a 64x64 texture (mask 6) on a 768
 * texel tile with clamp set, at coordinates 324..383: clamped at 64 they
 * drew one flat grey. The GPU has no clamp at a distance, so such an axis
 * repeats. -DXGFX_CLAMP_EXTENT=0: the clamp bit at the texture's edge */
static uint32_t wrap_of(uint8_t cm, uint8_t mask, unsigned extent) {
    if (XGFX_MASK0_CLAMP && !mask) return XGFX_WRAP_CLAMP;
    if (XGFX_CLAMP_EXTENT && (cm & 2) && mask < 16 && extent > (1u << mask))
        return (cm & 1) ? XGFX_WRAP_MIRROR : XGFX_WRAP_REPEAT;
    return (cm & 2) ? XGFX_WRAP_CLAMP : (cm & 1) ? XGFX_WRAP_MIRROR : XGFX_WRAP_REPEAT;
}

/* does the combiner read TEXEL1 (mux value 2 in an rgb slot, 9 in C, 2 in an alpha slot)? */
static int cc_uses_texel1(void) {
    uint8_t f[2][8];
    int cy, k;
    f3d_cc_decode(R.cc0, R.cc1, f);
    for (cy = 0; cy < 2; cy++) {
        for (k = 0; k < 4; k++)
            if (f[cy][k] == 2 || (k == 2 && f[cy][k] == 9)) return 1;
        for (k = 4; k < 8; k++)
            if (f[cy][k] == 2) return 1;
    }
    return 0;
}

static void bind_unit(int u, int tidx) {
    Tile* t = &R.tile[tidx & 7];
    TexEntry* e = texture_for_tile(tidx & 7);
    s_st.tex[u] = e ? e->handle : 0;
    if (s_trace)
        xhw_logf("[DRAW] bind %d tile %d fmt %u siz %u line %u tmem %u sl %u tl %u sh %u th %u pal %u cm %u/%u mask %u/%u shift %u/%u -> addr %08x %ux%u (scale %04x/%04x)",
                 u, tidx & 7, t->fmt, t->siz, t->line, t->tmem, t->uls >> 2, t->ult >> 2, t->lrs >> 2, t->lrt >> 2,
                 t->palette, t->cms, t->cmt, t->masks, t->maskt, t->shifts, t->shiftt, e ? (unsigned)e->addr : 0u,
                 e ? e->pw : 0u, e ? e->ph : 0u, (unsigned)R.tex_ss, (unsigned)R.tex_st);
    if (!e) return;
    s_st.wrap_s[u] = wrap_of(t->cms, t->masks, t->lrs > t->uls ? (unsigned)((t->lrs - t->uls) >> 2) + 1u : 1u);
    s_st.wrap_t[u] = wrap_of(t->cmt, t->maskt, t->lrt > t->ult ? (unsigned)((t->lrt - t->ult) >> 2) + 1u : 1u);
    if (XHW_TEST_BUILD && u == 0) {   /* test builds: what the mask-0 clamp changes (flush logs it) */
        s_m0[0] = !t->masks && !(t->cms & 2) ? (float)e->w / (float)e->pw : -1.0f;
        s_m0[1] = !t->maskt && !(t->cmt & 2) ? (float)e->h / (float)e->ph : -1.0f;
        s_m0_tile = (uint32_t)t->fmt << 24 | (uint32_t)t->siz << 16 | (uint32_t)e->w << 8 | e->h;
        s_m0_addr = e->addr;
    }
    s_st.filter[u] = ((R.omh >> F3D_G_MDSFT_TEXTFILT) & 3) != 0;
    /* u = (s * shift - uls) / padded width */
    s_u_scale[u] = shift_scale(t->shifts) / (float)e->pw;
    s_u_off[u] = t->uls * 0.25f / (float)e->pw;
    s_v_scale[u] = shift_scale(t->shiftt) / (float)e->ph;
    s_v_off[u] = t->ult * 0.25f / (float)e->ph;
}

#ifndef XGFX_BLEND_MEM
#define XGFX_BLEND_MEM 1
#endif
/* N64 render mode -> XgfxState pixel choices (wings64-dc draw_state) */
/* Fog is the blender's: G_FOG in the geometry mode only makes the RSP work
 * out each vertex's fog amount; the colour is blended toward the fog colour
 * only where the render mode's P input is G_BL_CLR_FOG (3) in the first
 * cycle (G_RM_FOG_SHADE_A, G_RM_FOG_PRIM_A), or in the second when it is two
 * cycles. Banjo-Kazooie's models all draw with G_RM_PASS in the first cycle
 * (its decomp, modelRender.c's tables) and leave G_FOG as the last model set
 * it: its actor shadows (badShad.c: the shadow model, depth compare, PASS)
 * were fogged toward the level's fog colour, orange now and then on red
 * (vbk22). Found by a subagent from the decomp. -DXGFX_FOG_BLENDER=0: G_FOG
 * alone fogs, as before; -DXGFX_FOG_LOG=1: release builds log each render
 * mode that loses its fog, once */
#ifndef XGFX_FOG_BLENDER
#define XGFX_FOG_BLENDER 1
#endif
#ifndef XGFX_FOG_LOG
#define XGFX_FOG_LOG 0
#endif
static void pixel_state(int depth_override_always) {
    uint32_t rm = R.oml;
    int cycle = (R.omh >> F3D_G_MDSFT_CYCLETYPE) & 3;
    int zb = (R.geom & F3D_G_ZBUFFER) && !depth_override_always;
    s_st.blend = ((rm & F3D_FORCE_BL) && cycle < 2) ? XGFX_BLEND_ALPHA : XGFX_BLEND_OFF;
    /* a one-cycle blender that never reads the frame (no MEM as P or M in
     * either cycle's fields: All-Star Baseball 2000's sky and grass, IN * 0 +
     * IN * 1 under FORCE_BL) writes its input as it is: no blending with the
     * combiner's alpha, which was 0 there (the sky came out the clear colour).
     * -DXGFX_BLEND_MEM=0: FORCE_BL alone blends, as before */
    if (XGFX_BLEND_MEM && s_st.blend == XGFX_BLEND_ALPHA && cycle == 0) {
        uint32_t p0 = (rm >> 30) & 3, p1 = (rm >> 28) & 3, m0 = (rm >> 22) & 3, m1 = (rm >> 20) & 3;
        if (p0 != 1 && p1 != 1 && m0 != 1 && m1 != 1) {
            s_st.blend = XGFX_BLEND_OFF;
            if (XHW_TEST_BUILD) {   /* which render modes this changes, once each */
                static uint32_t s_seen[8];
                unsigned k;
                for (k = 0; k < 8 && s_seen[k] && s_seen[k] != rm; k++) {}
                if (k < 8 && !s_seen[k]) s_seen[k] = rm, xhw_logf("[GFX] render mode %08x: FORCE_BL, no MEM read: opaque", (unsigned)rm);
            }
        }
    }
    if ((rm & 3) == 1)   /* G_AC_THRESHOLD: against the blend colour's alpha */
        s_st.alpha_ref = R.blend[3] ? R.blend[3] : 1;
    else if ((rm & F3D_CVG_X_ALPHA) && cycle < 2)
        s_st.alpha_ref = 128;   /* coverage x alpha cutouts: alpha test at 0.5 */
    else
        s_st.alpha_ref = 0;
    s_st.depth_func = zb && (rm & F3D_Z_CMP) ? XGFX_DEPTH_LEQUAL : (zb && (rm & F3D_Z_UPD)) ? XGFX_DEPTH_ALWAYS
                                                                                              : XGFX_DEPTH_OFF;
    s_st.depth_write = zb && (rm & F3D_Z_UPD);
#ifdef XGFX_DEBUG_NOZ
    s_st.depth_func = XGFX_DEPTH_OFF;
    s_st.depth_write = 0;
#endif
    s_decal = ((rm >> 10) & 3) == 3;
    s_st.depth_bias = 0;
    s_zrel = 0.0f;
    if (s_st.depth_func == XGFX_DEPTH_LEQUAL) {
        /* DIVERGES: later depth-tested states are pulled toward the eye by a
         * step each (64 at most), so coplanar layers (the landing pads'
         * bullseye) resolve to the later draw as on the RDP */
        if (XGFX_Z_TIEBREAK && s_zorder < 64) s_zorder++;
        s_st.depth_bias = (XGFX_Z_TIEBREAK ? s_zorder : 0) + (s_decal ? 16u : 0u);
        if (XGFX_Z_REL) {
            /* DIVERGES: the bias is a fraction of the distance (2^-16 per
             * step, as wings64-dc's), applied to the vertices' depth in
             * to_out; a polygon offset of that many Z24 steps is nothing at
             * a distance and the landing target's bullseye striped on the
             * console (runs 1-3; xemu's float depth hid it) */
            s_zrel = (float)s_st.depth_bias * XGFX_Z_REL_STEP;
            s_st.depth_bias = 0;
        }
    }
    s_st.color_write = 1;
    s_st.stencil = XGFX_STENCIL_OFF;
    s_st.fog = (R.geom & F3D_G_FOG) && cycle < 2;
    if (XGFX_FOG_BLENDER && s_st.fog) {
        uint32_t p0 = (rm >> 30) & 3, p1 = (rm >> 28) & 3;
        if (p0 != 3 && !(cycle == 1 && p1 == 3)) {
            s_st.fog = 0;
            if (XHW_TEST_BUILD || XGFX_FOG_LOG) {   /* which render modes lose it, once each */
                static uint32_t s_seen[8];
                unsigned k;
                for (k = 0; k < 8 && s_seen[k] && s_seen[k] != rm; k++) {}
                if (k < 8 && !s_seen[k])
                    s_seen[k] = rm, xhw_logf("[GFX] render mode %08x: G_FOG on, the blender takes no fog colour: unfogged "
                                             "(fog colour %02x%02x%02x)", (unsigned)rm, R.fogc[0], R.fogc[1], R.fogc[2]);
            }
        }
    }
    memcpy(s_st.fog_color, R.fogc, 4);
    memcpy(s_st.prim, R.prim, 4);
    memcpy(s_st.env, R.env, 4);
    s_st.prim_lod_frac = R.prim_lod_frac;
    s_st.lod_frac = 0;   /* DIVERGES: one mip level is drawn (PLAN.md D4 phase A) */
    /* hor+ (map_setup): a world view's perspective draw under the full-screen
     * scissor is drawn across the whole width; other draws stay in the box,
     * moved by the HUD anchor ('L' / 'R': the side margin) */
    s_extend = s_hp && (s_world || (XGFX_WORLD_GENERIC && !s_tagged)) && !s_sc_on && !depth_override_always /* a rectangle */ &&
               R.proj[2][3] != 0.0f;
    s_xoff = 0.0f;
    if (s_hp && !s_extend && XGFX_HUD_ANCHOR) {
        if (s_anchor == 'L') s_xoff = s_wx0 - s_cx;
        else if (s_anchor == 'R') s_xoff = s_wx1 - (s_cx + s_cw);
    }
    {   /* the clip rectangle: the content rect, or a small scissor/viewport */
        float x0 = s_sc_on ? s_sc[0] : 0.0f, y0 = s_sc_on ? s_sc[1] : 0.0f;
        float x1 = s_sc_on ? s_sc[2] : N64_W, y1 = s_sc_on ? s_sc[3] : N64_H;
        if (s_sc_on) {
            s_st.scissor[0] = (int32_t)(x0 * s_sx + s_ox + s_xoff + 0.5f);
            s_st.scissor[1] = (int32_t)(y0 * s_sy + s_oy + 0.5f);
            s_st.scissor[2] = (int32_t)(x1 * s_sx + s_ox + s_xoff + 0.5f);
            s_st.scissor[3] = (int32_t)(y1 * s_sy + s_oy + 0.5f);
        } else {   /* x: scissor_x, once s_overlay is known */
            s_st.scissor[0] = (int32_t)(s_cx + s_xoff);
            s_st.scissor[1] = (int32_t)s_cy;
            s_st.scissor[2] = (int32_t)(s_cx + s_cw + s_xoff);
            s_st.scissor[3] = (int32_t)(s_cy + s_ch);
        }
    }
}

/* the full-screen clip's x: the drawn width for an extended world draw and
 * for the fills overlay_full may stretch to the 16:9 edges; the box (moved
 * by the anchor) for the rest, which the N64 cropped at its screen edge */
static void scissor_x(void) {
    if (s_sc_on || !(s_extend || (s_overlay && s_hp))) return;
    s_st.scissor[0] = (int32_t)s_wx0;
    s_st.scissor[2] = (int32_t)s_wx1;
}

/* the combiner reads no texel: a texture bound under a shade-only combiner (sky gradients) */
static int cc_reads_texel0(void) {
    uint8_t f[2][8];
    int cy, k, cycles = ((R.omh >> F3D_G_MDSFT_CYCLETYPE) & 3) == 1 ? 2 : 1;
    f3d_cc_decode(R.cc0, R.cc1, f);
    for (cy = 0; cy < cycles; cy++) {
        for (k = 0; k < 4; k++)
            if (f[cy][k] == 1 || f[cy][k] == 2 || (k == 2 && (f[cy][k] == 8 || f[cy][k] == 9))) return 1;
        for (k = 4; k < 8; k++)
            if (f[cy][k] == 1 || f[cy][k] == 2) return 1;
    }
    return 0;
}

#ifndef XGFX_TEX_UNREAD
#define XGFX_TEX_UNREAD 1
#endif
static void draw_state(void) {
    int cycle = (R.omh >> F3D_G_MDSFT_CYCLETYPE) & 3;
    flush();
    memset(&s_st, 0, sizeof s_st);
    s_st.cc0 = R.cc0 & 0x00FFFFFFu;
    s_st.cc1 = R.cc1;
    s_st.two_cycle = cycle == 1;
    pixel_state(0);
    s_textured = s_tex1 = 0;
    /* a combiner that reads no texel draws untextured whatever G_TEXTURE says:
     * no texture is looked up, converted or bound (Mario Party's board intro
     * drew shade-only quads with the texture on: 36 misses of an unset tile
     * 0 a run, each a lookup). Copy and fill modes bypass the combiner.
     * -DXGFX_TEX_UNREAD=0: bound whenever G_TEXTURE is on, as before */
    if (R.tex_on && (!XGFX_TEX_UNREAD || cycle >= 2 || cc_reads_texel0())) {
        bind_unit(0, R.tex_tile);
        s_textured = s_st.tex[0] != 0;
        if (s_st.two_cycle && cc_uses_texel1()) {
            bind_unit(1, R.tex_tile + 1);
            s_tex1 = s_st.tex[1] != 0;
        }
    }
    s_overlay = XGFX_OVERLAY_FULL && s_st.depth_func == XGFX_DEPTH_OFF && (!s_textured || !cc_reads_texel0());
    scissor_x();
    if (R.cimg == R.zimg && R.cimg) {   /* shadow volume pass 1 (shadow volumes below) */
        s_st.stencil = XGFX_STENCIL_MARK;
        s_st.color_write = 0;
        s_st.depth_write = 0;
        s_st.blend = XGFX_BLEND_OFF;
        s_st.alpha_ref = 0;
        s_st.depth_func = XGFX_DEPTH_LEQUAL;
        s_st.depth_bias = 0;
        s_zrel = 0.0f;
    } else if (s_shadow_pass2) {
        s_st.stencil = XGFX_STENCIL_TEST;
    }
    s_st_epoch = s_epoch;
}

/* --------------------------------------------------------------- triangles */

static uint8_t to8(float f) {
    int i = (int)(f * 255.0f + 0.5f);
    return (uint8_t)(i < 0 ? 0 : i > 255 ? 255 : i);
}

static void to_out(const Vtx* cv, XgfxVtx* o, const Vtx* flat) {
    Vtx* v = (Vtx*)cv;   /* the vertex buffer's or a clip copy's: its cache fields are ours */
    uint32_t fogb = (R.geom & F3D_G_FOG) != 0;
    float z01;
    if (!XGFX_VTX_OUT_CACHE || v->ostamp != s_ostamp || v->ofog != fogb) {
        float iw = 1.0f / v->w;
        v->oz = (v->z * iw + 1.0f) * 0.5f;
        v->ox = (R.vp_tx + v->x * iw * R.vp_sx) * s_sx + s_ox;
        v->oy = (R.vp_ty - v->y * iw * R.vp_sy) * s_sy + s_oy;
        v->orgba[0] = to8(v->r);
        v->orgba[1] = to8(v->g);
        v->orgba[2] = to8(v->b);
        v->orgba[3] = fogb ? 255 : to8(v->a);   /* the RSP puts fog in shade alpha; the combiner sees 1 */
        v->ofog = fogb;
        v->ostamp = s_ostamp;
    }
    z01 = v->oz;
    if (s_zrel > 0.0f && R.proj[2][3] < -1e-6f) {
        /* nearer by s_zrel of the eye distance: depth tends to A far away
         * (A = (1 + P22/P23)/2 for a perspective P, scale included), and a
         * point at w * (1 - f) lies (A - z01) * f nearer */
        float a = 0.5f * (1.0f + R.proj[2][2] / R.proj[2][3]);
        z01 -= (a - z01) * s_zrel;
    }
    o->x = v->ox + s_xoff;
    o->y = v->oy;
    o->z = (z01 < 0.0f ? 0.0f : z01 > 1.0f ? 1.0f : z01) * s_out.zmax;
#ifdef XGFX_DEBUG_ZHALF
    o->z *= 0.5f;
#endif
    o->w = v->w;
    if (flat) {
        o->rgba[0] = to8(flat->r);
        o->rgba[1] = to8(flat->g);
        o->rgba[2] = to8(flat->b);
        o->rgba[3] = fogb ? 255 : to8(flat->a);
    } else {
        memcpy(o->rgba, v->orgba, 4);
    }
    o->s0 = v->s * s_u_scale[0] - s_u_off[0];
    o->t0 = v->t * s_v_scale[0] - s_v_off[0];
    o->s1 = v->s * s_u_scale[1] - s_u_off[1];
    o->t1 = v->t * s_v_scale[1] - s_v_off[1];
    o->fog = v->fog * (1.0f / 255.0f);
}

/* SUBSCREEN-sized overlay (fades, the HUD's fade box): stretched to the
 * content rect, since the port draws 3D views unscissored to the edges.
 * DIVERGES: as wings64-dc's overlay_full. With hor+ (map_setup), an
 * untextured fill spanning the view's width (clears, fades, the letterbox's
 * top and bottom bars) is stretched across the drawn width, rows as they
 * are, as Birdman64's Stretch class; the anchor shift does not apply. */
static void overlay_full(XgfxVtx* p, int n) {
    float x0 = p[0].x, x1 = p[0].x, y0 = p[0].y, y1 = p[0].y;
    float sx0 = SUB_X0 * s_sx + s_ox + s_xoff, sx1 = SUB_X1 * s_sx + s_ox + s_xoff, sy0 = SUB_Y0 * s_sy + s_oy,
          sy1 = SUB_Y1 * s_sy + s_oy;
    int i, span_y;
    if (s_title) return;
    for (i = 1; i < n; i++) {
        if (p[i].x < x0) x0 = p[i].x;
        if (p[i].x > x1) x1 = p[i].x;
        if (p[i].y < y0) y0 = p[i].y;
        if (p[i].y > y1) y1 = p[i].y;
    }
    if (x0 > sx0 || x1 < sx1) return;
    span_y = y0 <= sy0 && y1 >= sy1;
    if (!span_y && !s_hp) return;
    if (s_hp) s_side_drawn = 1;   /* stretched across the 16:9 width */
    for (i = 0; i < n; i++) {
        if (p[i].x <= sx0) p[i].x = s_hp ? s_wx0 : s_cx;
        else if (p[i].x >= sx1) p[i].x = s_hp ? s_wx1 : s_cx + s_cw;
        if (!span_y) continue;
        if (p[i].y <= sy0) p[i].y = s_cy;
        else if (p[i].y >= sy1) p[i].y = s_cy + s_ch;
    }
}

/* ---- the probe: small copies of the frame being drawn ----
 * DIVERGES: Pokemon Snap tells what is in a photo by copying the 8x8
 * pixels under its crosshair out of the frame it is drawing (LOADTILE from
 * the colour image, a COPY texrect into an 8-wide image), drawing a
 * Pokemon, copying again, ... and counting on the CPU how many of the 64
 * pixels each one changed (0x80359244: 32 or more and it is the subject).
 * The frame is drawn by the NV2A and is not in RDRAM mid-frame, so here a
 * software grid of that region (the first LOADTILE's rectangle, at most
 * 256 pixels) follows the frame's triangles with their own depths, each
 * pixel holding the number of the copy it was last drawn before; a copy
 * writes those numbers, not colours. What the game compares (changed or
 * not) comes out as the RDP's; colours do not exist here. -DXGFX_PROBE=0:
 * the copies draw on the screen, as before (the photo check sees nothing) */
#define PROBE_MAX 256
#ifndef XGFX_PROBE_MAXW
#define XGFX_PROBE_MAXW 16u
#endif
#ifndef XGFX_PROBE_ROWS
#define XGFX_PROBE_ROWS 2
#endif
static struct {
    int on, pending, logged;
    uint32_t main;               /* the frame's colour image */
    uint32_t target;             /* the copy's small colour image, while set */
    unsigned tw;                 /* its width */
    int x0, y0, w, h;            /* the region, N64 pixels */
    uint16_t epoch;              /* the copy each pixel was last drawn before */
    uint16_t id[PROBE_MAX];
    float z[PROBE_MAX];
} s_probe;

static void probe_reset(void) {
    int k;
    for (k = 0; k < PROBE_MAX; k++) s_probe.id[k] = 0, s_probe.z[k] = 3e38f;
    s_probe.epoch = 1;
}

/* LOADTILE: a rectangle of the colour image being drawn is a copy's source */
static void probe_load(uint32_t addr, unsigned x0, unsigned y0, unsigned x1, unsigned y1, unsigned siz) {
    unsigned w = x1 - x0 + 1u, h = y1 - y0 + 1u;
    s_probe.pending = 0;
    /* a one-row region is a picture copied out a strip at a time (Mario Tennis's 32x32
     * capture in 69x1 loads), not a test of what was drawn there: the copies stay on the
     * normal path (the probe made each strip a grid of its own; the match ran at 12 fps
     * against 30). -DXGFX_PROBE_ROWS=1: probed too */
    if (h < XGFX_PROBE_ROWS) return;
    if (!R.cimg || R.cimg == R.zimg || (addr & 0x1FFFFFFFu) != (R.cimg & 0x1FFFFFFFu) || siz != 2u || w * h > PROBE_MAX)
        return;
    if (!s_probe.on || (int)x0 != s_probe.x0 || (int)y0 != s_probe.y0 || (int)w != s_probe.w || (int)h != s_probe.h) {
        if (!s_probe.logged++)
            xhw_logf("[GFX] probe: the game copies %ux%u at %u,%u of the frame it draws: a software grid follows it", w,
                     h, x0, y0);
        s_probe.on = 1;
        s_probe.x0 = (int)x0, s_probe.y0 = (int)y0, s_probe.w = (int)w, s_probe.h = (int)h;
        s_probe.main = R.cimg;
        probe_reset();
    }
    s_probe.pending = 1;
}

/* SETCIMG: 1 when it is a copy's small target (not the screen); a new
 * frame's colour image starts the grid over */
static int probe_cimg(uint32_t c, unsigned w, unsigned siz) {
    if (!s_probe.on) return 0;
    if (w >= 64u) {
        if (c != R.zimg && c != s_probe.main) s_probe.main = c, probe_reset();
        s_probe.target = 0;
        return 0;
    }
    /* at most 16 wide: Pokemon Snap's photo check copies into 8-wide images; Mario
     * Tennis captures a 32x32 picture of the frame in strips (128 copies a frame), no
     * test of what was drawn, and the marks ran a row past its image into the game's
     * own data (-DXGFX_PROBE_MAXW=32: as before) */
    if (s_probe.pending && siz == 2u && w <= XGFX_PROBE_MAXW) {
        s_probe.target = c;
        s_probe.tw = w;
        if (XHW_TEST_BUILD) {   /* diagnosis: the copy targets */
            static uint32_t s_seen[8];
            static unsigned s_n;
            unsigned k;
            for (k = 0; k < s_n && s_seen[k] != c; k++) {}
            if (k == s_n && s_n < 8) s_seen[s_n++] = c, xhw_logf("[GFX] probe target %08x, %u wide (frame %u)", (unsigned)c, w, (unsigned)s_frame_no);
        }
        return 1;
    }
    return 0;
}

/* a texrect into the copy's target: the grid's numbers into RDRAM */
#ifndef XGFX_PROBE_CLIP
#define XGFX_PROBE_CLIP 1
#endif
/* -DXGFX_PROBE_LOG=1: the probe's copy lines and its 30-frame summary in release builds too (Pokémon
 * Snap on red: the photo check never found a Pokémon in focus, the Beach tutorial never unlocked) */
#ifndef XGFX_PROBE_LOG
#define XGFX_PROBE_LOG 0
#endif
static int probe_copy(uint32_t w0, uint32_t w1) {
    uint16_t xl, yl, xh, yh;
    uint8_t tile;
    int i, j, cw, ch;
    if (!s_probe.target || R.cimg != s_probe.target) return 0;
    f3d_texrect_decode(w0, w1, &xl, &yl, &xh, &yh, &tile);
    /* no more than the texrect covers: Mario
     * Tennis's attract demo copies less than the region it loaded, and writing the
     * whole region at the rect put the marks over its archive handle beside the
     * target. -DXGFX_PROBE_CLIP=0: the region's size, as before */
    {
        int rw = xh >= xl ? ((xh - xl) >> 2) + 1 : 0, rh = yh >= yl ? ((yh - yl) >> 2) + 1 : 0;   /* copy mode: inclusive */
        cw = XGFX_PROBE_CLIP && rw < s_probe.w ? rw : s_probe.w;
        ch = XGFX_PROBE_CLIP && rh < s_probe.h ? rh : s_probe.h;
    }
    if (XHW_TEST_BUILD) {   /* diagnosis: where the copies land */
        static unsigned s_n;
        if (s_n++ < 24)
            xhw_logf("[GFX] probe copy to %08x: rect %u,%u-%u,%u, region %dx%d, scissor %d,%d-%d,%d (frame %u)",
                     (unsigned)s_probe.target, xl >> 2, yl >> 2, xh >> 2, yh >> 2, s_probe.w, s_probe.h, (int)s_scissor[0],
                     (int)s_scissor[1], (int)s_scissor[2], (int)s_scissor[3], (unsigned)s_frame_no);
    }
    for (j = 0; j < ch; j++)
        for (i = 0; i < cw; i++) {
            unsigned dx = (unsigned)(xl >> 2) + (unsigned)i, dy = (unsigned)(yl >> 2) + (unsigned)j;
            if (dx >= s_probe.tw) continue;
            /* and inside the scissor, as the RDP clips (the attract demo's copy lands past
             * the target's rows otherwise) */
            if (XGFX_PROBE_CLIP && ((float)dx < s_scissor[0] || (float)dx >= s_scissor[2] || (float)dy < s_scissor[1] ||
                                    (float)dy >= s_scissor[3]))
                continue;
            wr16(XU_K0(s_probe.target + 2u * (dy * s_probe.tw + dx)),
                 (uint16_t)((s_probe.id[j * s_probe.w + i] << 1) | 1u));
        }
    if (XHW_TEST_BUILD || XGFX_PROBE_LOG) {   /* test builds: the first copies that saw something drawn since the one before
                             * (a frame's first copy, the background, apart: it spent the lines) */
        static unsigned s_said, s_said1;
        int k, changed = 0;
        for (k = 0; k < s_probe.w * s_probe.h; k++) changed += s_probe.id[k] == s_probe.epoch;
        if (changed && (s_probe.epoch == 1 ? s_said1++ < 8 : s_said < 40)) {
            s_said += s_probe.epoch != 1;
            xhw_logf("[GFX] probe copy %u: %d of %d pixels drawn since the last (frame %u)", (unsigned)s_probe.epoch,
                     changed, s_probe.w * s_probe.h, (unsigned)s_frame_no);
        }
        {   /* and every 30 frames: the most a later copy (a Pokemon) saw, how many reached half */
            static unsigned s_from, s_most, s_half, s_copies, s_lines;
            if (s_probe.epoch != 1) {
                s_copies++;
                if ((unsigned)changed > s_most) s_most = (unsigned)changed;
                s_half += changed * 2 >= s_probe.w * s_probe.h;
            }
            if (s_frame_no - s_from >= 30u) {
                if (s_copies && s_lines < 400) {
                    s_lines++;
                    xhw_logf("[GFX] probe frames %u-%u: %u later copies, the most %u of %d, %u at half or more", s_from,
                             (unsigned)s_frame_no, s_copies, s_most, s_probe.w * s_probe.h, s_half);
                }
                s_from = s_frame_no, s_most = s_half = s_copies = 0;
            }
        }
    }
    s_probe.epoch++;
    s_probe.pending = 0;
    return 1;
}

/* a triangle of the frame onto the grid (N64 pixel centres, depth-tested
 * as the RDP would) */
static void probe_tri(const XgfxVtx* a, const XgfxVtx* b, const XgfxVtx* c) {
    float ax = (a->x - s_ox - s_xoff) / s_sx, ay = (a->y - s_oy) / s_sy;
    float bx = (b->x - s_ox - s_xoff) / s_sx, by = (b->y - s_oy) / s_sy;
    float cx = (c->x - s_ox - s_xoff) / s_sx, cy = (c->y - s_oy) / s_sy;
    float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    float rx0 = (float)s_probe.x0, ry0 = (float)s_probe.y0, rx1 = rx0 + (float)s_probe.w, ry1 = ry0 + (float)s_probe.h;
    int zb = (R.geom & F3D_G_ZBUFFER) != 0, cmp = zb && (R.oml & F3D_Z_CMP), upd = zb && (R.oml & F3D_Z_UPD), i, j;
    int i0, i1, j0, j1;
    if (area == 0.0f) return;
    if ((ax < rx0 && bx < rx0 && cx < rx0) || (ax >= rx1 && bx >= rx1 && cx >= rx1) ||
        (ay < ry0 && by < ry0 && cy < ry0) || (ay >= ry1 && by >= ry1 && cy >= ry1))
        return;
    {   /* only the grid pixels under the triangle's bounding box (the region can be 85x3: every
         * triangle of a frame scanned all of it cost Mario Tennis's match 40 ms a frame in xemu) */
        float mnx = ax < bx ? (ax < cx ? ax : cx) : (bx < cx ? bx : cx), mxx = ax > bx ? (ax > cx ? ax : cx) : (bx > cx ? bx : cx);
        float mny = ay < by ? (ay < cy ? ay : cy) : (by < cy ? by : cy), mxy = ay > by ? (ay > cy ? ay : cy) : (by > cy ? by : cy);
        i0 = (int)(mnx - rx0 - 1.0f), i1 = (int)(mxx - rx0 + 1.0f), j0 = (int)(mny - ry0 - 1.0f), j1 = (int)(mxy - ry0 + 1.0f);
        if (i0 < 0) i0 = 0;
        if (j0 < 0) j0 = 0;
        if (i1 > s_probe.w - 1) i1 = s_probe.w - 1;
        if (j1 > s_probe.h - 1) j1 = s_probe.h - 1;
    }
    for (j = j0; j <= j1; j++)
        for (i = i0; i <= i1; i++) {
            float px = rx0 + (float)i + 0.5f, py = ry0 + (float)j + 0.5f;
            float e0 = (bx - px) * (cy - py) - (by - py) * (cx - px);   /* a's weight x area */
            float e1 = (cx - px) * (ay - py) - (cy - py) * (ax - px);
            float e2 = area - e0 - e1, z;
            int k = j * s_probe.w + i;
            if (area > 0.0f ? (e0 < 0.0f || e1 < 0.0f || e2 < 0.0f) : (e0 > 0.0f || e1 > 0.0f || e2 > 0.0f)) continue;
            z = (e0 * a->z + e1 * b->z + e2 * c->z) / area;
            if (cmp && z > s_probe.z[k]) continue;
            s_probe.id[k] = s_probe.epoch;
            if (upd) s_probe.z[k] = z;
        }
}

static void emit_polyp(const Vtx* const* poly, int n, const Vtx* flat) {
    XgfxVtx pv[12];
    int i;
    if (s_nb + (uint32_t)(n - 2) > BATCH_TRIS) flush();
    for (i = 0; i < n; i++) to_out(poly[i], &pv[i], flat);
    if (s_overlay) overlay_full(pv, n);
    if (XGFX_PROBE && s_probe.on && R.cimg == s_probe.main)
        for (i = 1; i + 1 < n; i++) probe_tri(&pv[0], &pv[i], &pv[i + 1]);
    for (i = 1; i + 1 < n; i++) {
        XgfxVtx* d = &s_batch[s_nb * 3];
        d[0] = pv[0];
        d[1] = pv[i];
        d[2] = pv[i + 1];
        s_nb++;
    }
}

static void emit_poly(const Vtx* poly, int n, const Vtx* flat) {
    const Vtx* pp[12];
    int i;
    for (i = 0; i < n && i < 12; i++) pp[i] = &poly[i];
    emit_polyp(pp, n, flat);
}

static void lerp_vtx(Vtx* o, const Vtx* a, const Vtx* b, float t) {
    const float *pa = &a->x, *pb = &b->x;
    float* po = &o->x;
    unsigned i;
    for (i = 0; i < VTX_LERP_FLOATS; i++) po[i] = pa[i] + (pb[i] - pa[i]) * t;
    o->ostamp = 0;
}

/* Sutherland-Hodgman in clip space against near (z + w >= 0), the x/y
 * guard band and far (z <= w); few triangles need it */
#ifdef XGFX_DEBUG_NOCULL
#define CULLMASK 0u
#else
#define CULLMASK (F3D_G_CULL_FRONT | F3D_G_CULL_BACK)
#endif

#ifdef XGFX_DEBUG_NOCLIPCULL
#define CLIPCULLMASK 0u
#else
#define CLIPCULLMASK CULLMASK
#endif

static void clip_tri(Vtx* const v[3], const Vtx* flat) {
    const uint32_t cullmask = CLIPCULLMASK;
    Vtx bufA[12], bufB[12], *in = bufA, *out = bufB, *tmp;
    int n = 3, plane, i;
    for (i = 0; i < 3; i++) in[i] = *v[i];
    for (plane = 0; plane < 6 && n >= 3; plane++) {
        int m = 0;
        for (i = 0; i < n; i++) {
            Vtx *a = &in[i], *b = &in[(i + 1) % n];
            float da, db;
            switch (plane) {
                case 0: da = a->z + a->w; db = b->z + b->w; break;
                case 1: da = GUARD * a->w - a->x; db = GUARD * b->w - b->x; break;
                case 2: da = GUARD * a->w + a->x; db = GUARD * b->w + b->x; break;
                case 3: da = GUARD * a->w - a->y; db = GUARD * b->w - b->y; break;
                case 4: da = GUARD * a->w + a->y; db = GUARD * b->w + b->y; break;
                default:
                    da = XGFX_FAR_CLIP ? a->w - a->z : 1.0f;
                    db = XGFX_FAR_CLIP ? b->w - b->z : 1.0f;
                    break;
            }
            if (da >= 0.0f && m < 12) out[m++] = *a;
            if ((da >= 0.0f) != (db >= 0.0f) && m < 12) lerp_vtx(&out[m++], a, b, da / (da - db));
        }
        n = m;
        tmp = in;
        in = out;
        out = tmp;
    }
    if (n < 3) return;
    for (i = 0; i < n; i++)
        if (in[i].w < 1e-5f) in[i].w = 1e-5f, in[i].ostamp = 0;
    if (R.geom & cullmask) {   /* all w > 0 now: the NDC area sign is the true orientation */
        float area = 0.0f;
        for (i = 0; i < n; i++) {
            int j = (i + 1) % n;
            area += (in[i].x / in[i].w) * (in[j].y / in[j].w) - (in[j].x / in[j].w) * (in[i].y / in[i].w);
        }
        if ((R.geom & F3D_G_CULL_BACK) && -area >= 0.0f) return;
        if ((R.geom & F3D_G_CULL_FRONT) && -area <= 0.0f) return;
    }
    emit_poly(in, n, flat);
}

static uint32_t s_t_rej_far, s_t_rej_near, s_t_far, s_t_near;   /* rejected beyond far / near; clipped at far / near */
static uint32_t s_t_in, s_t_rej, s_t_cull, s_t_clip, s_t_shadow, s_vtx_loads, s_mtx_loads, s_rects, s_fills, s_zclears;

static void do_tri(int ia, int ib, int ic, int flag) {
    Vtx* v[3] = { &R.vc[ia], &R.vc[ib], &R.vc[ic] };
    const Vtx* flat = (R.geom & F3D_G_SHADING_SMOOTH) ? NULL : v[flag < 3 ? flag : 0];
    int needs_clip;
    if (R.cimg == R.zimg && R.cimg && !XGFX_SHADOW_VOL) return;
    if (s_shadow_pass2 && !XGFX_SHADOW_VOL) return;
    s_t_in++;
    if (v[0]->oc & v[1]->oc & v[2]->oc & (31 | 64)) {   /* all beyond one plane */
        uint32_t all = v[0]->oc & v[1]->oc & v[2]->oc;
        s_t_rej++;
        s_t_rej_far += (all & 64) != 0;
        s_t_rej_near += (all & 16) != 0;
        return;
    }
    s_t_far += ((v[0]->oc | v[1]->oc | v[2]->oc) & 64) != 0;
    s_t_near += ((v[0]->oc | v[1]->oc | v[2]->oc) & 16) != 0;
    needs_clip = ((v[0]->oc | v[1]->oc | v[2]->oc) & 32) != 0;
    if ((R.geom & CULLMASK) && !needs_clip) {
        /* screen-space orientation, as the RSP culls: every w is > 0 here. The
         * homogeneous 3x3 determinant (wings64-dc's) has the same sign in exact
         * arithmetic, but in float it cancelled to the wrong sign on large
         * near triangles (the sea under a low glider came out black). */
        float i0 = 1.0f / v[0]->w, i1 = 1.0f / v[1]->w, i2 = 1.0f / v[2]->w;
        float x0 = v[0]->x * i0, y0 = v[0]->y * i0, x1 = v[1]->x * i1, y1 = v[1]->y * i1;
        float x2 = v[2]->x * i2, y2 = v[2]->y * i2;
        float cross = -((x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0));
        if ((R.geom & F3D_G_CULL_BACK) && cross >= 0.0f) { s_t_cull++; return; }
        if ((R.geom & F3D_G_CULL_FRONT) && cross <= 0.0f) { s_t_cull++; return; }
    }
    if (needs_clip) s_t_clip++;
    if (s_st_epoch != s_epoch) draw_state();
    if (!needs_clip) {
        emit_polyp((const Vtx* const*)v, 3, flat);
        return;
    }
    clip_tri(v, flat);
}

/* ------------------------------------------------------------ shadow volumes
 *
 * PW64 draws vehicle shadows as z-buffer shadow volumes (kernel/graphics.c
 * uvGfxStateDrawDL): pass 1 draws the volume's back faces with the colour
 * image pointed at the z-buffer (cimg == zimg), writing "nearest" wherever
 * they are visible; pass 2 calls the same display list again and draws the
 * front faces as dark translucent shade, z-tested, so only terrain inside
 * the volume darkens. Here pass 1 marks the stencil where the back faces
 * pass the depth test, and pass 2 (the second call of that display list)
 * draws only where nothing was marked, clearing the mark as it goes: the
 * same pixels as the RDP's trick, without touching the depth buffer. */
static uint32_t s_shadow_dl;   /* the display list pass 1 called */
static int s_shadow_rec, s_shadow_depth;

/* ------------------------------------------------------------- rectangles */

static uint32_t cc_words_const(int d_rgb, int d_a, uint32_t* cc1) {
    /* (0 - 0) * 0 + D in both cycles: a = 15, b = 15, c = 31, d (3 bits); alpha a = b = c = 7 */
    uint32_t w0 = (15u << 20) | (31u << 15) | (7u << 12) | (7u << 9) | (15u << 5) | 31u;
    *cc1 = (15u << 28) | ((uint32_t)d_rgb << 15) | (7u << 12) | ((uint32_t)d_a << 9) | (15u << 24) | (7u << 21) |
           (7u << 18) | ((uint32_t)d_rgb << 6) | (7u << 3) | (uint32_t)d_a;
    return w0;
}

static void screen_quad(float x0, float y0, float x1, float y1, float s0, float t0, float s1, float t1) {
    Vtx q[4];
    XgfxVtx o[4];
    int i;
    if (s_sc_on) {
        float nx0 = x0 < s_sc[0] ? s_sc[0] : x0, nx1 = x1 > s_sc[2] ? s_sc[2] : x1;
        float ny0 = y0 < s_sc[1] ? s_sc[1] : y0, ny1 = y1 > s_sc[3] ? s_sc[3] : y1;
        if (nx0 >= nx1 || ny0 >= ny1) return;
        if (x1 > x0) { float k = (s1 - s0) / (x1 - x0); s0 += (nx0 - x0) * k; s1 -= (x1 - nx1) * k; }
        if (y1 > y0) { float k = (t1 - t0) / (y1 - y0); t0 += (ny0 - y0) * k; t1 -= (y1 - ny1) * k; }
        x0 = nx0; x1 = nx1; y0 = ny0; y1 = ny1;
    }
    memset(q, 0, sizeof q);
    q[0].s = s0; q[0].t = t0;
    q[1].s = s1; q[1].t = t0;
    q[2].s = s0; q[2].t = t1;
    q[3].s = s1; q[3].t = t1;
    for (i = 0; i < 4; i++) {
        float x = (i & 1) ? x1 : x0, y = (i & 2) ? y1 : y0;
        o[i].x = x * s_sx + s_ox + s_xoff;
        o[i].y = y * s_sy + s_oy;
        o[i].z = 0.0f;
        o[i].w = 1.0f;
        o[i].rgba[0] = o[i].rgba[1] = o[i].rgba[2] = o[i].rgba[3] = 0;   /* RDP rectangles have no shade */
        o[i].s0 = q[i].s * s_u_scale[0] - s_u_off[0];
        o[i].t0 = q[i].t * s_v_scale[0] - s_v_off[0];
        o[i].s1 = q[i].s * s_u_scale[1] - s_u_off[1];
        o[i].t1 = q[i].t * s_v_scale[1] - s_v_off[1];
        o[i].fog = 0.0f;
    }
    if (s_overlay) overlay_full(o, 4);
    if (s_nb + 2 > BATCH_TRIS) flush();
    {
        XgfxVtx* d = &s_batch[s_nb * 3];
        d[0] = o[0]; d[1] = o[1]; d[2] = o[2];
        d[3] = o[1]; d[4] = o[3]; d[5] = o[2];
        s_nb += 2;
    }
}

static void do_fillrect(uint32_t w0, uint32_t w1) {
    uint16_t lrx, lry, ulx, uly;
    s_fills++;
    int cycle = (R.omh >> F3D_G_MDSFT_CYCLETYPE) & 3;
    f3d_fillrect_decode(w0, w1, &lrx, &lry, &ulx, &uly);
    if (cycle >= 2) { lrx++; lry++; }   /* fill/copy include the lower-right edge */
    if (R.cimg != R.zimg && lry > 0 && (uint32_t)lry > s_cimg_maxy) s_cimg_maxy = (uint32_t)lry;
    if (R.cimg == R.zimg && R.cimg) {   /* z-buffer clear (Birdman64 gfx.md) */
        float x0 = ulx, y0 = uly, x1 = lrx, y1 = lry;
        s_zclears++;
        flush();
        if (s_trace) xhw_logf("[DRAW] z clear %d,%d-%d,%d fill %08x", ulx, uly, lrx, lry, (unsigned)R.fill);
        if (s_null_back) {
        } else if (x0 <= SUB_X0 && x1 >= SUB_X1 && y0 <= SUB_Y0 && y1 >= SUB_Y1) {   /* SUBSCREEN-sized: the whole picture */
            xgfx_clear((int32_t)s_wx0, (int32_t)s_cy, (int32_t)s_wx1, (int32_t)(s_cy + s_ch), 0, 0, 1, 1.0f);
        } else {
            xgfx_clear((int32_t)(x0 * s_sx + s_ox), (int32_t)(y0 * s_sy + s_oy), (int32_t)(x1 * s_sx + s_ox),
                       (int32_t)(y1 * s_sy + s_oy), 0, 0, 1, 1.0f);
        }
        DIRTY();
        return;
    }
    flush();
    memset(&s_st, 0, sizeof s_st);
    pixel_state(1);
    s_st.fog = 0;
    s_st.depth_bias = 0;
    s_textured = s_tex1 = 0;
    if (cycle == 3) {   /* G_CYC_FILL: the RGBA5551 fill colour, no blend */
        uint16_t c = (uint16_t)(R.fill & 0xFFFF);
        s_st.cc0 = cc_words_const(3 /* PRIM */, 3, &s_st.cc1);
        s_st.prim[0] = (uint8_t)(((c >> 11) & 31) * 255 / 31);
        s_st.prim[1] = (uint8_t)(((c >> 6) & 31) * 255 / 31);
        s_st.prim[2] = (uint8_t)(((c >> 1) & 31) * 255 / 31);
        if (XGFX_FILL32 && s_cimg_siz == 3) {   /* a 32-bit colour image: the fill colour is RGBA8888 */
            s_st.prim[0] = (uint8_t)(R.fill >> 24);
            s_st.prim[1] = (uint8_t)(R.fill >> 16);
            s_st.prim[2] = (uint8_t)(R.fill >> 8);
        }
        s_st.prim[3] = 255;
        s_st.blend = XGFX_BLEND_OFF;
        s_st.alpha_ref = 0;
    } else {
        s_st.cc0 = R.cc0 & 0x00FFFFFFu;
        s_st.cc1 = R.cc1;
        s_st.two_cycle = cycle == 1;
    }
    s_overlay = XGFX_OVERLAY_FULL && !s_title;
    scissor_x();
    if (s_overlay && ulx <= SUB_X0 && lrx >= SUB_X1 && uly <= SUB_Y0 && lry >= SUB_Y1) {
        ulx = uly = 0;
        lrx = (uint16_t)N64_W;
        lry = (uint16_t)N64_H;
    }
    if (s_trace) xhw_logf("[DRAW] fillrect %d,%d-%d,%d cyc %d cimg %08x zimg %08x", ulx, uly, lrx, lry, cycle,
                          (unsigned)R.cimg, (unsigned)R.zimg);
    screen_quad(ulx, uly, lrx, lry, 0, 0, 0, 0);
    flush();
    DIRTY();
}

/* A texrect drawn while the colour image is the depth buffer's memory is a
 * copy into RDRAM, done here on the CPU. Banjo-Kazooie's pause (its decomp,
 * code_8DC20.c func_80315084 -> func_80314BB0) copies the frame into its
 * depth buffer (0x8000E800, inside the boot segment: 32x32 LOADTILE and
 * texrect blocks), then every paused frame copies that memory back as its
 * background (func_80315110). The copy went to the screen (the texrect path
 * ignores the colour image) and RDRAM there kept the boot segment's ROM
 * bytes (the RDP's own depth clears are GPU clears here), so the paused
 * background was those bytes: the static on red. Only 1:1 RGBA16 copies from
 * a LOADTILE; anything else is drawn as before. Found by a subagent from the
 * decomp. -DXGFX_ZIMG_TEXRECT=0: drawn on the screen, as before */
#ifndef XGFX_ZIMG_TEXRECT
#define XGFX_ZIMG_TEXRECT 1
#endif
static uint32_t s_zcimg_w;   /* SETCIMG's width while the colour image is the Z image */

/* the texrect into the Z image's RDRAM (XGFX_ZIMG_TEXRECT); 1 when done */
static int zimg_texrect(uint16_t xl, uint16_t yl, uint16_t xh, uint16_t yh, uint8_t tile, int16_t s, int16_t t,
                        int16_t dsdx, int16_t dtdy, int cycle) {
    const Tile* tt = &R.tile[tile & 7];
    const TmemLoad* ld = &R.loads[(tt->tmem >> 3) & 63];
    int one = cycle == 2 ? 4 << 10 : 1 << 10;
    int x, y, x0 = xl >> 2, y0 = yl >> 2, x1 = (xh >> 2) + (cycle >= 2), y1 = (yh >> 2) + (cycle >= 2);
    if (!XGFX_ZIMG_TEXRECT || !R.cimg || R.cimg != R.zimg || !s_zcimg_w) return 0;
    if (!ld->valid || !ld->tile || tt->fmt != F3D_G_IM_FMT_RGBA || tt->siz != F3D_G_IM_SIZ_16b || dsdx != one || dtdy != 1 << 10)
        return 0;
    if (XHW_TEST_BUILD) {   /* the first few: what is copied where */
        static unsigned s_n;
        if (s_n++ < 4) xhw_logf("[GFX] texrect into the Z image: %08x from %08x, %dx%d (by the CPU)", (unsigned)R.cimg,
                               (unsigned)ld->addr, (int)(x1 - x0), (int)(y1 - y0));
    }
    if (fbfx_stale_count()) fb_touch(ld->addr, (ld->rows - 1u) * ld->stride + ld->row_bytes);   /* the frame into RDRAM first */
    if (cycle < 2 && XGFX_TEXRECT_SNAP) x1 = (xh + 3) >> 2, y1 = (yh + 3) >> 2;
    for (y = y0; y < y1; y++) {
        int ti = (t >> 5) + (y - y0) - (tt->ult >> 2);
        if ((float)y < s_scissor[1] || (float)y >= s_scissor[3] || ti < 0 || ti >= (int)ld->rows) continue;
        for (x = x0; x < x1; x++) {
            int si = (s >> 5) + (x - x0) - (tt->uls >> 2);
            if ((float)x < s_scissor[0] || (float)x >= s_scissor[2] || si < 0 || 2 * si >= (int)ld->row_bytes) continue;
            wr16(XU_K0(R.cimg + 2u * ((uint32_t)y * s_zcimg_w + (uint32_t)x)),
                 rd16(ld->addr + (uint32_t)ti * ld->stride + 2u * (uint32_t)si));
        }
    }
    return 1;
}

static void do_texrect(uint32_t w0, uint32_t w1) {
    uint16_t xl, yl, xh, yh;
    if (XGFX_PROBE && probe_copy(w0, w1)) return;
    s_rects++;
    uint8_t tile;
    int16_t s, t, dsdx, dtdy;
    int cycle = (R.omh >> F3D_G_MDSFT_CYCLETYPE) & 3;
    float x0, y0, x1, y1, fs, ft, fdx, fdy;
    f3d_texrect_decode(w0, w1, &xl, &yl, &xh, &yh, &tile);
    f3d_texrect_half1_decode(R.half1, &s, &t);
    f3d_texrect_half2_decode(R.half2, &dsdx, &dtdy);
    if (zimg_texrect(xl, yl, xh, yh, tile, s, t, dsdx, dtdy, cycle)) return;   /* into RDRAM, not the screen */
    x0 = xl * 0.25f; y0 = yl * 0.25f; x1 = xh * 0.25f; y1 = yh * 0.25f;
    if (cycle >= 2) { x1 += 1.0f; y1 += 1.0f; }
    /* the RDP draws a pixel row (column) in 1/2-cycle mode when any of its
     * subscanlines (subpixels) is inside the rectangle: (y << 2) to
     * ((y + 1) << 2) - 1 fills one whole row, which upscaled would leave a
     * quarter-pixel gap a row (GoldenEye's 440x1 background lines) */
    if (XGFX_TEXRECT_SNAP && cycle < 2) { x1 = ceilf(x1); y1 = ceilf(y1); }
    fs = s / 32.0f;
    ft = t / 32.0f;
    fdx = dsdx / 1024.0f;
    fdy = dtdy / 1024.0f;
    if (cycle == 2) fdx *= 0.25f;   /* copy mode: dsdx is x4 */
    flush();
    if (s_trace) {
        const Tile* tt = &R.tile[tile & 7];
        const TmemLoad* ld = &R.loads[(tt->tmem >> 3) & 63];
        xhw_logf("[DRAW] texrect %d,%d-%d,%d tile %u s %d t %d (1/32) dsdx %d dtdy %d | tile fmt %u siz %u line %u "
                 "tmem %u sl %u tl %u sh %u th %u mask %u/%u | load %u addr %08x bytes %u dxt %u", (int)x0, (int)y0,
                 (int)x1, (int)y1, (unsigned)tile, s, t, dsdx, dtdy, tt->fmt, tt->siz, tt->line, tt->tmem, tt->uls >> 2,
                 tt->ult >> 2, tt->lrs >> 2, tt->lrt >> 2, tt->masks, tt->maskt, ld->valid, (unsigned)ld->addr,
                 (unsigned)ld->bytes, ld->dxt);
    }
    memset(&s_st, 0, sizeof s_st);
    pixel_state(1);
    s_st.fog = 0;
    s_st.depth_bias = 0;
    if (cycle == 2) {   /* copy: the texel as it is */
        s_st.cc0 = cc_words_const(1 /* TEXEL0 */, 1, &s_st.cc1);
        s_st.blend = XGFX_BLEND_OFF;
    } else {
        s_st.cc0 = R.cc0 & 0x00FFFFFFu;
        s_st.cc1 = R.cc1;
        s_st.two_cycle = cycle == 1;
    }
    bind_unit(0, tile);
    s_textured = s_st.tex[0] != 0;
    s_tex1 = 0;
    if (s_st.two_cycle && cc_uses_texel1()) {
        bind_unit(1, tile + 1);
        s_tex1 = s_st.tex[1] != 0;
    }
    if (s_textured && (cycle == 2 || (x1 - x0) * fdx < 64.0f)) s_st.filter[0] = 0;   /* 1:1 blits (gfx.md) */
    s_overlay = XGFX_OVERLAY_FULL && !s_textured;
    scissor_x();
    screen_quad(x0, y0, x1, y1, fs, ft, fs + (x1 - x0) * fdx, ft + (y1 - y0) * fdy);
    flush();
    DIRTY();
}

/* ---------------------------------------------- raw RDP triangles
 *
 * Fast3D passes each G_RDPHALF_1 + G_RDPHALF_CONT pair to the RDP as one
 * 64-bit command word. GoldenEye draws its sky's cloud layer that way: RDP
 * triangle commands (0xC8-0xCF) with their edge, shade, texture and z
 * coefficients worked out on the CPU (sky.c skyRenderTri; its last pair
 * ends in G_RDPHALF_2). They were
 * ignored, so the sky showed whatever was under it (the purple Dam). The
 * edge walker's description is turned back into its two trapezoids: the
 * major edge H from YH to YL against the high minor edge (XM) to YM and the
 * low one (XL) from YM (GoldenEye's are rectangles: XM apart from XH, YM =
 * YL). An attribute at (x, y) is its value at the major edge on the first
 * scanline plus DaDe a scanline down that edge plus DaDx across.
 * -DXGFX_RDP_TRI=0: ignored, as before */
static uint64_t s_rdp_w[22];
static unsigned s_rdp_n, s_rdp_need;

static float rdp_sext14(uint64_t v) {
    int32_t x = (int32_t)(v & 0x3FFFu);
    if (x & 0x2000) x -= 0x4000;
    return (float)x * 0.25f;
}
/* component k (0..3) of a coefficient block: integer halves in ints, fractions in fracs */
static float rdp_fix(uint64_t ints, uint64_t fracs, int k) {
    int16_t i = (int16_t)(ints >> (48 - 16 * k));
    uint16_t f = (uint16_t)(fracs >> (48 - 16 * k));
    return (float)i + (float)f / 65536.0f;
}

/* one corner: position and its attributes from the coefficients */
static void rdp_vtx(XgfxVtx* o, float x, float y, float ys, float xh, float dxh, const uint64_t* sh, const uint64_t* tx,
                    const uint64_t* zz, int persp) {
    float dy = y - ys, dx = x - (xh + dxh * dy);
    unsigned c;
    memset(o, 0, sizeof *o);
    o->x = x * s_sx + s_ox + s_xoff;
    o->y = y * s_sy + s_oy;
    o->w = 1.0f;
    if (sh)
        for (c = 0; c < 4; c++) {
            float v = rdp_fix(sh[0], sh[2], (int)c) + rdp_fix(sh[4], sh[6], (int)c) * dy + rdp_fix(sh[1], sh[3], (int)c) * dx;
            o->rgba[c] = (uint8_t)(v < 0.0f ? 0.0f : v > 255.0f ? 255.0f : v);
        }
    if (tx) {
        float ts = rdp_fix(tx[0], tx[2], 0) + rdp_fix(tx[4], tx[6], 0) * dy + rdp_fix(tx[1], tx[3], 0) * dx;
        float tt = rdp_fix(tx[0], tx[2], 1) + rdp_fix(tx[4], tx[6], 1) * dy + rdp_fix(tx[1], tx[3], 1) * dx;
        float tw = rdp_fix(tx[0], tx[2], 2) + rdp_fix(tx[4], tx[6], 2) * dy + rdp_fix(tx[1], tx[3], 2) * dx;
        if (persp && tw > 0.0f) {   /* S/W and T/W (W 1.0 = 0x8000); the hardware divides per pixel */
            ts = ts / tw * 32768.0f;
            tt = tt / tw * 32768.0f;
            o->w = 32768.0f / tw;
        }
        ts /= 32.0f;   /* s10.5 -> texels */
        tt /= 32.0f;
        o->s0 = ts * s_u_scale[0] - s_u_off[0];
        o->t0 = tt * s_v_scale[0] - s_v_off[0];
        o->s1 = ts * s_u_scale[1] - s_u_off[1];
        o->t1 = tt * s_v_scale[1] - s_v_off[1];
    }
    if (zz) {
        float z = (float)(int32_t)(zz[0] >> 32) / 65536.0f + (float)(int32_t)(zz[1] >> 32) / 65536.0f * dy +
                  (float)(int32_t)zz[0] / 65536.0f * dx;
        z /= 32768.0f;
        o->z = (z < 0.0f ? 0.0f : z > 1.0f ? 1.0f : z) * s_out.zmax;
    }
}

static void rdp_tri(void) {
    const uint64_t* w = s_rdp_w;
    unsigned cmd = (unsigned)(w[0] >> 56) & 0x3Fu, tile = (unsigned)(w[0] >> 48) & 7u, k;
    int shade = (cmd & 4) != 0, tex = (cmd & 2) != 0, zb = (cmd & 1) != 0;
    int cycle = (R.omh >> F3D_G_MDSFT_CYCLETYPE) & 3, persp = (R.omh & F3D_G_TP_PERSP) != 0;
    const uint64_t* sh = w + 4;
    const uint64_t* tx = w + 4 + (shade ? 8 : 0);
    const uint64_t* zz = tx + (tex ? 8 : 0);
    float yl = rdp_sext14(w[0] >> 32), ym = rdp_sext14(w[0] >> 16), yh = rdp_sext14(w[0]);
    float xl = (float)(int32_t)(w[1] >> 32) / 65536.0f, dxl = (float)(int32_t)w[1] / 65536.0f;
    float xh = (float)(int32_t)(w[2] >> 32) / 65536.0f, dxh = (float)(int32_t)w[2] / 65536.0f;
    float xm = (float)(int32_t)(w[3] >> 32) / 65536.0f, dxm = (float)(int32_t)w[3] / 65536.0f;
    float ys = floorf(yh), px[8], py[8];
    unsigned nq = 0;
    if (!(yl > yh)) return;
    if (ym < yh) ym = yh;
    if (ym > yl) ym = yl;
    /* the upper part: the major edge and the high minor edge (XM), YH to YM */
    if (ym > yh) {
        px[0] = xh + dxh * (yh - ys); py[0] = yh;
        px[1] = xm + dxm * (yh - ys); py[1] = yh;
        px[2] = xm + dxm * (ym - ys); py[2] = ym;
        px[3] = xh + dxh * (ym - ys); py[3] = ym;
        nq = 1;
    }
    /* the lower part: the major edge and the low minor edge (XL from YM), YM to YL */
    if (yl > ym) {
        px[nq * 4 + 0] = xh + dxh * (ym - ys); py[nq * 4 + 0] = ym;
        px[nq * 4 + 1] = xl;                   py[nq * 4 + 1] = ym;
        px[nq * 4 + 2] = xl + dxl * (yl - ym); py[nq * 4 + 2] = yl;
        px[nq * 4 + 3] = xh + dxh * (yl - ys); py[nq * 4 + 3] = yl;
        nq++;
    }
    flush();
    memset(&s_st, 0, sizeof s_st);
    pixel_state(!zb);
    s_st.fog = 0;
    s_st.depth_bias = 0;
    s_st.cc0 = R.cc0 & 0x00FFFFFFu;
    s_st.cc1 = R.cc1;
    s_st.two_cycle = cycle == 1;
    s_textured = 0;
    s_tex1 = 0;
    if (tex) {
        bind_unit(0, (int)tile);
        s_textured = s_st.tex[0] != 0;
        if (s_st.two_cycle && cc_uses_texel1()) {
            bind_unit(1, (int)tile + 1);
            s_tex1 = s_st.tex[1] != 0;
        }
    }
    s_overlay = 0;
    if (s_trace)
        xhw_logf("[DRAW] rdp tri %02x tile %u: yh %d ym %d yl %d, xh %d xm %d xl %d", cmd | 0xC0u, tile, (int)yh, (int)ym,
                 (int)yl, (int)xh, (int)xm, (int)xl);
    for (k = 0; k < nq; k++) {
        XgfxVtx q[4];
        unsigned i;
        for (i = 0; i < 4; i++)
            rdp_vtx(&q[i], px[k * 4 + i], py[k * 4 + i], ys, xh, dxh, shade ? sh : NULL, tex ? tx : NULL, zb ? zz : NULL,
                    persp);
        if (s_nb + 2 > BATCH_TRIS) flush();
        {
            XgfxVtx* d = &s_batch[s_nb * 3];
            d[0] = q[0]; d[1] = q[1]; d[2] = q[2];
            d[3] = q[0]; d[4] = q[2]; d[5] = q[3];
            s_nb += 2;
        }
    }
    flush();
    DIRTY();
}

static void rdp_word(uint64_t w) {
    if (XHW_TEST_BUILD) {   /* test builds: the first raw words, to name what a game sends */
        static unsigned s_said;
        if (s_said++ < 24)
            xhw_logf("[GFX] raw RDP word %u: %08x %08x (word %u of %u)", s_said, (unsigned)(w >> 32), (unsigned)w,
                     s_rdp_n, s_rdp_need);
    }
    if (!s_rdp_n) {
        unsigned cmd = (unsigned)(w >> 56) & 0x3Fu;
        if (cmd < 0x08u || cmd > 0x0Fu) return;   /* another raw RDP command: not taken */
        s_rdp_need = 4u + (cmd & 4u ? 8u : 0u) + (cmd & 2u ? 8u : 0u) + (cmd & 1u ? 2u : 0u);
    }
    s_rdp_w[s_rdp_n++] = w;
    if (s_rdp_n == s_rdp_need) {
        s_rdp_n = 0;
        rdp_tri();
    }
}

/* ------------------------------------------------------------ scissor */

/* A scissor or viewport clearly smaller than the SUBSCREEN view (the photo
 * album's 80x59 thumbnails, its 200x150 viewer) is honoured; the game
 * resets the scissor to the full screen after setting a thumbnail's, and
 * the RSP clips to the viewport anyway, so a small viewport alone clips */
static void clip_rect_update(void) {
    float x0 = s_scissor[0], y0 = s_scissor[1], x1 = s_scissor[2], y1 = s_scissor[3];
    float kx = N64_W / 320.0f, ky = N64_H / 240.0f;
    int sc_small = x1 - x0 < 280.0f * kx || y1 - y0 < 190.0f * ky;
    int vp_small = R.vp_sx < 140.0f * kx || R.vp_sy < 95.0f * ky;
    if (!sc_small) { x0 = 0.0f; y0 = 0.0f; x1 = N64_W; y1 = N64_H; }
    if (vp_small) {
        if (R.vp_tx - R.vp_sx > x0) x0 = R.vp_tx - R.vp_sx;
        if (R.vp_tx + R.vp_sx < x1) x1 = R.vp_tx + R.vp_sx;
        if (R.vp_ty - R.vp_sy > y0) y0 = R.vp_ty - R.vp_sy;
        if (R.vp_ty + R.vp_sy < y1) y1 = R.vp_ty + R.vp_sy;
    }
    s_sc[0] = x0; s_sc[1] = y0; s_sc[2] = x1; s_sc[3] = y1;
    s_sc_on = XGFX_SCISSOR && (sc_small || vp_small);
    DIRTY();
}

/* ------------------------------------------------------------ interpreter */

static uint32_t s_unknown[256];

/* -------------------------------------------- the F3DEX family (ucode.h)
 *
 * One interpreter for every family: run_dl's switch is Fast3D's, and a
 * command of another GBI is either done in ex_cmd (what Fast3D has no
 * equal for: the 32-80 vertex buffer, TRI2, QUAD, MODIFYVTX, CULLDL,
 * BRANCH_Z, LOAD_UCODE, F3DEX2's geometry mode and MOVEWORD/MOVEMEM
 * layouts) or rewritten in place as the Fast3D command it equals (G_DL,
 * G_ENDDL, G_MTX, G_TEXTURE, the othermodes, the RDP halves) and left to
 * the switch, which keeps the display-list stack, the shadow passes, the
 * widescreen tags and the RDP. The state stays in Fast3D's terms (R.geom
 * in Fast3D's bits). F3DLX/F3DLP and the .NoN/.Rej variants differ from
 * their parent in clipping and precision only: the same commands. */
static int s_fam = GFX_UC_F3D;   /* s_uc->family */

#ifndef XGFX_DKR
#define XGFX_DKR 1
#endif
static int s_rare, s_dkr_on;   /* Rare's DKR microcode (below, run_dl) */
void f3d_set_ucode(const GfxUcode* uc) {
    int rare = XGFX_DKR && uc && !uc->found && uc->family == GFX_UC_F3D;
    s_uc = uc ? uc : &s_uc_f3d;
    s_fam = s_uc->family;
    s_vmax = s_uc->vtx_max > VC_MAX ? VC_MAX : s_uc->vtx_max < 16 ? 16 : s_uc->vtx_max;
    if (rare != s_rare) s_dkr_on = 0;
    s_rare = rare;
    if (s_rare && s_dkr_on) s_vmax = 32;
}

/* the RDP halves that follow a G_TEXRECT */
static int is_half(uint32_t op) {
    if (s_fam == GFX_UC_F3DEX2 || s_fam == GFX_UC_S2DEX2) return op == F3DEX2_G_RDPHALF_1 || op == F3DEX2_G_RDPHALF_2;
    return op >= 0xB2 && op <= 0xB4;
}

static void do_viewport(uint32_t m) {   /* Vp: s16 vscale[4], vtrans[4]; x, y with 2 fraction bits */
    R.vp_sx = (int16_t)rd16(m) * 0.25f; R.vp_sy = (int16_t)rd16(m + 2) * 0.25f;
    R.vp_tx = (int16_t)rd16(m + 8) * 0.25f; R.vp_ty = (int16_t)rd16(m + 10) * 0.25f;
    R.vp_sz = (int16_t)rd16(m + 4); R.vp_tz = (int16_t)rd16(m + 12);
    s_ostamp++;
    clip_rect_update();
}

static void do_light(unsigned l, uint32_t m) {   /* Light: u8 col[3], pad, colc[3], pad, s8 dir[3] */
    if (l > 8) return;
    R.light_col[l][0] = rd8(m); R.light_col[l][1] = rd8(m + 1); R.light_col[l][2] = rd8(m + 2);
    R.light_dir[l][0] = (int8_t)rd8(m + 8); R.light_dir[l][1] = (int8_t)rd8(m + 9);
    R.light_dir[l][2] = (int8_t)rd8(m + 10);
}

/* G_LINE3D (Fast3D, L3DEX, L3DEX2): a line between two loaded vertices,
 * drawn as a quad of two triangles across it in screen space, wd + 2 half
 * N64 pixels wide (wd 0: one pixel), both faces, in the slots past the
 * vertex cache. It was skipped (counted): Pokémon Snap's and Yoshi's Story's
 * line microcodes drew nothing (docs/roadmap.md B3).
 * -DXGFX_LINES=0: skipped, as before */
#ifndef XGFX_LINES
#define XGFX_LINES 1
#endif
static unsigned s_lines_drawn;
static void do_line(unsigned ia, unsigned ib, unsigned wd) {
    Vtx *a, *b, *q = &R.vc[VC_MAX];
    float ax, ay, bx, by, px, py, len, h, nx, ny;
    uint32_t geom = R.geom;
    int i;
    if (!XGFX_LINES || ia >= s_vmax || ib >= s_vmax) return;
    a = &R.vc[ia], b = &R.vc[ib];
    if (a->w <= 0.0f || b->w <= 0.0f) return;   /* (behind the eye: a clipped line is left out) */
    ax = a->x / a->w, ay = a->y / a->w, bx = b->x / b->w, by = b->y / b->w;
    px = (bx - ax) * R.vp_sx, py = (by - ay) * R.vp_sy;   /* N64 pixels */
    len = sqrtf(px * px + py * py);
    if (len < 1e-6f || R.vp_sx == 0.0f || R.vp_sy == 0.0f) return;
    h = 0.25f * (float)(wd + 2);   /* half the width, N64 pixels */
    nx = -py / len * h / R.vp_sx, ny = px / len * h / R.vp_sy;   /* across the line, in NDC */
    q[0] = *a, q[1] = *a, q[2] = *b, q[3] = *b;
    q[0].x += nx * a->w, q[0].y += ny * a->w, q[1].x -= nx * a->w, q[1].y -= ny * a->w;
    q[2].x += nx * b->w, q[2].y += ny * b->w, q[3].x -= nx * b->w, q[3].y -= ny * b->w;
    for (i = 0; i < 4; i++) q[i].ostamp = 0, q[i].oc = outcode(&q[i]);
    R.geom &= ~(uint32_t)(F3D_G_CULL_FRONT | F3D_G_CULL_BACK);
    do_tri(VC_MAX, VC_MAX + 1, VC_MAX + 2, 0);
    do_tri(VC_MAX + 1, VC_MAX + 3, VC_MAX + 2, 0);
    R.geom = geom;
    s_lines_drawn++;
}

static void ex_tri(unsigned a, unsigned b, unsigned c) {
    /* the flag rotates the indices: the flat-shade vertex comes first */
    if (a < s_vmax && b < s_vmax && c < s_vmax && !(s_uc->flags & GFX_UC_F_LINE)) do_tri((int)a, (int)b, (int)c, 0);
}

/* gSPModifyVertex: one field of a loaded vertex, in the RSP's screen terms */
static void do_modifyvtx(uint8_t where, unsigned i, uint32_t val) {
    Vtx* v;
    if (i >= s_vmax) return;
    v = &R.vc[i];
    v->ostamp = 0;
    switch (where) {
        case F3DEX_G_MWO_POINT_RGBA:
            v->r = (val >> 24) * (1.0f / 255.0f); v->g = ((val >> 16) & 0xFF) * (1.0f / 255.0f);
            v->b = ((val >> 8) & 0xFF) * (1.0f / 255.0f); v->a = (val & 0xFF) * (1.0f / 255.0f);
            break;
        case F3DEX_G_MWO_POINT_ST:   /* s10.5, already scaled by gSPTexture */
            v->s = (int16_t)(val >> 16) * (1.0f / 32.0f);
            v->t = (int16_t)val * (1.0f / 32.0f);
            break;
        case F3DEX_G_MWO_POINT_XYSCREEN:   /* s13.2 screen pixels, back through the viewport at this w */
            if (R.vp_sx != 0.0f && R.vp_sy != 0.0f) {
                v->x = ((int16_t)(val >> 16) * 0.25f - R.vp_tx) / R.vp_sx * v->w;
                v->y = (R.vp_ty - (int16_t)val * 0.25f) / R.vp_sy * v->w;
                v->oc = outcode(v);
            }
            break;
        case F3DEX_G_MWO_POINT_ZSCREEN:   /* 16.16 screen z */
            if (R.vp_sz != 0.0f) {
                v->z = ((float)(int32_t)val * (1.0f / 65536.0f) - R.vp_tz) / R.vp_sz * v->w;
                v->oc = outcode(v);
            }
            break;
    }
}

/* gSPCullDisplayList: the rest of the list is skipped when every vertex in
 * the range is outside the same plane */
static int cull_dl(unsigned vs, unsigned ve) {
    uint32_t all = 0xFFFFFFFFu;
    unsigned i;
    if (ve >= s_vmax) ve = s_vmax - 1;
    if (vs > ve) return 0;
    for (i = vs; i <= ve; i++) all &= R.vc[i].oc;
    return (all & (15u | 16u | 64u)) != 0;
}

/* gSPBranchLessZ: branch when the vertex's screen z (0..G_MAXZ, 16.16) is
 * at most zval */
static int branch_z(unsigned i, uint32_t zval) {
    const Vtx* v;
    if (i >= s_vmax) return 0;
    v = &R.vc[i];
    if (v->w <= 0.0f) return 0;
    return (v->z / v->w * R.vp_sz + R.vp_tz) * 65536.0f <= (float)zval;
}

static void load_ucode(uint32_t w0) { f3d_set_ucode(gfx_ucode_at(R.half1, (w0 & 0xFFFFu) + 1u)); }

/* S2DEX's own commands (backgrounds, objects): not drawn yet; they must not
 * reach Fast3D's switch, where their numbers mean MTX, VTX, MOVEMEM */
static int s2dex_own(uint32_t op) {
    if (s_fam == GFX_UC_S2DEX) return (op >= 0x01 && op <= 0x05) || (op >= 0xB0 && op <= 0xB2) || (op >= 0xC1 && op <= 0xC4);
    if (s_fam == GFX_UC_S2DEX2) return (op >= 0x01 && op <= 0x0B) || op == 0xDA || op == 0xDC;
    return 0;
}

/* ---------------------------------------------- S2DEX backgrounds
 *
 * G_BG_COPY and G_BG_1CYC (uObjBg / uObjScaleBg): a texture image, maybe
 * bigger than TMEM, drawn into a frame on the screen, 1:1 or scaled, the
 * image wrapping at its width and height. Drawn here in 64-texel chunks,
 * each one a LOADTILE rectangle of the image through the texture cache (a
 * stand-in tile 7 and load slot 0, restored after), each chunk's quad at
 * the screen span its texels cover. uObjBg: imageX u10.5, imageW u10.2,
 * frameX s10.2, frameW u10.2, imageY, imageH, frameY, frameH, imagePtr,
 * imageLoad, imageFmt, imageSiz, imagePal, imageFlip; uObjScaleBg then
 * scaleW and scaleH (u5.10). Not handled: imageFlip, the scaled BG's
 * imageYorig. -DXGFX_S2DEX_BG=0: skipped, as before */
#define BG_CHUNK 64u
typedef struct { float scr0, scr1, tex0; unsigned chunk; } BgSpan;

/* the spans of one axis: screen f0..f1 shows texels from t0 on at `scale`
 * a pixel, wrapping at `size`, in chunks of `cn` texels */
static unsigned bg_spans(BgSpan* out, unsigned max, float f0, float f1, float t0, float scale, unsigned size,
                         unsigned cn) {
    unsigned n = 0;
    float x = f0;
    if (!size || scale <= 0.0f) return 0;
    while (x < f1 - 0.001f && n < max) {
        float tex = fmodf(t0 + (x - f0) * scale, (float)size), end, xe;
        unsigned c;
        if (tex < 0.0f) tex += (float)size;
        c = (unsigned)(tex / (float)cn);
        end = (float)((c + 1u) * cn) < (float)size ? (float)((c + 1u) * cn) : (float)size;
        xe = x + (end - tex) / scale;
        if (xe > f1) xe = f1;
        if (xe <= x) xe = x + 1.0f;
        out[n].scr0 = x, out[n].scr1 = xe, out[n].tex0 = tex - (float)(c * cn), out[n].chunk = c;
        n++;
        x = xe;
    }
    return n;
}

static void s2dex_bg(uint32_t w1, int copy) {
    uint32_t a = seg(w1), ptr;
    unsigned img_w, img_h, fmt, siz, pal, ny, nx, iy, ix, cw_tex, ch_tex;
    float fx, fy, fw, fh, ix0, iy0, sx = 1.0f, sy = 1.0f;
    int cycle = (R.omh >> F3D_G_MDSFT_CYCLETYPE) & 3;
    BgSpan ys[48], xs[48];
    Tile keep_tile = R.tile[7];
    TmemLoad keep_load = R.loads[0];
    ix0 = rd16(a + 0) / 32.0f;
    img_w = rd16(a + 2) >> 2;
    fx = (int16_t)rd16(a + 4) / 4.0f;
    fw = rd16(a + 6) / 4.0f;
    iy0 = rd16(a + 8) / 32.0f;
    img_h = rd16(a + 10) >> 2;
    fy = (int16_t)rd16(a + 12) / 4.0f;
    fh = rd16(a + 14) / 4.0f;
    ptr = rd32(a + 16);
    fmt = (rd16(a + 22) >> 8) & 7;
    siz = rd16(a + 22) & 3;
    pal = rd16(a + 24) & 15;
    if (!copy) {
        sx = rd16(a + 28) / 1024.0f;
        sy = rd16(a + 30) / 1024.0f;
    }
    if (!img_w || !img_h || fw <= 0.0f || fh <= 0.0f || sx <= 0.0f || sy <= 0.0f) return;
    cw_tex = BG_CHUNK;
    ch_tex = (4096u / ((BG_CHUNK << siz) >> 1)) < BG_CHUNK ? 4096u / ((BG_CHUNK << siz) >> 1) : BG_CHUNK;
    ny = bg_spans(ys, 48, fy, fy + fh, iy0, sy, img_h, ch_tex);
    nx = bg_spans(xs, 48, fx, fx + fw, ix0, sx, img_w, cw_tex);
    flush();
    memset(&s_st, 0, sizeof s_st);
    pixel_state(1);
    s_st.fog = 0;
    s_st.depth_bias = 0;
    if (cycle >= 2) {
        s_st.cc0 = cc_words_const(1 /* TEXEL0 */, 1, &s_st.cc1);
        s_st.blend = cycle == 2 ? s_st.blend : XGFX_BLEND_OFF;
    } else {
        s_st.cc0 = R.cc0 & 0x00FFFFFFu;
        s_st.cc1 = R.cc1;
        s_st.two_cycle = cycle == 1;
    }
    s_tex1 = 0;
    s_overlay = 0;
    if (s_trace)
        xhw_logf("[DRAW] bg %s %08x %ux%u fmt %u siz %u: frame %d,%d %dx%d from %d,%d scale %d/%d (x1000), %u x %u spans",
                 copy ? "copy" : "1cyc", (unsigned)ptr, img_w, img_h, fmt, siz, (int)fx, (int)fy, (int)fw, (int)fh, (int)ix0,
                 (int)iy0, (int)(sx * 1000.0f), (int)(sy * 1000.0f), nx, ny);
    for (iy = 0; iy < ny; iy++)
        for (ix = 0; ix < nx; ix++) {
            unsigned tx = xs[ix].chunk * cw_tex, ty = ys[iy].chunk * ch_tex;
            unsigned cw = img_w - tx < cw_tex ? img_w - tx : cw_tex, ch = img_h - ty < ch_tex ? img_h - ty : ch_tex;
            unsigned row = (cw << siz) >> 1;
            Tile* t = &R.tile[7];
            TmemLoad* ld = &R.loads[0];
            memset(t, 0, sizeof *t);
            t->fmt = (uint8_t)fmt; t->siz = (uint8_t)siz; t->palette = (uint8_t)pal;
            t->line = (uint16_t)((row + 7u) >> 3); t->tmem = 0;
            t->cms = t->cmt = 2;
            t->lrs = (uint16_t)((cw - 1u) << 2); t->lrt = (uint16_t)((ch - 1u) << 2);
            memset(ld, 0, sizeof *ld);
            ld->addr = ptr + ((((uint32_t)ty * img_w + tx) << siz) >> 1);
            ld->row_bytes = (uint16_t)row;
            ld->stride = (uint16_t)((img_w << siz) >> 1);
            ld->rows = (uint16_t)ch;
            ld->bytes = (uint32_t)ch * t->line * 8u;
            ld->tile = 1;
            ld->valid = 1;
            flush();
            bind_unit(0, 7);
            s_textured = s_st.tex[0] != 0;
            if (cycle >= 2 || (sx == 1.0f && sy == 1.0f)) s_st.filter[0] = 0;
            screen_quad(xs[ix].scr0, ys[iy].scr0, xs[ix].scr1, ys[iy].scr1, xs[ix].tex0, ys[iy].tex0,
                        xs[ix].tex0 + (xs[ix].scr1 - xs[ix].scr0) * sx, ys[iy].tex0 + (ys[iy].scr1 - ys[iy].scr0) * sy);
        }
    flush();
    R.tile[7] = keep_tile;
    R.loads[0] = keep_load;
    DIRTY();
}

/* ---------------------------------------------- S2DEX objects
 *
 * uObjTxtr loads (a block, a tile or a TLUT) are replayed as the RDP
 * commands the microcode sends (SETTIMG, SETTILE 7, LOADBLOCK / LOADTILE /
 * LOADTLUT) through the interpreter itself; uObjSprite draws from TMEM at
 * imageAdrs with imageStride, as the render tile: OBJ_RECTANGLE unscaled by
 * the matrix, OBJ_RECTANGLE_R through the 2D sub-matrix (X, Y, BaseScale),
 * OBJ_SPRITE through the whole one (A B C D, X Y). uObjMtx: s32 A B C D
 * (s15.16), s16 X Y (s10.2), u16 BaseScaleX/Y (u5.10); uObjSprite: objX
 * s10.2, scaleW u5.10, imageW u10.5, objY, scaleH, imageH, imageStride
 * (64-bit words), imageAdrs (TMEM words), fmt, siz, pal, flags (flipS 0x01,
 * flipT 0x10). -DXGFX_S2DEX_OBJ=0: skipped, as before */
static void run_dl(const uint32_t* pc);
static float s_omtx[8] = { 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f };   /* A B C D X Y BaseScaleX BaseScaleY */
static uint32_t s_obj_rm;   /* G_OBJ_RENDERMODE */

static void obj_matrix(uint32_t a, unsigned len) {
    a = seg(a);
    if (len >= 24) {
        s_omtx[0] = (int32_t)rd32(a) / 65536.0f; s_omtx[1] = (int32_t)rd32(a + 4) / 65536.0f;
        s_omtx[2] = (int32_t)rd32(a + 8) / 65536.0f; s_omtx[3] = (int32_t)rd32(a + 12) / 65536.0f;
        a += 16;
    }
    s_omtx[4] = (int16_t)rd16(a) / 4.0f; s_omtx[5] = (int16_t)rd16(a + 2) / 4.0f;
    s_omtx[6] = rd16(a + 4) / 1024.0f; s_omtx[7] = rd16(a + 6) / 1024.0f;
    if (s_omtx[6] <= 0.0f) s_omtx[6] = 1.0f;
    if (s_omtx[7] <= 0.0f) s_omtx[7] = 1.0f;
}

static void obj_loadtxtr(uint32_t a) {
    uint32_t type, img, d[16];
    unsigned n = 0, tmem, p1, p2;
    a = seg(a);
    type = rd32(a); img = rd32(a + 4);
    tmem = rd16(a + 8); p1 = rd16(a + 10); p2 = rd16(a + 12);
    if (type == 0x00001033u) {          /* G_OBJLT_TXTRBLOCK: tsize, tline */
        d[n++] = 0xFD000000u | (1u << 19); d[n++] = img;                                   /* SETTIMG 8-bit, width 1 */
        d[n++] = 0xF5000000u | (1u << 19) | (tmem & 0x1FFu); d[n++] = 7u << 24;            /* SETTILE 7 */
        d[n++] = 0xF3000000u; d[n++] = (7u << 24) | ((((p1 + 1u) << 3) - 1u) & 0xFFFu) << 12 | (p2 & 0xFFFu);
    } else if (type == 0x00FC1034u) {   /* G_OBJLT_TXTRTILE: twidth, theight */
        unsigned w = (p1 + 1u) << 1;
        d[n++] = 0xFD000000u | (2u << 19) | ((w - 1u) & 0xFFFu); d[n++] = img;             /* SETTIMG 16-bit */
        d[n++] = 0xF5000000u | (2u << 19) | (((p1 + 1u) >> 2) & 0x1FFu) << 9 | (tmem & 0x1FFu); d[n++] = 7u << 24;
        d[n++] = 0xF4000000u; d[n++] = (7u << 24) | (((w - 1u) << 2) & 0xFFFu) << 12 | (p2 & 0xFFFu);
    } else if (type == 0x00000030u) {   /* G_OBJLT_TLUT: phead, pnum */
        d[n++] = 0xFD000000u | (2u << 19); d[n++] = img;
        d[n++] = 0xF5000000u | (2u << 19) | (p1 & 0x1FFu); d[n++] = 7u << 24;
        d[n++] = 0xF0000000u; d[n++] = (7u << 24) | ((p2 << 2) & 0xFFFu) << 12;
    } else {
        return;
    }
    d[n++] = (uint32_t)(s_fam == GFX_UC_S2DEX2 ? F3DEX2_G_ENDDL : F3D_G_ENDDL) << 24; d[n++] = 0;
    run_dl(d);
}

/* mode 0: OBJ_RECTANGLE, 1: OBJ_RECTANGLE_R (the sub-matrix), 2: OBJ_SPRITE (the whole matrix) */
static void obj_sprite(uint32_t a, int mode) {
    float sw, sh, iw, ih, x0, y0, x1, y1, s0 = 0.0f, t0 = 0.0f, s1, t1, px[4], py[4], ps[4], pt[4];
    unsigned flags, i;
    int cycle = (R.omh >> F3D_G_MDSFT_CYCLETYPE) & 3;
    Tile* t = &R.tile[0];
    XgfxVtx o[4];
    a = seg(a);
    sw = rd16(a + 2) / 1024.0f; iw = rd16(a + 4) / 32.0f;
    sh = rd16(a + 10) / 1024.0f; ih = rd16(a + 12) / 32.0f;
    if (sw <= 0.0f || sh <= 0.0f || iw < 1.0f || ih < 1.0f) return;
    x0 = (int16_t)rd16(a) / 4.0f; y0 = (int16_t)rd16(a + 8) / 4.0f;
    x1 = x0 + iw / sw; y1 = y0 + ih / sh;
    s1 = iw; t1 = ih;
    flags = rd16(a + 22) & 0xFFu;
    if (flags & 0x01u) { float k = s0; s0 = s1; s1 = k; }
    if (flags & 0x10u) { float k = t0; t0 = t1; t1 = k; }
    /* the render tile, as the microcode sets it */
    memset(t, 0, sizeof *t);
    t->fmt = (uint8_t)(rd16(a + 20) >> 8); t->siz = (uint8_t)(rd16(a + 20) & 3);
    t->palette = (uint8_t)((rd16(a + 22) >> 8) & 15);
    t->line = rd16(a + 16); t->tmem = rd16(a + 18);
    t->cms = t->cmt = (s_obj_rm & 0x01u) ? 0 : 2;   /* G_OBJRM_NOTXCLAMP */
    t->lrs = (uint16_t)(((unsigned)iw - 1u) << 2); t->lrt = (uint16_t)(((unsigned)ih - 1u) << 2);
    if (mode == 1) {
        x0 = x0 / s_omtx[6] + s_omtx[4]; x1 = x1 / s_omtx[6] + s_omtx[4];
        y0 = y0 / s_omtx[7] + s_omtx[5]; y1 = y1 / s_omtx[7] + s_omtx[5];
    }
    flush();
    memset(&s_st, 0, sizeof s_st);
    pixel_state(1);
    s_st.fog = 0;
    s_st.depth_bias = 0;
    if (cycle == 2) {
        s_st.cc0 = cc_words_const(1 /* TEXEL0 */, 1, &s_st.cc1);
    } else {
        s_st.cc0 = R.cc0 & 0x00FFFFFFu;
        s_st.cc1 = R.cc1;
        s_st.two_cycle = cycle == 1;
    }
    bind_unit(0, 0);
    s_textured = s_st.tex[0] != 0;
    s_tex1 = 0;
    s_overlay = 0;
    if (!(s_obj_rm & 0x08u)) s_st.filter[0] = 0;   /* G_OBJRM_BILERP, else point */
    if (XHW_TEST_BUILD) {   /* test builds: the first objects drawn */
        static unsigned s_said;
        if (s_said++ < 4)
            xhw_logf("[GFX] S2DEX object %s at %d,%d %dx%d (fmt %u siz %u, TMEM %u)", mode == 2 ? "sprite" : "rectangle",
                     (int)x0, (int)y0, (int)(x1 - x0), (int)(y1 - y0), t->fmt, t->siz, t->tmem);
    }
    if (s_trace)
        xhw_logf("[DRAW] obj %s %d,%d-%d,%d tmem %u line %u fmt %u siz %u", mode == 2 ? "sprite" : "rect", (int)x0, (int)y0,
                 (int)x1, (int)y1, t->tmem, t->line, t->fmt, t->siz);
    if (mode != 2) {
        screen_quad(x0, y0, x1, y1, s0, t0, s1, t1);
        flush();
        DIRTY();
        return;
    }
    px[0] = x0; py[0] = y0; ps[0] = s0; pt[0] = t0;
    px[1] = x1; py[1] = y0; ps[1] = s1; pt[1] = t0;
    px[2] = x0; py[2] = y1; ps[2] = s0; pt[2] = t1;
    px[3] = x1; py[3] = y1; ps[3] = s1; pt[3] = t1;
    for (i = 0; i < 4; i++) {
        float X = s_omtx[0] * px[i] + s_omtx[1] * py[i] + s_omtx[4], Y = s_omtx[2] * px[i] + s_omtx[3] * py[i] + s_omtx[5];
        memset(&o[i], 0, sizeof o[i]);
        o[i].x = X * s_sx + s_ox + s_xoff;
        o[i].y = Y * s_sy + s_oy;
        o[i].w = 1.0f;
        o[i].s0 = ps[i] * s_u_scale[0] - s_u_off[0];
        o[i].t0 = pt[i] * s_v_scale[0] - s_v_off[0];
    }
    if (s_nb + 2 > BATCH_TRIS) flush();
    {
        XgfxVtx* d = &s_batch[s_nb * 3];
        d[0] = o[0]; d[1] = o[1]; d[2] = o[2];
        d[3] = o[1]; d[4] = o[3]; d[5] = o[2];
        s_nb += 2;
    }
    flush();
    DIRTY();
}

static int ex_cmd(uint32_t* pw0, uint32_t* pw1) {
    uint32_t w0 = *pw0, w1 = *pw1, op = w0 >> 24, n, v0;
    uint8_t a, b, c, where, q[4];
    uint16_t vi;
    if (XGFX_S2DEX_OBJ && (s_fam == GFX_UC_S2DEX || s_fam == GFX_UC_S2DEX2)) {
        int f2 = s_fam == GFX_UC_S2DEX2;
        if (op == (f2 ? 0x05u : 0xC1u)) { obj_loadtxtr(w1); return 1; }                           /* OBJ_LOADTXTR */
        if (op == (f2 ? 0x06u : 0xC2u)) { obj_loadtxtr(w1); obj_sprite(w1 + 24u, 2); return 1; }   /* LDTX_SPRITE */
        if (op == (f2 ? 0x07u : 0xC3u)) { obj_loadtxtr(w1); obj_sprite(w1 + 24u, 0); return 1; }   /* LDTX_RECT */
        if (op == (f2 ? 0x08u : 0xC4u)) { obj_loadtxtr(w1); obj_sprite(w1 + 24u, 1); return 1; }   /* LDTX_RECT_R */
        if (op == (f2 ? 0x01u : 0x03u)) { obj_sprite(w1, 0); return 1; }                           /* OBJ_RECTANGLE */
        if (op == (f2 ? 0x02u : 0x04u)) { obj_sprite(w1, 2); return 1; }                           /* OBJ_SPRITE */
        if (op == (f2 ? 0xDAu : 0xB2u)) { obj_sprite(w1, 1); return 1; }                           /* OBJ_RECTANGLE_R */
        if (op == (f2 ? 0x0Bu : 0xB1u)) { s_obj_rm = w1; return 1; }                               /* OBJ_RENDERMODE */
        if (!f2 && op == 0x05u) { obj_matrix(w1, w0 & 0xFFFFu); return 1; }                        /* OBJ_MOVEMEM */
        if (f2 && op == 0xDCu && (w0 & 0xFFu) == 14u) {   /* G_MOVEMEM G_MV_MATRIX: the obj matrix (24) or sub-matrix (8) */
            obj_matrix(w1, ((w0 >> 19) & 0x1Fu) * 8u + 8u);
            return 1;
        }
    }
    if (s2dex_own(op)) {
        int fam2 = s_fam == GFX_UC_S2DEX2;
        if (XGFX_S2DEX_BG && op == (fam2 ? 0x0Au : 0x02u)) { s2dex_bg(w1, 1); return 1; }   /* G_BG_COPY */
        if (XGFX_S2DEX_BG && op == (fam2 ? 0x09u : 0x01u)) { s2dex_bg(w1, 0); return 1; }   /* G_BG_1CYC */
        s_unknown[op]++;
        return 1;
    }
    if (s_fam == GFX_UC_F3DEX || s_fam == GFX_UC_S2DEX) {
        int old = (s_uc->flags & GFX_UC_F_OLD) != 0;
        switch (op) {
            case F3DEX_G_VTX:
                f3dex_vtx_decode(w0, &n, &v0);
                s_vtx_loads++;
                do_vtx(n, v0, w1);
                return 1;
            case F3DEX_G_TRI1: f3dex_tri_decode(w1, &a, &b, &c); ex_tri(a, b, c); return 1;
            case F3DEX_G_TRI2:
                f3dex_tri_decode(w0, &a, &b, &c); ex_tri(a, b, c);
                f3dex_tri_decode(w1, &a, &b, &c); ex_tri(a, b, c);
                return 1;
            case F3DEX_G_QUAD:   /* F3D_OLD's; the line microcodes' G_LINE3D otherwise (vertex index * 2) */
                if (old) {
                    f3dex_old_quad_decode(w1, q);
                    ex_tri(q[0], q[1], q[2]);
                    ex_tri(q[0], q[2], q[3]);
                } else if (s_uc->flags & GFX_UC_F_LINE) {
                    do_line(((w1 >> 16) & 0xFFu) / 2u, ((w1 >> 8) & 0xFFu) / 2u, w1 & 0xFFu);
                }
                return 1;
            case F3DEX_G_MODIFYVTX:
                if (old) return 0;   /* G_RDPHALF_CONT */
                f3dex_modifyvtx_decode(w0, &where, &vi);
                do_modifyvtx(where, vi, w1);
                return 1;
            case F3DEX_G_CULLDL:
                f3dex_culldl_decode(w0, w1, old, &a, &b);
                if (!cull_dl(a, b)) return 1;
                *pw0 = (uint32_t)F3D_G_ENDDL << 24;
                return 0;
            case F3DEX_G_BRANCH_Z:
                if (!branch_z(f3dex_branchz_vtx(w0), w1)) return 1;
                *pw0 = (uint32_t)F3D_G_DL << 24 | (uint32_t)F3D_G_DL_NOPUSH << 16;
                *pw1 = R.half1;
                return 0;
            case F3DEX_G_LOAD_UCODE: load_ucode(w0); return 1;
        }
        return 0;
    }
    /* F3DEX2, S2DEX2 */
    switch (op) {
        case F3DEX2_G_NOOP: case F3DEX2_G_SPNOOP: case F3DEX2_G_DMA_IO: case F3DEX2_G_SPECIAL_1:
        case F3DEX2_G_SPECIAL_2: case F3DEX2_G_SPECIAL_3:
            return 1;
        case F3DEX2_G_LINE3D:   /* L3DEX2: the vertices (index * 2) and the width in w0 */
            do_line(((w0 >> 16) & 0xFFu) / 2u, ((w0 >> 8) & 0xFFu) / 2u, w0 & 0xFFu);
            return 1;
        case F3DEX2_G_VTX:
            f3dex2_vtx_decode(w0, &n, &v0);
            s_vtx_loads++;
            do_vtx(n, v0, w1);
            return 1;
        case F3DEX2_G_MODIFYVTX:
            f3dex_modifyvtx_decode(w0, &where, &vi);
            do_modifyvtx(where, vi, w1);
            return 1;
        case F3DEX2_G_CULLDL:
            f3dex_culldl_decode(w0, w1, 0, &a, &b);
            if (!cull_dl(a, b)) return 1;
            *pw0 = (uint32_t)F3D_G_ENDDL << 24;
            return 0;
        case F3DEX2_G_BRANCH_Z:
            if (!branch_z(f3dex_branchz_vtx(w0), w1)) return 1;
            *pw0 = (uint32_t)F3D_G_DL << 24 | (uint32_t)F3D_G_DL_NOPUSH << 16;
            *pw1 = R.half1;
            return 0;
        case F3DEX2_G_TRI1: f3dex_tri_decode(w0, &a, &b, &c); ex_tri(a, b, c); return 1;
        case F3DEX2_G_TRI2:
        case F3DEX2_G_QUAD:
            f3dex_tri_decode(w0, &a, &b, &c); ex_tri(a, b, c);
            f3dex_tri_decode(w1, &a, &b, &c); ex_tri(a, b, c);
            return 1;
        case F3DEX2_G_TEXTURE:
            *pw0 = (uint32_t)F3D_G_TEXTURE << 24 | (w0 & 0x3F00u) | f3dex2_texture_on(w0);
            return 0;
        case F3DEX2_G_POPMTX:
            for (n = f3dex2_popmtx_count(w1); n && R.mv_top > 0; n--) R.mv_top--;
            R.mvp_dirty = 1;
            return 1;
        case F3DEX2_G_GEOMETRYMODE:
            R.geom2 = f3dex2_geom_apply(R.geom2, w0, w1);
            R.geom = f3dex2_geom_to_f3d(R.geom2);
            DIRTY();
            return 1;
        case F3DEX2_G_MTX:
            *pw0 = (uint32_t)F3D_G_MTX << 24 | (uint32_t)f3dex2_mtx_params(w0) << 16;
            return 0;
        case F3DEX2_G_MOVEWORD: {
            uint8_t idx, l;
            uint16_t off;
            f3dex2_moveword_decode(w0, &idx, &off);
            if (idx == F3D_G_MW_SEGMENT) set_segment((off >> 2) & 15, w1);
            else if (idx == F3D_G_MW_NUMLIGHT) R.num_lights = f3dex2_numlights_from_data(w1);
            else if (idx == F3D_G_MW_FOG) f3d_fog_from_data(w1, &R.fog_m, &R.fog_o);
            else if (idx == F3D_G_MW_LIGHTCOL && (l = f3dex2_lightcol_index(off)) < 9) {
                R.light_col[l][0] = w1 >> 24; R.light_col[l][1] = w1 >> 16; R.light_col[l][2] = w1 >> 8;
            }
            return 1;
        }
        case F3DEX2_G_MOVEMEM: {
            uint8_t idx;
            uint32_t ofs, len, m = seg(w1);
            f3dex2_movemem_decode(w0, &idx, &ofs, &len);
            if (idx == F3DEX2_G_MV_VIEWPORT) do_viewport(m);
            else if (idx == F3DEX2_G_MV_LIGHT) do_light(f3dex2_light_index(ofs), m);
            else s_unknown[op]++;   /* the forced matrix, points: not used by the games so far */
            return 1;
        }
        case F3DEX2_G_LOAD_UCODE: load_ucode(w0); return 1;
        case F3DEX2_G_DL: *pw0 = (uint32_t)F3D_G_DL << 24 | (w0 & 0x00FF0000u); return 0;
        case F3DEX2_G_ENDDL: *pw0 = (uint32_t)F3D_G_ENDDL << 24; return 0;
        case F3DEX2_G_RDPHALF_1: *pw0 = (uint32_t)F3D_G_RDPHALF_1 << 24; return 0;
        case F3DEX2_G_RDPHALF_2: *pw0 = (uint32_t)F3D_G_RDPHALF_2 << 24; return 0;
        case F3DEX2_G_SETOTHERMODE_L: *pw0 = (uint32_t)F3D_G_SETOTHERMODE_L << 24 | f3dex2_othermode_to_f3d(w0); return 0;
        case F3DEX2_G_SETOTHERMODE_H: *pw0 = (uint32_t)F3D_G_SETOTHERMODE_H << 24 | f3dex2_othermode_to_f3d(w0); return 0;
    }
    if (op < 0xC0) {   /* no F3DEX2 command: Fast3D's numbers here would mean something else */
        s_unknown[op]++;
        return 1;
    }
    return 0;   /* the RDP's */
}

#ifndef XGFX_RARE_DLINMEM
#define XGFX_RARE_DLINMEM 1
#endif
static int s_dlinmem_depth;   /* Rare's 0x07 inside one (run_dl) */

/* ---------------------------------------------------- Rare's DKR microcode
 *
 * Diddy Kong Racing's microcode (and Jet Force Gemini's, a later one; both
 * strip the version text) changes Fast3D's geometry: the encodings are the
 * DKR decomp's include/f3ddkr.h (CC0), read as the key. Matrices are whole
 * MVPs the CPU made (mtxf_to_mtx: Nintendo's integer/fraction layout) loaded
 * into one of 4 slots and selected (G_MTX: the slot in w0 bits 22-23, or JFG's
 * in bits 16-19; G_MOVEWORD 0x0A selects one); vertices are 10 bytes (s16 x,
 * y, z, u8 r, g, b, a) transformed by the selected slot alone, written from 0
 * or appended (w0 bit 16), and with billboarding on (G_MOVEWORD 0x02) added
 * to vertex 0's clip position; G_TRIN (0x05) draws up to 16 triangles from
 * memory, 16 bytes each (flags: 0x40 draw back faces; three vertex indices;
 * u, v per vertex in 10.5), w0 bit 16 texturing them; JFG's 0xBF sets the
 * bases its vertex and triangle addresses are relative to. Turned on once a
 * microcode with no version text sends 0x05 or 0x07 (a Fast3D one never
 * does), so the first such frame's geometry before it reads as Fast3D.
 * DIVERGES: none meant. -DXGFX_DKR=0: Fast3D, as before (defined above f3d_set_ucode) */
static float s_dkr_m[4][4][4];     /* the MVP slots */
static int s_dkr_cur, s_dkr_bb;    /* the slot vertices use; billboarding */
static unsigned s_dkr_vn;          /* vertices loaded (for an append) */
static uint32_t s_dkr_voff, s_dkr_toff;   /* JFG's 0xBF bases */

static void dkr_turn_on(unsigned op) {
    s_dkr_on = 1;
    s_vmax = 32;
    xhw_logf("[GFX] the microcode with no version text sent %02X: Rare's DKR geometry from here on (XGFX_DKR)", op);
}

static void dkr_mtx(uint32_t w0, uint32_t w1) {
    float m[4][4];
    unsigned idx = (w0 >> 16) & 0xFu ? (w0 >> 16) & 3u : (w0 >> 22) & 3u;
    int mult = ((w0 >> 16) & 0xFu) && ((w0 >> 23) & 1u);
    mtx_from_n64(m, seg(w1));
    if (mult) mtx_mul(s_dkr_m[idx], m, s_dkr_m[0]);
    else memcpy(s_dkr_m[idx], m, sizeof m);
    s_dkr_cur = (int)idx;
    s_ostamp++;
}

static void dkr_vtx(uint32_t w0, uint32_t w1) {
    uint32_t m = (w0 >> 19) & 0x1Fu, len = w0 & 0xFFFFu, n, i, v0, a;
    const float(*M)[4] = s_dkr_m[s_dkr_cur];
    n = len == 10u * m + 8u ? m : m + 1u;   /* JFG's count, or DKR's (its length 18n + 8) */
    v0 = (w0 >> 16) & 1u ? s_dkr_vn : 0u;
    a = seg(w1) + s_dkr_voff;
    for (i = 0; i < n && v0 + i < s_vmax; i++, a += 10) {
        Vtx* v = &R.vc[v0 + i];
        float x = (float)(int16_t)rd16(a), y = (float)(int16_t)rd16(a + 2), z = (float)(int16_t)rd16(a + 4);
        v->ostamp = 0;
        v->x = x * M[0][0] + y * M[1][0] + z * M[2][0] + M[3][0];
        v->y = x * M[0][1] + y * M[1][1] + z * M[2][1] + M[3][1];
        v->z = x * M[0][2] + y * M[1][2] + z * M[2][2] + M[3][2];
        v->w = x * M[0][3] + y * M[1][3] + z * M[2][3] + M[3][3];
        if (s_dkr_bb && v0 + i > 0) {   /* billboard: on the anchor (vertex 0), its w */
            v->x += R.vc[0].x, v->y += R.vc[0].y, v->z += R.vc[0].z;
            v->w = R.vc[0].w;
        }
        v->oc = outcode(v);
        v->r = rd8(a + 6) * (1.0f / 255.0f);
        v->g = rd8(a + 7) * (1.0f / 255.0f);
        v->b = rd8(a + 8) * (1.0f / 255.0f);
        v->a = rd8(a + 9) * (1.0f / 255.0f);
        v->s = v->t = 0.0f;
        if ((R.geom & F3D_G_FOG) && v->w > 0.0f) {
            float f = v->z / v->w * R.fog_m + R.fog_o;
            v->fog = f < 0.0f ? 0.0f : f > 255.0f ? 255.0f : f;
        } else {
            v->fog = 0.0f;
        }
    }
    s_dkr_vn = v0 + i;
}

static void dkr_trin(uint32_t w0, uint32_t w1) {
    uint32_t n = (w0 & 0xFFFFu) / 16u, i, a = seg(w1) + s_dkr_toff, geom = R.geom;
    int tex = (int)((w0 >> 16) & 1u);
    if (!n) n = ((w0 >> 20) & 0xFu) + 1u;
    if (n > 16) n = 16;
    if (R.tex_on != tex || R.tex_tile != 0) {
        R.tex_on = (uint8_t)tex;
        R.tex_tile = 0;
        DIRTY();
    }
    for (i = 0; i < n; i++, a += 16) {
        unsigned flags = rd8(a), vi[3], k;
        vi[0] = rd8(a + 1), vi[1] = rd8(a + 2), vi[2] = rd8(a + 3);
        if (vi[0] >= s_dkr_vn || vi[1] >= s_dkr_vn || vi[2] >= s_dkr_vn) continue;
        for (k = 0; k < 3; k++) {
            R.vc[vi[k]].s = (float)(int16_t)rd16(a + 4 + 4 * k) * (1.0f / 32.0f);
            R.vc[vi[k]].t = (float)(int16_t)rd16(a + 6 + 4 * k) * (1.0f / 32.0f);
        }
        {   /* 0x40: both faces; else the back culled */
            uint32_t want = (geom & ~CULLMASK) | ((flags & 0x40u) ? 0u : F3D_G_CULL_BACK);
            if (R.geom != want) {
                R.geom = want;
                DIRTY();
            }
        }
        do_tri((int)vi[0], (int)vi[1], (int)vi[2], 0);
    }
    if (R.geom != geom) {
        R.geom = geom;
        DIRTY();
    }
}

/* A list that runs off RDRAM's end (no G_ENDDL, or a G_DL near the top)
 * read whatever followed the window; one that calls itself ran until the
 * watchdog (docs/review-2026-10.md 5c.3). The task ends there, logged once.
 * The budget counts every command of a task (a frame is tens of
 * thousands). -DXGFX_DL_BUDGET=0: no budget; -DXGFX_DL_BOUNDS=0: no end check */
#ifndef XGFX_DL_BUDGET
#define XGFX_DL_BUDGET 1000000u
#endif
#ifndef XGFX_DL_BOUNDS
#define XGFX_DL_BOUNDS 1
#endif
static uint32_t s_dl_cmds;
static void run_dl(const uint32_t* pc) {
    const uint32_t* stack[DL_DEPTH];
    int sp = 0;
    for (;;) {
        uint32_t w0, w1;
        if (XGFX_DL_BOUNDS) {   /* (synthesised lists live outside the window and end themselves) */
            uintptr_t o = (uintptr_t)((const uint8_t*)pc - xrc_rdram_host);
            if (o <= xrc_rdram_size && o + 8u > xrc_rdram_size) {
                static int s_said;
                if (!s_said++) xhw_logf("[GFX] display list ran off RDRAM's end: the task ends there");
                return;
            }
        }
        if (XGFX_DL_BUDGET && ++s_dl_cmds > XGFX_DL_BUDGET) {
            static int s_said;
            if (s_dl_cmds == XGFX_DL_BUDGET + 1u && s_said++ < 4)
                xhw_logf("[GFX] display list: %u commands in one task (a loop?): the task ends there", (unsigned)XGFX_DL_BUDGET);
            return;
        }
        w0 = pc[0], w1 = pc[1];
        if (XHW_TEST_BUILD) {   /* test builds: the last commands, for tex_miss's report */
            s_ring[s_ring_n & 15][0] = w0, s_ring[s_ring_n & 15][1] = w1;
            s_ring_n++;
        }
        pc += 2;
        if (s_fam != GFX_UC_F3D && ex_cmd(&w0, &w1)) continue;
        switch (w0 >> 24) {
            case F3D_G_TEXTURE: case F3D_G_SETOTHERMODE_H: case F3D_G_SETOTHERMODE_L: case F3D_G_SETGEOMETRYMODE:
            case F3D_G_CLEARGEOMETRYMODE: case F3D_G_SETTILE: case F3D_G_SETTILESIZE: case F3D_G_LOADBLOCK:
            case F3D_G_SETCOMBINE: case F3D_G_SETTIMG: case F3D_G_SETFOGCOLOR: case F3D_G_SETBLENDCOLOR:
            case F3D_G_RDPSETOTHERMODE:
                DIRTY();
                break;
        }
        switch (w0 >> 24) {
            case F3D_G_MTX:
                s_mtx_loads++;
                if (s_dkr_on) dkr_mtx(w0, w1);
                else do_mtx(w0, w1);
                break;
            case F3D_G_POPMTX:
                if (R.mv_top > 0) { R.mv_top--; R.mvp_dirty = 1; }
                break;
            case F3D_G_VTX: {
                uint32_t n, v0, len, addr;
                if (s_dkr_on) {
                    s_vtx_loads++;
                    dkr_vtx(w0, w1);
                    break;
                }
                f3d_vtx_decode(w0, w1, &n, &v0, &len, &addr);
                s_vtx_loads++;
                do_vtx(n, v0, addr);
                break;
            }
            case F3D_G_TRI1: {
                uint8_t a, b, c, f;
                if (s_dkr_on) {   /* JFG: the bases its vertex (w1) and triangle (w0) addresses are relative to */
                    s_dkr_toff = w0 & 0xFFFFFFu;
                    s_dkr_voff = w1 & 0xFFFFFFu;
                    break;
                }
                f3d_tri1_decode(w1, &a, &b, &c, &f);
                if (a < 16 && b < 16 && c < 16) do_tri(a, b, c, f);
                break;
            }
            case F3D_G_RESERVED1:   /* Rare's G_TRIN (DKR, JFG): triangles from memory */
                if (s_rare) {
                    if (!s_dkr_on) dkr_turn_on(0x05);
                    dkr_trin(w0, w1);
                } else {
                    s_unknown[F3D_G_RESERVED1]++;
                }
                break;
            case F3D_G_RESERVED2:
                if (s_rare && !s_dkr_on) dkr_turn_on(0x07);
                /* Rare's later Fast3D (Jet Force Gemini; its microcode has no
                 * version text): 0x07 runs n commands from memory and comes
                 * back, w0 = 07 nn llll (n, and their bytes), w1 their address.
                 * JFG puts each texture rectangle's SETTILE / LOADBLOCK /
                 * SETTILESIZE / RDPHALF words there: unread, its text and its
                 * menu backgrounds drew as grey boxes (tile 0 never set).
                 * Nintendo's Fast3D leaves 0x07 reserved.
                 * -DXGFX_RARE_DLINMEM=0: unknown, as before */
                if (XGFX_RARE_DLINMEM && s_fam == GFX_UC_F3D && s_dlinmem_depth < 2) {
                    uint32_t n = (w0 >> 16) & 0xFFu, i, a = seg(w1);
                    uint32_t buf[2 * 256 + 2];
                    if (!n) n = (w0 & 0xFFFFu) / 8u;
                    if (n > 256) n = 256;
                    for (i = 0; i < n; i++) buf[2 * i] = rd32(a + 8 * i), buf[2 * i + 1] = rd32(a + 8 * i + 4);
                    buf[2 * n] = (uint32_t)F3D_G_ENDDL << 24, buf[2 * n + 1] = 0;
                    s_dlinmem_depth++;
                    run_dl(buf);
                    s_dlinmem_depth--;
                } else {
                    s_unknown[F3D_G_RESERVED2]++;
                }
                break;
            case F3D_G_TRI4:   /* Rare's Fast3D (GoldenEye, Perfect Dark): 4-bit indices, x and y in w1, z in w0 */
                if (XGFX_TRI4 && s_fam == GFX_UC_F3D) {
                    int i;
                    for (i = 0; i < 4; i++) {
                        int x = (int)(w1 >> (8 * i)) & 15, y = (int)(w1 >> (8 * i + 4)) & 15, z = (int)(w0 >> (4 * i)) & 15;
                        if (x | y | z) do_tri(x, y, z, 0);   /* all three 0: no triangle */
                    }
                } else {
                    s_unknown[0xB1]++;
                }
                break;
            case F3D_G_DL: {
                uint8_t push;
                uint32_t addr;
                f3d_dl_decode(w0, w1, &push, &addr);
                if (R.cimg == R.zimg && R.cimg) {   /* shadow pass 1: the list pass 2 will call again */
                    if (!s_shadow_rec) { s_shadow_dl = addr; s_shadow_rec = 1; }
                } else if (s_shadow_dl && addr == s_shadow_dl && push && !s_shadow_pass2) {
                    /* pass 2: draw where pass 1 marked nothing, until this call returns */
                    s_shadow_dl = 0;
                    s_shadow_pass2 = 1;
                    s_shadow_depth = sp + 1;
                    DIRTY();
                }
                if (push) {
                    if (sp == DL_DEPTH) return;
                    stack[sp++] = pc;
                }
                pc = (const uint32_t*)host(addr);
                break;
            }
            case F3D_G_ENDDL:
                if (s_shadow_pass2 && sp == s_shadow_depth) {
                    s_shadow_pass2 = 0;
                    DIRTY();
                }
                if (sp == 0) return;
                pc = stack[--sp];
                break;
            case F3D_G_MOVEMEM: {
                uint8_t idx;
                uint32_t len, addr, m;
                f3d_movemem_decode(w0, w1, &idx, &len, &addr);
                m = seg(addr);
                if (idx == F3D_G_MV_VIEWPORT) do_viewport(m);
                else if (idx >= F3D_G_MV_L0 && idx <= F3D_G_MV_L7) do_light((idx - F3D_G_MV_L0) / 2, m);
                break;
            }
            case F3D_G_MOVEWORD: {
                uint8_t idx;
                uint16_t off;
                uint32_t data;
                f3d_moveword_decode(w0, w1, &idx, &off, &data);
                if (s_dkr_on && idx == 0x02) {   /* DKR: G_MW_BILLBOARD */
                    s_dkr_bb = (int)(data & 1u);
                    break;
                }
                if (s_dkr_on && idx == 0x0A) {   /* DKR: G_MW_MVPMATRIX, the slot << 6 */
                    s_dkr_cur = (int)((data >> 6) & 3u);
                    break;
                }
                if (idx == F3D_G_MW_SEGMENT) set_segment((off >> 2) & 15, data);
                else if (idx == F3D_G_MW_NUMLIGHT) R.num_lights = f3d_numlights_from_data(data);
                else if (idx == F3D_G_MW_FOG) f3d_fog_from_data(data, &R.fog_m, &R.fog_o);
                else if (idx == F3D_G_MW_LIGHTCOL) {
                    uint8_t l = f3d_lightcol_index(off);
                    if (l < 9) { R.light_col[l][0] = data >> 24; R.light_col[l][1] = data >> 16; R.light_col[l][2] = data >> 8; }
                }
                break;
            }
            case F3D_G_TEXTURE: {
                uint8_t lvl, tile, on;
                uint16_t s, t;
                f3d_texture_decode(w0, w1, &lvl, &tile, &on, &s, &t);
                R.tex_on = on;
                R.tex_tile = tile;
                R.tex_ss = s;
                R.tex_st = t;
                break;
            }
            case F3D_G_SETOTHERMODE_H: R.omh = f3d_othermode_apply(R.omh, w0, w1); break;
            case F3D_G_SETOTHERMODE_L: R.oml = f3d_othermode_apply(R.oml, w0, w1); break;
            case F3D_G_RDPSETOTHERMODE:   /* gDPSetOtherMode: both words at once (Ocarina of Time's render modes) */
                R.omh = w0 & 0x00FFFFFFu;
                R.oml = w1;
                break;
            case F3D_G_SETGEOMETRYMODE: R.geom |= w1; break;
            case F3D_G_CLEARGEOMETRYMODE: R.geom &= ~w1; break;
            case F3D_G_RDPHALF_1: R.half1 = w1; break;
            case F3D_G_RDPHALF_2:   /* also the last word of a raw RDP triangle (GoldenEye's sky) */
                R.half2 = w1;
                if (XGFX_RDP_TRI && s_rdp_n) rdp_word((uint64_t)R.half1 << 32 | w1);
                break;
            case F3D_G_RDPHALF_CONT:
                if (XGFX_RDP_TRI) rdp_word((uint64_t)R.half1 << 32 | w1);
                break;
            case F3D_G_TEXRECT:
            case F3D_G_TEXRECTFLIP:
                /* PW64's F3D emits E4, B3 (s,t), B2 (dsdx,dtdy); current gbi.h says E4, B4, B3:
                 * take the two halves that follow, whichever numbering (wings64-dc traps.md) */
                if (is_half(pc[0] >> 24) && is_half(pc[2] >> 24)) {
                    R.half1 = pc[1];
                    R.half2 = pc[3];
                    pc += 4;
                }
                do_texrect(w0, w1);
                break;
            case F3D_G_FILLRECT: do_fillrect(w0, w1); break;
            case F3D_G_SETTIMG: f3d_setimg_decode(w0, w1, &R.timg_fmt, &R.timg_siz, &R.timg_w, &R.timg_addr); break;
            case F3D_G_SETTILE: {
                struct f3d_settile s;
                Tile* t;
                f3d_settile_decode(w0, w1, &s);
                t = &R.tile[s.tile & 7];
                t->fmt = s.fmt; t->siz = s.siz; t->line = s.line; t->tmem = s.tmem;
                t->cms = s.cms; t->cmt = s.cmt; t->masks = s.masks; t->maskt = s.maskt;
                t->shifts = s.shifts; t->shiftt = s.shiftt; t->palette = s.palette;
                break;
            }
            case F3D_G_SETTILESIZE: {
                uint8_t tile;
                uint16_t uls, ult, lrs, lrt;
                f3d_tilesize_decode(w0, w1, &tile, &uls, &ult, &lrs, &lrt);
                tile &= 7;
                R.tile[tile].uls = uls; R.tile[tile].ult = ult; R.tile[tile].lrs = lrs; R.tile[tile].lrt = lrt;
                break;
            }
            case F3D_G_LOADBLOCK: {
                uint8_t tile;
                uint16_t uls, ult, lrs, dxt;
                TmemLoad* ld;
                unsigned bpt;
                f3d_loadblock_decode(w0, w1, &tile, &uls, &ult, &lrs, &dxt);
                ld = &R.loads[(R.tile[tile & 7].tmem >> 3) & 63];
                bpt = R.timg_siz == 3 ? 4 : R.timg_siz == 2 ? 2 : 1;   /* LOADBLOCK in SETTIMG's size */
                /* a 32-bit load tile under a 16-bit image: the texels are 32-bit (Mario
                 * Party's title logo, 188x5 RGBA32 strips 3760 bytes apart, came out as
                 * 1880-byte loads and two rows a strip) */
                if (XGFX_LOAD32 && R.timg_siz == 2 && R.tile[tile & 7].siz == 3) bpt = 4;
                ld->addr = R.timg_addr;
                ld->bytes = (lrs + 1u) * bpt;
                ld->dxt = dxt;
                ld->valid = 1;
                ld->tile = 0;
                ld->tmem = R.tile[tile & 7].tmem;
                ld->seq = ++s_load_seq;
                if (R.timg_siz == 0) ld->bytes = (lrs + 2u) / 2;
                break;
            }
            case F3D_G_LOADTILE: {
                uint8_t tile;
                uint16_t uls, ult, lrs, lrt;
                TmemLoad* ld;
                unsigned x0, y0, x1, y1, bpp2;   /* bpp2: bytes per texel x 2 */
                f3d_loadtile_decode(w0, w1, &tile, &uls, &ult, &lrs, &lrt);
                ld = &R.loads[(R.tile[tile & 7].tmem >> 3) & 63];
                ld->tmem = R.tile[tile & 7].tmem;
                ld->seq = ++s_load_seq;
                bpp2 = R.timg_siz == 3 ? 8 : R.timg_siz == 2 ? 4 : R.timg_siz == 1 ? 2 : 1;
                x0 = uls >> 2; y0 = ult >> 2; x1 = lrs >> 2; y1 = lrt >> 2;
                if (x1 < x0 || y1 < y0) break;
                ld->addr = R.timg_addr + (y0 * R.timg_w + x0) * bpp2 / 2;
                ld->row_bytes = (uint16_t)(((x1 - x0 + 1) * bpp2 + 1) / 2);
                ld->stride = (uint16_t)(R.timg_w * bpp2 / 2);
                ld->rows = (uint16_t)(y1 - y0 + 1);
                ld->bytes = ld->rows * R.tile[tile & 7].line * 8u;
                if (XGFX_PROBE) probe_load(seg(R.timg_addr), x0, y0, x1, y1, R.timg_siz);
                /* a 32-bit tile's line covers one of TMEM's two banks: a row is
                 * twice it (Mario Party's title logo, 188x5 RGBA32 strips by
                 * LOADTILE, came out 2 rows tall and stretched). -DXGFX_LOADTILE32=0: as before */
                if (XGFX_LOADTILE32 && R.tile[tile & 7].siz == 3) ld->bytes *= 2u;
                ld->dxt = 0;
                ld->tile = 1;
                ld->valid = 1;
                break;
            }
            case F3D_G_LOADTLUT: {   /* the palette at TMEM word 256 + n (CI4: 16 a palette) */
                uint8_t tile;
                uint16_t count;
                unsigned at, i;
                uint32_t src = seg(R.timg_addr);
                f3d_loadtlut_decode(w1, &tile, &count);
                at = R.tile[tile & 7].tmem;
                if (at < 256) break;
                /* SL and TL place the palette in the image, and SH - SL counts it:
                 * GoldenEye keeps each palette right after its texels and loads it
                 * with SL = the texel count (gDPLoadTLUT07); reading from the image's
                 * start gave its CI textures their own texels as colours (the purple
                 * sky layer, the green blocks). -DXGFX_TLUT_OFFSET=0: as before */
                if (XGFX_TLUT_OFFSET) {
                    unsigned sl = (w0 >> 14) & 0x3FFu, tl = (w0 >> 2) & 0x3FFu;
                    src += (((uint32_t)tl * R.timg_w + sl) << R.timg_siz) >> 1;
                    count = (uint16_t)(count >= sl ? count - sl : 0);
                }
                for (at -= 256, i = 0; i <= count && at + i < 256; i++) R.tlut[at + i] = rd16(src + 2 * i);
                s_tlut_gen++;
                DIRTY();
                break;
            }
            case F3D_G_SETCOMBINE: R.cc0 = w0; R.cc1 = w1; break;
            case F3D_G_SETPRIMCOLOR:
                if (((uint32_t)R.prim[0] << 24 | (uint32_t)R.prim[1] << 16 | (uint32_t)R.prim[2] << 8 | R.prim[3]) != w1 ||
                    R.prim_lod_frac != (uint8_t)w0)
                    DIRTY();
                R.prim[0] = (uint8_t)(w1 >> 24); R.prim[1] = (uint8_t)(w1 >> 16);
                R.prim[2] = (uint8_t)(w1 >> 8); R.prim[3] = (uint8_t)w1;
                R.prim_lod_frac = (uint8_t)w0;
                break;
            case F3D_G_SETENVCOLOR:
                if (((uint32_t)R.env[0] << 24 | (uint32_t)R.env[1] << 16 | (uint32_t)R.env[2] << 8 | R.env[3]) != w1) DIRTY();
                R.env[0] = (uint8_t)(w1 >> 24); R.env[1] = (uint8_t)(w1 >> 16);
                R.env[2] = (uint8_t)(w1 >> 8); R.env[3] = (uint8_t)w1;
                break;
            case F3D_G_SETFOGCOLOR:
                R.fogc[0] = (uint8_t)(w1 >> 24); R.fogc[1] = (uint8_t)(w1 >> 16);
                R.fogc[2] = (uint8_t)(w1 >> 8); R.fogc[3] = (uint8_t)w1;
                break;
            case F3D_G_SETBLENDCOLOR:
                R.blend[0] = (uint8_t)(w1 >> 24); R.blend[1] = (uint8_t)(w1 >> 16);
                R.blend[2] = (uint8_t)(w1 >> 8); R.blend[3] = (uint8_t)w1;
                break;
            case F3D_G_SETFILLCOLOR: R.fill = w1; break;
            case F3D_G_SETCIMG: {
                uint32_t c = seg(w1);
                if (XGFX_PROBE && probe_cimg(c, (w0 & 0xFFFu) + 1u, (w0 >> 19) & 3u)) {
                    R.cimg = c;   /* a probe copy's target: not the screen */
                    break;
                }
                if (c == R.zimg) s_zcimg_w = (w0 & 0xFFFu) + 1u;
                if (c != R.zimg) {   /* the frame's colour image as the RDP has it (framebuffer readback) */
                    s_cimg_siz = (w0 >> 19) & 3u;
                    s_cimg_w = (w0 & 0xFFFu) + 1u;
                }
                if (c != R.zimg) frame_cimg(c);
                if (c != R.zimg) screen_size(c, s_cimg_w);
                if ((R.cimg == R.zimg) != (c == R.zimg)) DIRTY();
                if (R.cimg == R.zimg && R.cimg && c != R.zimg) s_shadow_rec = 0;   /* pass 1 ended */
                R.cimg = c;
                break;
            }
            case F3D_G_SETZIMG: R.zimg = seg(w1); DIRTY(); break;
            case F3D_G_SETSCISSOR: {
                uint8_t mode;
                uint16_t ulx, uly, lrx, lry;
                f3d_scissor_decode(w0, w1, &mode, &ulx, &uly, &lrx, &lry);
                s_scissor[0] = ulx * 0.25f; s_scissor[1] = uly * 0.25f;
                s_scissor[2] = lrx * 0.25f; s_scissor[3] = lry * 0.25f;
                if ((uint32_t)(lry >> 2) > s_cimg_maxy) s_cimg_maxy = lry >> 2u;
                clip_rect_update();
                break;
            }
            case F3D_G_NOOP:   /* the port's widescreen tags (patches/050, 052; wings64_widescreen.h) */
                if (w1 == WINGS64_TAG_WORLD_ON || w1 == WINGS64_TAG_WORLD_OFF) {
                    s_world = w1 == WINGS64_TAG_WORLD_ON;
                    s_tagged = 1;
                    DIRTY();
                } else if ((w1 & 0xFFFFFF00u) == WINGS64_TAG_ANCHOR) {
                    s_anchor = (int)(w1 & 0xFF);
                    DIRTY();
                }
                break;
            case F3D_G_RDPFULLSYNC:
                s_fullsync = 1;   /* the RDP's interrupt at the end: DP done (sp.c) */
                break;
            case F3D_G_RDPLOADSYNC: case F3D_G_RDPPIPESYNC: case F3D_G_RDPTILESYNC:
            case F3D_G_SPNOOP: case F3D_G_SETPRIMDEPTH: case F3D_G_SETCONVERT: case F3D_G_SETKEYR:
            case F3D_G_SETKEYGB: case F3D_G_CULLDL:
                break;
            default:
                s_unknown[w0 >> 24]++;
                break;
        }
    }
}

/* ------------------------------------------------------------ frame */

static int s_init;
static uint32_t s_frames;
static uint64_t s_dl_us;

/* ---- the frozen frame under the game's screen fades (patches/026) ----
 * DIVERGES: the N64 copies the last frame into both colour buffers and draws
 * each fade step over it; the back end clears every frame, so the picture
 * presented last is captured once and drawn first in every frame until the
 * fade ends (as wings64-dc replays its last polygon lists). Without it every
 * fade ran over black: a black flash at each menu and level change. */
#ifndef XGFX_HOLD_LAST
#define XGFX_HOLD_LAST 1
#endif
static volatile int s_hold_req;
static int s_hold_on;
static uint32_t s_hold_tex;
static float s_hold_u, s_hold_v;
static int s_hold_linear;   /* s_hold_tex is the GPU hold surface (texel coordinates) */

void f3d_hold_last(int on) { s_hold_req = XGFX_HOLD_LAST && on; }

/* patches/028: the photo album draws each photo once and relies on the N64
 * frame buffer keeping it. DIVERGES: while it asks, every frame starts with
 * the picture presented last (a linear view of that buffer, no copy: it is
 * not drawn into again for two frames), so what was drawn stays. */
#ifndef XGFX_FB_PERSIST
#define XGFX_FB_PERSIST 1
#endif
static volatile int s_persist;
void f3d_fb_persist(int on) { s_persist = XGFX_FB_PERSIST && on; }

#ifndef XGFX_HOLD_GPU
#define XGFX_HOLD_GPU 1   /* 0: the CPU reads the last frame back (~244 ms a fade on the console) */
#endif

/* the whole framebuffer from texture `tex`, s 0..u, t 0..v (texel coordinates for a linear texture) */
static void hold_quad(uint32_t tex, float u, float v, int filter) {
    XgfxState st;
    XgfxVtx q[6];
    float x1 = (float)s_out.fb_w, y1 = (float)s_out.fb_h;
    int i;
    s_side_drawn = 1;   /* the whole last frame, strips included */
    memset(&st, 0, sizeof st);
    st.cc0 = cc_words_const(1 /* TEXEL0 */, 1, &st.cc1);
    st.tex[0] = tex;
    st.wrap_s[0] = st.wrap_t[0] = XGFX_WRAP_CLAMP;
    st.filter[0] = (uint32_t)filter;
    st.color_write = 1;
    st.scissor[2] = s_out.fb_w;
    st.scissor[3] = s_out.fb_h;
    memset(q, 0, sizeof q);
    for (i = 0; i < 6; i++) {
        static const uint8_t corner[6] = { 0, 1, 2, 2, 1, 3 };
        int c = corner[i];
        q[i].x = (c & 1) ? x1 : 0.0f;
        q[i].y = (c & 2) ? y1 : 0.0f;
        q[i].w = 1.0f;
        q[i].s0 = (c & 1) ? u : 0.0f;
        q[i].t0 = (c & 2) ? v : 0.0f;
    }
    xgfx_draw(&st, q, 2);
}

#ifndef XGFX_HOLD_EVICT
#define XGFX_HOLD_EVICT 1   /* 0: never drop the texture cache for the BACK menu's frame and card */
#endif

static void hold_frame(void) {
    if (s_hold_req != s_hold_on) {
        uint64_t t0 = xhw_time_us();
        s_hold_on = s_hold_req;
        if (s_hold_tex) xgfx_tex_destroy(s_hold_tex);
        s_hold_tex = 0;
        s_hold_linear = 0;
        if (s_hold_on && XGFX_HOLD_GPU) {   /* one draw copies the last frame into the hold surface */
            float w, h;
            uint32_t last = xgfx_tex_linear_last(&w, &h);
            if (last && xgfx_target_hold(1)) {
                hold_quad(last, w, h, 0);
                xgfx_target_hold(0);
                s_hold_tex = xgfx_tex_linear_hold(&s_hold_u, &s_hold_v);
                s_hold_linear = s_hold_tex != 0;
            }
            if (last) xgfx_tex_destroy(last);
        }
        if (s_hold_on && !s_hold_tex) s_hold_tex = xgfx_capture_last(&s_hold_u, &s_hold_v);
        if (s_hold_on && !s_hold_tex && XGFX_HOLD_EVICT) {   /* the game's textures fill the pool: see below */
            f3d_tex_reset();
            s_hold_tex = xgfx_capture_last(&s_hold_u, &s_hold_v);
        }
        if (XHW_TEST_BUILD && s_hold_on)
            xhw_logf("[GFX] hold: last frame captured %s in %u ms", s_hold_linear ? "on the GPU" : "by the CPU",
                     (unsigned)((xhw_time_us() - t0) / 1000u));
        if (s_hold_on && !s_hold_tex) xhw_log("[GFX] hold: the last frame could not be captured (pool full)");
    }
    if (!s_hold_tex) {
        if (s_persist) {
            float w, h;
            uint32_t last = xgfx_tex_linear_last(&w, &h);
            if (last) {
                hold_quad(last, w, h, 0);
                xgfx_tex_destroy(last);
            }
        }
        return;
    }
    hold_quad(s_hold_tex, s_hold_u, s_hold_v, !s_hold_linear && s_hold_u * (float)s_out.fb_w > 1024.0f);
}

/* The pillarbox strips outside the 4:3 box, cleared to black at the start
 * of every frame (the box of the frame before: map_setup comes later and
 * rarely changes). With hor+ a world view draws into them; a menu or the
 * title draws only inside the box and does not always clear the whole
 * screen, so a framebuffer of the three could show leftovers there: at
 * 720p a line now and then in the black at the right (maintainer, console
 * run 15). A held or persisted frame (hold_frame) and this frame's
 * extended draws come after and cover the strips as before; a frame without
 * hor+ clears them again at its end (f3d_run).
 * DIVERGES: none (the N64 has no pillarbox). Kill switch -DXGFX_SIDE_CLEAR=0. */
#ifndef XGFX_SIDE_CLEAR
#define XGFX_SIDE_CLEAR 1
#endif
static void side_clear(void) {
    int32_t x0 = (int32_t)s_cx, x1 = (int32_t)(s_cx + s_cw), h = (int32_t)s_out.fb_h;
    if (!XGFX_SIDE_CLEAR || s_out.fb_w <= 0 || h <= 0) return;
    if (x0 > 0) xgfx_clear(0, 0, x0, h, 0xFF000000u, 1, 0, 1.0f);
    if (x1 < s_out.fb_w) xgfx_clear(x1, 0, s_out.fb_w, h, 0xFF000000u, 1, 0, 1.0f);
}

/* A frame with no display list: the held picture (f3d_hold_last(1) first),
 * dimmed, and a card image over it, presented. The BACK menu's frames over
 * the game's last one (xbox/src/rt/settings_menu.c, xhw_panel_render).
 * img: tw x th ARGB (powers of two) holding a pw x ph card in 480-line
 * pixels, straight alpha; shown centred at the content box's scale (1x at
 * 480, 1.5x at 720p; 3/4 as wide on an anamorphic 480), `fade` 0..256 of
 * its opacity and of the dim, `slide` pixels below its place. */
void f3d_present_held(const uint32_t* img, int tw, int th, int pw, int ph, int fade, int slide) {
    XgfxState st;
    XgfxVtx q[6];
    float zy, zx, x0, y0, x1, y1;
    uint32_t tex = 0;
    int i;
    static const uint8_t corner[6] = { 0, 1, 2, 2, 1, 3 };
    if (fade < 0) fade = 0;
    if (fade > 256) fade = 256;
    f3d_present_frame();   /* a game frame still open (its swap never came) */
    xgfx_frame_begin();
    map_setup();
    s_side_drawn = 0;
    hold_frame();
    if (!s_side_drawn) side_clear();
    memset(&st, 0, sizeof st);
    st.color_write = 1;
    st.blend = XGFX_BLEND_ALPHA;
    st.scissor[2] = s_out.fb_w;
    st.scissor[3] = s_out.fb_h;
    /* the dim: black at up to 55 % over the whole frame */
    st.cc0 = cc_words_const(4 /* SHADE */, 4, &st.cc1);
    memset(q, 0, sizeof q);
    for (i = 0; i < 6; i++) {
        int c = corner[i];
        q[i].x = (c & 1) ? (float)s_out.fb_w : 0.0f;
        q[i].y = (c & 2) ? (float)s_out.fb_h : 0.0f;
        q[i].w = 1.0f;
        q[i].rgba[3] = (uint8_t)(140 * fade / 256);
    }
    xgfx_draw(&st, q, 2);
    if (img && fade > 0) {
        tex = xgfx_tex_create((uint32_t)tw, (uint32_t)th, img);
        /* A game's cached textures can fill the pool so the card (512x512, 1 MB)
         * finds no room (Mario Kart 64 on red: 1.2 MB free in pieces, the menu
         * opened with no card). The game is frozen while the menu shows: drop
         * its cache (the textures reload from RDRAM after) and try again. */
        if (!tex && XGFX_HOLD_EVICT) {
            static int said;
            f3d_tex_reset();
            tex = xgfx_tex_create((uint32_t)tw, (uint32_t)th, img);
            if (!said) {
                said = 1;
                xhw_logf("[GFX] menu card: texture pool full, the game's texture cache dropped (%s)",
                         tex ? "card shown" : "still no room");
            }
        }
    }
    if (tex) {
        /* TEXEL0 x SHADE in colour and alpha: the card, faded by the shade's alpha */
        st.cc0 = (1u << 20) | (4u << 15) | (1u << 12) | (4u << 9) | (1u << 5) | 4u;
        st.cc1 = (15u << 28) | (15u << 24) | (1u << 21) | (4u << 18) | (7u << 15) | (7u << 12) | (7u << 9) | (7u << 6) |
                 (7u << 3) | 7u;
        st.tex[0] = tex;
        st.wrap_s[0] = st.wrap_t[0] = XGFX_WRAP_CLAMP;
        zy = s_ch / 480.0f;
        zx = zy * s_cw / (s_ch * 4.0f / 3.0f);
        st.filter[0] = (zx == 1.0f && zy == 1.0f) ? 0u : 1u;
        x0 = (float)(int)(((float)s_out.fb_w - (float)pw * zx) * 0.5f);
        y0 = (float)(int)(((float)s_out.fb_h - (float)ph * zy) * 0.5f) + (float)slide * zy;
        x1 = x0 + (float)pw * zx;
        y1 = y0 + (float)ph * zy;
        for (i = 0; i < 6; i++) {
            int c = corner[i];
            q[i].x = (c & 1) ? x1 : x0;
            q[i].y = (c & 2) ? y1 : y0;
            q[i].w = 1.0f;
            q[i].rgba[0] = q[i].rgba[1] = q[i].rgba[2] = 255;
            q[i].rgba[3] = (uint8_t)(255 * fade / 256);
            q[i].s0 = (c & 1) ? (float)pw / (float)tw : 0.0f;
            q[i].t0 = (c & 2) ? (float)ph / (float)th : 0.0f;
        }
        xgfx_draw(&st, q, 2);
        xgfx_tex_destroy(tex);   /* deferred until the GPU is done with it */
    }
    xgfx_present();
}

/* ---------------------------------------------- framebuffer effects (fbfx.h)
 * The frame is drawn on the NV2A and never lands in RDRAM; a game that reads
 * it back (Mario Kart 64's big screens copy the frame before with the CPU:
 * render_courses.c in the decomp; Pokemon Snap's photos) found stale memory.
 * DIVERGES: none meant: RDRAM gets the picture the game drew, at N64 size.
 * At the end of every frame one draw copies the picture, N64 size, onto the
 * back end's readback stage (a quarter of it per colour image, fbfx.c).
 * At the start of the next frame (the GPU is done with it by then) the
 * colour image's pages are looked at (xhw_watch_pages: the x86's accessed
 * bits, armed every frame): a frame some thread read or wrote since is hot
 * for FBFX_HOT frames, and a hot frame's stage picture is converted into
 * RDRAM (RGBA5551 or RGBA8888, word-swapped) every frame. So the first read
 * after a quiet spell sees the picture before (MK64's screen: one frame
 * late, once); a game that never reads its frame pays the copy draw and the
 * page arming, no CPU conversion. A texture load from a frame not yet
 * converted (the interpreter's own read: a frame used as a texture the frame
 * after) converts it first (fb_touch).
 * XGFX_FB_READBACK (default 0, off): 1 as above, 2 every frame (Pokemon Snap's one-shot
 * photo reads would need it; ~1 ms a frame of CPU on the console, not
 * measured), 3 staged every frame but converted only when the game
 * invalidates the data cache over it (osInvalDCache, f3d_fb_inval) or
 * textures from it, 0 off (the old renderer: RDRAM keeps what the game wrote).
 * Not done: a colour image drawn and textured from within the same frame
 * (render to texture: Zelda's pause, Banjo's jigsaw) draws on the screen as
 * before (XGFX_FB_RTT is reserved for it; docs/renderer.md). */
#ifndef XGFX_FB_PROBE
#define XGFX_FB_PROBE 0   /* 1: the first 8 readbacks' stage and RDRAM bytes logged (Banjo's START-screen static on red) */
#endif
#ifndef XGFX_FB_READBACK
#define XGFX_FB_READBACK 0   /* off by default: Pilotwings 64 touches its frames every ~2 s and ran at half speed in xemu */
#endif
#ifndef XGFX_FB_DUMP
#define XGFX_FB_DUMP 0   /* debug: n > 0 dumps RDRAM's frame after the n-th readback ([FBDUMP]) */
#endif
#if XGFX_FB_DUMP
void xhw_fbdump(const void* fb, int w, int h, int bpp, int pitch);
#endif
static uint32_t s_fb_stage_w, s_fb_stage_h;
static uint32_t s_fb_copies, s_fb_reads, s_fb_touched, s_fb_us;   /* per [GFX] interval */

static int fb_watch(const FbfxSlot* sl, int arm) {
    return XGFX_FB_READBACK == 1 ? xhw_watch_pages((uint32_t)(uintptr_t)xu_host(sl->addr), fbfx_bytes(sl), arm) : 0;
}

/* RDRAM gets slot i's picture (gfx lock held) */
static void fb_resolve(int i) {
    const FbfxSlot* sl = fbfx_slot(i);
    const uint8_t* stage;
    int32_t pitch, bpp, r[4];
    uint64_t t0;
    if (!sl || !sl->addr || !sl->stale) return;
    t0 = xhw_time_us();
    fbfx_mark(i, 0);
    stage = (const uint8_t*)xgfx_stage_read(&pitch, &bpp);
    if (!stage) return;
    fbfx_rect(i, (int32_t)s_fb_stage_w, (int32_t)s_fb_stage_h, r);
    fbfx_convert(sl, r, stage, pitch, bpp, xu_host(0), xrc_rdram_size);
    s_fb_reads++;
#if XGFX_FB_PROBE   /* diagnosis, release builds too: what the first readbacks held (the stage, then RDRAM's frame) */
    static unsigned s_probe_n;
    if (++s_probe_n <= 8) {
        const uint8_t* s0 = stage + (size_t)r[1] * (size_t)pitch + (size_t)r[0] * (size_t)(bpp / 8);
        uint32_t k, sum = 0, rsum = 0;
        for (k = 0; k < 256; k++) sum = sum * 31u + s0[k];
        for (k = 0; k < 256 && k < fbfx_bytes(sl); k++) rsum = rsum * 31u + xu_host(0)[(sl->addr + k) & (xrc_rdram_size - 1)];
        xhw_logf("[GFX] fb probe %u: rect %d,%d %dx%d, stage %02x%02x%02x%02x %02x%02x%02x%02x sum %08x, rdram %08x sum %08x",
                 s_probe_n, (int)r[0], (int)r[1], (int)r[2], (int)r[3], s0[0], s0[1], s0[2], s0[3], s0[4], s0[5], s0[6], s0[7],
                 (unsigned)sum, (unsigned)sl->addr, (unsigned)rsum);
    }
#endif
#if XGFX_FB_DUMP   /* debug: RDRAM's frame as the game reads it, the n-th time, as an [FBDUMP] */
    {
        static unsigned s_n;
        static uint32_t s_img[320 * 240];
        uint32_t x, y;
        if (++s_n == XGFX_FB_DUMP && sl->siz == 2 && sl->w <= 320 && sl->h <= 240) {
            for (y = 0; y < sl->h; y++)
                for (x = 0; x < sl->w; x++) {
                    uint32_t c = *(const uint16_t*)(xu_host(0) + ((sl->addr + 2u * (y * sl->w + x)) ^ 2u));
                    s_img[y * sl->w + x] = 0xFF000000u | (c >> 11) << 19 | ((c >> 6) & 31) << 11 | ((c >> 1) & 31) << 3;
                }
            xhw_logf("[GFX] fb dump: readback %u of %08x", s_n, (unsigned)sl->addr);
            xhw_fbdump(s_img, (int)sl->w, (int)sl->h, 32, (int)sl->w * 4);
        }
    }
#endif
    s_fb_us += (uint32_t)(xhw_time_us() - t0);
}

static void fb_touch(uint32_t addr, uint32_t bytes) {
    int i;
    while ((i = fbfx_find(seg(addr), bytes)) >= 0) fb_resolve(i);
}

/* osInvalDCache over a colour image: the game is about to read it with the
 * CPU (Banjo-Kazooie copies its frame into the jigsaw transition's 80
 * textures, model_copyFramebufferToTextures, right after invalidating it):
 * the staged picture into RDRAM now. The trigger of XGFX_FB_READBACK 3, and
 * a no-op in the other modes' frames already resolved (gfx lock held). */
#ifndef XGFX_FB_INVAL_SHOT
#define XGFX_FB_INVAL_SHOT 0   /* debug: n > 0 takes a screenshot n frames after each such readback */
#endif
static unsigned s_inval_shot;
void f3d_fb_inval(uint32_t addr, uint32_t bytes) {
    unsigned before = s_fb_reads;
    int i;
    if (!XGFX_FB_READBACK || !bytes) return;
    addr = seg(addr);
    /* only a range that covers at least half of a frame: a DMA's destination
     * invalidated over an old frame's memory is not a read of the picture */
    for (i = 0; i < FBFX_SLOTS; i++) {
        const FbfxSlot* sl = fbfx_slot(i);
        uint32_t lo, hi;
        if (!sl || !sl->addr || !sl->stale) continue;
        lo = addr > sl->addr ? addr : sl->addr;
        hi = addr + bytes < sl->addr + fbfx_bytes(sl) ? addr + bytes : sl->addr + fbfx_bytes(sl);
        if (hi > lo && (hi - lo) * 2u >= fbfx_bytes(sl)) fb_resolve(i);
    }
    if (XGFX_FB_INVAL_SHOT && s_fb_reads != before) s_inval_shot = XGFX_FB_INVAL_SHOT;
}

/* the start of a frame: the frames read lately into RDRAM, the watch armed again */
static void fb_frame_start(void) {
    int i;
    if (!XGFX_FB_READBACK) return;
    if (XGFX_FB_INVAL_SHOT && s_inval_shot && !--s_inval_shot) xhw_shot_request(1);
    for (i = 0; i < FBFX_SLOTS; i++) {
        const FbfxSlot* sl = fbfx_slot(i);
        int touched;
        if (!sl->addr) continue;
        touched = XGFX_FB_READBACK == 2 || fb_watch(sl, 0) > 0;   /* 3: never here; osInvalDCache resolves */
        s_fb_touched += touched && XGFX_FB_READBACK == 1;
        if (fbfx_frame_start(i, touched)) fb_resolve(i);
        fb_watch(sl, 1);
    }
}

/* the frame's picture onto the stage, its pages guarded (end of frame, before the port's own drawing) */
static void fb_stage(void) {
    FbfxSlot drop[FBFX_SLOTS];
    int i, nd;
    uint32_t back, h;
    int32_t sw, sh, r[4];
    float bw, bh, x0, y0, x1, y1;
    if (!XGFX_FB_READBACK || !R.cimg || R.cimg == R.zimg || s_hold_tex) return;
    /* the height: the frame's lowest scissor or fill row, at most 3/4 of the width (320 x 240) */
    h = s_cimg_w * 3u / 4u;
    if (s_cimg_maxy >= 8u && s_cimg_maxy < h) h = s_cimg_maxy;
    i = fbfx_frame_end(R.cimg, s_cimg_w, h, s_cimg_siz, s_frames, xrc_rdram_size, drop, &nd);
    (void)drop;
    if (i < 0) return;
    flush();
    back = xgfx_tex_linear_back(&bw, &bh);
    if (!back) return;
    if (xgfx_target_stage(1, &sw, &sh)) {
        XgfxState st;
        XgfxVtx q[6];
        const FbfxSlot* sl = fbfx_slot(i);
        int k;
        s_fb_stage_w = (uint32_t)sw;
        s_fb_stage_h = (uint32_t)sh;
        fbfx_rect(i, sw, sh, r);
        /* the N64 frame's pixels on the screen: the same mapping the draws had */
        x0 = s_ox;
        y0 = s_oy;
        x1 = s_ox + N64_W * s_sx * (float)sl->w / (float)s_cimg_w;
        y1 = s_oy + N64_H * s_sy * (float)sl->h / ((float)s_cimg_w * 0.75f);
        memset(&st, 0, sizeof st);
        st.cc0 = cc_words_const(1 /* TEXEL0 */, 1, &st.cc1);
        st.tex[0] = back;
        st.wrap_s[0] = st.wrap_t[0] = XGFX_WRAP_CLAMP;
        st.filter[0] = 1;   /* a 2:1 shrink through bilinear is a box filter */
        st.color_write = 1;
        st.scissor[0] = r[0];
        st.scissor[1] = r[1];
        st.scissor[2] = r[0] + r[2];
        st.scissor[3] = r[1] + r[3];
        memset(q, 0, sizeof q);
        for (k = 0; k < 6; k++) {
            static const uint8_t corner[6] = { 0, 1, 2, 2, 1, 3 };
            int c = corner[k];
            q[k].x = (float)((c & 1) ? r[0] + r[2] : r[0]);
            q[k].y = (float)((c & 2) ? r[1] + r[3] : r[1]);
            q[k].w = 1.0f;
            q[k].s0 = (c & 1) ? x1 : x0;
            q[k].t0 = (c & 2) ? y1 : y0;
        }
        if (!xgfx_stage_cpu(r, x0, y0, x1, y1)) xgfx_draw(&st, q, 2);   /* nv2a.c XGFX_FB_READBACK_CPU, else the GPU */
        xgfx_target_stage(0, &sw, &sh);
        DIRTY();
        fbfx_mark(i, 1);
        s_fb_copies++;
    }
    xgfx_tex_destroy(back);
}

int f3d_last_fullsync(void) { return s_fullsync; }

/* A frame is opened by the first graphics task after a present and
 * presented by f3d_present_frame: at the end of the task (f3d_run with
 * present = 1), or when the game swaps (gfx.c). Mario Party draws one frame
 * in several tasks (its swap-chain thread sends tasks until the last one's
 * flag, then calls osViSwapBuffer: src/engine/graphics.c in the decomp);
 * presented task by task, every other picture on the console was a part
 * of the frame on black (maintainer, vmp1: "flickers so bad"). */
static int s_fopen;
static uint32_t s_open_cimg;   /* the open frame's colour image: a buffer the game swaps to (physical) */
static uint32_t s_swapped[4];  /* the buffers the game swapped to lately */
static int s_split;            /* the frame stays open past its task (gfx.c: present on swap) */
static void frame_open(void);

static int swap_buffer(uint32_t c) {
    int i;
    for (i = 0; i < 4; i++)
        if (s_swapped[i] && s_swapped[i] == c) return 1;
    return 0;
}

/* G_SETCIMG: the frame's buffer. A task drawing into another buffer the
 * game swaps to has begun the next frame: the open one is complete (a
 * scheduler that swaps at the retrace after the task, as Ocarina of Time's,
 * may run the next frame's task first). */
/* vi.c: the rows the VI mode shows of the framebuffer (its yScale over the
 * field's lines); 0 when no mode is known (weak: the host tests) */
__attribute__((weak)) unsigned xu_vi_rows(void) { return 0; }

/* a buffer the game swaps to, drawn at width w: the screen's size in N64
 * pixels (the height from the VI mode when it is not 3/4 of the width:
 * 640x240) and the map to the output. 320x240 maps as before.
 * DIVERGES: none (the VI scales such a buffer to the whole screen).
 * -DXGFX_HIRES=0: 320x240 always (a wider buffer's left part shown). */
static void screen_size(uint32_t c, uint32_t w) {
    float fw, fh;
    unsigned rows;
    if (!XGFX_HIRES || w < 256u || w > 1024u || !swap_buffer(c)) return;
    fw = (float)w;
    fh = (float)(w * 3u / 4u);
    rows = xu_vi_rows();
    if (rows >= 120u && rows <= 1024u && (rows > (unsigned)fh * 17u / 16u || rows < (unsigned)fh * 15u / 16u))
        fh = (float)rows;
    if (fw == s_n64w && fh == s_n64h) return;
    flush();
    xhw_logf("[GFX] screen %ux%u (a buffer %u wide)", (unsigned)fw, (unsigned)fh, (unsigned)w);
    s_n64w = fw, s_n64h = fh;
    map_setup();
    s_scissor[0] = s_scissor[1] = 0.0f; s_scissor[2] = N64_W; s_scissor[3] = N64_H;
    clip_rect_update();
}

static void frame_cimg(uint32_t c) {
    if (!s_split || !s_fopen || !swap_buffer(c)) return;
    if (s_open_cimg && s_open_cimg != c) {
        flush();
        f3d_present_frame();
        frame_open();
        DIRTY();
    }
    s_open_cimg = c;
}

static void frame_open(void) {
    if (s_fopen) return;
    s_fopen = 1;
    s_open_cimg = 0;
    s_zorder = 0;   /* the depth tie-break counts on across the frame's tasks */
    xgfx_frame_begin();
    fb_frame_start();
    s_cimg_maxy = 0;
    s_side_drawn = 0;
    side_clear();
    hold_frame();
}

/* osViSwapBuffer(fb): the open frame presented if it is fb's (or drew
 * into no buffer the game swaps to); 1 when it was */
int f3d_swap(uint32_t fb) {
    fb &= 0x1FFFFFFFu;
    if (!swap_buffer(fb)) {
        memmove(&s_swapped[1], &s_swapped[0], 3 * sizeof s_swapped[0]);
        s_swapped[0] = fb;
    }
    if (!s_fopen || (s_open_cimg && s_open_cimg != fb)) return 0;
    f3d_present_frame();
    return 1;
}

void f3d_split_frames(int on) { s_split = on; }
int f3d_frame_open(void) { return s_fopen; }

void f3d_run(uint32_t dl, int present) {
    uint64_t t0 = xhw_time_us();
    int i;
    if (!s_init) {
        s_init = 1;
        R.vp_sx = 160.0f; R.vp_sy = 120.0f; R.vp_tx = 160.0f; R.vp_ty = 120.0f;
        s_ostamp++;
        for (i = 0; i < 16; i++) R.mv[0][i >> 2][i & 3] = R.proj[i >> 2][i & 3] = (i % 5) == 0 ? 1.0f : 0.0f;
    }
    frame_open();
    s_title = wx_game_state() == 0;
    map_setup();
    s_world = 0;
    s_anchor = 'C';
    R.mv_top = 0;
    R.mvp_dirty = 1;
    s_shadow_dl = 0;
    s_shadow_rec = s_shadow_pass2 = 0;
    s_scissor[0] = s_scissor[1] = 0.0f; s_scissor[2] = N64_W; s_scissor[3] = N64_H;
    clip_rect_update();
    s_frame_no++;
    s_fullsync = 0;
    s_rdp_n = 0;
    s_trace = XGFX_TRACE_FRAME && s_frames == XGFX_TRACE_FRAME;
    if ((XGFX_TRACE_PRIM || XGFX_TRACE_MISS) && s_trace_next == 1) s_trace_next = 2, s_trace = 1;
#if XGFX_TRACE_MS
    {
        static int s_traced;
        static uint64_t s_first;   /* ms from the first frame */
        if (!s_first) s_first = t0;
        if (!s_traced && t0 - s_first >= (uint64_t)XGFX_TRACE_MS * 1000u) s_traced = s_trace = 1;
    }
#endif
    DIRTY();
    s_dl_cmds = 0;
    s_in_dl = 1, s_present_in_dl = 0;
    xhw_perf_phase(1);
    run_dl((const uint32_t*)host(dl));
    flush();
    xhw_perf_phase(0);
    s_in_dl = 0;
    if (!s_benching) {
        /* a present the list made (a split frame) is the present's, not the list's (review 2.2) */
        uint64_t t1 = xhw_time_us(), us = t1 - t0 > s_present_in_dl ? t1 - t0 - s_present_in_dl : 0;
        s_dl_us += us;
        xhw_perf_add(XHW_PERF_DL, us);
    }
    if (present) f3d_present_frame();
}

/* gfx.c's XRC_DLBENCH: one run of the list into the open frame, the back end
 * left out (null_back) or not; the caller times it */
void f3d_bench(uint32_t dl, int null_back) {
    s_benching = 1;
    s_null_back = null_back;
    f3d_run(dl, 0);
    s_null_back = 0;
    s_benching = 0;
}

/* the open frame's end: the port's drawing, the present, the counters */
static unsigned s_presents;
unsigned f3d_presents(void) { return s_presents; }

void f3d_present_frame(void) {
    int i;
    if (!s_fopen) return;
    s_fopen = 0;
    s_presents++;
    if (s_title && XGFX_TITLE_FIT) {   /* the black margin around the fitted title picture (drawn last) */
        int32_t m = (int32_t)(TITLE_MARGIN * s_ch / 480.0f), mx = (int32_t)((float)m * s_cw / (s_ch * 4.0f / 3.0f)),
                cx = (int32_t)s_cx,
                cw = (int32_t)s_cw, ch = (int32_t)s_ch;
        xgfx_clear(cx, 0, cx + cw, m, 0xFF000000u, 1, 0, 1.0f);
        xgfx_clear(cx, ch - m, cx + cw, ch, 0xFF000000u, 1, 0, 1.0f);
        xgfx_clear(cx, m, cx + mx, ch - m, 0xFF000000u, 1, 0, 1.0f);
        xgfx_clear(cx + cw - mx, m, cx + cw, ch - m, 0xFF000000u, 1, 0, 1.0f);
    }
    /* and again once the frame is drawn, unless it drew into the strips on
     * purpose (a hor+ world draw, a fill stretched to 16:9, a held frame):
     * the fitted title maps its N64 x 310-320 past the box (about 13 px at
     * 720p) and not every draw there is scissored to it, so a sliver of the
     * flyover showed as a line in the right strip (console, rc1). Keyed on
     * what was drawn, not on hor+ being on: at 720p with Widescreen Auto a
     * screen with no tagged world view is hor+ and drew nothing there (rc1b) */
    if (!s_side_drawn) side_clear();
    fb_stage();
    wx_frame_overlay();   /* the port's own text (FPS counter, settings panel; patches/021) */
    {
        uint64_t t1 = xhw_time_us(), us, log;
        xhw_perf_phase(2);
        xgfx_present();
        xhw_perf_phase(0);
        us = xhw_time_us() - t1;
        log = xhw_perf_log_us();   /* the [PERF] lines written inside it */
        us = us > log ? us - log : 0;
        xhw_perf_add(XHW_PERF_PRESENT, us);
        if (s_in_dl) s_present_in_dl += xhw_time_us() - t1, xhw_perf_phase(1);
    }
    if (s_frames % 600 == 0)
        xhw_logf("[GFX] frame %u state %d: tris %u, rejected %u (far %u, near %u), culled %u, clipped %u (far %u, near %u) | "
                 "vtx loads %u, mtx %u",
                 s_frames, wx_game_state(), s_t_in, s_t_rej, s_t_rej_far, s_t_rej_near, s_t_cull, s_t_clip, s_t_far,
                 s_t_near, s_vtx_loads, s_mtx_loads);
    {   /* frames that drew next to nothing: what they did (the in-game flashing) */
        static unsigned s_nthin;
        if (XHW_TEST_BUILD && s_t_in < 20 && !s_hold_tex && s_nthin < 40) {
            s_nthin++;
            xhw_logf("[GFX] thin frame %u state %d: tris %u, vtx loads %u, mtx %u, rects %u, fills %u, zclears %u, "
                     "cimg %08x zimg %08x", s_frames, wx_game_state(), s_t_in, s_vtx_loads, s_mtx_loads, s_rects,
                     s_fills, s_zclears, (unsigned)R.cimg, (unsigned)R.zimg);
        }
    }
    {   /* frames that drew far less (or far more) than the one before: what flickers */
        static uint32_t s_prev_tris, s_prev_state, s_nodd;
        int st = wx_game_state();
        if (XHW_TEST_BUILD && s_frames > 0 && st == (int)s_prev_state && s_nodd < 200 &&
            (s_t_in * 2 < s_prev_tris || s_t_in > s_prev_tris * 2) && (s_t_in > 30 || s_prev_tris > 30)) {
            s_nodd++;
            xhw_logf("[GFX] odd frame %u state %d: tris %u (before %u), vtx loads %u, mtx %u, rects %u, fills %u, "
                     "zclears %u, cimg %08x", s_frames, st, s_t_in, s_prev_tris, s_vtx_loads, s_mtx_loads, s_rects,
                     s_fills, s_zclears, (unsigned)R.cimg);
        }
        s_prev_tris = s_t_in;
        s_prev_state = (uint32_t)st;
    }
    s_rects = s_fills = s_zclears = 0;
    s_t_in = s_t_rej = s_t_cull = s_t_clip = s_vtx_loads = s_mtx_loads = 0;
    s_t_rej_far = s_t_rej_near = s_t_far = s_t_near = 0;
    if (++s_frames % 600 == 0) {
        xhw_logf("[GFX] frame %u: display lists %u us a frame (avg over 600)", s_frames, (unsigned)(s_dl_us / 600));
        if (XGFX_FB_READBACK)
            xhw_logf("[GFX] framebuffer readback (mode %d): %u copies, %u frames touched, %u read back in %u us",
                     XGFX_FB_READBACK, s_fb_copies, s_fb_touched, s_fb_reads, (unsigned)s_fb_us);
        s_fb_copies = s_fb_reads = s_fb_touched = s_fb_us = 0;
        s_dl_us = 0;
        for (i = 0; i < 256; i++)
            if (s_unknown[i]) {
                xhw_logf("[GFX] unhandled op %02X x%u", i, s_unknown[i]);
                s_unknown[i] = 0;
            }
    }
}
