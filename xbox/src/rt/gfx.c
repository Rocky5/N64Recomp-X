/* gfx.c - graphics tasks to the interpreter (PLAN.md 3.7).
 *
 * osSpTaskStartGo hands a graphics task here (ultra/sp.c); the display list
 * at the task's data_ptr goes through the Fast3D interpreter
 * (xbox/src/gfx/f3d_interp.c) onto the NV2A back end, and the frame is
 * presented at its end. The microcode registry (xbox/src/gfx/ucode.c)
 * names the task's GBI from the version text in its microcode's data
 * segment (Fast3D, F3DEX, F3DEX2, S2DEX; docs/renderer.md), and the
 * interpreter starts the list in it. */
#include <stdlib.h>

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
    f3d_set_wide3d(g_xrc_settings.widescreen == XRC_WS_FORCE);
    f3d_set_widescreen(g_xrc_settings.widescreen == XRC_WS_ON || g_xrc_settings.widescreen == XRC_WS_FORCE ||
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

/* The renderer's lock from a game thread: the runtime lock's levels it holds
 * (interrupts off) are let go while it waits, and taken back with the
 * renderer held: no thread that holds the runtime lock waits for the
 * renderer then, so the two cannot cross (the BACK menu holds the renderer
 * while it is open and takes no runtime lock; the VI thread takes the
 * renderer only after it lets the runtime lock go). DIVERGES: while it waits
 * for the BACK menu, other threads may take the runtime lock its
 * interrupts-off section held (the game is paused by the menu then).
 * -DXRC_GFX_LOCK_DROP=0: waits holding it, as before */
#ifndef XRC_GFX_LOCK_DROP
#define XRC_GFX_LOCK_DROP 1
#endif
int xu_lock_drop(void);
void xu_lock_take(int n);
static int gfx_lock_game(void) {   /* the levels to take back after */
    int n = XRC_GFX_LOCK_DROP ? xu_lock_drop() : 0;
    xrc_gfx_lock();
    return n;
}

/* A present waits for the frame on screen to go, up to a vblank, and on the N64 a swap only writes a register. A thread that
 * swapped therefore slept where the game has it running, and lower threads ran in its place: Snowboard Kids' main thread reuses
 * the second task of a frame as soon as it has built two more, its graphics thread had still to read that task, read another
 * frame's buffer, the one just swapped, and waited for ever for it to leave the screen (0 to 39 frames, a different number each
 * run). The present now waits for the first thing that can tell: a thread about to block, the next task, a retrace.
 * DIVERGES: none meant. -DXRC_SWAP_AT_BLOCK=0: in the swap, as before */
#ifndef XRC_SWAP_AT_BLOCK
#define XRC_SWAP_AT_BLOCK 1
#endif
static volatile int s_swap_pending;
static volatile uint32_t s_swap_fb;
static volatile unsigned s_swap_retrace;

/* the VI thread, every retrace: a frame left open with no task and no swap
 * to its buffer for XRC_SWAP_STALE retraces (a game that draws once and
 * waits, or swaps a buffer it did not draw into) is presented as it is */
static void swap_run(void);
void xrc_gfx_vi(unsigned retrace) {
    if (s_swap_pending && retrace != s_swap_retrace && !s_busy) {   /* nothing blocked or drew since the swap (a task presents it at its start) */
        xrc_gfx_lock();
        swap_run();
        xrc_gfx_unlock();
    }
    if (!XRC_PRESENT_ON_SWAP || !s_up || !s_swapped || s_busy || !f3d_frame_open()) return;
    if (retrace - s_task_retrace < XRC_SWAP_STALE) return;
    xrc_gfx_lock();
    if (!s_busy && f3d_frame_open()) f3d_present_frame();
    xrc_gfx_unlock();
}

/* osViSwapBuffer (ultra/vi.c) */
static void swap_now(uint32_t fb) {   /* the renderer held */
    if (!s_swapped) {
        s_swapped = 1;
        f3d_split_frames(1);
        xhw_logf("[GFX] the game swaps (osViSwapBuffer %08x): frames presented at the swap", (unsigned)fb);
    }
    if (f3d_swap(fb)) s_open_tasks = 0;
}
static void swap_run(void) {          /* the renderer held: a swap still to do */
    if (s_swap_pending) {
        s_swap_pending = 0;
        swap_now(s_swap_fb);
    }
}
void xrc_gfx_swap(uint32_t fb) {
    if (!XRC_PRESENT_ON_SWAP || !s_up) return;
    if (XRC_SWAP_AT_BLOCK && !s_swap_pending) {
        s_swap_fb = fb;
        s_swap_retrace = xu_retrace();
        s_swap_pending = 1;
        return;
    }
    xu_lock_take(gfx_lock_game());
    swap_run();     /* (a swap with another still waiting: no task between them, so no frame to wait for) */
    swap_now(fb);
    xrc_gfx_unlock();
}
/* osRecvMesg, about to block */
void xrc_gfx_swap_block(void) {
    if (!s_swap_pending) return;
    xu_lock_take(gfx_lock_game());
    swap_run();
    xrc_gfx_unlock();
}

/* test builds, -DXRC_DLDUMP=<n>: the first XRC_DLDUMP_N graphics tasks
 * after retrace <n> are dumped before they run, for tools/xbox/bench_dl.py
 * to replay on the host: the task, then the first time the whole RDRAM
 * image as the runtime keeps it ([DLRAM]), after that only the 4 KB pages
 * that changed since the last dump ([DLPAGES]: each page's index, then its
 * bytes). COM1 carries about 10 KB a second in xemu: a whole image is a
 * minute or two, a frame's changed pages seconds. */
#if defined(XRC_DLDUMP) && XRC_DLDUMP > 0
#ifndef XRC_DLDUMP_N
#define XRC_DLDUMP_N 6
#endif
static uint32_t page_hash(const uint8_t* p) {
    uint32_t h = 2166136261u, i, w;
    for (i = 0; i < 4096; i += 4) {
        memcpy(&w, p + i, 4);
        h = (h ^ w) * 16777619u;
    }
    return h;
}
static void dl_dump(uint32_t task) {
    static unsigned s_done;
    static uint32_t* s_hash;
    uint32_t words[17], np = xrc_rdram_size / 4096u;
    unsigned i, n = 0;
    if (s_done >= XRC_DLDUMP_N) return;
#if defined(XRC_DLDUMP_SHOT) && XRC_DLDUMP_SHOT   /* -DXRC_DLDUMP_SHOT=1: from the first screenshot request (BACK + WHITE) on, not a retrace */
    if (!xhw_shot_requests()) return;
#else
    if (xu_retrace() < (unsigned)XRC_DLDUMP) return;
#endif
    if (!s_hash && !(s_hash = (uint32_t*)malloc(np * sizeof *s_hash))) return;
    for (i = 0; i < 16; i++) words[i] = rd32(task + i * 4);
    words[16] = xrc_rdram_size;
    xhw_logf("[DLDUMP] task %08x data %08x ucode data %08x", (unsigned)task, (unsigned)words[TK_DATA_PTR / 4],
             (unsigned)words[TK_UCODE_DATA / 4]);
    xhw_dump_blob("DLTASK", words, sizeof words);
    if (!s_done++) {
        for (i = 0; i < np; i++) s_hash[i] = page_hash(xrc_rdram_host + i * 4096u);
        xhw_dump_zblob("DLRAM", xrc_rdram_host, xrc_rdram_size);
        return;
    }
    {   /* the changed pages, gathered into one buffer (at most 256 of them) */
        enum { MAXP = 256 };
        uint8_t* buf = (uint8_t*)malloc(MAXP * 4100u);
        if (!buf) return;
        for (i = 0; i < np && n < MAXP; i++) {
            uint32_t h = page_hash(xrc_rdram_host + i * 4096u);
            if (h == s_hash[i]) continue;
            s_hash[i] = h;
            memcpy(buf + n * 4100u, &i, 4);
            memcpy(buf + n * 4100u + 4, xrc_rdram_host + i * 4096u, 4096);
            n++;
        }
        xhw_logf("[DLDUMP] %u pages changed", n);
        xhw_dump_zblob("DLPAGES", buf, n * 4100u);
        free(buf);
    }
}
#endif

/* -DXRC_DLBENCH=<n> (console diagnosis, any build): every n retraces one
 * graphics task is run XRC_DLBENCH_RUNS more times before its real run,
 * first with the back end left out (the interpreter's own time: the list
 * walk, vertices, clipping, textures looked up), then with it (the NV2A's
 * push buffer and waits too), and both are logged as [DLBENCH]: where a
 * game's "display lists" time in [PERF] goes, on the console's own CPU. The
 * game sees one long frame each time. */
#ifndef XRC_DLBENCH
#define XRC_DLBENCH 0
#endif
#ifndef XRC_DLBENCH_RUNS
#define XRC_DLBENCH_RUNS 8
#endif
static void dl_bench(uint32_t task) {
    static unsigned s_next = XRC_DLBENCH;
    uint32_t dl = rd32(task + TK_DATA_PTR), ud = rd32(task + TK_UCODE_DATA), us = rd32(task + TK_UCODE_DATA_SIZE);
    uint64_t t0, t1, t2;
    unsigned k, r = xu_retrace();
    if (!XRC_DLBENCH || r < s_next) return;
    s_next = r + XRC_DLBENCH;
    t0 = xhw_time_us();
    for (k = 0; k < XRC_DLBENCH_RUNS; k++) {
        f3d_set_ucode(gfx_ucode_for_task(ud, us, dl));
        f3d_bench(dl, 1);
    }
    t1 = xhw_time_us();
    for (k = 0; k < XRC_DLBENCH_RUNS; k++) {
        f3d_set_ucode(gfx_ucode_for_task(ud, us, dl));
        f3d_bench(dl, 0);
    }
    t2 = xhw_time_us();
    xhw_logf("[DLBENCH] retrace %u, list %08x: %u us a run with no back end, %u us with it (%u runs each)", r,
             (unsigned)dl, (unsigned)((t1 - t0) / XRC_DLBENCH_RUNS), (unsigned)((t2 - t1) / XRC_DLBENCH_RUNS),
             (unsigned)XRC_DLBENCH_RUNS);
}

/* diagnosis: -DXRC_PEEK_TASK=<N64 address> logs 32 bytes there as each
 * graphics task starts, with the retrace (XRC_PEEK_TASK2 and 3: two more
 * addresses on the same line), for the first XRC_PEEK_TASK_N tasks after
 * the first word turns non-zero: what a game's own frame-to-frame state
 * looks like at the RDP's start (Pokemon Snap's photo check: its two
 * detector contexts and their message queue, red only) */
#ifdef XRC_PEEK_TASK
#ifndef XRC_PEEK_TASK_N
#define XRC_PEEK_TASK_N 4000
#endif
static void peek_task(void) {
    static unsigned s_n;
    static const uint32_t at[3] = { (uint32_t)(XRC_PEEK_TASK),
#ifdef XRC_PEEK_TASK2
                                    (uint32_t)(XRC_PEEK_TASK2),
#else
                                    0,
#endif
#ifdef XRC_PEEK_TASK3
                                    (uint32_t)(XRC_PEEK_TASK3)
#else
                                    0
#endif
    };
    int k;
    if (s_n >= XRC_PEEK_TASK_N || (!s_n && !rd32(at[0]))) return;
    s_n++;
    for (k = 0; k < 3; k++) {
        uint32_t a = at[k];
        if (!a) continue;
        xhw_logf("[PEEKT] r%u %08x: %08x %08x %08x %08x %08x %08x %08x %08x", xu_retrace(), (unsigned)a, (unsigned)rd32(a),
                 (unsigned)rd32(a + 4), (unsigned)rd32(a + 8), (unsigned)rd32(a + 12), (unsigned)rd32(a + 16),
                 (unsigned)rd32(a + 20), (unsigned)rd32(a + 24), (unsigned)rd32(a + 28));
    }
}
#endif

/* A graphics task runs on the thread that starts it, where on the N64 the
 * RSP and RDP drew beside the CPU: a long one held up the game threads that
 * share the scheduler's kernel level. Donkey Kong 64's audio thread then
 * answered its start timer two retraces late, its scheduler armed the timer
 * again, and the extra message left the audio thread waiting for a task
 * nobody started (silence once graphics begin). -DXRC_GFX_BREATHE=<ms>:
 * every <ms> of a task the thread yields to the others of its level, when it
 * holds no level of the runtime's lock. 0 (the default until measured): as before */
#ifndef XRC_GFX_BREATHE
#define XRC_GFX_BREATHE 0
#endif
static int s_breathe_ok;
static uint64_t s_breathe_t;
void xrc_gfx_breathe(void) {
    uint64_t now;
    if (!XRC_GFX_BREATHE || !s_breathe_ok) return;
    now = xhw_time_us();
    if (now - s_breathe_t < (uint64_t)(XRC_GFX_BREATHE) * 1000u) return;
    xhw_yield();
    s_breathe_t = xhw_time_us();
}

/* XGFX_PROBE_DPSYNC (f3d_interp.c): the tasks whose DP done went to the
 * game's queue, in order; thread.c says when the game takes one. One
 * producer (the task's thread, under the graphics lock), one consumer (the
 * thread taking the message, under the runtime's lock) */
#define DPQ 16
static uint32_t s_dpq[DPQ];
static volatile unsigned s_dpq_in, s_dpq_out;
void xrc_gfx_dp_taken(void) {
    if (s_dpq_out == s_dpq_in) return;
    if (XHW_TEST_BUILD) {   /* how far behind the tasks the game takes its DP dones */
        static unsigned s_n;
        if (!(s_n++ % 600))
            xhw_logf("[GFX] DP done %u taken: task %u, %u tasks run, %u waiting", s_n, (unsigned)s_dpq[s_dpq_out % DPQ],
                     f3d_task_no(), s_dpq_in - s_dpq_out - 1u);
    }
    f3d_probe_restore(s_dpq[s_dpq_out % DPQ]);
    s_dpq_out++;
}

/* DIVERGES: a task that comes while the BACK menu is open waits for it to
 * close (the game is frozen under the menu; settings_menu.c) */
static int s_dp = 1;
static void task_begin(int* split, int* n, unsigned* before);
static void task_end(int split, unsigned before);
void xrc_gfx_task(uint32_t task) {
    int split, n;
    unsigned before;
    s_dp = 1;
    if (!s_up) return;
#ifdef XRC_TIMELINE
    { void xrc_timeline_task(int end); xrc_timeline_task(0); }   /* ultra/thread.c */
#endif
    task_begin(&split, &n, &before);
#if defined(XRC_DLDUMP) && XRC_DLDUMP > 0
    dl_dump(task);
#endif
#ifdef XRC_PEEK_TASK
    peek_task();
#endif
    if (XRC_DLBENCH) dl_bench(task);
    s_busy = 1;
    s_breathe_ok = n == 0, s_breathe_t = xhw_time_us();
    f3d_set_ucode(gfx_ucode_for_task(rd32(task + TK_UCODE_DATA), rd32(task + TK_UCODE_DATA_SIZE),
                                     rd32(task + TK_DATA_PTR)));
    f3d_run(rd32(task + TK_DATA_PTR), !split);
    task_end(split, before);
#ifdef XRC_TIMELINE
    { void xrc_timeline_task(int end); xrc_timeline_task(1); }
#endif
}

/* osDpSetNextBuffer (ultra/sp.c): an RDP list the game wrote itself, with no RSP task (Killer Instinct Gold clears its screen so) */
void xrc_gfx_dp_list(uint32_t list, uint32_t bytes) {
    int split, n;
    unsigned before;
    if (!s_up) return;
    task_begin(&split, &n, &before);
    s_busy = 1;
    s_breathe_ok = n == 0, s_breathe_t = xhw_time_us();
    f3d_run_rdp(list, bytes, !split);
    task_end(split, before);
}

/* the renderer taken for a task; the frame's state as the task finds it */
static void task_begin(int* split_out, int* n_out, unsigned* before) {
    int split, n = gfx_lock_game();
    while (xrc_menu_open()) {
        xrc_gfx_unlock();
        xhw_sleep_ms(10);
        xrc_gfx_lock();
    }
    xu_lock_take(n);   /* the menu closed and the renderer is this thread's */
    swap_run();
    split = s_swapped && s_open_tasks < XRC_SWAP_TASKS;
    if (s_swapped && s_open_tasks == XRC_SWAP_TASKS) {
        s_open_tasks++;
        f3d_split_frames(0);
        xhw_logf("[GFX] %u tasks and no swap to their buffer: frames presented at every task's end", XRC_SWAP_TASKS);
    }
    *before = f3d_presents();
    *split_out = split;
    *n_out = n;
}

/* a task's end: the renderer let go */
static void task_end(int split, unsigned before) {
    s_busy = 0;
    s_breathe_ok = 0;
    s_task_retrace = xu_retrace();
    if (split) s_open_tasks = f3d_presents() != before ? 0 : s_open_tasks + 1;
    s_dp = f3d_last_fullsync();
    if (s_dp && xu_event_set(OS_EVENT_DP)) {   /* (a DP done no queue gets is never taken) */
        if (s_dpq_in - s_dpq_out == DPQ) s_dpq_out++;   /* lost track (dropped posts): the oldest goes */
        s_dpq[s_dpq_in % DPQ] = f3d_task_no();
        s_dpq_in++;
    }
    xrc_gfx_unlock();
}

/* osInvalDCache (ultra/misc.c): a frame there about to be read by the CPU */
void xrc_gfx_inval(uint32_t addr, uint32_t len) {
    if (!s_up) return;
    xu_lock_take(gfx_lock_game());
    swap_run();
    f3d_fb_inval(addr, len);
    xrc_gfx_unlock();
}

/* sp.c: whether the task just run raises DP done (its list's FullSync) */
int xrc_gfx_task_dp(void) { return !s_up || s_dp; }

/* the interpreter's hooks: no game state known (no title fit), no port
 * drawing of our own at the end of a frame yet (the BACK menu, M2) */
int wx_game_state(void) { return -1; }
void wx_frame_overlay(void) {}
