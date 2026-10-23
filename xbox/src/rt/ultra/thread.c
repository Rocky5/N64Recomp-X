/* thread.c - threads, message queues, events, the interrupt mask (PLAN.md 3.3).
 *
 * Every N64 thread is a kernel thread at a fixed level in N64 order (the
 * Xbox kernel preempts strictly by priority, as libultra did); its body is a
 * fresh recomp_context (a0 = arg, sp = the game's own stack in RDRAM) that
 * calls the entry through get_function. Message queues block for real under
 * one recursive lock (libultra's "interrupts off"), each blocked thread on
 * its own event; the queue's fields are kept in RDRAM in N64 layout after
 * every change, since game code reads validCount inline. From Wings64-X's
 * os_shim.c; the data model is new (structs in RDRAM, host state in side
 * tables keyed by the struct's N64 address). */
#include <stdio.h>

#include "ultra.h"

static struct { uint32_t mq, msg; } s_events[OS_NUM_EVENTS];
__attribute__((weak)) void xrc_gfx_dp_taken(void) {}   /* gfx.c: XGFX_PROBE_DPSYNC */

/* N64 threads with a record (and, once started, a host thread with a
 * committed 128 KB stack): Pokemon Snap's object threads on its Beach
 * course passed 32 at once ("Too many N64 threads"); only a game that makes
 * them pays for them. -DXRC_MAX_THREADS=32: as before */
#ifndef XRC_MAX_THREADS
#define XRC_MAX_THREADS 64
#endif
#define MAX_THREADS XRC_MAX_THREADS
/* osStopThread(self) waits for an osStartThread instead of parking for
 * good: HAL's object manager (Pokemon Snap) runs each object's thread a
 * frame at a time that way, and destroys and creates them again on the
 * same OSThread structs: an osCreateThread over a stopped thread makes the
 * next osStartThread run the new entry on the same kernel thread (back to
 * thread_body's start). Kill switch: -DXRC_THREAD_RESUME=0 (parks). */
#ifndef XRC_THREAD_REUSE
#define XRC_THREAD_REUSE 1
#endif
#ifndef XRC_THREAD_RESUME
#define XRC_THREAD_RESUME 1
#endif
#define HOST_STACK (128 * 1024)
/* -DXRC_THREAD_TRACE=1 (diagnosis): every create, start, stop and destroy
 * logged with the calling thread's id, the first 400 */
#ifndef XRC_THREAD_SP16
#define XRC_THREAD_SP16 1
#endif
#ifndef XRC_THREAD_TRACE
#define XRC_THREAD_TRACE 0
#endif
/* osStopThread / osDestroyThread of another thread: it stops at its next
 * block (woken there if it is blocked already) and never runs on; created
 * and started again, it starts over at its entry (Mario Party 2 destroys
 * its audio thread, closes the audio library and makes it again: the old
 * thread woke on a retrace and read the freed globals). The N64 stops it at
 * once; one CPU means it is not running then, only ready or blocked.
 * Kill switch: -DXRC_THREAD_HALT=0 (destroy leaves it running, stop is
 * ignored, as before). */
#ifndef XRC_THREAD_HALT
#define XRC_THREAD_HALT 1
#endif
static unsigned s_trace;
#define TRACE(...) do { if (XRC_THREAD_TRACE && s_trace++ < 400) xhw_logf(__VA_ARGS__); } while (0)   /* the kernel thread's; the game's own stack is in RDRAM */

/* ---- the lock ---- */
static xhw_mutex* s_lock;
static volatile int s_depth;   /* the owner's recursion count (only the owner changes it) */

static int s_perf = -1;   /* [PERF2] (-DXHW_PERF_DETAIL=1): lock takes and the boost's level changes counted */
void xu_lock(void) {
    if (!s_lock) s_lock = xhw_mutex_create();
    if (s_perf < 0) s_perf = xhw_perf_detail();
    if (s_perf) xhw_perf_add(XHW_PERF_LOCKS, 0);
    xhw_mutex_lock(s_lock);
    s_depth++;
}
void xu_unlock(void) {
    s_depth--;
    xhw_mutex_unlock(s_lock);
}

/* ---- threads ---- */
typedef struct {
    uint32_t t;                 /* the OSThread's N64 address; 0 for the boot thread */
    xhw_thread* kt;
    xhw_event* ev;              /* wake-up */
    volatile uint32_t wait;     /* what it sleeps on (a queue address, |1 for "full"), 0 when running */
    uint32_t entry, arg, sp;
    int ints_off;               /* __osDisableInt / cop0 status IE clear: holds one lock level */
    int mask_off;               /* osSetIntMask(OS_IM_NONE): 1 took that level, 2 found it taken */
    volatile int stopped;       /* in osStopThread, until an osStartThread */
    volatile int fresh;         /* osCreateThread again on this OSThread: start over at the entry */
    volatile int halted;        /* another thread stopped or destroyed it: it stops at its next block */
    volatile int faulting;      /* stopped in xu_fault_wait, for the game's fault thread */
    volatile int dead;          /* destroyed, or its entry returned: its record (and host thread, once
                                 * stopped) may serve the next new OSThread */
    void* jb[5];                /* thread_body's restart point (__builtin_setjmp) */
    recomp_context* ctx;        /* thread_body's: its sp for interp.c's exception code */
    volatile unsigned sleeps;   /* times it went to sleep (xu_send's yield) */
    uint32_t gp;                /* its creator's gp (XRC_THREAD_GP) */
    int fr;                     /* it runs with Status.FR (recomp.h XRC_FR) */
    void* starter;              /* the Thr that started it (XRC_START_DEFER) */
    unsigned starter_sleeps;    /* its sleeps then */
    int rs;                     /* XRC_RUN_TOKEN: RS_NONE, RS_READY (runnable), RS_BLOCKED (on an N64 object) */
    unsigned seq;               /* its place among the ready threads of its priority (FIFO) */
    xhw_event* tev;             /* the run token handed to it */
} Thr;
enum { RS_NONE, RS_READY, RS_BLOCKED };
static Thr s_thr[MAX_THREADS];
static volatile int s_started;   /* game threads the runtime runs (osStartThread, a dispatch: xu_start_dispatched) */
static volatile int s_nthr;
static uint32_t s_tls = 0xFFFFFFFFu;

static int fixed_level(int pri);
/* Distinct N64 priorities that the fixed map below puts on one level (9 and
 * 10 on 8, 100 and 110 on 10) ran side by side, where libultra runs the
 * higher alone (Castlevania: Legacy of Darkness raced: each boot ended
 * differently). The priorities in use are ranked: each keeps its fixed
 * level unless one below it took that level, then it moves up one (and
 * down again from the top, levels 3..14), so each distinct priority gets
 * its own level while there are at most 12; a game whose priorities do not
 * collide keeps the fixed map exactly. -DXRC_RANK_LEVELS=0: the fixed map */
#ifndef XRC_RANK_LEVELS
#define XRC_RANK_LEVELS 0
#endif
#define RANK_MAX 12
static int s_rank_pri[RANK_MAX], s_rank_lv[RANK_MAX], s_rank_n;

int xu_level_of(int pri) {
    int i;
    if (XRC_RANK_LEVELS && pri > 0)
        for (i = 0; i < s_rank_n; i++)
            if (s_rank_pri[i] == pri) return s_rank_lv[i];
    return fixed_level(pri);
}

static int fixed_level(int pri) {
    /* N64 priorities 0..255 onto levels 2..14, in order (Wings64-X's map,
     * from Pilotwings 64's threads: idle 0, app 10, kernel 12, audio 110,
     * scheduler 127, PI manager 150, render/fault 250, VI manager 254). The
     * watchdog sits above them at 15. */
    if (pri <= 0) return 2;
    if (pri <= 10) return 8;
    if (pri <= 12) return 9;
    if (pri <= 110) return 10;
    if (pri <= 127) return 11;
    if (pri <= 150) return 12;
    if (pri <= 250) return 13;
    return 14;
}

xhw_thread* xu_spawn(void (*fn)(void*), void* arg, int level, uint32_t stack) {
    xhw_thread* t = xhw_thread_spawn(fn, arg, level, stack);
    if (!t) xhw_fatal("Out of threads", "A runtime thread could not be started.");
    return t;
}

static Thr* find(uint32_t t) {
    int i;
    for (i = 0; i < s_nthr; i++)
        if (s_thr[i].t == t) return &s_thr[i];
    return NULL;
}

static Thr* add(uint32_t t) {
    Thr* d;
    if (s_nthr == MAX_THREADS) xrc_fatal_vram("Too many N64 threads", t, MAX_THREADS);
    d = &s_thr[s_nthr];
    memset(d, 0, sizeof *d);
    d->t = t;
    d->ev = xhw_event_create();
    d->tev = xhw_event_create();
    s_nthr++;
    if (s_nthr > 32 && !(s_nthr & 7)) xhw_logf("[OS] %d N64 thread records", s_nthr);
    return d;
}

