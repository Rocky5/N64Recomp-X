/* xrom.h - ROM validation (PLAN.md 3.5): what the game pack expects (an
 * xrom_target, from the runtime), the result of looking at the user's image,
 * and the pure functions that produce it.
 *
 * Read by both compile triples (the hw side fills it in, the game side shows
 * it), so it holds only 32-bit scalars and byte arrays: no bit-fields, no
 * 64-bit members, no enums as fields. The functions marked "pure" call
 * nothing but libc string routines and compile on a host with plain C
 * (-DXROM_HOST_TEST). */
#ifndef XROM_H
#define XROM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* byte order of the file as found (the bytes decide, not the extension) */
enum { XROM_ORDER_UNKNOWN = 0, XROM_ORDER_Z64, XROM_ORDER_V64, XROM_ORDER_N64 };

/* how far the check got and why it stopped */
enum {
    XROM_OK = 0,
    XROM_NO_ROM,       /* no file with an accepted extension */
    XROM_BAD_SIZE,     /* not the pack's size */
    XROM_BAD_HEADER,   /* first word is none of the three byte orders */
    XROM_WRONG_GAME,   /* a valid N64 header, another game */
    XROM_WRONG_REGION, /* the same game, another release (the code's last letter differs) */
    XROM_BAD_HASH,     /* the right game code, not a known dump */
    XROM_UNREADABLE    /* the file can't be opened or read */
};

typedef struct xrom_info {
    uint32_t order;       /* XROM_ORDER_* */
    uint32_t verdict;     /* XROM_* */
    uint32_t size;        /* file size in bytes */
    uint32_t region;      /* byte 0x3E: 'E', 'J', 'P' */
    uint32_t version;     /* byte 0x3F */
    uint32_t crc1;        /* header 0x10 */
    uint32_t crc2;        /* header 0x14 */
    char title[24];       /* header 0x20, 20 chars, NUL-terminated, trailing blanks cut */
    char code[8];         /* header 0x3B..0x3E, 4 chars + NUL */
    uint8_t sha1[20];     /* of the normalised image */
    char sha1_hex[44];    /* the same, 40 lowercase hex + NUL */
    char label[24];       /* the release that matched, e.g. "NPWE" */
    char file[64];        /* the file name on D: */
    char message[256];    /* the user-facing reason for a failed verdict */
} xrom_info;

/* ---- what the pack expects (the runtime fills it from xrc_game) ---- */
#define XROM_MAX_SHA1 4
typedef struct xrom_target {
    char title[40];                 /* "Pilotwings 64" */
    char code[8];                   /* "NPWE"; empty: any */
    uint32_t size;                  /* bytes; 0: any N64 size */
    uint32_t nsha1;
    char sha1[XROM_MAX_SHA1][44];   /* 40 lowercase hex of the normalised image */
} xrom_target;

/* ---- SHA-1 (xhw_sha1.c), incremental ---- */
typedef struct xsha1_ctx {
    uint32_t state[5];
    uint32_t count_lo, count_hi;   /* bytes hashed so far */
    uint32_t buflen;
    uint8_t buf[64];
} xsha1_ctx;
void xsha1_init(xsha1_ctx* c);
void xsha1_update(xsha1_ctx* c, const void* data, uint32_t len);
void xsha1_final(xsha1_ctx* c, uint8_t out[20]);
void xsha1_hex(const uint8_t digest[20], char out[41]);

/* ---- pure functions (xhw_rom.c) ---- */
uint32_t xrom_detect_order(const uint8_t first4[4]);
/* In place: v64 swaps every byte pair, n64 reverses every 4 bytes; z64 is left alone. */
void xrom_normalise(uint8_t* rom, uint32_t size, uint32_t order);
/* Title, code, region, version, CRCs from a normalised image of at least 0x40 bytes. */
void xrom_parse_header(const uint8_t* rom, xrom_info* out);
uint32_t xrom_check_size(xrom_info* info, const xrom_target* t, uint32_t size);
uint32_t xrom_check_header(xrom_info* info, const xrom_target* t);
uint32_t xrom_check_hash(xrom_info* info, const xrom_target* t);
/* The whole thing on a file image held in `buf` (normalised in place). */
uint32_t xrom_verify(uint8_t* buf, uint32_t size, xrom_info* info, const xrom_target* t);
/* "size", "bad header", ... for lastexit.txt */
const char* xrom_verdict_name(uint32_t verdict);

/* The ROM inside the XBE (PLAN.md 3.5): n64rx appends it as a section of
 * this name that the kernel never loads (tools/xbox/xbe_sections.py). */
#define XROM_XBE_SECTION "$ROM"
/* Section `name` in an XBE image header: `hdr` holds the file's first `len`
 * bytes (SizeOfHeaders; the kernel maps them at the base address, 0x10000).
 * 1 with its raw offset and size in the file; 0 if it isn't there or the
 * header doesn't hold together. */
uint32_t xrom_xbe_section(const uint8_t* hdr, uint32_t len, const char* name, uint32_t* raw, uint32_t* size);

#ifndef XROM_HOST_TEST
/* ---- the Xbox part (xhw_rom.c): find, load, check ---- */
/* The normalised image in RAM (z64 byte order), or never returns: a card,
 * then the dashboard. The loading screen shows "Reading ROM" and "Checking
 * the ROM (SHA-1)" with their progress. */
const uint8_t* xhw_rom_load_verified(const xrom_target* target, xrom_info* out);
const xrom_info* xhw_rom_info(void);   /* what the last successful load found */
#endif

#ifdef __cplusplus
}
#endif
#endif
