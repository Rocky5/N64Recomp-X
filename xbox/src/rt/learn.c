/* learn.c - the safety net and the learn log (PLAN.md 3.14 step 5).
 *
 * A call to an address the function table does not know (get_function's
 * miss: a function pointer, a jal the recompiler could not resolve, a
 * function the analyser never found) runs the code there in the
 * interpreter instead of stopping the game, when the address is in RDRAM
 * and holds something. get_function must return a recomp_func_t*, which
 * carries no address, so each such address gets a small x86 stub
 * (push vram; call xrc_safety_run) entered in the table: later lookups are
 * as fast as for any function, any thread may call it, and the stub always
 * interprets what RDRAM holds now (an overlay that replaces the code needs
 * nothing invalidated).
 *
 * Every function that ran this way, and every function it called that the
 * table does not know, is logged once:
 *
 *   [LEARN] func <vram> <bytes> from <caller>
 *   [LEARN] dma <rom offset> <ram> <size>
 *
 * (hex; bytes: the span the run reached, 0 in the line logged as it starts
 * (a thread's entry may never return), then once more when it returns;
 * caller: the function the call came from, or 0) and the same lines without the prefix go to
 * E:\UDATA\<title id>\learn.txt, appended across boots. The dma line is the
 * newest PI DMA that covered the address before it ran (pi.c calls
 * xrc_learn_dma for each; a DMA that continues the last one in ROM and RAM
 * extends it): the analyser takes it as a segment. A DMA or
 * osInvalICache over a logged address logs it again (new code may be
 * there). `n64rx build <rom> --learn learn.txt` feeds the file back
 * (tools/n64rx/n64rx_lib/learn.py) and the next build recompiles them.
 *
 * Kill switch: -DXRC_SAFETY_NET=0 (rt.h) makes a miss the fatal card again. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt.h"
#include "xhw.h"

xhw_thread* xu_spawn(void (*fn)(void*), void* arg, int level, uint32_t stack);   /* ultra/thread.c */

/* addresses the net remembers (each a stub or a logged in-line callee); 512
 * ran out in Paperboy (C++: its virtual functions are reached only through
 * vtables, 521 of them in 20 s), and a lookup past it was the "no function"
 * card. About 52 bytes each. */
#ifndef XRC_MAX_LEARN
#define XRC_MAX_LEARN 4096
#endif
#define MAX_LEARN XRC_MAX_LEARN
#ifndef XRC_STUB_REENTER
#define XRC_STUB_REENTER 1
#endif
#define MAX_DMA 1024         /* the newest PI DMAs, to say where code came from */
/* learn.txt: 48 KB stopped it growing, with no message; Ocarina of Time
 * logs about 70 KB of lines in one boot, so its learning could not converge
 * (docs/review-2026-10.md 5a.5). Twice this in BSS (the text and the
 * writer's copy). -DXRC_LEARN_TEXT_KB=48: as before */
#ifndef XRC_LEARN_TEXT_KB
#define XRC_LEARN_TEXT_KB 160
#endif
#define TEXT_MAX (XRC_LEARN_TEXT_KB * 1024) /* learn.txt */

void xrc_interp_learn(uint8_t* rdram, recomp_context* ctx, uint32_t entry, uint32_t caller);   /* interp.c */
int xrc_ovl_enter(uint32_t vram, recomp_func_t* f);                                             /* ovl.c */
void xrc_ovl_reenter(uint32_t vram, int idx);                                                   /* ovl.c */
uint32_t xrc_ovl_vram_of(uintptr_t host);                                                       /* ovl.c */

typedef struct {
    uint32_t vram, caller, len, logged_len;
    uint8_t logged, stub, said;
    uint16_t slot;   /* its stub's function-table index (ovl.c), to put back */
} Learnt;

typedef struct { uint32_t rom, ram, size; } Dma;

static xhw_mutex* s_lock;
static Learnt s_l[MAX_LEARN];
static unsigned s_nl;
static Dma s_dma[MAX_DMA];
static unsigned s_ndma;
static char s_text[TEXT_MAX];
static unsigned s_tlen;
static int s_text_loaded, s_text_full;

