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
#include <string.h>

#include "rt.h"
#include "xhw.h"

#ifndef XRC_KSEG1
#define XRC_KSEG1 1
#endif

uint8_t* xrc_rdram;
uint8_t* xrc_rdram_host;
uint32_t xrc_rdram_size;

int xrc_mem_init(uint32_t mb) {
    uint32_t bytes = mb << 20;
    uint8_t* p = (uint8_t*)xhw_rdram_alloc(bytes);
    if (!p) return 0;
    if (((uintptr_t)p & 0x80000000u) == 0) {   /* not the kernel's 0x80000000 + physical mapping */
        xhw_logf("[MEM] rdram at %08x: not in the physical window", (unsigned)(uintptr_t)p);
        return 0;
    }
    xrc_rdram_host = p;
    xrc_rdram_size = bytes;
    xrc_rdram = p - 0x80000000u;
    xhw_logf("[RECOMP] window: %u MB, physical %08x, rdram base %08x", (unsigned)mb,
             (unsigned)((uintptr_t)p & 0x7FFFFFFFu), (unsigned)(uintptr_t)xrc_rdram);
#if XRC_KSEG1
    if (!xhw_alias_map((uint32_t)(uintptr_t)(xrc_rdram + 0xA0000000u), (uint32_t)(uintptr_t)p & 0x7FFFFFFFu, bytes))
        xhw_logf("[MEM] KSEG1 not aliased (host %08x in use): uncached RDRAM accesses fault",
                 (unsigned)(uintptr_t)(xrc_rdram + 0xA0000000u));
#endif
    xrc_mmio_init();
    return 1;
}

void* xrc_host(uint32_t vram, uint32_t bytes) {
    uint32_t phys = vram & 0x1FFFFFFFu;
    if (!xrc_rdram_host || phys >= xrc_rdram_size || bytes > xrc_rdram_size - phys) return NULL;
    return xrc_rdram_host + phys;
}
