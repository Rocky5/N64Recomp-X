/* guard.c - recompiled code from RDRAM is only run while RDRAM still holds
 * it (docs/decisions.md "Captured code is checked before it runs").
 *
 * Code the learning loop captured from RDRAM (learn.c, n64rx's code_*
 * segments: Rare's decompressed overlays) was recompiled from the bytes one
 * run saw. Overlays share addresses: Banjo-Kazooie loads its levels'
 * code at 0x803863F0 one after another, and a level's recompiled code then
 * ran where another level's was loaded (the actor that crashed in
 * anSeq_update). n64rx wraps every recompiled function of a captured page
 * (gen/xrc_guard.c, pipeline.py write_guard): it runs while its state is
 * "verified"; otherwise xrc_guard_miss hashes its bytes in RDRAM against
 * the bytes it was recompiled from (the function's own: a page's would
 * change with data beside the code). A match verifies it; anything
 * else runs the function in the interpreter from RDRAM and saves the page
 * again for the next --learn build. A page is unverified again when the
 * game invalidates the instruction cache over it (osInvalICache: it loaded
 * code there) or a PI DMA writes it.
 * DIVERGES: code a game writes without invalidating the instruction cache
 * is not seen (the N64 would run stale cache lines too, mostly).
 * Kill switch: N64RX_GUARD=0 (n64rx: no wrappers, as before). */
#include <stdio.h>
#include <stdlib.h>

#include "rt.h"
#include "xhw.h"

/* gen/xrc_guard.c's tables, sorted by vram; a build without captured code has none */
__attribute__((weak)) const uint32_t xrc_guard_vram[1];
__attribute__((weak)) const uint32_t xrc_guard_size[1];
__attribute__((weak)) const uint32_t xrc_guard_hash[1];
__attribute__((weak)) const unsigned xrc_guard_count;
__attribute__((weak)) volatile unsigned char xrc_guard_state[1];
__attribute__((weak)) recomp_func_t* const xrc_guard_body[1];   /* where one vram has several versions */
__attribute__((weak)) const unsigned char xrc_guard_retrace_each[1];   /* a learnt DMA overlay's: checked each retrace */

#define PAGE 0x1000u   /* invalidations round to it */
enum { UNVERIFIED = 0, VERIFIED = 1, OTHER = 2 };

void xrc_interp(uint8_t* rdram, recomp_context* ctx, uint32_t entry);
void xrc_learn_recapture_n(uint32_t vram, uint32_t bytes);   /* learn.c: every page the function covers */

/* Code a game pages in through its TLB (Conker's 0x15000000, 32 entries over megabytes). A page that just left the TLB says
 * nothing about the code recompiled from it, yet the hash could not read it, the function counted as changed and ran in the
 * interpreter, whose every fetch then took misses of its own, until the page next changed: code stayed interpreted because it
 * had been paged out. Now the check takes the page as the CPU's fetch would, through the game's own miss code; an entry
 * rewritten only checks again what it maps, not what it stopped mapping (tlb.c); and the hash looks a page up once, not
 * once a word, over 32 entries.
 * DIVERGES: a verified function runs with its page out of the TLB, where the N64's fetch would fault it back in; other code loaded
 * at that address with no mapping in between goes unseen until something maps it. -DXRC_GUARD_PAGED=0: as before */
#ifndef XRC_GUARD_PAGED
#define XRC_GUARD_PAGED 1
#endif
uint32_t xrc_tlb_resolve(uint32_t vaddr, int store);

static int word(uint32_t v, uint32_t* w) {
    if ((int32_t)v >= 0) {   /* KUSEG: through the TLB */
        uint32_t p = xrc_tlb_to_phys(v);
        if (XRC_GUARD_PAGED && p == ~0u) p = xrc_tlb_resolve(v, 0);
        if (p == ~0u || p >= xrc_rdram_size) return 0;
        v = 0x80000000u | p;
    }
    *w = xrc_word(v);
    return 1;
}

/* FNV-1a over the function's words as the N64 reads them (pipeline.py's twin) */
static int fn_hash(uint32_t v, uint32_t n, uint32_t* out) {
    uint32_t h = 0x811C9DC5u, i, w, page = 1u, phys = 0;
    for (i = 0; i < n; i += 4) {
        uint32_t a = v + i;
        if (XRC_GUARD_PAGED && (int32_t)a >= 0) {
            if ((a & ~(PAGE - 1)) != page) {   /* (a miss for this page can evict the one before: never read back) */
                if (!word(a, &w)) return 0;
                page = a & ~(PAGE - 1);
                phys = xrc_tlb_to_phys(page);
                if (phys == ~0u) page = 1u;    /* (an entry that could not stay: word by word) */
            } else {
                w = xrc_word(0x80000000u | (phys + (a & (PAGE - 1))));
            }
        } else if (!word(a, &w)) {
            return 0;
        }
        h = (h ^ w) * 0x01000193u;
    }
    *out = h;
    return 1;
}

