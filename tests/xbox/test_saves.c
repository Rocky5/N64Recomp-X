/* test_saves.c - every save type on the host (PLAN.md 3.6, 4.6).
 *
 * Built by tools/xbox/test_saves.py with xbox/src/rt/ultra/savedev.c,
 * pfs.c and pi.c; save.c's side (the images, their files, the motor) and the
 * platform are this test's. RDRAM is a host buffer in the runtime's
 * word-swapped layout, so the osPfs* and PI paths move bytes as on the
 * console. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "savedev.h"
#include "ultra/ultra.h"

static int s_fail, s_checks;
#define CHECK(cond, ...) do { \
        s_checks++; \
        if (!(cond)) { s_fail++; printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); \
                       printf("\n"); } \
    } while (0)

/* ---- the platform, stubbed ---- */
uint8_t* xrc_rdram;
uint8_t* xrc_rdram_host;
uint32_t xrc_rdram_size;
void xhw_logf(const char* fmt, ...) { (void)fmt; }
int xu_send(uint32_t mq, uint32_t msg, int block, int jam) { (void)mq; (void)msg; (void)block; (void)jam; return 0; }
uint64_t xu_count64(void) { return 0x12345678u; }
void xu_save_lock(void) {}
void xu_save_unlock(void) {}
int xu_pi_save_dma(uint32_t addr, uint32_t dram, uint32_t n, int write);
/* pi.c's ROM side (xrom.h, learn.c): no ROM here */
typedef void (*xrom_sink)(void* user, uint32_t at, const uint8_t* src, uint32_t n);
int xhw_rom_read(uint32_t off, uint32_t n, xrom_sink sink, void* user) { (void)off; (void)n; (void)sink; (void)user; return 0; }
uint32_t xhw_rom_size(void) { return 0; }
const uint8_t* xhw_rom_resident(void) { return NULL; }
void xrc_learn_dma(uint32_t rom, uint32_t ram, uint32_t size) { (void)rom; (void)ram; (void)size; }

/* ---- save.c's side ---- */
static uint8_t s_pak[4][XU_PAK_SIZE], s_pak_up[4], s_pak_blank[4];
static int s_pak_dirty[4], s_motor[4];
uint8_t* xu_pak_image(int port) {
    if (!s_pak_up[port]) {
        s_pak_up[port] = 1;
        if (s_pak_blank[port]) memset(s_pak[port], 0, XU_PAK_SIZE);
        else xu_pak_format(s_pak[port]);
    }
    return s_pak[port];
}
void xu_pak_dirty(int port) { s_pak_dirty[port]++; }
void xu_pak_rumble(int port, int on) { s_motor[port] = on; }

static uint8_t s_cart[XU_FLASH_SIZE];
static int s_cart_file = XU_CART_UNKNOWN, s_cart_dirty, s_cart_opens;
uint8_t* xu_cart_image(int* kind) {
    if (*kind == XU_CART_UNKNOWN) *kind = s_cart_file == XU_CART_FLASH ? XU_CART_FLASH : XU_CART_SRAM;
    memset(s_cart, 0xFF, sizeof s_cart);
    s_cart_opens++;
    return s_cart;
}
void xu_cart_dirty(void) { s_cart_dirty++; }

/* ---- calling the runtime's libultra ---- */
#define STACK 0x80300000u
static int32_t call(void (*f)(uint8_t*, recomp_context*), const uint32_t* a, int n) {
    recomp_context c;
    int i;
    memset(&c, 0, sizeof c);
    c.r29 = (gpr)(int32_t)STACK;
    if (n > 0) c.r4 = (gpr)(int32_t)a[0];
    if (n > 1) c.r5 = (gpr)(int32_t)a[1];
    if (n > 2) c.r6 = (gpr)(int32_t)a[2];
    if (n > 3) c.r7 = (gpr)(int32_t)a[3];
    for (i = 4; i < n; i++) wr32(STACK + 0x10 + 4 * (uint32_t)(i - 4), a[i]);
    f(NULL, &c);
    return (int32_t)c.r2;
}
#define CALL(f, ...) call(f##_recomp, (const uint32_t[]){__VA_ARGS__}, \
                          (int)(sizeof((const uint32_t[]){__VA_ARGS__}) / sizeof(uint32_t)))
