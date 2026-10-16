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
/* tlb.c: the N64 TLB's 32 entries; a valid entry's pages are aliased at
 * the host address the generated code computes for them (-DXRC_TLB=0: kept,
 * not aliased). _probe is tlbp's Index (bit 31: no match); _to_phys ~0 when
 * unmapped. */
void xrc_tlb_write(unsigned idx, uint32_t mask, uint32_t hi, uint32_t lo0, uint32_t lo1);
void xrc_tlb_read(unsigned idx, uint32_t* mask, uint32_t* hi, uint32_t* lo0, uint32_t* lo1);
uint32_t xrc_tlb_probe(uint32_t hi);
uint32_t xrc_tlb_to_phys(uint32_t vaddr);

/* mmio.c: the RCP's registers, the PIF and the cartridge through KSEG1, for
 * the interpreter (xrc_mmio_is: KSEG1 past RDRAM; -DXRC_MMIO=0: never) */
void xrc_mmio_init(void);
void xrc_mmio_vi(void);
int xrc_mmio_is(uint32_t vaddr);
uint32_t xrc_mmio_read(uint32_t vaddr);
void xrc_mmio_write(uint32_t vaddr, uint32_t v);
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

/* learn.c: the safety net (PLAN.md 3.14 step 5). -DXRC_SAFETY_NET=0: a call
 * to an address with no function is the fatal card, as before the net */
#ifndef XRC_SAFETY_NET
#define XRC_SAFETY_NET 1
#endif
recomp_func_t* xrc_learn_lookup(uint32_t vram, uintptr_t host_caller);
void xrc_learn_dma(uint32_t rom, uint32_t ram, uint32_t size);
void xrc_learn_inval(uint32_t ram, uint32_t size);

/* hooks.c */
void xrc_fatal_vram(const char* what, uint32_t vram, uint32_t extra) __attribute__((noreturn));

#endif
