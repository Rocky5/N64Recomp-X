/* ucode.h - the graphics microcode registry (PLAN.md 3.7, docs/renderer.md).
 *
 * A graphics task names its microcode by address only; which GBI its
 * display lists speak is read from the microcode itself: the version text
 * every SGI/Nintendo graphics microcode carries in its data segment
 * ("RSP SW Version: 2.0D, 04-01-96" for Fast3D, "RSP Gfx ucode F3DEX
 * fifo 1.23 Yoshitaka Yasumoto 1997 Nintendo." for F3DEX and its line,
 * LX/LP and .NoN/.Rej variants, "... 2.08 ..." for F3DEX2, "S2DEX ..." for
 * the sprite microcode). That text is the microcode's behaviour, not a
 * game's: one rule serves every ROM linking the same library. A data
 * segment with no such text is taken as Fast3D (the build before the
 * registry: Pilotwings 64 renders as before). */
#ifndef XRC_GFX_UCODE_H
#define XRC_GFX_UCODE_H
#include <stddef.h>
#include <stdint.h>

/* ---- switches (docs/testing.md): 0 interprets that family as Fast3D, the
 * build before the registry ---- */
#ifndef XGFX_UCODE_F3DEX
#define XGFX_UCODE_F3DEX 1     /* F3DEX 0.9x/1.x, F3DLX, F3DLP, L3DEX, their .NoN/.Rej */
#endif
#ifndef XGFX_UCODE_F3DEX2
#define XGFX_UCODE_F3DEX2 1    /* F3DEX2, F3DZEX, F3DLX2, L3DEX2, their .NoN/.Rej */
#endif
#ifndef XGFX_UCODE_S2DEX
#define XGFX_UCODE_S2DEX 1     /* S2DEX 1.x and 2.x: backgrounds and objects */
#endif

enum {
    GFX_UC_F3D = 0,     /* Fast3D (gbi.h with no F3DEX_GBI) */
    GFX_UC_F3DEX,       /* F3DEX_GBI: Fast3D's numbering, 32+ vertices, TRI2 */
    GFX_UC_F3DEX2,      /* F3DEX_GBI_2: renumbered, new MTX/MOVEMEM/MOVEWORD/GEOMETRYMODE layouts */
    GFX_UC_S2DEX,       /* S2DEX 1.x: F3DEX's numbering for the shared commands */
    GFX_UC_S2DEX2,      /* S2DEX 2.x: F3DEX2's numbering for the shared commands */
    GFX_UC_FAMILIES
};

#define GFX_UC_F_OLD 1u   /* F3DEX 0.9x (F3D_OLD): CULLDL in Fast3D's units, 0xB2 RDPHALF_CONT, QUAD 0xB5 */
#define GFX_UC_F_LINE 2u  /* L3DEX: TRI commands draw lines (not drawn here) */
#define GFX_UC_F_BG 4u    /* F3DEXBG: Conker's own F3DEX2 (f3d_interp.c, "F3DEXBG") */

typedef struct {
    uint8_t family;      /* GFX_UC_* */
    uint8_t flags;       /* GFX_UC_F_* */
    uint8_t vtx_max;     /* the vertex buffer: 16 (Fast3D), 32, 64 (.Rej), 80 (F3DLP) */
    uint8_t found;       /* the version text was found */
    char name[48];       /* the version text as found, for the log */
} GfxUcode;

/* The microcode a data segment's bytes (in N64 byte order) belong to. Pure,
 * host-tested (tests/xbox/test_f3dex.c). Always fills *out (Fast3D when no
 * text is found); returns out->found. */
int gfx_ucode_parse(const uint8_t* data, size_t n, GfxUcode* out);

/* The microcode whose data segment is at this N64 address (a task's
 * ucode_data, a G_LOAD_UCODE's RDPHALF_1), read from RDRAM once per address
 * and remembered; logs "[GFX] ucode ..." the first time. Never NULL. */
const GfxUcode* gfx_ucode_at(uint32_t data_addr, uint32_t data_size);

/* A microcode with no version text (Jet Force Gemini's: Rare stripped it)
 * read as Fast3D drew nothing right: its display list says the family. The
 * GBI's command bytes differ: F3DEX2 sets state with 0xD9-0xDF, 0xE2 and
 * 0xE3, Fast3D and F3DEX with 0xB6-0xBF. `words` is the first n command
 * words (w0 of each, the top-level list in N64 order). Pure, host-tested.
 * Returns the family it votes for, or -1 when the list does not say. */
int gfx_ucode_guess(const uint32_t* w0s, unsigned n);
/* gfx_ucode_at for a task: a microcode without text guessed from its
 * display list (dl: the task's data_ptr) the first time.
 * -DXGFX_UCODE_GUESS=0: Fast3D, as before. */
const GfxUcode* gfx_ucode_for_task(uint32_t data_addr, uint32_t data_size, uint32_t dl);

#endif