ULTRA(osPfsInitPak); ULTRA(osPfsInit); ULTRA(osPfsChecker); ULTRA(osPfsFreeBlocks); ULTRA(osPfsAllocateFile);
ULTRA(osPfsDeleteFile); ULTRA(osPfsFileState); ULTRA(osPfsFindFile); ULTRA(osPfsReadWriteFile);
ULTRA(osPfsNumFiles); ULTRA(osPfsIsPlug); ULTRA(__osContRamRead); ULTRA(__osContDataCrc);

/* ---- a second formulation of the CRCs (MSB first, no augmentation) ---- */
static uint8_t ref_data_crc(const uint8_t* d) {
    unsigned crc = 0;
    int i, b;
    for (i = 0; i < 32; i++) {
        crc ^= d[i];
        for (b = 0; b < 8; b++) crc = (crc & 0x80) ? ((crc << 1) ^ 0x85) & 0xFF : (crc << 1) & 0xFF;
    }
    return (uint8_t)crc;
}
static uint8_t ref_addr_crc(unsigned block) {
    unsigned crc = 0;
    int i;
    for (i = 10; i >= 0; i--) {
        unsigned fb = ((crc >> 4) & 1) ^ ((block >> i) & 1);
        crc = (crc << 1) & 0x1F;
        if (fb) crc ^= 0x15;
    }
    return (uint8_t)crc;
}

static uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }

/* a PIF read or write as libultra builds it: the address with its CRC */
static int pif_read(int port, unsigned block, uint8_t* out) {
    uint8_t cmd[3] = {0x02, (uint8_t)(block >> 3), (uint8_t)((block << 5) | xu_pak_addr_crc((uint16_t)block))}, resp[33];
    if (xu_pak_pif(port, cmd, 3, resp, 33)) return -1;
    memcpy(out, resp, 32);
    return resp[32] == ref_data_crc(resp) ? 0 : 1;
}
static int pif_write(int port, unsigned block, const uint8_t* in) {
    uint8_t cmd[35], resp[1];
    cmd[0] = 0x03, cmd[1] = (uint8_t)(block >> 3), cmd[2] = (uint8_t)((block << 5) | xu_pak_addr_crc((uint16_t)block));
    memcpy(cmd + 3, in, 32);
    if (xu_pak_pif(port, cmd, 35, resp, 1)) return -1;
    return resp[0] == ref_data_crc(in) ? 0 : 1;
}

static void test_crc(void) {
    uint8_t d[32];
    unsigned b, i, bad = 0;
    for (b = 0; b < 0x800; b++)
        if (xu_pak_addr_crc((uint16_t)b) != ref_addr_crc(b)) bad++;
    CHECK(bad == 0, "%u address CRCs differ", bad);
    CHECK(xu_pak_addr_crc(1) == 0x15, "block 1: %02x", xu_pak_addr_crc(1));
    srand(7);
    for (b = 0, bad = 0; b < 2000; b++) {
        for (i = 0; i < 32; i++) d[i] = (uint8_t)rand();
        if (xu_pak_data_crc(d) != ref_data_crc(d)) bad++;
    }
    CHECK(bad == 0, "%u data CRCs differ", bad);
    memset(d, 0, 32);
    CHECK(xu_pak_data_crc(d) == 0, "zeros: %02x", xu_pak_data_crc(d));
    wr32(0x80100000u, 0x01020304u);   /* through the runtime's __osContDataCrc, from RDRAM */
    memset(d, 0, 32), d[0] = 1, d[1] = 2, d[2] = 3, d[3] = 4;
    for (i = 4; i < 32; i++) wr8(0x80100000u + i, 0);
    CHECK(CALL(__osContDataCrc, 0x80100000u) == ref_data_crc(d), "__osContDataCrc");
}

static void test_format(void) {
    static uint8_t img[XU_PAK_SIZE];
    static const int ids[4] = {1, 3, 4, 6};
    unsigned i, sum = 0, isum = 0;
    xu_pak_format(img);
    for (i = 0; i < 4; i++) CHECK(xu_pak_id_ok(img + 32 * ids[i]), "ID copy in block %d", ids[i]);
    for (i = 0; i < 28; i += 2) sum += be16(img + 32 + i), isum += (uint16_t)~be16(img + 32 + i);
    CHECK(be16(img + 60) == (uint16_t)sum && be16(img + 62) == (uint16_t)isum, "ID sums");
    CHECK(img[32 + 25] & 1, "device ID bit 0 (libultra repairs a pak without it)");
    CHECK(img[32 + 26] == 1, "one bank: %d", img[32 + 26]);
    for (i = 5, sum = 0; i < 128; i++) {
        CHECK(be16(img + 256 + 2 * i) == 3, "page %u free", i);
        sum += img[256 + 2 * i] + img[256 + 2 * i + 1];
    }
    CHECK(img[257] == (uint8_t)sum && img[257] == 0x71, "inode sum %02x", img[257]);
    CHECK(!memcmp(img + 256, img + 512, 256), "the inode mirror");
    for (i = 24 * 32, sum = 0; i < 40 * 32; i++) sum |= img[i];
    CHECK(sum == 0, "an empty directory");
}

