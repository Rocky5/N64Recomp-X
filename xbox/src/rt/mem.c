/* mem.c - the RDRAM window (PLAN.md 3.2.1).
 *
 * One physically contiguous, cached allocation of the pack's RDRAM size
 * (xhw_rdram_alloc, Wings64-X's): the kernel maps physical P at host address
 * 0x80000000 + P, so with rdram = P the generated code's
 * `rdram + (uint32_t)vaddr` lands on RDRAM byte (vaddr - 0x80000000) for every
 * KSEG0 address with no subtraction and no mask. KSEG1 and physical
 * addresses the game hands the runtime (DMA, task pointers) are masked with
 * 0x1FFFFFFF here, as the N64 did. A CPU access the game makes through KSEG1
 * lands at host P + 0xA0000000 + x: the window's frames are aliased there
 * too (xhw_alias.c), so uncached RDRAM is the same bytes with no extra
 * instruction (-DXRC_KSEG1=0: not aliased, such an access faults and the
 * crash guard names it; -DXRC_MEM_MASK=1 masks every access instead); the
 * RCP's registers past it are mmio.c's, set up here too. The window is not
 * lazily committed: the game touches all of it.
 *
 * The boot block (osTvType, osRomBase, osMemSize ...) and the boot segment
 * copy come with the ROM in M1. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "rt.h"
#include "xhw.h"

#ifndef XRC_KSEG1
#define XRC_KSEG1 1
#endif

uint8_t* xrc_rdram;
uint8_t* xrc_rdram_host;
uint32_t xrc_rdram_size;

/* The RCP's registers and the RSP's memories through KSEG1 (0xA4000000 +):
 * a CPU access the game makes there itself (code the runtime does not
 * replace: Pokemon Snap reads SP_IMEM and SP_DMEM at boot, a PI_STATUS
 * poll) lands at rdram + 0xA4xxxxxx, kernel space nothing maps. One shadow
 * page per register block is mapped there (xhw_alias_map, the TLB's
 * mechanism), past the end of the window's own allocation: reads see what
 * was written, or zero (PI and SP idle), and the RSP's memories hold what
 * the boot code leaves (IMEM 6103 and DMEM all ones at offset 0, which a
 * game's copy-protection check reads). Kill switch: -DXRC_HW_SHADOW=0 (the
 * access faults and the crash guard names it, as before). */
#ifndef XRC_HW_SHADOW
#define XRC_HW_SHADOW 1
#endif
/* the register model (mmio.c) maps the same pages, kept current; the
 * shadow pages stand in only when it is off (-DXRC_MMIO=0) */
#ifndef XRC_MMIO
#define XRC_MMIO 1
#endif
#define HW_SHADOW (XRC_HW_SHADOW && !XRC_MMIO)
static const uint32_t k_hw_blocks[] = {0x04000000u, 0x04001000u, 0x04040000u, 0x04100000u, 0x04300000u,
                                       0x04400000u, 0x04500000u, 0x04600000u, 0x04700000u, 0x04800000u};
#define HW_SHADOW_BYTES (sizeof k_hw_blocks / sizeof k_hw_blocks[0] * 4096u)

static void hw_shadow(uint8_t* p, uint32_t bytes) {
    uint32_t i, mapped = 0, phys = ((uint32_t)(uintptr_t)p & 0x7FFFFFFFu) + bytes;
    uint32_t* page = (uint32_t*)(p + bytes);
    page[0] = 0xFFFFFFFFu;    /* SP_DMEM[0] */
    page[1024] = 6103;        /* SP_IMEM[0] */
    for (i = 0; i < sizeof k_hw_blocks / sizeof k_hw_blocks[0]; i++)
        mapped += xhw_alias_map((uint32_t)(uintptr_t)(xrc_rdram + 0xA0000000u + k_hw_blocks[i]), phys + i * 4096u, 4096u)
                      ? 1 : 0;
    xhw_logf("[RECOMP] RCP registers through KSEG1: %u of %u blocks shadowed", (unsigned)mapped,
             (unsigned)(sizeof k_hw_blocks / sizeof k_hw_blocks[0]));
}

