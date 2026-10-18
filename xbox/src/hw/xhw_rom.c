/* xhw_rom.c - find, load and validate the user's ROM (PLAN.md 3.5; from
 * Wings64-X's, made game-agnostic).
 *
 * The port ships no game data: the ROM is the user's own, inside default.xbe
 * as the section $ROM that n64rx appends and the kernel never loads (read
 * from D:\default.xbe at its raw offset, so it costs no RAM until here), or,
 * for a build without it (n64rx build --rom-beside, or -DXHW_ROM_IN_XBE=0
 * here), a file next to default.xbe.
 * Nothing runs on an image that has not passed the SHA-1 check against the
 * game pack's list (the runtime hands it over as an xrom_target), so a bad
 * dump, another game or another release stops here with a card that says
 * what was found and what is expected.
 *
 * Two halves. The pure functions (detect order, normalise, parse header,
 * check size/header/hash, find a section in an XBE header) call no kernel
 * routines and build on a host with -DXROM_HOST_TEST (test_xbe_rom.py). The
 * Xbox half (find $ROM or the file in D:\, read it in 256 KB
 * pieces under the loading screen's bar, hash, report) is behind
 * #ifndef XROM_HOST_TEST. A ROM up to XRC_ROM_RESIDENT_MB stays in RAM
 * whole; a bigger one is only streamed through the hash and then served by
 * the page cache (xhw_romcache.c). Either way xhw_rom_read is the one way
 * to its bytes.
 *
 * The extension is only a hint to find the file; the first four bytes decide
 * the byte order (.z64 as is, .v64 swapped pairs, .n64 reversed words) and
 * the image is normalised in RAM to z64, which is what the game reads and
 * what the hash is of. */
#include <stdio.h>
#include <string.h>

#include "xrom.h"

#ifndef XROM_HOST_TEST
#include <stdarg.h>
#include <stdlib.h>
#include <windows.h>

#include "xhw.h"
#include "xhw_internal.h"
#include "xhw_romcache.h"
#endif

/* ---- pure part ---- */

uint32_t xrom_detect_order(const uint8_t b[4]) {
    if (b[0] == 0x80 && b[1] == 0x37 && b[2] == 0x12 && b[3] == 0x40) return XROM_ORDER_Z64;
    if (b[0] == 0x37 && b[1] == 0x80 && b[2] == 0x40 && b[3] == 0x12) return XROM_ORDER_V64;
    if (b[0] == 0x40 && b[1] == 0x12 && b[2] == 0x37 && b[3] == 0x80) return XROM_ORDER_N64;
    return XROM_ORDER_UNKNOWN;
}

void xrom_normalise(uint8_t* rom, uint32_t size, uint32_t order) {
    uint32_t i;
    uint8_t t;
    if (order == XROM_ORDER_V64) {
        for (i = 0; i + 1 < size; i += 2) {
            t = rom[i]; rom[i] = rom[i + 1]; rom[i + 1] = t;
        }
    } else if (order == XROM_ORDER_N64) {
        for (i = 0; i + 3 < size; i += 4) {
            t = rom[i]; rom[i] = rom[i + 3]; rom[i + 3] = t;
            t = rom[i + 1]; rom[i + 1] = rom[i + 2]; rom[i + 2] = t;
        }
    }
}

static uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* A byte for a message: printable ASCII as is, anything else a dot. */
static char printable(uint8_t c) { return c >= 0x20 && c < 0x7F ? (char)c : '.'; }

void xrom_parse_header(const uint8_t* rom, xrom_info* out) {
    int i, n;
    out->crc1 = be32(rom + 0x10);
    out->crc2 = be32(rom + 0x14);
    for (i = 0; i < 20; i++) out->title[i] = printable(rom[0x20 + i]);
    for (n = 20; n > 0 && (out->title[n - 1] == ' ' || out->title[n - 1] == '.'); n--) {}
    out->title[n] = '\0';
    for (i = 0; i < 4; i++) out->code[i] = printable(rom[0x3B + i]);
    out->code[4] = '\0';
    out->region = rom[0x3E];
    out->version = rom[0x3F];
}

