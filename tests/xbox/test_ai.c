/* test_ai.c - the runtime's audio interface (ultra/ai.c) on the host.
 *
 * Built by tools/xbox/test_ai.py with xbox/src/rt/ultra/ai.c and stubs for
 * the platform: the clock (xhw_time_us) is this test's variable, OS_EVENT_AI
 * is counted. Checks: a buffer drains at the AI rate in wall-clock time and
 * its event comes when it has played; a buffer queued while the DAC's
 * backlog is spent (the carried deficit covers it) is posted at once
 * (XRC_AI_SPENT: Resident Evil 2's 192-sample buffers, shorter than a
 * retrace, waited for an event that never came). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ultra/ultra.h"

ULTRA(osAiSetFrequency);
ULTRA(osAiSetNextBuffer);
ULTRA(osAiGetLength);
void xu_ai_tick(void);

static int s_fail, s_checks;
#define CHECK(cond, ...) do { \
        s_checks++; \
        if (!(cond)) { s_fail++; printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); \
                       printf("\n"); } \
    } while (0)

/* ---- the platform, stubbed ---- */
static uint64_t s_us;
uint64_t xhw_time_us(void) { return s_us; }
static uint8_t s_ram[1u << 16] __attribute__((aligned(8)));
uint8_t* xrc_rdram = s_ram;
uint8_t* xrc_rdram_host = s_ram;
uint32_t xrc_rdram_size = sizeof s_ram;
uint32_t xrc_tlb_to_phys(uint32_t vaddr) { (void)vaddr; return ~0u; }
void xu_lock(void) {}
void xu_unlock(void) {}
static unsigned s_events;
void xu_post_event(int ev) { if (ev == OS_EVENT_AI) s_events++; }
void xrc_audio_rate(uint32_t hz) { (void)hz; }
void xrc_audio_buffer(uint32_t buf, uint32_t bytes) { (void)buf; (void)bytes; }

static void next_buffer(uint32_t bytes) {
    recomp_context c;
    memset(&c, 0, sizeof c);
    c.r4 = (gpr)(int32_t)0x80001000u;
    c.r5 = bytes;
    osAiSetNextBuffer_recomp(NULL, &c);
}

int main(void) {
    recomp_context c;
    memset(&c, 0, sizeof c);
    wr32(0x80000300, 1);   /* osTvType: NTSC */
    c.r4 = 22050;
    osAiSetFrequency_recomp(NULL, &c);
    CHECK((uint32_t)c.r2 == 22047u, "22050 asked: %u", (unsigned)c.r2);   /* NTSC's clock / its divider */

    /* a retrace's worth (368 samples, 1472 bytes): its event when it has played, not before */
    s_us = 1000000;
    next_buffer(1472);
    CHECK(s_events == 0, "no event as it starts behind nothing: %u", s_events);
    s_us += 8000;
    xu_ai_tick();
    CHECK(s_events == 0, "half played: %u events", s_events);
    s_us += 9000;   /* 17 ms: 374 samples at 22047 Hz, past its 368 */
    xu_ai_tick();
    CHECK(s_events == 1, "played: %u events", s_events);

    /* the DAC overdrawn (late by a retrace), then a short buffer: the deficit covers it, it has played */
    s_us += 17000;
    xu_ai_tick();
    s_events = 0;
    next_buffer(0x300);   /* 192 samples */
    CHECK(s_events == 1, "a spent buffer is posted at once: %u events", s_events);
    s_events = 0;
    for (int i = 0; i < 8; i++) {   /* Resident Evil 2's loop: a buffer per event, a retrace apart */
        s_us += 16683;
        xu_ai_tick();
        next_buffer(0x300);
    }
    CHECK(s_events >= 6, "the short buffers keep their events coming (one a buffer, or at the next tick): %u", s_events);

    printf("test_ai: %d checks, %d failed\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
