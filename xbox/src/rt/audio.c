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
 * The first task names the microcode (PLAN.md 3.8's registry): its data
 * segment's command jump table, read in N64 order, goes to
 * asp_hle_identify, which tells aspMain (ABI1), the nead family (Mario
 * Kart 64, SM64 EU, Zelda) and n_aspMain (Paper Mario) apart by the
 * table's shape; asp_hle runs that ABI from then on. A microcode it does not
 * know is logged and plays silence: its tasks are dropped and the AI
 * buffers reach the AC97 as zeros (the virtual DAC still paces the game).
 * Kill switch -DXRC_AUDIO=0: the lists are counted and dropped, silent;
 * -DASP_HLE_NEAD=0 / -DASP_HLE_NAUDIO=0 silence one family. Test builds with -DXRC_AUDIO_DUMP=<s> log 3 s of the
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
static int s_up, s_on, s_id, s_silent;
static unsigned s_tasks, s_buffers, s_tasks_iv, s_peak;
static uint64_t s_frames_in, s_frames_dropped, s_task_us, s_stats_t0;

/* the HLE's RDRAM: a span of 4 KB (DMEM's size: no transfer is longer), or
 * up to RDRAM's end when that is nearer (Diddy Kong Racing saves its audio
 * buffers at 0x3FF0C0, under the top of 4 MB: every SAVEBUFF there was a
 * "bad address", dropped, before). -DXRC_ASP_SPAN_END=0: 4 KB always */
#ifndef XRC_ASP_SPAN_END
#define XRC_ASP_SPAN_END 1
#endif
static void* asp_addr(uint32_t a, void* user) {
    uint32_t p = a & 0x1FFFFFFFu;
    (void)user;
    if (XRC_ASP_SPAN_END && p < xrc_rdram_size && xrc_rdram_size - p < 0x1000u)
        return xrc_host(a, xrc_rdram_size - p);
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

static void audio_up(void) {
    if (s_up) return;
    s_up = 1;
    s_on = xhw_audio_init(s_rate);
    xhw_logf("[AUDIO] output %s at %u Hz", s_on ? "on" : "OFF (silent)", (unsigned)s_rate);
}

/* A game may run more than one audio microcode (Castlevania: Legacy of
 * Darkness: its lists for another one, read as aspMain's, saved buffers to
 * RDRAM 0x280 and wrote over its own code at 0x80000400). Each task's
 * microcode data is looked up here; a new one is identified as the first
 * was, up to four kept (ABI and resampler table each). -DXRC_AUDIO_MULTI_UC=0:
 * the first task's microcode for every task, as before */
#ifndef XRC_AUDIO_MULTI_UC
#define XRC_AUDIO_MULTI_UC 1
#endif
static struct { uint32_t data; asp_ucode_t uc; uint32_t lut_off; int lut_ok, silent; } s_ucs[4];
static unsigned s_nucs;
static uint32_t s_cur_data;

/* the first task: which ABI its microcode speaks, and its resampler table */
static void identify(uint32_t task);
static void switch_ucode(uint32_t task) {
    uint32_t data = rd32(task + TK_UCODE_DATA), lut_off;
    unsigned i;
    uint8_t be[512];
    if (!XRC_AUDIO_MULTI_UC || data == s_cur_data) return;
    for (i = 0; i < s_nucs && s_ucs[i].data != data; i++) {}
    if (i == s_nucs) {   /* a new one: identified as the first task's was */
        identify(task);
        return;
    }
    s_cur_data = data;
    s_silent = s_ucs[i].silent;
    asp_hle_set_ucode(&s_ucs[i].uc);
    lut_off = s_ucs[i].lut_off;
    if (s_ucs[i].lut_ok && xrc_host(data + lut_off, sizeof be)) {
        for (i = 0; i < sizeof be; i++) be[i] = rd8(data + lut_off + i);
        asp_hle_set_resample_lut_be(be);
    }
}

static void identify(uint32_t task) {
    uint32_t data = rd32(task + TK_UCODE_DATA), size = rd32(task + TK_UCODE_DATA_SIZE), lut_off = 0, i, n;
    uint8_t head[0x80];
    asp_ucode_t uc;
    int lut_ok;
    s_id = 1;
    n = size < sizeof head ? size : (uint32_t)sizeof head;
    if (!xrc_host(data, n)) n = 0;
    for (i = 0; i < n; i++) head[i] = rd8(data + i);
    asp_hle_identify(head, n, &uc);
    xhw_logf("[AUDIO] microcode %08x (%u bytes, text hash %08x), data %08x (%u bytes): %s (%u-entry table at data+%02x, "
             "opcodes %08x, flags %x)",
             (unsigned)rd32(task + TK_UCODE), (unsigned)rd32(task + TK_UCODE_SIZE),
             (unsigned)fnv_words(rd32(task + TK_UCODE), rd32(task + TK_UCODE_SIZE) ? rd32(task + TK_UCODE_SIZE) : 0x1000u),
             (unsigned)data, (unsigned)size, asp_hle_abi_name(uc.abi), (unsigned)uc.entries,
             (unsigned)(uc.abi == ASP_ABI_NAUDIO ? 0 : 0x10), (unsigned)uc.present, (unsigned)uc.flags);
    s_cur_data = data;
    s_silent = uc.abi == ASP_ABI_UNKNOWN;
    if (uc.abi == ASP_ABI_UNKNOWN) {
        xhw_logf("[AUDIO] unknown audio microcode: its tasks are skipped, the output is silence");
        lut_ok = 0;
    } else {
        asp_hle_set_ucode(&uc);
        lut_ok = find_lut(task, &lut_off);
        if (lut_ok) xhw_logf("[AUDIO] RSP audio HLE: resampler table at data+%03x", (unsigned)lut_off);
        else xhw_logf("[AUDIO] RSP audio HLE: no resampler table in the microcode data (linear fallback)");
    }
    if (s_nucs < 4) {
        s_ucs[s_nucs].data = data, s_ucs[s_nucs].uc = uc, s_ucs[s_nucs].lut_off = lut_off;
        s_ucs[s_nucs].lut_ok = lut_ok, s_ucs[s_nucs].silent = s_silent;
        s_nucs++;
    }
}

void xrc_audio_task(uint32_t task) {
    uint32_t list = rd32(task + TK_DATA_PTR), bytes = rd32(task + TK_DATA_SIZE);
    const uint32_t* cmds;
    uint64_t t0, us;
    s_tasks++;
#ifdef XRC_MEMBENCH   /* diagnosis: how fast RDRAM reads at a few physical offsets, once, at task 200 */
    if (s_tasks == 2 || s_tasks == 200) {
        static const uint32_t offs[] = {0x000000, 0x080000, 0x100000, 0x200000, 0x300000, 0x380000, 0x3C0000, 0x3F0000, 0x400000, 0x700000};
        unsigned k;
        for (k = 0; k < sizeof offs / sizeof offs[0]; k++) {
            volatile uint32_t* p = (volatile uint32_t*)xrc_host(0x80000000u | offs[k], 0x10000);
            uint32_t sum = 0, j;
            uint64_t t0 = xhw_time_us();
            if (!p) continue;
            for (j = 0; j < 0x10000 / 4; j++) sum += p[j];
            {
                void xhw_page_info(uint32_t va, uint32_t* pde, uint32_t* pte);
                uint32_t pde, pte;
                xhw_page_info((uint32_t)(uintptr_t)p, &pde, &pte);
            if (offs[k] < 0x100000 && xrc_tlb_to_phys(0x00100000u + offs[k]) == offs[k]) {   /* the same frames through a KUSEG alias */
                volatile uint32_t* q = (volatile uint32_t*)(xrc_rdram + 0x00100000u + offs[k]);
                uint32_t s2 = 0, j2;
                uint64_t t1 = xhw_time_us();
                for (j2 = 0; j2 < 0x10000 / 4; j2++) s2 += q[j2];
                xhw_logf("[MEMB]   through KUSEG %08x at %p: %u us (%08x)", (unsigned)(0x00100000u + offs[k]), (void*)q,
                         (unsigned)(xhw_time_us() - t1), (unsigned)s2);
            }
                xhw_logf("[MEMB] task %u phys %06x at %p: 64 KB in %u us (%08x) pde %08x pte %08x", s_tasks,
                         (unsigned)offs[k], (void*)p, (unsigned)(xhw_time_us() - t0), (unsigned)sum, (unsigned)pde,
                         (unsigned)pte);
            }
        }
    }
#endif
    if (!XRC_AUDIO) return;
    audio_up();
    if (!s_id) identify(task);
    else switch_ucode(task);
    if (s_silent) return;
    cmds = (const uint32_t*)xrc_host(list, bytes);
    if (!cmds) {
        static unsigned s_bad;
        if (++s_bad <= 3)
            xhw_logf("[AUDIO] task %u: list %08x (%u bytes) outside RDRAM: skipped", s_tasks, (unsigned)list, (unsigned)bytes);
        return;
    }
#ifdef XRC_AUDIO_DUMP   /* diagnosis: -DXRC_AUDIO_DUMP=<task>, that task's commands */
    if (s_tasks + 1 == (unsigned)(XRC_AUDIO_DUMP)) {
        uint32_t k;
        xhw_logf("[ACMD] task %u: %u bytes of commands", s_tasks + 1, (unsigned)bytes);
        for (k = 0; k + 1 < bytes / 4u && k < 2400; k += 8)
            xhw_logf("[ACMD] %08x %08x  %08x %08x  %08x %08x  %08x %08x", (unsigned)cmds[k], (unsigned)cmds[k + 1],
                     (unsigned)cmds[k + 2], (unsigned)cmds[k + 3], (unsigned)cmds[k + 4], (unsigned)cmds[k + 5],
                     (unsigned)cmds[k + 6], (unsigned)cmds[k + 7]);
    }
#endif
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
                 (unsigned)st->bad_addr, (unsigned)st->cmds[32]);
        if (st->low_n)
            xhw_logf("[AUDIO] %u stores below RDRAM 0x1000: the first by %08x %08x at %08x, task %u command %u after "
                     "%08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x",
                     (unsigned)st->low_n, (unsigned)st->low_w0, (unsigned)st->low_w1, (unsigned)st->low_addr,
                     (unsigned)st->low_task, (unsigned)st->low_index, st->low_prev[0], st->low_prev[1], st->low_prev[2],
                     st->low_prev[3], st->low_prev[4], st->low_prev[5], st->low_prev[6], st->low_prev[7],
                     st->low_prev[8], st->low_prev[9], st->low_prev[10], st->low_prev[11]);
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
    if (!s_up) audio_up();
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
            uint32_t x = s_silent ? 0 : w[done + i];
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