#if defined(__i386__)
#define STUB_BYTES 24
static uint8_t s_stubs[MAX_LEARN][STUB_BYTES] __attribute__((aligned(16)));
#endif

static void lock(void) {
    if (!s_lock) s_lock = xhw_mutex_create();
    xhw_mutex_lock(s_lock);
}
static void unlock(void) { xhw_mutex_unlock(s_lock); }

/* an open-addressed index over s_l (twice its size, never full): vram -> slot + 1 */
#define LEARN_HASH (2 * MAX_LEARN)
static uint16_t s_hash[LEARN_HASH];
static unsigned hash_of(uint32_t vram) { return (unsigned)((vram >> 2) * 2654435761u) % LEARN_HASH; }

static Learnt* find(uint32_t vram) {
    unsigned h = hash_of(vram), n;
    for (n = 0; n < LEARN_HASH && s_hash[h]; n++, h = (h + 1) % LEARN_HASH)
        if (s_l[s_hash[h] - 1].vram == vram) return &s_l[s_hash[h] - 1];
    return NULL;
}

static void index_add(unsigned slot) {
    unsigned h = hash_of(s_l[slot].vram);
    while (s_hash[h]) h = (h + 1) % LEARN_HASH;
    s_hash[h] = (uint16_t)(slot + 1);
}

/* ---- learn.txt ---- */
static void text_load(void) {
    char path[96];
    void* f;
    s_text_loaded = 1;
    snprintf(path, sizeof path, "%slearn.txt", xhw_save_dir());
    if ((f = xhw_file_open(path)) != NULL) {
        uint32_t n = xhw_file_size(f);
        if (n < TEXT_MAX - 1 && xhw_file_read(f, 0, s_text, n)) s_tlen = n;
        else if (n >= TEXT_MAX - 1) s_text_full = 1;   /* larger than the buffer: kept as it is, never overwritten */
        xhw_file_close(f);
    }
    if (s_text_full) xhw_logf("[LEARN] learn.txt is larger than %u KB: kept, nothing added this boot", (unsigned)XRC_LEARN_TEXT_KB);
    if (!s_tlen) {
        static const char head[] = "# n64rx learn file: code this build did not recompile (PLAN.md 3.14).\n"
                                   "# n64rx build <rom> --learn learn.txt recompiles it next time.\n";
        memcpy(s_text, head, sizeof head - 1);
        s_tlen = sizeof head - 1;
    }
    s_text[s_tlen] = 0;
}

/* learn.txt is written by its own thread, at most every LEARN_WRITE_MS,
 * outside every lock. Ocarina of Time on the console (vzel13, 2026-10-18):
 * written at once for each new line (a temp file, flushed, read back,
 * renamed), 2208 lines rewrote up to 48 KB 2208 times on the game's
 * thread; the HDD never stopped and frames took up to 2.6 s. The same line
 * again in one boot (an overlay loaded again) is not logged again either.
 * -DXRC_LEARN_WRITE_MS=0: written at once, every line logged, as before. */
#ifndef XRC_LEARN_WRITE_MS
#define XRC_LEARN_WRITE_MS 3000
#endif
static xhw_mutex* s_text_lock;
static int s_text_dirty;
static uint32_t s_lines[8192];   /* this boot's lines, hashed (0: free) */

static int seen_line(const char* line) {
    uint32_t h = 2166136261u, i;
    const char* c;
    for (c = line; *c; c++) h = (h ^ (uint8_t)*c) * 16777619u;
    h |= 1u;
    for (i = 0; i < 8192; i++) {
        uint32_t* s = &s_lines[(h + i) & 8191];
        if (*s == h) return 1;
        if (!*s) {
            if (i < 64) *s = h;   /* nearly full: logged again rather than probed for long */
            return 0;
        }
    }
    return 0;
}

static void text_write(const char* text, unsigned n) {
    char path[96];
    snprintf(path, sizeof path, "%slearn.txt", xhw_save_dir());
    if (!xhw_write_file_atomic(path, text, n)) xhw_logf("[LEARN] could not write %s", path);
}

