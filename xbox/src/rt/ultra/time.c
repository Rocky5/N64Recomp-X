/* time.c - the CPU counter, osGetTime and the timers (PLAN.md 3.3).
 *
 * The VR4300's counter runs at 46.875 MHz (half the CPU clock): here it is
 * the ACPI timer scaled (xhw_clock.c), never the TSC, whatever CPU the
 * console has. osSetTime and the game's own writes to Count move an offset.
 * The timers are one runtime thread at level 14 sleeping until the next
 * deadline, as Wings64-X's shim; each OSTimer's fields stay in RDRAM. */
#include "ultra.h"

#define MAX_TIMERS 16

static int64_t s_offset;   /* osSetTime / mtc0 Count against the clock */

static uint64_t clock_count(void) { return xhw_time_us() * 46875ull / 1000ull; }
uint64_t xu_count64(void) { return clock_count() + (uint64_t)s_offset; }

ULTRA(osGetCount) {
    (void)rdram;
    RET((uint32_t)xu_count64());
}

ULTRA(osGetTime) {
    (void)rdram;
    RET64(xu_count64());
}

/* osSetTime(OSTime): the time in a0/a1 (a 64-bit argument, O32) */
ULTRA(osSetTime) {
    (void)rdram;
    s_offset = (int64_t)(xu_arg64(ctx, 0) - clock_count());
}

/* osSetCount(u32) (some libultras export it): the low word of the counter */
ULTRA(osSetCount) {
    uint64_t now = xu_count64();
    (void)rdram;
    s_offset += (int64_t)(((now & ~0xFFFFFFFFull) | ARG(0)) - now);
}

static struct { uint32_t t; uint64_t due, interval; uint32_t mq, msg; } s_timers[MAX_TIMERS];
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
                if (s_timers[i].interval) s_timers[i].due += s_timers[i].interval;
                else s_timers[i].t = 0;
                if (mq) xu_send(mq, msg, 0, 0);
            }
            if (s_timers[i].t && s_timers[i].due < next) next = s_timers[i].due;
        }
        xu_unlock();
        ms = next == ~0ull ? XHW_INFINITE : (uint32_t)((next - now) * 1000ull / 46875000ull) + 1;
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