const char* xrom_verdict_name(uint32_t v) {
    switch (v) {
    case XROM_OK: return "ok";
    case XROM_NO_ROM: return "no ROM";
    case XROM_BAD_SIZE: return "size";
    case XROM_BAD_HEADER: return "bad header";
    case XROM_WRONG_GAME: return "wrong game";
    case XROM_WRONG_REGION: return "region";
    case XROM_BAD_HASH: return "bad SHA-1";
    case XROM_UNREADABLE: return "unreadable";
    }
    return "unknown";
}

static uint32_t fail(xrom_info* info, uint32_t verdict) {
    info->verdict = verdict;
    return verdict;
}

uint32_t xrom_check_size(xrom_info* info, const xrom_target* t, uint32_t size) {
    info->size = size;
    if (t->size ? size == t->size : size >= 0x100000u && size <= 0x4000000u) return XROM_OK;
    if (t->size)
        snprintf(info->message, sizeof info->message, "This is not a %s ROM (size %u bytes; expected %u MB).",
                 t->title, (unsigned)size, (unsigned)(t->size >> 20));
    else
        snprintf(info->message, sizeof info->message, "This file's size (%u bytes) is not an N64 ROM's.",
                 (unsigned)size);
    return fail(info, XROM_BAD_SIZE);
}

uint32_t xrom_check_header(xrom_info* info, const xrom_target* t) {
    if (info->order == XROM_ORDER_UNKNOWN) {
        snprintf(info->message, sizeof info->message, "Not an N64 ROM (bad header).");
        return fail(info, XROM_BAD_HEADER);
    }
    if (!t->code[0] || strcmp(info->code, t->code) == 0) return XROM_OK;
    if (strncmp(info->code, t->code, 3) == 0) {
        snprintf(info->message, sizeof info->message,
                 "This %s is release %s; this build is made from %s. Use the ROM you built it from.", t->title,
                 info->code, t->code);
        return fail(info, XROM_WRONG_REGION);
    }
    snprintf(info->message, sizeof info->message, "This ROM is not %s (\"%s\", game code %s).", t->title,
             info->title, info->code);
    return fail(info, XROM_WRONG_GAME);
}

uint32_t xrom_check_hash(xrom_info* info, const xrom_target* t) {
    uint32_t i;
    xsha1_hex(info->sha1, info->sha1_hex);
    for (i = 0; i < t->nsha1 && i < XROM_MAX_SHA1; i++) {
        if (strcmp(t->sha1[i], info->sha1_hex) == 0) {
            snprintf(info->label, sizeof info->label, "%s", t->code);
            info->message[0] = '\0';
            info->verdict = XROM_OK;
            return XROM_OK;
        }
    }
    snprintf(info->message, sizeof info->message,
             "Bad or modified dump: SHA-1 is %s, expected %s. Use the ROM this build was made from.",
             info->sha1_hex, t->nsha1 ? t->sha1[0] : "(none)");
    return fail(info, XROM_BAD_HASH);
}

uint32_t xrom_verify(uint8_t* buf, uint32_t size, xrom_info* info, const xrom_target* t) {
    xsha1_ctx c;
    memset(info, 0, sizeof *info);
    if (xrom_check_size(info, t, size) != XROM_OK) return info->verdict;
    info->order = xrom_detect_order(buf);
    if (info->order == XROM_ORDER_UNKNOWN) return xrom_check_header(info, t);
    xrom_normalise(buf, size, info->order);
    xrom_parse_header(buf, info);
    if (xrom_check_header(info, t) != XROM_OK) return info->verdict;
    xsha1_init(&c);
    xsha1_update(&c, buf, size);
    xsha1_final(&c, info->sha1);
    return xrom_check_hash(info, t);
}

static uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint32_t xrom_xbe_section(const uint8_t* hdr, uint32_t len, const char* name, uint32_t* raw, uint32_t* size) {
    uint32_t base, nsec, tab, i, n = (uint32_t)strlen(name);
    if (len < 0x178 || len > 0x10000 || memcmp(hdr, "XBEH", 4) != 0) return 0;
    base = le32(hdr + 0x104);
    nsec = le32(hdr + 0x11C);
    tab = le32(hdr + 0x120) - base;
    if (nsec > 256 || tab >= len || nsec * 0x38u > len - tab) return 0;
    for (i = 0; i < nsec; i++) {
        const uint8_t* s = hdr + tab + i * 0x38u;
        uint32_t at = le32(s + 20) - base;
        if (at >= len || n + 1 > len - at || memcmp(hdr + at, name, n + 1) != 0) continue;
        *raw = le32(s + 12);
        *size = le32(s + 16);
        return 1;
    }
    return 0;
}

/* ---- Xbox part ---- */
#ifndef XROM_HOST_TEST

#ifndef XHW_ROM_IN_XBE
#define XHW_ROM_IN_XBE 1   /* 0: ignore $ROM, read a ROM beside default.xbe only */
#endif

/* PLAN.md 3.5: resident up to this many MB (0: the page cache for every
 * ROM, a test of the cache on a small one), the page cache over it */
#ifndef XRC_ROM_RESIDENT_MB
#define XRC_ROM_RESIDENT_MB 16
#endif
#ifndef XRC_ROM_CACHE_MB
#define XRC_ROM_CACHE_MB 16       /* the page cache's LRU cap (PLAN.md 3.10) */
#endif
#ifndef XRC_ROM_CACHE_720P_MB
#define XRC_ROM_CACHE_720P_MB 12  /* the same when the boot predicts 720p (the bigger framebuffers) */
#endif

#define CHUNK (256u * 1024u)
#define XBE_HDR ((const uint8_t*)0x00010000)   /* where the kernel maps the image header */
#define CARD_LINES 8
#define CARD_COLS 70   /* the card's lines: the loading screen's safe width */

static xrom_info s_info;
static xrom_target s_target;
static int s_inside;   /* the ROM is default.xbe's $ROM section */
static const uint8_t* s_resident;   /* the image in RAM, or NULL: the page cache */
static uint32_t s_size;

const xrom_info* xhw_rom_info(void) { return &s_info; }
uint32_t xhw_rom_size(void) { return s_size; }
const uint8_t* xhw_rom_resident(void) { return s_resident; }

int xhw_rom_read(uint32_t off, uint32_t n, xrom_sink sink, void* user) {
    if (s_resident) {
        if (off >= s_size || n > s_size - off) return 0;
        if (n) sink(user, 0, s_resident + off, n);
        return 1;
    }
    return s_size ? xhw_romcache_read(off, n, sink, user) : 0;
}

uint32_t xhw_rom_pin(uint32_t off, uint32_t len) { return s_resident || !s_size ? 0 : xhw_romcache_pin(off, len); }

/* Extension (after the last dot) is one of the accepted ones, any case. */
static int accepted_ext(const char* name) {
    static const char* const ext[] = { "z64", "n64", "v64", "rom", "bin" };
    const char* dot = strrchr(name, '.');
    size_t i;
    if (!dot) return 0;
    for (i = 0; i < sizeof ext / sizeof ext[0]; i++)
        if (_stricmp(dot + 1, ext[i]) == 0) return 1;
    return 0;
}

/* Files of ours that could carry an accepted extension but aren't a ROM */
static int ours(const char* name) {
    return _strnicmp(name, "default", 7) == 0 || _strnicmp(name, "settings", 8) == 0 ||
           _strnicmp(name, "boot", 4) == 0 || _strnicmp(name, "autopad", 7) == 0;
}