static void test_pif(void) {
    uint8_t a[32], b[32];
    int i, d0;
    memset(s_pak_up, 0, sizeof s_pak_up);
    for (i = 0; i < 32; i++) a[i] = (uint8_t)(i * 7 + 1);
    d0 = s_pak_dirty[0];
    CHECK(pif_write(0, 100, a) == 0, "write: the answer's CRC");
    CHECK(s_pak_dirty[0] > d0, "a write marks the pak dirty");
    CHECK(!memcmp(s_pak[0] + 100 * 32, a, 32), "the write landed at block 100");
    CHECK(pif_read(0, 100, b) == 0 && !memcmp(a, b, 32), "read back, CRC good");
    CHECK(pif_read(0, 1, b) == 0 && xu_pak_id_ok(b), "the ID through the PIF");
    /* the bank register: one bank, reads zeros, writes change nothing */
    memset(a, 0, 32);
    CHECK(pif_write(0, 0x400, a) == 0, "bank select");
    CHECK(pif_read(0, 0x400, b) == 0 && b[31] == 0, "0x8000 reads zero");
    {
        uint8_t cmd[3] = {0x02, 0, 0}, resp[33];
        CHECK(xu_pak_pif(0, cmd, 3, resp, 4) == -1, "a read with no room: no answer");
    }
    /* no pak: the CRC comes back wrong (libultra then asks the status) */
    xu_pak_set_kind(2, XU_PAK_NONE);
    CHECK(xu_pak_kind(2) == XU_PAK_NONE, "port 3 empty");
    CHECK(pif_read(2, 1, b) == 1, "no pak: a bad CRC");
}

static void test_rumble(void) {
    uint8_t a[32], b[32];
    /* AUTO, the motor probe first: a Rumble Pak */
    xu_pak_set_kind(1, XU_PAK_AUTO);
    CHECK(xu_pak_kind(1) == XU_PAK_MEMORY, "AUTO answers as a Controller Pak");
    memset(a, 0xFE, 32);
    pif_write(1, 0x400, a);
    CHECK(xu_pak_kind(1) == XU_PAK_RUMBLE, "the 0xFE probe made it a Rumble Pak");
    CHECK(pif_read(1, 0x400, b) == 0 && b[31] == 0x80, "0x8000 reads 0x80: %02x", b[31]);
    memset(a, 1, 32);
    pif_write(1, 0x600, a);
    CHECK(s_motor[1] == 1, "0xC000 <- 1: the motor on");
    memset(a, 0, 32);
    pif_write(1, 0x600, a);
    CHECK(s_motor[1] == 0, "0xC000 <- 0: off");
    /* AUTO, the file system read first: stays a Controller Pak */
    xu_pak_set_kind(0, XU_PAK_AUTO);
    pif_read(0, 1, b);
    memset(a, 0xFE, 32);
    pif_write(0, 0x400, a);
    CHECK(xu_pak_kind(0) == XU_PAK_MEMORY, "a pak whose files were read keeps them");
    CHECK(pif_read(0, 0x400, b) == 0 && b[31] != 0xFE, "and fails the motor probe");
}

#define PFS 0x80200000u
#define NAME 0x80200100u
#define EXT 0x80200120u
#define OUT 0x80200130u
#define BUF 0x80210000u
static void put_name(const char* n, const char* e) {
    int i;
    for (i = 0; i < 16; i++) wr8(NAME + (uint32_t)i, (uint8_t)n[i]);
    for (i = 0; i < 4; i++) wr8(EXT + (uint32_t)i, (uint8_t)e[i]);
}

