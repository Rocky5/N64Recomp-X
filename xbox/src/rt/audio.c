/* audio.c - audio tasks through asp_hle and the AI buffers to the AC97
 * (PLAN.md 3.8, the `hle` path; Wings64-X's game/audio.c on this data model).
 *
 * osSpTaskStartGo hands an M_AUDTASK here (ultra/sp.c), on the game's own
 * audio thread: its Acmd list (native words in the image) runs through the
 * RSP audio HLE (xbox/src/hw/asp_hle.c, built word-swapped: the list's
 * addresses are physical, the DMA transfers swap halfwords). The AI buffer
 * the list wrote reaches osAiSetNextBuffer (ultra/ai.c, the virtual DAC
 * that paces the game) and comes here: its words hold L in the high half
 * and R in the low, so each becomes a native L, R pair on the way into the
 * AC97 pump's ring (xbox/src/hw/xhw_audio.c, to 48 kHz). The volume setting
 * scales it there.
 *
 * The A_RESAMPLE filter table sits in the microcode's data segment, which
 * the game DMAs with each task: the first task's segment is searched for a
 * 512-byte table that passes asp_hle's checks (phase 63-p mirrors phase p,
 * the taps of each phase sum to ~1.0). Nothing from the ROM is stored. With
 * no table found the linear fallback plays (asp_hle.c DIVERGES) and the
 * boot log says so.
 *
 * The microcode is not identified yet: every audio task is taken for the
 * stock aspMain ABI and its text's hash is logged, for the registry M4 adds
 * (PLAN.md 3.8). Kill switch -DXRC_AUDIO=0: the lists are counted and
 * dropped, silent. Test builds with -DXRC_AUDIO_DUMP=<s> log 3 s of the
 * output from second <s> as [AUDDUMP] lines (tools/xbox/auddump_to_wav.py;
 * the WAV is the game's sound: never in the tree). */
#include "../hw/asp_hle.h"
#include "settings.h"
#include "ultra/ultra.h"

#ifndef XRC_AUDIO
#define XRC_AUDIO 1
#endif
#define STATS_US 10000000ull   /* an [AUDIO] line every 10 s (test builds) */
#ifndef XRC_AUDIO_DUMP
#define XRC_AUDIO_DUMP 0
#endif

uint32_t xu_ai_underruns(void);

static uint32_t s_rate = 22050;
static int s_up, s_on;
static unsigned s_tasks, s_buffers, s_tasks_iv, s_peak;
static uint64_t s_frames_in, s_frames_dropped, s_task_us, s_stats_t0;

static void* asp_addr(uint32_t a, void* user) {
    (void)user;
    return xrc_host(a, 0x1000);
}

static uint32_t fnv_words(uint32_t a, uint32_t n) {
    uint32_t h = 2166136261u, i;
    for (i = 0; i + 4 <= n; i += 4) h = (h ^ rd32(a + i)) * 16777619u;
    return h;
}

/* the resampler table in the task's microcode data, in N64 byte order */
static int find_lut(uint32_t task, uint32_t* where) {
    uint32_t data = rd32(task + TK_UCODE_DATA), size = rd32(task + TK_UCODE_DATA_SIZE), off, i;
    uint8_t be[512];
    if (!xrc_host(data, size) || size < sizeof be) return 0;
    for (off = 0; off + sizeof be <= size; off += 8) {
        for (i = 0; i < sizeof be; i++) be[i] = rd8(data + off + i);
        if (asp_hle_set_resample_lut_be(be)) {
            *where = off;
            return 1;
        }
    }
    return 0;
}

static void audio_up(uint32_t task) {
    uint32_t lut_off = 0;
    int lut_ok = 0;
    if (s_up) return;
    s_up = 1;
    if (task) {
        lut_ok = find_lut(task, &lut_off);
        xhw_logf("[AUDIO] microcode %08x (%u bytes, text hash %08x), data %08x (%u bytes): taken as aspMain",
                 (unsigned)rd32(task + TK_UCODE), (unsigned)rd32(task + TK_UCODE_SIZE),
                 (unsigned)fnv_words(rd32(task + TK_UCODE), rd32(task + TK_UCODE_SIZE) ? rd32(task + TK_UCODE_SIZE) : 0x1000u),
                 (unsigned)rd32(task + TK_UCODE_DATA), (unsigned)rd32(task + TK_UCODE_DATA_SIZE));
    }
    s_on = xhw_audio_init(s_rate);
    if (lut_ok)
        xhw_logf("[AUDIO] RSP audio HLE: resampler table at data+%03x; output %s at %u Hz", (unsigned)lut_off,
                 s_on ? "on" : "OFF (silent)", (unsigned)s_rate);
    else
        xhw_logf("[AUDIO] RSP audio HLE: no resampler table in the microcode data (linear fallback); output %s at %u Hz",
                 s_on ? "on" : "OFF (silent)", (unsigned)s_rate);
}

