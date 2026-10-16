/* gfx.c - graphics tasks to the interpreter (PLAN.md 3.7).
 *
 * osSpTaskStartGo hands a graphics task here (ultra/sp.c); the display list
 * at the task's data_ptr goes through the Fast3D interpreter
 * (xbox/src/gfx/f3d_interp.c) onto the NV2A back end, and the frame is
 * presented at its end. The microcode registry (the task's ucode hash ->
 * the family's opcode table, PLAN.md 3.7) comes with the second family;
 * every pack today is Fast3D. */
#include "f3d_interp.h"
#include "settings.h"
#include "ultra/ultra.h"
#include "xgfx.h"

static int s_up;

void xrc_gfx_boot(void) {
    xgfx_init();
    s_up = 1;
    xgfx_set_fps_overlay(g_xrc_settings.fps);
    xgfx_set_tex_filter(g_xrc_settings.texture_filter);
    f3d_set_widescreen(g_xrc_settings.widescreen == XRC_WS_ON ||
                       (g_xrc_settings.widescreen == XRC_WS_AUTO && (xhw_video()->height >= 720 || xhw_video_widescreen_set())));
}

void xrc_gfx_task(uint32_t task) {
    if (!s_up) return;
    f3d_run(rd32(task + TK_DATA_PTR));
}

/* the interpreter's hooks: no game state known (no title fit), no port
 * drawing of our own at the end of a frame yet (the BACK menu, M2) */
int wx_game_state(void) { return -1; }
void wx_frame_overlay(void) {}