static void text_writer(void* arg) {
    static char copy[TEXT_MAX];
    (void)arg;
    for (;;) {
        unsigned n = 0;
        xhw_sleep_ms(XRC_LEARN_WRITE_MS);
        xhw_mutex_lock(s_text_lock);
        if (s_text_dirty) memcpy(copy, s_text, s_tlen), n = s_tlen, s_text_dirty = 0;
        xhw_mutex_unlock(s_text_lock);
        if (n) text_write(copy, n);
    }
}

/* one line, logged and added to the file unless the file has it */
static void emit(const char* line) {
    unsigned n = (unsigned)strlen(line);
    if (!s_text_lock) {   /* two first lines at once: one mutex wins */
        xhw_mutex* m = xhw_mutex_create();
        __sync_val_compare_and_swap(&s_text_lock, (xhw_mutex*)0, m);
    }
    xhw_mutex_lock(s_text_lock);
    if (XRC_LEARN_WRITE_MS && seen_line(line)) {
        xhw_mutex_unlock(s_text_lock);
        return;
    }
    xhw_logf("[LEARN] %s", line);
    if (!s_text_loaded) text_load();
    {   /* the same line from an earlier boot: already there */
        const char* p = s_text;
        while ((p = strstr(p, line)) != NULL) {
            if ((p == s_text || p[-1] == '\n') && p[n] == '\n') {
                xhw_mutex_unlock(s_text_lock);
                return;
            }
            p += n;
        }
    }
    if (s_text_full || s_tlen + n + 2 >= TEXT_MAX) {
        if (!s_text_full) xhw_logf("[LEARN] learn.txt full (%u KB): later lines are not kept", (unsigned)XRC_LEARN_TEXT_KB);
        s_text_full = 1;
        xhw_mutex_unlock(s_text_lock);
        return;
    }
    memcpy(s_text + s_tlen, line, n);
    s_tlen += n;
    s_text[s_tlen++] = '\n';
    s_text[s_tlen] = 0;
    if (!XRC_LEARN_WRITE_MS) {
        text_write(s_text, s_tlen);
    } else if (!s_text_dirty++) {
        static int s_up;
        if (!s_up) s_up = 1, xu_spawn(text_writer, NULL, 6, 16 * 1024);
    }
    xhw_mutex_unlock(s_text_lock);
}

/* ---- code no ROM DMA put there (decompressed: Rare's games, gzip ones) ----
 * The 4 KB page of RDRAM that holds it is saved as it is the first time the
 * net runs code in it: learn_code.bin beside learn.txt (records of vram,
 * size, then the bytes in N64 order; test builds also send it over COM1 as
 * [LEARNCODE] base64), so n64rx --learn can recompile what no ROM holds.
 * -DXRC_LEARN_CODE=0: not captured. */
#ifndef XRC_LEARN_CODE
#define XRC_LEARN_CODE 1
#endif
#define CODE_BLOCK 0x1000u   /* a record's largest size */
static uint8_t s_captured[(8u << 20) / 0x1000u];   /* KSEG0 pages saved */

/* code a game maps with its own TLB and pages in on demand (Conker's
 * 0x15000000): the physical frames behind neighbouring virtual pages are
 * anywhere, so such code is saved a 4 KB page at a time, read through the
 * TLB, keyed by its virtual page */
#define VPAGE 0x1000u
#ifndef XRC_HIGH_SAFETY   /* KSEG2/3 code through the safety net (holds_code) */
#define XRC_HIGH_SAFETY 1
#endif
#define MAX_VPAGES 2048
static uint32_t s_vpages[MAX_VPAGES];
static uint32_t s_vhash[MAX_VPAGES];   /* each captured KUSEG page's bytes, hashed */
static unsigned char s_vmulti[MAX_VPAGES];
static unsigned s_nvpages;

/* GROUP in a record's size word: its pages were saved at one moment (one
 * function's span, xrc_learn_recapture_n), so they are one version of the
 * code there (n64rx's learn.py keeps them together) */
