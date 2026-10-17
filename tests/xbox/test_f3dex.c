/* test_f3dex.c - the microcode registry and the F3DEX family in the
 * interpreter, on the host (tools/xbox/test_f3dex.py, docs/renderer.md).
 *
 * The real f3d_interp.c, texconv.c and ucode.c over a fake RDRAM (the
 * word-swapped image the runtime keeps) and a fake back end that records
 * what xgfx_draw gets. Display lists are built here with the encodings of
 * each GBI (gbi.h's macros written out), no ROM: Fast3D (Pilotwings 64's,
 * which must draw as before), F3DEX (and F3D_OLD's QUAD and CULLDL), F3DEX2
 * (renumbered commands, MTX/MOVEMEM/MOVEWORD/GEOMETRYMODE layouts, the
 * 32-entry vertex buffer, CULLDL, BRANCH_Z, POPMTX, LOAD_UCODE), and the
 * version texts the registry reads. */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "f3d_interp.h"
#include "f3dex_gbi.h"
#include "texconv.h"
#include "ucode.h"
#include "xgfx.h"
#include "xhw.h"

/* ---- the fake RDRAM: words native, halfwords at ^2, bytes at ^3 ---- */
#define RAM_SIZE (1u << 20)
static uint8_t s_ram[RAM_SIZE] __attribute__((aligned(16)));
uint8_t* xrc_rdram_host = s_ram;
uint8_t* xrc_rdram = s_ram;
uint32_t xrc_tlb_to_phys(uint32_t vaddr) { (void)vaddr; return ~0u; }   /* no TLB on the host */
uint32_t xrc_rdram_size = RAM_SIZE;

static void put32(uint32_t a, uint32_t v) { *(uint32_t*)(s_ram + (a & (RAM_SIZE - 1))) = v; }
static void put16(uint32_t a, uint16_t v) { *(uint16_t*)(s_ram + ((a ^ 2) & (RAM_SIZE - 1))) = v; }
static void put8(uint32_t a, uint8_t v) { s_ram[(a ^ 3) & (RAM_SIZE - 1)] = v; }

void xu_from_rdram(uint8_t* dst, uint32_t src, uint32_t n) {
    uint32_t i;
    for (i = 0; i < n; i++) dst[i] = s_ram[((src + i) ^ 3) & (RAM_SIZE - 1)];
}

/* ---- the fake back end ---- */
#define MAXV 600
static XgfxVtx s_v[MAXV];
static unsigned s_nv;
static XgfxOutput s_o = { 640, 480, 32, 16777215.0f, 1 };

void xgfx_output(XgfxOutput* out) { *out = s_o; }
void xgfx_frame_begin(void) {}
void xgfx_draw(const XgfxState* st, const XgfxVtx* v, uint32_t ntris) {
    (void)st;
    for (uint32_t i = 0; i < ntris * 3 && s_nv < MAXV; i++) s_v[s_nv++] = v[i];
}
void xgfx_clear(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t argb, int color, int depth, float z01) {
    (void)x0; (void)y0; (void)x1; (void)y1; (void)argb; (void)color; (void)depth; (void)z01;
}
void xgfx_present(void) {}
uint32_t xgfx_tex_create(uint32_t w, uint32_t h, const uint32_t* argb) { (void)w; (void)h; (void)argb; return 1; }
void xgfx_tex_destroy(uint32_t tex) { (void)tex; }
uint32_t xgfx_capture_last(float* u, float* v) { (void)u; (void)v; return 0; }
uint32_t xgfx_tex_linear_last(float* w, float* h) { (void)w; (void)h; return 0; }
int xgfx_target_hold(int on) { (void)on; return 0; }
uint32_t xgfx_tex_linear_hold(float* w, float* h) { (void)w; (void)h; return 0; }
void xgfx_debug_dump(void) {}
/* the readback stage (framebuffer effects): off unless a test turns it on */
static int s_stage_on, s_stage_target;
static uint32_t s_stage[640 * 480];
static unsigned s_stage_copies;
uint32_t xgfx_tex_linear_back(float* w, float* h) { *w = 640.0f; *h = 480.0f; return s_stage_on ? 7 : 0; }
int xgfx_target_stage(int on, int32_t* w, int32_t* h) {
    if (!s_stage_on) return 0;
    if (!on && s_stage_target) s_stage_copies++;
    s_stage_target = on;
    *w = 640;
    *h = 480;
    return 1;
}
const void* xgfx_stage_read(int32_t* pitch, int32_t* bpp) {
    *pitch = 640 * 4;
    *bpp = 32;
    return s_stage_on ? s_stage : NULL;
}
static uint32_t s_watch_va, s_watch_bytes;
static int s_watch_arms, s_watch_touched;
int xhw_watch_pages(uint32_t va, uint32_t bytes, int arm) {
    s_watch_va = va, s_watch_bytes = bytes;
    if (arm) s_watch_arms++;
    return arm ? (int)(bytes / 4096u) : s_watch_touched;
}
static char s_log[4096];
void xhw_log(const char* s) { (void)s; }
void xhw_logf(const char* fmt, ...) {
    va_list ap;
    size_t n = strlen(s_log);
    va_start(ap, fmt);
    if (n + 200 < sizeof s_log) {
        vsnprintf(s_log + n, sizeof s_log - n - 2, fmt, ap);
        strcat(s_log, "\n");
    }
    va_end(ap);
}
uint64_t xhw_time_us(void) { return 0; }
void xhw_perf_add(int what, uint64_t us) { (void)what; (void)us; }
void xhw_panel_render(uint32_t* img, int pitch_px, const xhw_panel* p) { (void)img; (void)pitch_px; (void)p; }
int wx_game_state(void) { return -1; }
void wx_frame_overlay(void) {}