static void test_pfs(void) {
    const uint32_t mq = 0x80201000u, company = 0x3031, game = 0x4E4B5445;   /* "01", "NKTE" */
    int i, ok;
    memset(s_pak_up, 0, sizeof s_pak_up);
    memset(s_pak_blank, 0, sizeof s_pak_blank);
    xu_pak_set_kind(0, XU_PAK_AUTO);
    CHECK(CALL(osPfsInitPak, mq, PFS, 0) == 0, "osPfsInitPak on a formatted pak");
    CHECK(rd32(PFS + 8) == 0 && rd32(PFS + 4) == mq && rd32(PFS + 96) == 5 && rd32(PFS + 92) == 24 &&
              rd32(PFS + 80) == 16 && rd8(PFS + 100) == 1, "the OSPfs fields");
    CHECK(rd8(PFS + 12 + 26) == 1, "the ID copied into the OSPfs");
    CHECK(CALL(osPfsFreeBlocks, PFS, OUT) == 0 && rd32(OUT) == 123 * 256, "free %u", rd32(OUT));
    put_name("MARIOKART64\0\0\0\0\0", "A\0\0\0");
    CHECK(CALL(osPfsAllocateFile, PFS, company, game, NAME, EXT, 1000, OUT) == 0, "allocate 1000 bytes");
    CHECK(rd32(OUT) == 0, "file 0: %d", (int)rd32(OUT));
    CHECK(CALL(osPfsAllocateFile, PFS, company, game, NAME, EXT, 1000, OUT) == 9, "again: PFS_ERR_EXIST");
    CHECK(CALL(osPfsFreeBlocks, PFS, OUT) == 0 && rd32(OUT) == 119 * 256, "4 pages taken: %u", rd32(OUT));
    for (i = 0; i < 1024; i++) wr8(BUF + (uint32_t)i, (uint8_t)(i ^ 0x5A));
    CHECK(CALL(osPfsReadWriteFile, PFS, 0, 1, 0, 1024, BUF) == 0, "write the file");
    CHECK(CALL(osPfsReadWriteFile, PFS, 0, 1, 1024, 32, BUF) == 5, "past the end: PFS_ERR_INVALID");
    CHECK(CALL(osPfsReadWriteFile, PFS, 0, 0, 0, 30, BUF) == 5, "not whole blocks: PFS_ERR_INVALID");
    for (i = 0; i < 1024; i++) wr8(BUF + (uint32_t)i, 0);
    CHECK(CALL(osPfsReadWriteFile, PFS, 0, 0, 256, 512, BUF + 256) == 0, "read the middle");
    for (i = 256, ok = 1; i < 768; i++) ok &= rd8(BUF + (uint32_t)i) == (uint8_t)(i ^ 0x5A);
    CHECK(ok, "the bytes came back");
    CHECK(s_pak[0][5 * 256 + 256] == (uint8_t)(256 ^ 0x5A), "page 6 holds the file's second page");
    CHECK(CALL(osPfsFileState, PFS, 0, OUT) == 0 && rd32(OUT) == 1024 && rd32(OUT + 4) == game &&
              rd16(OUT + 8) == company && rd8(OUT + 14) == 'M', "osPfsFileState");
    CHECK(CALL(osPfsFindFile, PFS, company, game, NAME, EXT, OUT) == 0 && rd32(OUT) == 0, "found");
    CHECK(CALL(osPfsNumFiles, PFS, OUT, OUT + 4) == 0 && rd32(OUT) == 16 && rd32(OUT + 4) == 1, "one file");
    CHECK(s_pak[0][257] == xu_pak_inode_sum(s_pak[0] + 256, 5) && !memcmp(s_pak[0] + 256, s_pak[0] + 512, 256),
          "the inode sum and mirror after allocating");
    /* the game's own pfs code sees the same: a second init, the PIF read of the directory */
    {
        uint8_t b[32];
        CHECK(pif_read(0, 24, b) == 0 && be16(b + 4) == company && be16(b + 6) == 5, "the entry through the PIF");
    }
    CHECK(CALL(osPfsDeleteFile, PFS, company, game, NAME, EXT) == 0, "delete");
    CHECK(CALL(osPfsFreeBlocks, PFS, OUT) == 0 && rd32(OUT) == 123 * 256, "pages back: %u", rd32(OUT));
    CHECK(CALL(osPfsFindFile, PFS, company, game, NAME, EXT, OUT) == 5 && (int32_t)rd32(OUT) == -1, "gone");
    /* the checker: an entry pointing at a system page goes */
    CALL(osPfsAllocateFile, PFS, company, game, NAME, EXT, 256, OUT);
    s_pak[0][24 * 32 + 7] = 2;   /* start page 2 */
    CHECK(CALL(osPfsChecker, PFS) == 0 && (rd32(PFS) & 2), "the checker fixed something");
    CHECK(CALL(osPfsNumFiles, PFS, OUT, OUT + 4) == 0 && rd32(OUT + 4) == 0, "the broken entry went");
    CHECK(CALL(osPfsFreeBlocks, PFS, OUT) == 0 && rd32(OUT) == 123 * 256, "its page is free again");
    /* a zeroed pak: the ID is repaired; its zero table adds up but holds no
     * free page (as libultra sees it) until the checker runs */
    s_pak_blank[3] = 1;
    xu_pak_set_kind(3, XU_PAK_MEMORY);
    CHECK(CALL(osPfsInitPak, mq, PFS, 3) == 0, "a zeroed pak initialises");
    CHECK(xu_pak_id_ok(s_pak[3] + 32), "its ID repaired");
    CHECK(CALL(osPfsFreeBlocks, PFS, OUT) == 0 && rd32(OUT) == 0, "no free page: %u", rd32(OUT));
    CHECK(CALL(osPfsChecker, PFS) == 0, "the checker");
    CHECK(CALL(osPfsFreeBlocks, PFS, OUT) == 0 && rd32(OUT) == 123 * 256, "then all free: %u", rd32(OUT));
    s_pak[3][257] ^= 0x55, s_pak[3][513] ^= 0x55;
    CHECK(CALL(osPfsInitPak, mq, PFS, 3) == 3, "both tables bad: PFS_ERR_INCONSISTENT");
    CHECK(CALL(osPfsChecker, PFS) == 0, "the checker rebuilds them");
    CHECK(CALL(osPfsInitPak, mq, PFS, 3) == 0, "then it initialises");
    CHECK(CALL(osPfsInit, mq, PFS, 3) == 0, "osPfsInit too");
    /* no pak, the Rumble Pak */
    xu_pak_set_kind(2, XU_PAK_NONE);
    CHECK(CALL(osPfsInitPak, mq, PFS, 2) == 1, "no pak: PFS_ERR_NOPACK");
    xu_pak_set_kind(1, XU_PAK_RUMBLE);
    CHECK(CALL(osPfsInitPak, mq, PFS, 1) == 11, "a Rumble Pak: PFS_ERR_DEVICE");
    CHECK(CALL(osPfsIsPlug, mq, OUT) == 0 && rd8(OUT) == 0xB, "osPfsIsPlug: %x", rd8(OUT));
    CHECK(CALL(__osContRamRead, mq, 0, 1, BUF) == 0 && rd8(BUF + 26) == 1, "__osContRamRead of the ID");
}