/* The card's text, wrapped to the screen */
typedef struct card {
    char text[CARD_LINES][CARD_COLS + 8];
    const char* ptr[CARD_LINES + 1];
    int n;
} card;

static void card_line(card* c, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void card_line(card* c, const char* fmt, ...) {
    va_list ap;
    if (c->n >= CARD_LINES) return;
    va_start(ap, fmt);
    vsnprintf(c->text[c->n], sizeof c->text[0], fmt, ap);
    va_end(ap);
    c->ptr[c->n] = c->text[c->n];
    c->n++;
    c->ptr[c->n] = NULL;
}

/* Words of `s` to lines of at most CARD_COLS columns; a word longer than that is cut. */
static void card_wrap(card* c, const char* s) {
    while (*s) {
        int len = (int)strlen(s), cut = len;
        if (len > CARD_COLS) {
            for (cut = CARD_COLS; cut > 0 && s[cut] != ' '; cut--) {}
            if (cut == 0) cut = CARD_COLS;
        }
        card_line(c, "%.*s", cut, s);
        s += cut;
        while (*s == ' ') s++;
    }
}

static const char* order_name(uint32_t o) {
    return o == XROM_ORDER_Z64 ? "z64" : o == XROM_ORDER_V64 ? "v64" : o == XROM_ORDER_N64 ? "n64" : "unknown";
}

static void __attribute__((noreturn)) no_rom_card(void) {
    card c;
    xhw_logf("[ROM] no ROM found in %s", xhw_game_dir());
    xhw_exit_reason("ROM check failed (no ROM)");
    c.n = 0;
    card_line(&c, "This needs your own copy of %s (%s),", s_target.title, s_target.code);
    card_line(&c, "in the same folder as default.xbe: this build has none inside it");
    card_line(&c, "(n64rx build puts it in default.xbe; --rom-beside next to it).");
    card_line(&c, "");
    card_line(&c, "Any file name works: .z64 .n64 .v64 .rom .bin");
    card_line(&c, "Nothing is bundled with this build.");
    xhw_error_card("No ROM found", c.ptr, 30);
}

/* Log the failure, put the reason on the card and leave. */
static void __attribute__((noreturn)) fail_card(const xrom_info* info, const char* title) {
    card c;
    char reason[96];
    xhw_logf("[ROM] FAILED: %s", info->message);
    snprintf(reason, sizeof reason, "ROM check failed (%s)", xrom_verdict_name(info->verdict));
    xhw_exit_reason(reason);
    c.n = 0;
    card_wrap(&c, info->message);
    card_line(&c, "");
    card_line(&c, "File: D:\\%s%s (%u bytes, %s)", info->file, s_inside ? ", " XROM_XBE_SECTION : "",
              (unsigned)info->size, order_name(info->order));
    if (info->code[0])
        card_line(&c, "Title \"%s\", code %s, version %u", info->title, info->code, (unsigned)info->version);
    if (info->verdict == XROM_BAD_HASH) {
        card_line(&c, "SHA-1 %s (yours)", info->sha1_hex);
        card_line(&c, "SHA-1 %s (expected)", s_target.nsha1 ? s_target.sha1[0] : "");
    }
    xhw_error_card(title, c.ptr, 30);
}

/* test builds: a file on D: by name (MX_ROM), ahead of $ROM. 1 if set. */
static int forced_rom(char* name, int cap) {
#if XHW_TEST_BUILD
    const char* forced = getenv("MX_ROM");
    if (forced && *forced) {
        snprintf(name, (size_t)cap, "%s", forced);
        return 1;
    }
#endif
    (void)name;
    (void)cap;
    return 0;
}

/* $ROM in the image header the kernel mapped: 1 and its place in default.xbe */
static int rom_in_xbe(uint32_t* raw, uint32_t* size) {
#if XHW_ROM_IN_XBE
    return (int)xrom_xbe_section(XBE_HDR, le32(XBE_HDR + 0x108), XROM_XBE_SECTION, raw, size);
#else
    (void)raw;
    (void)size;
    return 0;
#endif
}

/* First file in D:\ with an accepted extension. 1 if found. */
static int find_rom(char* name, int cap) {
    xhw_dir_entry e;
    void* h;
    int ok;
    h = xhw_dir_first("D:\\*", &e);
    if (!h) return 0;
    for (ok = 1; ok;) {
        if (!e.is_dir && accepted_ext(e.name) && !ours(e.name)) {
            snprintf(name, (size_t)cap, "%s", e.name);
            FindClose((HANDLE)h);
            return 1;
        }
        ok = xhw_dir_next(h, &e);   /* closes the handle when the list ends */
    }
    return 0;
}

static void __attribute__((noreturn)) read_failed(HANDLE h, uint32_t done) {
    snprintf(s_info.message, sizeof s_info.message, "Read error after %u bytes (error %u).", (unsigned)done,
             (unsigned)GetLastError());
    CloseHandle(h);
    fail(&s_info, XROM_UNREADABLE);
    fail_card(&s_info, "ROM can't be read");
}

static void log_header(const xrom_info* info) {
    xhw_logf("[ROM] title \"%s\" code %s region %c v%u crc 0x%08x 0x%08x order %s", info->title, info->code,
             (char)info->region, (unsigned)info->version, (unsigned)info->crc1, (unsigned)info->crc2,
             order_name(info->order));
}

/* the title card's chips (PLAN.md 5.5): the header's code, region and
 * revision; the size and where the ROM is */
static void rom_chips(const xrom_info* info) {
    static const struct { char c; const char* name; } k_region[] = {
        { 'E', "USA" }, { 'J', "Japan" }, { 'P', "Europe" }, { 'U', "Australia" }, { 'D', "Germany" },
        { 'F', "France" }, { 'I', "Italy" }, { 'S', "Spain" }, { 'X', "Europe" }, { 'Y', "Europe" },
        { 'N', "Canada" }, { 'A', "Asia" }, { 'C', "China" }, { 'K', "Korea" }, { 'B', "Brazil" },
    };
    const char* rn = NULL;
    char id[40], where[40];
    size_t i;
    for (i = 0; i < sizeof k_region / sizeof k_region[0]; i++)
        if (k_region[i].c == (char)info->region) rn = k_region[i].name;
    if (rn) snprintf(id, sizeof id, "%s Â· %s Â· 1.%u", info->code, rn, (unsigned)info->version);
    else snprintf(id, sizeof id, "%s Â· 1.%u", info->code, (unsigned)info->version);
    snprintf(where, sizeof where, "%u MB Â· %s", (unsigned)((info->size + (1u << 20) - 1) >> 20),
             s_inside ? "built into this XBE" : "beside this XBE");
    xhw_splash_rom(id, where, 0);
}

/* the SHA-1 in s_info against the pack's: a card, or the verified line */
static void hash_verdict(void) {
    xrom_info* info = &s_info;
    if (xrom_check_hash(info, &s_target) != XROM_OK) {
        xhw_logf("[ROM] SHA-1 %s", info->sha1_hex);
        if (s_inside)   /* the build checked it on the PC: the copy went wrong */
            snprintf(info->message, sizeof info->message,
                     "The ROM inside default.xbe is damaged (SHA-1 is %s, expected %s). Copy default.xbe again.",
                     info->sha1_hex, s_target.nsha1 ? s_target.sha1[0] : "(none)");
        fail_card(info, "Bad or modified ROM dump");
    }
    xhw_logf("[ROM] verified: %s %s, SHA-1 ok", s_target.title, info->label);
    xhw_splash_rom(NULL, NULL, 1);
}

/* A ROM for the page cache: read once through a small buffer, normalised a
 * piece at a time, hashed, and nothing kept (PLAN.md 3.5). The header check
 * runs on the first piece, before the rest is read. */
static void stream_check(HANDLE h, uint32_t size, xsha1_ctx* sha) {
    xrom_info* info = &s_info;
    uint8_t* buf = (uint8_t*)malloc(CHUNK);
    uint32_t done = 0;
    char stage[64];
    if (!buf) xhw_fatal("Out of memory", "Can't allocate the ROM's read buffer.");
    snprintf(stage, sizeof stage, "Verifying the ROM (%u MB, SHA-1)", (unsigned)((size + (1u << 20) - 1) >> 20));
    xhw_splash_stage(stage);
    xhw_splash_progress(0.0f);
    xsha1_init(sha);
    while (done < size) {
        DWORD got = 0, want = size - done < CHUNK ? size - done : CHUNK;
        if (!ReadFile(h, buf, want, &got, NULL) || got != want) {
            free(buf);
            read_failed(h, done + got);
        }
        if (done == 0) {
            info->order = xrom_detect_order(buf);
            if (info->order == XROM_ORDER_UNKNOWN) {
                free(buf);
                CloseHandle(h);
                xrom_check_header(info, &s_target);
                fail_card(info, "Not an N64 ROM");
            }
            xrom_normalise(buf, got, info->order);
            xrom_parse_header(buf, info);
            log_header(info);
            rom_chips(info);
            if (xrom_check_header(info, &s_target) != XROM_OK) {
                free(buf);
                CloseHandle(h);
                fail_card(info, info->verdict == XROM_WRONG_REGION ? "Another release of the game" : "Not the right game");
            }
        } else {
            xrom_normalise(buf, got, info->order);
        }
        xsha1_update(sha, buf, got);
        done += got;
        xhw_splash_progress((float)done / (float)size);
    }
    free(buf);
}

/* the page cache's cap: XRC_ROM_CACHE_MB, less at 720p */
static uint32_t cache_cap(void) {
    xhw_video_plan p;
    xhw_video_predict(&p);
    return (uint32_t)(strcmp(p.name, "720p") == 0 ? XRC_ROM_CACHE_720P_MB : XRC_ROM_CACHE_MB) << 20;
}

const uint8_t* xhw_rom_load_verified(const xrom_target* target, xrom_info* out) {
    char path[96], stage[64];
    xrom_info* info = &s_info;
    HANDLE h;
    DWORD hi = 0, lo;
    uint8_t* buf;
    uint32_t done = 0, size = 0, raw = 0;
    uint64_t t0, t1, t2;
    xsha1_ctx sha;

    s_target = *target;
    memset(info, 0, sizeof *info);
    s_inside = 0;
    if (!forced_rom(info->file, (int)sizeof info->file)) {
        s_inside = rom_in_xbe(&raw, &size);
        if (s_inside) snprintf(info->file, sizeof info->file, "default.xbe");
        else if (!find_rom(info->file, (int)sizeof info->file)) no_rom_card();
    }
    snprintf(path, sizeof path, "D:\\%s", info->file);
    if (s_inside)
        xhw_logf("[ROM] " XROM_XBE_SECTION " in %s at 0x%x, %u bytes, not preloaded", path, (unsigned)raw,
                 (unsigned)size);
    else
        xhw_logf("[ROM] file %s", path);

    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        snprintf(info->message, sizeof info->message, "The ROM file can't be opened (error %u).",
                 (unsigned)GetLastError());
        fail(info, XROM_UNREADABLE);
        fail_card(info, "ROM can't be read");
    }
    lo = GetFileSize(h, &hi);
    if (s_inside) {
        /* the section must lie inside the file (a copy of the XBE cut short) */
        if (hi == 0 && (lo < raw || size > lo - raw)) {
            snprintf(info->message, sizeof info->message,
                     "default.xbe is cut short (%u bytes; the ROM inside it ends at %u). Copy it again.",
                     (unsigned)lo, (unsigned)(raw + size));
            CloseHandle(h);
            fail(info, XROM_UNREADABLE);
            fail_card(info, "ROM can't be read");
        }
        if (SetFilePointer(h, (LONG)raw, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER) {
            snprintf(info->message, sizeof info->message, "Can't seek to the ROM in default.xbe (error %u).",
                     (unsigned)GetLastError());
            CloseHandle(h);
            fail(info, XROM_UNREADABLE);
            fail_card(info, "ROM can't be read");
        }
        hi = 0;
        lo = size;
    }
    if (hi || xrom_check_size(info, &s_target, lo) != XROM_OK) {
        CloseHandle(h);
        if (hi) xrom_check_size(info, &s_target, 0xFFFFFFFFu);
        fail_card(info, "Not the right ROM");
    }
    size = lo;
    s_size = size;
    if (size > (uint32_t)XRC_ROM_RESIDENT_MB << 20) {
        t0 = xhw_time_us();
        stream_check(h, size, &sha);
        xsha1_final(&sha, info->sha1);
        t1 = xhw_time_us();
        xhw_splash_progress(1.0f);
        xhw_logf("[ROM] %u KB streamed and hashed in %u ms, nothing kept: the page cache (over %u MB)",
                 (unsigned)(size >> 10), (unsigned)((t1 - t0) / 1000), (unsigned)XRC_ROM_RESIDENT_MB);
        hash_verdict();
        /* the handle stays open: the cache's fills read from it */
        if (!xhw_romcache_start(h, s_inside ? raw : 0, size, info->order, cache_cap()))
            xhw_fatal("Out of memory", "No room for the ROM's page cache.");
        xhw_mem_log("rom page cache");
        if (out) *out = *info;
        return NULL;
    }
    buf = (uint8_t*)malloc(size);
    if (!buf) xhw_fatal("Out of memory", "Can't allocate the ROM's buffer.");
    xhw_commit(buf, size);
    snprintf(stage, sizeof stage, s_inside ? "Reading the ROM from this XBE (%u MB)" : "Reading the ROM (%u MB)",
             (unsigned)((size + (1u << 20) - 1) >> 20));
    xhw_splash_stage(stage);
    xhw_splash_progress(0.0f);
    t0 = xhw_time_us();
    while (done < size) {
        DWORD got = 0, want = size - done < CHUNK ? size - done : CHUNK;
        if (!ReadFile(h, buf + done, want, &got, NULL) || got == 0) read_failed(h, done);
        done += got;
        xhw_splash_progress((float)done / (float)size);
    }
    CloseHandle(h);
    t1 = xhw_time_us();

    info->order = xrom_detect_order(buf);
    if (info->order == XROM_ORDER_UNKNOWN) {
        xrom_check_header(info, &s_target);
        fail_card(info, "Not an N64 ROM");
    }
    xrom_normalise(buf, size, info->order);
    xrom_parse_header(buf, info);
    log_header(info);
    rom_chips(info);
    if (xrom_check_header(info, &s_target) != XROM_OK)
        fail_card(info, info->verdict == XROM_WRONG_REGION ? "Another release of the game" : "Not the right game");

    xhw_splash_stage("Verifying the ROM (SHA-1)");
    xsha1_init(&sha);
    for (done = 0; done < size; done += CHUNK) {
        xsha1_update(&sha, buf + done, size - done < CHUNK ? size - done : CHUNK);
        xhw_splash_progress((float)done / (float)size);
    }
    xsha1_final(&sha, info->sha1);
    t2 = xhw_time_us();
    xhw_splash_progress(1.0f);
    xhw_logf("[ROM] loaded %u KB in %u ms, SHA-1 in %u ms", (unsigned)(size >> 10), (unsigned)((t1 - t0) / 1000),
             (unsigned)((t2 - t1) / 1000));
    hash_verdict();
    s_resident = buf;
    if (out) *out = *info;
    return buf;
}

#endif /* XROM_HOST_TEST */
