/* f3d_interp.h - the Fast3D interpreter (f3d_interp.c) and the hooks it
 * takes from the rest of the game side. */
#ifndef F3D_INTERP_H
#define F3D_INTERP_H
#include <stdint.h>

/* One graphics task: interpret the display list at this N64 address and
 * present the frame. */
void f3d_run(uint32_t dl);
/* hor+ on or off (the settings' Widescreen row), taken at the next frame */
void f3d_set_widescreen(int on);
/* Level load (the game reuses its heap): drop every cached texture. */
void f3d_tex_reset(void);
/* Screen fades (patches/026): 1 keeps the picture presented last under every
 * following frame, 0 ends it. Taken at the next frame (any thread may call). */
void f3d_hold_last(int on);
/* The photo album (patches/028): 1 keeps the picture presented last under
 * every following frame, 0 ends it. */
void f3d_fb_persist(int on);

/* ---- hooks (xbox/src/game) ---- */
/* The game's top-level state (0 = title screen), for the title fit; the
 * runtime's gfx.c (a pack may tell it later; -1: unknown). */
int wx_game_state(void);
/* The port's own drawing at the end of the frame (FPS counter, settings
 * panel); runs inside the frame, after the game's display list. */
void wx_frame_overlay(void);

#endif
