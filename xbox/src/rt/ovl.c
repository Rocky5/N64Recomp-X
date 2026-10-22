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
/* the page map, two levels over the whole 32-bit space: a 1 MB group's 256
 * page pointers, made on first use (KSEG0's code, and code a game maps with
 * its own TLB anywhere: Turok 2 at 0x00200000, Conker at 0x10000000,
 * GoldenEye at 0x70000000) */
#define GROUP_PAGES 256u
static uint16_t** s_groups[4096];
static uint16_t** page_ptr(uint32_t vram, int make) {
    uint16_t*** g = &s_groups[vram >> 20];
    if (!*g) {
        if (!make) return NULL;
        *g = (uint16_t**)calloc(GROUP_PAGES, sizeof(uint16_t*));
        if (!*g) return NULL;
    }
    return &(*g)[(vram >> PAGE_SHIFT) & (GROUP_PAGES - 1)];
}
#define SLOT(vram) (((vram) >> 2) & (PAGE_WORDS - 1))
/* learn.c's stubs, entered at run time: room for every address the net
 * keeps (XRC_MAX_LEARN); 512 sent the 513th and later down the slow path,
 * a scan and a log line per call (docs/review-2026-10.md 5a.3) */
#ifndef XRC_MAX_LEARN
#define XRC_MAX_LEARN 4096
#endif
#define LEARN_SLOTS XRC_MAX_LEARN
#ifndef XRC_OVERLAYS
#define XRC_OVERLAYS 1
#endif

static recomp_func_t** s_funcs;   /* index -> function; [0] unused */
static uint32_t s_nfuncs, s_cap;

static int32_t s_section_addr_store[1];
int32_t* section_addresses = s_section_addr_store;

uint32_t xrc_ovl_function_count(void) { return s_nfuncs; }

static uint32_t* s_sec_base;   /* section -> the table index of its first function */
static uint8_t* s_ovl;         /* section shares its vram with another */
static uint8_t* s_active;      /* overlay: the one the table maps now */
static uint8_t* s_gone;        /* a section another ROM range was loaded over (xrc_ovl_dma) */
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
    uint16_t** pg = (vram & 3) ? NULL : page_ptr(vram, 1);
    if (!pg) return 0;
    if (s_nfuncs >= s_cap) return 0;
    if (!*pg) {
        *pg = (uint16_t*)calloc(PAGE_WORDS, sizeof(uint16_t));
        if (!*pg) return 0;
    }
    s_funcs[s_nfuncs + 1] = f;
    __asm__ volatile("" ::: "memory");   /* the pointer before the index: get_function runs on other threads */
    (*pg)[SLOT(vram)] = (uint16_t)++s_nfuncs;
    return 1;
}

/* learn.c: a stub for an address the net took (under its lock) */
int xrc_ovl_enter(uint32_t vram, recomp_func_t* f);   /* its table index, 0 when there is no room */
int xrc_ovl_enter(uint32_t vram, recomp_func_t* f) { return enter(vram, f) ? (int)s_nfuncs : 0; }

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
    {   /* indexed by the recompiler's section index, which has gaps (a section
         * with neither functions nor relocations gets no table entry but keeps
         * its number): sized by the largest index, not the entry count, which
         * wrote past the block (docs/review-2026-10.md 5a.1) */
        size_t n = 1;
        for (i = 0; i < xrc_game.num_sections; i++)
            if ((size_t)xrc_game.sections[i].index + 1 > n) n = (size_t)xrc_game.sections[i].index + 1;
        section_addresses = (int32_t*)calloc(n, sizeof(int32_t));
    }
    if (!s_funcs || !section_addresses) return 0;
    s_sec_base = (uint32_t*)calloc(xrc_game.num_sections + 1, sizeof *s_sec_base);
    s_ovl = (uint8_t*)calloc(xrc_game.num_sections + 1, 3);
    if (!s_sec_base || !s_ovl) return 0;
    s_active = s_ovl + xrc_game.num_sections + 1;
    s_gone = s_active + xrc_game.num_sections + 1;
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

void xu_log_regs(void);   /* ultra/thread.c */

recomp_func_t* xrc_try_function(uint32_t vram) {
    uint16_t** pg = page_ptr(vram, 0);
    if (!pg || !*pg) return NULL;
    {
        uint16_t i = (*pg)[SLOT(vram)];
        return i ? s_funcs[i] : NULL;
    }
}

recomp_func_t* get_function(int32_t vram) {
    uint16_t** pg = page_ptr((uint32_t)vram, 0);
    if (pg && *pg) {
        uint16_t i = (*pg)[SLOT((uint32_t)vram)];
        if (i) return s_funcs[i];
    }
    {
        recomp_func_t* f = xrc_learn_lookup((uint32_t)vram, (uintptr_t)__builtin_return_address(0));
        if (f) return f;
    }
    xu_log_regs();
    xrc_fatal_vram("No function at this address", (uint32_t)vram, (uint32_t)(uintptr_t)__builtin_return_address(0));
}

