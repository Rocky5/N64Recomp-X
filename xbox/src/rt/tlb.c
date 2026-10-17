/* tlb.c - the N64's TLB, as far as games use it (docs/decisions.md "The
 * TLB"; PLAN.md 3.2.1).
 *
 * Games map a virtual range (KUSEG: SM64's goddard maps its dynlist data at
 * 0x04000000 with 64 KB pages onto a buffer in RDRAM) and then read it
 * through the generated code's ordinary loads: rdram + vaddr. That host
 * address is P + vaddr (rdram is the window's physical base P), user space,
 * so each mapped page is aliased there
 * onto the Xbox frames that hold the N64 page (xhw_alias.c); the hot path
 * stays one addressing mode. The 32 entries are kept here as the game wrote
 * them (tlbr and tlbp read them back; osVirtualToPhysical and __osProbeTLB
 * translate through them).
 *
 * Entered from the interpreter's tlbwi/tlbwr/tlbr/tlbp (the TLB routines the
 * recompiler refuses run there) and from the runtime's osMapTLB family when
 * the analyser named them (ultra/more2.c).
 *
 * DIVERGES: the ASID is not compared (every entry matches as if global; a
 * game switching address spaces by ASID would see the last mapping); an
 * entry the host cannot alias (its host range is in use by the XBE or the
 * heap, or it is KSEG2/3, whose host address is the kernel's) is kept for
 * translation but logged, and CPU accesses through
 * it fault with the crash guard's card; code linked at TLB addresses is not
 * in the function table (ovl.c).
 * Kill switch: -DXRC_TLB=0 (the entries are kept, nothing is aliased: the
 * old no-op TLB). */
#include <stdint.h>

#include "rt.h"
#include "xhw.h"

#ifndef XRC_TLB
#define XRC_TLB 1
#endif

#define NENT 32
#define LO_V 0x2u
#define LO_G 0x1u

typedef struct {
    uint32_t mask, hi, lo[2];
    uint32_t host[2], bytes[2];   /* what is aliased now (bytes 0: nothing) */
} Ent;

static Ent s_ent[NENT];
static unsigned s_logged;

/* one page's size: PageMask 0 = 4 KB, 0x1E000 = 64 KB ... 0x1FFE000 = 16 MB */
static uint32_t page_bytes(uint32_t mask) { return ((mask | 0x1FFFu) + 1) >> 1; }
static uint32_t vpn2(const Ent* e) { return e->hi & ~(e->mask | 0x1FFFu); }
static uint32_t lo_phys(uint32_t lo) { return ((lo >> 6) & 0xFFFFFu) << 12; }

static void unalias(Ent* e) {
    int k;
    for (k = 0; k < 2; k++)
        if (e->bytes[k]) {
#if XRC_TLB
            xhw_alias_unmap(e->host[k], e->bytes[k]);
#endif
            e->bytes[k] = 0;
        }
}

static void alias(Ent* e, unsigned idx) {
    uint32_t sz = page_bytes(e->mask), base = vpn2(e);
    int k;
    for (k = 0; k < 2; k++) {
        uint32_t va = base + (uint32_t)k * sz, pa = lo_phys(e->lo[k]);
        uint32_t host = (uint32_t)(uintptr_t)(xrc_rdram + va);
        uint32_t xpa = ((uint32_t)(uintptr_t)xrc_rdram_host & 0x7FFFFFFFu) + pa;
        int ok;
        if (!(e->lo[k] & LO_V)) continue;
        if (va >= 0x80000000u && va < 0xC0000000u) continue;   /* KSEG0/1 never go through the TLB */
        if (va >= 0xC0000000u) host = 0;                       /* KSEG2/3: the kernel's system space there */
#if XRC_TLB
        ok = pa < xrc_rdram_size && sz <= xrc_rdram_size - pa && xhw_alias_map(host, xpa, sz);
#else
        ok = 0, (void)host, (void)xpa;
#endif
        if (ok) e->host[k] = host, e->bytes[k] = sz;
        if (s_logged < 24) {
            s_logged++;
            xhw_logf("[TLB] %u: %08x-%08x -> %08x%s", idx, (unsigned)va, (unsigned)(va + sz - 1), (unsigned)pa,
                     ok ? "" : XRC_TLB ? " (not aliased: the host range is in use or outside RDRAM)"
                                       : " (not aliased: -DXRC_TLB=0)");
        }
    }
}

static int s_any_valid;

void xrc_tlb_write(unsigned idx, uint32_t mask, uint32_t hi, uint32_t lo0, uint32_t lo1) {
    Ent* e = &s_ent[idx & (NENT - 1)];
    unalias(e);
    e->mask = mask & 0x01FFE000u;
    e->hi = hi;
    e->lo[0] = lo0;
    e->lo[1] = lo1;
    alias(e, idx & (NENT - 1));
    if ((lo0 | lo1) & LO_V) s_any_valid = 1;   /* xrc_tlb_to_phys looks only once anything was mapped */
}