static int verify(unsigned i) {
    uint32_t h;
    if (xrc_guard_state[i] == UNVERIFIED)
        xrc_guard_state[i] = fn_hash(xrc_guard_vram[i], xrc_guard_size[i], &h) && h == xrc_guard_hash[i] ? VERIFIED
                                                                                                      : OTHER;
    return xrc_guard_state[i] == VERIFIED;
}

/* Versions: a game that loads several scenes' code at one address (Mario
 * Tennis at 0x800E8000..0x8010D000) was captured once per scene, and n64rx
 * recompiled each version as its own overlay section (learn.py
 * code_<vram>_v<n>); the table maps one of them at the address. When the
 * version called does not match RDRAM, the others at that vram are tried
 * (they sit beside it, sorted by vram) and the one that matches runs.
 * Kill switch: -DXRC_GUARD_VERSIONS=0 (the interpreter, as before). */
#ifndef XRC_GUARD_VERSIONS
#define XRC_GUARD_VERSIONS 1
#endif
static recomp_func_t* sibling(unsigned i) {
    unsigned j;
    uint32_t v = xrc_guard_vram[i];
    if (!XRC_GUARD_VERSIONS) return NULL;
    for (j = i; j > 0 && xrc_guard_vram[j - 1] == v; j--) {}
    for (; j < xrc_guard_count && xrc_guard_vram[j] == v; j++)
        if (j != i && xrc_guard_body[j] && verify(j)) return xrc_guard_body[j];
    return NULL;
}

/* Each retrace the versioned entries (one vram, several versions) are checked
 * again: a game that decompresses a scene's code with the CPU writes it with
 * no DMA the guard sees, and may invalidate the cache before it writes (the
 * N64's cache would hold the old lines a moment; the guard's state held them
 * for good). One hash each on their next call, a frame. Kill switch:
 * -DXRC_GUARD_RETRACE=0 (only invalidations and DMAs, as before). */
#ifndef XRC_GUARD_RETRACE
#define XRC_GUARD_RETRACE 1
#endif
static unsigned char s_multi_known, *s_multi;
void xrc_guard_retrace(void) {
    unsigned i;
    if (!XRC_GUARD_RETRACE || !xrc_guard_count) return;
    if (!s_multi_known) {   /* which entries share their vram, once (none: no versions) */
        s_multi_known = 1;
        int any;
        for (i = 0; i + 1 < xrc_guard_count && xrc_guard_vram[i] != xrc_guard_vram[i + 1]; i++) {}
        any = i + 1 < xrc_guard_count;
        for (i = 0; i < xrc_guard_count && !any; i++) any = xrc_guard_retrace_each[i];
        if (any && (s_multi = (unsigned char*)calloc(xrc_guard_count, 1)) != NULL) {
            for (i = 0; i + 1 < xrc_guard_count; i++)
                if (xrc_guard_vram[i] == xrc_guard_vram[i + 1]) s_multi[i] = s_multi[i + 1] = 1;
            /* a learnt DMA overlay's functions (n64rx's learn_* segments) too: the CPU may write
             * another overlay over them with no DMA (Star Fox 64 decompresses its scenes) */
            for (i = 0; i < xrc_guard_count; i++) s_multi[i] |= xrc_guard_retrace_each[i];
        }
    }
    if (!s_multi) return;
    for (i = 0; i < xrc_guard_count; i++)
        if (s_multi[i]) xrc_guard_state[i] = UNVERIFIED;
}

/* Whether a function recompiled at vram is what RDRAM holds now: a captured function's bytes against any of its
 * versions; code not captured always is. (interp.c: a jump through a register that lands where another version of an
 * overlay has a function is not a call to it) */
int xrc_guard_current(uint32_t vram);
int xrc_guard_current(uint32_t vram) {
    unsigned lo = 0, hi = xrc_guard_count, j;
    while (lo < hi) {
        unsigned m = (lo + hi) / 2;
        if (xrc_guard_vram[m] < vram) lo = m + 1;
        else hi = m;
    }
    if (lo >= xrc_guard_count || xrc_guard_vram[lo] != vram) return 1;
    for (j = lo; j < xrc_guard_count && xrc_guard_vram[j] == vram; j++)
        if (verify(j)) return 1;
    return 0;
}

void xrc_guard_miss(uint8_t* rdram, recomp_context* ctx, uint32_t vram, unsigned i, recomp_func_t* body) {
    static uint32_t s_logged_at[64];   /* each function once (the first 32 lines were one function's) */
    static unsigned s_logged;
    unsigned k;
    recomp_func_t* other;
    if (i < xrc_guard_count && verify(i)) {
        body(rdram, ctx);
        return;
    }
    if (i < xrc_guard_count && (other = sibling(i)) != NULL) {
        other(rdram, ctx);
        return;
    }
    for (k = 0; k < s_logged && s_logged_at[k] != vram; k++) {}
    if (k == s_logged && s_logged < 64) {
        s_logged_at[s_logged++] = vram;
        xhw_logf("[GUARD] %08x: RDRAM holds other code than it was recompiled from; interpreted", (unsigned)vram);
    }
    xrc_learn_recapture_n(vram, i < xrc_guard_count ? xrc_guard_size[i] : 4u);
    xrc_interp(rdram, ctx, vram);
}

