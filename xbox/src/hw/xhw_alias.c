/* xhw_alias.c - physical pages mapped a second time at a chosen address:
 * the N64 TLB's host side (xbox/src/rt/tlb.c, docs/decisions.md "The TLB").
 *
 * The generated code addresses N64 memory as rdram + vaddr with rdram = P,
 * the window's physical base (mem.c: KSEG0 0x80000000 + x lands on host
 * 0x80000000 + P + x), so a TLB-mapped vaddr V (KUSEG, below 0x80000000)
 * lands at host address P + V: user space. When osMapTLB maps V onto N64
 * physical X, the page tables here map that host page onto Xbox physical
 * P + X, the same frame the window shows at rdram + 0x80000000 + X: the
 * game's loads and stores through V then hit the right bytes with no extra
 * instruction anywhere (the hot path does not change).
 *
 * The Xbox runs everything in ring 0 with one address space and the x86
 * page tables self-mapped (non-PAE: the PTEs at 0xC0000000, the page
 * directory at 0xC0300000), so this writes them directly:
 *   - below 0x80000000 the whole 4 MB regions the range touches are first
 *     reserved from the kernel (NtAllocateVirtualMemory, MEM_RESERVE, never
 *     released), so it never commits anything of its own there and never
 *     shares a page table with us; a region already in use refuses the
 *     range;
 *   - a 4 MB region with no page table gets one (a contiguous page, zeroed;
 *     never freed: 4 KB per 4 MB region the game ever maps);
 *   - in kernel space, a region the kernel maps with a 4 MB page is split
 *     into 4 KB PTEs with the same frames and flags first, and a page
 *     already mapped is only taken when its frame is at or above 64 MB (a
 *     128 MB console's upper half, which xhw_ramlock.c holds and nothing
 *     reads through this window); its PTE is restored on unmap.
 *   Any other mapped page (the kernel, the window over the low 64 MB, the
 *   page tables) refuses the whole range: the caller logs it and the access
 *   faults where it would have, with the crash guard's card.
 * Interrupts are off while the tables change; each page is invlpg'd.
 * The same mapping serves KSEG1 (the window again at P + 0xA0000000, mem.c)
 * and the register pages of the runtime's RCP model (xhw_alias_page, mmio.c).
 * Kill switches: the runtime's -DXRC_TLB=0, -DXRC_KSEG1=0 and -DXRC_MMIO=0
 * each keep their caller from coming here. */
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>
#include <string.h>

#include "xhw.h"

#define PTE_BASE 0xC0000000u
#define PDE_BASE 0xC0300000u
#define PTE_P 0x001u
#define PTE_RW 0x002u
#define PTE_A 0x020u
#define PTE_D 0x040u
#define PDE_PS 0x080u
#define PTE_OURS (PTE_P | PTE_RW | PTE_A | PTE_D)
#define LOW_TOP (64u << 20)
#define MAX_SAVED 2048u   /* taken upper-half pages (8 MB worth) */

static volatile uint32_t* pde_of(uint32_t va) { return (volatile uint32_t*)(PDE_BASE + (va >> 22) * 4); }
static volatile uint32_t* pte_of(uint32_t va) { return (volatile uint32_t*)(PTE_BASE + (va >> 12) * 4); }
static void invlpg(uint32_t va) { __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory"); }
static uint32_t irq_off(void) {
    uint32_t f;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(f) : : "memory");
    return f;
}
static void irq_restore(uint32_t f) { __asm__ volatile("pushl %0; popfl" : : "r"(f) : "memory", "cc"); }

static struct { uint32_t va, pte; } s_saved[MAX_SAVED];
static unsigned s_nsaved;
static uint32_t s_reserved[512 / 32];   /* user-space 4 MB regions held (bit per region) */

/* a range that may be aliased: not the first 4 MB (the XBE's), not the page
 * tables, no wrap */
static int range_ok(uint32_t va, uint32_t bytes) {
    if ((va | bytes) & 0xFFFu || !bytes || va < 0x400000u || va + bytes < va) return 0;
    if (va < PTE_BASE + 0x400000u && va + bytes > PTE_BASE) return 0;
    if (va < 0x80000000u && va + bytes > 0x80000000u) return 0;
    return 1;
}

/* user space: hold every 4 MB region the range touches; 0 if one is in use */
static int reserve(uint32_t va, uint32_t bytes) {
    uint32_t r, last = (va + bytes - 1) >> 22;
    if (va >= 0x80000000u) return 1;
    for (r = va >> 22; r <= last; r++) {
        PVOID base = (PVOID)(uintptr_t)(r << 22);
        SIZE_T size = 0x400000u;
        if (s_reserved[r >> 5] & (1u << (r & 31))) continue;
        if (!NT_SUCCESS(NtAllocateVirtualMemory(&base, 0, &size, MEM_RESERVE, PAGE_READWRITE))) return 0;
        s_reserved[r >> 5] |= 1u << (r & 31);
    }
    return 1;
}