/* ---- checks ---- */
static int s_fail;
#define CHECK(c, ...)                                         \
    do {                                                      \
        if (!(c)) {                                           \
            s_fail++;                                         \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);      \
            printf(__VA_ARGS__);                              \
            printf("\n");                                     \
        }                                                     \
    } while (0)

/* ---- building lists ---- */
static uint32_t s_pc;   /* where the next command goes */
static void cmd(uint32_t w0, uint32_t w1) { put32(s_pc, w0); put32(s_pc + 4, w1); s_pc += 8; }

#define MTX_AT 0x10000u    /* diag 1/256: object units 256 = 1 in clip space */
#define MTXI_AT 0x10040u   /* identity */
#define VP_AT 0x10080u
#define VTX_AT 0x11000u
#define DL_AT 0x20000u
#define SUB_AT 0x21000u

static void mtx(uint32_t a, int32_t diag16) {   /* s15.16 on the diagonal, w 1 */
    int e;
    for (e = 0; e < 16; e++) {
        int32_t v = (e % 5 == 0) ? (e == 15 ? 0x10000 : diag16) : 0;
        put16(a + 2u * e, (uint16_t)((uint32_t)v >> 16));
        put16(a + 32u + 2u * e, (uint16_t)v);
    }
}

static void vtx(unsigned i, int x, int y, int z) {   /* Vtx: s16 ob[3], flag, s16 tc[2], u8 cn[4] */
    uint32_t a = VTX_AT + 16u * i;
    put16(a, (uint16_t)x); put16(a + 2, (uint16_t)y); put16(a + 4, (uint16_t)z); put16(a + 6, 0);
    put16(a + 8, 0); put16(a + 10, 0);
    put8(a + 12, 255); put8(a + 13, 128); put8(a + 14, 64); put8(a + 15, 255);
}

static void setup_data(void) {
    unsigned i;
    mtx(MTX_AT, 256);        /* 1/256 */
    mtx(MTXI_AT, 0x10000);   /* 1 */
    /* Vp { 640, 480, 511, 0 }, { 640, 480, 511, 0 }: the full 320x240 screen */
    put16(VP_AT, 640); put16(VP_AT + 2, 480); put16(VP_AT + 4, 511); put16(VP_AT + 6, 0);
    put16(VP_AT + 8, 640); put16(VP_AT + 10, 480); put16(VP_AT + 12, 511); put16(VP_AT + 14, 0);
    /* 0..39: a triangle fan of small counter-clockwise triangles across the screen */
    for (i = 0; i < 40; i++) vtx(i, -200 + (int)(i % 8) * 50, -100 + (int)(i / 8) * 40, 0);
    /* 40..43: all right of the screen (x = 2) */
    for (i = 40; i < 44; i++) vtx(i, 512 + (int)i, (int)i, 0);
}

