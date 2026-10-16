/* ovl.c - function lookup by N64 address (PLAN.md 3.4).
 *
 * get_function(vram) is a two-level table over KSEG0 at 4-byte granularity:
 * a page map (one entry per 4 KB of the window: 2,048 for 8 MB) of pointers
 * to per-page arrays of 1,024 function indices (uint16_t; index 0 is "none",
 * so up to 65,535 functions), allocated only for pages that hold a function
 * start. Two loads and a bounds check. The input is the recompiler's section
 * table (recomp_overlays.inl through xrc_game); every section is entered at
 * its link address here.
 *
 * Overlays (sections that share vram: a game's levels loaded one at a time
 * at the same address) are tracked by their PI DMA source: pi.c reports
 * every DMA (xrc_ovl_dma), and a DMA from inside an overlay's ROM range to
 * its own RAM position makes that overlay the one the table maps: its range
 * is cleared, the interpreted routines there entered, then its functions
 * (each section keeps the indices it got at init, so a switch allocates
 * nothing). Until the first such DMA the section entered last wins, as
 * before. Kill switch: -DXRC_OVERLAYS=0 (no switching).
 *
 * A function pointer that misses the table goes to the safety net (learn.c:
 * the code there is interpreted and logged for the next build) when the
 * address holds code; otherwise, or with -DXRC_SAFETY_NET=0, it is a fatal
 * card with the address and the caller, never a silent jump. */
#include <stdlib.h>
#include <string.h>

#include "rt.h"
#include "xhw.h"

#define PAGE_SHIFT 12
#define PAGE_WORDS (1u << (PAGE_SHIFT - 2))
/* the page map: KSEG0's 8 MB, then the first 16 MB of KUSEG, where a game's
 * own TLB maps code (Turok 2 and 3, Armorines: 0x00200000 +) */
#define KSEG0_PAGES ((8u << 20) >> PAGE_SHIFT)
#define KUSEG_PAGES ((16u << 20) >> PAGE_SHIFT)
#define MAX_PAGES (KSEG0_PAGES + KUSEG_PAGES)
static inline uint32_t page_of(uint32_t vram) {   /* MAX_PAGES: outside the map */
    uint32_t p;
    if (vram >= 0x80000000u) {
        p = (vram - 0x80000000u) >> PAGE_SHIFT;
        return p < KSEG0_PAGES ? p : MAX_PAGES;
    }
    p = vram >> PAGE_SHIFT;
    return p < KUSEG_PAGES ? KSEG0_PAGES + p : MAX_PAGES;
}
#define SLOT(vram) (((vram) >> 2) & (PAGE_WORDS - 1))
#define LEARN_SLOTS 512   /* learn.c's stubs, entered at run time */
#ifndef XRC_OVERLAYS
#define XRC_OVERLAYS 1
#endif

static uint16_t* s_pages[MAX_PAGES];
static recomp_func_t** s_funcs;   /* index -> function; [0] unused */
static uint32_t s_nfuncs, s_cap;

static int32_t s_section_addr_store[1];
int32_t* section_addresses = s_section_addr_store;

uint32_t xrc_ovl_function_count(void) { return s_nfuncs; }

static uint32_t* s_sec_base;   /* section -> the table index of its first function */
static uint8_t* s_ovl;         /* section shares its vram with another */
static uint8_t* s_active;      /* overlay: the one the table maps now */
static uint32_t s_novl, s_interp_base;

/* the routines n64rx hands to the interpreter (interp.c, PLAN.md 3.14):
 * gen/xrc_interp.c defines these when there are any */
typedef struct { uint32_t vram; recomp_func_t* fn; } XrcInterpEntry;
__attribute__((weak)) const XrcInterpEntry xrc_interp_table[1];
__attribute__((weak)) const unsigned xrc_interp_count;
/* the runtime's libultra at the game's addresses (gen/xrc_interp.c): only
 * interpreted code calls them by address (the recompiled code by name) */
typedef struct { uint32_t vram; recomp_func_t* fn; } XrcUltraEntry;
__attribute__((weak)) const XrcUltraEntry xrc_ultra_table[1];
__attribute__((weak)) const unsigned xrc_ultra_count;

