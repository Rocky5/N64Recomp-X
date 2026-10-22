/* hooks.c - the runtime functions recomp.h declares (PLAN.md 3.2, 3.3):
 * the COP0 status register, pause_self, and the stops (break, an unknown
 * jump-table target, a syscall, a failed check) as fatal cards that name the
 * N64 address. */
#include <stdio.h>

#include "rt.h"
#include "xhw.h"

void xrc_fatal_vram(const char* what, uint32_t vram, uint32_t extra) {
    char line[96];
    snprintf(line, sizeof line, "%s: %08x (%08x)", what, (unsigned)vram, (unsigned)extra);
    xhw_fatal("The game stopped", line);
}

/* cop0_status_write / _read: ultra/thread.c (the interrupt mask) */

void switch_error(const char* func, uint32_t vram, uint32_t jtbl) {
    xhw_logf("[RECOMP] %s: jump table %08x has no case for the value at %08x", func, (unsigned)jtbl, (unsigned)vram);
    xrc_fatal_vram("Jump table out of range", vram, jtbl);
}

void do_break(uint32_t vram) { xrc_fatal_vram("break", vram, 0); }

void recomp_syscall_handler(uint8_t* rdram, recomp_context* ctx, int32_t instruction_vram) {
    (void)rdram;
    (void)ctx;
    xrc_fatal_vram("syscall", (uint32_t)instruction_vram, 0);
}

/* A branch to itself (the idle thread's loop): this kernel thread blocks for
 * good, which is the Xbox's idle. */
void xu_token_idle(void);   /* ultra/thread.c: the run token to the others */
void pause_self(uint8_t* rdram) {
    xhw_event* e = xhw_event_create();
    (void)rdram;
    xu_token_idle();
    for (;;) xhw_event_wait(e, XHW_INFINITE);
}

void xrc_check_failed(const char* what) { xrc_fatal_vram(what, 0, 0); }