int xrc_mem_init(uint32_t mb) {
    uint32_t bytes = mb << 20;
#ifdef XRC_RDRAM_PAD_MB   /* diagnosis: RDRAM placed this many MB lower in physical memory */
    uint8_t* p = (uint8_t*)xhw_rdram_alloc(bytes + ((uint32_t)(XRC_RDRAM_PAD_MB) << 20) + (HW_SHADOW ? HW_SHADOW_BYTES : 0));
    /* (the allocator works down from the top: RDRAM at the bottom of the larger block sits lower) */
#else
    uint8_t* p = (uint8_t*)xhw_rdram_alloc(bytes + (HW_SHADOW ? HW_SHADOW_BYTES : 0));
#endif
    if (!p) return 0;
    if (((uintptr_t)p & 0x80000000u) == 0) {   /* not the kernel's 0x80000000 + physical mapping */
        xhw_logf("[MEM] rdram at %08x: not in the physical window", (unsigned)(uintptr_t)p);
        return 0;
    }
    xrc_rdram_host = p;
    xrc_rdram_size = bytes;
    xrc_rdram = p - 0x80000000u;
#ifdef XRC_RDRAM_USER
    /* a game that maps KUSEG high up with its own TLB (GoldenEye's 0x7F000000):
     * rdram + vaddr would land on the kernel's map of physical memory, which
     * cannot be aliased; the window's frames are aliased at a user address
     * instead (n64rx picks it from the TLB ranges the console logged), so
     * KSEG0 is there and the high KUSEG pages below it, in user space too.
     * DIVERGES from the default layout: 4 KB pages for RDRAM, not the kernel's
     * large ones (more x86 TLB misses). Not aliased there: the default. */
    if (xhw_alias_map((uint32_t)(XRC_RDRAM_USER), (uint32_t)(uintptr_t)p & 0x7FFFFFFFu, bytes))
        xrc_rdram = (uint8_t*)(uintptr_t)(uint32_t)(XRC_RDRAM_USER) - 0x80000000u;
    else
        xhw_logf("[MEM] rdram not aliased at %08x (in use): the kernel's window", (unsigned)(XRC_RDRAM_USER));
#endif
    xhw_logf("[RECOMP] window: %u MB, physical %08x, rdram base %08x", (unsigned)mb,
             (unsigned)((uintptr_t)p & 0x7FFFFFFFu), (unsigned)(uintptr_t)xrc_rdram);
    if (HW_SHADOW) hw_shadow(p, bytes);
#ifdef XRC_MEMBENCH   /* diagnosis: RDRAM's read speed before anything else runs */
    {
        uint32_t off, j, sum = 0;
        for (off = 0; off < bytes; off += off >= 0x200000 && off < 0x300000 ? 0x10000 : 0x100000) {
            volatile uint32_t* q = (volatile uint32_t*)(p + off);
            uint64_t t0 = xhw_time_us();
            for (j = 0; j < 0x10000 / 4; j++) sum += q[j];
            xhw_logf("[MEMB] boot phys %06x (%08x): 64 KB in %u us", (unsigned)off, (unsigned)(((uintptr_t)p & 0x7FFFFFFFu) + off),
                     (unsigned)(xhw_time_us() - t0));
        }
        {   /* and memory elsewhere: a static block, a heap block */
            static uint32_t s_blk[0x10000 / 4];
            uint32_t xhw_phys_of(const void* p);
            uint32_t* h = (uint32_t*)malloc(0x10000);
            volatile uint32_t* qs[2] = {s_blk, h};
            unsigned k;
            for (k = 0; k < 2; k++) {
                volatile uint32_t* q = qs[k];
                uint64_t t0 = xhw_time_us();
                if (!q) continue;
                for (j = 0; j < 0x10000 / 4; j++) sum += q[j];
                xhw_logf("[MEMB] boot %s at %p (physical %08x): 64 KB in %u us", k ? "heap" : "static", (void*)q,
                         (unsigned)xhw_phys_of((const void*)q), (unsigned)(xhw_time_us() - t0));
            }
            free(h);
        }
        (void)sum;
    }
#endif
#if XRC_KSEG1
    if (!xhw_alias_map((uint32_t)(uintptr_t)(xrc_rdram + 0xA0000000u), (uint32_t)(uintptr_t)p & 0x7FFFFFFFu, bytes)) {
        /* some page of the range is the kernel's (the console, 8 MB windows at
         * 0xA3631000; not xemu): every free page aliased on its own, the
         * first refused one logged with what holds it. Majora's Mask's first
         * uncached write landed on an unaliased page and crashed (vmm3).
         * DIVERGES: an access to a page left out still faults */
        uint32_t base = (uint32_t)(uintptr_t)(xrc_rdram + 0xA0000000u), pa = (uint32_t)(uintptr_t)p & 0x7FFFFFFFu;
        uint32_t off, refused = 0, first = 0, pde = 0, pte = 0;
        for (off = 0; off < bytes; off += 4096)
            if (!xhw_alias_map(base + off, pa + off, 4096)) {
                if (!refused++) first = base + off, xhw_alias_why(first, &pde, &pte);
            }
        if (refused)
            xhw_logf("[MEM] KSEG1 alias: %u of %u pages refused, the first %08x (PDE %08x, PTE %08x): uncached "
                     "accesses there fault", (unsigned)refused, (unsigned)(bytes / 4096), (unsigned)first,
                     (unsigned)pde, (unsigned)pte);
        else
            xhw_logf("[MEM] KSEG1 aliased page by page at %08x", (unsigned)base);
    }
#endif
    xrc_mmio_init();
#ifdef XRC_WATCH
    xhw_watch_write((uint32_t)(uintptr_t)(xrc_rdram + (uint32_t)(XRC_WATCH)));   /* diagnosis: xhw_crash.c */
#endif
    return 1;
}