/* the calling thread's record; runtime threads and the boot thread get one on first use */
static Thr* self(void) {
    Thr* d;
    if (s_tls == 0xFFFFFFFFu) s_tls = xhw_tls_alloc();
    d = (Thr*)xhw_tls_get(s_tls);
    if (d) return d;
    xu_lock();
    d = add(0);
    xu_unlock();
    if (!xhw_thread_self()) xhw_thread_adopt_self(8);
    d->kt = xhw_thread_self();
    xhw_tls_set(s_tls, d);
    return d;
}

/* libultra's __osRunningThread, at the address n64rx found in the thread
 * getters (-DXRC_RUNNING_THREAD): game code reads it inline (Conker's main
 * thread checks its own id through it; NULL+0x14 otherwise). Kept as the
 * N64 would have it: a thread that resumes writes itself, one that blocks
 * writes the highest-priority thread left runnable (the kernel preempts by
 * priority, as libultra did). Without the address: nothing written. */
#ifdef XRC_RUNNING_THREAD
static void running_is(uint32_t t) {
    if (t && xu_mapped(XRC_RUNNING_THREAD)) wr32(XRC_RUNNING_THREAD, t);
}
static void running_next(const Thr* me) {
    uint32_t best = 0;
    int32_t bp = -1;
    int i;
    for (i = 0; i < s_nthr; i++) {
        const Thr* d = &s_thr[i];
        int32_t p;
        if (d == me || !d->t || !d->kt || d->wait || d->stopped) continue;
        p = (int32_t)rd32(d->t + TH_PRIORITY);
        if (p > bp) bp = p, best = d->t;
    }
    running_is(best);
}
#else
#define running_is(t) ((void)0)
#define running_next(me) ((void)0)
#endif

static void make_ready(Thr* d);
static void make_blocked(Thr* d);
static void resched(void);
static void token_wait(Thr* me);
static void token_yield(Thr* me);

/* under the lock: wake every thread sleeping on `key` (they recheck); with
 * the run token each is ready, and the highest may take it */
static void wake_all(uint32_t key) {
    int i, n = 0;
    for (i = 0; i < s_nthr; i++)
        if (s_thr[i].wait == key) make_ready(&s_thr[i]), xhw_event_signal(s_thr[i].ev), n++;
    if (n) resched();
}

/* under the lock: sleep until woken; every level of the lock is let go
 * meanwhile (a thread blocked with interrupts "off" lets others run, as a
 * thread switch did on the N64) and taken back after */
static void stop_wait(Thr* me, int held);

static int pri_of(const Thr* d) { return d->t ? (int)(int32_t)rd32(d->t + TH_PRIORITY) : -1; }
static void rank_update(int extra);

/* ---- the run token (docs/roadmap.md B1) ----
 * libultra runs one thread at a time: the highest-priority runnable one,
 * alone until it blocks, FIFO among equals; an interrupt that readies a
 * higher one switches to it at once, unless interrupts are off. The kernel
 * time-slices threads that share a level and lets any lower level run while
 * a thread waits on a host lock (a log line, a file, the runtime's lock),
 * which the N64 never did: every XRC_START_DEFER / XRC_SEND_YIELD /
 * XRC_RANK_LEVELS switch is a patch over one case of that. With the token,
 * a game thread runs game code only while it holds the token. It gives it
 * up when it blocks on an N64 object (a queue, a stop, a fault, the idle
 * loop), never on a host wait. A thread made ready that outranks the holder
 * takes it: the holder waits at its next safe point (a runtime call), or at
 * once where the kernel's levels preempt it; with interrupts off the switch
 * waits for them to come on. The runtime's own threads (VI, timers, tasks,
 * audio, saves) are the interrupts and never hold it.
 * DIVERGES: none meant; a holder preempted by an interrupt runs on until its
 * next runtime call when the kernel gives it the CPU (a level shared with the
 * thread that took the token).
 * -DXRC_RUN_TOKEN=1 to use it; 0 (the default until a console set): the
 * kernel's levels alone, as before. */
#ifndef XRC_RUN_TOKEN
#define XRC_RUN_TOKEN 0
#endif
static Thr* volatile s_run;     /* the holder, or NULL when no game thread is runnable */
static unsigned s_seq;
static volatile int s_pend;     /* a higher thread was made ready while the holder had interrupts off */
static unsigned s_handoffs, s_late;

static Thr* tls_thr(void) { return s_tls == 0xFFFFFFFFu ? NULL : (Thr*)xhw_tls_get(s_tls); }
/* a game thread the token rules: from its osStartThread on (its host thread
 * may run before osStartThread has its handle) */
static int in_token(const Thr* d) { return XRC_RUN_TOKEN && d && d->t && d->rs != RS_NONE; }

/* under the lock: the highest-priority ready thread, the earliest made ready among equals */
static Thr* pick(void) {
    Thr* best = NULL;
    int bp = -1, i;
    for (i = 0; i < s_nthr; i++) {
        Thr* d = &s_thr[i];
        int p;
        if (d->rs != RS_READY) continue;
        p = pri_of(d);
        if (!best || p > bp || (p == bp && (int)(d->seq - best->seq) < 0)) best = d, bp = p;
    }
    return best;
}

/* under the lock: the token to the thread that should run now. A holder
 * that can still run keeps it unless a strictly higher priority is ready,
 * and keeps it while its interrupts are off (s_pend: ints_on looks again) */
static void resched(void) {
    Thr *best, *cur = s_run;
    if (!XRC_RUN_TOKEN) return;
    s_pend = 0;
    best = pick();
    if (best == cur) return;
    if (cur && cur->rs == RS_READY) {
        if (!best || pri_of(best) <= pri_of(cur)) return;
        if (cur->ints_off) {
            s_pend = 1;
            return;
        }
        cur->seq = ++s_seq;   /* preempted: behind the others of its priority, as __osEnqueueThread puts it */
    }
    s_run = best;
    if (best) s_handoffs++, xhw_event_signal(best->tev);
}

/* under the lock */
static void make_ready(Thr* d) {
    if (!in_token(d) || d->rs == RS_READY) return;   /* (RS_NONE: not a started game thread) */
    d->rs = RS_READY;
    d->seq = ++s_seq;
}
/* under the lock: a thread started (osStartThread, a dispatch) joins the token's threads, ready */
static void token_join(Thr* d) {
    if (!XRC_RUN_TOKEN || !d->t) return;
    d->rs = RS_BLOCKED;
    make_ready(d);
    resched();
}
static void make_blocked(Thr* d) {
    if (!in_token(d)) return;
    d->rs = RS_BLOCKED;
    if (s_run == d) s_run = NULL;
    resched();
}

/* before a game thread runs game code again: it waits for the token with
 * every level of the lock let go (taken back after). A thread created again
 * meanwhile (osCreateThread over it) starts over at its entry */
static void token_wait(Thr* me) {
    int n, i;
    if (!in_token(me)) return;
    xu_lock();
    if (!me->halted) make_ready(me);
    resched();
    n = s_depth;
    while (s_run != me) {
        for (i = 0; i < n; i++) xu_unlock();
        xhw_event_wait(me->tev, XHW_INFINITE);
        if (XRC_THREAD_RESUME && me->fresh && s_run == me) __builtin_longjmp(me->jb, 1);   /* no level held */
        for (i = 0; i < n; i++) xu_lock();
    }
    xu_unlock();
}

/* a safe point in a game thread (a runtime call that may have readied a
 * higher thread, interrupts back on): on with the token, or wait for it */
static void token_yield(Thr* me) {
    if (!in_token(me) || me->rs != RS_READY) return;
    if (s_run == me && !s_pend) return;
    if (s_run != me) s_late++;   /* taken while it ran: by an interrupt, or a level the kernel shares */
    token_wait(me);
}

/* hooks.c's pause_self: the game's idle loop blocks this thread for good */
void xu_token_idle(void) {
    Thr* me = tls_thr();
    if (!in_token(me)) return;
    xu_lock();
    make_blocked(me);
    xu_unlock();
}

/* the token and the thread layer's state, for the stall report and the host test */
unsigned xu_token_handoffs(void) { return s_handoffs; }
int xu_token_holder(void) { return s_run && s_run->t ? (int)(int32_t)rd32(s_run->t + TH_ID) : -1; }

/* another thread already blocked on `key` at this thread's priority or above:
 * libultra hands a message to the first such waiter, not to a thread that
 * has yet to block */
static int waiter_first(const Thr* me, uint32_t key) {
    int i, p = pri_of(me);
    for (i = 0; i < s_nthr; i++)
        if (&s_thr[i] != me && s_thr[i].wait == key && pri_of(&s_thr[i]) >= p) return 1;
    return 0;
}

