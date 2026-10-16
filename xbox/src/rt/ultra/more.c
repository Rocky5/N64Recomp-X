/* more.c - libultra functions on the recompiler's ignored list that a game
 * built from the ROM alone (PLAN.md 3.14) calls from its own code, so the
 * runtime has to answer them:
 *   - the TLB (osMapTLB, osUnmapTLB, osUnmapTLBAll, osMapTLBRdb): the
 *     runtime has no TLB; game code reaches RDRAM through KSEG0 addresses
 *     (Super Mario 64 clears the TLB at boot and maps nothing it uses).
 * DIVERGES: no TLB; a game that reads through a mapped KUSEG address is
 * not supported (osMapTLB says so once in the log). */
#include "ultra.h"

ULTRA(osUnmapTLBAll) {
    (void)rdram;
    (void)ctx;
}

ULTRA(osUnmapTLB) {
    (void)rdram;
    (void)ctx;
}

ULTRA(osMapTLB) {
    static int said;
    (void)rdram;
    if (!said) {
        said = 1;
        xhw_logf("[OS] osMapTLB(%u, vaddr %08x): no TLB here, the mapping is ignored", (unsigned)ARG(0),
                 (unsigned)ARG(2));
    }
}

ULTRA(osMapTLBRdb) {
    (void)rdram;
    (void)ctx;
}
