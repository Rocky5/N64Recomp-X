/* mmio.c - the RCP's registers, the PIF and the cartridge through KSEG1
 * (docs/decisions.md "Hardware registers"; PLAN.md 3.14).
 *
 * The runtime replaces libultra's I/O by name; a build from the ROM alone
 * meets versions the analyser did not name (a later osCartRomInit polling
 * PI_STATUS, __osSiRawStartDma, osAiSetNextBuffer, __osViSwapContext ...)
 * and recompiles them, and their loads and stores at 0xA4xxxxxx land at host
 * P + 0xA4xxxxxx, where nothing is. Two parts:
 *
 *   - the model: the analyser sends every live function with a constant
 *     hardware address in it to the interpreter (romonly.py), whose loads and
 *     stores in KSEG1 outside RDRAM come here. A write with an effect does
 *     it through the runtime's own paths: PI_WR_LEN is a ROM (or save) DMA
 *     (pi.c) and raises the PI event, SI_PIF_ADDR_WR64B/RD64B run the PIF
 *     (si.c), AI_LEN queues a buffer and AI_DACRATE sets the rate (ai.c),
 *     VI_ORIGIN swaps the framebuffer (vi.c), SP DMA moves between RDRAM and
 *     the SP's memory and clearing SP_STATUS's halt runs the task the DMA put
 *     at DMEM 0xFC0 (sp.c). Reads give the idle machine: DMA never busy, the
 *     SP halted, the AI length from ai.c, VI_CURRENT from the clock (ACPI),
 *     MI_VERSION a retail RCP's; the cartridge reads through the ROM path,
 *     domain 2 through the save.
 *   - the pages: each register block's 4 KB page is a page of our own aliased
 *     at its host address (xhw_alias_page), holding the registers as the
 *     model keeps them, so code that still reaches them recompiled (through
 *     a pointer the analyser could not follow) reads idle values instead of
 *     faulting. Its writes land there with no effect; the VI thread puts the
 *     status words back every retrace (xrc_mmio_vi).
 *
 * DIVERGES: DMA completes at once (no busy period, no interrupt latency);
 * a recompiled store to a register has no effect; VI_ORIGIN's one-line
 * offset is taken off by VI_WIDTH (an interlaced odd field shows a line
 * low); the SP runs a task only through the DMEM task block (no direct
 * IMEM microcode); MI_INTR stays clear (the runtime posts the events).
 * Kill switch: -DXRC_MMIO=0 (no model, no pages: KSEG1 register accesses
 * fault with the crash guard's card as before). */
#include <string.h>

#include "rt.h"
#include "xhw.h"
#include "ultra/ultra.h"

#ifndef XRC_MMIO
#define XRC_MMIO 1
#endif

ULTRA(osAiSetNextBuffer);
ULTRA(osAiGetLength);
ULTRA(osAiSetFrequency);
ULTRA(osViSwapBuffer);
ULTRA(__osSiRawStartDma);
uint32_t xu_pi_word(uint32_t dev);
uint32_t xu_pi_dma(uint32_t dev, uint32_t dram, uint32_t n);
int xu_pi_save_dma(uint32_t addr, uint32_t dram, uint32_t n, int write);
int xu_pi_save_io(uint32_t addr, uint32_t* word, int write);

/* the pages, by N64 physical address */
static const uint32_t k_page[] = {0x04000000u, 0x04001000u, 0x04040000u, 0x04080000u, 0x04100000u, 0x04200000u,
                                  0x04300000u, 0x04400000u, 0x04500000u, 0x04600000u, 0x04700000u, 0x04800000u,
                                  0x1FC00000u};
#define NPAGE (sizeof k_page / sizeof k_page[0])
static uint32_t s_store[NPAGE][1024];   /* when a page could not be aliased (and on the host) */
static uint32_t* s_page[NPAGE];
static uint32_t s_none;                 /* any other register: reads 0 */
static uint32_t s_task;                 /* the RDRAM address the SP's DMA took to DMEM 0xFC0 */
static unsigned s_logged;

#define SPS_HALT 0x1u
#define SPS_BROKE 0x2u
#define SPS_SIG2 0x200u   /* libultra's SP_STATUS_TASKDONE */

static uint32_t* reg(uint32_t phys) {
    unsigned i;
    for (i = 0; i < NPAGE; i++)
        if ((phys & ~0xFFFu) == k_page[i]) return &(s_page[i] ? s_page[i] : s_store[i])[(phys & 0xFFFu) >> 2];
    s_none = 0;
    return &s_none;
}
#define R(phys) (*reg(phys))