static void sleep_on(Thr* me, uint32_t key) {
    int n = s_depth, i;
    /* sp.c: a task this thread started is done now. Its events may be what
     * this thread was about to wait for (Mario Party's scheduler waits for
     * SP done right after starting the task): back to the caller's check,
     * unless a thread that blocked there first gets it (nnsched's audio
     * thread, waiting for the graphics task it yielded) */
    if (xu_rcp_flush(me) && !waiter_first(me, key)) return;
    me->wait = key;
    me->sleeps++;
    running_next(me);
    make_blocked(me);   /* the run token to the next */
    for (i = 0; i < n; i++) xu_unlock();
    if (!me->halted) xhw_event_wait(me->ev, XHW_INFINITE);
    if (XRC_THREAD_HALT && me->halted) {   /* stopped or destroyed meanwhile: no lock held here */
        me->wait = 0;
        me->halted = 0;
        stop_wait(me, 0);                  /* started again: on from here, or (created again) its entry */
    } else if (XRC_THREAD_HALT && me->fresh && XRC_THREAD_RESUME) {
        me->wait = 0;                      /* created and started again before it got here */
        __builtin_longjmp(me->jb, 1);
    }
    token_wait(me);   /* no level held here */
    for (i = 0; i < n; i++) xu_lock();
    me->wait = 0;
    running_is(me->t);
}

static void park(void) {
    xhw_event* e = xhw_event_create();
    for (;;) xhw_event_wait(e, XHW_INFINITE);
}

/* a stopped N64 thread waits here for osStartThread; created again in the
 * meantime, it starts over at its (new) entry */
/* `held`: the levels of the lock it holds (interrupts off), let go while it
 * waits: a thread that stopped itself with interrupts off kept the lock, and
 * the VI thread, the timers and every queue waited for good
 * (docs/review-2026-10.md 5b.5). -DXRC_STOP_UNLOCK=0: kept, as before */
#ifndef XRC_STOP_UNLOCK
#define XRC_STOP_UNLOCK 1
#endif
static void stop_wait(Thr* me, int held) {
    int i;
    if (!XRC_STOP_UNLOCK) held = 0;
    if (!XRC_THREAD_RESUME || !me->t) {
        for (i = 0; i < held; i++) xu_unlock();
        park();
    }
    xu_lock();
    me->stopped = 1;
    running_next(me);
    make_blocked(me);
    xu_unlock();
    for (i = 0; i < held; i++) xu_unlock();
    while (me->stopped) xhw_event_wait(me->ev, XHW_INFINITE);
    if (me->fresh) __builtin_longjmp(me->jb, 1);   /* no level held: thread_body starts it over */
    token_wait(me);
    for (i = 0; i < held; i++) xu_lock();
    wr16(me->t + TH_STATE, OS_STATE_RUNNING);
}

/* A thread started by a higher N64 priority that shares its kernel level
 * waits to begin until that thread has slept once (or 200 ms): libultra
 * runs the higher alone, and boot code goes on after its osStartThread
 * calls expecting the threads it started to wait. Star Fox 64's main thread
 * (100) starts its graphics thread (40), then makes the message queues; the
 * graphics thread, run beside it, sent its first DMA's message into a
 * queue that was then made again, and waited on it for good.
 * -DXRC_START_DEFER=1 to use it; 0 (the default for now): the new thread
 * runs at once, as before. A lower kernel level is not enough either: a
 * host block in the starter (a log line, the lock) lets any lower level run,
 * and with XRC_RANK_LEVELS (every priority its own level) Star Fox 64's
 * graphics, timer and serial threads all slept on queues main then made
 * again. -DXRC_START_DEFER=2: the wait whatever the levels */
#ifndef XRC_START_DEFER
#define XRC_START_DEFER 0   /* off until tested on more games (Star Fox 64: xemu) */
#endif
static void start_defer(Thr* d) {
    Thr* s = (Thr*)d->starter;
    uint64_t t0;
    if (!XRC_START_DEFER || !s || !s->t || !d->t) return;
    if (pri_of(s) <= pri_of(d)) return;
    if (XRC_START_DEFER < 2 && xu_level_of(pri_of(s)) != xu_level_of(pri_of(d))) return;
    t0 = xhw_time_us();
    /* or it lowers its priority below this one's (an idle thread that drops to 0 and spins) */
    while (s->sleeps == d->starter_sleeps && !s->wait && !s->stopped && !s->dead && pri_of(s) > pri_of(d) &&
           xhw_time_us() - t0 < 200000u)
        xhw_yield();
}

/* A new thread starts with its creator's gp: code built with gp-relative
 * data (Starshot's `sw v0, 0xFC0(gp)`, its entry code setting gp to
 * 0x800B8500 before main) wrote through gp 0, page 0 of KUSEG, in its first
 * thread. gp is one value for the whole program in N64 games; libultra
 * leaves it alone in a new thread's context, so the threads run with the
 * gp the entry code set. Strands (strand.c) take the last one a thread was
 * created with (xu_program_gp). -DXRC_THREAD_GP=0: gp 0, as before */
#ifndef XRC_THREAD_GP
#define XRC_THREAD_GP 1
#endif
static uint32_t s_program_gp;
uint32_t xu_program_gp(void) { return XRC_THREAD_GP ? s_program_gp : 0u; }

static void thread_body(void* p) {
    Thr* d = (Thr*)p;
    recomp_context ctx;
    if (s_tls == 0xFFFFFFFFu) s_tls = xhw_tls_alloc();
    xhw_tls_set(s_tls, d);
    start_defer(d);
    if (XRC_THREAD_RESUME && __builtin_setjmp(d->jb)) {   /* created again (stop_wait) */
        d = (Thr*)xhw_tls_get(s_tls);
        xhw_logf("[OS] thread %d started again: entry %08x", (int)(int32_t)rd32(d->t + TH_ID), (unsigned)d->entry);
        /* no level of the lock is held on the way here: a thread destroyed
         * with interrupts off starts over with them on, at its own level
         * (its next ints_on unlocked as a non-owner: 5b.5) */
        if (XRC_STOP_UNLOCK && (d->ints_off || d->mask_off)) {
            d->ints_off = d->mask_off = 0;
            xhw_thread_set_level(d->kt, xu_level_of(pri_of(d)));
        }
    }
    d->fresh = 0;
    token_wait(d);   /* the run token before its first instruction */
    running_is(d->t);
    xrc_ctx_init(&ctx, d->sp);
    xrc_ctx_fr(&ctx, d->fr);
    if (XRC_THREAD_GP) ctx.r28 = (gpr)(int32_t)d->gp;
    d->ctx = &ctx;
    ctx.r4 = (gpr)(int32_t)d->arg;
    set_cop1_cs(0x01000000u);   /* FCSR as osCreateThread leaves it: FS (recomp.h XRC_FTZ), round to nearest */
    get_function((int32_t)d->entry)(xrc_rdram, &ctx);
    /* an N64 thread that returns stops (libultra's __osCleanupThread) */
    xhw_logf("[OS] thread %d returned", (int)(int32_t)rd32(d->t + TH_ID));
    wr16(d->t + TH_STATE, OS_STATE_STOPPED);
    d->dead = 1;
    stop_wait(d, d->ints_off ? s_depth : 0);
}

/* osCreateThread(t, id, entry, arg, sp, pri) */
ULTRA(osCreateThread) {
    uint32_t t = ARG(0);
    Thr* d;
    (void)rdram;
    xu_lock();
    d = find(t);
    if (!d && XRC_THREAD_REUSE && XRC_THREAD_RESUME) {
        /* DIVERGES: a destroyed thread's record and host thread, parked in
         * stop_wait, serve a new OSThread (Pokemon Snap makes and destroys
         * object threads by the dozen: "Too many N64 threads" at its first
         * course). -DXRC_THREAD_REUSE=0: a new record every time. */
        int i;
        for (i = 0; i < s_nthr; i++)
            if (s_thr[i].dead && s_thr[i].stopped && s_thr[i].kt && s_thr[i].t) {
                d = &s_thr[i];
                d->t = t;
                break;
            }
    }
    if (!d) d = add(t);
    d->dead = 0;
    if (d->kt && XRC_THREAD_RESUME) d->fresh = 1;   /* the old one is stopped: starts over */
    d->entry = ARG(2);
    d->arg = ARG(3);
    d->gp = (uint32_t)ctx->r28;
    if (d->gp) s_program_gp = d->gp;
    /* libultra starts the thread 16 bytes below the sp it is given
     * (osCreateThread: context.sp = sp - 16): the four argument words a
     * MIPS callee may store above its own frame. Without them a thread's
     * entry wrote its argument into the word just past its stack (Pokemon
     * Snap: the next object-thread stack's free-list link, corrupted with a
     * GObj pointer). -DXRC_THREAD_SP16=0: the sp as given. */
    d->sp = ARG(4) - (XRC_THREAD_SP16 ? 16u : 0u);
    {   /* the game's osCreateThread puts FR in the new context's Status (XRC_FR 1), or Rare's does
         * unless the id is negative, which it keeps negated (XRC_FR 2: Jet Force Gemini's FR = 0 thread) */
        int32_t id = (int32_t)ARG(1);
        d->fr = XRC_FR == 1 || XRC_FR == 2;   /* 3: libultra's FR = 0; the game's own mtc0 Status sets it */
        if (XRC_FR == 2 && id < 0) d->fr = 0, id = -id;
        wr32(t + TH_ID, (uint32_t)id);
    }
    wr32(t + TH_PRIORITY, ARG(5));
    TRACE("[OS] trace: create %08x id %d entry %08x pri %d (kt %d stopped %d wait %x) by %08x", (unsigned)t,
          (int)ARG(1), (unsigned)ARG(2), (int)ARG(5), d->kt != 0, d->stopped, (unsigned)d->wait, (unsigned)self()->t);
    wr16(t + TH_STATE, OS_STATE_STOPPED);
    wr32(t + TH_NEXT, 0);
    xu_unlock();
}

