/* ai.c - the audio interface as a virtual DAC (PLAN.md 3.8; Wings64-X's).
 *
 * Games pace their synthesis by the DAC's backlog, so the DAC drains at the
 * AI rate in wall-clock time and music tempo tracks real time. osAiGetLength
 * is the buffer playing now, not the queued next one (wings64-dc's finding).
 * A late audio thread finds it overdrawn: the deficit is carried for up to
 * AI_CATCHUP_MAX frames, then dropped and counted as an underrun. OS_EVENT_AI
 * is posted when a buffer would have finished. Each buffer's samples go to
 * the AC97 through ../audio.c. */
#include "ultra.h"

void xrc_audio_rate(uint32_t hz);
void xrc_audio_buffer(uint32_t buf, uint32_t bytes);

#define AI_CATCHUP_MAX 4
/* AI_LEN is what is left of the buffer playing now, never more than that
 * buffer's own length; the backlog here can hold more (a game that queues
 * ahead after a hitch). Castlevania: Legacy of Darkness sizes its next frame
 * as target - length / 4 + 0x100 into a halfword: a length past one buffer
 * wrapped it, the frame overran its buffer into the next one's header, and
 * its audio manager read a sample as a pointer. -DXRC_AI_LEN_CLAMP=0: the
 * whole backlog less the queued buffer, as before */
#ifndef XRC_AI_LEN_CLAMP
#define XRC_AI_LEN_CLAMP 1
#endif
static uint32_t s_rate = 22050;
static int32_t s_pending, s_queued, s_prev;   /* s_prev: the buffer before the queued one, in samples */
static uint64_t s_last_us, s_frac;
static uint32_t s_underruns;
static uint64_t s_samples;

/* OS_EVENT_AI per buffer, and on every retrace's look: the AI interrupts
 * when the buffer playing ends and the queued one starts, and again when
 * that one ends; and on the N64 it comes whether or not the game asks the
 * AI anything. The event used to come only when the whole backlog ran out,
 * and only from inside an osAi* call: All-Star Baseball 2000's audio thread
 * waits on the event alone, made ~6 frames a second, and the sound events
 * its game thread posted meanwhile overflowed its player's queue (the
 * player's own tick was lost: a freeze). -DXRC_AI_EVENTS=0: as before */
#ifndef XRC_AI_EVENTS
#define XRC_AI_EVENTS 1
#endif
static void drain(void) {
    uint64_t now = xhw_time_us(), n;
    int64_t after;
    if (!s_last_us) s_last_us = now;
    s_frac += (now - s_last_us) * s_rate;
    s_last_us = now;
    n = s_frac / 1000000ull;
    s_frac -= n * 1000000ull;
    after = (int64_t)s_pending - (int64_t)n;
    if (s_pending > 0 && after <= 0) xu_post_event(OS_EVENT_AI);
    else if (XRC_AI_EVENTS && s_queued > 0 && s_pending > s_queued && after <= s_queued)
        xu_post_event(OS_EVENT_AI);   /* the buffer playing ended; the queued one plays */
    s_pending -= n > 0x10000000ull ? 0x10000000 : (int32_t)n;
}

/* vi.c, every retrace: the DAC drains on its own (the interrupt comes unasked) */
void xu_ai_tick(void);
void xu_ai_tick(void) {
    if (!XRC_AI_EVENTS || s_pending <= 0) return;
    xu_lock();   /* (the DAC's state is the game's audio thread's too) */
    drain();
    xu_unlock();
}

uint32_t xu_ai_underruns(void) { return s_underruns; }

/* osAiSetFrequency(u32): the DAC divides the video clock (osViClock, by
 * osTvType), so the rate is rounded to clock / divider, and that is what
 * libultra returns and the synthesizer then runs at (22050 asked: 22048) */
ULTRA(osAiSetFrequency) {
    uint32_t f = ARG(0), tv = rd32(0x80000300), clock, div;
    (void)rdram;
    clock = tv == 0 ? 49656530u : tv == 2 ? 48628316u : 48681812u;   /* PAL, MPAL, NTSC */
    if (!f) {
        RET(-1);
        return;
    }
    div = (clock + f / 2) / f;
    if (div < 132) {   /* AI_MIN_DAC_RATE */
        RET(-1);
        return;
    }
    s_rate = clock / div;
    xrc_audio_rate(s_rate);
    RET(s_rate);
}

/* A buffer queued while the DAC has nothing left of its backlog (the
 * carried deficit already covers it) starts at once, and the AI interrupts
 * as it starts: posted then. Resident Evil 2 queues 0x300-byte buffers (192
 * samples, under a retrace's 367 at 22 kHz) and waits on OS_EVENT_AI alone:
 * after its second buffer the backlog stayed at or below 0, the drain posts
 * only when a positive backlog runs out, and its DAC thread waited for good.
 * Its synthesizer makes a list only as the DAC's index moves, so no audio
 * task ran, and its scheduler starts its own RSP task only after an audio
 * task: no picture after the second frame. -DXRC_AI_SPENT=0: as before */
#ifndef XRC_AI_SPENT
#define XRC_AI_SPENT 1
#endif

/* osAiSetNextBuffer(buf, size) */
ULTRA(osAiSetNextBuffer) {
    uint32_t buf = ARG(0), size = ARG(1);
    (void)rdram;
    xu_lock();
    drain();
    if (s_pending < -AI_CATCHUP_MAX * 368) {
        if (s_samples) s_underruns++;
        s_pending = 0;
    }
    s_prev = s_queued;
    s_queued = (int32_t)(size >> 2);
    s_pending += s_queued;
    s_samples += size >> 2;
    if (XRC_AI_SPENT && s_queued > 0 && s_pending <= 0) xu_post_event(OS_EVENT_AI);
    xu_unlock();
    xrc_audio_buffer(buf, size);
    RET(0);
}

ULTRA(osAiGetLength) {
    (void)rdram;
    xu_lock();
    drain();
    if (s_pending <= 0) {
        RET(0);
    } else {
        int32_t playing = s_pending > s_queued ? s_pending - s_queued : s_pending;
        if (XRC_AI_LEN_CLAMP && s_pending > s_queued && s_prev > 0 && playing > s_prev) playing = s_prev;
        RET((uint32_t)playing << 2);
    }
    xu_unlock();
}

/* AI_STATUS from the virtual DAC: busy while a buffer plays, FIFO full while
 * another is queued behind it (the AI holds two). All-Star Baseball 2000
 * fills the FIFO until it reads full: always 0, it synthesized ~200 frames a
 * second and starved its game thread. -DXRC_AI_STATUS=0: 0, as before */
#ifndef XRC_AI_STATUS
#define XRC_AI_STATUS 1
#endif
uint32_t xu_ai_status(void) {
    uint32_t st;
    if (!XRC_AI_STATUS) return 0;
    xu_lock();
    drain();
    st = s_pending <= 0 ? 0 : s_pending > s_queued ? 0xC0000000u : 0x40000000u;   /* FULL | BUSY, BUSY */
    xu_unlock();
    return st;
}

ULTRA(osAiGetStatus) {
    (void)rdram;
    RET(xu_ai_status());
}
