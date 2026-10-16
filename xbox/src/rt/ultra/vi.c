/* vi.c - the video interface: the retrace thread, the swap latch, the mode
 * the game set (PLAN.md 3.3).
 *
 * The retrace is xhw_wait_vblank (pbkit's vblank once the renderer runs at
 * 60 Hz, else 60.000 Hz from the ACPI timer). Every retrace: the swap
 * latches, the osViSetEvent message goes out every `retraceCount` retraces,
 * and the exit combos get their VI look (PLAN.md 3.9: the game has not read
 * the controllers for a while). The VI mode is kept for the interpreter's
 * content rect (M1+: 320x240, 640x480 interlaced, letterboxed modes).
 *
 * DIVERGES (Wings64-X's): the retrace message is skipped when it would
 * leave the game's queue fewer than XRC_VI_RESERVE free slots. Graphics
 * tasks run synchronously on the scheduler's thread, retraces pile up
 * meanwhile, and the task's own SP/DP done events (posted as it returns)
 * found the queue full and were lost; the game then waited for its RDP
 * timeout (Wings64-X console run 3). A late retrace message is harmless.
 *
 * DIVERGES: while the BACK menu is open no retrace message goes out, so the
 * game waits at its retrace (../settings_menu.c), and this thread drives
 * the menu. */
#include "ultra.h"

int xrc_menu_open(void);
void xrc_menu_vi(unsigned retrace);
void xrc_video_vi(unsigned retrace);

#ifndef XRC_VI_RESERVE
#define XRC_VI_RESERVE 2
#endif

static volatile uint32_t s_mq, s_msg, s_every = 1;
static volatile uint32_t s_cur, s_next, s_mode, s_black, s_features;
static volatile unsigned s_retrace;
static volatile int s_up;

unsigned xu_retrace(void) { return s_retrace; }
uint32_t xu_vi_mode(void) { return s_mode; }
uint32_t xu_vi_framebuffer(void) { return s_cur; }

static void vi_thread(void* arg) {
    (void)arg;
    for (;;) {
        uint32_t mq;
        xhw_wait_vblank();
        if (s_cur != s_next) s_cur = s_next;
        s_retrace++;
        mq = s_mq;
        xrc_menu_vi(s_retrace);
        xrc_video_vi(s_retrace);   /* the video-mode prompt: A, or the timeout (../video.c) */
        if (mq && s_retrace % s_every == 0 && !xrc_menu_open()) {
            int32_t count = (int32_t)rd32(mq + MQ_MSGCOUNT), valid = (int32_t)rd32(mq + MQ_VALIDCOUNT);
            if (!XRC_VI_RESERVE || valid + 1 + XRC_VI_RESERVE <= count) xu_send(mq, s_msg, 0, 0);
        }
        xu_post_event(OS_EVENT_VI);
        xrc_mmio_vi();   /* the register pages' status words, for recompiled pollers (../mmio.c) */
        xhw_combo_vi_look(s_retrace);
        if (XHW_TEST_BUILD) {   /* no graphics task for a second: say once what everyone waits on */
            static unsigned s_seen, s_still;
            unsigned g = xu_gfx_tasks();
            if (g != s_seen) s_seen = g, s_still = 0;
            else if (++s_still == 60 && g) xu_stall_report("no graphics task for 60 retraces");
            else if (s_still == 300 && !g) xu_stall_report("no graphics task at all after 300 retraces");
            if (s_retrace % 1800 == 0)   /* every 30 s */
                xhw_logf("[VI] retrace %u: %u gfx tasks, queue %08x %u/%u", s_retrace, g, (unsigned)s_mq,
                         s_mq ? (unsigned)rd32(s_mq + MQ_VALIDCOUNT) : 0u, s_mq ? (unsigned)rd32(s_mq + MQ_MSGCOUNT) : 0u);
        }
    }
}

ULTRA(osCreateViManager) {
    (void)rdram;
    (void)ctx;
    if (s_up) return;
    s_up = 1;
    xu_spawn(vi_thread, NULL, 14, 32 * 1024);
    xhw_log("[OS] VI manager up");
}

/* osViSetEvent(mq, msg, retraceCount) */
ULTRA(osViSetEvent) {
    (void)rdram;
    s_msg = ARG(1);
    s_every = ARG(2) ? ARG(2) : 1;
    s_mq = ARG(0);
}

ULTRA(osViSwapBuffer) {
    uint32_t fb = ARG(0);
    (void)rdram;
    s_next = fb;
    if (!s_cur) s_cur = fb;
}

ULTRA(osViGetCurrentFramebuffer) {
    (void)rdram;
    RET(s_cur);
}

ULTRA(osViGetNextFramebuffer) {
    (void)rdram;
    RET(s_next);
}

ULTRA(osViSetMode) {
    (void)rdram;
    if (s_mode != ARG(0)) xhw_logf("[OS] VI mode at %08x", (unsigned)ARG(0));
    s_mode = ARG(0);
}

ULTRA(osViBlack) {
    (void)rdram;
    s_black = ARG(0) & 0xFF;
}

ULTRA(osViSetSpecialFeatures) {
    (void)rdram;
    s_features = ARG(0);
}

ULTRA(osViSetXScale) {
    (void)rdram;
    (void)ctx;
}

ULTRA(osViSetYScale) {
    (void)rdram;
    (void)ctx;
}

ULTRA(osViRepeatLine) {
    (void)rdram;
    (void)ctx;
}
