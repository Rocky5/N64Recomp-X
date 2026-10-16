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
static uint32_t s_rate = 22050;
static int32_t s_pending, s_queued;
static uint64_t s_last_us, s_frac;
static uint32_t s_underruns;
static uint64_t s_samples;

static void drain(void) {
    uint64_t now = xhw_time_us(), n;
    if (!s_last_us) s_last_us = now;
    s_frac += (now - s_last_us) * s_rate;
    s_last_us = now;
    n = s_frac / 1000000ull;
    s_frac -= n * 1000000ull;
    if (s_pending > 0 && (int64_t)s_pending - (int64_t)n <= 0) xu_post_event(OS_EVENT_AI);
    s_pending -= n > 0x10000000ull ? 0x10000000 : (int32_t)n;
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

/* osAiSetNextBuffer(buf, size) */
ULTRA(osAiSetNextBuffer) {
    uint32_t buf = ARG(0), size = ARG(1);
    (void)rdram;
    drain();
    if (s_pending < -AI_CATCHUP_MAX * 368) {
        if (s_samples) s_underruns++;
        s_pending = 0;
    }
    s_queued = (int32_t)(size >> 2);
    s_pending += s_queued;
    s_samples += size >> 2;
    xrc_audio_buffer(buf, size);
    RET(0);
}

ULTRA(osAiGetLength) {
    (void)rdram;
    drain();
    if (s_pending <= 0) RET(0);
    else RET((uint32_t)(s_pending > s_queued ? s_pending - s_queued : s_pending) << 2);
}

ULTRA(osAiGetStatus) {
    (void)rdram;
    RET(0);
}
