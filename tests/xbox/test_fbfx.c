/* test_fbfx.c - framebuffer effects' bookkeeping on the host (fbfx.c;
 * tools/xbox/test_fbfx.py, docs/renderer.md "Framebuffer effects"): which
 * colour images become slots, which slots a new one drops, the stale marks
 * the readback keys on, where a slot sits on the stage, and the conversion
 * into the word-swapped RDRAM image. The interpreter's side (the copy, the
 * guards, the fault) is in test_f3dex.c's test_readback. */
#include <stdio.h>
#include <string.h>

#include "fbfx.h"

static int s_fail;
#define CHECK(c, ...)                                    \
    do {                                                 \
        if (!(c)) {                                      \
            s_fail++;                                    \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                         \
            printf("\n");                                \
        }                                                \
    } while (0)

#define RAM (4u << 20)
static uint8_t s_ram[RAM] __attribute__((aligned(16)));
static uint32_t s_stage[640 * 480];

int main(void) {
    FbfxSlot drop[FBFX_SLOTS];
    int nd, a, b, c, d, e;
    int32_t r[4];
    fbfx_reset();
    /* not colour images */
    CHECK(fbfx_frame_end(0, 320, 240, 2, 1, RAM, drop, &nd) < 0, "address 0 taken");
    CHECK(fbfx_frame_end(0x100004, 320, 240, 2, 1, RAM, drop, &nd) < 0, "unaligned taken");
    CHECK(fbfx_frame_end(0x100000, 320, 240, 1, 1, RAM, drop, &nd) < 0, "8-bit taken");
    CHECK(fbfx_frame_end(RAM - 1000, 320, 240, 2, 1, RAM, drop, &nd) < 0, "past RDRAM taken");
    /* a triple-buffered game: three slots, then the same three again */
    a = fbfx_frame_end(0x80100000u, 320, 240, 2, 1, RAM, drop, &nd);
    b = fbfx_frame_end(0x00140000u, 320, 240, 2, 2, RAM, drop, &nd);
    c = fbfx_frame_end(0x00180000u, 320, 240, 2, 3, RAM, drop, &nd);
    CHECK(a >= 0 && b >= 0 && c >= 0 && a != b && b != c && a != c && nd == 0, "three slots %d %d %d", a, b, c);
    CHECK(fbfx_slot(a)->addr == 0x100000u, "KSEG0 not masked: %08x", (unsigned)fbfx_slot(a)->addr);
    fbfx_mark(a, 1);
    CHECK(fbfx_frame_end(0x100000u, 320, 240, 2, 4, RAM, drop, &nd) == a && nd == 0, "same image, new slot");
    CHECK(fbfx_slot(a)->stale == 1, "same image lost its stale mark");
    CHECK(fbfx_stale_count() == 1, "stale count %d", fbfx_stale_count());
    CHECK(fbfx_find(0x100000u + 153599u, 1) == a, "last byte not found");
    CHECK(fbfx_find(0x100000u + 153600u, 1) < 0, "byte past the end found");
    CHECK(fbfx_find(0x80100010u, 4) == a, "KSEG0 find");
    CHECK(fbfx_find(0x140000u, 4) < 0, "a slot not stale found");
    /* a fourth image, then a fifth: the least recently drawn goes, copied to drop[] */
    d = fbfx_frame_end(0x1C0000u, 320, 240, 2, 5, RAM, drop, &nd);
    CHECK(d >= 0 && nd == 0, "fourth slot %d drops %d", d, nd);
    e = fbfx_frame_end(0x200000u, 320, 240, 2, 6, RAM, drop, &nd);
    CHECK(e == b && nd == 1 && drop[0].addr == 0x140000u, "fifth took %d, dropped %d (%08x)", e, nd, (unsigned)drop[0].addr);
    /* a new layout over a stale image drops it (the caller unguards it) */
    e = fbfx_frame_end(0x100000u + 0x1000u, 320, 240, 2, 7, RAM, drop, &nd);
    CHECK(nd >= 1 && drop[0].addr == 0x100000u && drop[0].stale == 1, "overlap: %d dropped, first %08x stale %d", nd,
          (unsigned)drop[0].addr, drop[0].stale);
    CHECK(fbfx_find(0x100000u, 4) < 0, "the dropped image still found");
    /* hot: a touch reads back while stale for FBFX_HOT frames, cooling by one a frame */
    fbfx_reset();
    a = fbfx_frame_end(0x100000u, 320, 240, 2, 1, RAM, drop, &nd);
    CHECK(fbfx_frame_start(a, 1) == 0, "not stale yet, read back");
    fbfx_mark(a, 1);
    CHECK(fbfx_frame_start(a, 0) == 1, "touched last frame: not read back");
    {
        int k, n = 0;
        for (k = 0; k < FBFX_HOT + 5; k++) n += fbfx_frame_start(a, 0);
        CHECK(n == FBFX_HOT - 2, "cooled after %d frames, want %d", n, FBFX_HOT - 2);
    }
    CHECK(fbfx_frame_start(a, 0) == 0, "cold, still read back");
    CHECK(fbfx_frame_start(a, 1) == 1, "touched again, not read back");
    CHECK(fbfx_frame_end(0x100000u, 320, 240, 2, 2, RAM, drop, &nd) == a && fbfx_slot(a)->hot == FBFX_HOT,
          "the same image lost its heat");
    /* the stage: quarters, a 640-wide image halved */
    fbfx_reset();
    a = fbfx_frame_end(0x100000u, 320, 240, 2, 1, RAM, drop, &nd);
    b = fbfx_frame_end(0x200000u, 640, 480, 3, 2, RAM, drop, &nd);
    fbfx_rect(a, 640, 480, r);
    CHECK(r[0] == 0 && r[1] == 0 && r[2] == 320 && r[3] == 240, "slot 0 rect %d %d %d %d", r[0], r[1], r[2], r[3]);
    fbfx_rect(b, 640, 480, r);
    CHECK(r[0] == 320 && r[1] == 0 && r[2] == 320 && r[3] == 240, "640x480 rect %d %d %d %d", r[0], r[1], r[2], r[3]);
    /* conversion: 32-bit stage into an RGBA8888 image (words native), 2x up */
    {
        int i;
        for (i = 0; i < 640 * 480; i++) s_stage[i] = (i % 640) >= 320 ? 0xFF102030u : 0;
        fbfx_convert(fbfx_slot(b), r, (const uint8_t*)s_stage, 640 * 4, 32, s_ram, RAM);
        CHECK(*(uint32_t*)(s_ram + 0x200000u) == 0x102030FFu, "RGBA8888 %08x", *(uint32_t*)(s_ram + 0x200000u));
        CHECK(*(uint32_t*)(s_ram + 0x200000u + 4u * (479u * 640u + 639u)) == 0x102030FFu, "RGBA8888 last pixel");
        CHECK(*(uint32_t*)(s_ram + 0x200000u + 4u * 640u * 480u) == 0, "written past the image");
    }
    /* 16-bit stage (R5G6B5 at 720p) into RGBA5551: green 0x07E0 -> 0x07C1, halfwords at ^2 */
    {
        uint16_t st16[640 * 4];
        int i;
        for (i = 0; i < 640 * 4; i++) st16[i] = i & 1 ? 0x07E0u : 0xF800u;
        fbfx_reset();
        a = fbfx_frame_end(0x300000u, 16, 2, 2, 1, RAM, drop, &nd);
        CHECK(a < 0, "16x2 image taken (too small)");
        a = fbfx_frame_end(0x300000u, 16, 8, 2, 1, RAM, drop, &nd);
        r[0] = 0, r[1] = 0, r[2] = 16, r[3] = 4;
        fbfx_convert(fbfx_slot(a), r, (const uint8_t*)st16, 640 * 2, 16, s_ram, RAM);
        CHECK(*(uint16_t*)(s_ram + (0x300000u ^ 2u)) == 0xF801u, "pixel 0 %04x", *(uint16_t*)(s_ram + (0x300000u ^ 2u)));
        CHECK(*(uint16_t*)(s_ram + (0x300002u ^ 2u)) == 0x07C1u, "pixel 1 %04x", *(uint16_t*)(s_ram + (0x300002u ^ 2u)));
    }
    if (s_fail) printf("test_fbfx: %d failed\n", s_fail);
    return s_fail ? 1 : 0;
}