static uint32_t s_zero_frame;   /* xhw_alias_zero_page's frame: a mapping of it may be replaced */

/* the page may be taken: unmapped, our page of zeros, or (kernel space) an upper-half frame */
static int page_free(uint32_t va) {
    uint32_t pde = *pde_of(va), pte;
    if (!(pde & PTE_P)) return 1;
    if (pde & PDE_PS) return va >= 0x80000000u && (pde & 0xFFC00000u) >= LOW_TOP;
    pte = *pte_of(va);
    return !(pte & PTE_P) || (s_zero_frame && (pte & 0xFFFFF000u) == s_zero_frame) ||
           (va >= 0x80000000u && (pte & 0xFFFFF000u) >= LOW_TOP);
}

/* Kernel space past the physical map (0xA0000000 up: the KSEG1 alias and the
 * RCP's register pages) is aliased only through page tables of our own. A
 * table already there is the kernel's, and its page was found holding other
 * things on the console: Majora's Mask's 8 MB window at physical 0x03631000
 * held the table of 0xA3800000 (at 0x03A30000), so the game's own stores
 * became its PTEs (values like 80762388) and an uncached store faulted on a
 * page the kernel "held" (vmm14); Mario Tennis's table there read 0 a run of
 * pages at a time, and healing them looped with the boot frozen (vmt2). Such
 * a table is adopted: a copy made in a page of ours, the PDE pointed at it.
 * -DXHW_ALIAS_OWN_TABLES=0: the table there is used, as before */
#ifndef XHW_ALIAS_OWN_TABLES
#define XHW_ALIAS_OWN_TABLES 1
#endif
static uint32_t s_own_pt[1024];   /* per 4 MB region: the physical page of our table there, 0 none */
/* An adopted table starts empty: past the physical map nothing of the
 * kernel's lives there (xemu has no table there at all); on red the only
 * tables found were ours, left by an earlier XBE, whose page had been
 * reused, so their entries are whatever that page holds now (copied, they
 * were taken as pages in use, or as aliases onto random frames).
 * -DXHW_ALIAS_ADOPT_COPY=1: copied */
#ifndef XHW_ALIAS_ADOPT_COPY
#define XHW_ALIAS_ADOPT_COPY 0
#endif
static unsigned s_adopt_had;   /* present entries an adopted table held (not copied) */
static uint32_t s_old_pde[1024];  /* what the PDE held before ours (0: nothing) */
static unsigned s_nadopt;
static int owned(uint32_t va) {
    uint32_t pt = s_own_pt[va >> 22], pde = *pde_of(va);
    return pt && (pde & PTE_P) && !(pde & PDE_PS) && (pde & 0xFFFFF000u) == pt;
}

/* a page table for va's 4 MB region; splits a large page (and, with `adopt`,
 * copies a table of the kernel's into one of ours). 0 if none could be had.
 * The page comes from the kernel with interrupts on; only the PDE write and
 * the flush run with them off. */
static int table_for_ex(uint32_t va, int adopt) {
    volatile uint32_t* pde = pde_of(va);
    uint32_t old = *pde, *pt, i, f;
    adopt = adopt && XHW_ALIAS_OWN_TABLES && !owned(va);
    if ((old & PTE_P) && !(old & PDE_PS) && !adopt) return 1;
    pt = (uint32_t*)MmAllocateContiguousMemoryEx(4096, 0, 0xFFFFFFFFu, 4096, PAGE_READWRITE);
    if (!pt) return 0;
    if ((old & PTE_P) && (old & PDE_PS))   /* split: the same frames, the low flags (G stays bit 8) */
        for (i = 0; i < 1024; i++) pt[i] = ((old & 0xFFC00000u) + (i << 12)) | (old & 0x17Fu & ~PDE_PS);
    else if ((old & PTE_P) && XHW_ALIAS_ADOPT_COPY)   /* adopt: the entries there as they are */
        for (i = 0; i < 1024; i++) pt[i] = *pte_of((va & 0xFFC00000u) + (i << 12));
    else if (old & PTE_P) {   /* adopt empty: count what was there, for the log */
        s_adopt_had = 0;
        for (i = 0; i < 1024; i++) s_adopt_had += (*pte_of((va & 0xFFC00000u) + (i << 12)) & PTE_P) != 0;
        memset(pt, 0, 4096);
    } else
        memset(pt, 0, 4096);
    f = irq_off();
    if (*pde != old) {   /* changed meanwhile: keep the one there */
        irq_restore(f);
        MmFreeContiguousMemory(pt);
        return (*pde & PTE_P) && !(*pde & PDE_PS);
    }
    *pde = ((uint32_t)MmGetPhysicalAddress(pt) & 0xFFFFF000u) | PTE_P | PTE_RW | PTE_A;
    for (i = 0; i < 1024; i++) invlpg((va & 0xFFC00000u) + (i << 12));
    s_own_pt[va >> 22] = (uint32_t)MmGetPhysicalAddress(pt) & 0xFFFFF000u;
    s_old_pde[va >> 22] = old;
    irq_restore(f);
    if ((old & PTE_P) && !(old & PDE_PS) && s_nadopt++ < 8)
        xhw_logf("[MEM] alias: a page table found at %08x (page %08x, %u entries present) replaced by one of ours "
                 "(%08x)%s", (unsigned)(va & 0xFFC00000u), (unsigned)(old & 0xFFFFF000u), s_adopt_had,
                 (unsigned)((uint32_t)MmGetPhysicalAddress(pt) & 0xFFFFF000u), XHW_ALIAS_ADOPT_COPY ? ", copied" : "");
    return 1;
}
static int table_for(uint32_t va) { return table_for_ex(va, 0); }

