/* pfs.c - the Controller Pak's file system for the replaced osPfs* (PLAN.md
 * 3.3, 3.6).
 *
 * The recompiler's list replaces osPfsInitPak, osPfsAllocateFile and the
 * rest of the seven, and a game whose libultra the rules named also calls
 * the ignored ones (osPfsInit, osPfsIsPlug, osPfsChecker, __osContRamRead
 * ...). They all work here on the same 32 KB image the PIF serves
 * (savedev.c, save.c), in libultra's layout with one bank: the ID in blocks
 * 1, 3, 4 and 6, the label in block 7, the inode table in page 1 (its
 * mirror in page 2: unit n is page n's next page, 1 the end, 3 free; unit
 * 0's low byte the sum of units 5..127), 16 directory entries in pages 3
 * and 4, files from page 5. A game that runs its own (recompiled) osPfs code
 * reaches the same bytes through the PIF, so the two agree.
 *
 * OSPfs (N64 layout): status +0, queue +4, channel +8, id[32] +12,
 * label[32] +44, version +76, dir_size +80, inode_table +84, minode_table
 * +88, dir_table +92, inode_start_page +96, banks +100, activebank +101.
 * OSPfsState: file_size +0, game_code +4, company_code +8, ext_name +10,
 * game_name +14. */
#include "savedev.h"
#include "ultra.h"

enum {
    PFS_ERR_NOPACK = 1, PFS_ERR_NEW_PACK = 2, PFS_ERR_INCONSISTENT = 3, PFS_ERR_INVALID = 5, PFS_DATA_FULL = 7,
    PFS_DIR_FULL = 8, PFS_ERR_EXIST = 9, PFS_ERR_DEVICE = 11,
};
enum { PFS_INITIALIZED = 1, PFS_CORRUPTED = 2 };
#define START 5    /* the first file page */
#define DIRS 16
#define DIR_BLOCK 24
#define EOF_PAGE 1
#define FREE_PAGE 3

static uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8), p[1] = (uint8_t)v; }
static void put32(uint8_t* p, uint32_t v) { put16(p, (uint16_t)(v >> 16)), put16(p + 2, (uint16_t)v); }

static int port_of(uint32_t pfs) { return (int)(rd32(pfs + 8) & 3); }

static int pak_err(int port) {
    int k = XRC_SAVE_PAK ? xu_pak_kind(port) : XU_PAK_NONE;
    return k == XU_PAK_NONE ? PFS_ERR_NOPACK : k == XU_PAK_RUMBLE ? PFS_ERR_DEVICE : 0;
}

/* libultra's __osCheckId: the pak in the port is the one osPfsInitPak saw
 * (the BACK menu's Paks page can swap it, or format it with a new ID);
 * else PFS_ERR_NEW_PACK and the game initialises it again */
static int pak_err_id(int port, uint32_t pfs) {
    uint8_t id[32];
    int r = pak_err(port), same;
    if (r) return r;
    xu_from_rdram(id, pfs + 12, 32);
    xu_save_lock();
    same = !memcmp(id, xu_pak_image(port) + 32, 32);
    xu_save_unlock();
    return same ? 0 : PFS_ERR_NEW_PACK;
}

/* the ID: block 1, else a good copy, else a new one (libultra's repair) */
static void id_fix(int port, uint8_t* img) {
    static const int at[4] = {1, 3, 4, 6};
    uint8_t probe[32];
    int i, good = -1;
    xu_pak_access(port, 32, probe, 0);   /* a file system read, as the game's own code makes */
    for (i = 0; i < 4 && good < 0; i++)
        if (xu_pak_id_ok(img + 32 * at[i])) good = at[i];
    if (good == 1) return;
    if (good < 0) xu_pak_make_id(img + 32, (uint32_t)xu_count64());
    else memcpy(img + 32, img + 32 * good, 32);
    for (i = 1; i < 4; i++) memcpy(img + 32 * at[i], img + 32, 32);
    xu_pak_dirty(port);
}

static uint8_t* inode(uint8_t* img) { return img + 256; }
static int inode_ok(const uint8_t* page) { return page[1] == xu_pak_inode_sum(page, START); }

static void inode_write(int port, uint8_t* img) {
    uint8_t* in = inode(img);
    in[0] = 0;
    in[1] = xu_pak_inode_sum(in, START);
    memcpy(img + 512, in, 256);
    xu_pak_dirty(port);
}

