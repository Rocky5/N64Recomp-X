/*
 * f3dex_gbi.h - the F3DEX (F3DEX_GBI) and F3DEX2 (F3DEX_GBI_2) GBI: the
 * opcodes and field layouts where they differ from Fast3D (f3d_gbi.h),
 * and the decoders the interpreter uses for them.
 *
 * Source of truth: gbi.h as the mk64 decomp carries it (F3DEX_GBI with and
 * without F3D_OLD, F3DEX_GBI_2) and papermario's (F3DEX_GBI_2); read, not
 * copied. Everything not listed here (the RDP commands 0xC0..0xFF but
 * F3DEX2's 0xE0..0xE3 and 0xF1, the segment table, the texture loads) is
 * Fast3D's. Names carry F3DEX_ / F3DEX2_ prefixes so this header sits next
 * to f3d_gbi.h. Only <stdint.h> is included; static inline, host-tested
 * (tests/xbox/test_f3dex.c).
 */
#ifndef XRC_GFX_F3DEX_GBI_H
#define XRC_GFX_F3DEX_GBI_H

#include <stdint.h>

/* ---- F3DEX 0.9x/1.x: Fast3D's numbering, plus (gbi.h G_IMMFIRST-n) ---- */
#define F3DEX_G_VTX           0x04 /* w0 = v0*2 <<16 | n <<10 | n*16-1 */
#define F3DEX_G_TRI1          0xBF /* w1 = indices x2 (flag rotates them) */
#define F3DEX_G_CULLDL        0xBE /* w0 = vstart*2, w1 = vend*2 (F3D_OLD: x40, vend+1) */
#define F3DEX_G_QUAD          0xB5 /* F3D_OLD only: w1 = v3 v0 v1 v2 (x2) */
#define F3DEX_G_RDPHALF_1     0xB4
#define F3DEX_G_RDPHALF_2     0xB3
#define F3DEX_G_MODIFYVTX     0xB2 /* F3D_OLD: G_RDPHALF_CONT */
#define F3DEX_G_TRI2          0xB1 /* w0 low 24 = triangle 1, w1 = triangle 2; also gSP1Quadrangle */
#define F3DEX_G_BRANCH_Z      0xB0 /* w0 = vtx*5 <<12 | vtx*2, w1 = zval; RDPHALF_1 = the list */
#define F3DEX_G_LOAD_UCODE    0xAF /* w0 = dsize-1, w1 = text; RDPHALF_1 = data */

/* ---- F3DEX2 (gbi.h F3DEX_GBI_2) ---- */
#define F3DEX2_G_NOOP         0x00
#define F3DEX2_G_VTX          0x01
#define F3DEX2_G_MODIFYVTX    0x02
#define F3DEX2_G_CULLDL       0x03
#define F3DEX2_G_BRANCH_Z     0x04
#define F3DEX2_G_TRI1         0x05 /* indices x2 in w0 */
#define F3DEX2_G_TRI2         0x06
#define F3DEX2_G_QUAD         0x07
#define F3DEX2_G_LINE3D       0x08
#define F3DEX2_G_SPECIAL_3    0xD3
#define F3DEX2_G_SPECIAL_2    0xD4
#define F3DEX2_G_SPECIAL_1    0xD5
#define F3DEX2_G_DMA_IO       0xD6
#define F3DEX2_G_TEXTURE      0xD7
#define F3DEX2_G_POPMTX       0xD8
#define F3DEX2_G_GEOMETRYMODE 0xD9
#define F3DEX2_G_MTX          0xDA
#define F3DEX2_G_MOVEWORD     0xDB
#define F3DEX2_G_MOVEMEM      0xDC
#define F3DEX2_G_LOAD_UCODE   0xDD
#define F3DEX2_G_DL           0xDE
#define F3DEX2_G_ENDDL        0xDF
#define F3DEX2_G_SPNOOP       0xE0
#define F3DEX2_G_RDPHALF_1    0xE1
#define F3DEX2_G_SETOTHERMODE_L 0xE2
#define F3DEX2_G_SETOTHERMODE_H 0xE3
#define F3DEX2_G_RDPHALF_2    0xF1

