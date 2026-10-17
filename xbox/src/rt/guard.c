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

#include "rt.h"
#include "xhw.h"

/* gen/xrc_guard.c's tables, sorted by vram; a build without captured code has none */
__attribute__((weak)) const uint32_t xrc_guard_vram[1];
__attribute__((weak)) const uint32_t xrc_guard_size[1];
__attribute__((weak)) const uint32_t xrc_guard_hash[1];
__attribute__((weak)) const unsigned xrc_guard_count;
__attribute__((weak)) volatile unsigned char xrc_guard_state[1];

#define PAGE 0x1000u   /* invalidations round to it */
enum { UNVERIFIED = 0, VERIFIED = 1, OTHER = 2 };

void xrc_interp(uint8_t* rdram, recomp_context* ctx, uint32_t entry);
void xrc_learn_recapture(uint32_t vram);   /* learn.c */

static int word(uint32_t v, uint32_t* w) {
    if ((int32_t)v >= 0) {   /* KUSEG: through the TLB */
        uint32_t p = xrc_tlb_to_phys(v);
        if (p == ~0u || p >= xrc_rdram_size) return 0;
        v = 0x80000000u | p;
    }
    *w = xrc_word(v);
    return 1;
}

/* FNV-1a over the function's words as the N64 reads them (pipeline.py's twin) */
static int fn_hash(uint32_t v, uint32_t n, uint32_t* out) {
    uint32_t h = 0x811C9DC5u, i, w;
    for (i = 0; i < n; i += 4) {
        if (!word(v + i, &w)) return 0;
        h = (h ^ w) * 0x01000193u;
    }
    *out = h;
    return 1;
}

void xrc_guard_miss(uint8_t* rdram, recomp_context* ctx, uint32_t vram, unsigned i, recomp_func_t* body) {
    static unsigned s_logged;
    uint32_t h;
    if (i < xrc_guard_count && xrc_guard_state[i] == UNVERIFIED)
        xrc_guard_state[i] = fn_hash(xrc_guard_vram[i], xrc_guard_size[i], &h) && h == xrc_guard_hash[i] ? VERIFIED
                                                                                                      : OTHER;
    if (i < xrc_guard_count && xrc_guard_state[i] == VERIFIED) {
        body(rdram, ctx);
        return;
    }
    if (s_logged < 32) {
        s_logged++;
        xhw_logf("[GUARD] %08x: RDRAM holds other code than it was recompiled from; interpreted", (unsigned)vram);
    }
    xrc_learn_recapture(vram);
    xrc_interp(rdram, ctx, vram);
}

/* the game invalidated the instruction cache over [vram, vram + len), or a DMA wrote it */
void xrc_guard_inval(uint32_t vram, uint32_t len) {
    unsigned lo = 0, hi = xrc_guard_count;
    uint32_t end;
    if (!xrc_guard_count || !len) return;
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