#define GROUP 0x40000000u
static void save_rec(uint32_t base, uint32_t size, uint32_t flags, uint32_t (*word)(uint32_t));
static void save(uint32_t base, uint32_t size, uint32_t (*word)(uint32_t)) { save_rec(base, size, 0, word); }
static void save_rec(uint32_t base, uint32_t size, uint32_t flags, uint32_t (*word)(uint32_t)) {
    /* one record at a time: the three capture paths each had a busy flag but
     * shared this buffer, and a mixed record went into learn_code.bin, which
     * has no framing (docs/review-2026-10.md 5a.6) */
    static uint8_t sbuf[8 + CODE_BLOCK];
    static volatile long s_rec_busy;
    uint8_t* buf = sbuf;
    char path[96], line[64];
    uint32_t i, sz = size | flags;
    while (__sync_lock_test_and_set(&s_rec_busy, 1)) xhw_sleep_ms(0);
    if (size > CODE_BLOCK && (buf = (uint8_t*)malloc(8 + size)) == NULL) {
        __sync_lock_release(&s_rec_busy);
        return;
    }
    for (i = 0; i < 4; i++) {
        buf[i] = (uint8_t)(base >> (24 - 8 * i));
        buf[4 + i] = (uint8_t)(sz >> (24 - 8 * i));
    }
    for (i = 0; i < size; i += 4) {
        uint32_t w = word(base + i);
        buf[8 + i] = (uint8_t)(w >> 24), buf[9 + i] = (uint8_t)(w >> 16);
        buf[10 + i] = (uint8_t)(w >> 8), buf[11 + i] = (uint8_t)w;
    }
    snprintf(path, sizeof path, "%slearn_code.bin", xhw_save_dir());
    if (!xhw_append_file(path, buf, 8 + size)) xhw_logf("[LEARN] could not write %s", path);
    if (XHW_TEST_BUILD) xhw_dump_blob("LEARNCODE", buf, 8 + size);
    if (buf != sbuf) free(buf);
    __sync_lock_release(&s_rec_busy);
    snprintf(line, sizeof line, "code %08x %x%s", (unsigned)base, (unsigned)size, flags & GROUP ? " group" : "");
    emit(line);
}

static uint32_t word_kseg0(uint32_t v) { return xrc_word(0x80000000u | (v & 0x1FFFFFFFu)); }
static uint32_t word_mapped(uint32_t v) { return xrc_word(0x80000000u | xrc_tlb_to_phys(v)); }

static void capture_locked(uint32_t vram);
static void capture(uint32_t vram) {   /* one at a time: two threads' dumps would interleave on COM1 */
    static volatile long s_busy;
    if (!XRC_LEARN_CODE) return;
    while (__sync_lock_test_and_set(&s_busy, 1)) xhw_sleep_ms(0);
    capture_locked(vram);
    __sync_lock_release(&s_busy);
}

static void capture_locked(uint32_t vram) {
    uint32_t phys, blk, i;
    if ((int32_t)vram >= 0) {   /* KUSEG: through the TLB, a page at a time */
        uint32_t vp = vram & ~(VPAGE - 1);
        if (xrc_tlb_to_phys(vp) == ~0u || xrc_tlb_to_phys(vp + VPAGE - 4) == ~0u) return;
        uint32_t h = 0x811C9DC5u, k;
        for (k = 0; k < VPAGE; k += 4) h = (h ^ word_mapped(vp + k)) * 0x01000193u;
        for (i = 0; i < s_nvpages; i++)
            if (s_vpages[i] == vp) {
                /* the same virtual page holding other code now (Castlevania: Legacy of Darkness
                 * pages each actor's code in at 0x0F000000): written down once, so n64rx leaves
                 * the page to this net instead of recompiling one actor's bytes for all */
                if (s_vhash[i] != h && !s_vmulti[i]) {
                    char line[48];
                    s_vmulti[i] = 1;
                    snprintf(line, sizeof line, "kuseg_versions %08x", (unsigned)vp);
                    emit(line);
                }
                return;
            }
        if (s_nvpages == MAX_VPAGES) return;
        s_vhash[s_nvpages] = h;
        s_vpages[s_nvpages++] = vp;
        save(vp, VPAGE, word_mapped);
        return;
    }
    /* KSEG0: the 4 KB page, when code in it first runs (a 64 KB block could be
     * saved before the rest of it is loaded: Donkey Kong 64 then ran stale
     * bytes) */
    phys = vram & 0x1FFFFFFFu;
    if (phys >= xrc_rdram_size) return;
    blk = phys / VPAGE;
    if (blk >= sizeof s_captured || s_captured[blk]) return;
    s_captured[blk] = 1;
    save(0x80000000u | (phys & ~(VPAGE - 1)), VPAGE, word_kseg0);
}