/* F3DEX2 G_MTX params (w0 low byte, PUSH stored inverted) */
#define F3DEX2_G_MTX_PUSH       0x01
#define F3DEX2_G_MTX_LOAD       0x02
#define F3DEX2_G_MTX_PROJECTION 0x04

/* F3DEX2 G_MOVEMEM indices */
#define F3DEX2_G_MV_MMTX     2
#define F3DEX2_G_MV_PMTX     6
#define F3DEX2_G_MV_VIEWPORT 8
#define F3DEX2_G_MV_LIGHT    10   /* offset 0 lookat x, 24 lookat y, 24*(n+1) light n (1-based) */
#define F3DEX2_G_MV_POINT    12
#define F3DEX2_G_MV_MATRIX   14

/* F3DEX2 geometry mode bits that moved (the rest are Fast3D's) */
#define F3DEX2_G_CULL_FRONT     0x00000200
#define F3DEX2_G_CULL_BACK      0x00000400
#define F3DEX2_G_SHADING_SMOOTH 0x00200000

/* gSPModifyVertex's `where` (both GBIs) */
#define F3DEX_G_MWO_POINT_RGBA     0x10
#define F3DEX_G_MWO_POINT_ST       0x14
#define F3DEX_G_MWO_POINT_XYSCREEN 0x18
#define F3DEX_G_MWO_POINT_ZSCREEN  0x1C

/* ---- decoders ---- */

/* A triangle's three indices, x2 in bits 16..23, 8..15, 0..7 (TRI1's w1 in
 * F3DEX, w0 in F3DEX2, either half of TRI2 and QUAD). */
static inline void f3dex_tri_decode(uint32_t w, uint8_t* a, uint8_t* b, uint8_t* c) {
    *a = (uint8_t)(((w >> 16) & 0xFFu) >> 1);
    *b = (uint8_t)(((w >> 8) & 0xFFu) >> 1);
    *c = (uint8_t)((w & 0xFFu) >> 1);
}

/* F3D_OLD's G_QUAD: w1 = v3 v0 v1 v2 (x2): triangles v0 v1 v2 and v0 v2 v3 */
static inline void f3dex_old_quad_decode(uint32_t w1, uint8_t v[4]) {
    v[0] = (uint8_t)(((w1 >> 16) & 0xFFu) >> 1);
    v[1] = (uint8_t)(((w1 >> 8) & 0xFFu) >> 1);
    v[2] = (uint8_t)((w1 & 0xFFu) >> 1);
    v[3] = (uint8_t)((w1 >> 24) >> 1);
}

static inline void f3dex_vtx_decode(uint32_t w0, uint32_t* n, uint32_t* v0) {
    *n = (w0 >> 10) & 0x3Fu;
    *v0 = ((w0 >> 16) & 0xFFu) >> 1;
}

/* F3DEX2 G_VTX: w0 = n <<12 | (v0+n) <<1 */
static inline void f3dex2_vtx_decode(uint32_t w0, uint32_t* n, uint32_t* v0) {
    *n = (w0 >> 12) & 0xFFu;
    *v0 = (((w0 >> 1) & 0x7Fu) - *n) & 0xFFu;
}

/* G_CULLDL: the vertex range to test (inclusive) */
static inline void f3dex_culldl_decode(uint32_t w0, uint32_t w1, int old, uint8_t* vs, uint8_t* ve) {
    if (old) {   /* F3D_OLD: vstart*40, (vend+1)*40, both & 15 */
        *vs = (uint8_t)((w0 & 0xFFFFu) / 40u);
        *ve = (uint8_t)(((w1 / 40u) + 15u) & 0xFu);
    } else {
        *vs = (uint8_t)((w0 & 0xFFFFu) >> 1);
        *ve = (uint8_t)((w1 & 0xFFFFu) >> 1);
    }
}

/* G_MODIFYVTX (both GBIs): w0 = where <<16 | vtx*2 */
static inline void f3dex_modifyvtx_decode(uint32_t w0, uint8_t* where, uint16_t* vtx) {
    *where = (uint8_t)((w0 >> 16) & 0xFFu);
    *vtx = (uint16_t)((w0 & 0xFFFFu) >> 1);
}