static void flash_cmd(uint32_t v) { xu_cart_write_word(XU_CART_BASE + 0x10000, v); }

static void test_flash(void) {
    uint8_t id[8], page[128], back[128];
    int i, ok;
    xu_cart_reset();
    s_cart_file = XU_CART_UNKNOWN;
    /* osFlashInit -> osFlashReadId: status, then the ID by DMA */
    flash_cmd(0xD2000000u);
    CHECK(xu_cart_kind() == XU_CART_FLASH, "a command to 0x08010000 means Flash");
    flash_cmd(0xE1000000u);
    xu_cart_read(XU_CART_BASE, id, 8);
    CHECK(be16(id) == 0x1111 && be16(id + 2) == 0x8001 && be16(id + 4) == 0x00C2 && be16(id + 6) == 0x001D,
          "the silicon ID");
    /* sector erase, the status, its clear */
    flash_cmd(0x4B000000u | 0x80);
    flash_cmd(0x78000000u);
    CHECK((xu_cart_read_word(XU_CART_BASE) & 0xFF) == 0x08, "erase done");
    CHECK((xu_cart_read_word(XU_CART_BASE) & 2) == 0, "never busy");
    flash_cmd(0xD2000000u);
    xu_cart_write_word(XU_CART_BASE, 0);
    CHECK(xu_cart_read_word(XU_CART_BASE) == 0, "cleared");
    /* program page 0x85 (in sector 1) */
    for (i = 0; i < 128; i++) page[i] = (uint8_t)(i * 3);
    flash_cmd(0xB4000000u);
    xu_cart_write(XU_CART_BASE, page, 128);
    flash_cmd(0xB4000000u);   /* NEW_FLASH sends it again: the buffer stays */
    flash_cmd(0xA5000000u | 0x85);
    CHECK((xu_cart_read_word(XU_CART_BASE) & 4) == 4, "program done");
    CHECK(s_cart_dirty > 0, "the Flash is dirty");
    flash_cmd(0xF0000000u);
    xu_cart_read(XU_CART_BASE + 0x85 * 128, back, 128);
    CHECK(!memcmp(page, back, 128), "read array: the page");
    xu_cart_read(XU_CART_BASE + 0x84 * 128, back, 128);
    for (i = 0, ok = 1; i < 128; i++) ok &= back[i] == 0xFF;
    CHECK(ok, "the page before is erased");
    /* programming only clears bits; a sector erase sets them again */
    memset(page, 0x0F, 128);
    flash_cmd(0xB4000000u);
    xu_cart_write(XU_CART_BASE, page, 128);
    flash_cmd(0xA5000000u | 0x85);
    flash_cmd(0xF0000000u);
    xu_cart_read(XU_CART_BASE + 0x85 * 128, back, 128);
    CHECK(back[1] == (3 & 0x0F) && back[100] == ((300 & 0xFF) & 0x0F), "program ANDs: %02x", back[1]);
    flash_cmd(0x4B000000u | 0x85);   /* any page of the sector */
    flash_cmd(0x78000000u);
    flash_cmd(0xF0000000u);
    xu_cart_read(XU_CART_BASE + 0x85 * 128, back, 128);
    CHECK(back[1] == 0xFF, "the sector erased");
    /* through the PI path: page 0x100 (an address past 0x10000 in read-array mode) */
    for (i = 0; i < 128; i++) page[i] = (uint8_t)~i;
    flash_cmd(0xB4000000u);
    xu_cart_write(XU_CART_BASE, page, 128);
    flash_cmd(0xA5000000u | 0x100);
    flash_cmd(0xF0000000u);
    CHECK(xu_pi_save_dma(0xA8000000u + 0x100 * 128, 0x80220000u, 128, 0) == 1, "domain 2 is the save");
    CHECK(rd8(0x80220000u) == 0xFF && rd8(0x80220005u) == (uint8_t)~5, "PI DMA read of page 0x100");
    /* chip erase */
    flash_cmd(0x3C000000u);
    flash_cmd(0x78000000u);
    flash_cmd(0xF0000000u);
    xu_cart_read(XU_CART_BASE + 0x100 * 128, back, 4);
    CHECK(back[0] == 0xFF && back[3] == 0xFF, "the chip erased");
    /* a .fla on the disk: a read first still finds Flash */
    xu_cart_reset();
    s_cart_file = XU_CART_FLASH;
    xu_cart_read(XU_CART_BASE, back, 4);
    CHECK(xu_cart_kind() == XU_CART_FLASH, "the file decides before any command");
}