/* an overlay the game relocated (ovl.c xrc_ovl_relocate: the Zelda engine's
 * Overlay_Relocate), once per link address: n64rx recompiles the ones a run
 * used as relocatable sections (analyze/zovl.py), not all of them */
void xrc_learn_reloc(uint32_t vram, uint32_t ram, uint32_t len);
void xrc_learn_reloc(uint32_t vram, uint32_t ram, uint32_t len) {
    static uint32_t s_seen[1024];
    static unsigned s_n;
    char line[48];
    unsigned i;
    for (i = 0; i < s_n; i++)
        if (s_seen[i] == vram) return;
    if (s_n == 1024) return;
    s_seen[s_n++] = vram;
    /* (where it landed and how long: pages the net captured there were this overlay's code, which the
     * next build recompiles from the ROM instead) */
    snprintf(line, sizeof line, "reloc %08x %08x %x", (unsigned)vram, (unsigned)ram, (unsigned)len);
    emit(line);
}

/* the KUSEG regions the TLB maps, once per 1 MB, and whether the host could
 * alias them (tlb.c: one that lands on the kernel's map of physical memory
 * cannot): the next build puts RDRAM where all of them can (n64rx
 * -DXRC_RDRAM_USER, from the lowest region, when any failed) */
void xrc_learn_kuseg(uint32_t va, int aliased) {
    static uint32_t s_seen[64];
    static unsigned s_n;
    char line[48];
    uint32_t key = (va >> 20) << 1 | (aliased ? 0u : 1u);
    unsigned i;
    for (i = 0; i < s_n; i++)
        if (s_seen[i] == key) return;
    if (s_n == 64) return;
    s_seen[s_n++] = key;
    snprintf(line, sizeof line, "kuseg %08x %x", (unsigned)(va & 0xFFF00000u), aliased ? 0u : 1u);
    emit(line);
}

/* guard.c: a captured page now holds other code than the build recompiled
 * (an overlay loaded over another): saved again, once a page a run, so the
 * next --learn build has this one too */
/* A function the guard found changed may run over several pages (Mario
 * Tennis's 0x8010CF2C, 0x14F4 bytes): each of them is saved, or the next
 * build has a version cut short at the first page's end, whose function
 * "runs out of the segment" and is interpreted again, round after round.
 * -DXRC_RECAPTURE_SPAN=0: the first page only, as before */
#ifndef XRC_RECAPTURE_SPAN
#define XRC_RECAPTURE_SPAN 1
#endif
void xrc_learn_recapture(uint32_t vram);
void xrc_learn_recapture_n(uint32_t vram, uint32_t bytes) {
    static uint32_t s_done[64];
    static unsigned s_n;
    static volatile long s_busy;
    uint32_t v, lo = vram & ~(VPAGE - 1), end = vram + (bytes ? bytes - 1u : 0u), hi;
    unsigned i;
    if (!XRC_RECAPTURE_SPAN || end < vram || bytes > 0x10000u) end = vram;
    hi = (end & ~(VPAGE - 1)) + VPAGE;
    if (!XRC_LEARN_CODE) return;
    if (hi - lo == VPAGE || (int32_t)lo >= 0 || (lo & 0x1FFFFFFFu) + (hi - lo) > xrc_rdram_size) {
        for (v = lo; v < hi; v += VPAGE) xrc_learn_recapture(v);   /* one page, or through the TLB: a page each */
        return;
    }
    for (i = 0; i < s_n; i++)
        if (s_done[i] == vram) return;
    if (s_n == 64) return;
    s_done[s_n++] = vram;
    while (__sync_lock_test_and_set(&s_busy, 1)) xhw_sleep_ms(0);
    /* from the function's own start to its last page's end, at one moment: learn.py makes it a version
     * rooted at that start alone (a group from the page's start made versions with starts it guessed) */
    v = vram & ~3u;
    save_rec(0x80000000u | (v & 0x1FFFFFFFu), hi - v, GROUP, word_kseg0);
    __sync_lock_release(&s_busy);
}

