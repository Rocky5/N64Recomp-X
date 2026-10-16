/* rt.h - shared inside the runtime (xbox/src/rt, the game triple). */
#ifndef XRC_RT_H
#define XRC_RT_H

#include <stddef.h>
#include <stdint.h>

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

/* hooks.c */
void xrc_fatal_vram(const char* what, uint32_t vram, uint32_t extra) __attribute__((noreturn));

#endif
