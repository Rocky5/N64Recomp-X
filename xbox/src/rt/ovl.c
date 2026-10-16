/* ovl.c - function lookup by N64 address (PLAN.md 3.4).
 *
 * get_function(vram) is a two-level table over KSEG0 at 4-byte granularity:
 * a page map (one entry per 4 KB of the window: 2,048 for 8 MB) of pointers
 * to per-page arrays of 1,024 function indices (uint16_t; index 0 is "none",
 * so up to 65,535 functions), allocated only for pages that hold a function
 * start. Two loads and a bounds check. The input is the recompiler's section
 * table (recomp_overlays.inl through xrc_game); every section is entered at
 * its link address here. Overlays tracked by their PI DMA source, and code
 * loaded on demand as XBE sections, come in M5.
 *
 * A function pointer that misses the table is a fatal card with the address
 * and the caller, never a silent jump. */
#include <stdlib.h>
#include <string.h>

#include "rt.h"
#include "xhw.h"

#define PAGE_SHIFT 12
#define PAGE_WORDS (1u << (PAGE_SHIFT - 2))
#define MAX_PAGES ((8u << 20) >> PAGE_SHIFT)

static uint16_t* s_pages[MAX_PAGES];
static recomp_func_t** s_funcs;   /* index -> function; [0] unused */
static uint32_t s_nfuncs;

static int32_t s_section_addr_store[1];
int32_t* section_addresses = s_section_addr_store;

uint32_t xrc_ovl_function_count(void) { return s_nfuncs; }

static int enter(uint32_t vram, recomp_func_t* f) {
    uint32_t off = vram - 0x80000000u, page = off >> PAGE_SHIFT;
    if (vram < 0x80000000u || page >= MAX_PAGES || (vram & 3)) return 0;
    if (!s_pages[page]) {
        s_pages[page] = (uint16_t*)calloc(PAGE_WORDS, sizeof(uint16_t));
        if (!s_pages[page]) return 0;
    }
    s_funcs[++s_nfuncs] = f;
    s_pages[page][(off >> 2) & (PAGE_WORDS - 1)] = (uint16_t)s_nfuncs;
    return 1;
}

int xrc_ovl_init(void) {
    size_t i, j, total = 0;
    for (i = 0; i < xrc_game.num_sections; i++) total += xrc_game.sections[i].num_funcs;
    if (total > 65535) {
        xhw_logf("[RECOMP] %u functions: more than the table holds", (unsigned)total);
        return 0;
    }
    s_funcs = (recomp_func_t**)calloc(total + 1, sizeof *s_funcs);
    if (xrc_game.num_sections > 1)
        section_addresses = (int32_t*)calloc(xrc_game.num_sections, sizeof(int32_t));
    if (!s_funcs || !section_addresses) return 0;
    for (i = 0; i < xrc_game.num_sections; i++) {
        const SectionTableEntry* s = &xrc_game.sections[i];
        section_addresses[s->index] = (int32_t)s->ram_addr;
        for (j = 0; j < s->num_funcs; j++)
            if (!enter(s->ram_addr + s->funcs[j].offset, s->funcs[j].func)) {
                xhw_logf("[RECOMP] function at %08x outside the table", (unsigned)(s->ram_addr + s->funcs[j].offset));
                return 0;
            }
    }
    xhw_logf("[RECOMP] %u functions in %u sections", (unsigned)s_nfuncs, (unsigned)xrc_game.num_sections);
    {   /* the code's size: the MIPS text the functions came from, and the span
         * their x86 occupies (the generated objects link together; an
         * estimate under ThinLTO, which may move a few). PLAN.md 3.11's
         * expansion factor. */
        uintptr_t lo = ~(uintptr_t)0, hi = 0;
        uint32_t mips = 0;
        for (i = 0; i < xrc_game.num_sections; i++)
            for (j = 0; j < xrc_game.sections[i].num_funcs; j++) {
                uintptr_t f = (uintptr_t)xrc_game.sections[i].funcs[j].func;
                mips += xrc_game.sections[i].funcs[j].rom_size;
                if (f < lo) lo = f;
                if (f > hi) hi = f;
            }
        if (mips && hi > lo)
            xhw_logf("[RECOMP] code: %u KB of MIPS -> about %u KB of x86 (x%u.%u)", (unsigned)(mips >> 10),
                     (unsigned)((hi - lo) >> 10), (unsigned)((hi - lo) / mips),
                     (unsigned)((hi - lo) * 10 / mips % 10));
    }
    return 1;
}

recomp_func_t* get_function(int32_t vram) {
    uint32_t off = (uint32_t)vram - 0x80000000u, page = off >> PAGE_SHIFT;
    if (page < MAX_PAGES && s_pages[page]) {
        uint16_t i = s_pages[page][(off >> 2) & (PAGE_WORDS - 1)];
        if (i) return s_funcs[i];
    }
    xrc_fatal_vram("No function at this address", (uint32_t)vram, (uint32_t)(uintptr_t)__builtin_return_address(0));
}