void xrc_learn_recapture(uint32_t vram) {
    static uint32_t s_done[256];
    static unsigned s_n;
    static volatile long s_busy;
    uint32_t vp = vram & ~(VPAGE - 1);
    unsigned i;
    if (!XRC_LEARN_CODE) return;
    for (i = 0; i < s_n; i++)
        if (s_done[i] == vp) return;
    if (s_n == 256) return;
    s_done[s_n++] = vp;
    while (__sync_lock_test_and_set(&s_busy, 1)) xhw_sleep_ms(0);
    if ((int32_t)vp >= 0) {
        char line[48];
        snprintf(line, sizeof line, "kuseg_versions %08x", (unsigned)vp);   /* other code at a mapped page */
        emit(line);
        if (xrc_tlb_to_phys(vp) != ~0u && xrc_tlb_to_phys(vp + VPAGE - 4) != ~0u) save(vp, VPAGE, word_mapped);
    } else if ((vp & 0x1FFFFFFFu) < xrc_rdram_size) {
        save(0x80000000u | (vp & 0x1FFFFFFFu), VPAGE, word_kseg0);
    }
    __sync_lock_release(&s_busy);
}

/* the blocks [lo, hi) the build asks for: captured code whose jump table
 * sits in RDRAM no capture holds yet (n64rx's want.txt; the function's
 * interpreter stub calls this the first time it runs, when its table is
 * loaded with it) */
void xrc_learn_want(uint32_t lo, uint32_t hi) {
    uint32_t v;
    for (v = lo & ~(VPAGE - 1); v < hi; v += VPAGE) capture(v);
}

static void log_one(Learnt* e) {
    char line[80];
    uint32_t phys = e->vram & 0x1FFFFFFFu;
    if (XRC_HIGH_SAFETY && e->vram >= 0xC0000000u) {   /* through the TLB: the DMA that filled its page */
        uint32_t p = xrc_tlb_to_phys(e->vram);
        if (p == ~0u) {   /* unmapped since: nothing to tie it to (n64rx finds its ROM table entry) */
            snprintf(line, sizeof line, "func %08x %x from %08x", (unsigned)e->vram, (unsigned)e->len,
                     (unsigned)e->caller);
            e->logged = 1, e->logged_len = e->len;
            emit(line);
            return;
        }
        phys = p;
    }
    unsigned i, n = s_ndma, k = n < MAX_DMA ? n : MAX_DMA;
    e->logged = 1, e->logged_len = e->len;
    for (i = 0; i < k; i++) {   /* newest first */
        const Dma* d = &s_dma[(n - 1 - i) % MAX_DMA];
        if (phys - (d->ram & 0x1FFFFFFFu) < d->size) {
            snprintf(line, sizeof line, "dma %08x %08x %x", (unsigned)d->rom, (unsigned)(d->ram | 0x80000000u),
                     (unsigned)d->size);
            emit(line);
            break;
        }
    }
    if (i == k && e->vram < 0xC0000000u) capture(e->vram);   /* no DMA from the ROM put it there: its bytes, from RDRAM */
    snprintf(line, sizeof line, "func %08x %x from %08x", (unsigned)e->vram, (unsigned)e->len, (unsigned)e->caller);
    emit(line);
}

static Learnt* add(uint32_t vram, uint32_t caller) {
    Learnt* e = find(vram);
    if (e) return e;
    if (s_nl >= MAX_LEARN) {
        static int said;
        if (!said) said = 1, xhw_logf("[LEARN] the net's table is full (%u addresses): %08x and later ones are not run",
                                      (unsigned)MAX_LEARN, (unsigned)vram);
        return NULL;
    }
    e = &s_l[s_nl++];
    memset(e, 0, sizeof *e);
    e->vram = vram, e->caller = caller;
    index_add((unsigned)(e - s_l));
    return e;
}