static void idle(void) {
    R(0x04040010u) = SPS_HALT | SPS_BROKE | SPS_SIG2 | (R(0x04040010u) & 0x7F80u);   /* SP_STATUS: as __osSpGetStatus */
    R(0x04040014u) = R(0x04040018u) = R(0x0404001Cu) = 0;               /* SP DMA full, busy; semaphore */
    R(0x0410000Cu) = 0x80u;                                             /* DPC_STATUS: CBUF_READY */
    R(0x04300004u) = 0x02020102u;                                       /* MI_VERSION */
    R(0x04300008u) = 0;                                                 /* MI_INTR */
    R(0x0450000Cu) = 0;                                                 /* AI_STATUS */
    R(0x04600010u) = 0;                                                 /* PI_STATUS */
    R(0x04800018u) = 0;                                                 /* SI_STATUS */
}

void xrc_mmio_init(void) {
    unsigned i;
#if XRC_MMIO
    for (i = 0; i < NPAGE; i++) {
        uint32_t host = (uint32_t)(uintptr_t)(xrc_rdram + 0xA0000000u + k_page[i]);
        s_page[i] = (uint32_t*)xhw_alias_page(host);
        if (!s_page[i] && s_logged++ < 4)
            xhw_logf("[MMIO] the page at %08x is not aliased (host %08x in use): recompiled code faults there",
                     (unsigned)(0xA0000000u + k_page[i]), (unsigned)host);
    }
#endif
    (void)i;
    R(0x0470000Cu) = 0x14u, R(0x04700000u) = 0x0Eu, R(0x04700004u) = 0x40u;   /* RI_SELECT, MODE, CONFIG */
    /* the RSP's memories as the boot code leaves them: DMEM all ones and IMEM
     * 6103 at offset 0, which a game's copy-protection check reads (Pokemon
     * Snap; mem.c's shadow pages did it before the model) */
    R(0x04000000u) = 0xFFFFFFFFu, R(0x04001000u) = 6103;
    idle();
}

/* the VI thread, each retrace: the status words back for recompiled pollers */
void xrc_mmio_vi(void) {
#if XRC_MMIO
    idle();
#endif
}

/* a vaddr the model answers: KSEG1 past the RDRAM there is (memory a game
 * probes beyond it reads 0 and keeps nothing, as on an N64 without the
 * Expansion Pak) */
int xrc_mmio_is(uint32_t vaddr) {
    return XRC_MMIO && vaddr >= 0xA0000000u && vaddr < 0xC0000000u && (vaddr & 0x1FFFFFFFu) >= xrc_rdram_size;
}

static uint32_t call(void (*fn)(uint8_t*, recomp_context*), uint32_t a0, uint32_t a1) {
    recomp_context ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.r4 = (gpr)(int32_t)a0, ctx.r5 = (gpr)(int32_t)a1;
    fn(xrc_rdram, &ctx);
    return (uint32_t)ctx.r2;
}

/* the VI's current half-line from the clock: 525 lines (NTSC) a frame */
static uint32_t vi_line(void) {
    uint64_t us = xhw_time_us() % 16683u;
    return (uint32_t)(us * 525u / 16683u) & ~1u;
}

static void sp_dma(uint32_t len, int to_rdram) {
    uint32_t mem = R(0x04040000u) & 0x1FF8u, dram = R(0x04040004u) & 0x00FFFFF8u;
    uint32_t n = (len & 0xFF8u) + 8, count = ((len >> 12) & 0xFFu) + 1, skip = (len >> 20) & 0xFF8u, i, k;
    if (!to_rdram && mem == 0x0FC0u) s_task = 0x80000000u | dram;   /* physical: as KSEG0 */
    for (i = 0; i < count; i++, dram += n + skip)
        for (k = 0; k < n; k += 4) {
            uint32_t* w = reg(0x04000000u + ((mem + k) & 0x1FFCu));
            uint32_t* r = (uint32_t*)(xrc_rdram_host + ((dram + k) % xrc_rdram_size));
            if (to_rdram) *r = *w;
            else *w = *r;
        }
    R(0x04040000u) = (R(0x04040000u) & 0x1000u) | ((mem + count * n) & 0xFFFu);
}

/* SP_STATUS written: the bits as __osSpSetStatus's; clearing halt runs the task */
static void sp_status(uint32_t w) {
    uint32_t st = R(0x04040010u);
    int i;
    for (i = 0; i < 8; i++) {
        if (w & (1u << (9 + 2 * i))) st &= ~(0x80u << i);
        if (w & (1u << (10 + 2 * i))) st |= 0x80u << i;
    }
    if (w & 0x4u) st &= ~SPS_BROKE;
    if (w & 0x2u) st |= SPS_HALT;
    R(0x04040010u) = st;
    if (!(w & 0x1u)) return;
    if (!s_task) {
        if (s_logged++ < 8) xhw_log("[MMIO] SP started with no task block in DMEM: ignored");
        return;
    }
    {
        uint32_t task = 0x80000000u | s_task, type = rd32(task + TK_TYPE);
        if (type == M_GFXTASK) {
            xrc_gfx_task(task);
            xu_post_event_first(OS_EVENT_DP);
            xu_post_event_first(OS_EVENT_SP);
        } else {
            xrc_audio_task(task);
            xu_post_event_first(OS_EVENT_SP);
        }
    }
    R(0x04040010u) = st | SPS_HALT | SPS_BROKE | SPS_SIG2;
}

