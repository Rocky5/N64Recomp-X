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

/* Present on swap. The N64 shows a framebuffer when the game swaps to it
 * (osViSwapBuffer), whatever number of tasks drew it. Once the game has
 * swapped, a task's frame stays open after it and is presented at the
 * swap to its buffer, or when a task starts drawing into another buffer
 * the game swaps to (f3d_interp.c f3d_swap, frame_cimg). Mario Party draws
 * a frame in several tasks: presented task by task it flickered on the
 * console (vmp1). A game that never swaps, or whose swaps never name the
 * buffers it draws into (XRC_SWAP_TASKS tasks without a present), is
 * presented at the end of every task, as before.
 * DIVERGES: none meant (the N64's behaviour); the old present, at every
 * task's end: -DXRC_PRESENT_ON_SWAP=0. */
#ifndef XRC_PRESENT_ON_SWAP
#define XRC_PRESENT_ON_SWAP 1
#endif
#ifndef XRC_SWAP_TASKS
#define XRC_SWAP_TASKS 16
#endif
static int s_swapped;
static unsigned s_open_tasks;   /* tasks since the last present on a swap */
static volatile int s_busy;               /* a task in the interpreter */
static volatile unsigned s_task_retrace;  /* the retrace the last task ended at */
#ifndef XRC_SWAP_STALE
#define XRC_SWAP_STALE 6   /* retraces an open frame waits for its swap before it is presented anyway */
#endif

/* the VI thread, every retrace: a frame left open with no task and no swap
 * to its buffer for XRC_SWAP_STALE retraces (a game that draws once and
 * waits, or swaps a buffer it did not draw into) is presented as it is */
void xrc_gfx_vi(unsigned retrace) {
    if (!XRC_PRESENT_ON_SWAP || !s_up || !s_swapped || s_busy || !f3d_frame_open()) return;
    if (retrace - s_task_retrace < XRC_SWAP_STALE) return;
    xrc_gfx_lock();
    if (!s_busy && f3d_frame_open()) f3d_present_frame();
    xrc_gfx_unlock();
}

/* osViSwapBuffer (ultra/vi.c) */
void xrc_gfx_swap(uint32_t fb) {
    if (!XRC_PRESENT_ON_SWAP || !s_up) return;
    xrc_gfx_lock();
    if (!s_swapped) {
        s_swapped = 1;
        f3d_split_frames(1);
        xhw_logf("[GFX] the game swaps (osViSwapBuffer %08x): frames presented at the swap", (unsigned)fb);
    }
    if (f3d_swap(fb)) s_open_tasks = 0;
    xrc_gfx_unlock();
}

/* DIVERGES: a task that comes while the BACK menu is open waits for it to
 * close (the game is frozen under the menu; settings_menu.c) */
static int s_dp = 1;
void xrc_gfx_task(uint32_t task) {
    int split;
    unsigned before;
    s_dp = 1;
    if (!s_up) return;
    xrc_gfx_lock();
    while (xrc_menu_open()) {
        xrc_gfx_unlock();
        xhw_sleep_ms(10);
        xrc_gfx_lock();
    }
    split = s_swapped && s_open_tasks < XRC_SWAP_TASKS;
    if (s_swapped && s_open_tasks == XRC_SWAP_TASKS) {
        s_open_tasks++;
        f3d_split_frames(0);
        xhw_logf("[GFX] %u tasks and no swap to their buffer: frames presented at every task's end", XRC_SWAP_TASKS);
    }
    before = f3d_presents();
    s_busy = 1;
    f3d_set_ucode(gfx_ucode_at(rd32(task + TK_UCODE_DATA), rd32(task + TK_UCODE_DATA_SIZE)));
    f3d_run(rd32(task + TK_DATA_PTR), !split);
    s_busy = 0;
    s_task_retrace = xu_retrace();
    if (split) s_open_tasks = f3d_presents() != before ? 0 : s_open_tasks + 1;
    s_dp = f3d_last_fullsync();
    xrc_gfx_unlock();
}

/* sp.c: whether the task just run raises DP done (its list's FullSync) */
int xrc_gfx_task_dp(void) { return !s_up || s_dp; }

/* the interpreter's hooks: no game state known (no title fit), no port
 * drawing of our own at the end of a frame yet (the BACK menu, M2) */
int wx_game_state(void) { return -1; }
void wx_frame_overlay(void) {}
