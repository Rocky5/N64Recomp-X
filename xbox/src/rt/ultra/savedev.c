/* savedev.c - the save devices' protocols (PLAN.md 3.6); see savedev.h.
 *
 * Controller Pak: 32 KB behind the PIF's commands 0x02/0x03. A command
 * carries a 16-bit address, the block number above a 5-bit CRC of it; a read
 * answers 32 bytes and their 8-bit CRC, a write answers the CRC of what it
 * took. Addresses at 0x8000 and up are not memory: libultra writes its bank
 * number there (the 1 Mbit paks; one bank here, so it is ignored, as a 256
 * Kbit pak ignores it) and reads back zeros. A bad address CRC is not
 * checked, as most emulators do not.
 *
 * Rumble Pak: libultra's osMotorInit writes 0xFE to 0x8000 and reads it
 * back (a Controller Pak would show it: "wrong device"), then 0x80: the
 * Rumble Pak reads 0x80 there; 0xC000 takes the motor's on (1) and off (0).
 * A port set to AUTO answers as a Controller Pak until a game writes that
 * 0xFE or 0x80 before it ever read the pak's file system (blocks 1..39):
 * then it is a Rumble Pak until the game restarts. A game that keeps its
 * saves on the pak and probes for rumble later keeps its Controller Pak.
 *
 * SRAM: 32 KB from 0x08000000, read and written by PI DMA (mirrored every
 * 32 KB up to the Flash's command register).
 *
 * FlashRAM (Macronix MX29L1100, 1 Mbit, 128-byte pages, 16 KB sectors):
 * commands are words written to 0x08010000; data goes by DMA at 0x08000000.
 *   0xF0000000 read array: DMA reads the memory
 *   0xE1000000 silicon ID: DMA reads 0x11118001 and the maker word
 *   0xD2000000 status: a word read returns the status (bit 3 erase done,
 *              bit 2 program done, never busy); a word write to the data
 *              address clears it
 *   0x4B000000 | page: the sector holding the page is next to erase
 *   0x3C000000 the whole chip is next to erase
 *   0x78000000 execute that erase (to 0xFF)
 *   0xB4000000 page buffer mode: DMA writes fill the 128-byte buffer
 *   0xA5000000 | page: program the buffer into the page (bits only clear,
 *              as on the chip; an erased page takes the buffer as it is)
 * The maker word says NEW_FLASH, so libultra's page addresses are bytes
 * (OLD_FLASH halves them in read-array mode). Which cartridge save a game
 * has is decided on its first access: a command to the Flash's register,
 * or else the file already on the disk (save.c), or else SRAM. */
#include <string.h>

#include "savedev.h"

/* ---- CRCs ---- */
uint8_t xu_pak_addr_crc(uint16_t block) {
    unsigned crc = 0, a = block & 0x7FFu;
    int i;
    for (i = 0; i < 16; i++) {   /* the 11 address bits, then 5 zeros */
        unsigned top = crc & 0x10;
        crc = ((crc << 1) | ((a & 0x400) ? 1u : 0u)) & 0x1F;
        a <<= 1;
        if (top) crc ^= 0x15;
    }
    return (uint8_t)crc;
}

uint8_t xu_pak_data_crc(const uint8_t* data) {
    unsigned crc = 0;
    int i, b;
    for (i = 0; i <= 32; i++)    /* the 256 data bits, then 8 zeros */
        for (b = 7; b >= 0; b--) {
            unsigned top = crc & 0x80;
            crc = ((crc << 1) | (i < 32 ? (data[i] >> b) & 1u : 0u)) & 0xFF;
            if (top) crc ^= 0x85;
        }
    return (uint8_t)crc;
}

/* ---- the pak's format ---- */
static uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8), p[1] = (uint8_t)v; }

/* the ID's sum and inverted sum over its first 14 halfwords */
static void id_sums(const uint8_t* id, uint16_t* sum, uint16_t* isum) {
    int j;
    *sum = *isum = 0;
    for (j = 0; j < 28; j += 2) {
        *sum = (uint16_t)(*sum + be16(id + j));
        *isum = (uint16_t)(*isum + (uint16_t)~be16(id + j));
    }
}

int xu_pak_id_ok(const uint8_t* id) {
    uint16_t s, is;
    id_sums(id, &s, &is);
    return be16(id + 28) == s && be16(id + 30) == is && (id[25] & 1);
}