/* a vertex as the interpreter put it out: framebuffer pixels (640x480 = 2x the N64's 320x240) */
static float sx(int x) { return ((float)x / 256.0f * 160.0f + 160.0f) * 2.0f; }
static float sy(int y) { return (120.0f - (float)y / 256.0f * 120.0f) * 2.0f; }
static int near(float a, float b) { return fabsf(a - b) < 0.6f; }

static unsigned run(const GfxUcode* uc) {
    s_nv = 0;
    f3d_set_ucode(uc);
    f3d_run(DL_AT, 1);
    return s_nv / 3;
}

static GfxUcode uc_of(const char* text) {
    GfxUcode u;
    gfx_ucode_parse((const uint8_t*)text, strlen(text), &u);
    return u;
}

/* ------------------------------------------------------------ the tests */

static void test_parse(void) {
    static const struct { const char* text; int fam, vmax, flags, found; } k[] = {
        { "xx\0RSP SW Version: 2.0D, 04-01-96\0", GFX_UC_F3D, 16, 0, 1 },
        { "RSP Gfx ucode F3DEX         0.95 Yoshitaka Yasumoto Nintendo.", GFX_UC_F3DEX, 32, GFX_UC_F_OLD, 1 },
        { "\x01\x02RSP Gfx ucode F3DEX       fifo 1.23 Yoshitaka Yasumoto 1997 Nintendo.", GFX_UC_F3DEX, 32, 0, 1 },
        { "RSP Gfx ucode F3DLX.Rej   fifo 1.23 Yoshitaka Yasumoto 1997 Nintendo.", GFX_UC_F3DEX, 64, 0, 1 },
        { "RSP Gfx ucode F3DLP.Rej   fifo 1.23 Yoshitaka Yasumoto 1997 Nintendo.", GFX_UC_F3DEX, 80, 0, 1 },
        { "RSP Gfx ucode L3DEX       fifo 1.23 Yoshitaka Yasumoto 1997 Nintendo.", GFX_UC_F3DEX, 32, GFX_UC_F_LINE, 1 },
        { "RSP Gfx ucode F3DEX.NoN   fifo 2.08  Yoshitaka Yasumoto 1999 Nintendo.", GFX_UC_F3DEX2, 32, 0, 1 },
        { "RSP Gfx ucode F3DZEX.NoN  fifo 2.06H Yoshitaka Yasumoto 1998 Nintendo.", GFX_UC_F3DEX2, 32, 0, 1 },
        { "RSP Gfx ucode F3DEX.Rej   xbus 2.07  Yoshitaka Yasumoto 1998 Nintendo.", GFX_UC_F3DEX2, 64, 0, 1 },
        { "RSP Gfx ucode S2DEX  fifo 1.07 Yoshitaka Yasumoto 1997 Nintendo.", GFX_UC_S2DEX, 32, 0, 1 },
        { "RSP Gfx ucode S2DEX       fifo 2.05  Yoshitaka Yasumoto 1998 Nintendo.", GFX_UC_S2DEX2, 32, 0, 1 },
        { "RSP Gfx[Safe] S2DEX       fifo 2.05  Yoshitaka Yasumoto 1998 Nintendo.", GFX_UC_S2DEX2, 32, 0, 1 },
        { "nothing to see", GFX_UC_F3D, 16, 0, 0 },
    };
    unsigned i;
    for (i = 0; i < sizeof k / sizeof k[0]; i++) {
        GfxUcode u;
        size_t n = strlen(k[i].text);
        if (k[i].text[0] == 'x') n = 36;   /* the one with a NUL before the text */
        int f = gfx_ucode_parse((const uint8_t*)k[i].text, n, &u);
        CHECK(f == k[i].found && u.family == k[i].fam && u.vtx_max == k[i].vmax && u.flags == k[i].flags,
              "parse %u: found %d family %d vtx %d flags %d", i, f, u.family, u.vtx_max, u.flags);
    }
    {
        GfxUcode u = uc_of("RSP Gfx ucode F3DEX       fifo 1.23 Yoshitaka Yasumoto 1997 Nintendo.");
        CHECK(!strncmp(u.name, "RSP Gfx ucode F3DEX", 19), "name '%s'", u.name);
    }
}