ULTRA(osStartThread) {
    uint32_t t = ARG(0);
    Thr* d = find(t);
    Thr* me = self();
    int pri;
    (void)rdram;
    if (!d) xrc_fatal_vram("osStartThread: unknown thread", t, 0);
    TRACE("[OS] trace: start %08x (kt %d stopped %d wait %x fresh %d) by %08x", (unsigned)t, d->kt != 0, d->stopped,
          (unsigned)d->wait, d->fresh, (unsigned)me->t);
    if (!d->kt) {
        pri = (int)(int32_t)rd32(t + TH_PRIORITY);
        wr16(t + TH_STATE, OS_STATE_RUNNABLE);
        rank_update(pri);
        d->starter = me;
        d->starter_sleeps = me->sleeps;
        xu_lock();
        token_join(d);
        xu_unlock();
        d->kt = xhw_thread_spawn(thread_body, d, xu_level_of(pri), HOST_STACK);
        if (!d->kt) xrc_fatal_vram("osStartThread: no kernel thread", t, 0);
        s_started++;
        xhw_logf("[OS] thread %d started: entry %08x, sp %08x, N64 priority %d -> level %d",
                 (int)(int32_t)rd32(t + TH_ID), (unsigned)d->entry, (unsigned)d->sp, pri, xu_level_of(pri));
    } else if (XRC_THREAD_RESUME && d->stopped) {
        wr16(t + TH_STATE, OS_STATE_RUNNABLE);
        xu_lock();
        d->stopped = 0;
        make_ready(d);
        resched();
        xu_unlock();
        xhw_event_signal(d->ev);
    } else if (XRC_THREAD_RESUME && d->faulting) {
        /* stopped faulted (xu_fault_wait): a fault thread that lets it go with
         * osStartThread (All-Star Baseball 2000's pager, beside the flag it
         * leaves set) makes it runnable, as libultra's does */
        wr16(t + TH_STATE, OS_STATE_RUNNABLE);
    } else if (d->halted) {   /* stopped by another thread, started again before it reached its block */
        wr16(t + TH_STATE, OS_STATE_RUNNABLE);
        xu_lock();
        d->halted = 0;
        make_ready(d);
        resched();
        xu_unlock();
        xhw_event_signal(d->ev);
    }
    if (in_token(me)) {   /* a higher one took the token: it runs first, as libultra dispatches it */
        token_yield(me);
        return;
    }
    /* a higher N64 priority runs first, as libultra dispatches it: where the
     * kernel levels are equal (50 and 51 share one) the caller waits until
     * it stops or blocks on a queue; a higher level preempts by itself */
    /* at most 200 ms: a started thread that waits by polling (a pause, an
     * osYieldThread loop) sets neither, and the starter never returned (5b.6) */
    if (XRC_THREAD_RESUME && me->t && (int32_t)rd32(t + TH_PRIORITY) > (int32_t)rd32(me->t + TH_PRIORITY) &&
        xu_level_of((int32_t)rd32(t + TH_PRIORITY)) == xu_level_of((int32_t)rd32(me->t + TH_PRIORITY))) {
        uint64_t t0 = xhw_time_us();
        while (!d->stopped && !d->wait && xhw_time_us() - t0 < 200000u) xhw_yield();
    }
    /* the boot code is not a thread: on the N64 the first osStartThread
     * dispatches and the boot code never runs again */
    if (me->t == 0 && !me->entry) {
        xhw_log("[OS] boot code done: the boot thread parks");
        park();
    }
}

/* interp.c: an eret outside the game's exception code, with k0 an OSThread
 * the runtime does not run: the game's own (unnamed) osStartThread
 * dispatching a thread it made with its own osCreateThread (a compressed
 * boot's libultra, before learning names it: Blast Corps). That thread is
 * started here from its context, as its first dispatch would run it: pc
 * (the entry), sp, a0 (the argument). 1 when it was.
 * DIVERGES: none meant. -DXRC_DISPATCH_START=0: the eret is only a return. */
#ifndef XRC_DISPATCH_START
#define XRC_DISPATCH_START 1
#endif
enum { TH_CTX_A0 = 0x3C, TH_CTX_SP = 0xF4, TH_CTX_PC = 0x11C };
int xu_threads_started(void) { return s_started; }

int xu_start_dispatched(uint32_t t) {
    Thr* d;
    uint32_t pc, sp;
    int pri;
    if (!XRC_DISPATCH_START || (t & 0xE0000003u) != 0x80000000u || (t & 0x1FFFFFFFu) + 0x130u > xrc_rdram_size)
        return 0;
    pc = rd32(t + TH_CTX_PC);
    sp = rd32(t + TH_CTX_SP);
    if ((pc & 0xE0000003u) != 0x80000000u || (sp & 0xE0000000u) != 0x80000000u) return 0;
    xu_lock();
    d = find(t);
    if (d && d->kt) {   /* the runtime runs it already: the eret only returns */
        xu_unlock();
        return 0;
    }
    if (!d) d = add(t);
    d->entry = pc;
    d->arg = rd32(t + TH_CTX_A0);
    d->sp = sp;
    d->gp = rd32(t + 0xEC);   /* its context's gp (it ran with 0: review 5b.19) */
    d->fr = XRC_FR && (rd32(t + 0x118) & 0x04000000u);   /* and its Status's FR */
    pri = (int)(int32_t)rd32(t + TH_PRIORITY);
    wr16(t + TH_STATE, OS_STATE_RUNNABLE);
    rank_update(pri);
    token_join(d);
    d->kt = xhw_thread_spawn(thread_body, d, xu_level_of(pri), HOST_STACK);
    xu_unlock();
    if (!d->kt) return 0;
    s_started++;
    xhw_logf("[OS] thread %d dispatched by the game's own code: started at %08x, sp %08x, N64 priority %d",
             (int)(int32_t)rd32(t + TH_ID), (unsigned)pc, (unsigned)sp, pri);
    return 1;
}

/* another thread stopped: at its next block, woken if it is blocked (XRC_THREAD_HALT) */
static void halt_other(uint32_t t, const char* what) {
    Thr* d = find(t);
    if (!XRC_THREAD_HALT || !XRC_THREAD_RESUME) {
        if (what) xhw_logf("[OS] %s of another thread (%08x): not supported, ignored", what, (unsigned)t);
        return;
    }
    if (!d || !d->kt || d->stopped) return;   /* never started, or stopped already */
    wr16(t + TH_STATE, OS_STATE_STOPPED);
    xu_lock();
    d->halted = 1;
    /* ready, waiting for the run token: off the run queue, as libultra takes
     * a stopped thread off it (osStartThread puts it back) */
    if (in_token(d) && d->rs == RS_READY && s_run != d) d->rs = RS_BLOCKED;
    xu_unlock();
    xhw_event_signal(d->ev);                  /* blocked: it wakes and stops */
}

ULTRA(osStopThread) {
    uint32_t t = ARG(0);
    (void)rdram;
    TRACE("[OS] trace: stop %08x by %08x", (unsigned)t, (unsigned)self()->t);
    if (!t || t == self()->t) {
        Thr* me = self();
        if (me->t) wr16(me->t + TH_STATE, OS_STATE_STOPPED);
        stop_wait(me, me->ints_off ? s_depth : 0);
        return;
    }
    halt_other(t, "osStopThread");
}

ULTRA(osDestroyThread) {
    uint32_t t = ARG(0);
    (void)rdram;
    static unsigned s_logged;
    TRACE("[OS] trace: destroy %08x by %08x", (unsigned)t, (unsigned)self()->t);
    if (!t || t == self()->t) {
        if (t) wr16(t + TH_STATE, OS_STATE_STOPPED);
        self()->dead = 1;
        stop_wait(self(), self()->ints_off ? s_depth : 0);
        return;
    }
    /* another thread: stopped (HAL's object manager stops them first) or
     * stopped now, until it is created and started again */
    {
        Thr* o = find(t);
        if (o) o->dead = 1;
    }
    if (s_logged++ < 4) xhw_logf("[OS] osDestroyThread of another thread (%08x): it stops", (unsigned)t);
    halt_other(t, NULL);
}

