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

#define MAX_THREADS 32
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

void xu_lock(void) {
    if (!s_lock) s_lock = xhw_mutex_create();
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
    volatile int stopped;       /* in osStopThread, until an osStartThread */
    volatile int fresh;         /* osCreateThread again on this OSThread: start over at the entry */
    volatile int halted;        /* another thread stopped or destroyed it: it stops at its next block */
    volatile int dead;          /* destroyed, or its entry returned: its record (and host thread, once
                                 * stopped) may serve the next new OSThread */
    void* jb[5];                /* thread_body's restart point (__builtin_setjmp) */
    recomp_context* ctx;        /* thread_body's: its sp for interp.c's exception code */
} Thr;
static Thr s_thr[MAX_THREADS];
static volatile int s_nthr;
static uint32_t s_tls = 0xFFFFFFFFu;

int xu_level_of(int pri) {
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
    s_nthr++;
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
    if (t) wr32(XRC_RUNNING_THREAD, t);
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

/* under the lock: wake every thread sleeping on `key` (they recheck) */
static void wake_all(uint32_t key) {
    int i;
    for (i = 0; i < s_nthr; i++)
        if (s_thr[i].wait == key) xhw_event_signal(s_thr[i].ev);
}

/* under the lock: sleep until woken; every level of the lock is let go
 * meanwhile (a thread blocked with interrupts "off" lets others run, as a
 * thread switch did on the N64) and taken back after */
static void stop_wait(Thr* me);

static int pri_of(const Thr* d) { return d->t ? (int)(int32_t)rd32(d->t + TH_PRIORITY) : -1; }

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
    running_next(me);
    for (i = 0; i < n; i++) xu_unlock();
    if (!me->halted) xhw_event_wait(me->ev, XHW_INFINITE);
    if (XRC_THREAD_HALT && me->halted) {   /* stopped or destroyed meanwhile: no lock held here */
        me->wait = 0;
        me->halted = 0;
        stop_wait(me);                     /* started again: on from here, or (created again) its entry */
    } else if (XRC_THREAD_HALT && me->fresh && XRC_THREAD_RESUME) {
        me->wait = 0;                      /* created and started again before it got here */
        __builtin_longjmp(me->jb, 1);
    }
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
static void stop_wait(Thr* me) {
    if (!XRC_THREAD_RESUME || !me->t) park();
    me->stopped = 1;
    running_next(me);
    while (me->stopped) xhw_event_wait(me->ev, XHW_INFINITE);
    if (me->fresh) __builtin_longjmp(me->jb, 1);
    wr16(me->t + TH_STATE, OS_STATE_RUNNING);
}

static void thread_body(void* p) {
    Thr* d = (Thr*)p;
    recomp_context ctx;
    if (s_tls == 0xFFFFFFFFu) s_tls = xhw_tls_alloc();
    xhw_tls_set(s_tls, d);
    if (XRC_THREAD_RESUME && __builtin_setjmp(d->jb)) {   /* created again (stop_wait) */
        d = (Thr*)xhw_tls_get(s_tls);
        xhw_logf("[OS] thread %d started again: entry %08x", (int)(int32_t)rd32(d->t + TH_ID), (unsigned)d->entry);
    }
    d->fresh = 0;
    running_is(d->t);
    xrc_ctx_init(&ctx, d->sp);
    d->ctx = &ctx;
    ctx.r4 = (gpr)(int32_t)d->arg;
    set_cop1_cs(0);
    get_function((int32_t)d->entry)(xrc_rdram, &ctx);
    /* an N64 thread that returns stops (libultra's __osCleanupThread) */
    xhw_logf("[OS] thread %d returned", (int)(int32_t)rd32(d->t + TH_ID));
    wr16(d->t + TH_STATE, OS_STATE_STOPPED);
    d->dead = 1;
    stop_wait(d);
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
    /* libultra starts the thread 16 bytes below the sp it is given
     * (osCreateThread: context.sp = sp - 16): the four argument words a
     * MIPS callee may store above its own frame. Without them a thread's
     * entry wrote its argument into the word just past its stack (Pokemon
     * Snap: the next object-thread stack's free-list link, corrupted with a
     * GObj pointer). -DXRC_THREAD_SP16=0: the sp as given. */
    d->sp = ARG(4) - (XRC_THREAD_SP16 ? 16u : 0u);
    wr32(t + TH_ID, ARG(1));
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
        d->kt = xhw_thread_spawn(thread_body, d, xu_level_of(pri), HOST_STACK);
        if (!d->kt) xrc_fatal_vram("osStartThread: no kernel thread", t, 0);
        xhw_logf("[OS] thread %d started: entry %08x, sp %08x, N64 priority %d -> level %d",
                 (int)(int32_t)rd32(t + TH_ID), (unsigned)d->entry, (unsigned)d->sp, pri, xu_level_of(pri));
    } else if (XRC_THREAD_RESUME && d->stopped) {
        wr16(t + TH_STATE, OS_STATE_RUNNABLE);
        d->stopped = 0;
        xhw_event_signal(d->ev);
    } else if (d->halted) {   /* stopped by another thread, started again before it reached its block */
        wr16(t + TH_STATE, OS_STATE_RUNNABLE);
        d->halted = 0;
        xhw_event_signal(d->ev);
    }
    /* a higher N64 priority runs first, as libultra dispatches it: where the
     * kernel levels are equal (50 and 51 share one) the caller waits until
     * it stops or blocks on a queue; a higher level preempts by itself */
    if (XRC_THREAD_RESUME && me->t && (int32_t)rd32(t + TH_PRIORITY) > (int32_t)rd32(me->t + TH_PRIORITY) &&
        xu_level_of((int32_t)rd32(t + TH_PRIORITY)) == xu_level_of((int32_t)rd32(me->t + TH_PRIORITY)))
        while (!d->stopped && !d->wait) xhw_yield();
    /* the boot code is not a thread: on the N64 the first osStartThread
     * dispatches and the boot code never runs again */
    if (me->t == 0 && !me->entry) {
        xhw_log("[OS] boot code done: the boot thread parks");
        park();
    }
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
    d->halted = 1;
    xhw_event_signal(d->ev);                  /* blocked: it wakes and stops */
}