/* G_BRANCH_Z (both GBIs): the vertex, from the x2 field */
static inline uint16_t f3dex_branchz_vtx(uint32_t w0) { return (uint16_t)((w0 & 0xFFFu) >> 1); }

/* F3DEX2 G_MTX params -> Fast3D's (PROJECTION 1, LOAD 2, PUSH 4) */
static inline uint8_t f3dex2_mtx_params(uint32_t w0) {
    uint32_t p = (w0 & 0xFFu) ^ F3DEX2_G_MTX_PUSH;
    return (uint8_t)(((p & F3DEX2_G_MTX_PROJECTION) ? 0x01u : 0u) | (p & F3DEX2_G_MTX_LOAD) |
                     ((p & F3DEX2_G_MTX_PUSH) ? 0x04u : 0u));
}

/* F3DEX2 geometry mode -> Fast3D's bits (the interpreter keeps Fast3D's) */
static inline uint32_t f3dex2_geom_to_f3d(uint32_t g) {
    uint32_t o = g & ~(F3DEX2_G_CULL_FRONT | F3DEX2_G_CULL_BACK | F3DEX2_G_SHADING_SMOOTH | 0x00001000u | 0x00002000u);
    if (g & F3DEX2_G_CULL_FRONT) o |= 0x00001000u;
    if (g & F3DEX2_G_CULL_BACK) o |= 0x00002000u;
    if (g & F3DEX2_G_SHADING_SMOOTH) o |= 0x00000200u;
    return o;
}

/* F3DEX2 G_GEOMETRYMODE: w0 low 24 = ~clear, w1 = set */
static inline uint32_t f3dex2_geom_apply(uint32_t g, uint32_t w0, uint32_t w1) {
    return (g & (w0 | 0xFF000000u)) | w1;
}

/* F3DEX2 G_SETOTHERMODE_*: w0 = (32-sft-len) <<8 | len-1 -> Fast3D's sft <<8 | len */
static inline uint32_t f3dex2_othermode_to_f3d(uint32_t w0) {
    uint32_t len = (w0 & 0xFFu) + 1u, sft = 32u - ((w0 >> 8) & 0xFFu) - len;
    return ((sft & 0xFFu) << 8) | (len & 0xFFu);
}

/* F3DEX2 G_MOVEMEM: w0 = (len-1)/8 <<19 | ofs/8 <<8 | idx */
static inline void f3dex2_movemem_decode(uint32_t w0, uint8_t* idx, uint32_t* ofs, uint32_t* len) {
    *idx = (uint8_t)(w0 & 0xFFu);
    *ofs = ((w0 >> 8) & 0xFFu) * 8u;
    *len = (((w0 >> 19) & 0x1Fu) + 1u) * 8u;
}

/* F3DEX2 G_MOVEWORD: w0 = index <<16 | offset */
static inline void f3dex2_moveword_decode(uint32_t w0, uint8_t* idx, uint16_t* off) {
    *idx = (uint8_t)((w0 >> 16) & 0xFFu);
    *off = (uint16_t)(w0 & 0xFFFFu);
}

static inline uint8_t f3dex2_numlights_from_data(uint32_t d) { return (uint8_t)(d / 24u); }
/* G_MW_LIGHTCOL: aLIGHT_n = (n-1)*0x18; 0-based index, 0xFF for a bLIGHT or other word */
static inline uint8_t f3dex2_lightcol_index(uint16_t off) { return (off % 24u) ? 0xFFu : (uint8_t)(off / 24u); }
/* G_MOVEMEM G_MV_LIGHT: 0-based light index, 0xFF for the lookat slots */
static inline uint8_t f3dex2_light_index(uint32_t ofs) { return ofs < 48u ? 0xFFu : (uint8_t)(ofs / 24u - 2u); }
/* G_TEXTURE: on in bits 1..7 */
static inline uint8_t f3dex2_texture_on(uint32_t w0) { return (uint8_t)((w0 >> 1) & 0x7Fu); }
/* G_POPMTX: w1 = bytes (64 a matrix) */
static inline uint32_t f3dex2_popmtx_count(uint32_t w1) { return w1 >> 6; }

#endif
