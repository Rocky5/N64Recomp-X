/* more2.c - libultra pieces ROM-only games reach that the other files do
 * not hold (PLAN.md 3.3, 3.14):
 *   - the TLB family (osMapTLB, osUnmapTLB, osUnmapTLBAll, __osProbeTLB, the
 *     __osGetTLB* readers, osSetTLBASID, osMapTLBRdb) on tlb.c, for when the
 *     analyser names them; unnamed, the same code runs in the interpreter,
 *     whose tlbwi/tlbp/tlbr reach tlb.c too.
 *   - __osEnqueueAndYield, which libultra code the analyser did not name
 *     calls (Dr. Mario 64's osYieldThread): the threads are the kernel's
 *     (thread.c), so it is a pause with the interrupt lock let go; every
 *     libultra caller rechecks its condition in a loop (osRecvMesg's
 *     validCount, osYieldThread none), so a return is a spurious wake-up.
 * DIVERGES: osSetTLBASID does nothing (tlb.c ignores the ASID); osMapTLBRdb
 * maps nothing (no debugger port); __osEnqueueAndYield does not queue the
 * thread (a waiter polls every millisecond instead of sleeping until
 * __osPopThread). */
#include "ultra.h"

void xu_pause_unlocked(uint32_t ms);   /* thread.c */

/* __osEnqueueAndYield(queue): 1 ms, so a busy caller at a high level does
 * not starve the lower ones (the kernel preempts strictly by priority) */
ULTRA(__osEnqueueAndYield) {
    (void)rdram;
    (void)ctx;
    xu_pause_unlocked(1);
}

#define NTLB 32
#define LO_FLAGS 0x1Eu   /* cacheable noncoherent, dirty, valid */

static uint32_t lo_of(uint32_t paddr, uint32_t g) {
    return paddr == 0xFFFFFFFFu ? 1u : ((((paddr & 0x1FFFFFFFu) >> 12) << 6) | LO_FLAGS | g);
}

/* osMapTLB(index, pagemask, vaddr, evenpaddr, oddpaddr, asid): an asid of
 * -1 makes the entry global, a paddr of -1 leaves that page invalid */
ULTRA(osMapTLB) {
    uint32_t asid = ARG(5), g = asid == 0xFFFFFFFFu ? 1u : 0u;
    (void)rdram;
    xrc_tlb_write(ARG(0) & (NTLB - 1), ARG(1), (ARG(2) & 0xFFFFE000u) | (g ? 0 : (asid & 0xFF)), lo_of(ARG(3), g),
                  lo_of(ARG(4), g));
}

/* an unmapped entry: EntryHi in KSEG0 (never matches), both pages invalid */
static void unmap(unsigned i) { xrc_tlb_write(i, 0, 0x80000000u + (i << 13), 1, 1); }

ULTRA(osUnmapTLB) {
    (void)rdram;
    unmap(ARG(0) & (NTLB - 1));
}

ULTRA(osUnmapTLBAll) {
    unsigned i;
    (void)rdram;
    (void)ctx;
    for (i = 0; i < NTLB; i++) unmap(i);
}

ULTRA(osSetTLBASID) {
    (void)rdram;
    (void)ctx;
}

ULTRA(osMapTLBRdb) {
    (void)rdram;
    (void)ctx;
}

/* __osProbeTLB(vaddr): the physical address, or -1 when no entry maps it */
ULTRA(__osProbeTLB) {
    (void)rdram;
    RET(xrc_tlb_to_phys(ARG(0)));
}

static uint32_t tlb_field(unsigned i, int which) {
    uint32_t f[4];
    xrc_tlb_read(i & (NTLB - 1), &f[0], &f[1], &f[2], &f[3]);
    return f[which];
}
ULTRA(__osGetTLBPageMask) {
    (void)rdram;
    RET(tlb_field(ARG(0), 0));
}
ULTRA(__osGetTLBHi) {
    (void)rdram;
    RET(tlb_field(ARG(0), 1));
}
ULTRA(__osGetTLBLo0) {
    (void)rdram;
    RET(tlb_field(ARG(0), 2));
}
ULTRA(__osGetTLBLo1) {
    (void)rdram;
    RET(tlb_field(ARG(0), 3));
}
ULTRA(__osGetTLBASID) {
    (void)rdram;
    (void)ctx;
    RET(0);
}