/* the table and its mirror agree and add up; 0 or PFS_ERR_INCONSISTENT */
static int inode_fix(int port, uint8_t* img) {
    if (inode_ok(inode(img))) {
        if (memcmp(img + 512, inode(img), 256)) memcpy(img + 512, inode(img), 256), xu_pak_dirty(port);
        return 0;
    }
    if (inode_ok(img + 512)) {
        memcpy(inode(img), img + 512, 256);
        xu_pak_dirty(port);
        return 0;
    }
    return PFS_ERR_INCONSISTENT;
}

static uint16_t unit(const uint8_t* img, int page) { return be16(img + 256 + 2 * page); }
static void set_unit(uint8_t* img, int page, uint16_t v) { put16(img + 256 + 2 * page, v); }
static uint8_t* dir(uint8_t* img, int j) { return img + 32 * (DIR_BLOCK + j); }
static int dir_used(const uint8_t* d) { return be32(d) != 0 && be16(d + 4) != 0; }

/* a file's pages in order; -1 if the chain is broken */
static int chain(const uint8_t* img, uint16_t start, uint8_t* pages) {
    int n = 0;
    uint16_t p = start;
    while (p != EOF_PAGE) {
        if ((p >> 8) != 0 || (p & 0xFF) < START || (p & 0xFF) >= 128 || n >= 128 - START) return -1;
        pages[n++] = (uint8_t)p;
        p = unit(img, p & 0xFF);
    }
    return n;
}

static void fill_pfs(uint32_t pfs, uint32_t mq, int ch, const uint8_t* img) {
    wr32(pfs + 0, PFS_INITIALIZED);
    wr32(pfs + 4, mq);
    wr32(pfs + 8, (uint32_t)ch);
    xu_to_rdram(pfs + 12, img + 32, 32);
    xu_to_rdram(pfs + 44, img + 7 * 32, 32);
    wr32(pfs + 76, img[32 + 27]);
    wr32(pfs + 80, DIRS);
    wr32(pfs + 84, 8);
    wr32(pfs + 88, 16);
    wr32(pfs + 92, DIR_BLOCK);
    wr32(pfs + 96, START);
    wr8(pfs + 100, 1);
    wr8(pfs + 101, 0);
}

/* osPfsChecker's work: entries with broken chains or shared pages go, every
 * page no file holds is free again; the number of entries removed */
static int check(int port, uint8_t* img) {
    uint8_t owner[128], pages[128], fresh[256];
    int j, i, fixed = 0;
    if (!inode_ok(inode(img)) && inode_ok(img + 512)) memcpy(inode(img), img + 512, 256);
    memset(owner, 0, sizeof owner);
    memset(fresh, 0, sizeof fresh);
    for (i = START; i < 128; i++) put16(fresh + 2 * i, FREE_PAGE);
    for (j = 0; j < DIRS; j++) {
        uint8_t* d = dir(img, j);
        int n, bad = 0;
        if (!dir_used(d)) {
            if (be32(d) || be16(d + 4)) memset(d, 0, 32), fixed++;
            continue;
        }
        n = chain(img, be16(d + 6), pages);
        if (n <= 0) bad = 1;
        for (i = 0; i < n && !bad; i++)
            if (owner[pages[i]]) bad = 1;
        if (bad) {
            memset(d, 0, 32);
            fixed++;
            continue;
        }
        for (i = 0; i < n; i++) {
            owner[pages[i]] = (uint8_t)(j + 1);
            put16(fresh + 2 * pages[i], i + 1 < n ? pages[i + 1] : EOF_PAGE);
        }
    }
    memcpy(inode(img), fresh, 256);
    inode_write(port, img);
    return fixed;
}

static int free_pages(const uint8_t* img) {
    int i, n = 0;
    for (i = START; i < 128; i++)
        if (unit(img, i) == FREE_PAGE) n++;
    return n;
}

/* the entry with these names, or -1 */
static int find(uint8_t* img, uint16_t company, uint32_t game, uint32_t name, uint32_t ext) {
    uint8_t n[16], e[4];
    int j;
    xu_from_rdram(n, name, 16);
    xu_from_rdram(e, ext, 4);
    for (j = 0; j < DIRS; j++) {
        uint8_t* d = dir(img, j);
        if (dir_used(d) && be32(d) == game && be16(d + 4) == company && !memcmp(d + 12, e, 4) && !memcmp(d + 16, n, 16))
            return j;
    }
    return -1;
}