static void test_decoders(void) {
    uint32_t n, v0;
    uint8_t a, b, c, vs, ve, where, q[4];
    uint16_t vi;
    /* gsSPVertex(v, 4, 20): F3DEX (n << 10 | 16n-1, v0*2), F3DEX2 (n << 12 | (v0+n) << 1) */
    f3dex_vtx_decode(0x04000000u | (40u << 16) | (4u << 10) | 63u, &n, &v0);
    CHECK(n == 4 && v0 == 20, "f3dex vtx %u %u", n, v0);
    f3dex2_vtx_decode(0x01000000u | (4u << 12) | (24u << 1), &n, &v0);
    CHECK(n == 4 && v0 == 20, "f3dex2 vtx %u %u", n, v0);
    f3dex2_vtx_decode(0x01000000u | (32u << 12) | (32u << 1), &n, &v0);
    CHECK(n == 32 && v0 == 0, "f3dex2 vtx 32 %u %u", n, v0);
    f3dex_tri_decode((62u << 16) | (2u << 8) | 0u, &a, &b, &c);
    CHECK(a == 31 && b == 1 && c == 0, "tri %u %u %u", a, b, c);
    f3dex_old_quad_decode((6u << 24) | (0u << 16) | (2u << 8) | 4u, q);
    CHECK(q[0] == 0 && q[1] == 1 && q[2] == 2 && q[3] == 3, "old quad");
    f3dex_culldl_decode(0xBE000000u | 0u, 31u * 2u, 0, &vs, &ve);
    CHECK(vs == 0 && ve == 31, "culldl %u %u", vs, ve);
    f3dex_culldl_decode(0xBE000000u | (2u * 40u), ((7u + 1u) & 15u) * 40u, 1, &vs, &ve);
    CHECK(vs == 2 && ve == 7, "culldl old %u %u", vs, ve);
    f3dex_modifyvtx_decode(0x02000000u | (0x14u << 16) | (5u * 2u), &where, &vi);
    CHECK(where == 0x14 && vi == 5, "modifyvtx");
    CHECK(f3dex_branchz_vtx(0xB0000000u | ((7u * 5u) << 12) | (7u * 2u)) == 7, "branchz vtx");
    /* gsSPMatrix(m, G_MTX_PROJECTION | G_MTX_LOAD | G_MTX_NOPUSH): stored (p ^ PUSH) */
    CHECK(f3dex2_mtx_params(0xDA380007u) == (0x01 | 0x02), "mtx proj load %02x", f3dex2_mtx_params(0xDA380007u));
    CHECK(f3dex2_mtx_params(0xDA380000u) == 0x04, "mtx mv mul push %02x", f3dex2_mtx_params(0xDA380000u));
    CHECK(f3dex2_geom_to_f3d(0x00200405u) == (0x200u | 0x2000u | 0x5u), "geom %08x", f3dex2_geom_to_f3d(0x00200405u));
    CHECK(f3dex2_geom_apply(0x00000405u, 0x00FFFBFFu, 0x00010000u) == 0x00010005u, "geom apply");
    /* gsSPSetOtherMode(G_SETOTHERMODE_H, G_MDSFT_TEXTLUT 14, 2, ...): (32-14-2) << 8 | 1 */
    CHECK(f3dex2_othermode_to_f3d((16u << 8) | 1u) == ((14u << 8) | 2u), "othermode");
    {
        uint8_t idx;
        uint32_t ofs, len;
        f3dex2_movemem_decode(0xDC000000u | ((16u - 1u) / 8u) << 19 | 0u << 8 | 8u, &idx, &ofs, &len);
        CHECK(idx == 8 && ofs == 0 && len == 16, "movemem vp");
        f3dex2_movemem_decode(0xDC000000u | ((16u - 1u) / 8u) << 19 | (48u / 8u) << 8 | 10u, &idx, &ofs, &len);
        CHECK(idx == 10 && f3dex2_light_index(ofs) == 0, "movemem light 1");
    }
    CHECK(f3dex2_numlights_from_data(2u * 24u) == 2, "numlights");
    CHECK(f3dex2_lightcol_index(0x18) == 1 && f3dex2_lightcol_index(0x1c) == 0xFF, "lightcol");
    CHECK(f3dex2_texture_on(0xD7000002u) == 1, "texture on");
    CHECK(f3dex2_popmtx_count(128) == 2, "popmtx");
}