void xrc_audio_task(uint32_t task) {
    uint32_t list = rd32(task + TK_DATA_PTR), bytes = rd32(task + TK_DATA_SIZE);
    const uint32_t* cmds;
    uint64_t t0, us;
    s_tasks++;
    if (!XRC_AUDIO) return;
    audio_up(task);
    cmds = (const uint32_t*)xrc_host(list, bytes);
    if (!cmds) {
        if (s_tasks <= 3) xhw_logf("[AUDIO] task %u: list %08x (%u bytes) outside RDRAM", s_tasks, (unsigned)list, (unsigned)bytes);
        return;
    }
    t0 = xhw_time_us();
    asp_hle_run(cmds, bytes / 8u, asp_addr, NULL);
    us = xhw_time_us() - t0;
    s_task_us += us;
    xhw_perf_add(XHW_PERF_AUDIO, us);
    if (XHW_TEST_BUILD && s_tasks <= 3)
        xhw_logf("[AUDIO] task %u: %u commands in %u us", s_tasks, (unsigned)(bytes / 8u), (unsigned)us);
    s_tasks_iv++;
    if (!s_stats_t0) s_stats_t0 = t0;
    if (XHW_TEST_BUILD && t0 - s_stats_t0 >= STATS_US) {
        const asp_hle_stats_t* st = asp_hle_stats();
        xhw_logf("[AUDIO] %u tasks (%u us each), %u buffers, %u frames in, %u dropped (ring full), %u skipped "
                 "(latency cap), peak %u, %u AI underruns, %u bad addresses, %u unknown commands",
                 s_tasks, (unsigned)(s_task_us / (s_tasks_iv ? s_tasks_iv : 1)), s_buffers, (unsigned)s_frames_in,
                 (unsigned)s_frames_dropped, (unsigned)(xhw_audio_skipped() / 2u), s_peak, (unsigned)xu_ai_underruns(),
                 (unsigned)st->bad_addr, (unsigned)st->cmds[16]);
        s_task_us = 0;
        s_tasks_iv = 0;
        s_peak = 0;
        s_stats_t0 = t0;
    }
}

/* osAiSetFrequency's rate (the DAC's actual one) */
void xrc_audio_rate(uint32_t hz) {
    xhw_logf("[AUDIO] AI rate %u Hz", (unsigned)hz);
    if (!hz) return;
    s_rate = hz;
    if (s_on) xhw_audio_set_rate(hz);
}

#if XRC_AUDIO_DUMP
static void dump(const int16_t* lr, uint32_t n) {
    static uint64_t seen, logged;
    static const char hex[] = "0123456789abcdef";
    char line[16 + 64 * 8 + 1];
    uint32_t i = 0;
    uint64_t from = (uint64_t)XRC_AUDIO_DUMP * s_rate, upto = from + 3ull * s_rate;
    if (!logged) xhw_logf("[AUDDUMP] rate %u", (unsigned)s_rate), logged = 1;
    while (i < n) {
        uint32_t k = 0, j;
        char* o = line;
        if (seen + i < from || seen + i >= upto) {
            i++;
            continue;
        }
        memcpy(o, "[AUDDUMP] ", 10), o += 10;
        for (; k < 64 && i < n && seen + i < upto; k++, i++)
            for (j = 0; j < 2; j++) {
                uint16_t v = (uint16_t)lr[2 * i + j];
                *o++ = hex[v >> 12], *o++ = hex[(v >> 8) & 15], *o++ = hex[(v >> 4) & 15], *o++ = hex[v & 15];
            }
        *o = 0;
        xhw_logf("%s", line);
    }
    seen += n;
}
#endif

/* osAiSetNextBuffer's buffer: `bytes` of s16 stereo at N64 address `buf` */
void xrc_audio_buffer(uint32_t buf, uint32_t bytes) {
    static int16_t out[2 * 512];
    const uint32_t* w;
    uint32_t frames = bytes / 4u, space, done = 0;
    int32_t vol = g_xrc_settings.volume;
    s_buffers++;
    if (!XRC_AUDIO || !frames) return;
    if (!s_up) audio_up(0);
    if (!s_on) return;
    w = (const uint32_t*)xrc_host(buf, bytes);
    if (!w) return;
    if (XHW_TEST_BUILD && s_buffers <= 3) xhw_logf("[AUDIO] AI buffer %u: %u bytes at %08x", s_buffers, (unsigned)bytes, (unsigned)buf);
    space = xhw_audio_space();
    if (frames > space) s_frames_dropped += frames - space, frames = space;
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    while (done < frames) {
        uint32_t n = frames - done > 512 ? 512 : frames - done, i;
        for (i = 0; i < n; i++) {
            uint32_t x = w[done + i];
            int32_t l = (int16_t)(x >> 16), r = (int16_t)x;
            unsigned al = (unsigned)(l < 0 ? -l : l);
            if (al > s_peak) s_peak = al;
            if (vol != 100) l = l * vol / 100, r = r * vol / 100;
            out[2 * i] = (int16_t)l, out[2 * i + 1] = (int16_t)r;
        }
#if XRC_AUDIO_DUMP
        if (XHW_TEST_BUILD) dump(out, n);
#endif
        xhw_audio_write(out, n);
        done += n;
    }
    s_frames_in += frames;
}