/* osPfsInitPak(mq, pfs, channel) */
static int init_pak(uint32_t mq, uint32_t pfs, int ch, int checker) {
    uint8_t* img;
    int r, port = ch & 3;
    if ((r = pak_err(port)) != 0) return r;
    xu_save_lock();
    img = xu_pak_image(port);
    id_fix(port, img);
    fill_pfs(pfs, mq, ch, img);
    r = inode_fix(port, img);
    if (checker && check(port, img)) wr32(pfs, PFS_INITIALIZED | PFS_CORRUPTED);
    xu_save_unlock();
    return checker ? 0 : r;
}
ULTRA(osPfsInitPak) {
    (void)rdram;
    RET(init_pak(ARG(0), ARG(1), (int)ARG(2), 0));
}
/* osPfsInit(mq, pfs, channel): the older libultras', the checker included */
ULTRA(osPfsInit) {
    (void)rdram;
    RET(init_pak(ARG(0), ARG(1), (int)ARG(2), 1));
}

/* osPfsChecker(pfs) */
ULTRA(osPfsChecker) {
    uint32_t pfs = ARG(0);
    int port = port_of(pfs), r;
    (void)rdram;
    if ((r = pak_err(port)) != 0) {
        RET(r);
        return;
    }
    xu_save_lock();
    id_fix(port, xu_pak_image(port));
    wr32(pfs, check(port, xu_pak_image(port)) ? PFS_INITIALIZED | PFS_CORRUPTED : PFS_INITIALIZED);
    xu_save_unlock();
    RET(0);
}

/* osPfsRepairId(pfs) */
ULTRA(osPfsRepairId) {
    uint32_t pfs = ARG(0);
    int port = port_of(pfs), r;
    (void)rdram;
    if ((r = pak_err(port)) != 0) {
        RET(r);
        return;
    }
    xu_save_lock();
    id_fix(port, xu_pak_image(port));
    xu_to_rdram(pfs + 12, xu_pak_image(port) + 32, 32);
    xu_save_unlock();
    RET(0);
}

/* osPfsIsPlug(mq, u8* pattern): a bit per port with a pak in */
ULTRA(osPfsIsPlug) {
    uint8_t bits = 0;
    int i;
    (void)rdram;
    for (i = 0; i < 4; i++)
        if (XRC_SAVE_PAK && xu_pak_kind(i) != XU_PAK_NONE) bits |= (uint8_t)(1 << i);
    wr8(ARG(1), bits);
    RET(0);
}

/* osPfsNumFiles(pfs, s32* max_files, s32* files_used) */
ULTRA(osPfsNumFiles) {
    int port = port_of(ARG(0)), r, j, used = 0;
    (void)rdram;
    if ((r = pak_err_id(port, ARG(0))) != 0) {
        RET(r);
        return;
    }
    xu_save_lock();
    for (j = 0; j < DIRS; j++)
        if (dir_used(dir(xu_pak_image(port), j))) used++;
    xu_save_unlock();
    wr32(ARG(1), DIRS);
    wr32(ARG(2), (uint32_t)used);
    RET(0);
}

/* osPfsFreeBlocks(pfs, s32* bytes_not_used) */
ULTRA(osPfsFreeBlocks) {
    int port = port_of(ARG(0)), r;
    (void)rdram;
    if ((r = pak_err_id(port, ARG(0))) != 0) {
        RET(r);
        return;
    }
    xu_save_lock();
    r = free_pages(xu_pak_image(port));
    xu_save_unlock();
    wr32(ARG(1), (uint32_t)r * 256u);
    RET(0);
}

/* osPfsFindFile(pfs, u16 company, u32 game, u8* name, u8* ext, s32* file_no) */
ULTRA(osPfsFindFile) {
    int port = port_of(ARG(0)), r, j;
    (void)rdram;
    if ((r = pak_err_id(port, ARG(0))) != 0) {
        RET(r);
        return;
    }
    xu_save_lock();
    j = find(xu_pak_image(port), (uint16_t)ARG(1), ARG(2), ARG(3), ARG(4));
    xu_save_unlock();
    wr32(ARG(5), (uint32_t)j);
    RET(j < 0 ? PFS_ERR_INVALID : 0);
}