static int enter(uint32_t vram, recomp_func_t* f) {
    uint32_t page = page_of(vram);
    if (page >= MAX_PAGES || (vram & 3)) return 0;
    if (s_nfuncs >= s_cap) return 0;
    if (!s_pages[page]) {
        s_pages[page] = (uint16_t*)calloc(PAGE_WORDS, sizeof(uint16_t));
        if (!s_pages[page]) return 0;
    }
    s_funcs[s_nfuncs + 1] = f;
    __asm__ volatile("" ::: "memory");   /* the pointer before the index: get_function runs on other threads */
    s_pages[page][SLOT(vram)] = (uint16_t)++s_nfuncs;
    return 1;
}

/* learn.c: a stub for an address the net took (under its lock) */
int xrc_ovl_enter(uint32_t vram, recomp_func_t* f);
int xrc_ovl_enter(uint32_t vram, recomp_func_t* f) { return enter(vram, f); }

/* the recompiled function whose x86 holds `host` (the nearest start at or
 * below it; under ThinLTO an estimate), 0 when none: the caller the learn
 * log names */
uint32_t xrc_ovl_vram_of(uintptr_t host);
uint32_t xrc_ovl_vram_of(uintptr_t host) {
    uintptr_t best = 0;
    uint32_t vram = 0;
    size_t i, j;
    for (i = 0; i < xrc_game.num_sections; i++)
        for (j = 0; j < xrc_game.sections[i].num_funcs; j++) {
            uintptr_t f = (uintptr_t)xrc_game.sections[i].funcs[j].func;
            if (f <= host && f > best) best = f, vram = xrc_game.sections[i].ram_addr + xrc_game.sections[i].funcs[j].offset;
        }
    return vram;
}

