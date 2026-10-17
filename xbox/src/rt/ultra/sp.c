/* sp.c - RSP tasks, run synchronously on the calling thread (PLAN.md 3.3).
 *
 * As in every sibling: osSpTaskStartGo runs the task at once (a graphics
 * task through the interpreter, an audio task through asp_hle, M2), then
 * posts SP done (and DP done for graphics), as the RSP and RDP interrupts
 * would. One CPU means a render thread buys nothing. osSpTaskYield is
 * ignored and osSpTaskYielded says no (upstream does the same).
 *
 * DIVERGES: the done events go to the front of their queue. On the N64 the
 * RSP ran beside the CPU, so the retraces that came during a task were
 * handled while it ran and its done interrupt came after them. Here the
 * task runs on the scheduler's own thread: those retraces wait in the queue
 * ahead of the done events, and a scheduler that sees them with its task
 * still "running" asks it to yield and then recovers from a task "that did
 * not yield" (Pilotwings 64's _uvScHandleRetrace, xemu: the first frame took
 * longer than a retrace). Done first, then the late retraces, is what the
 * scheduler would have seen. */
#include <stdio.h>

#include "ultra.h"

static unsigned s_gfx, s_aud;

unsigned xu_gfx_tasks(void) { return s_gfx; }

/* until the interpreter and the audio HLE are linked: count and log */
__attribute__((weak)) void xrc_gfx_task(uint32_t task) {
    if (s_gfx <= 3)
        xhw_logf("[SP] gfx task %u: ucode %08x (%u bytes), data %08x (%u bytes)", s_gfx, (unsigned)rd32(task + TK_UCODE),
                 (unsigned)rd32(task + TK_UCODE_SIZE), (unsigned)rd32(task + TK_DATA_PTR),
                 (unsigned)rd32(task + TK_DATA_SIZE));
}

/* gfx.c: the task's list ended with a FullSync (weak: always, as before) */
__attribute__((weak)) int xrc_gfx_task_dp(void) { return 1; }

__attribute__((weak)) void xrc_audio_task(uint32_t task) {
    if (s_aud <= 1)
        xhw_logf("[SP] audio task %u: ucode %08x, data %08x (%u bytes)", s_aud, (unsigned)rd32(task + TK_UCODE),
                 (unsigned)rd32(task + TK_DATA_PTR), (unsigned)rd32(task + TK_DATA_SIZE));
}

ULTRA(osSpTaskLoad) {
    (void)rdram;
    (void)ctx;
}

/* A task runs to its end inside osSpTaskStartGo; its done events wait until
 * the thread that started it next blocks (or the next retrace): on the N64
 * the RCP takes milliseconds, and code that starts a task outside the
 * scheduler's thread records it after the call (Blast Corps' scheduler
 * then took a DP done with no task recorded and asserted). The events go
 * out together and in order: SP done ahead of DP done.
 * Kill switch: -DXRC_RCP_DEFER=0 (posted inside the call, as before). */
#ifndef XRC_DP_ON_FULLSYNC
#define XRC_DP_ON_FULLSYNC 1
#endif
#ifndef XRC_RCP_DEFER
#define XRC_RCP_DEFER 1
#endif
static volatile unsigned s_pend_sp, s_pend_dp;

static void post_done(int dp) {
    if (dp) xu_post_event_first(OS_EVENT_DP);   /* jammed in reverse: SP done is read first */
    xu_post_event_first(OS_EVENT_SP);
}

/* under the lock (thread.c before a thread sleeps, vi.c every retrace) */
void xu_rcp_flush(void) {
    while (s_pend_sp) {
        int dp = s_pend_dp != 0;
        s_pend_sp--;
        if (dp) s_pend_dp--;
        post_done(dp);
    }
}

ULTRA(osSpTaskStartGo) {
    uint32_t task = ARG(0), type = rd32(task + TK_TYPE);
    int gfx = type == M_GFXTASK;
    (void)rdram;
    if (gfx) {
        s_gfx++;
        xrc_gfx_task(task);
        /* DIVERGES from the runtime's old way: DP done only when the list ended
         * with a FullSync, as the RDP raises it (Blast Corps' scheduler asserts
         * on a DP done with no RDP task). -DXRC_DP_ON_FULLSYNC=0: always. */
        if (XRC_DP_ON_FULLSYNC && !xrc_gfx_task_dp()) gfx = 0;
    } else {
        s_aud++;
        xrc_audio_task(task);
    }
    if (!XRC_RCP_DEFER) {
        post_done(gfx);
        return;
    }
    xu_lock();
    s_pend_sp++;
    if (gfx) s_pend_dp++;
    xu_unlock();
}

ULTRA(osSpTaskYield) {
    (void)rdram;
    (void)ctx;
}

ULTRA(osSpTaskYielded) {
    (void)rdram;
    RET(0);
}

ULTRA(__osSpSetPc) {
    (void)rdram;
    RET(0);
}

/* the DP: always idle */
ULTRA(osDpSetNextBuffer) {
    (void)rdram;
    RET(0);
}
ULTRA(osDpGetStatus) {
    (void)rdram;
    RET(0);
}
ULTRA(osDpSetStatus) {
    (void)rdram;
    (void)ctx;
}