/* ---- the interpreter's report: a function it ran for the net returned ---- */
void xrc_learn_func(uint32_t vram, uint32_t len, uint32_t caller);
void xrc_learn_func(uint32_t vram, uint32_t len, uint32_t caller) {
    Learnt* e;
    lock();
    if ((e = add(vram, caller)) != NULL) {
        if (len > e->len && len < 0x10000u) e->len = len;
        if (!e->caller) e->caller = caller;
        if (!e->logged || (!e->logged_len && e->len)) log_one(e);   /* at entry (len 0), and once more with its span */
    }
    unlock();
}

/* ---- the net ---- */
void xrc_safety_run(uint8_t* rdram, recomp_context* ctx, uint32_t vram);
void xrc_safety_run(uint8_t* rdram, recomp_context* ctx, uint32_t vram) {
    uint32_t caller;
    Learnt* e;
    lock();
    e = find(vram);
    caller = e ? e->caller : 0;
    unlock();
    xrc_learn_func(vram, 0, caller);   /* logged before it runs: a thread's entry may never return */
    xrc_interp_learn(rdram, ctx, vram, caller);
}

/* a body n64rx gave a function withheld from recompilation (the test of the
 * net: N64RX_WITHHOLD) calls this, as a miss would */
void xrc_learn_enter(uint8_t* rdram, recomp_context* ctx, uint32_t vram);
void xrc_learn_enter(uint8_t* rdram, recomp_context* ctx, uint32_t vram) {
    uint32_t caller = xrc_ovl_vram_of((uintptr_t)__builtin_return_address(0));
    lock();
    add(vram, caller);
    unlock();
    xrc_safety_run(rdram, ctx, vram);
}

#if !defined(__i386__)
/* host builds (the tests): one address at a time */
static uint32_t s_pending;
static void host_thunk(uint8_t* rdram, recomp_context* ctx) { xrc_safety_run(rdram, ctx, s_pending); }
#endif

/* a call into a KUSEG page the TLB does not map: the CPU would miss on the
 * fetch, and the game's refill code and fault path page it in (GoldenEye
 * evicts its 0x7F000000 code pages at random, tlb_manage.c in its decomp,
 * and calls through pointers into them). -DXRC_LOOKUP_TLB=0: no function,
 * as before. */
#ifndef XRC_LOOKUP_TLB
#define XRC_LOOKUP_TLB 1
#endif
uint32_t xrc_tlb_resolve(uint32_t vaddr, int store);   /* tlb.c */

/* KSEG2/3 code the game maps with its TLB where it loaded it (Paper Mario's
 * effects at 0xE0xxxxxx): the safety net runs it (the interpreter fetches
 * through the TLB) and learns it, by its physical page, where it ended the
 * game with "No function at this address" before. -DXRC_HIGH_SAFETY=0: as before (defined above) */
static int holds_code(uint32_t vram) {
    uint32_t off = vram - 0x80000000u;
    if (XRC_HIGH_SAFETY && vram >= 0xC0000000u) {
        uint32_t p = xrc_tlb_to_phys(vram);
        if (p == ~0u && XRC_LOOKUP_TLB && !(vram & 3)) p = xrc_tlb_resolve(vram, 0);
        if (p == ~0u || p + 8 > xrc_rdram_size || (vram & 3)) return 0;
        return (xrc_word(0x80000000u | p) | xrc_word(0x80000000u | (p + 4))) != 0;
    }
    if ((int32_t)vram >= 0) {   /* KUSEG: code the game maps with its own TLB (Turok 2), by its physical page */
        uint32_t p = xrc_tlb_to_phys(vram);
        if (p == ~0u && XRC_LOOKUP_TLB && vram >= 0x10000u && !(vram & 3)) p = xrc_tlb_resolve(vram, 0);
        if (p == ~0u || p + 8 > xrc_rdram_size || (vram & 3)) return 0;
        return (xrc_word(0x80000000u | p) | xrc_word(0x80000000u | (p + 4))) != 0;
    }
    return vram >= 0x80000000u && off + 8 <= xrc_rdram_size && !(vram & 3) && (xrc_word(vram) | xrc_word(vram + 4));
}

