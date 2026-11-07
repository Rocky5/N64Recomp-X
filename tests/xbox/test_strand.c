/* test_strand.c - a hand-written stack switch on strands (xbox/src/rt/strand.c, xrc_strand_switch), on pthreads.
 *
 * Built by tools/xbox/test_strand.py. Two coroutines trade the CPU the way Chopper Attack's tasks do: a routine leaves its
 * registers behind, goes to another stack, and comes back when something switches to its sp. The interpreter's part is
 * interp.c's (test_interp covers it); here "game code" is C calling xrc_strand_switch as run() does. */
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rt.h"
#include "xhw.h"

/* ---- the platform, on pthreads ---- */
struct xhw_event {
    pthread_mutex_t m;
    pthread_cond_t c;
    int set;
};
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
    (void)ms;
    pthread_mutex_lock(&e->m);
    while (!e->set) pthread_cond_wait(&e->c, &e->m);
    e->set = 0;
    pthread_mutex_unlock(&e->m);
    return 1;
}
#define MAX_TLS 8
static pthread_key_t s_keys[MAX_TLS];
static uint32_t s_nkeys;
uint32_t xhw_tls_alloc(void) {
    pthread_key_create(&s_keys[s_nkeys], NULL);
    return s_nkeys++;
}
void* xhw_tls_get(uint32_t slot) { return pthread_getspecific(s_keys[slot]); }
void xhw_tls_set(uint32_t slot, void* v) { pthread_setspecific(s_keys[slot], v); }
struct xhw_thread { int unused; };
static xhw_thread s_a_thread;
xhw_thread* xhw_thread_self(void) { return &s_a_thread; }
void xhw_thread_set_level(xhw_thread* t, int level) { (void)t, (void)level; }
typedef struct { void (*fn)(void*); void* arg; } Start;
static void* tramp(void* p) {
    Start s = *(Start*)p;
    free(p);
    s.fn(s.arg);
    return NULL;
}
static int s_spawned;
xhw_thread* xhw_thread_spawn(void (*fn)(void*), void* arg, int level, uint32_t stack) {
    pthread_t th;
    pthread_attr_t a;
    Start* s = malloc(sizeof *s);
    (void)level;
    s->fn = fn, s->arg = arg;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, stack < 256 * 1024 ? 256 * 1024 : stack);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    s_spawned++;
    return pthread_create(&th, &a, tramp, s) ? NULL : &s_a_thread;
}
void xhw_sleep_ms(uint32_t ms) { (void)ms; sched_yield(); }
uint64_t xhw_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}
void xhw_logf(const char* fmt, ...) { (void)fmt; }

/* ---- the runtime around it ---- */
static uint8_t s_ram[0x10000];
uint8_t* xrc_rdram;
uint32_t xrc_rdram_size = sizeof s_ram;
static char s_thr;   /* one N64 thread */
static __thread void* t_thr;
void* xu_self_thr(void) { return t_thr ? t_thr : &s_thr; }
void xu_adopt_thr(void* thr) { t_thr = thr; }
int xu_thr_level(void* thr) { (void)thr; return 8; }
uint32_t xu_program_gp(void) { return 0; }
void xrc_interp_ra(uint8_t* rdram, recomp_context* ctx, uint32_t entry) { (void)rdram, (void)ctx, (void)entry; abort(); }
void xrc_fatal_vram(const char* msg, uint32_t vram, uint32_t extra) {
    printf("  fatal: %s (%08x, %08x)\n", msg, (unsigned)vram, (unsigned)extra);
    exit(1);
}

void xrc_strand_switch(uint8_t* rdram, recomp_context* ctx, uint32_t target, uint32_t my_sp);

/* ---- the coroutines ---- */
#define SP_MAIN 0x80009000u
#define SP_A 0x80008000u
#define SP_B 0x80007000u
#define ENTRY_A 0x80001000u
#define ENTRY_B 0x80002000u
#define SENTINEL 0x8FFFFFF0u
#define EXIT_A 0x80003000u

static int s_fail;
#define CHECK(name, got, want) do { unsigned g_ = (unsigned)(got), w_ = (unsigned)(want); \
    if (g_ != w_) { printf("  %s: got %08x, want %08x\n", name, g_, w_); s_fail++; } } while (0)

/* a routine that yields: its registers go, those of the context at `sp` come */
static void go(recomp_context* ctx, uint32_t my_sp, uint32_t to_sp, uint32_t target, uint32_t tag) {
    ctx->r29 = (gpr)(int32_t)to_sp;
    ctx->r2 = (gpr)tag;
    ctx->r31 = (gpr)(int32_t)SENTINEL;
    xrc_strand_switch(xrc_rdram, ctx, target, my_sp);
}

static volatile int s_order[16], s_n;
static void note(int v) { s_order[s_n++] = v; }

static void entry_a(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    CHECK("a: the registers of the switch", ctx->r2, 1);
    CHECK("a: its stack", ctx->r29, SP_A);
    CHECK("a: the ra it was given", ctx->r31, SENTINEL);
    note(10);
    go(ctx, SP_A, SP_B, ENTRY_B, 2);    /* a new context */
    CHECK("a: back with b's registers", ctx->r2, 3);
    CHECK("a: back on its stack", ctx->r29, SP_A);
    note(11);
    go(ctx, SP_A, SP_MAIN, SENTINEL, 4);   /* to the one that started it */
    CHECK("a: again, with main's registers", ctx->r2, 5);
    note(12);
    go(ctx, SP_A, SP_B, SENTINEL, 6);
    abort();   /* the test ends with it parked */
}

static void entry_b(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    CHECK("b: the registers of the switch", ctx->r2, 2);
    note(20);
    go(ctx, SP_B, SP_A, SENTINEL, 3);
    CHECK("b: back with a's registers", ctx->r2, 6);
    note(21);
    go(ctx, SP_B, SP_MAIN, SENTINEL, 7);
    abort();
}

recomp_func_t* get_function(int32_t vram) { (void)vram; abort(); }
recomp_func_t* xrc_try_function(uint32_t vram) {
    return vram == ENTRY_A ? entry_a : vram == ENTRY_B ? entry_b : NULL;
}

int main(void) {
    static recomp_context ctx;
    static const int want[] = {10, 20, 11, 30, 12, 21, 31};
    unsigned i;
    xrc_rdram = s_ram - 0x80000000u;
    memset(&ctx, 0, sizeof ctx);
    go(&ctx, SP_MAIN, SP_A, ENTRY_A, 1);
    CHECK("main: back with a's registers", ctx.r2, 4);
    CHECK("main: back on its stack", ctx.r29, SP_MAIN);
    note(30);
    go(&ctx, SP_MAIN, SP_A, SENTINEL, 5);
    CHECK("main: back with b's registers", ctx.r2, 7);
    note(31);
    CHECK("order: how many ran", s_n, sizeof want / sizeof want[0]);
    for (i = 0; i < sizeof want / sizeof want[0]; i++) CHECK("order", s_order[i], want[i]);
    CHECK("each new context one host thread", s_spawned, 2);
    printf("test_strand: %s\n", s_fail ? "FAILED" : "passed");
    fflush(stdout);
    _Exit(s_fail != 0);   /* a and b stay parked */
}