/* the ranking again from the live threads' priorities (and `extra`, one
 * about to be used), each host thread moved to its level; a thread holding
 * interrupts off (boosted to 14) is left until it lets go */
static void rank_update(int extra) {
    int pri[MAX_THREADS + 1], n = 0, i, j, lv[RANK_MAX], old_n = s_rank_n;
    if (!XRC_RANK_LEVELS) return;
    for (i = 0; i < s_nthr; i++)
        if (s_thr[i].t && !s_thr[i].dead) pri[n++] = pri_of(&s_thr[i]);
    if (extra > 0) pri[n++] = extra;
    for (i = 1; i < n; i++)   /* sorted, distinct, > 0 */
        for (j = i; j > 0 && pri[j - 1] > pri[j]; j--) {
            int t = pri[j];
            pri[j] = pri[j - 1], pri[j - 1] = t;
        }
    for (i = j = 0; i < n; i++)
        if (pri[i] > 0 && (!j || pri[j - 1] != pri[i])) pri[j++] = pri[i];
    n = j;
    if (n > RANK_MAX) {
        s_rank_n = 0;   /* too many to keep apart: the fixed map */
    } else {
        for (i = 0; i < n; i++) {
            lv[i] = fixed_level(pri[i]);
            if (i && lv[i] <= lv[i - 1]) lv[i] = lv[i - 1] + 1;
        }
        for (i = n - 1; i >= 0; i--) {
            if (lv[i] > 14) lv[i] = 14;
            if (i < n - 1 && lv[i] >= lv[i + 1]) lv[i] = lv[i + 1] - 1;
            if (lv[i] < 3) lv[i] = 3;
        }
        s_rank_n = 0;
        for (i = 0; i < n; i++) s_rank_pri[i] = pri[i], s_rank_lv[i] = lv[i];
        s_rank_n = n;
    }
    {
        static int s_said;
        int moved = 0;
        for (i = 0; i < n && i < RANK_MAX; i++) moved += lv[i] != fixed_level(pri[i]);   /* (lv holds RANK_MAX: 5b.22) */
        if (moved && s_said < 8 && (n != old_n || !s_said)) {
            s_said++;
            xhw_logf("[OS] thread levels: %d priorities, %d moved off the fixed map to keep them apart", n, moved);
        }
    }
    for (i = 0; i < s_nthr; i++) {
        Thr* d = &s_thr[i];
        if (d->t && d->kt && !d->dead && !d->ints_off) xhw_thread_set_level(d->kt, xu_level_of(pri_of(d)));
    }
}

#ifndef XRC_PRI_LOCKED
#define XRC_PRI_LOCKED 1
#endif
#ifndef XRC_INTS_BOOST
#define XRC_INTS_BOOST 1
#endif
ULTRA(osSetThreadPri) {
    uint32_t t = ARG(0);
    int pri = (int)(int32_t)ARG(1);
    Thr* d = t ? find(t) : self();
    (void)rdram;
    if (!d) return;
    /* under the lock (rank_update reads every record), and a thread holding
     * interrupts off stays at 14 until ints_on puts it at its new level: it
     * dropped from 14 to 8 inside its critical section, Kirby 64's deadlock
     * again for any game that re-prioritises there (5b.7). -DXRC_PRI_LOCKED=0:
     * as before */
    if (XRC_PRI_LOCKED) xu_lock();
    if (d->t) wr32(d->t + TH_PRIORITY, (uint32_t)pri);
    rank_update(0);
    if (d->kt && !(XRC_PRI_LOCKED && XRC_INTS_BOOST && d->ints_off)) xhw_thread_set_level(d->kt, xu_level_of(pri));
    resched();   /* a lowered holder or a raised ready thread: the token moves */
    if (XRC_PRI_LOCKED) xu_unlock();
    token_yield(tls_thr());
}

ULTRA(osGetThreadPri) {
    uint32_t t = ARG(0);
    Thr* d = t ? find(t) : self();
    (void)rdram;
    RET(d && d->t ? rd32(d->t + TH_PRIORITY) : 0);
}

ULTRA(osGetThreadId) {
    uint32_t t = ARG(0);
    Thr* d = t ? find(t) : self();
    (void)rdram;
    RET(d && d->t ? rd32(d->t + TH_ID) : 0);
}

ULTRA(osYieldThread) {
    Thr* me = tls_thr();
    (void)rdram;
    (void)ctx;
    if (in_token(me) && me->rs == RS_READY) {   /* behind the others of its priority, as libultra's yield */
        xu_lock();
        me->seq = ++s_seq;
        if (s_run == me) s_run = NULL;
        resched();
        xu_unlock();
        token_wait(me);
        return;
    }
    xhw_yield();
}

/* gfx.c: a game thread about to wait for the renderer's lock, which the BACK
 * menu holds for as long as it is open, lets go of every level of the
 * runtime lock it holds with interrupts off, as it would at a block, and
 * takes them back after (xu_lock_take). Mario Tennis's graphics thread
 * started its task with interrupts off (__osDisableInt at 0x8003274C, then
 * osSpTaskStartGo at 0x80032790): it waited in xrc_gfx_task holding the
 * runtime lock, the VI thread waited for that lock, and the menu (fed from
 * the VI thread) took no more input and never closed (red, vmt7, retrace
 * 1480 for good). A thread the runtime does not know holds nothing here. */
int xu_lock_drop(void);
int xu_lock_drop(void) {
    Thr* me = s_tls == 0xFFFFFFFFu ? NULL : (Thr*)xhw_tls_get(s_tls);
    int n = me && me->ints_off ? s_depth : 0, i;
    for (i = 0; i < n; i++) xu_unlock();
    return n;
}
void xu_lock_take(int n);
void xu_lock_take(int n) {
    while (n-- > 0) xu_lock();
}

/* more2.c's __osEnqueueAndYield: a pause with every level of the lock let
 * go when the thread holds it (interrupts "off", as libultra's callers do),
 * taken back after */
void xu_pause_unlocked(uint32_t ms);
void xu_pause_unlocked(uint32_t ms) {
    Thr* me = self();
    int n = me->ints_off ? s_depth : 0, i;
    if (in_token(me)) {   /* a wait on an N64 object, polled: the token to the others meanwhile */
        xu_lock();
        make_blocked(me);
        xu_unlock();
        if (!ms) ms = 1;   /* a yield would take it straight back */
    }
    for (i = 0; i < n; i++) xu_unlock();
    if (ms) xhw_sleep_ms(ms);
    else xhw_yield();
    token_wait(me);
    for (i = 0; i < n; i++) xu_lock();
}

/* ---- message queues ---- */
ULTRA(osCreateMesgQueue) {
    uint32_t mq = ARG(0);
    static unsigned s_again;
    int i;
    (void)rdram;
    /* a queue made again over messages or a sleeper: a thread used it before
     * its maker got there (the boot race of threads run beside their starter) */
    if ((int32_t)rd32(mq + MQ_VALIDCOUNT) > 0 && (int32_t)rd32(mq + MQ_VALIDCOUNT) <= (int32_t)rd32(mq + MQ_MSGCOUNT) &&
        rd32(mq + MQ_MSG) == ARG(1) && s_again < 8) {
        s_again++;
        xhw_logf("[OS] queue %08x made again over %d message(s): a thread sent before its maker made it",
                 (unsigned)mq, (int)(int32_t)rd32(mq + MQ_VALIDCOUNT));
    }
    for (i = 0; i < s_nthr && s_again < 8; i++)
        if (s_thr[i].t && !s_thr[i].dead && (s_thr[i].wait & ~1u) == mq) {
            s_again++;
            xhw_logf("[OS] queue %08x made again with N64 thread %08x asleep on it", (unsigned)mq, (unsigned)s_thr[i].t);
        }
    wr32(mq + MQ_MTQUEUE, 0);
    wr32(mq + MQ_FULLQUEUE, 0);
    wr32(mq + MQ_VALIDCOUNT, 0);
    wr32(mq + MQ_FIRST, 0);
    wr32(mq + MQ_MSGCOUNT, ARG(2));
    wr32(mq + MQ_MSG, ARG(1));
}

/* A message that wakes a thread of higher N64 priority switches to it at
 * once on the N64 (the sender runs again when it blocks). Where the two
 * share a kernel level (9 and 10, 100 and 110 do: xu_level_of) the sender
 * kept running beside it; Castlevania: Legacy of Darkness raced that way
 * (its runs ended differently each boot). The sender yields until the
 * woken thread sleeps again, at most 20 ms; not with interrupts off (the
 * woken thread needs the lock). -DXRC_SEND_YIELD=0: as before */