int xrc_ovl_init(void) {
    size_t i, j, total = 0;
    for (i = 0; i < xrc_game.num_sections; i++) total += xrc_game.sections[i].num_funcs;
    total += xrc_interp_count + xrc_ultra_count;
    if (total + LEARN_SLOTS > 65535) {
        xhw_logf("[RECOMP] %u functions: more than the table holds", (unsigned)total);
        return 0;
    }
    s_cap = (uint32_t)total + LEARN_SLOTS;
    s_funcs = (recomp_func_t**)calloc(s_cap + 1, sizeof *s_funcs);
    if (xrc_game.num_sections > 1)
        section_addresses = (int32_t*)calloc(xrc_game.num_sections, sizeof(int32_t));
    if (!s_funcs || !section_addresses) return 0;
    s_sec_base = (uint32_t*)calloc(xrc_game.num_sections + 1, sizeof *s_sec_base);
    s_ovl = (uint8_t*)calloc(xrc_game.num_sections + 1, 2);
    if (!s_sec_base || !s_ovl) return 0;
    s_active = s_ovl + xrc_game.num_sections + 1;
    for (i = 0; i < xrc_ultra_count; i++)   /* first: a recompiled function at the same address wins */
        enter(xrc_ultra_table[i].vram, xrc_ultra_table[i].fn);
    for (i = 0; i < xrc_game.num_sections; i++) {
        const SectionTableEntry* s = &xrc_game.sections[i];
        section_addresses[s->index] = (int32_t)s->ram_addr;
        s_sec_base[i] = s_nfuncs + 1;
        for (j = 0; j < xrc_game.num_sections; j++) {
            const SectionTableEntry* o = &xrc_game.sections[j];
            if (j != i && s->size && o->size && s->ram_addr < o->ram_addr + o->size && o->ram_addr < s->ram_addr + s->size)
                s_ovl[i] = 1;
        }
        s_novl += s_ovl[i];
        for (j = 0; j < s->num_funcs; j++)
            if (!enter(s->ram_addr + s->funcs[j].offset, s->funcs[j].func)) {
                xhw_logf("[RECOMP] function at %08x outside the table", (unsigned)(s->ram_addr + s->funcs[j].offset));
                return 0;
            }
    }
    s_interp_base = s_nfuncs + 1;
    for (i = 0; i < xrc_interp_count; i++)
        if (!enter(xrc_interp_table[i].vram, xrc_interp_table[i].fn)) return 0;
    xhw_logf("[RECOMP] %u functions in %u sections%s", (unsigned)s_nfuncs, (unsigned)xrc_game.num_sections,
             xrc_interp_count ? " (some interpreted: hand-written code)" : "");
    if (s_novl)
        xhw_logf("[OVL] %u sections share their vram with another: mapped by the PI DMA that loads them%s",
                 (unsigned)s_novl, XRC_OVERLAYS ? "" : " (off: -DXRC_OVERLAYS=0)");
    if (xrc_interp_count) xhw_logf("[RECOMP] %u routines run interpreted", (unsigned)xrc_interp_count);
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

recomp_func_t* xrc_try_function(uint32_t vram) {
    uint32_t page = page_of(vram);
    if (page >= MAX_PAGES || !s_pages[page]) return NULL;
    {
        uint16_t i = s_pages[page][SLOT(vram)];
        return i ? s_funcs[i] : NULL;
    }
}

recomp_func_t* get_function(int32_t vram) {
    uint32_t page = page_of((uint32_t)vram);
    if (page < MAX_PAGES && s_pages[page]) {
        uint16_t i = s_pages[page][SLOT((uint32_t)vram)];
        if (i) return s_funcs[i];
    }
    {
        recomp_func_t* f = xrc_learn_lookup((uint32_t)vram, (uintptr_t)__builtin_return_address(0));
        if (f) return f;
    }
    xrc_fatal_vram("No function at this address", (uint32_t)vram, (uint32_t)(uintptr_t)__builtin_return_address(0));
}

/* ---- overlays (PLAN.md 3.4): the one the last PI DMA loaded is mapped ---- */
static void set_index(uint32_t vram, uint16_t idx) {
    uint32_t page = page_of(vram);
    if (page < MAX_PAGES && s_pages[page])
        s_pages[page][SLOT(vram)] = idx;
}

static void activate(size_t i) {
    const SectionTableEntry* s = &xrc_game.sections[i];
    uint32_t v, j;
    static unsigned s_logged;
    for (j = 0; j < xrc_game.num_sections; j++) {
        const SectionTableEntry* o = &xrc_game.sections[j];
        if (s_ovl[j] && s->ram_addr < o->ram_addr + o->size && o->ram_addr < s->ram_addr + s->size) s_active[j] = 0;
    }
    /* the overlay that was here before: its functions now miss (the safety
     * net runs what is in RAM, as the N64 would run the new bytes) */
    for (v = s->ram_addr & ~3u; v < s->ram_addr + s->size; v += 4) set_index(v, 0);
    for (j = 0; j < xrc_interp_count; j++)   /* interpreted routines run what is in RAM: any overlay's */
        if (xrc_interp_table[j].vram - s->ram_addr < s->size) set_index(xrc_interp_table[j].vram, (uint16_t)(s_interp_base + j));
    for (j = 0; j < s->num_funcs; j++) set_index(s->ram_addr + s->funcs[j].offset, (uint16_t)(s_sec_base[i] + j));
    s_active[i] = 1;
    if (s_logged++ < 64)
        xhw_logf("[OVL] %08x: ROM %x mapped (%u functions)", (unsigned)s->ram_addr, (unsigned)s->rom_addr,
                 (unsigned)s->num_funcs);
}

void xrc_ovl_dma(uint32_t rom, uint32_t ram, uint32_t size);
void xrc_ovl_dma(uint32_t rom, uint32_t ram, uint32_t size) {
    size_t i;
    if (!XRC_OVERLAYS || !s_novl || !size) return;
    ram = (ram & 0x1FFFFFFFu) | 0x80000000u;
    for (i = 0; i < xrc_game.num_sections; i++) {
        const SectionTableEntry* s = &xrc_game.sections[i];
        if (!s_ovl[i] || s_active[i]) continue;
        /* from inside its ROM range, to where that ROM sits when it is loaded */
        if (rom < s->rom_addr + s->size && s->rom_addr < rom + size && ram - rom == s->ram_addr - s->rom_addr)
            activate(i);
    }
}