/* repaired (all ones), a random word, the serial (left zero), device ID 1
 * (a pak with banks), one bank, version 0, the sums */
void xu_pak_make_id(uint8_t* id, uint32_t random) {
    uint16_t s, is;
    memset(id, 0, 32);
    id[0] = id[1] = id[2] = id[3] = 0xFF;
    id[4] = (uint8_t)(random >> 24), id[5] = (uint8_t)(random >> 16), id[6] = (uint8_t)(random >> 8);
    id[7] = (uint8_t)random;
    put16(id + 24, 1);
    id[26] = 1;
    id_sums(id, &s, &is);
    put16(id + 28, s);
    put16(id + 30, is);
}

/* libultra's __osSumcalc over the inode units from start_page on, kept to
 * the byte that unit 0's page field holds */
uint8_t xu_pak_inode_sum(const uint8_t* page, int start_page) {
    unsigned sum = 0;
    int i;
    for (i = start_page * 2; i < 256; i++) sum += page[i];
    return (uint8_t)sum;
}

void xu_pak_format(uint8_t* img) { xu_pak_format_id(img, 0x4E525821u); }   /* "NRX!" */

void xu_pak_format_id(uint8_t* img, uint32_t random) {
    static const int ids[4] = {1, 3, 4, 6};
    uint8_t* inode = img + 256;
    int i;
    memset(img, 0, XU_PAK_SIZE);
    xu_pak_make_id(img + 32, random);
    for (i = 1; i < 4; i++) memcpy(img + 32 * ids[i], img + 32, 32);
    for (i = 5; i < 128; i++) put16(inode + 2 * i, 3);   /* every data page free (PFS_PAGE_NOT_USED) */
    inode[1] = xu_pak_inode_sum(inode, 5);
    memcpy(img + 512, inode, 256);                       /* the mirror in page 2 */
}

/* ---- the notes, for the BACK menu's Paks page (settings_menu.c) ----
 * A directory entry (32 bytes at block 24 + n): game code +0 (4), company
 * +4 (2), first page +6 (inode unit), status +8, ext name +12 (4), note
 * name +16 (16), both in the pak's character set. Used: a game code and a
 * company (libultra's test). */
char xu_pak_char(uint8_t c) {
    static const char sym[] = "!\"#'*+,-./:=?@";
    if (c == 0x0F) return ' ';
    if (c >= 0x10 && c <= 0x19) return (char)('0' + c - 0x10);
    if (c >= 0x1A && c <= 0x33) return (char)('A' + c - 0x1A);
    if (c >= 0x34 && c <= 0x41) return sym[c - 0x34];
    return c ? '?' : 0;   /* kana and the rest */
}

static int dir_used(const uint8_t* d) { return (d[0] | d[1] | d[2] | d[3]) && be16(d + 4); }

/* a note's pages, following its inode chain; -1 if the chain is broken */
static int note_chain(const uint8_t* img, uint16_t p, uint8_t* pages) {
    int n = 0;
    while (p != 1) {
        if ((p >> 8) || p < 5 || p >= 128 || n >= 123) return -1;
        if (pages) pages[n] = (uint8_t)p;
        n++;
        p = be16(img + 256 + 2 * p);
    }
    return n;
}

static void printable(char* out, const uint8_t* in, int n) {
    int i;
    for (i = 0; i < n; i++) out[i] = in[i] >= 0x20 && in[i] < 0x7F ? (char)in[i] : '?';
    out[n] = 0;
}

int xu_pak_notes(const uint8_t* img, xu_pak_note* out, int* free_pages) {
    int j, i, n = 0;
    if (free_pages) {
        *free_pages = 0;
        for (i = 5; i < 128; i++)
            if (be16(img + 256 + 2 * i) == 3) ++*free_pages;
    }
    for (j = 0; j < 16; j++) {
        const uint8_t* d = img + 32 * (24 + j);
        xu_pak_note* o = out + n;
        int k = 0, c;
        if (!dir_used(d)) continue;
        o->slot = (uint8_t)j;
        o->company = be16(d + 4);
        o->game = ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) | ((uint32_t)d[2] << 8) | d[3];
        c = note_chain(img, be16(d + 6), NULL);
        o->pages = (int16_t)c;
        printable(o->code, d, 4);
        printable(o->code + 4, d + 4, 2);
        for (i = 0; i < 16 && (c = xu_pak_char(d[16 + i])) != 0; i++) o->name[k++] = (char)c;
        while (k > 0 && o->name[k - 1] == ' ') k--;
        if (d[12]) {
            o->name[k++] = '.';
            for (i = 0; i < 4 && (c = xu_pak_char(d[12 + i])) != 0; i++) o->name[k++] = (char)c;
        }
        o->name[k] = 0;
        n++;
    }
    return n;
}

