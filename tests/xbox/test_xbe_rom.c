/* test_xbe_rom.c - the ROM inside the XBE (PLAN.md 3.5): xrom_xbe_section
 * finds the $ROM section tools/xbox/xbe_sections.py appended, and the bytes
 * at its raw offset pass xrom_verify as the file beside the XBE did.
 *
 *   test_xbe_rom <xbe> <sha1 of the ROM> <game code>
 *
 * Run by tools/xbox/test_xbe_rom.py, which writes the XBE. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xrom.h"

static int fails;

#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
            fails++;                                                     \
        }                                                                \
    } while (0)

static uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int main(int argc, char** argv) {
    FILE* f;
    uint8_t *xbe, *hdr;
    long n;
    uint32_t len, raw = 0, size = 0, r2, s2;
    xrom_target t;
    xrom_info info;
    if (argc != 4) {
        printf("usage: test_xbe_rom <xbe> <sha1> <code>\n");
        return 2;
    }
    f = fopen(argv[1], "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    xbe = malloc((size_t)n);
    if (!xbe || fread(xbe, 1, (size_t)n, f) != (size_t)n) return 2;
    fclose(f);

    /* what the kernel maps at 0x10000: the first SizeOfHeaders bytes */
    len = le32(xbe + 0x108);
    hdr = malloc(len);
    memcpy(hdr, xbe, len);
    CHECK(xrom_xbe_section(hdr, len, XROM_XBE_SECTION, &raw, &size) == 1);
    CHECK(raw % 0x1000 == 0);
    CHECK(raw >= len);
    CHECK((long)raw + (long)size == n);   /* the last thing in the file */
    CHECK(xrom_xbe_section(hdr, len, "$$XTIMAGE", &r2, &s2) == 1 && r2 + s2 <= raw);
    CHECK(xrom_xbe_section(hdr, len, ".text", &r2, &s2) == 1);
    CHECK(xrom_xbe_section(hdr, len, "$RO", &r2, &s2) == 0);      /* a prefix is not a match */
    CHECK(xrom_xbe_section(hdr, len, "$ROMX", &r2, &s2) == 0);
    CHECK(xrom_xbe_section(hdr, 0x100, XROM_XBE_SECTION, &r2, &s2) == 0);   /* cut-short header */
    hdr[0] = 'Y';
    CHECK(xrom_xbe_section(hdr, len, XROM_XBE_SECTION, &r2, &s2) == 0);     /* not an XBE */
    hdr[0] = 'X';
    {   /* a section table pointer outside the header */
        uint32_t keep = le32(hdr + 0x120), bad = le32(hdr + 0x104) + len;
        memcpy(hdr + 0x120, &bad, 4);
        CHECK(xrom_xbe_section(hdr, len, XROM_XBE_SECTION, &r2, &s2) == 0);
        memcpy(hdr + 0x120, &keep, 4);
    }

    /* the embedded bytes pass the same check as the file beside the XBE */
    memset(&t, 0, sizeof t);
    snprintf(t.title, sizeof t.title, "N64Recomp-X test");
    snprintf(t.code, sizeof t.code, "%s", argv[3]);
    t.size = size;
    t.nsha1 = 1;
    snprintf(t.sha1[0], sizeof t.sha1[0], "%s", argv[2]);
    if (raw + size <= (uint32_t)n) {
        CHECK(xrom_verify(xbe + raw, size, &info, &t) == XROM_OK);
        CHECK(info.order == XROM_ORDER_Z64);
        CHECK(strcmp(info.code, argv[3]) == 0);
        xbe[raw + size / 2] ^= 1;   /* one bit off: the hash says so */
        CHECK(xrom_verify(xbe + raw, size, &info, &t) == XROM_BAD_HASH);
    }
    printf("test_xbe_rom: $ROM at 0x%x, %u bytes; %s\n", (unsigned)raw, (unsigned)size, fails ? "FAILED" : "ok");
    free(hdr);
    free(xbe);
    return fails ? 1 : 0;
}
