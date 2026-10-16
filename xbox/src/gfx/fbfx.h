/* fbfx.h - framebuffer effects' bookkeeping (fbfx.c; docs/renderer.md
 * "Framebuffer effects"). Pure C, no back end: the interpreter feeds it and
 * acts on its answers; tests/xbox/test_fbfx.c drives it on the host.
 *
 * The renderer draws on the NV2A, so a frame never lands in RDRAM. A game
 * that reads its own frame back (Mario Kart 64's big screens copy the frame
 * before with the CPU, Pokemon Snap's photos) would see whatever RDRAM held.
 * Each frame the interpreter presents is a slot here: its colour image
 * (address, width, height, pixel size), where its small copy sits on the
 * back end's readback stage, and whether RDRAM still lacks it (stale). */
#ifndef FBFX_H
#define FBFX_H
#include <stdint.h>

#define FBFX_SLOTS 4   /* a 2 x 2 grid on the stage: triple-buffered games use three */

typedef struct {
    uint32_t addr;    /* N64 physical address of the colour image; 0: free */
    uint32_t w, h;    /* pixels */
    uint32_t siz;     /* 2: RGBA5551, 3: RGBA8888 (G_IM_SIZ_*) */
    uint32_t frame;   /* the frame drawn into it last (least recently drawn goes first) */
    int stale;        /* RDRAM does not hold its last picture yet */
    int hot;          /* frames left of reading it back every frame (the game read it lately) */
} FbfxSlot;

#define FBFX_HOT 60   /* a frame the game stopped reading is still read back for 2 s (30 fps) */

void fbfx_reset(void);
static inline uint32_t fbfx_bytes(const FbfxSlot* s) { return s->w * s->h * (s->siz == 3 ? 4u : 2u); }
/* A frame drew into this colour image and is about to be presented: the slot
 * it goes to, or -1 when it is no frame we copy (a size or format outside
 * the RDP's colour images, past RDRAM's end). Slots it replaces or overlaps
 * are dropped first and copied out to drop[] (*ndrop of them): a stale one
 * must have its pages unguarded by the caller. */
int fbfx_frame_end(uint32_t addr, uint32_t w, uint32_t h, uint32_t siz, uint32_t frame, uint32_t ram_size,
                   FbfxSlot drop[FBFX_SLOTS], int* ndrop);
/* At a frame's start, before the readback: did any thread touch slot i's
 * pages since they were last armed? A touch makes the slot hot (FBFX_HOT
 * frames), no touch cools it by one. 1: read it back now (hot and stale). */
int fbfx_frame_start(int i, int touched);
/* the stale slot that overlaps [addr, addr + bytes), or -1 */
int fbfx_find(uint32_t addr, uint32_t bytes);
int fbfx_stale_count(void);
const FbfxSlot* fbfx_slot(int i);
void fbfx_mark(int i, int stale);
/* where slot i's picture goes on a stage_w x stage_h stage: x, y, w, h (the
 * slot's quarter; an image larger than a quarter is shrunk by a whole factor) */
void fbfx_rect(int i, int32_t stage_w, int32_t stage_h, int32_t r[4]);
/* the stage's picture of slot s (rect r; bpp 32: X8R8G8B8, 16: R5G6B5) into
 * RDRAM as the game would have it: ram is RDRAM byte 0 (the word-swapped
 * image: words native, a halfword at its address ^ 2), coverage/alpha set */
void fbfx_convert(const FbfxSlot* s, const int32_t r[4], const uint8_t* stage, int32_t pitch, int32_t bpp, uint8_t* ram,
                  uint32_t ram_size);

#endif
