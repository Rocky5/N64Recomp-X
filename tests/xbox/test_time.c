/* test_time.c - the runtime's clock on the host (PLAN.md 3.13, 4.6).
 *
 * Built by tools/xbox/test_time.py with xbox/src/rt/ultra/time.c and stubs
 * for the platform: the "ACPI clock" (xhw_time_us) is this test's variable.
 * Checks: osGetCount/osGetTime are the clock scaled to the VR4300's 46.875
 * MHz counter (so whatever the CPU's speed, a second of real time is
 * 46,875,000 counts), the 32-bit counter wraps as the N64's did, osSetTime
 * moves only an offset, and nothing depends on anything but the clock. */
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ultra/ultra.h"

ULTRA(osGetCount);
ULTRA(osGetTime);
ULTRA(osSetTime);
ULTRA(osSetTimer);

static int s_fail, s_checks;
#define CHECK(cond, ...) do { \
        s_checks++; \
        if (!(cond)) { s_fail++; printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); \
                       printf("\n"); } \
    } while (0)

/* ---- the platform, stubbed ---- */
static uint64_t s_us;
uint64_t xhw_time_us(void) { return s_us; }
uint64_t xhw_game_time_us(void) { return s_us; }
static uint8_t s_ram[1u << 16] __attribute__((aligned(8)));
uint8_t* xrc_rdram = s_ram;
uint32_t xrc_tlb_to_phys(uint32_t vaddr) { (void)vaddr; return ~0u; }   /* no TLB on the host */
uint8_t* xrc_rdram_host = s_ram;
uint32_t xrc_rdram_size = sizeof s_ram;
void xu_lock(void) {}
void xu_unlock(void) {}
static unsigned s_sent;
int xu_send(uint32_t mq, uint32_t msg, int block, int jam) { (void)mq; (void)msg; (void)block; (void)jam; s_sent++; return 0; }
/* the timer thread: one pass of its loop at a time (its wait returns here) */
static void (*s_thread_fn)(void*);
static jmp_buf s_pass;
static uint32_t s_wait_ms;
xhw_thread* xu_spawn(void (*fn)(void*), void* arg, int level, uint32_t stack) {
    (void)arg; (void)level; (void)stack;
    s_thread_fn = fn;
    return (xhw_thread*)1;
}
xhw_event* xhw_event_create(void) { return (xhw_event*)1; }
void xhw_event_signal(xhw_event* e) { (void)e; }
int xhw_event_wait(xhw_event* e, uint32_t ms) {
    (void)e;
    s_wait_ms = ms;
    longjmp(s_pass, 1);
}
static void timer_pass(void) {
    if (!setjmp(s_pass)) s_thread_fn(NULL);
}
void xrc_fatal_vram(const char* what, uint32_t vram, uint32_t extra) {
    printf("fatal %s %08x %08x\n", what, vram, extra);
    exit(2);
}

static uint32_t count(void) {
    recomp_context c;
    memset(&c, 0, sizeof c);
    osGetCount_recomp(NULL, &c);
    return (uint32_t)c.r2;
}

static uint64_t time64(void) {
    recomp_context c;
    memset(&c, 0, sizeof c);
    osGetTime_recomp(NULL, &c);
    return ((uint64_t)(uint32_t)c.r2 << 32) | (uint32_t)c.r3;
}

int main(void) {
    recomp_context c;
    s_us = 0;
    CHECK(count() == 0, "count at 0 us: %u", count());
    s_us = 1000000;
    CHECK(count() == 46875000u, "a second is %u counts", count());
    CHECK(time64() == 46875000ull, "osGetTime after a second: %llu", (unsigned long long)time64());
    s_us = 100ull * 1000000;   /* 100 s: past the 32-bit counter's 91.6 s */
    CHECK(time64() == 4687500000ull, "osGetTime at 100 s: %llu", (unsigned long long)time64());
    CHECK(count() == (uint32_t)4687500000ull, "the 32-bit counter wraps: %u", count());
    /* osSetTime(1e9 counts) now: later readings run on from there */
    memset(&c, 0, sizeof c);
    c.r4 = 0;
    c.r5 = (gpr)(int32_t)1000000000;
    osSetTime_recomp(NULL, &c);
    CHECK(time64() == 1000000000ull, "osSetTime: %llu", (unsigned long long)time64());
    s_us += 2000000;
    CHECK(time64() == 1000000000ull + 93750000ull, "two seconds later: %llu", (unsigned long long)time64());
    /* a periodic timer held up for many intervals (a long hitch) fires once,
     * is re-armed from now, and the thread's next wait is short, not an
     * underflowed eternity (XRC_TIMER_CATCHUP) */
    memset(&c, 0, sizeof c);
    c.r4 = (gpr)(int32_t)0x80000100u;       /* t */
    c.r6 = 0; c.r7 = 46875;                 /* countdown 1 ms */
    c.r29 = (gpr)(int32_t)0x80000200u;      /* the stack arguments: interval 1 ms, mq, msg */
    *(uint32_t*)(s_ram + 0x210) = 0; *(uint32_t*)(s_ram + 0x214) = 46875;
    *(uint32_t*)(s_ram + 0x218) = 0x80000300u; *(uint32_t*)(s_ram + 0x21C) = 666;
    osSetTimer_recomp(NULL, &c);
    CHECK(s_thread_fn != NULL, "osSetTimer started no timer thread");
    if (s_thread_fn) {
        timer_pass();
        CHECK(s_wait_ms <= 2, "a 1 ms timer waits %u ms", (unsigned)s_wait_ms);
        s_us += 10000000;   /* 10 s held up */
        s_sent = 0;
        timer_pass();
        CHECK(s_sent == 1, "after 10 s: %u messages (want one)", s_sent);
        CHECK(s_wait_ms <= 2, "after 10 s the next wait is %u ms", (unsigned)s_wait_ms);
        s_us += 1000;
        timer_pass();
        CHECK(s_sent == 2, "the next interval: %u messages", s_sent);
    }
    printf("test_time: %d checks, %d failed\n", s_checks, s_fail);
    return s_fail != 0;
}