/* Code captured at a KUSEG address the game maps through its TLB (Mario
 * Tennis's 0x50000000) is reached at that vaddr, but the game loads it and
 * invalidates the cache by its KSEG0 address, and remaps the TLB over other
 * pages: either way its guard entries are checked again (they sort first,
 * below 0x80000000). Kill switch: -DXRC_GUARD_KUSEG=0 (only the range named,
 * as before). */
#ifndef XRC_GUARD_KUSEG
#define XRC_GUARD_KUSEG 1
#endif
void xrc_guard_kuseg(void) {
    unsigned i;
    if (!XRC_GUARD_KUSEG) return;
    for (i = 0; i < xrc_guard_count && xrc_guard_vram[i] < 0x80000000u; i++) xrc_guard_state[i] = UNVERIFIED;
}

/* Only the KUSEG entries a change reaches: a TLB entry rewritten over
 * [va0, va1), and a DMA or an invalidation by the physical pages under the
 * entries' mappings. All-Star Baseball 2000 runs its whole game at KUSEG
 * 0x00100000: every DMA and every TLB write (its pager's) had every
 * function hashed again at its next call. A KUSEG page mapped nowhere is
 * checked again whenever anything changes. -DXRC_GUARD_KUSEG_EXACT=0: every
 * KUSEG entry, as before. */
#ifndef XRC_GUARD_KUSEG_EXACT
#define XRC_GUARD_KUSEG_EXACT 1
#endif
void xrc_guard_kuseg_range(uint32_t va0, uint32_t va1) {
    unsigned i;
    if (!XRC_GUARD_KUSEG) return;
    if (!XRC_GUARD_KUSEG_EXACT) { xrc_guard_kuseg(); return; }
    i = 0;
    if (XRC_GUARD_PAGED) {   /* sorted by vram: no scan of all of them for every TLB entry written */
        unsigned lo = 0, hi = xrc_guard_count;
        uint32_t from = va0 > 0x40000u ? va0 - 0x40000u : 0;   /* (a function that starts before it and reaches in) */
        while (lo < hi) {
            unsigned mid = (lo + hi) / 2;
            if (xrc_guard_vram[mid] < from) lo = mid + 1;
            else hi = mid;
        }
        i = lo;
    }
    for (; i < xrc_guard_count && xrc_guard_vram[i] < 0x80000000u && (!XRC_GUARD_PAGED || xrc_guard_vram[i] < va1); i++)
        if (xrc_guard_vram[i] < va1 && xrc_guard_vram[i] + xrc_guard_size[i] > va0) xrc_guard_state[i] = UNVERIFIED;
}

static void kuseg_phys(uint32_t p0, uint32_t p1) {
    uint32_t page = ~0u, pp = ~0u;
    unsigned i;
    for (i = 0; i < xrc_guard_count && xrc_guard_vram[i] < 0x80000000u; i++) {
        uint32_t v = xrc_guard_vram[i], n = xrc_guard_size[i], pg = v & ~(PAGE - 1), last, lp;
        if (pg != page) page = pg, pp = xrc_tlb_to_phys(pg);
        last = (v + (n ? n - 1 : 0)) & ~(PAGE - 1);
        lp = last == pg ? pp : xrc_tlb_to_phys(last);
        if (pp == ~0u || lp == ~0u) xrc_guard_state[i] = UNVERIFIED;
        else if ((pp + (v - pg) < p1 && pp + PAGE > p0) || (last != pg && lp < p1 && lp + PAGE > p0))
            xrc_guard_state[i] = UNVERIFIED;
    }
}

/* the game invalidated the instruction cache over [vram, vram + len), or a DMA wrote it */
void xrc_guard_inval(uint32_t vram, uint32_t len) {
    unsigned lo = 0, hi = xrc_guard_count;
    uint32_t end;
    if (!xrc_guard_count || !len) return;
    if (!XRC_GUARD_KUSEG_EXACT || !XRC_GUARD_KUSEG) xrc_guard_kuseg();
    else if (xrc_guard_vram[0] < 0x80000000u) {   /* KUSEG entries: by the physical range */
        uint32_t p = (int32_t)vram < 0 ? vram & 0x1FFFFFFFu : xrc_tlb_to_phys(vram);
        if (p == ~0u) xrc_guard_kuseg();
        else kuseg_phys(p & ~(PAGE - 1), p + len);
    }
    if ((vram & 0xE0000000u) == 0xA0000000u) vram ^= 0x20000000u;   /* KSEG1 onto KSEG0 */
    end = vram + len;
    vram &= ~(PAGE - 1);
    while (lo < hi) {   /* the first page at or past vram */
        unsigned mid = (lo + hi) / 2;
        if (xrc_guard_vram[mid] < vram) lo = mid + 1;
        else hi = mid;
    }
    if (lo) lo--;   /* a function that starts before vram can reach into it */
    for (; lo < xrc_guard_count && xrc_guard_vram[lo] < end; lo++)
        if (xrc_guard_vram[lo] + xrc_guard_size[lo] > vram) xrc_guard_state[lo] = UNVERIFIED;
}