/* Before the XBE is left (xhw_main.c): the page tables made where there was
 * none are taken out of the page directory. Page directory entries outlive
 * a quick reboot on the console, the tables' pages do not: the next XBE
 * (another game here, the dashboard) got the page under a table left behind
 * (Majora's Mask after Mario Tennis: its RDRAM window held Mario Tennis's
 * table at 0x03A30000). A region that had an entry before ours (a split
 * large page, an adopted table) gets that entry back as it was. */
void xhw_alias_shutdown(void) {
    uint32_t r, i, f = irq_off();
    for (r = 0; r < 1024; r++) {
        uint32_t va = r << 22;
        if (!s_own_pt[r] || !owned(va)) continue;
        /* a split large page gets its entry back; an adopted table (an earlier
         * XBE's, its page long reused) is not handed on to the next one */
        *pde_of(va) = (s_old_pde[r] & PTE_P) && ((s_old_pde[r] & PDE_PS) || XHW_ALIAS_ADOPT_COPY) ? s_old_pde[r] : 0;
        for (i = 0; i < 1024; i++) invlpg(va + (i << 12));
        s_own_pt[r] = 0;
    }
    irq_restore(f);
}

/* hold a user-space range for later aliasing, before anything else can be
 * placed there (the learnt KUSEG span of a game that maps its code through
 * the TLB: rt/boot.c). 1 if every 4 MB region of it is held */
int xhw_alias_hold(uint32_t va, uint32_t bytes) {
    va &= ~0x3FFFFFu;
    bytes = (bytes + 0x3FFFFFu) & ~0x3FFFFFu;
    return range_ok(va, bytes) && reserve(va, bytes);
}

static int map_flags(uint32_t va, uint32_t pa, uint32_t bytes, uint32_t flags);
int xhw_alias_map(uint32_t va, uint32_t pa, uint32_t bytes) { return map_flags(va, pa, bytes, PTE_OURS); }
/* read only (the ROM through KSEG1, xhw_rom_cart_map): never unmapped, a store there faults */
int xhw_alias_map_ro(uint32_t va, uint32_t pa, uint32_t bytes) { return map_flags(va, pa, bytes, PTE_OURS & ~PTE_RW); }

static int map_flags(uint32_t va, uint32_t pa, uint32_t bytes, uint32_t flags) {
    uint32_t off, f;
    if (!range_ok(va, bytes) || (pa & 0xFFFu) || !reserve(va, bytes)) return 0;
    /* the tables first: an adopted one is what page_free then reads */
    for (off = 0; off < bytes; off = (((va + off) & ~0x3FFFFFu) + 0x400000u) - va)
        if (!table_for_ex(va + off, va + off >= 0xA0000000u && va + off < PTE_BASE)) return 0;   /* a table made stays */
    for (off = 0; off < bytes; off += 4096)
        if (!page_free(va + off)) return 0;
    f = irq_off();
    for (off = 0; off < bytes; off += 4096) {
        uint32_t v = va + off;
        volatile uint32_t* pte = pte_of(v);
        if ((*pte & PTE_P) && (*pte & 0xFFFFF000u) >= LOW_TOP) {
            if (s_nsaved >= MAX_SAVED) {
                irq_restore(f);
                xhw_alias_unmap(va, off);
                return 0;
            }
            s_saved[s_nsaved].va = v, s_saved[s_nsaved].pte = *pte, s_nsaved++;
        }
        *pte = (pa + off) | flags;
        invlpg(v);
    }
    irq_restore(f);
    return 1;
}