#ifndef XRC_SEND_YIELD
#define XRC_SEND_YIELD 0
#endif
static void send_yield(uint32_t mq) {
    Thr* me;
    int i, n = 0, lv, p;
    Thr* w[4];
    unsigned gen[4];
    uint64_t t0;
    if (!XRC_SEND_YIELD || s_tls == 0xFFFFFFFFu) return;
    me = (Thr*)xhw_tls_get(s_tls);
    if (!me || !me->t || me->ints_off || s_depth > 0) return;   /* the lock held (by anyone): no wait under it */
    p = pri_of(me);
    lv = xu_level_of(p);
    for (i = 0; i < s_nthr && n < 4; i++) {
        Thr* d = &s_thr[i];
        if (d != me && d->t && d->kt && d->wait == mq && pri_of(d) > p && xu_level_of(pri_of(d)) == lv)
            w[n] = d, gen[n] = d->sleeps, n++;
    }
    if (!n) return;
    t0 = xhw_time_us();
    for (i = 0; i < n; i++)
        while (w[i]->sleeps == gen[i] && !w[i]->stopped && !w[i]->dead && xhw_time_us() - t0 < 20000u) xhw_yield();
}

#ifdef XRC_WATCH_QUEUE
/* -DXRC_WATCH_QUEUE2/3=<N64 address>: more queues in the same watch (Donkey Kong 64's audio thread
 * between its timer queue, the scheduler's and its done queue) */
static int watched_queue(uint32_t mq) {
    return mq == (uint32_t)(XRC_WATCH_QUEUE)
#ifdef XRC_WATCH_QUEUE2
        || mq == (uint32_t)(XRC_WATCH_QUEUE2)
#endif
#ifdef XRC_WATCH_QUEUE3
        || mq == (uint32_t)(XRC_WATCH_QUEUE3)
#endif
        ;
}
#endif
int xu_send(uint32_t mq, uint32_t msg, int block, int jam) {
    Thr* me = block ? self() : NULL;
    int32_t count, valid, first;
#ifdef XRC_WATCH_QUEUE
    if (watched_queue(mq)) {   /* diagnosis: what goes into one queue, from whom */
        static unsigned s_n;
        Thr* who = self();
#ifndef XRC_WATCH_QUEUE_ALL   /* -DXRC_WATCH_QUEUE_ALL=1: every message both ways (the first 20000) */
#define XRC_WATCH_QUEUE_ALL 0
#endif
        if (((s_n < 8 || msg != 1u) && s_n < 64) || s_n % 600 == 0 || (XRC_WATCH_QUEUE_ALL && s_n < 20000))
            xhw_logf("[OS] queue %08x <- %08x by N64 thread %08x (ra %08x)%s #%u valid %u, retrace %u", (unsigned)mq, (unsigned)msg,
                     (unsigned)who->t, who->ctx ? (unsigned)who->ctx->r31 : 0u, jam ? " jammed" : "", s_n, (unsigned)rd32(mq + MQ_VALIDCOUNT),
                     xu_retrace());
        s_n++;
    }
#endif
    xu_lock();
    for (;;) {
        count = (int32_t)rd32(mq + MQ_MSGCOUNT);
        valid = (int32_t)rd32(mq + MQ_VALIDCOUNT);
        if (valid < count) break;
        if (!block || count <= 0) {
            xu_unlock();
            return -1;
        }
        sleep_on(me, mq | 1);
    }
    first = (int32_t)rd32(mq + MQ_FIRST);
    if (jam) {
        first = (first + count - 1) % count;
        wr32(mq + MQ_FIRST, (uint32_t)first);
        wr32(rd32(mq + MQ_MSG) + 4 * (uint32_t)first, msg);
    } else {
        wr32(rd32(mq + MQ_MSG) + 4 * (uint32_t)((first + valid) % count), msg);
    }
    wr32(mq + MQ_VALIDCOUNT, (uint32_t)(valid + 1));
    wake_all(mq);
    xu_unlock();
    if (XRC_RUN_TOKEN) token_yield(tls_thr());   /* a game thread that woke a higher one: it runs first */
    else send_yield(mq);
    return 0;
}

ULTRA(osSendMesg) {
    (void)rdram;
    RET(xu_send(ARG(0), ARG(1), (int)ARG(2) == OS_MESG_BLOCK, 0));
}

ULTRA(osJamMesg) {
    (void)rdram;
    RET(xu_send(ARG(0), ARG(1), (int)ARG(2) == OS_MESG_BLOCK, 1));
}

ULTRA(osRecvMesg) {
    uint32_t mq = ARG(0), out = ARG(1);
    int block = (int)ARG(2) == OS_MESG_BLOCK;
    Thr* me = block ? self() : NULL;
    int32_t valid, first, count;
    (void)rdram;
#ifdef XRC_WATCH_RECV
    void xrc_trace_dump(void);   /* trace.c */
    if (mq == (uint32_t)(XRC_WATCH_RECV) && block && !rd32(mq + MQ_VALIDCOUNT)) {   /* diagnosis: who waits there */
        static unsigned s_n;
        if (s_n++ < 300) {
            uint32_t sp = (uint32_t)ctx->r29, i;
            xhw_logf("[OS] recv %08x blocks: N64 thread %08x, ra %08x, sp %08x, a1 %08x", (unsigned)mq,
                     (unsigned)me->t, (unsigned)ctx->r31, (unsigned)sp, (unsigned)ARG(1));
            for (i = 0; i < 64; i += 8)
                xhw_logf("[OS]   sp+%02x: %08x %08x %08x %08x %08x %08x %08x %08x", (unsigned)(4 * i),
                         rd32(sp + 4 * i), rd32(sp + 4 * i + 4), rd32(sp + 4 * i + 8), rd32(sp + 4 * i + 12),
                         rd32(sp + 4 * i + 16), rd32(sp + 4 * i + 20), rd32(sp + 4 * i + 24), rd32(sp + 4 * i + 28));
            if (s_n <= 2) xrc_trace_dump();
        }
    }
#endif
    xu_lock();
    if (block && rd32(mq + MQ_VALIDCOUNT)) xu_rcp_flush_recv(me);   /* sp.c XRC_RCP_RECV (off: nothing) */
    while ((valid = (int32_t)rd32(mq + MQ_VALIDCOUNT)) == 0) {
        if (!block) {
            xu_unlock();
            RET(-1);
            return;
        }
        sleep_on(me, mq);
    }
    first = (int32_t)rd32(mq + MQ_FIRST);
    count = (int32_t)rd32(mq + MQ_MSGCOUNT);
    if (out) wr32(out, rd32(rd32(mq + MQ_MSG) + 4 * (uint32_t)first));
    wr32(mq + MQ_FIRST, (uint32_t)((first + 1) % (count > 0 ? count : 1)));
    wr32(mq + MQ_VALIDCOUNT, (uint32_t)(valid - 1));
    if (mq == s_events[OS_EVENT_DP].mq && rd32(rd32(mq + MQ_MSG) + 4 * (uint32_t)first) == s_events[OS_EVENT_DP].msg)
        xrc_gfx_dp_taken();   /* the game takes a DP done: what the RDP wrote for that task is what it sees */
#ifdef XRC_WATCH_QUEUE
    if (watched_queue(mq)) {   /* diagnosis: who takes what */
        static unsigned s_r;
        if (s_r++ < 64 || (XRC_WATCH_QUEUE_ALL && s_r < 20000))
#ifndef XRC_WATCH_PEEK   /* -DXRC_WATCH_PEEK=<N64 address>: its word beside each take (DK64: the scheduler's audio list) */
#define XRC_WATCH_PEEK 0
#endif
            xhw_logf("[OS] queue %08x -> %08x to N64 thread %08x, %d left, peek %08x, retrace %u", (unsigned)mq,
                     (unsigned)rd32(rd32(mq + MQ_MSG) + 4 * (uint32_t)first), me ? (unsigned)me->t : (unsigned)self()->t,
                     (int)(valid - 1), XRC_WATCH_PEEK ? (unsigned)rd32((uint32_t)(XRC_WATCH_PEEK)) : 0u, xu_retrace());
    }
#endif
    wake_all(mq | 1);
    xu_unlock();
    token_yield(tls_thr());
    RET(0);
}

/* ---- events ---- */

#ifndef XRC_VI_ON_EVENT
#define XRC_VI_ON_EVENT 1   /* -DXRC_VI_ON_EVENT=0: retraces only after the runtime's osCreateViManager */
#endif
void xu_vi_start(void);   /* vi.c */

int xu_event_set(int e) { return e >= 0 && e < OS_NUM_EVENTS && s_events[e].mq != 0; }

