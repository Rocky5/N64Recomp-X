/* test_thread.c - the thread layer on the host (xbox/src/rt/ultra/thread.c,
 * docs/roadmap.md B1, docs/review-2026-10.md 5b.5-5b.7).
 *
 * Built by tools/xbox/test_thread.py twice: with the run token
 * (-DXRC_RUN_TOKEN=1) and without. The platform's threads, events, locks
 * and TLS are pthreads; RDRAM is a host buffer; "game code" is C functions
 * that get_function hands out for the entry addresses, calling the runtime's
 * libultra as recompiled code would. The host has no strict priorities, so
 * the order checks hold only where the token decides (every switch at a
 * block or a safe point), which is what they test: libultra's order, one
 * game thread at a time, a host wait keeping the turn. */
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ultra.h"

static int s_fail, s_checks;
#define CHECK(cond, ...) do { \
        s_checks++; \
        if (!(cond)) { s_fail++; printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); \
                       printf("\n"); } \
    } while (0)

/* ---- the platform on pthreads ---- */
uint8_t* xrc_rdram;
uint8_t* xrc_rdram_host;
uint32_t xrc_rdram_size;
static int s_verbose;

struct xhw_mutex { pthread_mutex_t m; };
xhw_mutex* xhw_mutex_create(void) {
    xhw_mutex* m = calloc(1, sizeof *m);
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&m->m, &a);
    return m;
}
void xhw_mutex_lock(xhw_mutex* m) { pthread_mutex_lock(&m->m); }
void xhw_mutex_unlock(xhw_mutex* m) { pthread_mutex_unlock(&m->m); }

struct xhw_event { pthread_mutex_t m; pthread_cond_t c; int set; };
xhw_event* xhw_event_create(void) {
    xhw_event* e = calloc(1, sizeof *e);
    pthread_mutex_init(&e->m, NULL);
    pthread_cond_init(&e->c, NULL);
    return e;
}
void xhw_event_signal(xhw_event* e) {
    pthread_mutex_lock(&e->m);
    e->set = 1;
    pthread_cond_signal(&e->c);
    pthread_mutex_unlock(&e->m);
}
int xhw_event_wait(xhw_event* e, uint32_t ms) {
    int ok = 1;
    pthread_mutex_lock(&e->m);
    if (ms == XHW_INFINITE) {
        while (!e->set) pthread_cond_wait(&e->c, &e->m);
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += ms / 1000, ts.tv_nsec += (long)(ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) ts.tv_sec++, ts.tv_nsec -= 1000000000L;
        while (!e->set && ok) ok = pthread_cond_timedwait(&e->c, &e->m, &ts) == 0;
        ok = e->set;
    }
    e->set = 0;
    pthread_mutex_unlock(&e->m);
    return ok;
}

uint32_t xhw_tls_alloc(void) {
    pthread_key_t k;
    pthread_key_create(&k, NULL);
    return (uint32_t)k;
}
void* xhw_tls_get(uint32_t slot) { return pthread_getspecific((pthread_key_t)slot); }
void xhw_tls_set(uint32_t slot, void* v) { pthread_setspecific((pthread_key_t)slot, v); }

