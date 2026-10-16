/* test_romcache.c - the ROM page cache's core (PLAN.md 3.5) on a fake file
 * and a fake lazy reservation: chunk boundaries, DMAs spanning chunks, word
 * reads, the LRU and its cap, pins, failed fills and commits, the counters,
 * per-chunk normalisation, and a random run against the whole image.
 *
 * Run by tools/xbox/test_romcache.py. The "ROM" is a synthetic pattern. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xrom.h"
#include "../../xbox/src/hw/xhw_romcache.h"

static int fails;

#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
            fails++;                                                     \
        }                                                                \
    } while (0)

/* ---- the fakes ---- */
typedef struct fake {
    uint8_t* file;           /* the file's bytes, in `order` */
    uint32_t order;
    uint8_t* base;           /* the "reservation" */
    uint32_t span;
    uint8_t committed[XROMC_MAX_CHUNKS];
    uint32_t ncommitted, max_committed;
    uint32_t fills, fail_fill_at, fail_commit;   /* fail the fill of this chunk + 1 (0: none); refuse commits */
    uint64_t clock;
} fake;

static int f_commit(void* user, uint8_t* p, uint32_t n) {
    fake* f = (fake*)user;
    uint32_t i = (uint32_t)(p - f->base) >> XROMC_SHIFT;
    if (f->fail_commit) return 0;
    CHECK(n == XROMC_CHUNK && ((p - f->base) & (XROMC_CHUNK - 1)) == 0);
    CHECK(!f->committed[i]);
    f->committed[i] = 1;
    if (++f->ncommitted > f->max_committed) f->max_committed = f->ncommitted;
    memset(p, 0xA5, n);   /* fresh pages are not the ROM */
    return 1;
}

static void f_decommit(void* user, uint8_t* p, uint32_t n) {
    fake* f = (fake*)user;
    uint32_t i = (uint32_t)(p - f->base) >> XROMC_SHIFT;
    CHECK(f->committed[i]);
    f->committed[i] = 0;
    f->ncommitted--;
    memset(p, 0xCD, n);   /* a stale read shows */
}

static int f_fill(void* user, uint32_t off, uint8_t* p, uint32_t n) {
    fake* f = (fake*)user;
    uint32_t i = off >> XROMC_SHIFT;
    CHECK(f->committed[i]);   /* reads commit first */
    f->fills++;
    f->clock += 10000;        /* 10 ms a fill */
    if (f->fail_fill_at && f->fail_fill_at - 1 == i) return 0;
    memcpy(p, f->file + off, n);
    xrom_normalise(p, n, f->order);
    return 1;
}

static uint64_t f_now(void* user) { return ((fake*)user)->clock; }

/* the sink: bytes to a buffer, pieces recorded */
typedef struct got {
    uint8_t* buf;
    uint32_t pieces, last_at, max_piece, expect_at;
    int order_ok;
} got;

static void sink(void* user, uint32_t at, const uint8_t* src, uint32_t n) {
    got* g = (got*)user;
    if (at != g->expect_at) g->order_ok = 0;
    g->expect_at = at + n;
    memcpy(g->buf + at, src, n);
    g->pieces++;
    if (n > g->max_piece) g->max_piece = n;
}

static uint8_t* s_img;   /* the normalised image, the reference */
static uint8_t s_out[8u << 20];

static int read_ok(xromc* c, uint32_t off, uint32_t n, uint32_t* pieces) {
    got g;
    memset(&g, 0, sizeof g);
    g.buf = s_out;
    g.order_ok = 1;
    if (!xromc_read(c, off, n, sink, &g)) return 0;
    if (pieces) *pieces = g.pieces;
    return g.order_ok && g.expect_at == n && memcmp(s_out, s_img + off, n) == 0;
}

static void check_invariants(xromc* c, fake* f) {
    uint32_t i, held = 0, pinned = 0;
    for (i = 0; i < c->nchunks; i++) {
        CHECK((c->state[i] != XROMC_EMPTY) == f->committed[i]);
        held += c->state[i] != XROMC_EMPTY;
        pinned += c->state[i] == XROMC_PINNED;
    }
    CHECK(held == c->held && held == f->ncommitted && pinned == c->pinned);
    CHECK(c->held <= c->cap);
}

static void setup(xromc* c, fake* f, uint32_t size, uint32_t cap_chunks, uint32_t order) {
    static const xromc_ops ops0 = { f_commit, f_decommit, f_fill, f_now, NULL };
    xromc_ops ops = ops0;
    uint32_t i;
    memset(f, 0, sizeof *f);
    f->span = (size + XROMC_CHUNK - 1) & ~(XROMC_CHUNK - 1);
    f->base = (uint8_t*)malloc(f->span);
    f->file = (uint8_t*)malloc(size);
    f->order = order;
    s_img = (uint8_t*)realloc(s_img, size);
    for (i = 0; i < size; i++) s_img[i] = (uint8_t)((i * 2654435761u) >> 13 ^ i >> 16);
    s_img[0] = 0x80; s_img[1] = 0x37; s_img[2] = 0x12; s_img[3] = 0x40;
    memcpy(f->file, s_img, size);
    xrom_normalise(f->file, size, order);   /* both swaps are their own inverse */
    memset(f->base, 0xEE, f->span);
    ops.user = f;
    CHECK(xromc_init(c, f->base, size, cap_chunks * XROMC_CHUNK, &ops));
}