ULTRA(osStopThread) {
    uint32_t t = ARG(0);
    (void)rdram;
    TRACE("[OS] trace: stop %08x by %08x", (unsigned)t, (unsigned)self()->t);
    if (!t || t == self()->t) {
        Thr* me = self();
        if (me->t) wr16(me->t + TH_STATE, OS_STATE_STOPPED);
        stop_wait(me);
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
        stop_wait(self());
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

ULTRA(osSetThreadPri) {
    uint32_t t = ARG(0);
    int pri = (int)(int32_t)ARG(1);
    Thr* d = t ? find(t) : self();
    (void)rdram;
    if (!d) return;
    if (d->t) wr32(d->t + TH_PRIORITY, (uint32_t)pri);
    if (d->kt) xhw_thread_set_level(d->kt, xu_level_of(pri));
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
    (void)rdram;
    (void)ctx;
    xhw_yield();
}

/* more2.c's __osEnqueueAndYield: a pause with every level of the lock let
 * go when the thread holds it (interrupts "off", as libultra's callers do),
 * taken back after */
void xu_pause_unlocked(uint32_t ms);
void xu_pause_unlocked(uint32_t ms) {
    Thr* me = self();
    int n = me->ints_off ? s_depth : 0, i;
    for (i = 0; i < n; i++) xu_unlock();
    if (ms) xhw_sleep_ms(ms);
    else xhw_yield();
    for (i = 0; i < n; i++) xu_lock();
}

/* ---- message queues ---- */
ULTRA(osCreateMesgQueue) {
    uint32_t mq = ARG(0);
    (void)rdram;
    wr32(mq + MQ_MTQUEUE, 0);
    wr32(mq + MQ_FULLQUEUE, 0);
    wr32(mq + MQ_VALIDCOUNT, 0);
    wr32(mq + MQ_FIRST, 0);
    wr32(mq + MQ_MSGCOUNT, ARG(2));
    wr32(mq + MQ_MSG, ARG(1));
}

int xu_send(uint32_t mq, uint32_t msg, int block, int jam) {
    Thr* me = block ? self() : NULL;
    int32_t count, valid, first;
#ifdef XRC_WATCH_QUEUE
    if (mq == (uint32_t)(XRC_WATCH_QUEUE)) {   /* diagnosis: what goes into one queue, from whom */
        static unsigned s_n;
        Thr* who = self();
        if (((s_n < 8 || msg != 1u) && s_n < 64) || s_n % 600 == 0)
            xhw_logf("[OS] queue %08x <- %08x by N64 thread %08x (ra %08x)%s #%u valid %u", (unsigned)mq, (unsigned)msg,
                     (unsigned)who->t, who->ctx ? (unsigned)who->ctx->r31 : 0u, jam ? " jammed" : "", s_n, (unsigned)rd32(mq + MQ_VALIDCOUNT));
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
#ifdef XRC_WATCH_QUEUE
    if (mq == (uint32_t)(XRC_WATCH_QUEUE)) {   /* diagnosis: who takes what */
        static unsigned s_r;
        if (s_r++ < 64)
            xhw_logf("[OS] queue %08x -> %08x to N64 thread %08x, %d left", (unsigned)mq,
                     (unsigned)rd32(rd32(mq + MQ_MSG) + 4 * (uint32_t)first), me ? (unsigned)me->t : (unsigned)self()->t,
                     (int)(valid - 1));
    }
#endif
    wake_all(mq | 1);
    xu_unlock();
    RET(0);
}

/* ---- events ---- */
static struct { uint32_t mq, msg; } s_events[OS_NUM_EVENTS];

ULTRA(osSetEventMesg) {
    uint32_t e = ARG(0);
    (void)rdram;
    if (e < OS_NUM_EVENTS) {
        s_events[e].mq = ARG(1);
        s_events[e].msg = ARG(2);
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
    running_next(me);
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
    if (ms < 10000) runq_pop(t);
    wr16(t + TH_STATE, OS_STATE_RUNNING);
    wr16(t + TH_FLAGS, 0);
    running_is(t);
    xu_unlock();
    if (s_faults < 16) s_faults++, xhw_logf("[OS] fault: thread %d %s after %d ms", (int)(int32_t)rd32(t + TH_ID),
                                            ms < 10000 ? "runnable again" : "never let go", ms);
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
static int ints_off(Thr* me) {
    if (me->ints_off) return 0;
    xu_lock();
    me->ints_off = 1;
    return 1;
}
static void ints_on(Thr* me) {
    if (!me->ints_off) return;
    me->ints_off = 0;
    xu_unlock();
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
    if (value & 1) ints_on(me);
    else ints_off(me);
}

gpr cop0_status_read(recomp_context* ctx) {
    return (gpr)(int32_t)((ctx->status_reg & ~1u) | (self()->ints_off ? 0u : 1u));
}

static uint32_t s_int_mask = 0x003FFF01u;   /* OS_IM_ALL */

ULTRA(osSetIntMask) {
    uint32_t old = s_int_mask;
    (void)rdram;
    s_int_mask = ARG(0);
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