static void test_sram(void) {
    int i, ok, opens;
    xu_cart_reset();
    s_cart_file = XU_CART_UNKNOWN;
    for (i = 0; i < 64; i++) wr8(0x80230000u + (uint32_t)i, (uint8_t)(i + 0x40));
    CHECK(xu_pi_save_dma(0xA8000101u, 0x80230000u, 64, 1) == 1, "SRAM write by PI DMA");
    CHECK(xu_cart_kind() == XU_CART_SRAM, "no command, no file: SRAM");
    CHECK(s_cart[0x101] == 0x40 && s_cart[0x140] == 0x7F, "the bytes in cartridge order");
    for (i = 0; i < 64; i++) wr8(0x80240000u + (uint32_t)i, 0);
    CHECK(xu_pi_save_dma(0x08008101u, 0x80240000u, 64, 0) == 1, "read back through the 32 KB mirror");
    for (i = 0, ok = 1; i < 64; i++) ok &= rd8(0x80240000u + (uint32_t)i) == (uint8_t)(i + 0x40);
    CHECK(ok, "SRAM round trip");
    CHECK(xu_pi_save_dma(0xB0001000u, 0x80240000u, 4, 0) == 0, "the ROM is not the save");
    xu_cart_write_word(XU_CART_BASE + 0x200, 0xDEADBEEFu);
    CHECK(xu_cart_read_word(XU_CART_BASE + 0x200) == 0xDEADBEEFu, "SRAM words");
    /* a game that turns out to have Flash: the command switches */
    opens = s_cart_opens;
    flash_cmd(0xD2000000u);
    CHECK(xu_cart_kind() == XU_CART_FLASH && s_cart_opens == opens + 1, "SRAM, then a Flash command: Flash");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    xrc_rdram_size = 8u << 20;
    xrc_rdram_host = calloc(1, xrc_rdram_size);
    xrc_rdram = xrc_rdram_host;
    test_crc();
    test_format();
    test_pif();
    test_rumble();
    test_pfs();
    test_flash();
    test_sram();
    printf("test_saves: %d checks, %d failed\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
