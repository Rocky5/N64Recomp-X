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

__attribute__((weak)) void xrc_audio_task(uint32_t task) {
    if (s_aud <= 1)
        xhw_logf("[SP] audio task %u: ucode %08x, data %08x (%u bytes)", s_aud, (unsigned)rd32(task + TK_UCODE),
                 (unsigned)rd32(task + TK_DATA_PTR), (unsigned)rd32(task + TK_DATA_SIZE));
}

ULTRA(osSpTaskLoad) {
    (void)rdram;
    (void)ctx;
}

ULTRA(osSpTaskStartGo) {
    uint32_t task = ARG(0), type = rd32(task + TK_TYPE);
    (void)rdram;
    if (type == M_GFXTASK) {
        s_gfx++;
        xrc_gfx_task(task);
        xu_post_event_first(OS_EVENT_DP);   /* jammed in reverse: SP done is read first */
        xu_post_event_first(OS_EVENT_SP);
    } else {
        s_aud++;
        xrc_audio_task(task);
        xu_post_event_first(OS_EVENT_SP);
    }
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
