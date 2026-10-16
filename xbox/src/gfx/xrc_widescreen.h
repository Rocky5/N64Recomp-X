/* xrc_widescreen.h - the interpreter's widescreen switches (PLAN.md 3.7,
 * 5.4), from Wings64-X's wings64_widescreen.h without its game hooks.
 *   -DXGFX_HORPLUS=0     16:9 as a plain stretch (no hor+)
 *   -DXGFX_HUD_ANCHOR=0  2D stays in the centred 4:3 box
 *   -DXGFX_WORLD_GENERIC=0  hor+ only inside a pack's world tags
 * Generic hor+ (PLAN.md 5.4, every pack on day one): until a pack emits a
 * world tag, every perspective draw under the full-screen scissor is the
 * world and is drawn across the whole width; the 2D (rectangles,
 * orthographic draws) stays in the 4:3 box. The game's own culling still
 * works to its 4:3 frustum, so objects can pop in at the edges until the
 * pack's culling patch lands. DIVERGES: hor+.
 * The G_NOOP tags are what a pack's MIPS-C patches may emit to mark a world
 * view and the 2D anchor (Wings64-X's values; no pack emits them yet). */
#ifndef XRC_WIDESCREEN_H
#define XRC_WIDESCREEN_H

#ifndef XGFX_HORPLUS
#define XGFX_HORPLUS 1
#endif
#ifndef XGFX_HUD_ANCHOR
#define XGFX_HUD_ANCHOR 1
#endif
#ifndef XGFX_WORLD_GENERIC
#define XGFX_WORLD_GENERIC 1
#endif

#define WINGS64_TAG_WORLD_ON 0x50575731u    /* "PWW1" */
#define WINGS64_TAG_WORLD_OFF 0x50575730u   /* "PWW0" */
#define WINGS64_TAG_ANCHOR 0x50574100u      /* "PWA" + 'L' / 'C' / 'R' */

#endif