int xu_pak_note_delete(uint8_t* img, int slot) {
    uint8_t pages[128], *d, *in = img + 256;
    int n, i;
    if (slot < 0 || slot >= 16) return -1;
    d = img + 32 * (24 + slot);
    if (!dir_used(d)) return -1;
    n = note_chain(img, be16(d + 6), pages);
    for (i = 0; i < n; i++) put16(in + 2 * pages[i], 3);   /* a broken chain frees nothing: the checker's job */
    in[0] = 0;
    in[1] = xu_pak_inode_sum(in, 5);
    memcpy(img + 512, in, 256);
    memset(d, 0, 32);
    return 0;
}

int xu_pak_note_copy(const uint8_t* src, int slot, uint8_t* dst) {
    uint8_t pages[128], *in = dst + 256, *d = NULL;
    const uint8_t* s;
    int n, i, j, k, prev = -1, free_pages = 0;
    if (slot < 0 || slot >= 16) return -1;
    s = src + 32 * (24 + slot);
    if (!dir_used(s) || (n = note_chain(src, be16(s + 6), pages)) <= 0) return -1;
    for (j = 0; j < 16; j++) {   /* the same note there already (libultra's names: game, company, ext, name) */
        const uint8_t* e = dst + 32 * (24 + j);
        if (dir_used(e) && !memcmp(e, s, 6) && !memcmp(e + 12, s + 12, 20)) return -2;
        if (!d && !dir_used(e)) d = dst + 32 * (24 + j);
    }
    for (i = 5; i < 128; i++)
        if (be16(in + 2 * i) == 3) free_pages++;
    if (!d || free_pages < n) return -3;
    memcpy(d, s, 32);
    for (i = 5, k = 0; i < 128 && k < n; i++) {   /* osPfsAllocateFile's order: the lowest free pages */
        if (be16(in + 2 * i) != 3) continue;
        if (prev < 0) put16(d + 6, (uint16_t)i);
        else put16(in + 2 * prev, (uint16_t)i);
        put16(in + 2 * i, 1);
        memcpy(dst + 256 * i, src + 256 * pages[k++], 256);
        prev = i;
    }
    in[0] = 0;
    in[1] = xu_pak_inode_sum(in, 5);
    memcpy(dst + 512, in, 256);
    return 0;
}

/* ---- the pak's address space ---- */
#ifndef XRC_PAK_BOTH
#define XRC_PAK_BOTH 1
#endif
static int s_kind[4] = {XU_PAK_AUTO, XU_PAK_AUTO, XU_PAK_AUTO, XU_PAK_AUTO};
static uint8_t s_fs_read[4];   /* AUTO: the game has read the file system */
static uint8_t s_pulled[4];    /* swapped since the game last asked the status */

void xu_pak_set_kind(int port, int kind) {
    if (port < 0 || port > 3) return;
    s_kind[port] = kind;
    s_fs_read[port] = 0;
}

void xu_pak_pull(int port) {
    if (port < 0 || port > 3) return;
    s_pulled[port] = 1;
    s_fs_read[port] = 0;
}

/* CONT_CARD_ON (0x01), and CONT_CARD_PULL (0x02) once after a swap: the
 * pad's own flag, which libultra's __osPfsGetStatus turns into
 * PFS_ERR_NEW_PACK. It stays up while the port is empty and goes with the
 * first status that reports a pak. */
uint8_t xu_pak_status(int port) {
    uint8_t st;
    if (xu_pak_kind(port) == XU_PAK_NONE) return 0;
    st = (uint8_t)(s_pulled[port & 3] ? 0x03 : 0x01);
    s_pulled[port & 3] = 0;
    return st;
}

int xu_pak_kind(int port) {
    if (port < 0 || port > 3) return XU_PAK_NONE;
    return s_kind[port] == XU_PAK_AUTO ? XU_PAK_MEMORY : s_kind[port];
}

