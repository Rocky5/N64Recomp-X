/* WWF No Mercy Plus V3 (NEDE, a ROM hack of WWF No Mercy): its hook in libultra's VI manager, written
 * here (MIT). n64rx builds it in place of the hack's code at 0x800430C0 (N64RX_PATCHES=0: not used).
 *
 * The hack jumps from __osViMgrMain (0x80038240, after a retrace's count) to 0x800430C0, a gap in the
 * boot segment, which stores the save block's address plus 0x10000 (the word at 0x800F7634, plus
 * 0x10000) at 0x804000F0, and jumps back into the manager's loop. Its menu overlay reads that word
 * (0x800E5B48: the base of a 109-entry table). The runtime replaces the VI manager, so the store never
 * ran, the word stayed 0 and the overlay wrote through 0xFFFF8864 at frame 173 (xemu).
 *
 * DIVERGES: the store is made at each of the runtime's retraces (xrc_game_vi, ultra/vi.c), as the
 * hack's manager made it at each of the N64's; called as a function, the hack's code stores and returns.
 */
#include <string.h>

#include "recomp.h"

extern uint8_t* xrc_rdram_host;

#define SAVE_BLOCK 0x800F7634u
#define HACK_WORD 0x804000F0u

static uint32_t* word_at(uint32_t vram) { return (uint32_t*)(xrc_rdram_host + (vram & 0x1FFFFFFFu)); }

static void store_hack_word(void) {
    uint32_t p = *word_at(SAVE_BLOCK);
    if (p) *word_at(HACK_WORD) = p + 0x10000u;
}

void xrc_game_vi(unsigned retrace) {
    (void)retrace;
    store_hack_word();
}

XRC_PATCH(0x800430C0) {
    (void)rdram;
    (void)ctx;
    store_hack_word();
}

/* 0x80001E08: the game's 4x4 float matrix product (out = a x b: a0 a, a1 b, a2 out), with the hack's hook. The hack
 * replaced the first element's store (0x80001E40) with `j 0x80432600`: when the word 12 bytes before out is 215.75
 * (0x4357C0CD) and that element is 0x4029A422, it stores 0x405D0000 instead, then jumps back to 0x80001E48. That jump
 * made the whole routine interpreted, and it runs about 8,000 times a second: 30% of the console's samples in a match
 * (xhw_prof, 2026-11-07). The same products and sums in the same order and single precision as the MIPS code; a row
 * of a is read as the row starts, b as each element is made, each element stored as it is made, as there. */
static float rd_f(uint8_t* rdram, gpr base, int32_t off) {
    int32_t w = MEM_W(off, base);
    float f;
    memcpy(&f, &w, 4);
    return f;
}

static void wr_f(uint8_t* rdram, gpr base, int32_t off, float f) {
    int32_t w;
    memcpy(&w, &f, 4);
    MEM_W(off, base) = w;
}

XRC_PATCH(0x80001E08) {
    gpr a = ctx->r4, b = ctx->r5, out = ctx->r6;
    int i, j;
    for (i = 0; i < 4; i++) {
        float a0 = rd_f(rdram, a, 16 * i), a1 = rd_f(rdram, a, 16 * i + 4);
        float a2 = rd_f(rdram, a, 16 * i + 8), a3 = rd_f(rdram, a, 16 * i + 12);
        for (j = 0; j < 4; j++) {
            float p0 = a0 * rd_f(rdram, b, 4 * j), p1 = a1 * rd_f(rdram, b, 16 + 4 * j);
            float p2 = a2 * rd_f(rdram, b, 32 + 4 * j), p3 = a3 * rd_f(rdram, b, 48 + 4 * j);
            float s = p0 + p1;
            s = s + p2;
            s = s + p3;
            if (i == 0 && j == 0 && (uint32_t)MEM_W(-12, out) == 0x4357C0CDu) {   /* the hack's hook */
                uint32_t w;
                memcpy(&w, &s, 4);
                if (w == 0x4029A422u) {
                    w = 0x405D0000u;
                    memcpy(&s, &w, 4);
                }
            }
            wr_f(rdram, out, 16 * i + 4 * j, s);
        }
    }
}

/* Its graphics thread (0x80003138, as stock No Mercy's) sends each frame as about six tasks and waits for each one's done
 * before the next: the runtime's 2 ms hold of a graphics task's done (ultra/sp.c, XRC_RCP_SENDER_US) came six times a
 * frame, and a 4-wrestler match ran at 20 to 28 fps with the CPU half idle. Nothing here waits on the RCP's time.
 * DIVERGES: the done comes as soon as the task is over (the N64's RDP takes its time). 4-wrestler match: a locked 30. */
int xrc_game_rcp_sender_us(void) { return 0; }

/* It culls what it draws to its own 4:3 view, so hor+ drew only its ropes and turnbuckle pads into the bars, over black
 * (f3d_interp.c, xrc_game_horplus). 16:9 shows the 4:3 picture between bars. */
int xrc_game_horplus(void) { return 0; }
