/* xhw_romcache.c - the ROM page cache (PLAN.md 3.5 "page cache", 3.10, M5).
 *
 * A ROM over XRC_ROM_RESIDENT_MB is not read into RAM: its size is reserved
 * (address space, no pages) and 64 KB chunks are committed and filled from
 * the file (default.xbe's $ROM section, or the ROM beside it) when a PI read
 * first touches them. The fill is a ReadFile into a chunk committed just
 * before it, on the reading thread: never a fault inside the kernel's file
 * system (Melee-X's rule: reads commit first). An LRU over the cap
 * (XRC_ROM_CACHE_MB, 16; 12 at 720p) decommits the coldest chunk when a new
 * one needs the room; the pack's pins ([rom] pin) stay held. Every reader of
 * the image goes through xhw_rom_read (xrom.h), resident or not; the cache
 * holds one lock across a read, so a chunk handed over is never evicted
 * under the copy.
 *
 * Two halves, as xhw_rom.c: the core (init, read, pin, the LRU) calls only
 * its callbacks and builds on a host with -DXROM_HOST_TEST
 * (test_romcache.py); the Xbox half (the reservation, the file, the lock,
 * the [PI] counters once a second when there were misses) is behind
 * #ifndef XROM_HOST_TEST. */
#include <string.h>

#include "xhw_romcache.h"

/* ---- core ---- */

static void lru_unlink(xromc* c, int i) {
    int p = c->prev[i], n = c->next[i];
    if (p >= 0) c->next[p] = (int16_t)n;
    else c->head = (int16_t)n;
    if (n >= 0) c->prev[n] = (int16_t)p;
    else c->tail = (int16_t)p;
    c->prev[i] = c->next[i] = -1;
}

static void lru_push_head(xromc* c, int i) {
    c->prev[i] = -1;
    c->next[i] = c->head;
    if (c->head >= 0) c->prev[c->head] = (int16_t)i;
    c->head = (int16_t)i;
    if (c->tail < 0) c->tail = (int16_t)i;
}

int xromc_init(xromc* c, uint8_t* base, uint32_t size, uint32_t cap_bytes, const xromc_ops* ops) {
    uint32_t i;
    memset(c, 0, sizeof *c);
    if (!size || size > XROMC_MAX_CHUNKS * XROMC_CHUNK) return 0;
    c->base = base;
    c->size = size;
    c->nchunks = (size + XROMC_CHUNK - 1) >> XROMC_SHIFT;
    c->cap = cap_bytes >> XROMC_SHIFT;
    if (c->cap < 2) c->cap = 2;
    c->head = c->tail = -1;
    for (i = 0; i < XROMC_MAX_CHUNKS; i++) c->prev[i] = c->next[i] = -1;
    c->ops = *ops;
    return 1;
}

/* the uint32_t counters in xromc_stats' order */
enum { C_HITS, C_MISSES, C_READS, C_FILL_KB, C_EVICTIONS, C_FAILURES };
static void count(xromc* c, int which, uint32_t v) {
    uint32_t* a = &c->total.hits + which;
    uint32_t* b = &c->window.hits + which;
    *a += v;
    *b += v;
}

static void evict_one(xromc* c) {
    int i = c->tail;
    if (i < 0) return;
    lru_unlink(c, i);
    c->state[i] = XROMC_EMPTY;
    c->held--;
    c->ops.decommit(c->ops.user, c->base + ((uint32_t)i << XROMC_SHIFT), XROMC_CHUNK);
    count(c, C_EVICTIONS, 1);
}

/* chunk i held and filled; 0 if it can't be (no memory, a read error) */
static int touch(xromc* c, uint32_t i) {
    uint8_t* p = c->base + (i << XROMC_SHIFT);
    uint32_t off = i << XROMC_SHIFT, n = c->size - off < XROMC_CHUNK ? c->size - off : XROMC_CHUNK;
    uint64_t t0;
    int ok;
    if (c->state[i] != XROMC_EMPTY) {
        count(c, C_HITS, 1);
        if (c->state[i] == XROMC_HELD && c->head != (int)i) {
            lru_unlink(c, (int)i);
            lru_push_head(c, (int)i);
        }
        return 1;
    }
    count(c, C_MISSES, 1);
    while (c->held >= c->cap && c->tail >= 0) evict_one(c);
    t0 = c->ops.now_us(c->ops.user);
    if (!c->ops.commit(c->ops.user, p, XROMC_CHUNK)) {
        count(c, C_FAILURES, 1);
        return 0;
    }
    if (n < XROMC_CHUNK) memset(p + n, 0, XROMC_CHUNK - n);
    ok = c->ops.fill(c->ops.user, off, p, n);
    t0 = c->ops.now_us(c->ops.user) - t0;
    c->window.stall_us += t0;
    c->total.stall_us += t0;
    if (!ok) {
        c->ops.decommit(c->ops.user, p, XROMC_CHUNK);
        count(c, C_FAILURES, 1);
        return 0;
    }
    count(c, C_FILL_KB, n >> 10);
    c->state[i] = XROMC_HELD;
    c->held++;
    lru_push_head(c, (int)i);
    return 1;
}

static const uint8_t s_zero[4096];

int xromc_read(xromc* c, uint32_t off, uint32_t n, xromc_sink sink, void* user) {
    uint32_t at = 0;
    if (off >= c->size || n > c->size - off) return 0;
    count(c, C_READS, 1);
    while (at < n) {
        uint32_t i = (off + at) >> XROMC_SHIFT, in = (off + at) & (XROMC_CHUNK - 1);
        uint32_t take = XROMC_CHUNK - in < n - at ? XROMC_CHUNK - in : n - at;
        if (touch(c, i)) {
            sink(user, at, c->base + off + at, take);
        } else {
            uint32_t z;
            for (z = 0; z < take; z += sizeof s_zero)
                sink(user, at + z, s_zero, take - z < sizeof s_zero ? take - z : (uint32_t)sizeof s_zero);
        }
        at += take;
    }
    return 1;
}