void xu_pak_access(int port, uint16_t addr, uint8_t* data, int write) {
    int kind;
    addr &= (uint16_t)~0x1Fu;
    if (port < 0 || port > 3) return;
    kind = s_kind[port];
    if (kind == XU_PAK_AUTO && XRC_PAK_BOTH) {
        /* AUTO answers as both: a Controller Pak below 0x8000, a Rumble Pak
         * from there (its probe reads 0x80 back, its motor at 0xC000). All-Star
         * Baseball 2000 probes the motor first and then looks for its saves:
         * as a Rumble Pak from the probe on (the rule before), it found none.
         * DIVERGES: no real pak is both. -DXRC_PAK_BOTH=0: the rule before */
        if (addr >= 0x8000) {
            if (write) {
                if (addr >= 0xC000 && addr < 0xD000) xu_pak_rumble(port, data[31] != 0);
            } else {
                memset(data, addr < 0x9000 ? 0x80 : 0x00, 32);
            }
            return;
        }
        kind = XU_PAK_MEMORY;
    } else if (kind == XU_PAK_AUTO && write && addr == 0x8000 && (data[31] == 0xFE || data[31] == 0x80) &&
               !s_fs_read[port]) {
        kind = s_kind[port] = XU_PAK_RUMBLE;   /* the motor probe, before any file system read */
    }
    if (kind == XU_PAK_RUMBLE) {
        if (write) {
            if (addr >= 0xC000 && addr < 0xD000) xu_pak_rumble(port, data[31] != 0);
        } else {
            memset(data, addr >= 0x8000 && addr < 0x9000 ? 0x80 : 0x00, 32);
        }
        return;
    }
    if (kind == XU_PAK_NONE) {
        if (!write) memset(data, 0, 32);
        return;
    }
    if (addr >= XU_PAK_SIZE) {   /* the bank register: one bank */
        if (!write) memset(data, 0, 32);
        return;
    }
    if (write) {
        memcpy(xu_pak_image(port) + addr, data, 32);
        xu_pak_dirty(port);
    } else {
        if (addr >= 32 && addr < 40 * 32) s_fs_read[port] = 1;
        memcpy(data, xu_pak_image(port) + addr, 32);
    }
}

int xu_pak_pif(int port, const uint8_t* cmd, int tx, uint8_t* resp, int rx) {
    uint16_t addr;
    uint8_t buf[32];
    if (tx < 3) return -1;
    addr = (uint16_t)((cmd[1] << 8) | cmd[2]);
    if (cmd[0] == 0x02 && rx >= 33) {
        xu_pak_access(port, addr, buf, 0);
        memcpy(resp, buf, 32);
        resp[32] = xu_pak_data_crc(buf);
        if (xu_pak_kind(port) == XU_PAK_NONE) resp[32] ^= 0xFF;   /* no pak: the CRC comes back wrong */
        return 0;
    }
    if (cmd[0] == 0x03 && tx >= 35 && rx >= 1) {
        memcpy(buf, cmd + 3, 32);
        xu_pak_access(port, addr, buf, 1);
        resp[0] = xu_pak_data_crc(cmd + 3);
        if (xu_pak_kind(port) == XU_PAK_NONE) resp[0] ^= 0xFF;
        return 0;
    }
    return -1;
}

/* ---- SRAM and FlashRAM ---- */
enum { F_READ, F_STATUS, F_ID, F_ERASE, F_WRITE };
static int s_cart = XU_CART_UNKNOWN, s_fmode = F_READ;
static uint8_t* s_img;
static uint8_t s_status, s_page[128];
static uint32_t s_erase_off, s_erase_len;

int xu_cart_is_save(uint32_t phys) {
    phys &= 0x1FFFFFFFu;
    return phys >= 0x05000000u && phys < 0x10000000u;
}

int xu_cart_kind(void) { return s_cart; }

void xu_cart_reset(void) {
    s_cart = XU_CART_UNKNOWN, s_fmode = F_READ, s_img = 0, s_status = 0;
    s_erase_off = s_erase_len = 0;
}

static void attach(int want) {
    int k = want;
    if (s_cart != XU_CART_UNKNOWN && (want == XU_CART_UNKNOWN || want == s_cart)) return;
    s_img = xu_cart_image(&k);
    s_cart = k;
}