/* A fault on the KSEG1 alias (the crash handler's fixup, tlb.c). Majora's
 * Mask on the console (vmm9, 2026-10-18): the 8 MB alias at 0xA3631000 was
 * made whole at boot (no refusal logged), yet its first uncached write at
 * a38af402 faulted a few seconds later: the kernel had used and released
 * pages of that range (vmm3 found one in use at boot). A page whose PTE is
 * gone is aliased again and the access retried; a page the kernel holds
 * now is logged with its PDE/PTE and stays a crash. DIVERGES: none (the
 * same bytes). Kill switch: -DXRC_KSEG1_HEAL=0 (such a fault is a crash). */
#ifndef XRC_KSEG1_HEAL
#define XRC_KSEG1_HEAL 1
#endif
int xrc_kseg1_heal(uint32_t host) {
#if XRC_KSEG1 && XRC_KSEG1_HEAL
    static unsigned s_healed, s_refused;
    uint32_t base = (uint32_t)(uintptr_t)(xrc_rdram + 0xA0000000u), off = host - base, page, pde, pte;
    if (!xrc_rdram_host || off >= xrc_rdram_size) return 0;
    page = base + (off & ~0xFFFu);
    xhw_alias_why(page, &pde, &pte);
    if (xhw_alias_map(page, ((uint32_t)(uintptr_t)xrc_rdram_host & 0x7FFFFFFFu) + (off & ~0xFFFu), 4096)) {
        if (s_healed++ < 16)
            xhw_logf("[MEM] KSEG1 page %08x lost (PDE %08x, PTE %08x): aliased again (%u)", (unsigned)page,
                     (unsigned)pde, (unsigned)pte, s_healed);
        return 1;
    }
    if (s_refused++ < 4)
        xhw_logf("[MEM] KSEG1 page %08x taken (PDE %08x, PTE %08x): the access faults", (unsigned)page,
                 (unsigned)pde, (unsigned)pte);
#else
    (void)host;
#endif
    return 0;
}

void* xrc_host(uint32_t vram, uint32_t bytes) {
    uint32_t phys = vram & 0x1FFFFFFFu;
    if ((int32_t)vram >= 0) {   /* KUSEG: through the game's TLB (Turok 2's code and data) */
        uint32_t p = xrc_tlb_to_phys(vram);
        if (p != ~0u) phys = p;
    }
    if (!xrc_rdram_host || phys >= xrc_rdram_size || bytes > xrc_rdram_size - phys) return NULL;
    return xrc_rdram_host + phys;
}
