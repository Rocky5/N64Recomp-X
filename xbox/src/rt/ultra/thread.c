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
#ifndef XRC_THREAD_RESUME
#define XRC_THREAD_RESUME 1
#endif
#define HOST_STACK (128 * 1024)
/* -DXRC_THREAD_TRACE=1 (diagnosis): every create, start, stop and destroy
 * logged with the calling thread's id, the first 400 */
#ifndef XRC_THREAD_TRACE
#define XRC_THREAD_TRACE 0
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
    void* jb[5];                /* thread_body's restart point (__builtin_setjmp) */
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

/* under the lock: wake every thread sleeping on `key` (they recheck) */
static void wake_all(uint32_t key) {
    int i;
    for (i = 0; i < s_nthr; i++)
        if (s_thr[i].wait == key) xhw_event_signal(s_thr[i].ev);
}

/* under the lock: sleep until woken; every level of the lock is let go
 * meanwhile (a thread blocked with interrupts "off" lets others run, as a
 * thread switch did on the N64) and taken back after */
static void sleep_on(Thr* me, uint32_t key) {
    int n = s_depth, i;
    me->wait = key;
    for (i = 0; i < n; i++) xu_unlock();
    xhw_event_wait(me->ev, XHW_INFINITE);
    for (i = 0; i < n; i++) xu_lock();
    me->wait = 0;
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
    xrc_ctx_init(&ctx, d->sp);
    ctx.r4 = (gpr)(int32_t)d->arg;
    set_cop1_cs(0);
    get_function((int32_t)d->entry)(xrc_rdram, &ctx);
    /* an N64 thread that returns stops (libultra's __osCleanupThread) */
    xhw_logf("[OS] thread %d returned", (int)(int32_t)rd32(d->t + TH_ID));
    wr16(d->t + TH_STATE, OS_STATE_STOPPED);
    stop_wait(d);
}

/* osCreateThread(t, id, entry, arg, sp, pri) */
ULTRA(osCreateThread) {
    uint32_t t = ARG(0);
    Thr* d;
    (void)rdram;
    xu_lock();
    d = find(t);
    if (!d) d = add(t);
    if (d->kt && XRC_THREAD_RESUME) d->fresh = 1;   /* the old one is stopped: starts over */
    d->entry = ARG(2);
    d->arg = ARG(3);
    d->sp = ARG(4);
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
    xhw_logf("[OS] osStopThread of another thread (%08x): not supported, ignored", (unsigned)t);
}

ULTRA(osDestroyThread) {
    uint32_t t = ARG(0);
    (void)rdram;
    static unsigned s_logged;
    TRACE("[OS] trace: destroy %08x by %08x", (unsigned)t, (unsigned)self()->t);
    if (!t || t == self()->t) {
        if (t) wr16(t + TH_STATE, OS_STATE_STOPPED);
        stop_wait(self());
        return;
    }
    /* another thread: it is stopped (HAL's object manager stops them first)
     * and stays so until it is created again */
    if (s_logged++ < 4) xhw_logf("[OS] osDestroyThread of another thread (%08x): it stays stopped", (unsigned)t);
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
    }
}

static void post(int e, int jam) {
    static unsigned s_dropped;
    if (e < 0 || e >= OS_NUM_EVENTS || !s_events[e].mq) return;
    if (xu_send(s_events[e].mq, s_events[e].msg, 0, jam) < 0 && s_dropped++ < 20)
        xhw_logf("[OS] event %d dropped: queue %08x full (%u)", e, (unsigned)s_events[e].mq,
                 (unsigned)rd32(s_events[e].mq + MQ_MSGCOUNT));
}

void xu_post_event(int e) { post(e, 0); }
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
void xu_stall_report(const char* why) {
    int i;
    xhw_logf("[OS] stall: %s (retrace %u)", why, xu_retrace());
    for (i = 0; i < s_nthr; i++) {
        uint32_t w = s_thr[i].wait, q = w & ~1u;
        if (!s_thr[i].t && !w) continue;
        xhw_logf("[OS] stall:   thread %d (pri %d) %s %08x (%u of %u)", s_thr[i].t ? (int)(int32_t)rd32(s_thr[i].t + TH_ID) : -1,
                 s_thr[i].t ? (int)(int32_t)rd32(s_thr[i].t + TH_PRIORITY) : -1,
                 !w ? "runs, or waits outside a queue" : (w & 1) ? "waits for room in" : "waits for a message in",
                 (unsigned)q, w ? (unsigned)rd32(q + MQ_VALIDCOUNT) : 0u, w ? (unsigned)rd32(q + MQ_MSGCOUNT) : 0u);
    }
}