/* the frame setup every list starts with: matrices, viewport, smooth shade, no cull */
static void head_f3d(void) {
    cmd(0x01000000u | (0x03u << 16) | 64u, MTX_AT);    /* G_MTX projection | load */
    cmd(0x01000000u | (0x02u << 16) | 64u, MTXI_AT);   /* modelview | load */
    cmd(0x03000000u | (0x80u << 16) | 16u, VP_AT);     /* G_MOVEMEM G_MV_VIEWPORT */
    cmd(0xB6000000u, 0xFFFFFFFFu);                     /* G_CLEARGEOMETRYMODE */
    cmd(0xB7000000u, 0x00000204u);                     /* G_SETGEOMETRYMODE SHADE | SHADING_SMOOTH */
}

static void head_ex2(void) {
    cmd(0xDA380007u, MTX_AT);    /* G_MTX projection | load | nopush (stored ^ PUSH) */
    cmd(0xDA380003u, MTXI_AT);   /* modelview | load | nopush */
    cmd(0xDC080008u, VP_AT);     /* G_MOVEMEM 16 bytes, G_MV_VIEWPORT */
    cmd(0xD9000000u, 0x00200004u);   /* G_GEOMETRYMODE: clear all, set SHADE | SHADING_SMOOTH */
}

static void test_fast3d(void) {
    unsigned t;
    s_pc = DL_AT;
    head_f3d();
    cmd(0x04000000u | ((3u - 1u) << 20) | (0u << 16) | 48u, VTX_AT);   /* G_VTX 3 at 0 */
    cmd(0xBF000000u, (0u << 16) | (10u << 8) | 20u);                 /* G_TRI1 0 1 2 (x10) */
    cmd(0xB8000000u, 0);
    t = run(NULL);
    CHECK(t == 1, "fast3d tris %u", t);
    if (t == 1) {
        CHECK(near(s_v[0].x, sx(-200)) && near(s_v[0].y, sy(-100)), "fast3d v0 %.1f %.1f (want %.1f %.1f)",
              s_v[0].x, s_v[0].y, sx(-200), sy(-100));
        CHECK(near(s_v[1].x, sx(-150)) && near(s_v[2].x, sx(-100)), "fast3d v1 v2 x %.1f %.1f", s_v[1].x, s_v[2].x);
        CHECK(s_v[0].rgba[0] == 255 && s_v[0].rgba[1] == 128 && s_v[0].rgba[2] == 64, "fast3d shade %u %u %u",
              s_v[0].rgba[0], s_v[0].rgba[1], s_v[0].rgba[2]);
    }
}