/* ---- overlays (PLAN.md 3.4): the one the last PI DMA loaded is mapped ---- */
static void set_index(uint32_t vram, uint16_t idx) {
    uint16_t** pg = page_ptr(vram, 0);
    if (pg && *pg)
        (*pg)[SLOT(vram)] = idx;
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

/* A section alone at its address is an overlay too when the game loads
 * other ROM there (Castlevania: Legacy of Darkness DMAs each actor's code to
 * 0x801CAEA0; one actor's was learnt and recompiled, and ran for the others:
 * a call through NULL). A DMA from another ROM range over a section's
 * functions unmaps them there (the net runs what RAM holds); the section's
 * own DMA maps them again. Captured code (past the ROM) has the guard
 * instead. -DXRC_OVL_FOREIGN=0: as before */
#ifndef XRC_OVL_FOREIGN
#define XRC_OVL_FOREIGN 1
#endif
uint32_t xhw_rom_size(void);
static void foreign(uint32_t rom, uint32_t ram, uint32_t size) {
    size_t i;
    uint32_t j;
    static unsigned s_logged;
    for (i = 0; i < xrc_game.num_sections; i++) {
        const SectionTableEntry* s = &xrc_game.sections[i];
        int own;
        if (s_ovl[i] || !s->size || !s->num_funcs || s->rom_addr >= xhw_rom_size()) continue;
        if (ram >= s->ram_addr + s->size || s->ram_addr >= ram + size) continue;
        own = rom < s->rom_addr + s->size && s->rom_addr < rom + size && ram - rom == s->ram_addr - s->rom_addr;
        if (own) {
            if (s_gone[i]) {
                for (j = 0; j < s->num_funcs; j++) set_index(s->ram_addr + s->funcs[j].offset, (uint16_t)(s_sec_base[i] + j));
                s_gone[i] = 0;
            }
            continue;
        }
        for (j = 0; j < s->num_funcs; j++) {
            uint32_t v = s->ram_addr + s->funcs[j].offset;
            if (v - ram < size) set_index(v, 0);
        }
        if (!s_gone[i] && s_logged++ < 16)
            xhw_logf("[OVL] %08x: ROM %x loaded over the code from ROM %x: its functions there unmapped", (unsigned)ram,
                     (unsigned)rom, (unsigned)s->rom_addr);
        s_gone[i] = 1;
    }
}

/* ---- overlays the game relocates itself (n64rx analyze/zovl.py) ----
 * The Zelda engine links each overlay at an address of its own above the
 * RAM and loads it where its allocator has room; n64rx recompiles each one
 * as a relocatable section at its link address (its own references read
 * section_addresses through RELOC_HI16/LO16) and hooks the game's
 * relocation routine (Overlay_Relocate(ram, header, link vram) in the
 * decomps): here, before the game patches the bytes, the section is mapped
 * where it landed and its references pointed there. What the table mapped
 * in that range before (another overlay that lived there, the safety net's
 * stubs) is cleared first, and the section's last place is unmapped.
 * -DXRC_RELOC_OVL=0: nothing mapped (the safety net runs the overlay). */
#ifndef XRC_RELOC_OVL
#define XRC_RELOC_OVL 1
#endif
static uint32_t* s_reloc_at;   /* section -> where it is mapped now (0: nowhere) */
void xrc_learn_reloc(uint32_t vram, uint32_t ram, uint32_t len);   /* learn.c */
static int put_index(uint32_t vram, uint16_t idx) {   /* set_index, making the page */
    uint16_t** pg = page_ptr(vram, 1);
    if (!pg) return 0;
    if (!*pg && !(*pg = (uint16_t*)calloc(PAGE_WORDS, sizeof(uint16_t)))) return 0;
    (*pg)[SLOT(vram)] = idx;
    return 1;
}
/* learn.c: a stub an overlay switch took off the table, back at its index */
void xrc_ovl_reenter(uint32_t vram, int idx);
void xrc_ovl_reenter(uint32_t vram, int idx) {
    if (idx > 0 && (uint32_t)idx <= s_nfuncs) put_index(vram, (uint16_t)idx);
}
/* section k's functions off the table at the place it was mapped (those
 * entries that are still its own) */
static void reloc_unmap(size_t k) {
    const SectionTableEntry* o = &xrc_game.sections[k];
    uint32_t j;
    if (!s_reloc_at[k]) return;
    for (j = 0; j < o->num_funcs; j++) {
        uint32_t at = s_reloc_at[k] + o->funcs[j].offset;
        uint16_t** pg = page_ptr(at, 0);
        if (pg && *pg && (*pg)[SLOT(at)] == (uint16_t)(s_sec_base[k] + j)) (*pg)[SLOT(at)] = 0;
    }
    s_reloc_at[k] = 0;
}

void xrc_ovl_relocate(uint32_t ram, uint32_t hdr, uint32_t vram) {
    static unsigned s_logged, s_unknown;
    size_t i, k;
    uint32_t j, v, len;
    const SectionTableEntry* s = NULL;
    if (!XRC_RELOC_OVL || !s_funcs) return;
    ram = (ram & 0x1FFFFFFFu) | 0x80000000u;
    if (!s_reloc_at && !(s_reloc_at = (uint32_t*)calloc(xrc_game.num_sections, sizeof *s_reloc_at))) return;
    for (i = 0; i < xrc_game.num_sections; i++)
        if (xrc_game.sections[i].num_relocs && xrc_game.sections[i].ram_addr == vram) break;
    if (i < xrc_game.num_sections) s = &xrc_game.sections[i];
    /* the code and data it occupies, from its header in RDRAM (text, data,
     * rodata sizes): whatever the table mapped there (an overlay that lived
     * there before, all of it; the safety net's stubs) is not this code */
    hdr = (hdr & 0x1FFFFFFFu) | 0x80000000u;
    len = xrc_word(hdr) + xrc_word(hdr + 4) + xrc_word(hdr + 8);
    if (s && s->size > len) len = s->size;
    if (len > 0x200000u) len = s ? s->size : 0;
    xrc_learn_reloc(vram, ram, len);   /* for the next build: the overlays a run used (learn.c) */
    for (k = 0; k < xrc_game.num_sections; k++)
        if (s_reloc_at[k] && s_reloc_at[k] < ram + len && ram < s_reloc_at[k] + xrc_game.sections[k].size) reloc_unmap(k);
    for (v = ram; v < ram + len; v += 4) set_index(v, 0);
    if (!s) {
        if (s_unknown++ < 8)
            xhw_logf("[OVL] relocated to %08x: no section linked at %08x (the safety net runs it)", (unsigned)ram, (unsigned)vram);
        return;
    }
    reloc_unmap(i);   /* its last place */
    section_addresses[s->index] = (int32_t)ram;
    for (j = 0; j < s->num_funcs; j++) put_index(ram + s->funcs[j].offset, (uint16_t)(s_sec_base[i] + j));
    s_reloc_at[i] = ram;
    if (s_logged++ < 24)
        xhw_logf("[OVL] %08x: the overlay linked at %08x relocated here (%u functions)", (unsigned)ram, (unsigned)vram,
                 (unsigned)s->num_funcs);
}

/* a link address inside a relocatable overlay -> where the game relocated
 * it now (the interpreter's entry for an overlay routine n64rx left to it:
 * its stub names the link address); other addresses as they are */
uint32_t xrc_ovl_reloc_vram(uint32_t vram) {
    size_t i;
    if (!XRC_RELOC_OVL || !s_reloc_at || vram < 0x80800000u) return vram;
    for (i = 0; i < xrc_game.num_sections; i++) {
        const SectionTableEntry* s = &xrc_game.sections[i];
        if (s->num_relocs && vram - s->ram_addr < s->size)
            return s_reloc_at[i] ? s_reloc_at[i] + (vram - s->ram_addr) : vram;
    }
    return vram;
}

void xrc_ovl_dma(uint32_t rom, uint32_t ram, uint32_t size);
void xrc_ovl_dma(uint32_t rom, uint32_t ram, uint32_t size) {
    size_t i;
    if (!size) return;
    ram = (ram & 0x1FFFFFFFu) | 0x80000000u;
    if (XRC_OVL_FOREIGN && s_gone) foreign(rom, ram, size);
    if (!XRC_OVERLAYS || !s_novl) return;
    for (i = 0; i < xrc_game.num_sections; i++) {
        const SectionTableEntry* s = &xrc_game.sections[i];
        if (!s_ovl[i] || s_active[i]) continue;
        /* from inside its ROM range, to where that ROM sits when it is loaded */
        if (rom < s->rom_addr + s->size && s->rom_addr < rom + size && ram - rom == s->ram_addr - s->rom_addr)
            activate(i);
        else if (XHW_TEST_BUILD && rom < s->rom_addr + s->size && s->rom_addr < rom + size) {
            static unsigned s_other;   /* diagnosis: its ROM taken somewhere else (a buffer, a copy) */
            if (s_other++ < 16)
                xhw_logf("[OVL] ROM %x (%x bytes) to %08x: inside the overlay at %08x (ROM %x), but not to its place",
                         (unsigned)rom, (unsigned)size, (unsigned)ram, (unsigned)s->ram_addr, (unsigned)s->rom_addr);
        }
    }
}