static uint32_t off_of(uint32_t phys) { return (phys & 0x1FFFFFFFu) - XU_CART_BASE; }

static void flash_cmd(uint32_t v) {
    uint32_t page = v & 0xFFFF;
    switch (v >> 24) {
    case 0xF0: s_fmode = F_READ; break;
    case 0xE1: s_fmode = F_ID; break;
    case 0xD2: s_fmode = F_STATUS; break;
    case 0x4B:
        s_fmode = F_ERASE;
        s_erase_off = (page * 128u) & (XU_FLASH_SIZE - 1) & ~0x3FFFu;
        s_erase_len = 0x4000;
        break;
    case 0x3C: s_fmode = F_ERASE, s_erase_off = 0, s_erase_len = XU_FLASH_SIZE; break;
    case 0x78:
        if (s_fmode == F_ERASE && s_erase_len) {
            memset(s_img + s_erase_off, 0xFF, s_erase_len);
            xu_cart_dirty();
            s_erase_len = 0;
        }
        s_status = 0x08, s_fmode = F_STATUS;
        break;
    case 0xB4: s_fmode = F_WRITE; break;
    case 0xA5: {
        uint32_t off = (page * 128u) & (XU_FLASH_SIZE - 1), i;
        for (i = 0; i < 128; i++) s_img[off + i] &= s_page[i];
        xu_cart_dirty();
        s_status = 0x04, s_fmode = F_STATUS;
        break;
    }
    default: break;
    }
}

void xu_cart_read(uint32_t phys, uint8_t* dst, uint32_t n) {
    uint32_t off = off_of(phys), i;
    attach(XU_CART_UNKNOWN);
    if (s_cart == XU_CART_SRAM) {
        for (i = 0; i < n; i++) dst[i] = s_img[(off + i) & (XU_SRAM_SIZE - 1)];
        return;
    }
    memset(dst, 0, n);
    if (s_fmode == F_READ) {
        for (i = 0; i < n; i++) dst[i] = s_img[(off + i) & (XU_FLASH_SIZE - 1)];
    } else if (s_fmode == F_ID || s_fmode == F_STATUS) {
        static const uint8_t id[8] = {XU_FLASH_TYPE >> 24, (XU_FLASH_TYPE >> 16) & 0xFF, (XU_FLASH_TYPE >> 8) & 0xFF,
                                      XU_FLASH_TYPE & 0xFF, XU_FLASH_MAKER >> 24, (XU_FLASH_MAKER >> 16) & 0xFF,
                                      (XU_FLASH_MAKER >> 8) & 0xFF, XU_FLASH_MAKER & 0xFF};
        if (s_fmode == F_ID) memcpy(dst, id, n < 8 ? n : 8);
        else if (n >= 4) dst[3] = s_status;
    }
}

void xu_cart_write(uint32_t phys, const uint8_t* src, uint32_t n) {
    uint32_t off = off_of(phys), i;
    attach(XU_CART_UNKNOWN);
    if (s_cart == XU_CART_SRAM) {
        for (i = 0; i < n; i++) s_img[(off + i) & (XU_SRAM_SIZE - 1)] = src[i];
        xu_cart_dirty();
        return;
    }
    if (s_fmode == F_WRITE)
        for (i = 0; i < n; i++) s_page[(off + i) & 127] = src[i];
}

uint32_t xu_cart_read_word(uint32_t phys) {
    uint8_t b[4];
    attach(XU_CART_UNKNOWN);
    if (s_cart == XU_CART_FLASH && s_fmode != F_ID && s_fmode != F_READ)
        return s_status;   /* status mode (and any mode but the two that read data) */
    xu_cart_read(phys & ~3u, b, 4);
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}

void xu_cart_write_word(uint32_t phys, uint32_t v) {
    uint32_t off = off_of(phys);
    if (off >= 0x10000 && off < 0x20000) {   /* the Flash's command register */
        attach(XU_CART_FLASH);
        if (s_cart == XU_CART_FLASH) flash_cmd(v);
        return;
    }
    attach(XU_CART_UNKNOWN);
    if (s_cart == XU_CART_FLASH) {
        if (s_fmode == F_STATUS) s_status = 0;   /* osFlashClearStatus */
        return;
    }
    {
        uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
        xu_cart_write(phys & ~3u, b, 4);
    }
}