ULTRA(osSetEventMesg) {
    uint32_t e = ARG(0);
    (void)rdram;
    if (e < OS_NUM_EVENTS) {
        /* the message before the queue, under the lock, and the retraces after
         * both: the VI thread posting between them sent the queue a NULL
         * message (Mario Tennis's own VI manager read one and crashed at its
         * first retrace in release builds, vmt2) */
        xu_lock();
        s_events[e].msg = ARG(2);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        s_events[e].mq = ARG(1);
        xu_unlock();
        if (e == OS_EVENT_VI && XRC_VI_ON_EVENT) xu_vi_start();   /* a game's own VI manager: the retraces it waits for */
        if (XHW_TEST_BUILD) xhw_logf("[OS] osSetEventMesg %d: queue %08x message %08x", e, (unsigned)ARG(1),
                                     (unsigned)ARG(2));
    }
}

static void post(int e, int jam) {
    static unsigned s_dropped, s_seen[OS_NUM_EVENTS];
    if (e < 0 || e >= OS_NUM_EVENTS || !s_events[e].mq) return;
    if (XHW_TEST_BUILD && s_seen[e] < 2)   /* test builds: the first posts of each event, where they go */
        s_seen[e]++, xhw_logf("[OS] event %d -> queue %08x message %08x", e, (unsigned)s_events[e].mq,
                              (unsigned)s_events[e].msg);
    if (xu_send(s_events[e].mq, s_events[e].msg, 0, jam) < 0 && s_dropped++ < 20)
        xhw_logf("[OS] event %d dropped: queue %08x full (%u)", e, (unsigned)s_events[e].mq,
                 (unsigned)rd32(s_events[e].mq + MQ_MSGCOUNT));
}

void xu_post_event(int e) { post(e, 0); }

/* free slots in event e's queue (a large number when it has none set) */
int xu_event_room(int e) {
    uint32_t mq;
    if (e < 0 || e >= OS_NUM_EVENTS || !(mq = s_events[e].mq)) return 1 << 30;
    return (int)((int32_t)rd32(mq + MQ_MSGCOUNT) - (int32_t)rd32(mq + MQ_VALIDCOUNT));
}

int xu_event_same(int a, int b) { return s_events[a].mq && s_events[a].mq == s_events[b].mq; }

/* the crash report's tail (trace.c): the crashing N64 thread's registers as
 * its context last held them (the generated code keeps them there) */
void xu_crash_regs(void) {
    Thr* me = self();
    const gpr* r;
    int i;
    if (!me->ctx) return;
    r = &me->ctx->r0;
    xhw_logf("[REGS] thread %d (N64 thread %08x):", me->t ? (int)(int32_t)rd32(me->t + TH_ID) : -1, (unsigned)me->t);
    for (i = 0; i < 32; i += 4)
        xhw_logf("[REGS]   r%-2d %08x  r%-2d %08x  r%-2d %08x  r%-2d %08x", i, (unsigned)r[i], i + 1, (unsigned)r[i + 1],
                 i + 2, (unsigned)r[i + 2], i + 3, (unsigned)r[i + 3]);
    if ((uint32_t)r[29] - 0x80000000u < xrc_rdram_size - 0x100u)   /* the N64 stack: the frames above */
        for (i = 0; i < 64; i += 8) {
            uint32_t a = (uint32_t)r[29] + 4u * (uint32_t)i;
            xhw_logf("[REGS]   sp+%03x: %08x %08x %08x %08x %08x %08x %08x %08x", 4 * i, (unsigned)rd32(a),
                     (unsigned)rd32(a + 4), (unsigned)rd32(a + 8), (unsigned)rd32(a + 12), (unsigned)rd32(a + 16),
                     (unsigned)rd32(a + 20), (unsigned)rd32(a + 24), (unsigned)rd32(a + 28));
        }
}

uint32_t xrc_interp_last_pc(void* thread);   /* interp.c */

/* a fatal on this thread (ovl.c: no function at an address): its MIPS
 * registers and the top of its stack, for the log */
void xu_log_regs(void) {
    const Thr* d = self();
    const recomp_context* c = d ? d->ctx : NULL;
    uint32_t sp, i;
    if (!c) return;
    xhw_logf("[CRASH] N64 thread %08x: a0 %08x a1 %08x a2 %08x a3 %08x v0 %08x v1 %08x t9 %08x", (unsigned)d->t,
             (unsigned)c->r4, (unsigned)c->r5, (unsigned)c->r6, (unsigned)c->r7, (unsigned)c->r2, (unsigned)c->r3,
             (unsigned)c->r25);
    xhw_logf("[CRASH]   s0 %08x s1 %08x s2 %08x s3 %08x s4 %08x sp %08x ra %08x", (unsigned)c->r16, (unsigned)c->r17,
             (unsigned)c->r18, (unsigned)c->r19, (unsigned)c->r20, (unsigned)c->r29, (unsigned)c->r31);
    sp = (uint32_t)c->r29;
    if ((sp & 0xC0000000u) == 0x80000000u && (sp & 0x1FFFFFFFu) + 64 <= xrc_rdram_size)
        for (i = 0; i < 64; i += 32)
            xhw_logf("[CRASH]   sp+%02x: %08x %08x %08x %08x %08x %08x %08x %08x", (unsigned)i, (unsigned)rd32(sp + i),
                     (unsigned)rd32(sp + i + 4), (unsigned)rd32(sp + i + 8), (unsigned)rd32(sp + i + 12),
                     (unsigned)rd32(sp + i + 16), (unsigned)rd32(sp + i + 20), (unsigned)rd32(sp + i + 24),
                     (unsigned)rd32(sp + i + 28));
}

/* strand.c: the contexts a game switches with setjmp/longjmp run on host
 * threads of their own, each the same N64 thread to the runtime */
void* xu_self_thr(void) { return self(); }
uint32_t xu_thr_t(void* thr) { return thr ? ((const Thr*)thr)->t : 0; }
int xu_thr_blocked(void* thr) { const Thr* d = (const Thr*)thr; return d->wait || d->stopped; }
void xu_adopt_thr(void* thr) {
    if (s_tls == 0xFFFFFFFFu) s_tls = xhw_tls_alloc();
    xhw_tls_set(s_tls, thr);
}
int xu_thr_level(void* thr) {
    const Thr* d = (const Thr*)thr;
    return d->t ? xu_level_of((int)(int32_t)rd32(d->t + TH_PRIORITY)) : 8;
}

/* the calling N64 thread's stack pointer as its context last held it (0 for
 * a runtime thread): interp.c runs the game's exception code below it */
uint32_t xu_self_sp(void) {
    Thr* me = self();
    return me->ctx ? (uint32_t)me->ctx->r29 : 0;
}

/* libultra's fault path for a TLB miss no refill vector mapped (tlb.c):
 * the thread stops faulted as __osException leaves it (__osFaultedThread,
 * state stopped, flag fault, cause and badvaddr in its context) and
 * OS_EVENT_FAULT goes out; the game's fault thread (GoldenEye's crashMain)
 * loads the page and makes it runnable again, which this waits for (up to
 * 10 s). The addresses come from n64rx, which finds __osException's fault
 * path in the ROM (-DXRC_FAULTED_THREAD, -DXRC_TH_BADVADDR). 0: no fault
 * thread, or it never let the thread go. */
/* A game's fault thread may let the faulted thread go by putting it on its
 * own run queue (GoldenEye's crashMain: __osEnqueueThread(&__osRunQueue)
 * and a yield), for libultra's dispatcher to pop. The runtime runs threads
 * itself and never pops it: the second fault queued the thread after
 * itself, and the third enqueue walked the cycle for good. It is taken off
 * here, as the dispatcher would. The queue is the game's __osRunQueue
 * (XRC_OS_RUNQUEUE, or 12 bytes below __osFaultedThread as libultra lays
 * them out, behind __osThreadTail: checked by its -1 priority).
 * -DXRC_FAULT_RUNQ=0: left as it is. */
#ifndef XRC_FAULT_RUNQ
#define XRC_FAULT_RUNQ 1
#endif
#if defined(XRC_FAULTED_THREAD) && defined(XRC_TH_BADVADDR)
static void runq_pop(uint32_t t) {
#ifdef XRC_OS_RUNQUEUE
    uint32_t q = XRC_OS_RUNQUEUE;
#else
    uint32_t q = (uint32_t)(XRC_FAULTED_THREAD) - 12u;
    if (rd32(q - 4) != 0xFFFFFFFFu) return;   /* not __osThreadTail's priority: another layout */
#endif
    uint32_t prev = q, cur;
    int n;
    if (!XRC_FAULT_RUNQ) return;
    for (n = 0; n < 64 && (cur = rd32(prev)) != 0 && (int32_t)rd32(cur + TH_PRIORITY) != -1; n++) {
        if (cur == t) wr32(prev, rd32(cur + TH_NEXT));   /* unlinked; prev stays */
        else prev = cur + TH_NEXT;
    }
}
#endif