/* what holds va: its PDE and (no large page) its PTE, for the log when a
 * range is refused (mem.c's KSEG1 alias on the console, 2026-10-17) */
void xhw_alias_why(uint32_t va, uint32_t* pde, uint32_t* pte) {
    *pde = *pde_of(va);
    *pte = (*pde & PTE_P) && !(*pde & PDE_PS) ? *pte_of(va) : 0;
}

void xhw_alias_unmap(uint32_t va, uint32_t bytes) {
    uint32_t off, f, i;
    if (!range_ok(va, bytes)) return;
    f = irq_off();
    for (off = 0; off < bytes; off += 4096) {
        uint32_t v = va + off, pde = *pde_of(v);
        volatile uint32_t* pte;
        if (!(pde & PTE_P) || (pde & PDE_PS)) continue;
        pte = pte_of(v);
        if ((*pte & 0xFFFu) != PTE_OURS || (*pte & 0xFFFFF000u) >= LOW_TOP) continue;   /* not ours */
        *pte = 0;
        for (i = 0; i < s_nsaved; i++)
            if (s_saved[i].va == v) {
                *pte = s_saved[i].pte;
                s_saved[i] = s_saved[--s_nsaved];
                break;
            }
        invlpg(v);
    }
    irq_restore(f);
}

/* ---- page watch (framebuffer readback, docs/renderer.md "Framebuffer
 * effects"): the x86 sets a PTE's accessed bit when it loads the page into
 * its TLB for any read or write. Arming clears the bits of the whole pages
 * inside the range (splitting a large page first, as above) and flushes
 * them from the TLB; the next look counts the pages touched since. No page
 * is ever made not present: a not-present page of this window that the
 * game touched was a bugcheck 0xA in the kernel's fault path before any
 * exception handler ran (xemu, the real kernel). ---- */
#define WINDOW 0x80000000u

int xhw_watch_pages(uint32_t va, uint32_t bytes, int arm) {
    uint32_t v = (va + 0xFFFu) & ~0xFFFu, end = (va + bytes) & ~0xFFFu, f;
    int n = 0;
    if (va < WINDOW || va + bytes < va || end > WINDOW + LOW_TOP || v >= end) return 0;
    if (arm) {
        uint32_t r;
        for (r = v; r < end; r = (r & ~0x3FFFFFu) + 0x400000u)
            if (!table_for(r)) return 0;
    }
    f = irq_off();
    for (; v < end; v += 4096) {
        uint32_t pde = *pde_of(v);
        volatile uint32_t* pte;
        if (!(pde & PTE_P)) continue;
        if (pde & PDE_PS) {   /* not split: no telling */
            n += !arm;
            continue;
        }
        pte = pte_of(v);
        if (!(*pte & PTE_P)) continue;
        if (arm) {
            *pte &= ~PTE_A;
            invlpg(v);
            n++;
        } else {
            n += (*pte & PTE_A) != 0;
        }
    }
    irq_restore(f);
    return n;
}

void* xhw_alias_page(uint32_t va) {
    void* p = MmAllocateContiguousMemoryEx(4096, 0, 0xFFFFFFFFu, 4096, PAGE_READWRITE);
    if (!p) return NULL;
    memset(p, 0, 4096);
    if (!xhw_alias_map(va, (uint32_t)MmGetPhysicalAddress(p) & 0xFFFFF000u, 4096)) {
        MmFreeContiguousMemory(p);
        return NULL;
    }
    return p;
}

int xhw_alias_zero_page(uint32_t va) {
    void* p;
    if (!s_zero_frame) {
        p = MmAllocateContiguousMemoryEx(4096, 0, 0xFFFFFFFFu, 4096, PAGE_READWRITE);
        if (!p) return 0;
        memset(p, 0, 4096);
        s_zero_frame = (uint32_t)MmGetPhysicalAddress(p) & 0xFFFFF000u;
    }
    return xhw_alias_map_ro(va, s_zero_frame, 4096);
}

/* diagnosis: va's page directory entry and, under a page table, its PTE (0 there for a large page) */
void xhw_page_info(uint32_t va, uint32_t* pde, uint32_t* pte) {
    *pde = *pde_of(va);
    *pte = (*pde & PTE_P) && !(*pde & PDE_PS) ? *pte_of(va) : 0;
}

/* diagnosis: the physical address behind a host pointer */
uint32_t xhw_phys_of(const void* p) { return (uint32_t)MmGetPhysicalAddress((PVOID)p); }