static void test_f3dex(void) {
    GfxUcode u = uc_of("RSP Gfx ucode F3DEX       fifo 1.23 Yoshitaka Yasumoto 1997 Nintendo.");
    unsigned t;
    s_pc = DL_AT;
    head_f3d();
    cmd(0x04000000u | (40u << 16) | (32u << 10) | (32u * 16u - 1u), VTX_AT);   /* 32 at 20? no: v0*2 = 40 -> 20 */
    /* the 32-entry buffer: 20..31 hold vertices 0..11 (the rest past the buffer are dropped) */
    cmd(0xB1000000u | (40u << 16) | (42u << 8) | 56u, (40u << 16) | (56u << 8) | 54u);   /* TRI2 20 21 28, 20 28 27 */
    cmd(0xBF000000u, (62u << 16) | (60u << 8) | 52u);   /* TRI1 31 30 26 */
    cmd(0xBE000000u | 40u, 62u);                        /* CULLDL 20..31: on screen, go on */
    cmd(0x04000000u | (0u << 16) | (4u << 10) | 63u, VTX_AT + 40u * 16u);   /* 4 at 0: the off-screen ones */
    cmd(0xBE000000u | 0u, 6u);                          /* CULLDL 0..3: all right of the screen, end */
    cmd(0xBF000000u, (0u << 16) | (2u << 8) | 4u);      /* not reached */
    cmd(0xB8000000u, 0);
    t = run(&u);
    CHECK(t == 3, "f3dex tris %u (want 3: TRI2 + TRI1, the culled list's TRI1 skipped)", t);
    if (t >= 1) CHECK(near(s_v[0].x, sx(-200)) && near(s_v[1].x, sx(-150)), "f3dex v %.1f %.1f", s_v[0].x, s_v[1].x);

    /* BRANCH_Z: RDPHALF_1 = the list, zval past the far plane: taken */
    s_pc = SUB_AT;
    cmd(0xBF000000u, (0u << 16) | (2u << 8) | 18u);   /* TRI1 0 1 9 */
    cmd(0xB8000000u, 0);
    s_pc = DL_AT;
    head_f3d();
    cmd(0x04000000u | (0u << 16) | (10u << 10) | 159u, VTX_AT);
    cmd(0xB4000000u, SUB_AT);
    cmd(0xB0000000u | ((3u * 5u) << 12) | 6u, 0xFFFFFFFFu);
    cmd(0xBF000000u, (0u << 16) | (2u << 8) | 4u);    /* not reached: the branch does not return */
    cmd(0xB8000000u, 0);
    t = run(&u);
    CHECK(t == 1, "f3dex branch_z taken: tris %u", t);
    put32(DL_AT + 7 * 8 + 4, 0);   /* zval 0: not taken */
    t = run(&u);
    CHECK(t == 1 && s_nv >= 3 && near(s_v[2].x, sx(-100)), "f3dex branch_z not taken: tris %u", t);

    /* F3D_OLD (MK64's 0.95): QUAD 0xB5, CULLDL in Fast3D's units */
    u = uc_of("RSP Gfx ucode F3DEX         0.95 Yoshitaka Yasumoto Nintendo.");
    s_pc = DL_AT;
    head_f3d();
    cmd(0x04000000u | (0u << 16) | (16u << 10) | 255u, VTX_AT);
    cmd(0xB5000000u, (18u << 24) | (0u << 16) | (2u << 8) | 16u);   /* QUAD 0 1 8 9 */
    cmd(0xBE000000u | 0u, ((15u + 1u) & 15u) * 40u);                 /* CULLDL 0..15 on screen */
    cmd(0xB2000000u, 0);                                             /* RDPHALF_CONT: nothing */
    cmd(0xB8000000u, 0);
    t = run(&u);
    CHECK(t == 2, "f3dex old quad tris %u", t);
}

