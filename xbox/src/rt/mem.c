/* mem.c - the RDRAM window (PLAN.md 3.2.1).
 *
 * One physically contiguous, cached allocation of the pack's RDRAM size
 * (xhw_rdram_alloc, Wings64-X's): the kernel maps physical P at host address
 * 0x80000000 + P, so with rdram = P the generated code's
 * `rdram + (uint32_t)vaddr` lands on RDRAM byte (vaddr - 0x80000000) for every
 * KSEG0 address with no subtraction and no mask. KSEG1 and physical
 * addresses the game hands the runtime (DMA, task pointers) are masked with
 * 0x1FFFFFFF here, as the N64 did; a CPU access the game makes through KSEG1
 * faults and the crash guard names it (the pack's patches fix the site,
 * Zelda64Recomp's way; -DXRC_MEM_MASK=1 masks every access instead). The
 * window is not lazily committed: the game touches all of it.
 *
 * The boot block (osTvType, osRomBase, osMemSize ...) and the boot segment
 * copy come with the ROM in M1. */
#include <stdint.h>
#include <string.h>

#include "rt.h"
#include "xhw.h"

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
    uint8_t* p = (uint8_t*)xhw_rdram_alloc(bytes + (XRC_HW_SHADOW ? HW_SHADOW_BYTES : 0));
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
    if (XRC_HW_SHADOW) hw_shadow(p, bytes);
    return 1;
}

void* xrc_host(uint32_t vram, uint32_t bytes) {
    uint32_t phys = vram & 0x1FFFFFFFu;
    if (!xrc_rdram_host || phys >= xrc_rdram_size || bytes > xrc_rdram_size - phys) return NULL;
    return xrc_rdram_host + phys;
}