recomp_func_t* xrc_learn_lookup(uint32_t vram, uintptr_t host_caller);
recomp_func_t* xrc_learn_lookup(uint32_t vram, uintptr_t host_caller) {
    recomp_func_t* f = NULL;
    Learnt* e;
    if (!XRC_SAFETY_NET) return NULL;
    if (!holds_code(vram)) {
        if (XHW_TEST_BUILD && vram >= 0x80000000u && (vram & 0x1FFFFFFFu) + 8 <= xrc_rdram_size)
            xhw_logf("[LEARN] %08x: no code there (words %08x %08x)", (unsigned)vram, (unsigned)xrc_word(vram),
                     (unsigned)xrc_word(vram + 4));
        return NULL;
    }
    lock();
    e = add(vram, xrc_ovl_vram_of(host_caller));
    if (e) {
#if defined(__i386__)
        uint8_t* p = s_stubs[e - s_l];
        if (!e->stub) {
            int32_t rel = (int32_t)((uintptr_t)xrc_safety_run - ((uintptr_t)p + 18));
            static const uint8_t pre[] = {0xFF, 0x74, 0x24, 0x0C, 0xFF, 0x74, 0x24, 0x0C};   /* push [esp+12] x2 */
            p[0] = 0x68;                                                                     /* push vram */
            memcpy(p + 1, &vram, 4);
            memcpy(p + 5, pre, sizeof pre);
            p[13] = 0xE8;                                                                    /* call xrc_safety_run */
            memcpy(p + 14, &rel, 4);
            p[18] = 0x83, p[19] = 0xC4, p[20] = 0x0C;                                        /* add esp, 12 */
            p[21] = 0xC3;                                                                    /* ret */
            __asm__ volatile("" ::: "memory");
            {
                int idx = xrc_ovl_enter(vram, (recomp_func_t*)(void*)p);
                if (idx) e->stub = 1, e->slot = (uint16_t)idx;
            }
            /* once per address: with no table room every call came here and
             * logged again (docs/review-2026-10.md 5a.3) */
            if (!e->said)
                xhw_logf("[LEARN] safety net: %08x has no function; interpreted (called from %08x)", (unsigned)vram,
                         (unsigned)e->caller);
            e->said = 1;
        } else if (XRC_STUB_REENTER && e->slot) {
            /* an overlay switch cleared its table slot: back in, so the next
             * call is a table hit again rather than this lookup (5a.4).
             * -DXRC_STUB_REENTER=0: as before */
            xrc_ovl_reenter(vram, e->slot);
        }
        f = (recomp_func_t*)(void*)p;
#else
        s_pending = vram;
        f = host_thunk;
#endif
    }
    unlock();
    return f;
}

/* ---- where code came from (pi.c), and code that may have changed ---- */
static void relog(uint32_t ram, uint32_t size) {
    uint32_t base = ram & 0x1FFFFFFFu;
    unsigned i;
    for (i = 0; i < s_nl; i++)
        if ((s_l[i].vram & 0x1FFFFFFFu) - base < size) s_l[i].logged = 0;
}

void xrc_learn_dma(uint32_t rom, uint32_t ram, uint32_t size);
void xrc_learn_dma(uint32_t rom, uint32_t ram, uint32_t size) {
    Dma* last;
    if (!XRC_SAFETY_NET || !size) return;
    lock();
    last = s_ndma ? &s_dma[(s_ndma - 1) % MAX_DMA] : NULL;
    if (last && rom == last->rom + last->size && ram == last->ram + last->size && last->size + size <= 0x800000u) {
        last->size += size;   /* the next chunk of one load (SM64 reads a segment 4 KB at a time): one segment */
    } else {
        Dma* d = &s_dma[s_ndma++ % MAX_DMA];
        d->rom = rom, d->ram = ram, d->size = size;
    }
    if (s_nl) relog(ram, size);
    unlock();
}

void xrc_learn_inval(uint32_t ram, uint32_t size);
void xrc_learn_inval(uint32_t ram, uint32_t size) {
    if (!XRC_SAFETY_NET || !s_nl) return;
    lock();
    relog(ram, size);
    unlock();
}