static void test_f3dex2(void) {
    GfxUcode u = uc_of("RSP Gfx ucode F3DEX.NoN   fifo 2.08  Yoshitaka Yasumoto 1999 Nintendo.");
    unsigned t;
    s_pc = SUB_AT;   /* a called list: a pushed matrix, two triangles, a pop */
    cmd(0xDA380000u, MTXI_AT);   /* modelview | mul | push */
    cmd(0x06000000u | (0u << 16) | (2u << 8) | 16u, (2u << 16) | (16u << 8) | 18u);   /* TRI2 0 1 8, 1 8 9 */
    cmd(0xD8380002u, 64);        /* POPMTX 1 */
    cmd(0xDF000000u, 0);
    s_pc = DL_AT;
    head_ex2();
    cmd(0xDB060018u, VTX_AT);                                   /* MOVEWORD SEGMENT 6 = the vertices */
    cmd(0x01000000u | (32u << 12) | (32u << 1), 0x06000000u);   /* VTX 32 at 0, segmented */
    cmd(0x05000000u | (0u << 16) | (2u << 8) | 16u, 0);          /* TRI1 0 1 8 */
    cmd(0xDE000000u, SUB_AT);                                   /* DL (push) */
    cmd(0xD9FFFFFFu, 0x00000400u);                              /* set CULL_BACK */
    cmd(0x05000000u | (0u << 16) | (16u << 8) | 2u, 0);          /* TRI1 0 8 1: clockwise, culled */
    cmd(0x07000000u | (4u << 16) | (6u << 8) | 22u, (4u << 16) | (22u << 8) | 20u);   /* QUAD 2 3 11 10 */
    cmd(0x03000000u | 0u, 62u);                                 /* CULLDL 0..31 on screen */
    cmd(0x01000000u | (4u << 12) | (4u << 1), 0x06000000u + 40u * 16u);   /* the off-screen 4 at 0 */
    cmd(0x03000000u | 0u, 6u);                                  /* CULLDL 0..3: end */
    cmd(0x05000000u | (0u << 16) | (2u << 8) | 4u, 0);
    cmd(0xDF000000u, 0);
    t = run(&u);
    CHECK(t == 5, "f3dex2 tris %u (want 5: TRI1, the called TRI2, the QUAD's two; the clockwise one culled)", t);
    if (t >= 1) {
        CHECK(near(s_v[0].x, sx(-200)) && near(s_v[0].y, sy(-100)) && near(s_v[2].y, sy(-60)),
              "f3dex2 v0 %.1f %.1f, v2 y %.1f", s_v[0].x, s_v[0].y, s_v[2].y);
    }

    /* LOAD_UCODE: an F3DEX list switches to F3DEX2 by the data segment's text */
    {
        const char* txt = "RSP Gfx ucode F3DEX.NoN   fifo 2.08  Yoshitaka Yasumoto 1999 Nintendo.";
        GfxUcode ex = uc_of("RSP Gfx ucode F3DEX       fifo 1.23 Yoshitaka Yasumoto 1997 Nintendo.");
        uint32_t d = 0x30000u, i;
        for (i = 0; i < 0x800; i++) put8(d + i, 0);
        for (i = 0; txt[i]; i++) put8(d + 0x100 + i, (uint8_t)txt[i]);
        s_pc = DL_AT;
        head_f3d();
        cmd(0xB4000000u, d);                 /* RDPHALF_1 = data */
        cmd(0xAF000000u | 0x7FFu, 0x40000u); /* LOAD_UCODE */
        cmd(0x01000000u | (3u << 12) | (3u << 1), VTX_AT);
        cmd(0x05000000u | (0u << 16) | (2u << 8) | 4u, 0);
        cmd(0xDF000000u, 0);
        s_log[0] = 0;
        t = run(&ex);
        CHECK(t == 1, "load_ucode: tris %u", t);
        CHECK(strstr(s_log, "[GFX] ucode RSP Gfx ucode F3DEX.NoN") && strstr(s_log, "F3DEX2"), "load_ucode log: %s", s_log);
    }
}

/* CI4 and CI8 through a TLUT (MK64's textures; G_LOADTLUT) */
static void test_ci(void) {
    uint16_t tlut[256];
    uint8_t ci4[8] = { 0x01, 0x23, 0, 0, 0, 0, 0, 0 }, ci8[8] = { 0, 17, 255, 0, 0, 0, 0, 0 };
    uint32_t out[4];
    unsigned i;
    for (i = 0; i < 256; i++) tlut[i] = (uint16_t)((i << 8) | 1u);   /* RGBA16: r = i >> 3, opaque */
    tlut[16 + 2] = 0xF801;   /* palette 1, entry 2: red */
    tlut[0x44] = 0x80FF;     /* IA16: i 0x80, a 0xFF */
    CHECK(texconv_ci_to_argb8(ci4, 8, 4, 1, N64SIZ_4b, tlut, 16, 0, out, 4, 1), "ci4");
    CHECK(out[2] == 0xFFFF0000u, "ci4 palette 1 entry 2: %08x", out[2]);
    CHECK(texconv_ci_to_argb8(ci8, 8, 3, 1, N64SIZ_8b, tlut, 0, 0, out, 4, 1), "ci8");
    CHECK(out[3] == out[2], "ci8 edge replicated");
    ci8[0] = 0x44;
    CHECK(texconv_ci_to_argb8(ci8, 8, 1, 1, N64SIZ_8b, tlut, 0, 1, out, 1, 1) && out[0] == 0xFF808080u, "ci8 ia16 %08x",
          out[0]);
    CHECK(!texconv_ci_to_argb8(ci8, 8, 1, 1, N64SIZ_16b, tlut, 0, 0, out, 1, 1), "ci16 refused");
}

