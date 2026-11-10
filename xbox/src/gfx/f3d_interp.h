/* f3d_interp.h - the display-list interpreter (f3d_interp.c: Fast3D, F3DEX,
 * F3DEX2, S2DEX's shared commands) and the hooks it takes from the rest of
 * the game side. */
#ifndef F3D_INTERP_H
#define F3D_INTERP_H
#include <stdint.h>

#include "ucode.h"

/* One graphics task: interpret the display list at this N64 address and
 * present the frame. */
void f3d_run_rdp(uint32_t list, uint32_t bytes, int present);   /* an RDP list of that length, no display list */
void f3d_run(uint32_t dl, int present);   /* present: 0 leaves the frame open for the next task */
void f3d_present_frame(void);   /* the open frame presented (none open: nothing) */
int f3d_swap(uint32_t fb);
unsigned f3d_task_no(void);             /* tasks run so far (XGFX_PROBE_DPSYNC) */
void f3d_probe_restore(unsigned task);  /* that task's probe copies back into RDRAM (XGFX_PROBE_DPSYNC) */
void f3d_fb_inval(uint32_t addr, uint32_t bytes);   /* osInvalDCache: a staged frame there into RDRAM (XGFX_FB_READBACK) */   /* osViSwapBuffer: presents the open frame if it is fb's; 1 when it did */
unsigned f3d_presents(void);   /* frames presented so far */
void f3d_bench(uint32_t dl, int null_back);   /* gfx.c XRC_DLBENCH: the list once more into the open frame (null_back: nothing drawn) */
void f3d_split_frames(int on);
int f3d_frame_open(void);   /* a frame is open (drawn into, not presented yet) */   /* frames may span tasks: a task that draws into another swapped buffer presents the open frame */
int f3d_last_fullsync(void);   /* the last f3d_run's list had a G_RDPFULLSYNC */
/* The microcode the next f3d_run starts in (ucode.h; NULL: Fast3D). A
 * G_LOAD_UCODE in the list switches it for the rest of that task. */
void f3d_set_ucode(const GfxUcode* uc);
/* hor+ on or off (the settings' Widescreen row), taken at the next frame */
void f3d_set_widescreen(int on);
/* 1: hor+ for any game (settings.h XRC_WS_FORCE); 0: only for a game that culls for it, which f3d_game_wide3d says */
void f3d_set_wide3d(int mode);
int f3d_game_wide3d(void);
/* Level load (the game reuses its heap): drop every cached texture. */
void f3d_tex_reset(void);
/* Screen fades (patches/026): 1 keeps the picture presented last under every
 * following frame, 0 ends it. Taken at the next frame (any thread may call). */
void f3d_hold_last(int on);
/* The photo album (patches/028): 1 keeps the picture presented last under
 * every following frame, 0 ends it. */
void f3d_fb_persist(int on);
/* A frame of the held picture alone (no display list), dimmed, with a card
 * image over it, presented: the BACK menu's frames over the frozen game
 * (call f3d_hold_last(1) first; f3d_interp.c says how the card is placed). */
void f3d_present_held(const uint32_t* img, int tw, int th, int pw, int ph, int fade, int slide);

/* ---- hooks (xbox/src/game) ---- */
/* The game's top-level state (0 = title screen), for the title fit; the
 * runtime's gfx.c (a pack may tell it later; -1: unknown). */
int wx_game_state(void);
/* The port's own drawing at the end of the frame (FPS counter, settings
 * panel); runs inside the frame, after the game's display list. */
void wx_frame_overlay(void);

#endif
