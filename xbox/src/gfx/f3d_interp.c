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
#define rd16(a) rd16(XU_K0(a))
#define rd32(a) rd32(XU_K0(a))
#define xu_host(a) xu_host(XU_K0(a))
#define xu_from_rdram(d, a, n) xu_from_rdram((d), XU_K0(a), (n))

/* ---- switches (docs/testing.md) ---- */
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
#ifndef XGFX_TILE_WRAP
#define XGFX_TILE_WRAP 1       /* tile sizes wrap in 12 bits (a scrolling tile with lrs < uls) */
#endif
#ifndef XGFX_DESWIZZLE32
#define XGFX_DESWIZZLE32 1     /* LOADBLOCK dxt 0 of RGBA32: odd rows swap texel pairs (TMEM's two banks) */
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
} Vtx;
#define VTX_LERP_FLOATS 11

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
    Vtx vc[VC_MAX];
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

/* --------------------------------------------------------------- matrices */

/* N64 Mtx = { s16 i[4][4]; u16 f[4][4] }: halfword e of each half, read
 * from the image at N64 addresses */
static void mtx_from_n64(float out[4][4], uint32_t m) {
    int e;
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

static void do_vtx(uint32_t n, uint32_t v0, uint32_t addr) {
    uint32_t i, src;
    src = seg(addr);
    if (R.mvp_dirty) {
        mtx_mul(R.mvp, R.mv[R.mv_top], R.proj);   /* row vectors: v MV P */
        R.mvp_dirty = 0;
    }
    for (i = 0; i < n && v0 + i < s_vmax; i++, src += 16) {
        /* N64 Vtx: s16 ob[3], u16 flag, s16 tc[2], u8 cn[4] */
        int16_t ob[3], tc[2];
        uint8_t cn[4];
        Vtx* v = &R.vc[v0 + i];
        float x, y, z;
        ob[0] = (int16_t)rd16(src); ob[1] = (int16_t)rd16(src + 2); ob[2] = (int16_t)rd16(src + 4);
        tc[0] = (int16_t)rd16(src + 8); tc[1] = (int16_t)rd16(src + 10);
        cn[0] = rd8(src + 12); cn[1] = rd8(src + 13); cn[2] = rd8(src + 14); cn[3] = rd8(src + 15);
        x = ob[0]; y = ob[1]; z = ob[2];
        const float(*m)[4] = R.mvp;
        v->x = x * m[0][0] + y * m[1][0] + z * m[2][0] + m[3][0];
        v->y = x * m[0][1] + y * m[1][1] + z * m[2][1] + m[3][1];
        v->z = x * m[0][2] + y * m[1][2] + z * m[2][2] + m[3][2];
        v->w = x * m[0][3] + y * m[1][3] + z * m[2][3] + m[3][3];
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

static void flush(void) {
    if (!s_nb) return;
    log_side_leak();
    if (s_trace) {
        uint32_t i;
        float zlo = 2e9f, zhi = -2e9f;
        for (i = 0; i < s_nb * 3; i++) {
            if (s_batch[i].z < zlo) zlo = s_batch[i].z;
            if (s_batch[i].z > zhi) zhi = s_batch[i].z;
        }
        xhw_logf("[DRAW] z %d..%d (x1000)", (int)(zlo * 1000.0f / s_out.zmax), (int)(zhi * 1000.0f / s_out.zmax));
    }
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
    uint32_t handle;
    uint16_t w, h, pw, ph;
} TexEntry;
static TexEntry s_texc[TEX_CACHE];
static uint32_t s_frame_no = 1;

/* every word of a texture up to this size, 16 samples of a larger one: a
 * game that writes its font's glyphs into the same slots changed bytes the
 * samples missed (Blast Corps' intro text: T for G). -DXGFX_TEX_FULLHASH_MAX=64:
 * as before */
#ifndef XGFX_TEX_FULLHASH_MAX
#define XGFX_TEX_FULLHASH_MAX 2048
#endif
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
    unsigned w = XGFX_TILE_WRAP ? ((((unsigned)t->lrs - t->uls) & 0xFFFu) >> 2) + 1 : ((t->lrs - t->uls) >> 2) + 1,
             h = XGFX_TILE_WRAP ? ((((unsigned)t->lrt - t->ult) & 0xFFFu) >> 2) + 1 : ((t->lrt - t->ult) >> 2) + 1;
    /* RGBA32 splits each texel across TMEM's two banks: SETTILE's line covers half a row */
    unsigned line = t->line * 8u * (t->siz == 3 ? 2u : 1u), bytes, pw, ph, slot, i;
    uint32_t key, sample;
    uint64_t tex_t0;
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
    /* tiles may declare more rows than were loaded (UVBT, fonts): only loaded rows exist */
    if (h > ld->bytes / line) h = ld->bytes / line;
    /* a load shorter than one row (Banjo-Kazooie: 48 bytes under a 64-byte
     * line) still fills that row's start: one row, the rest as RDRAM has it */
    if (!h && ld->bytes && XGFX_TILE_WRAP) h = 1;
    if (w == 0 || h == 0 || w > 1024 || h > 1024) { tex_miss("size", tidx); return NULL; }
    bytes = line * h;
    if (bytes > sizeof raw) { tex_miss("too big", tidx); return NULL; }
    key = t->fmt | t->siz << 3 | (w - 1) << 5 | (h - 1) << 15 | (uint32_t)(line & 0xFF) << 24 | (uint32_t)(ld->dxt != 0) << 31;
    if (t->fmt == F3D_G_IM_FMT_CI) {   /* the palette is part of the picture: in the key (equality only) */
        unsigned n = t->siz == F3D_G_IM_SIZ_4b ? 16u : 256u, b = t->siz == F3D_G_IM_SIZ_4b ? t->palette * 16u : 0u;
        uint32_t th = f3d_omh_text_lut(R.omh);
        for (i = 0; i < n; i++) th = (th ^ R.tlut[b + i]) * 16777619u;
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
    sample = sample_hash(host(ld->addr), ld->tile ? ld->row_bytes : bytes) ^
             (ld->tile ? sample_hash(host(ld->addr + (ld->rows - 1u) * ld->stride), ld->row_bytes) : 0);
    slot = ((ld->addr >> 3) * 2654435761u ^ key * 40503u) % TEX_CACHE;
    for (i = 0; i < 8; i++) {   /* 8-way probe */
        e = &s_texc[(slot + i) % TEX_CACHE];
        if (e->handle && e->addr == ld->addr && e->key == key && e->sample == sample) goto hit;
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
#endif
    pw = texconv_pow2(w);
    ph = texconv_pow2(h);
    if (pw * ph > sizeof conv / 4) { tex_miss("too big", tidx); return NULL; }
    if (t->fmt == F3D_G_IM_FMT_CI
            ? !texconv_ci_to_argb8(raw, line, w, h, (n64_siz_t)t->siz, R.tlut, t->palette * 16u,
                                   f3d_omh_text_lut(R.omh) == 3, conv, pw, ph)
            : !texconv_to_argb8(raw, line, w, h, (n64_fmt_t)t->fmt, (n64_siz_t)t->siz, conv, pw, ph)) {
        tex_miss("format", tidx);
        return NULL;
    }
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
    e->w = (uint16_t)w;
    e->h = (uint16_t)h;
    e->pw = (uint16_t)pw;
    e->ph = (uint16_t)ph;
    xhw_perf_add(XHW_PERF_TEX, xhw_time_us() - tex_t0);
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
static void tex_miss(const char* why, int tidx) {
    static unsigned s_n;
    const Tile* t = &R.tile[tidx & 7];
    const TmemLoad* ld = &R.loads[(t->tmem >> 3) & 63];
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
    }
}

static float shift_scale(int sh) {
    if (sh == 0) return 1.0f;
    if (sh <= 10) return 1.0f / (float)(1 << sh);
    return (float)(1 << (16 - sh));
}

static uint32_t wrap_of(uint8_t cm) {
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
        xhw_logf("[DRAW] bind %d tile %d fmt %u siz %u line %u tmem %u sl %u tl %u sh %u th %u pal %u -> addr %08x %ux%u",
                 u, tidx & 7, t->fmt, t->siz, t->line, t->tmem, t->uls >> 2, t->ult >> 2, t->lrs >> 2, t->lrt >> 2,
                 t->palette, e ? (unsigned)e->addr : 0u, e ? e->pw : 0u, e ? e->ph : 0u);
    if (!e) return;
    s_st.wrap_s[u] = wrap_of(t->cms);
    s_st.wrap_t[u] = wrap_of(t->cmt);
    s_st.filter[u] = ((R.omh >> F3D_G_MDSFT_TEXTFILT) & 3) != 0;
    /* u = (s * shift - uls) / padded width */
    s_u_scale[u] = shift_scale(t->shifts) / (float)e->pw;
    s_u_off[u] = t->uls * 0.25f / (float)e->pw;
    s_v_scale[u] = shift_scale(t->shiftt) / (float)e->ph;
    s_v_off[u] = t->ult * 0.25f / (float)e->ph;
}

/* N64 render mode -> XgfxState pixel choices (wings64-dc draw_state) */
static void pixel_state(int depth_override_always) {
    uint32_t rm = R.oml;
    int cycle = (R.omh >> F3D_G_MDSFT_CYCLETYPE) & 3;
    int zb = (R.geom & F3D_G_ZBUFFER) && !depth_override_always;
    s_st.blend = ((rm & F3D_FORCE_BL) && cycle < 2) ? XGFX_BLEND_ALPHA : XGFX_BLEND_OFF;
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

static void draw_state(void) {
    int cycle = (R.omh >> F3D_G_MDSFT_CYCLETYPE) & 3;
    flush();
    memset(&s_st, 0, sizeof s_st);
    s_st.cc0 = R.cc0 & 0x00FFFFFFu;
    s_st.cc1 = R.cc1;
    s_st.two_cycle = cycle == 1;
    pixel_state(0);
    s_textured = s_tex1 = 0;
    if (R.tex_on) {
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

static void to_out(const Vtx* v, XgfxVtx* o, const Vtx* flat) {
    float iw = 1.0f / v->w;
    float z01 = (v->z * iw + 1.0f) * 0.5f;
    if (s_zrel > 0.0f && R.proj[2][3] < -1e-6f) {
        /* nearer by s_zrel of the eye distance: depth tends to A far away
         * (A = (1 + P22/P23)/2 for a perspective P, scale included), and a
         * point at w * (1 - f) lies (A - z01) * f nearer */
        float a = 0.5f * (1.0f + R.proj[2][2] / R.proj[2][3]);
        z01 -= (a - z01) * s_zrel;
    }
    const Vtx* c = flat ? flat : v;
    o->x = (R.vp_tx + v->x * iw * R.vp_sx) * s_sx + s_ox + s_xoff;
    o->y = (R.vp_ty - v->y * iw * R.vp_sy) * s_sy + s_oy;
    o->z = (z01 < 0.0f ? 0.0f : z01 > 1.0f ? 1.0f : z01) * s_out.zmax;
#ifdef XGFX_DEBUG_ZHALF
    o->z *= 0.5f;
#endif
    o->w = v->w;
    o->rgba[0] = to8(c->r);
    o->rgba[1] = to8(c->g);
    o->rgba[2] = to8(c->b);
    o->rgba[3] = (R.geom & F3D_G_FOG) ? 255 : to8(c->a);   /* the RSP puts fog in shade alpha; the combiner sees 1 */
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

static void emit_poly(const Vtx* poly, int n, const Vtx* flat) {
    XgfxVtx pv[12];
    int i;
    if (s_nb + (uint32_t)(n - 2) > BATCH_TRIS) flush();
    for (i = 0; i < n; i++) to_out(&poly[i], &pv[i], flat);
    if (s_overlay) overlay_full(pv, n);
    for (i = 1; i + 1 < n; i++) {
        XgfxVtx* d = &s_batch[s_nb * 3];
        d[0] = pv[0];
        d[1] = pv[i];
        d[2] = pv[i + 1];
        s_nb++;
    }
}

static void lerp_vtx(Vtx* o, const Vtx* a, const Vtx* b, float t) {
    const float *pa = &a->x, *pb = &b->x;
    float* po = &o->x;
    unsigned i;
    for (i = 0; i < VTX_LERP_FLOATS; i++) po[i] = pa[i] + (pb[i] - pa[i]) * t;
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
        if (in[i].w < 1e-5f) in[i].w = 1e-5f;
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
        Vtx tri[3] = { *v[0], *v[1], *v[2] };
        emit_poly(tri, 3, flat);
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
        if (x0 <= SUB_X0 && x1 >= SUB_X1 && y0 <= SUB_Y0 && y1 >= SUB_Y1) {   /* SUBSCREEN-sized: the whole picture */
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

static void do_texrect(uint32_t w0, uint32_t w1) {
    uint16_t xl, yl, xh, yh;
    s_rects++;
    uint8_t tile;
    int16_t s, t, dsdx, dtdy;
    int cycle = (R.omh >> F3D_G_MDSFT_CYCLETYPE) & 3;
    float x0, y0, x1, y1, fs, ft, fdx, fdy;
    f3d_texrect_decode(w0, w1, &xl, &yl, &xh, &yh, &tile);
    f3d_texrect_half1_decode(R.half1, &s, &t);
    f3d_texrect_half2_decode(R.half2, &dsdx, &dtdy);
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

void f3d_set_ucode(const GfxUcode* uc) {
    s_uc = uc ? uc : &s_uc_f3d;
    s_fam = s_uc->family;
    s_vmax = s_uc->vtx_max > VC_MAX ? VC_MAX : s_uc->vtx_max < 16 ? 16 : s_uc->vtx_max;
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
    clip_rect_update();
}

static void do_light(unsigned l, uint32_t m) {   /* Light: u8 col[3], pad, colc[3], pad, s8 dir[3] */
    if (l > 8) return;
    R.light_col[l][0] = rd8(m); R.light_col[l][1] = rd8(m + 1); R.light_col[l][2] = rd8(m + 2);
    R.light_dir[l][0] = (int8_t)rd8(m + 8); R.light_dir[l][1] = (int8_t)rd8(m + 9);
    R.light_dir[l][2] = (int8_t)rd8(m + 10);
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

static int ex_cmd(uint32_t* pw0, uint32_t* pw1) {
    uint32_t w0 = *pw0, w1 = *pw1, op = w0 >> 24, n, v0;
    uint8_t a, b, c, where, q[4];
    uint16_t vi;
    if (s2dex_own(op)) {
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
            case F3DEX_G_QUAD:   /* F3D_OLD's; the line microcodes' G_LINE3D otherwise (not drawn) */
                if (old) {
                    f3dex_old_quad_decode(w1, q);
                    ex_tri(q[0], q[1], q[2]);
                    ex_tri(q[0], q[2], q[3]);
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
        case F3DEX2_G_SPECIAL_2: case F3DEX2_G_SPECIAL_3: case F3DEX2_G_LINE3D:
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
            if (idx == F3D_G_MW_SEGMENT) R.seg[(off >> 2) & 15] = w1;
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

static void run_dl(const uint32_t* pc) {
    const uint32_t* stack[DL_DEPTH];
    int sp = 0;
    for (;;) {
        uint32_t w0 = pc[0], w1 = pc[1];
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
            case F3D_G_MTX: s_mtx_loads++; do_mtx(w0, w1); break;
            case F3D_G_POPMTX:
                if (R.mv_top > 0) { R.mv_top--; R.mvp_dirty = 1; }
                break;
            case F3D_G_VTX: {
                uint32_t n, v0, len, addr;
                f3d_vtx_decode(w0, w1, &n, &v0, &len, &addr);
                s_vtx_loads++;
                do_vtx(n, v0, addr);
                break;
            }
            case F3D_G_TRI1: {
                uint8_t a, b, c, f;
                f3d_tri1_decode(w1, &a, &b, &c, &f);
                if (a < 16 && b < 16 && c < 16) do_tri(a, b, c, f);
                break;
            }
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
                if (idx == F3D_G_MW_SEGMENT) R.seg[(off >> 2) & 15] = data;
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
            case F3D_G_RDPHALF_2: R.half2 = w1; break;
            case F3D_G_RDPHALF_CONT: break;
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
                for (at -= 256, i = 0; i <= count && at + i < 256; i++) R.tlut[at + i] = rd16(src + 2 * i);
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
 * measured), 0 off (the old renderer: RDRAM keeps what the game wrote).
 * Not done: a colour image drawn and textured from within the same frame
 * (render to texture: Zelda's pause, Banjo's jigsaw) draws on the screen as
 * before (XGFX_FB_RTT is reserved for it; docs/renderer.md). */
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

/* the start of a frame: the frames read lately into RDRAM, the watch armed again */
static void fb_frame_start(void) {
    int i;
    if (!XGFX_FB_READBACK) return;
    for (i = 0; i < FBFX_SLOTS; i++) {
        const FbfxSlot* sl = fbfx_slot(i);
        int touched;
        if (!sl->addr) continue;
        touched = XGFX_FB_READBACK == 2 || fb_watch(sl, 0) > 0;
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
        xgfx_draw(&st, q, 2);
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
    s_trace = XGFX_TRACE_FRAME && s_frames == XGFX_TRACE_FRAME;
    if (XGFX_TRACE_MS) {
        static int s_traced;
        static uint64_t s_first;   /* ms from the first frame */
        if (!s_first) s_first = t0;
        if (!s_traced && t0 - s_first >= (uint64_t)XGFX_TRACE_MS * 1000u) s_traced = s_trace = 1;
    }
    DIRTY();
    run_dl((const uint32_t*)host(dl));
    flush();
    {
        uint64_t t1 = xhw_time_us();
        s_dl_us += t1 - t0;
        xhw_perf_add(XHW_PERF_DL, t1 - t0);
    }
    if (present) f3d_present_frame();
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
        uint64_t t1 = xhw_time_us();
        xgfx_present();
        xhw_perf_add(XHW_PERF_PRESENT, xhw_time_us() - t1);
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