/* osPfsAllocateFile(pfs, u16 company, u32 game, u8* name, u8* ext, int bytes, s32* file_no) */
ULTRA(osPfsAllocateFile) {
    uint32_t company = ARG(1) & 0xFFFF, game = ARG(2);
    int32_t bytes = (int32_t)ARG(5);
    int port = port_of(ARG(0)), r, j, need, i, prev = -1, slot = -1;
    uint8_t* img;
    (void)rdram;
    if ((r = pak_err_id(port, ARG(0))) != 0) {
        RET(r);
        return;
    }
    if (!company || !game || bytes <= 0) {
        RET(PFS_ERR_INVALID);
        return;
    }
    need = (bytes + 255) / 256;
    xu_save_lock();
    img = xu_pak_image(port);
    if ((j = find(img, (uint16_t)company, game, ARG(3), ARG(4))) >= 0) {
        xu_save_unlock();
        wr32(ARG(6), (uint32_t)j);
        RET(PFS_ERR_EXIST);
        return;
    }
    for (j = 0; j < DIRS && slot < 0; j++)
        if (!dir_used(dir(img, j))) slot = j;
    r = need > free_pages(img) ? PFS_DATA_FULL : slot < 0 ? PFS_DIR_FULL : 0;
    if (!r) {
        uint8_t* d = dir(img, slot);
        uint16_t first = 0;
        for (i = START; i < 128 && need; i++) {
            if (unit(img, i) != FREE_PAGE) continue;
            if (prev < 0) first = (uint16_t)i;
            else set_unit(img, prev, (uint16_t)i);
            set_unit(img, i, EOF_PAGE);
            prev = i, need--;
        }
        inode_write(port, img);
        memset(d, 0, 32);
        put32(d, game);
        put16(d + 4, (uint16_t)company);
        put16(d + 6, first);
        d[8] = 2;   /* DIR_STATUS_OCCUPIED */
        xu_from_rdram(d + 12, ARG(4), 4);
        xu_from_rdram(d + 16, ARG(3), 16);
        xu_pak_dirty(port);
        wr32(ARG(6), (uint32_t)slot);
    }
    xu_save_unlock();
    RET(r);
}

/* osPfsDeleteFile(pfs, u16 company, u32 game, u8* name, u8* ext) */
ULTRA(osPfsDeleteFile) {
    uint32_t company = ARG(1) & 0xFFFF, game = ARG(2);
    int port = port_of(ARG(0)), r, j, n, i;
    uint8_t pages[128], *img;
    (void)rdram;
    if ((r = pak_err_id(port, ARG(0))) != 0) {
        RET(r);
        return;
    }
    if (!company || !game) {
        RET(PFS_ERR_INVALID);
        return;
    }
    xu_save_lock();
    img = xu_pak_image(port);
    j = find(img, (uint16_t)company, game, ARG(3), ARG(4));
    if (j >= 0) {
        n = chain(img, be16(dir(img, j) + 6), pages);
        for (i = 0; i < n; i++) set_unit(img, pages[i], FREE_PAGE);
        inode_write(port, img);
        memset(dir(img, j), 0, 32);
    }
    xu_save_unlock();
    RET(j < 0 ? PFS_ERR_INVALID : 0);
}

/* osPfsReadWriteFile(pfs, s32 file_no, u8 flag, int offset, int size, u8* buf):
 * whole blocks of 32 bytes inside the file */
ULTRA(osPfsReadWriteFile) {
    int port = port_of(ARG(0)), file = (int)ARG(1), flag = (int)(ARG(2) & 0xFF), r, n;
    int32_t off = (int32_t)ARG(3), size = (int32_t)ARG(4);
    uint32_t buf = ARG(5);
    uint8_t pages[128], *img, *d;
    (void)rdram;
    if ((r = pak_err_id(port, ARG(0))) != 0) {
        RET(r);
        return;
    }
    if (file < 0 || file >= DIRS || (flag != 0 && flag != 1) || size <= 0 || (size & 31) || off < 0 || (off & 31)) {
        RET(PFS_ERR_INVALID);
        return;
    }
    xu_save_lock();
    img = xu_pak_image(port);
    d = dir(img, file);
    n = dir_used(d) ? chain(img, be16(d + 6), pages) : -1;
    if (n < 0 || off + size > n * 256) {
        r = PFS_ERR_INVALID;
    } else {
        while (size > 0) {
            uint32_t at = pages[off / 256] * 256u + (uint32_t)(off % 256);
            if (flag) xu_from_rdram(img + at, buf, 32);
            else xu_to_rdram(buf, img + at, 32);
            off += 32, size -= 32, buf += 32;
        }
        if (flag) {
            d[8] |= 2;
            xu_pak_dirty(port);
        }
    }
    xu_save_unlock();
    RET(r);
}

