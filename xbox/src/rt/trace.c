/* trace.c - the functions a trace build (N64RX_TRACE=1: N64Recomp's
 * trace_mode, xbox/include/xrc/trace.h) entered last, logged when the game
 * crashes. One ring for every thread, each entry with the kernel thread
 * that made it; a store and an increment per function, so a trace build
 * runs slower but still runs. A build without trace_mode never calls in. */
#include "rt.h"
#include "xhw.h"

#define RING 4096   /* deep enough that a thread looping beside the crashing one does not push its entries out */

static const char* volatile s_name[RING];
static void* volatile s_thread[RING];
static volatile uint32_t s_vram[RING];   /* the interpreter's calls: an address, no name */
static volatile unsigned s_n;

void xrc_trace_enter(const char* name) {
    unsigned i = __atomic_fetch_add(&s_n, 1u, __ATOMIC_RELAXED) & (RING - 1);
    s_name[i] = name;
    s_vram[i] = 0;
    s_thread[i] = xhw_thread_self();
}

/* the interpreter's entries and calls (interp.c), in every build: the code a
 * crash in interpreted code was running */
void xrc_trace_vram(uint32_t vram) {
    unsigned i = __atomic_fetch_add(&s_n, 1u, __ATOMIC_RELAXED) & (RING - 1);
    s_name[i] = 0;
    s_vram[i] = vram;
    s_thread[i] = xhw_thread_self();
}

/* the crash guard's extra report (xhw_crash_set_extra): the last 64, oldest first */
void xrc_trace_dump(void);
void xu_crash_regs(void);   /* ultra/thread.c */
void xrc_interp_crash_report(void);   /* interp.c */
static void crash_report(void) {
    xu_crash_regs();
    xrc_interp_crash_report();
    xrc_trace_dump();
}
void xrc_trace_dump(void) {
    unsigned n = s_n, k, first = n > 64 ? n - 64 : 0;
    if (!n) return;
    xhw_logf("[TRACE] the last %u functions entered (of %u), oldest first:", n - first, n);
    for (k = first; k < n; k++) {
        unsigned i = k & (RING - 1);
        if (s_name[i]) xhw_logf("[TRACE]   %s (thread %p)", s_name[i], s_thread[i]);
        else if (s_vram[i]) xhw_logf("[TRACE]   %08x, interpreted (thread %p)", (unsigned)s_vram[i], s_thread[i]);
    }
    {   /* the calling (crashing) thread's own last 48, which another thread's loop can crowd out above */
        void* me = xhw_thread_self();
        unsigned idx[48], m = 0, lim = n > RING ? n - RING : 0;
        for (k = n; k > lim && m < 48; k--) {
            unsigned i = (k - 1) & (RING - 1);
            if (s_thread[i] == me && k - 1 < first) idx[m++] = i;
        }
        if (m) xhw_logf("[TRACE] this thread (%p), before those, oldest first:", me);
        while (m--) {
            unsigned i = idx[m];
            if (s_name[i]) xhw_logf("[TRACE]   %s", s_name[i]);
            else if (s_vram[i]) xhw_logf("[TRACE]   %08x, interpreted", (unsigned)s_vram[i]);
        }
    }
}

/* a thread's own last n entries, oldest first (the stall report's, per N64 thread) */
void xrc_trace_thread(void* thread, unsigned n);
void xrc_trace_thread(void* thread, unsigned n) {
    unsigned k, m = 0, idx[64], end = s_n, lim = end > RING ? end - RING : 0;
    if (n > 64) n = 64;
    for (k = end; k > lim && m < n; k--) {
        unsigned i = (k - 1) & (RING - 1);
        if (s_thread[i] == thread) idx[m++] = i;
    }
    while (m--) {
        unsigned i = idx[m];
        if (s_name[i]) xhw_logf("[TRACE]       %s", s_name[i]);
        else if (s_vram[i]) xhw_logf("[TRACE]       %08x, interpreted", (unsigned)s_vram[i]);
    }
}

void xrc_trace_init(void) { xhw_crash_set_extra(crash_report); }