void xrc_tlb_read(unsigned idx, uint32_t* mask, uint32_t* hi, uint32_t* lo0, uint32_t* lo1) {
    const Ent* e = &s_ent[idx & (NENT - 1)];
    *mask = e->mask, *hi = e->hi, *lo0 = e->lo[0], *lo1 = e->lo[1];
}

/* tlbp: the index of the entry that maps EntryHi's VPN2, or bit 31 set */
uint32_t xrc_tlb_probe(uint32_t hi) {
    unsigned i;
    for (i = 0; i < NENT; i++) {
        const Ent* e = &s_ent[i];
        if ((e->lo[0] | e->lo[1]) & LO_V && vpn2(e) == (hi & ~(e->mask | 0x1FFFu))) return i;
    }
    return 0x80000000u;
}

/* vaddr through the entries: its physical address, or ~0 when unmapped */
uint32_t xrc_tlb_to_phys(uint32_t vaddr) {
    unsigned i;
    if (!s_any_valid) return ~0u;   /* most games: the runtime's helpers pay nothing */
    for (i = 0; i < NENT; i++) {
        const Ent* e = &s_ent[i];
        uint32_t sz = page_bytes(e->mask), base = vpn2(e);
        if (vaddr - base < 2 * sz) {
            uint32_t lo = e->lo[(vaddr - base) >= sz];
            if (lo & LO_V) return lo_phys(lo) + ((vaddr - base) & (sz - 1));
        }
    }
    return ~0u;
}

/* ---- a miss: the N64's TLB refill and TLB invalid exceptions ----
 * Code and data a game pages in on demand (GoldenEye's 0x7F000000 from the
 * ROM; Conker jumps to 0x15000000 before anything maps it) fault on the
 * Xbox, unmapped at rdram + vaddr. The crash handler asks here first
 * (xhw_crash_set_fixup): the game's refill vector at 0x80000000 runs
 * (interp.c), and if the entry is still not valid, libultra's fault path
 * (ultra/thread.c: the thread stops faulted, OS_EVENT_FAULT goes to the
 * game's fault thread, which loads the page and makes it runnable again);
 * then the page is aliased and the instruction runs again.
 * DIVERGES: EPC is not given (the fault thread does not read it); a page
 * whose host range cannot be aliased (KUSEG addresses that land on the
 * kernel's map, GoldenEye's) stays a crash.
 * Kill switch: -DXRC_TLB_MISS=0 (a miss is a crash, as before). */
#ifndef XRC_TLB_MISS
#define XRC_TLB_MISS 1
#endif
int xrc_interp_refill(uint32_t vaddr, int store);   /* interp.c */
int xu_fault_wait(uint32_t vaddr, int store);       /* ultra/thread.c */

/* the entry that maps vaddr, valid, and aliased on the host: 1 */
static int aliased(uint32_t vaddr, int* valid) {
    unsigned i;
    *valid = 0;
    for (i = 0; i < NENT; i++) {
        const Ent* e = &s_ent[i];
        uint32_t sz = page_bytes(e->mask), base = vpn2(e);
        if (vaddr - base < 2 * sz) {
            int k = (vaddr - base) >= sz;
            if (!(e->lo[k] & LO_V)) continue;
            *valid = 1;
            if (e->bytes[k]) return 1;
        }
    }
    return 0;
}

int xrc_tlb_miss(uint32_t vaddr, int store) {
    static unsigned s_n;
    int i, valid, how = 0;
    for (i = 0; i < 8; i++) {
        if (aliased(vaddr, &valid)) break;
        if (valid) {   /* mapped, but the host range is taken */
            if (s_n < 32) s_n++, xhw_logf("[TLB] miss %08x: mapped but not aliased (the host range is in use)",
                                          (unsigned)vaddr);
            return 0;
        }
        if (xrc_interp_refill(vaddr, store)) {
            how |= 1;
            if (aliased(vaddr, &valid)) break;
        }
        if (!xu_fault_wait(vaddr, store)) {
            if (s_n < 32) s_n++, xhw_logf("[TLB] miss %08x (%s): the game's exception code did not map it and "
                                          "no fault thread took it", (unsigned)vaddr, store ? "store" : "load");
            return 0;
        }
        how |= 2;
    }
    if (i == 8) return 0;
    if (s_n < 32) s_n++, xhw_logf("[TLB] miss %08x (%s): mapped by %s", (unsigned)vaddr, store ? "store" : "load",
                                  how == 3 ? "the fault thread and the game's exception code" : how == 2 ? "the fault thread"
                                  : how == 1 ? "the game's exception code" : "another thread");
    return 1;
}

static int fixup(uint32_t host, int write) {
    uint32_t v = host - (uint32_t)(uintptr_t)xrc_rdram;
    if (v >= 0x80000000u) return 0;   /* KSEG0/1 are always there: a real crash */
    return xrc_tlb_miss(v, write);
}

void xrc_tlb_init(void) {
    if (XRC_TLB && XRC_TLB_MISS) xhw_crash_set_fixup(fixup);
}