int xu_fault_wait(uint32_t vaddr, int store) {
#if defined(XRC_FAULTED_THREAD) && defined(XRC_TH_BADVADDR)
    static unsigned s_faults;
    Thr* me = self();
    uint32_t t = me->t;
    int n, i, ms;
    if (!t || !s_events[OS_EVENT_FAULT].mq) return 0;
    xu_lock();
    wr32(XRC_FAULTED_THREAD, t);
    wr32(t + XRC_TH_BADVADDR - 4, (store ? 3u : 2u) << 2);   /* context.cause: TLBS / TLBL */
    wr32(t + XRC_TH_BADVADDR, vaddr);
    wr16(t + TH_STATE, OS_STATE_STOPPED);
    wr16(t + TH_FLAGS, 2);   /* OS_FLAG_FAULT */
    me->wait = 1;
    me->faulting = 1;
    running_next(me);
    make_blocked(me);
    post(OS_EVENT_FAULT, 0);
    if (s_faults < 16)
        xhw_logf("[OS] fault: thread %d at %08x, OS_EVENT_FAULT to %08x (%u of %u)", (int)(int32_t)rd32(t + TH_ID),
                 (unsigned)vaddr, (unsigned)s_events[OS_EVENT_FAULT].mq,
                 (unsigned)rd32(s_events[OS_EVENT_FAULT].mq + MQ_VALIDCOUNT),
                 (unsigned)rd32(s_events[OS_EVENT_FAULT].mq + MQ_MSGCOUNT));
    n = s_depth;
    for (i = 0; i < n; i++) xu_unlock();
    for (ms = 0; ms < 10000; ms++) {
        if (rd16(t + TH_STATE) != OS_STATE_STOPPED || !(rd16(t + TH_FLAGS) & 2)) break;
        xhw_sleep_ms(1);
    }
    for (i = 0; i < n; i++) xu_lock();
    me->wait = 0;
    me->faulting = 0;
    if (ms < 10000) runq_pop(t);
    wr16(t + TH_STATE, OS_STATE_RUNNING);
    wr16(t + TH_FLAGS, 0);
    running_is(t);
    token_wait(me);
    xu_unlock();
    if (s_faults < 16) s_faults++, xhw_logf("[OS] fault: thread %d %s after %d ms", (int)(int32_t)rd32(t + TH_ID),
                                            ms < 10000 ? "runnable again" : "never let go", ms);
    else {   /* how often, and what they cost (a game that pages its code through the TLB) */
        static unsigned s_more, s_ms;
        s_more++, s_ms += (unsigned)ms;
        if ((s_more & (s_more - 1)) == 0)
            xhw_logf("[OS] fault: %u more, %u ms waited in all (retrace %u)", s_more, s_ms, xu_retrace());
    }
    return ms < 10000;
#else
    (void)vaddr, (void)store;
    return 0;
#endif
}
void xu_post_event_first(int e) { post(e, 1); }

/* ---- the interrupt mask ---- */
/* __osDisableInt / __osRestoreInt and COP0 status IE: a thread that turns
 * interrupts off holds one level of the lock until it turns them back on, so
 * a game's critical section excludes the runtime's own queue updates. */
/* DIVERGES: while a thread has its interrupts off it runs above every game
 * thread (level 14), as nothing preempted it on the N64. Without this a
 * low thread holding the lock (Kirby 64's idle thread, N64 priority 0, at
 * level 2) was preempted by a higher one spinning on a flag the audio
 * thread sets (Kirby's wait for its music to stop, 0x800A77F8): the
 * holder never ran again, the VI thread waited on the lock, no retrace
 * reached the scheduler and the game stopped (xemu, about one boot in two).
 * -DXRC_INTS_BOOST=0: the thread keeps its level, as before */
static int ints_off(Thr* me) {
    if (me->ints_off) return 0;
    xu_lock();
    me->ints_off = 1;
    if (XRC_INTS_BOOST && me->kt && me->t) {
        xhw_thread_set_level(me->kt, 14);
        if (s_perf > 0) xhw_perf_add(XHW_PERF_INTS, 0);
    }
    return 1;
}
static void ints_on(Thr* me) {
    if (!me->ints_off) return;
    me->ints_off = 0;
    if (XRC_INTS_BOOST && me->kt && me->t) {
        xhw_thread_set_level(me->kt, xu_level_of((int)rd32(me->t + TH_PRIORITY)));
        if (s_perf > 0) xhw_perf_add(XHW_PERF_INTS, 0);
    }
    xu_unlock();
    if (s_pend) token_yield(me);   /* a switch that waited for interrupts on */
}

ULTRA(__osDisableInt) {
    (void)rdram;
    RET(ints_off(self()));
}

ULTRA(__osRestoreInt) {
    (void)rdram;
    if (ARG(0) & 1) ints_on(self());
}

void cop0_status_write(recomp_context* ctx, gpr value) {
    Thr* me = self();
    ctx->status_reg = (uint32_t)value;
    if (XRC_FR) ctx->mips3_float_mode = (uint8_t)(((uint32_t)value >> 26) & 1u);   /* FR: recomp.h XRC_FODD */
    if (value & 1) ints_on(me);
    else ints_off(me);
}

gpr cop0_status_read(recomp_context* ctx) {
    return (gpr)(int32_t)((ctx->status_reg & ~1u) | (self()->ints_off ? 0u : 1u));
}

static uint32_t s_int_mask = 0x003FFF01u;   /* OS_IM_ALL */

/* osSetIntMask(mask): a mask that lets no interrupt through (OS_IM_NONE:
 * libaudio's alEvtqPostEvent and alEvtqNextEvent around their list
 * updates) is a critical section on the N64, as __osDisableInt's is.
 * -DXRC_INTMASK_LOCK=1 holds the same lock level until a mask that lets
 * them through again. Off by default: Mario Party switches its coroutines
 * (strand.c) under such a mask, a parked strand kept the lock and the game
 * stopped at its second frame; All-Star Baseball 2000's freeze, the reason
 * it was tried, was its audio thread starved of AI events (ai.c). */
#ifndef XRC_INTMASK_LOCK
#define XRC_INTMASK_LOCK 0
#endif
ULTRA(osSetIntMask) {
    uint32_t old = s_int_mask, m = ARG(0);
    (void)rdram;
    s_int_mask = m;
    if (XRC_INTMASK_LOCK) {
        Thr* me = self();
        int none = (m & 0x003FFF00u) == 0;
        if (none && !me->mask_off) {
            me->mask_off = ints_off(me) ? 1 : 2;
        } else if (!none && me->mask_off) {
            if (me->mask_off == 1) ints_on(me);
            me->mask_off = 0;
        }
    }
    RET(old);
}

ULTRA(__osGetCause) {
    (void)rdram;
    RET(0);
}

/* test builds (vi.c, Wings64-X's stall watch): what every N64 thread waits
 * on, with the queue's fill; the addresses are the game's data symbols */
void xrc_trace_dump(void);   /* trace.c */
void xrc_trace_thread(void* thread, unsigned n);   /* trace.c */

void xu_stall_report(const char* why) {
    int i;
    xhw_logf("[OS] stall: %s (retrace %u)", why, xu_retrace());
    if (XRC_RUN_TOKEN)
        xhw_logf("[OS] stall:   run token with thread %d, %u hand-offs, %u taken while the holder ran%s",
                 xu_token_holder(), s_handoffs, s_late, s_pend ? ", a switch waits for interrupts on" : "");
    xrc_trace_dump();   /* what ran last (interpreted calls in every build; a trace build's functions) */
    for (i = 0; i < s_nthr; i++) {
        uint32_t w = s_thr[i].wait, q = w & ~1u;
        if (!s_thr[i].t && !w) continue;
        xhw_logf("[OS] stall:   thread %d (pri %d) %s %08x (%u of %u)%s%s", s_thr[i].t ? (int)(int32_t)rd32(s_thr[i].t + TH_ID) : -1,
                 s_thr[i].t ? (int)(int32_t)rd32(s_thr[i].t + TH_PRIORITY) : -1,
                 !w ? "runs, or waits outside a queue" : (w & 1) ? "waits for room in" : "waits for a message in",
                 (unsigned)q, w ? (unsigned)rd32(q + MQ_VALIDCOUNT) : 0u, w ? (unsigned)rd32(q + MQ_MSGCOUNT) : 0u,
                 s_thr[i].stopped ? ", stopped" : "", s_thr[i].halted ? ", halted" : "");
        if (!w) {   /* a running thread: where it was last (interp.c; its context's ra and sp) */
            uint32_t pc = xrc_interp_last_pc(s_thr[i].kt);
            xhw_logf("[OS] stall:     last interpreted pc %08x, ra %08x, sp %08x", (unsigned)pc,
                     s_thr[i].ctx ? (unsigned)s_thr[i].ctx->r31 : 0u, s_thr[i].ctx ? (unsigned)s_thr[i].ctx->r29 : 0u);
        }
        if (s_thr[i].kt) xrc_trace_thread(s_thr[i].kt, 16);   /* a trace build: its own last functions */
    }
}
