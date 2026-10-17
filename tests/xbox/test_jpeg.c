/* test_jpeg.c - the JPEG decoder task (xbox/src/rt/jpeg.c) on the host:
 * DC-only macroblocks give flat colours with the microcode's numbers.
 * Built by tools/xbox/test_jpeg.py. Exit code 0 = passed. */
#include <stdio.h>
#include <string.h>

#include "rt.h"
#include "xhw.h"
#include "ultra/ultra.h"

static uint8_t s_ram[0x10000];
uint8_t* xrc_rdram;
uint8_t* xrc_rdram_host = s_ram;
uint32_t xrc_rdram_size = sizeof s_ram;
void xhw_log(const char* s) { (void)s; }
void xhw_logf(const char* fmt, ...) { (void)fmt; }
uint32_t xrc_tlb_to_phys(uint32_t v) { (void)v; return ~0u; }

int xrc_jpeg_task(uint32_t task);
static int s_fail;
#define CHECK(c, ...) do { if (!(c)) { s_fail = 1; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static uint16_t px(unsigned r, unsigned g, unsigned b) { return (uint16_t)(r << 11 | g << 6 | b << 1 | 1); }

static void run(int ydc, int udc, int vdc, uint16_t want, const char* what) {
    uint32_t task = 0x80000100u, data = 0x80000200u, mb = 0x80001000u, i;
    memset(s_ram, 0, sizeof s_ram);
    wr32(task + TK_DATA_PTR, data & 0x1FFFFFFFu);
    wr32(data, mb & 0x1FFFFFFFu);
    wr32(data + 4, 1);
    wr32(data + 8, 2);
    wr32(data + 12, 0x400), wr32(data + 16, 0x480), wr32(data + 20, 0x500);
    for (i = 0; i < 64 * 3; i++) wr16(0x80000400u + 2 * i, 1);
    for (i = 0; i < 4; i++) wr16(mb + i * 128u, (uint16_t)ydc);
    wr16(mb + 4 * 128u, (uint16_t)udc);
    wr16(mb + 5 * 128u, (uint16_t)vdc);
    CHECK(xrc_jpeg_task(task) == 1, "%s: not run", what);
    for (i = 0; i < 256; i++)
        if (rd16(mb + 2 * i) != want) {
            CHECK(0, "%s: pixel %u is %04x, want %04x", what, (unsigned)i, rd16(mb + 2 * i), want);
            break;
        }
}

int main(void) {
    run(0, 0, 0, px(16, 16, 16), "mid grey");
    run(64, 0, 0, px(17, 17, 17), "Y DC 64");
    run(64, 0, 64, px(18, 16, 17), "Y and V DC 64");
    {   /* a mode the task does not have: not run */
        memset(s_ram, 0, sizeof s_ram);
        wr32(0x80000100u + TK_DATA_PTR, 0x200);
        wr32(0x80000200u + 8, 7);
        CHECK(xrc_jpeg_task(0x80000100u) == 0, "mode 7 run");
    }
    printf("test_jpeg: %s\n", s_fail ? "FAILED" : "passed");
    return s_fail;
}