uint32_t xrc_mmio_read(uint32_t vaddr) {
    uint32_t a = vaddr & 0x1FFFFFFCu, v;
    if (a >= 0x05000000u && a < 0x10000000u) return xu_pi_save_io(a, &v, 0) ? v : 0;
    if (a >= 0x10000000u && a < 0x1FC00000u) return xu_pi_word(a);
    switch (a) {
    case 0x0404001Cu:   /* SP_SEMAPHORE: read it, own it */
        v = R(a), R(a) = 1;
        return v;
    case 0x04400010u: return R(a) = vi_line();
    case 0x04500004u: return R(a) = call(osAiGetLength_recomp, 0, 0);
    case 0x0450000Cu: return R(a) = call(osAiGetLength_recomp, 0, 0) ? 0x40000000u : 0;   /* AI_STATUS: busy */
    }
    return R(a);
}

void xrc_mmio_write(uint32_t vaddr, uint32_t v) {
    uint32_t a = vaddr & 0x1FFFFFFCu;
    if (a >= 0x05000000u && a < 0x10000000u) {
        xu_pi_save_io(a, &v, 1);
        return;
    }
    if (a >= 0x10000000u && a < 0x1FC00000u) return;   /* the ROM */
    switch (a) {
    case 0x04040008u: sp_dma(v, 0); return;             /* SP_RD_LEN: RDRAM -> SP */
    case 0x0404000Cu: sp_dma(v, 1); return;             /* SP_WR_LEN */
    case 0x04040010u: sp_status(v); return;
    case 0x0404001Cu: R(a) = 0; return;
    case 0x04300000u: return;                           /* MI_MODE */
    case 0x04300008u: return;                           /* MI_INTR: read only */
    case 0x0430000Cu: {                                 /* MI_INTR_MASK: clear/set pairs */
        uint32_t m = R(a);
        int i;
        for (i = 0; i < 6; i++) {
            if (v & (1u << (2 * i))) m &= ~(1u << i);
            if (v & (1u << (2 * i + 1))) m |= 1u << i;
        }
        R(a) = m;
        return;
    }
    case 0x04400004u: {                                 /* VI_ORIGIN: the framebuffer, less its first line */
        uint32_t w = R(0x04400008u) & 0xFFFu, bpp = (R(0x04400000u) & 3u) == 3u ? 4u : 2u;
        R(a) = v;
        v &= 0x00FFFFFFu;
        if (w && v >= w * bpp) v -= w * bpp;
        call(osViSwapBuffer_recomp, 0x80000000u | v, 0);
        return;
    }
    case 0x04400010u: return;                           /* VI_CURRENT: clears the VI interrupt */
    case 0x04500004u:                                   /* AI_LEN: the buffer at AI_DRAM_ADDR */
        call(osAiSetNextBuffer_recomp, 0x80000000u | (R(0x04500000u) & 0x00FFFFF8u), v & 0x3FFF8u);
        return;
    case 0x0450000Cu: return;                           /* AI_STATUS: clears the AI interrupt */
    case 0x04500010u: {                                 /* AI_DACRATE: the divider less one */
        uint32_t tv = rd32(0x80000300u), clock = tv == 0 ? 49656530u : tv == 2 ? 48628316u : 48681812u;
        R(a) = v;
        call(osAiSetFrequency_recomp, clock / ((v & 0x3FFFu) + 1), 0);
        return;
    }
    case 0x04600008u: case 0x0460000Cu: {               /* PI_RD_LEN (RDRAM -> cart), PI_WR_LEN */
        uint32_t dram = 0x80000000u | (R(0x04600000u) & 0x00FFFFFEu), dev = R(0x04600004u), n = (v & 0x00FFFFFFu) + 1;   /* physical: as KSEG0, never through the TLB */
        R(a) = v;
        if (a == 0x04600008u) xu_pi_save_dma(dev, dram, n, 1);
        else if (!xu_pi_save_dma(dev, dram, n, 0)) xu_pi_dma(dev, dram, n);
        xu_post_event(OS_EVENT_PI);
        return;
    }
    case 0x04600010u: return;                           /* PI_STATUS: reset, clear interrupt */
    case 0x04800004u: case 0x04800010u:                 /* SI_PIF_ADDR_RD64B, _WR64B: the PIF's 64 bytes */
        call(__osSiRawStartDma_recomp, a == 0x04800010u, 0x80000000u | (R(0x04800000u) & 0x00FFFFFCu));
        return;
    case 0x04800018u: return;                           /* SI_STATUS: clears the SI interrupt */
    }
    R(a) = v;
}