/* osPfsFileState(pfs, s32 file_no, OSPfsState* state) */
ULTRA(osPfsFileState) {
    int port = port_of(ARG(0)), file = (int)ARG(1), r, n;
    uint32_t st = ARG(2);
    uint8_t pages[128], *img, *d;
    (void)rdram;
    if ((r = pak_err_id(port, ARG(0))) != 0) {
        RET(r);
        return;
    }
    if (file < 0 || file >= DIRS) {
        RET(PFS_ERR_INVALID);
        return;
    }
    xu_save_lock();
    img = xu_pak_image(port);
    d = dir(img, file);
    n = dir_used(d) ? chain(img, be16(d + 6), pages) : -1;
    if (n < 0) {
        r = PFS_ERR_INVALID;
    } else {
        wr32(st + 0, (uint32_t)n * 256u);
        wr32(st + 4, be32(d));
        wr16(st + 8, be16(d + 4));
        xu_to_rdram(st + 10, d + 12, 4);
        xu_to_rdram(st + 14, d + 16, 16);
    }
    xu_save_unlock();
    RET(r);
}

/* osPfsGetLabel(pfs, u8* label, int* len) */
ULTRA(osPfsGetLabel) {
    int port = port_of(ARG(0)), r, n;
    uint8_t label[32];
    (void)rdram;
    if ((r = pak_err_id(port, ARG(0))) != 0) {
        RET(r);
        return;
    }
    xu_save_lock();
    memcpy(label, xu_pak_image(port) + 7 * 32, 32);
    xu_save_unlock();
    for (n = 0; n < 32 && label[n]; n++) {}
    xu_to_rdram(ARG(1), label, 32);
    wr32(ARG(2), (uint32_t)n);
    RET(0);
}

/* ---- libultra's pak primitives, for a game that calls them itself ---- */
/* __osContRamRead(mq, channel, u16 block, u8* buffer) */
ULTRA(__osContRamRead) {
    uint8_t b[32];
    int port = (int)(ARG(1) & 3);
    (void)rdram;
    if (!XRC_SAVE_PAK || xu_pak_kind(port) == XU_PAK_NONE) {
        RET(PFS_ERR_NOPACK);
        return;
    }
    xu_save_lock();
    xu_pak_access(port, (uint16_t)(ARG(2) << 5), b, 0);
    xu_save_unlock();
    xu_to_rdram(ARG(3), b, 32);
    RET(0);
}

/* __osContRamWrite(mq, channel, u16 block, u8* buffer, int force) */
ULTRA(__osContRamWrite) {
    uint8_t b[32];
    int port = (int)(ARG(1) & 3);
    (void)rdram;
    if (!XRC_SAVE_PAK || xu_pak_kind(port) == XU_PAK_NONE) {
        RET(PFS_ERR_NOPACK);
        return;
    }
    xu_from_rdram(b, ARG(3), 32);
    xu_save_lock();
    xu_pak_access(port, (uint16_t)(ARG(2) << 5), b, 1);
    xu_save_unlock();
    RET(0);
}

ULTRA(__osContAddressCrc) {
    (void)rdram;
    RET(xu_pak_addr_crc((uint16_t)ARG(0)));
}

ULTRA(__osContDataCrc) {
    uint8_t b[32];
    (void)rdram;
    xu_from_rdram(b, ARG(0), 32);
    RET(xu_pak_data_crc(b));
}

/* __osPfsGetStatus(mq, channel): 0 with a pak in, PFS_ERR_NOPACK without,
 * PFS_ERR_NEW_PACK once after the BACK menu swapped it */
ULTRA(__osPfsGetStatus) {
    uint8_t st = XRC_SAVE_PAK ? xu_pak_status((int)(ARG(1) & 3)) : 0;
    (void)rdram;
    RET(!(st & 1) ? PFS_ERR_NOPACK : (st & 2) ? PFS_ERR_NEW_PACK : 0);
}