uint32_t xromc_pin(xromc* c, uint32_t off, uint32_t len) {
    uint32_t i, last, got = 0;
    if (!len || off >= c->size) return 0;
    if (len > c->size - off) len = c->size - off;
    last = (off + len - 1) >> XROMC_SHIFT;
    for (i = off >> XROMC_SHIFT; i <= last; i++) {
        if (c->state[i] == XROMC_PINNED) continue;
        if (c->pinned >= c->cap / 2) break;
        if (!touch(c, i)) break;
        lru_unlink(c, (int)i);
        c->state[i] = XROMC_PINNED;
        c->pinned++;
        got++;
    }
    return got;
}

xromc_stats xromc_take_window(xromc* c) {
    xromc_stats w = c->window;
    memset(&c->window, 0, sizeof c->window);
    return w;
}

/* ---- Xbox half ---- */
#ifndef XROM_HOST_TEST
#include <stdio.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include "xhw.h"
#include "xhw_internal.h"
#include "xrom.h"

static xromc s_cache;
static xhw_mutex* s_lock;
static HANDLE s_file = INVALID_HANDLE_VALUE;
static uint32_t s_raw;     /* the image's first byte in the file */
static uint32_t s_order;   /* the file's byte order: each chunk is normalised after its read */
static uint64_t s_window_t0;

static int x_commit(void* user, uint8_t* p, uint32_t n) {
    PVOID a = p;
    SIZE_T s = n;
    (void)user;
    return NT_SUCCESS(NtAllocateVirtualMemory(&a, 0, &s, MEM_COMMIT, PAGE_READWRITE));
}

static void x_decommit(void* user, uint8_t* p, uint32_t n) {
    PVOID a = p;
    SIZE_T s = n;
    (void)user;
    NtFreeVirtualMemory(&a, &s, MEM_DECOMMIT);
}

static int x_fill(void* user, uint32_t off, uint8_t* p, uint32_t n) {
    DWORD got = 0;
    (void)user;
    if (SetFilePointer(s_file, (LONG)(s_raw + off), NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER) return 0;
    if (!ReadFile(s_file, p, n, &got, NULL) || got != n) {
        xhw_logf("[PI] cache: read error at ROM 0x%x (%u of %u bytes, error %u)", (unsigned)off, (unsigned)got,
                 (unsigned)n, (unsigned)GetLastError());
        return 0;
    }
    xrom_normalise(p, n, s_order);
    return 1;
}

static uint64_t x_now(void* user) {
    (void)user;
    return xhw_time_us();
}

int xhw_romcache_start(void* file, uint32_t raw, uint32_t size, uint32_t order, uint32_t cap_bytes) {
    static const xromc_ops ops = { x_commit, x_decommit, x_fill, x_now, NULL };
    PVOID base = NULL;
    SIZE_T span = (size + XROMC_CHUNK - 1) & ~(XROMC_CHUNK - 1);
    if (!NT_SUCCESS(NtAllocateVirtualMemory(&base, 0, &span, MEM_RESERVE, PAGE_READWRITE))) {
        xhw_logf("[PI] cache: can't reserve %u KB of address space", (unsigned)(size >> 10));
        return 0;
    }
    if (!xromc_init(&s_cache, (uint8_t*)base, size, cap_bytes, &ops)) return 0;
    s_lock = xhw_mutex_create();
    s_file = (HANDLE)file;
    s_raw = raw;
    s_order = order;
    s_window_t0 = xhw_time_us();
    xhw_logf("[PI] cache: %u MB ROM reserved at %08x, %u KB chunks, LRU over %u MB", (unsigned)(size >> 20),
             (unsigned)(uintptr_t)base, (unsigned)(XROMC_CHUNK >> 10), (unsigned)(s_cache.cap >> 4));
    return 1;
}

/* the [PI] line: once a second at most, and only for a second with misses */
static void counters(void) {
    uint64_t now = xhw_time_us(), ms = (now - s_window_t0) / 1000;
    xromc_stats w;
    if (ms < 1000) return;
    w = xromc_take_window(&s_cache);
    s_window_t0 = now;
    if (!w.misses && !w.failures) return;
    xhw_logf("[PI] cache: %u reads, %u hits, %u misses (%u KB), stall %u ms in %u ms; %u KB held (%u pinned), "
             "%u evicted%s",
             (unsigned)w.reads, (unsigned)w.hits, (unsigned)w.misses, (unsigned)w.fill_kb,
             (unsigned)(w.stall_us / 1000), (unsigned)ms, (unsigned)(s_cache.held * (XROMC_CHUNK >> 10)),
             (unsigned)(s_cache.pinned * (XROMC_CHUNK >> 10)), (unsigned)w.evictions,
             w.failures ? ", FAILURES" : "");
}

int xhw_romcache_read(uint32_t off, uint32_t n, xromc_sink sink, void* user) {
    int ok;
    xhw_mutex_lock(s_lock);
    ok = xromc_read(&s_cache, off, n, sink, user);
    counters();
    xhw_mutex_unlock(s_lock);
    return ok;
}

uint32_t xhw_romcache_pin(uint32_t off, uint32_t len) {
    uint32_t got;
    xhw_mutex_lock(s_lock);
    got = xromc_pin(&s_cache, off, len);
    xhw_mutex_unlock(s_lock);
    return got;
}

#endif /* XROM_HOST_TEST */
