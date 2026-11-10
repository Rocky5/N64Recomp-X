/* xhw_romcache.h - the ROM page cache's core (PLAN.md 3.5 "page cache").
 *
 * A ROM bigger than the resident limit is reserved (address space only) at
 * its full size and filled 64 KB chunk at a time on first touch; an LRU over
 * a byte cap decommits cold chunks; pinned chunks stay. The core is pure:
 * committing, decommitting and filling are callbacks, so the host test
 * (tools/xbox/test_romcache.py) drives it with a fake file and a fake
 * reservation, and xhw_romcache.c's Xbox half with the kernel's.
 *
 * Not thread-safe by itself: the Xbox half holds its lock around every call. */
#ifndef XHW_ROMCACHE_H
#define XHW_ROMCACHE_H

#include <stdint.h>

#define XROMC_CHUNK (64u * 1024u)
#define XROMC_SHIFT 16
#define XROMC_MAX_CHUNKS 2048u   /* 128 MB: the biggest N64 ROM is 64 MB, a ROM hack's can pass it (5 KB more) */

typedef struct xromc_ops {
    /* make [p, p + n) usable memory (n = one chunk); 1 ok */
    int (*commit)(void* user, uint8_t* p, uint32_t n);
    void (*decommit)(void* user, uint8_t* p, uint32_t n);
    /* the normalised image's bytes [off, off + n) into p, which is committed; 1 ok */
    int (*fill)(void* user, uint32_t off, uint8_t* p, uint32_t n);
    uint64_t (*now_us)(void* user);
    void* user;
} xromc_ops;

/* what a read hands over: `n` bytes of the image at `src`, the `at`-th byte
 * of the request onwards */
typedef void (*xromc_sink)(void* user, uint32_t at, const uint8_t* src, uint32_t n);

typedef struct xromc_stats {
    uint32_t hits, misses;   /* chunk touches */
    uint32_t reads;          /* requests */
    uint32_t fill_kb;        /* read from the file */
    uint32_t evictions;
    uint32_t failures;       /* fills or commits that failed (zeros handed over) */
    uint64_t stall_us;       /* time in commit + fill */
} xromc_stats;

typedef struct xromc {
    uint8_t* base;           /* the reservation: image byte i at base + i */
    uint32_t size;           /* the image */
    uint32_t nchunks;
    uint32_t cap;            /* chunks that may be held (pins included) */
    uint32_t held;           /* chunks committed now */
    uint32_t pinned;
    int16_t prev[XROMC_MAX_CHUNKS], next[XROMC_MAX_CHUNKS];   /* the LRU, most recent at head */
    uint8_t state[XROMC_MAX_CHUNKS];                          /* XROMC_* */
    int16_t head, tail;
    xromc_ops ops;
    xromc_stats total, window;   /* since the start; since the last xromc_take_window */
} xromc;

enum { XROMC_EMPTY = 0, XROMC_HELD = 1, XROMC_PINNED = 2 };

/* `base` reserves at least `size` rounded up to a chunk; `cap_bytes` at
 * least two chunks (a read spanning a chunk boundary). 0 if size is 0 or
 * over 64 MB. */
int xromc_init(xromc* c, uint8_t* base, uint32_t size, uint32_t cap_bytes, const xromc_ops* ops);
/* [off, off + n) of the image to `sink`, a chunk at a time, each chunk
 * touched (filled if needed) just before it is handed over; a chunk that
 * can't be filled goes over as zeros. 0 if the range is outside the image
 * (nothing handed over), 1 otherwise. */
int xromc_read(xromc* c, uint32_t off, uint32_t n, xromc_sink sink, void* user);
/* The chunks under [off, off + len) filled and kept out of the LRU. Pins
 * may take at most half the cap; the rest of a range past that is refused.
 * Returns the chunks pinned by this call. */
uint32_t xromc_pin(xromc* c, uint32_t off, uint32_t len);
/* the window's counters, then a new window */
xromc_stats xromc_take_window(xromc* c);

#ifndef XROM_HOST_TEST
/* the Xbox half: xhw_rom.c starts it on the open file (a HANDLE) once the
 * streamed SHA-1 has passed; the image is file bytes [raw, raw + size) in
 * byte order `order` (XROM_ORDER_*). 0: no address space for it. */
int xhw_romcache_start(void* file, uint32_t raw, uint32_t size, uint32_t order, uint32_t cap_bytes);
int xhw_romcache_read(uint32_t off, uint32_t n, xromc_sink sink, void* user);
uint32_t xhw_romcache_pin(uint32_t off, uint32_t len);
#endif

#endif
