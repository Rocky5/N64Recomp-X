/* gfx.c - graphics tasks to the interpreter (PLAN.md 3.7).
 *
 * osSpTaskStartGo hands a graphics task here (ultra/sp.c); the display list
 * at the task's data_ptr goes through the Fast3D interpreter
 * (xbox/src/gfx/f3d_interp.c) onto the NV2A back end, and the frame is
 * presented at its end. The microcode registry (xbox/src/gfx/ucode.c)
 * names the task's GBI from the version text in its microcode's data
 * segment (Fast3D, F3DEX, F3DEX2, S2DEX; docs/renderer.md), and the
 * interpreter starts the list in it. */
#include "f3d_interp.h"
#include "settings.h"
#include "ultra/ultra.h"
#include "xgfx.h"

static volatile int s_up;
static xhw_mutex* s_lock;

int xrc_menu_open(void);

/* one thread in the interpreter and the back end at a time: the game's
 * graphics tasks, the BACK menu's frames (settings_menu.c, the VI thread) */
void xrc_gfx_lock(void) { xhw_mutex_lock(s_lock); }
void xrc_gfx_unlock(void) { xhw_mutex_unlock(s_lock); }
int xrc_gfx_ready(void) { return s_up; }

void xrc_gfx_boot(void) {
    s_lock = xhw_mutex_create();
    xgfx_init();
    s_up = 1;
    xgfx_set_fps_overlay(g_xrc_settings.fps);
    xgfx_set_tex_filter(g_xrc_settings.texture_filter ? 2 : 0);   /* settings: 0 N64, 1 sharp; xgfx: 2 sharp */
    f3d_set_widescreen(g_xrc_settings.widescreen == XRC_WS_ON ||
                       (g_xrc_settings.widescreen == XRC_WS_AUTO && (xhw_video()->height >= 720 || xhw_video_widescreen_set())));
}

/* DIVERGES: a task that comes while the BACK menu is open waits for it to
 * close (the game is frozen under the menu; settings_menu.c) */
static int s_dp = 1;
void xrc_gfx_task(uint32_t task) {
    s_dp = 1;
    if (!s_up) return;
    xrc_gfx_lock();
    while (xrc_menu_open()) {
        xrc_gfx_unlock();
        xhw_sleep_ms(10);
        xrc_gfx_lock();
    }
    f3d_set_ucode(gfx_ucode_at(rd32(task + TK_UCODE_DATA), rd32(task + TK_UCODE_DATA_SIZE)));
    f3d_run(rd32(task + TK_DATA_PTR));
    s_dp = f3d_last_fullsync();
    xrc_gfx_unlock();
}

/* sp.c: whether the task just run raises DP done (its list's FullSync) */
int xrc_gfx_task_dp(void) { return !s_up || s_dp; }

/* the interpreter's hooks: no game state known (no title fit), no port
 * drawing of our own at the end of a frame yet (the BACK menu, M2) */
int wx_game_state(void) { return -1; }
void wx_frame_overlay(void) {}
