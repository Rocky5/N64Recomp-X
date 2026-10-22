/* time.c - the CPU counter, osGetTime and the timers (PLAN.md 3.3).
 *
 * The VR4300's counter runs at 46.875 MHz (half the CPU clock): here it is
 * the ACPI timer scaled (xhw_clock.c), never the TSC, whatever CPU the
 * console has. osSetTime and the game's own writes to Count move an offset.
 * The timers are one runtime thread at level 14 sleeping until the next
 * deadline, as Wings64-X's shim; each OSTimer's fields stay in RDRAM. */
#include "ultra.h"

/* libultra keeps a list; 16 ended in a fatal card for a game with more pending (review 5b.18) */
#define MAX_TIMERS 64

static int64_t s_offset;   /* osSetTime / mtc0 Count against the clock */

static uint64_t clock_count(void) { return xhw_game_time_us() * 46875ull / 1000ull; }
uint64_t xu_count64(void) { return clock_count() + (uint64_t)s_offset; }

ULTRA(osGetCount) {
    (void)rdram;
    RET((uint32_t)xu_count64());
}

ULTRA(osGetTime) {
    (void)rdram;
    RET64(xu_count64());
}

static void timers_shift(int64_t d);
/* osSetTime(OSTime): the time in a0/a1 (a 64-bit argument, O32) */
ULTRA(osSetTime) {
    (void)rdram;
    int64_t old = s_offset;
    s_offset = (int64_t)(xu_arg64(ctx, 0) - clock_count());
    timers_shift(s_offset - old);   /* libultra's osSetTime leaves Count alone: pending timers keep their waits */
}

/* the counter's low word set (osSetCount; interp.c's mtc0 Count) */
void xu_count_set32(uint32_t v) {
    uint64_t now = xu_count64();
    s_offset += (int64_t)(((now & ~0xFFFFFFFFull) | v) - now);
}

/* osSetCount(u32) (some libultras export it): the low word of the counter */
ULTRA(osSetCount) {
    (void)rdram;
    xu_count_set32(ARG(0));
}

/* A periodic timer that fell more than one interval behind (the thread held
 * up: a 10 s screenshot in xemu, any long hitch) went on with its deadline
 * in the past: the wait to it underflowed and the timer thread slept for
 * good, so every timer stopped (Ocarina of Time's graphics thread then
 * waited for ever: no task done, no RCP-hung timeout). It is now re-armed
 * from the present, as libultra re-arms it from the interrupt, and a past
 * deadline waits 0 ms. -DXRC_TIMER_CATCHUP=0: as before */
#ifndef XRC_TIMER_CATCHUP
#define XRC_TIMER_CATCHUP 1
#endif

static struct { uint32_t t; uint64_t due, interval; uint32_t mq, msg; } s_timers[MAX_TIMERS];
/* deadlines are on the shifted counter: a jump of it (osSetTime) moves them with it (review 5b.18) */
static void timers_shift(int64_t d) {
    int i;
    if (!d) return;
    xu_lock();
    for (i = 0; i < MAX_TIMERS; i++)
        if (s_timers[i].t) s_timers[i].due += (uint64_t)d;
    xu_unlock();
}
static volatile int s_up;
static xhw_event* s_kick;

static void timer_thread(void* arg) {
    (void)arg;
    for (;;) {
        uint64_t now, next = ~0ull;
        int i;
        uint32_t ms;
        xu_lock();
        now = xu_count64();
        for (i = 0; i < MAX_TIMERS; i++) {
            if (!s_timers[i].t) continue;
            if (s_timers[i].due <= now) {
                uint32_t mq = s_timers[i].mq, msg = s_timers[i].msg;
                if (s_timers[i].interval) {
                    s_timers[i].due += s_timers[i].interval;
                    if (XRC_TIMER_CATCHUP && s_timers[i].due <= now) s_timers[i].due = now + s_timers[i].interval;
                } else {
                    s_timers[i].t = 0;
                }
                if (mq) xu_send(mq, msg, 0, 0);
            }
            if (s_timers[i].t && s_timers[i].due < next) next = s_timers[i].due;
        }
        xu_unlock();
        if (next == ~0ull) ms = XHW_INFINITE;
        else if (XRC_TIMER_CATCHUP && next <= now) ms = 0;
        else ms = (uint32_t)((next - now) * 1000ull / 46875000ull) + 1;
        xhw_event_wait(s_kick, ms);
    }
}

/* osSetTimer(t, OSTime countdown (a2/a3), OSTime interval (0x10/0x14), mq (0x18), msg (0x1C)) */
ULTRA(osSetTimer) {
    uint32_t t = ARG(0), mq = ARG(6), msg = ARG(7);
    uint64_t countdown = xu_arg64(ctx, 2), interval = xu_arg64(ctx, 4);
    int i, slot = -1;
    (void)rdram;
    if (!s_up) {
        s_up = 1;
        s_kick = xhw_event_create();
        xu_spawn(timer_thread, NULL, 14, 32 * 1024);
    }
    wr64(t + TM_INTERVAL, interval);
    wr64(t + TM_VALUE, countdown);
    wr32(t + TM_MQ, mq);
    wr32(t + TM_MSG, msg);
    xu_lock();
    for (i = 0; i < MAX_TIMERS; i++)
        if (s_timers[i].t == t || (!s_timers[i].t && slot < 0)) {
            slot = i;
            if (s_timers[i].t == t) break;
        }
    if (slot < 0) xrc_fatal_vram("osSetTimer: out of timers", t, MAX_TIMERS);
    s_timers[slot].t = t;
    s_timers[slot].due = xu_count64() + (countdown ? countdown : interval);
    s_timers[slot].interval = interval;
    s_timers[slot].mq = mq;
    s_timers[slot].msg = msg;
    xu_unlock();
    xhw_event_signal(s_kick);
    RET(0);
}

ULTRA(osStopTimer) {
    uint32_t t = ARG(0);
    int i, found = 0;
    (void)rdram;
    xu_lock();
    for (i = 0; i < MAX_TIMERS; i++)
        if (s_timers[i].t == t) {
            s_timers[i].t = 0;
            found = 1;
        }
    xu_unlock();
    RET(found ? 0 : -1);
}