static void teardown(fake* f) {
    free(f->base);
    free(f->file);
}

static xromc s_c;

int main(void) {
    xromc* c = &s_c;
    fake f;
    uint32_t pieces, i, size;
    xromc_stats w;

    /* ---- chunk boundaries, spans, words (32 MB + a partial chunk, z64) ---- */
    size = (32u << 20) + 0x8000;
    setup(c, &f, size, 4, XROM_ORDER_Z64);
    CHECK(c->nchunks == 513);
    CHECK(read_ok(c, 0x40, 0x100, &pieces) && pieces == 1);
    CHECK(c->window.misses == 1 && c->window.hits == 0);
    CHECK(read_ok(c, 0x1000, 4, &pieces) && pieces == 1);
    CHECK(c->window.hits == 1 && f.fills == 1);
    /* a word right at the end of a chunk, and one across a boundary */
    CHECK(read_ok(c, XROMC_CHUNK - 4, 4, &pieces) && pieces == 1);
    CHECK(read_ok(c, XROMC_CHUNK - 2, 4, &pieces) && pieces == 2);
    /* a DMA over three chunks from an odd offset */
    CHECK(read_ok(c, 3 * XROMC_CHUNK - 3, 2 * XROMC_CHUNK + 7, &pieces) && pieces == 4);
    /* the last, partial chunk; the last byte; one past the end */
    CHECK(read_ok(c, size - 0x9000, 0x9000, &pieces) && pieces == 2);
    CHECK(read_ok(c, size - 1, 1, NULL));
    CHECK(!xromc_read(c, size, 1, sink, NULL));
    CHECK(!xromc_read(c, size - 4, 8, sink, NULL));
    CHECK(!xromc_read(c, 0xFFFFFFF0u, 0x20, sink, NULL));
    CHECK(read_ok(c, 7, 0, &pieces) && pieces == 0);
    check_invariants(c, &f);

    /* ---- the LRU: cap 4; 0..3 held, 0 touched, 4 evicts 1 ---- */
    teardown(&f);
    setup(c, &f, 16u << 20, 4, XROM_ORDER_Z64);
    for (i = 0; i < 4; i++) CHECK(read_ok(c, i * XROMC_CHUNK + 8, 8, NULL));
    CHECK(read_ok(c, 0, 4, NULL));
    CHECK(read_ok(c, 4 * XROMC_CHUNK, 4, NULL));
    CHECK(f.committed[0] && !f.committed[1] && f.committed[2] && f.committed[3] && f.committed[4]);
    CHECK(c->window.evictions == 1);
    CHECK(read_ok(c, 5 * XROMC_CHUNK, 4, NULL));   /* next coldest: 2 */
    CHECK(!f.committed[2] && f.committed[0]);
    /* an evicted chunk comes back with the right bytes, not the stale ones */
    CHECK(read_ok(c, XROMC_CHUNK, XROMC_CHUNK, NULL));
    check_invariants(c, &f);
    /* a DMA bigger than the whole cap: right bytes, the cap held */
    CHECK(read_ok(c, 0x123, 10 * XROMC_CHUNK, &pieces) && pieces == 11);
    check_invariants(c, &f);
    CHECK(f.max_committed <= 4);

    /* ---- pins: at most half the cap; pinned chunks survive anything ---- */
    teardown(&f);
    setup(c, &f, 16u << 20, 8, XROM_ORDER_Z64);
    CHECK(xromc_pin(c, 10 * XROMC_CHUNK + 5, XROMC_CHUNK) == 2);    /* 10 and 11 */
    CHECK(xromc_pin(c, 10 * XROMC_CHUNK, 16) == 0);                 /* already */
    CHECK(xromc_pin(c, 40 * XROMC_CHUNK, 3 * XROMC_CHUNK) == 2);    /* 40, 41, then the limit (4) */
    CHECK(c->pinned == 4 && c->state[42] == XROMC_EMPTY);
    CHECK(xromc_pin(c, 0x7FFFFFF0u, 4) == 0);
    for (i = 0; i < 100; i++) CHECK(read_ok(c, ((i * 7) % 200) * XROMC_CHUNK, 64, NULL));
    CHECK(f.committed[10] && f.committed[11] && f.committed[40] && f.committed[41]);
    CHECK(read_ok(c, 10 * XROMC_CHUNK - 2, XROMC_CHUNK + 4, NULL));
    check_invariants(c, &f);

    /* ---- a failed fill: zeros handed over, nothing held, retried next time ---- */
    teardown(&f);
    setup(c, &f, 16u << 20, 4, XROM_ORDER_Z64);
    f.fail_fill_at = 3 + 1;
    {
        got g;
        memset(&g, 0, sizeof g);
        g.buf = s_out;
        g.order_ok = 1;
        memset(s_out, 0x11, 3 * XROMC_CHUNK);
        CHECK(xromc_read(c, 2 * XROMC_CHUNK, 3 * XROMC_CHUNK, sink, &g));
        CHECK(memcmp(s_out, s_img + 2 * XROMC_CHUNK, XROMC_CHUNK) == 0);
        for (i = 0; i < XROMC_CHUNK && s_out[XROMC_CHUNK + i] == 0; i++) {}
        CHECK(i == XROMC_CHUNK);
        CHECK(memcmp(s_out + 2 * XROMC_CHUNK, s_img + 4 * XROMC_CHUNK, XROMC_CHUNK) == 0);
        CHECK(g.order_ok && g.expect_at == 3 * XROMC_CHUNK);
    }
    CHECK(c->window.failures == 1 && c->state[3] == XROMC_EMPTY);
    check_invariants(c, &f);
    f.fail_fill_at = 0;
    CHECK(read_ok(c, 3 * XROMC_CHUNK, 16, NULL));
    /* no memory at all: zeros, nothing held */
    f.fail_commit = 1;
    memset(s_out, 0x11, 32);
    CHECK(xromc_read(c, 9 * XROMC_CHUNK, 32, sink, &(got){ s_out, 0, 0, 0, 0, 1 }));
    CHECK(s_out[0] == 0 && s_out[31] == 0 && c->state[9] == XROMC_EMPTY);
    f.fail_commit = 0;
    check_invariants(c, &f);

    /* ---- the counters: a window, the stall from the fake clock ---- */
    w = xromc_take_window(c);
    CHECK(w.misses >= 3 && w.failures == 2 && w.stall_us == (uint64_t)(f.fills) * 10000);
    CHECK(c->window.misses == 0 && c->total.misses == w.misses);
    CHECK(read_ok(c, 3 * XROMC_CHUNK, 16, NULL));
    w = xromc_take_window(c);
    CHECK(w.hits == 1 && w.misses == 0 && w.reads == 1 && w.stall_us == 0);

    /* ---- per-chunk normalisation: a v64 and an n64 file ---- */
    teardown(&f);
    setup(c, &f, 8u << 20, 6, XROM_ORDER_V64);
    CHECK(read_ok(c, 0, 0x40, NULL));
    CHECK(read_ok(c, XROMC_CHUNK - 3, 9, NULL));
    teardown(&f);
    setup(c, &f, 8u << 20, 6, XROM_ORDER_N64);
    CHECK(read_ok(c, XROMC_CHUNK - 1, 3, NULL));
    CHECK(read_ok(c, 5 * XROMC_CHUNK + 1, 3 * XROMC_CHUNK, NULL));

    /* ---- random DMAs and word reads against the image ---- */
    teardown(&f);
    size = 24u << 20;
    setup(c, &f, size, 16, XROM_ORDER_Z64);
    xromc_pin(c, 0x200000, 0x30000);
    srand(12345);
    for (i = 0; i < 20000; i++) {
        uint32_t r = (uint32_t)rand() * 32768u + (uint32_t)rand();
        uint32_t off = (i & 1) ? (r % (size / XROMC_CHUNK)) * XROMC_CHUNK + (r % 7) * 4 - 4 : r % size;
        uint32_t n = (i % 3 == 0) ? 4 : 1 + (uint32_t)rand() % (i % 50 == 0 ? (1u << 20) : 0x6000);
        if (off >= size) off = 0;
        if (n > size - off) n = size - off;
        if (!read_ok(c, off, n, NULL)) {
            CHECK(0);
            printf("  at %u: off %x n %x\n", (unsigned)i, (unsigned)off, (unsigned)n);
            break;
        }
    }
    check_invariants(c, &f);
    CHECK(f.max_committed <= 16 && c->pinned == 3);
    printf("  random: %u hits, %u misses, %u evictions\n", (unsigned)c->total.hits, (unsigned)c->total.misses,
           (unsigned)c->total.evictions);

    /* ---- the boot's streamed SHA-1 equals the whole image's ---- */
    {
        xsha1_ctx a, b;
        uint8_t da[20], db[20];
        xsha1_init(&a);
        xsha1_update(&a, s_img, size);
        xsha1_final(&a, da);
        xsha1_init(&b);
        for (i = 0; i < size; i += 256u * 1024u) xsha1_update(&b, s_img + i, 256u * 1024u);
        xsha1_final(&b, db);
        CHECK(memcmp(da, db, 20) == 0);
    }
    teardown(&f);
    free(s_img);

    printf("test_romcache: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