struct xhw_thread { pthread_t th; volatile int level; void (*fn)(void*); void* arg; };
static pthread_key_t s_self_key;
static pthread_once_t s_once = PTHREAD_ONCE_INIT;
static void self_key(void) { pthread_key_create(&s_self_key, NULL); }
static void* trampoline(void* p) {
    xhw_thread* t = p;
    pthread_setspecific(s_self_key, t);
    t->fn(t->arg);
    return NULL;
}
xhw_thread* xhw_thread_spawn(void (*fn)(void*), void* arg, int level, uint32_t stack) {
    xhw_thread* t = calloc(1, sizeof *t);
    pthread_attr_t a;
    pthread_once(&s_once, self_key);
    t->fn = fn, t->arg = arg, t->level = level;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, stack < 256 * 1024 ? 256 * 1024 : stack);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t->th, &a, trampoline, t)) return NULL;
    return t;
}
void xhw_thread_set_level(xhw_thread* t, int level) { t->level = level; }
xhw_thread* xhw_thread_self(void) {
    pthread_once(&s_once, self_key);
    return pthread_getspecific(s_self_key);
}
void xhw_thread_adopt_self(int level) {
    xhw_thread* t = calloc(1, sizeof *t);
    pthread_once(&s_once, self_key);
    t->th = pthread_self(), t->level = level;
    pthread_setspecific(s_self_key, t);
}
void xhw_sleep_ms(uint32_t ms) {
    struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    if (!ms) sched_yield();
    else nanosleep(&ts, NULL);
}
void xhw_yield(void) { sched_yield(); }
uint64_t xhw_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}
uint64_t xhw_game_time_us(void) { return xhw_time_us(); }
void xhw_log(const char* line) { if (s_verbose) printf("  log: %s\n", line); }
void xhw_logf(const char* fmt, ...) {
    va_list ap;
    if (!s_verbose) return;
    va_start(ap, fmt);
    printf("  log: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}
void xhw_fatal(const char* title, const char* body) {
    printf("FATAL %s: %s\n", title, body);
    exit(2);
}
void xrc_fatal_vram(const char* what, uint32_t vram, uint32_t extra) {
    printf("FATAL %s %08x %08x\n", what, (unsigned)vram, (unsigned)extra);
    exit(2);
}

/* ---- the rest of the runtime, stubbed ---- */
uint32_t xrc_tlb_to_phys(uint32_t v) { (void)v; return ~0u; }
int xu_rcp_flush(void* mine) { (void)mine; return 0; }
void xu_vi_start(void) {}
unsigned xu_retrace(void) { return 0; }
void xrc_trace_dump(void) {}
void xrc_trace_thread(void* thread, unsigned n) { (void)thread, (void)n; }
uint32_t xrc_interp_last_pc(void* thread) { (void)thread; return 0; }

/* ---- game code ---- */
#define NFUNC 32
static void (*s_entry[NFUNC])(uint32_t arg);
static void entry_wrap(uint8_t* rdram, recomp_context* ctx);
static void (*s_entry_fn[NFUNC])(uint8_t*, recomp_context*);
static int entry_index(uint32_t vram) { return (int)((vram - 0x80001000u) >> 4); }
recomp_func_t* get_function(int32_t vram) {
    int i = entry_index((uint32_t)vram);
    if (i < 0 || i >= NFUNC || !s_entry[i]) xrc_fatal_vram("no function", (uint32_t)vram, 0);
    (void)s_entry_fn;
    return entry_wrap;
}

/* one game thread at a time in game code (between runtime calls) */
static volatile int s_in_game, s_overlap, s_count_overlap = 1;
static pthread_mutex_t s_log_m = PTHREAD_MUTEX_INITIALIZER;
static char s_log[64][8];
static volatile int s_nlog;
static void enter(void) {
    if (__sync_add_and_fetch(&s_in_game, 1) > 1 && s_count_overlap) __sync_add_and_fetch(&s_overlap, 1);
}
static void leave(void) { __sync_sub_and_fetch(&s_in_game, 1); }
static void ev(const char* s) {
    pthread_mutex_lock(&s_log_m);
    if (s_nlog < 64) snprintf(s_log[s_nlog], sizeof s_log[0], "%s", s), s_nlog++;
    pthread_mutex_unlock(&s_log_m);
}

static void entry_wrap(uint8_t* rdram, recomp_context* ctx) {
    /* the entry runs from thread_body's context: its pc is not kept, so the
     * thread's own entry is found from its Thr's arg slot (a0 = index | arg << 8) */
    uint32_t a0 = (uint32_t)ctx->r4;
    (void)rdram;
    enter();
    s_entry[a0 & 0xFF](a0 >> 8);
    leave();
}

/* a runtime call from game code: out of game code for its length */
static _Thread_local uint32_t t_sp;
static uint32_t s_next_sp = 0x80700000u;
static int32_t call(void (*f)(uint8_t*, recomp_context*), uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
                    uint32_t a4, uint32_t a5) {
    recomp_context c;
    memset(&c, 0, sizeof c);
    if (!t_sp) t_sp = __sync_add_and_fetch(&s_next_sp, 0x400u);
    c.r29 = (gpr)(int32_t)t_sp;
    c.r4 = (gpr)(int32_t)a0, c.r5 = (gpr)(int32_t)a1, c.r6 = (gpr)(int32_t)a2, c.r7 = (gpr)(int32_t)a3;
    wr32(t_sp + 0x10, a4);
    wr32(t_sp + 0x14, a5);
    c.status_reg = 0x2000FF01u;
    leave();
    f(NULL, &c);
    enter();
    return (int32_t)c.r2;
}
#define DECL(n) void n##_recomp(uint8_t* rdram, recomp_context* ctx);
DECL(osCreateThread) DECL(osStartThread) DECL(osStopThread) DECL(osDestroyThread) DECL(osSetThreadPri)
DECL(osYieldThread) DECL(osCreateMesgQueue) DECL(osSendMesg) DECL(osRecvMesg) DECL(__osDisableInt)
DECL(__osRestoreInt)

static uint32_t TH(int i) { return 0x80100000u + 0x200u * (uint32_t)i; }
static uint32_t MQ(int i) { return 0x80200000u + 0x100u * (uint32_t)i; }
static void mq_make(int i, int slots) { call(osCreateMesgQueue_recomp, MQ(i), MQ(i) + 0x40, (uint32_t)slots, 0, 0, 0); }
/* thread i runs entry fn with arg, at pri */
static void create(int i, int fn, uint32_t arg, int pri) {
    call(osCreateThread_recomp, TH(i), (uint32_t)i, 0x80001000u + 16u * (uint32_t)fn, (uint32_t)fn | (arg << 8),
         0x80600000u - 0x2000u * (uint32_t)i, (uint32_t)pri);
}
static void start(int i) { call(osStartThread_recomp, TH(i), 0, 0, 0, 0, 0); }
static void recv(int q) { call(osRecvMesg_recomp, MQ(q), 0, 1, 0, 0, 0); }
static void send(int q) { call(osSendMesg_recomp, MQ(q), 1, 0, 0, 0, 0); }
static void yield(void) { call(osYieldThread_recomp, 0, 0, 0, 0, 0, 0); }
static void setpri(int i, int pri) { call(osSetThreadPri_recomp, i < 0 ? 0 : TH(i), (uint32_t)pri, 0, 0, 0, 0); }
static int32_t dis_int(void) { return call(__osDisableInt_recomp, 0, 0, 0, 0, 0, 0); }
static void res_int(int32_t v) { call(__osRestoreInt_recomp, (uint32_t)v, 0, 0, 0, 0, 0); }
static void stop_self(void) { call(osStopThread_recomp, 0, 0, 0, 0, 0, 0); }
static void host_wait_ms(uint32_t ms) { xhw_sleep_ms(ms); }   /* a host block inside game code (a log write) */
#define GRAVE 0   /* the queue nothing is ever sent to: where every scenario's threads end */
static void grave(void) { for (;;) recv(GRAVE); }

/* ---- scenarios ---- */
enum { F_S1M, F_S1A, F_S1B, F_S2S, F_S2C, F_S2D, F_S3H, F_S3L, F_S4H, F_S4W, F_S5X, F_S5Y, F_S6Z, F_S7S, F_S7T };
static volatile int s_level_in, s_level_out, s_level_mid;
static volatile int s_irq_go;

/* 1: libultra's order (token): a higher thread started runs at once, a send that
 * wakes a higher one switches to it, a block runs the next highest */
static void s1m(uint32_t a) {
    (void)a;
    ev("M1");
    mq_make(1, 1), mq_make(2, 1);
    create(2, F_S1A, 0, 20);
    start(2);
    ev("M2");
    create(3, F_S1B, 0, 5);
    start(3);
    ev("M3");
    send(1);
    ev("M4");
    recv(2);
    ev("M5");
    grave();
}
static void s1a(uint32_t a) { (void)a; ev("A1"); recv(1); ev("A2"); grave(); }
static void s1b(uint32_t a) { (void)a; ev("B1"); send(2); ev("B2"); grave(); }

/* 2: osYieldThread goes behind its equals */
static void s2s(uint32_t a) {
    (void)a;
    create(5, F_S2C, 0, 30), create(6, F_S2D, 0, 30);
    start(5), start(6);
    grave();
}
static void s2c(uint32_t a) { (void)a; ev("C1"); yield(); ev("C2"); yield(); grave(); }
static void s2d(uint32_t a) { (void)a; ev("D1"); yield(); ev("D2"); yield(); grave(); }

/* 3: a host wait inside game code keeps the turn: a lower thread does not run meanwhile */
static void s3h(uint32_t a) { (void)a; ev("H1"); create(8, F_S3L, 0, 45); start(8); host_wait_ms(80); ev("H2"); grave(); }
static void s3l(uint32_t a) { (void)a; ev("L1"); grave(); }

/* 4: an interrupt (a runtime thread's send) readies a higher thread; with
 * interrupts off it waits for them to come on */
static void s4h(uint32_t a) { (void)a; recv(4); ev("Hi"); grave(); }
static void s4w(uint32_t a) {
    int32_t s;
    (void)a;
    mq_make(4, 1);
    create(10, F_S4H, 0, 60);
    start(10);   /* runs at once and blocks on queue 4 */
    s = dis_int();
    s_irq_go = 1;   /* the "VI thread" sends now: it waits for the lock */
    host_wait_ms(60);
    ev("W1");
    res_int(s);
    host_wait_ms(60);
    ev("W2");
    grave();
}
static void* irq_thread(void* p) {
    (void)p;
    while (!s_irq_go) xhw_sleep_ms(1);
    xu_send(MQ(4), 7, 0, 0);
    return NULL;
}

/* 5 (5b.5): a thread that stops itself with interrupts off lets the lock go */
static void s5x(uint32_t a) {
    int32_t s;
    (void)a;
    s = dis_int();
    stop_self();
    ev("X2");
    res_int(s);
    grave();
}
static void s5y(uint32_t a) {
    (void)a;
    mq_make(5, 2);
    create(12, F_S5X, 0, 70);
    start(12);
    xhw_sleep_ms(30);   /* (without the token the two run side by side: let it stop) */
    send(5);            /* the lock: taken here */
    ev("Y1");
    start(12);
    xhw_sleep_ms(30);
    grave();
}

/* 6 (5b.7): osSetThreadPri inside a critical section keeps the boost */
static void s6z(uint32_t a) {
    int32_t s;
    (void)a;
    s = dis_int();
    s_level_in = xhw_thread_self()->level;
    setpri(-1, 5);
    s_level_mid = xhw_thread_self()->level;
    res_int(s);
    s_level_out = xhw_thread_self()->level;
    ev("Z");
    grave();
}

/* 7 (5b.6, no token): a same-level higher thread that polls does not keep its starter for good */
static void s7t(uint32_t a) {
    uint64_t t0 = xhw_time_us();
    (void)a;
    while (xhw_time_us() - t0 < 2500000u) yield();
    grave();
}
static void s7s(uint32_t a) {
    (void)a;
    create(15, F_S7T, 0, 105);
    start(15);
    ev("S2");
    grave();
}

/* a boot thread per scenario: it creates and starts the first thread, then parks */
static void* boot(void* p) {
    int* f = p;
    create(f[0], f[1], 0, f[2]);
    start(f[0]);
    return NULL;
}
static int run(int idx, int fn, int pri, int want, uint32_t ms) {
    static int args[16][3];
    pthread_t b;
    uint64_t t0 = xhw_time_us();
    s_nlog = 0;
    s_overlap = 0;
    args[idx][0] = idx, args[idx][1] = fn, args[idx][2] = pri;
    pthread_create(&b, NULL, boot, args[idx]);
    pthread_detach(b);
    while (s_nlog < want && xhw_time_us() - t0 < ms * 1000u) xhw_sleep_ms(2);
    xhw_sleep_ms(50);   /* anything that should not run has its chance */
    return s_nlog >= want;
}
static void order(const char* name, const char* want) {
    char got[256] = "";
    int i;
    for (i = 0; i < s_nlog; i++) strcat(got, s_log[i]), strcat(got, i + 1 < s_nlog ? " " : "");
    CHECK(!strcmp(got, want), "%s: order '%s', want '%s'", name, got, want);
}

int main(int argc, char** argv) {
    pthread_t irq;
    s_verbose = argc > 1 && !strcmp(argv[1], "-v");
    xrc_rdram_size = 8u << 20;
    xrc_rdram_host = calloc(1, xrc_rdram_size);
    xrc_rdram = xrc_rdram_host - 0x80000000u;
    s_entry[F_S1M] = s1m, s_entry[F_S1A] = s1a, s_entry[F_S1B] = s1b;
    s_entry[F_S2S] = s2s, s_entry[F_S2C] = s2c, s_entry[F_S2D] = s2d;
    s_entry[F_S3H] = s3h, s_entry[F_S3L] = s3l, s_entry[F_S4H] = s4h, s_entry[F_S4W] = s4w;
    s_entry[F_S5X] = s5x, s_entry[F_S5Y] = s5y, s_entry[F_S6Z] = s6z, s_entry[F_S7S] = s7s, s_entry[F_S7T] = s7t;
    mq_make(GRAVE, 1);

#ifndef TEST_SCEN_TOKEN
#define TEST_SCEN_TOKEN XRC_RUN_TOKEN
#endif
#if TEST_SCEN_TOKEN
    CHECK(run(1, F_S1M, 10, 9, 3000), "scenario 1 did not finish (%d events)", s_nlog);
    order("1", "M1 A1 M2 M3 A2 M4 B1 M5 B2");
    CHECK(!s_overlap, "1: %d overlaps", s_overlap);
    CHECK(run(4, F_S2S, 40, 4, 3000), "scenario 2 did not finish (%d events)", s_nlog);
    order("2", "C1 D1 C2 D2");
    CHECK(!s_overlap, "2: %d overlaps", s_overlap);
    CHECK(run(7, F_S3H, 50, 3, 3000), "scenario 3 did not finish (%d events)", s_nlog);
    order("3", "H1 H2 L1");
    s_count_overlap = 0;   /* 4: the interrupt's thread runs beside the holder on the host (no strict levels) */
    pthread_create(&irq, NULL, irq_thread, NULL);
    pthread_detach(irq);
    CHECK(run(9, F_S4W, 55, 3, 3000), "scenario 4 did not finish (%d events)", s_nlog);
    order("4", "W1 Hi W2");
    s_count_overlap = 1;
    CHECK(xu_token_handoffs() > 0, "no hand-off counted");
#else
    (void)irq;
    CHECK(run(14, F_S7S, 100, 1, 1200), "7 (5b.6): the starter never returned (%d events)", s_nlog);
#endif
    CHECK(run(11, F_S5Y, 65, 2, 3000), "5 (5b.5): a stop with interrupts off kept the lock (%d events)", s_nlog);
    order("5", "Y1 X2");
    CHECK(run(13, F_S6Z, 80, 1, 3000), "scenario 6 did not finish (%d events)", s_nlog);
    CHECK(s_level_in == 14 && s_level_mid == 14 && s_level_out == xu_level_of(5),
          "6 (5b.7): levels %d, %d, %d (want 14, 14, %d)", s_level_in, s_level_mid, s_level_out, xu_level_of(5));
    printf("test_thread (%s): %d checks, %d failed\n", XRC_RUN_TOKEN ? "run token" : "kernel levels", s_checks, s_fail);
    fflush(stdout);
    _exit(s_fail ? 1 : 0);   /* the scenarios' threads stay parked */
}
