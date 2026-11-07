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
    if (XHW_TEST_BUILD) {   /* what ran up to it (a crash report has this already) */
        void xrc_trace_dump(void);
        xrc_trace_dump();
    }
    xhw_fatal("The game stopped", line);
}

/* cop0_status_write / _read: ultra/thread.c (the interrupt mask) */

void switch_error(const char* func, uint32_t vram, uint32_t jtbl) {
    xhw_logf("[RECOMP] %s: jump table %08x has no case for the value at %08x", func, (unsigned)jtbl, (unsigned)vram);
    xrc_fatal_vram("Jump table out of range", vram, jtbl);
}

void do_break(uint32_t vram) { xrc_fatal_vram("break", vram, 0); }

/* A game's __osException may have a case of its own for a syscall: t0 = the EPC, EPC = its target, eret, every other
 * register as it was. Banjo-Tooie calls its overlays through stubs, `syscall <overlay>` until its linker, the target,
 * has loaded the overlay and rewritten them to a jump; the linker then jumps back to t0. Read from the game's own code:
 * andi t1, t0, 0x7C; addiu t2, zero, 0x20; a branch; and lui t1 / addiu t1 with the target in the next few words. 0:
 * no such case (libultra's panic path, a crash here as there). -DXRC_SYSCALL_CASE=0: a fatal card, as before */
#ifndef XRC_SYSCALL_CASE
#define XRC_SYSCALL_CASE 1
#endif
#if defined(XRC_OS_EXCEPTION) && XRC_SYSCALL_CASE
static uint32_t syscall_target(uint8_t* rdram) {
    uint32_t a = XRC_OS_EXCEPTION;
    unsigned i, j;
    for (i = 0; i < 32; i++, a += 4) {
        if ((uint32_t)MEM_W(0, (gpr)(int32_t)a) != 0x3109007Cu || (uint32_t)MEM_W(4, (gpr)(int32_t)a) != 0x240A0020u) continue;
        for (j = 2; j < 10; j++) {
            uint32_t hi = (uint32_t)MEM_W(4 * (int)j, (gpr)(int32_t)a), lo = (uint32_t)MEM_W(4 * (int)j + 4, (gpr)(int32_t)a);
            if ((hi & 0xFFFF0000u) == 0x3C090000u && (lo & 0xFFFF0000u) == 0x25290000u)
                return (hi << 16) + (uint32_t)(int32_t)(int16_t)lo;
        }
    }
    return 0;
}
#endif

void recomp_syscall_handler(uint8_t* rdram, recomp_context* ctx, int32_t instruction_vram) {
#if defined(XRC_OS_EXCEPTION) && XRC_SYSCALL_CASE
    uint32_t target = syscall_target(rdram);
    if (target) {
        ctx->r8 = (gpr)instruction_vram;
        get_function((int32_t)target)(rdram, ctx);
        return;
    }
#else
    (void)rdram, (void)ctx;
#endif
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