/* framebuffer readback (f3d_interp.c "framebuffer effects", fbfx.c): a frame
 * drawn into a 320x240 RGBA16 colour image is copied to the stage at its
 * end; at the next frame's start its pages are looked at and armed again,
 * and only a frame some thread touched is converted into RDRAM
 * (word-swapped RGBA5551) */
#define CIMG_AT 0x40000u
static void readback_frame(const GfxUcode* u) {
    s_pc = DL_AT;
    cmd(0xFF000000u | (2u << 19) | (320u - 1u), CIMG_AT);   /* G_SETCIMG RGBA16 320 */
    cmd(0xED000000u, (320u << 14) | (240u << 2));            /* G_SETSCISSOR 0,0 320,240 */
    cmd(0xB8000000u, 0);
    run(u);
}
/* RGBA32 dxt 0: an odd row's texel i is texel i ^ 2 (8-byte halves of 16-byte groups); even rows stay */
static void test_deswizzle32(void) {
    uint8_t d[64], want[64];
    unsigned i;
    for (i = 0; i < 64; i++) d[i] = want[i] = (uint8_t)i;
    for (i = 32; i < 64; i++) want[i] = (uint8_t)(((i - 32) ^ 8) + 32);
    texconv_deswizzle32(d, 32, 2);
    CHECK(!memcmp(d, want, sizeof d), "deswizzle32: byte 32 is %u, 40 is %u", d[32], d[40]);
}

static void test_readback(void) {
    GfxUcode u = uc_of("RSP SW Version: 2.0D, 04-01-96");
    unsigned i;
    uint32_t host0 = (uint32_t)(uintptr_t)s_ram, px = CIMG_AT + 2u * (5u * 320u + 7u);
    s_stage_on = 1;
    for (i = 0; i < 640u * 480u; i++) s_stage[i] = 0xFF000000u | ((i % 640u) < 320u && (i / 640u) < 240u ? 0xFF0000u : 0x0000FFu);
    s_stage_copies = 0;
    readback_frame(&u);
    CHECK(s_stage_copies == 1, "readback: %u stage copies after one frame", s_stage_copies);
    CHECK(s_v[s_nv - 1].x == 320.0f && s_v[s_nv - 1].y == 240.0f, "readback: the copy quad ends at %g,%g", s_v[s_nv - 1].x,
          s_v[s_nv - 1].y);
    /* untouched: armed, not converted */
    put16(px, 0x1234);
    s_watch_touched = 0;
    s_watch_arms = 0;
    readback_frame(&u);
    CHECK(s_watch_arms == 1 && s_watch_va == host0 + CIMG_AT && s_watch_bytes == 320u * 240u * 2u,
          "readback: arms %d va %08x bytes %u", s_watch_arms, (unsigned)(s_watch_va - host0), (unsigned)s_watch_bytes);
    CHECK(*(uint16_t*)(s_ram + (px ^ 2u)) == 0x1234u, "readback: an untouched frame was converted");
    /* touched: converted at the next frame's start; red in RGBA5551 is F801 */
    s_watch_touched = 3;
    readback_frame(&u);
    CHECK(*(uint16_t*)(s_ram + (px ^ 2u)) == 0xF801u, "readback: pixel %04x", *(uint16_t*)(s_ram + (px ^ 2u)));
    /* still hot without touches: converted again */
    put16(px, 0x1234);
    s_watch_touched = 0;
    readback_frame(&u);
    CHECK(*(uint16_t*)(s_ram + (px ^ 2u)) == 0xF801u, "readback: a hot frame was not converted again");
    s_stage_on = 0;
}

int main(void) {
    test_ci();
    setup_data();
    test_parse();
    test_decoders();
    test_fast3d();
    test_f3dex();
    test_f3dex2();
    test_fast3d();   /* and Fast3D again after the others: nothing left over */
    test_deswizzle32();
    test_readback();
    if (s_fail) printf("test_f3dex: %d failed\n", s_fail);
    return s_fail ? 1 : 0;
}
