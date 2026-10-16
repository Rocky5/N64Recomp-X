/* rt.h - shared inside the runtime (xbox/src/rt, the game triple). */
#ifndef XRC_RT_H
#define XRC_RT_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "recomp.h"
#include "xrc_game.h"

/* mem.c: the RDRAM window (PLAN.md 3.2.1). xrc_rdram is what the generated
 * code gets as `rdram`: the window's physical base, so rdram + 0x80000000 + x
 * is RDRAM byte x; xrc_rdram_host is that byte 0's host address. */
extern uint8_t* xrc_rdram;
extern uint8_t* xrc_rdram_host;
extern uint32_t xrc_rdram_size;
int xrc_mem_init(uint32_t mb);
/* host pointer of a KSEG0/KSEG1/physical N64 address inside the window, or NULL */
void* xrc_host(uint32_t vram, uint32_t bytes);
/* a 32-bit word of the image as the game sees it (words are native) */
static inline uint32_t xrc_word(uint32_t vram) { return *(uint32_t*)(xrc_rdram + (vram & 0x1FFFFFFFu) + 0x80000000u); }

/* ovl.c: the function table (PLAN.md 3.4) */
int xrc_ovl_init(void);
uint32_t xrc_ovl_function_count(void);

/* a fresh context for a thread or the boot code: everything zero, sp set,
 * interrupts on, and f_odd at f0's high word (the generated code reaches an
 * odd FPR as the high half of the even one below it: COP1 with FR = 0, as
 * O32 code runs) */
static inline void xrc_ctx_init(recomp_context* ctx, uint32_t sp) {
    memset(ctx, 0, sizeof *ctx);
    ctx->r29 = (gpr)(int32_t)sp;
    ctx->status_reg = 1;
    ctx->f_odd = &ctx->f0.u32h;
    ctx->mips3_float_mode = 0;
}

/* hooks.c */
void xrc_fatal_vram(const char* what, uint32_t vram, uint32_t extra) __attribute__((noreturn));

#endif
