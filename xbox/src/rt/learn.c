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
#include <string.h>

#include "rt.h"
#include "xhw.h"

#define MAX_LEARN 512        /* addresses the net remembers (each a stub or a logged in-line callee) */
#define MAX_DMA 1024         /* the newest PI DMAs, to say where code came from */
#define TEXT_MAX (48 * 1024) /* learn.txt */

void xrc_interp_learn(uint8_t* rdram, recomp_context* ctx, uint32_t entry, uint32_t caller);   /* interp.c */
int xrc_ovl_enter(uint32_t vram, recomp_func_t* f);                                             /* ovl.c */
uint32_t xrc_ovl_vram_of(uintptr_t host);                                                       /* ovl.c */

typedef struct {
    uint32_t vram, caller, len, logged_len;
    uint8_t logged, stub;
} Learnt;

typedef struct { uint32_t rom, ram, size; } Dma;

static xhw_mutex* s_lock;
static Learnt s_l[MAX_LEARN];
static unsigned s_nl;
static Dma s_dma[MAX_DMA];
static unsigned s_ndma;
static char s_text[TEXT_MAX];
static unsigned s_tlen;
static int s_text_loaded;

#if defined(__i386__)
#define STUB_BYTES 24
static uint8_t s_stubs[MAX_LEARN][STUB_BYTES] __attribute__((aligned(16)));
#endif

static void lock(void) {
    if (!s_lock) s_lock = xhw_mutex_create();
    xhw_mutex_lock(s_lock);
}
static void unlock(void) { xhw_mutex_unlock(s_lock); }

static Learnt* find(uint32_t vram) {
    unsigned i;
    for (i = 0; i < s_nl; i++)
        if (s_l[i].vram == vram) return &s_l[i];
    return NULL;
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
        xhw_file_close(f);
    }
    if (!s_tlen) {
        static const char head[] = "# n64rx learn file: code this build did not recompile (PLAN.md 3.14).\n"
                                   "# n64rx build <rom> --learn learn.txt recompiles it next time.\n";
        memcpy(s_text, head, sizeof head - 1);
        s_tlen = sizeof head - 1;
    }
    s_text[s_tlen] = 0;
}

/* one line, logged and added to the file unless the file has it */
static void emit(const char* line) {
    char path[96];
    unsigned n = (unsigned)strlen(line);
    xhw_logf("[LEARN] %s", line);
    if (!s_text_loaded) text_load();
    {   /* the same line from an earlier boot: already there */
        const char* p = s_text;
        while ((p = strstr(p, line)) != NULL) {
            if ((p == s_text || p[-1] == '\n') && p[n] == '\n') return;
            p += n;
        }
    }
    if (s_tlen + n + 2 >= TEXT_MAX) return;
    memcpy(s_text + s_tlen, line, n);
    s_tlen += n;
    s_text[s_tlen++] = '\n';
    s_text[s_tlen] = 0;
    snprintf(path, sizeof path, "%slearn.txt", xhw_save_dir());
    if (!xhw_write_file_atomic(path, s_text, s_tlen)) xhw_logf("[LEARN] could not write %s", path);
}

static void log_one(Learnt* e) {
    char line[80];
    uint32_t phys = e->vram & 0x1FFFFFFFu;
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
    snprintf(line, sizeof line, "func %08x %x from %08x", (unsigned)e->vram, (unsigned)e->len, (unsigned)e->caller);
    emit(line);
}

static Learnt* add(uint32_t vram, uint32_t caller) {
    Learnt* e = find(vram);
    if (e || s_nl >= MAX_LEARN) return e;
    e = &s_l[s_nl++];
    memset(e, 0, sizeof *e);
    e->vram = vram, e->caller = caller;
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

static int holds_code(uint32_t vram) {
    uint32_t off = vram - 0x80000000u;
    if ((int32_t)vram >= 0) {   /* KUSEG: code the game maps with its own TLB (Turok 2), by its physical page */
        uint32_t p = xrc_tlb_to_phys(vram);
        if (p == ~0u || p + 8 > xrc_rdram_size || (vram & 3)) return 0;
        return (xrc_word(0x80000000u | p) | xrc_word(0x80000000u | (p + 4))) != 0;
    }
    return vram >= 0x80000000u && off + 8 <= xrc_rdram_size && !(vram & 3) && (xrc_word(vram) | xrc_word(vram + 4));
}

recomp_func_t* xrc_learn_lookup(uint32_t vram, uintptr_t host_caller);
recomp_func_t* xrc_learn_lookup(uint32_t vram, uintptr_t host_caller) {
    recomp_func_t* f = NULL;
    Learnt* e;
    if (!XRC_SAFETY_NET || !holds_code(vram)) return NULL;
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
            if (xrc_ovl_enter(vram, (recomp_func_t*)(void*)p)) e->stub = 1;
            xhw_logf("[LEARN] safety net: %08x has no function; interpreted (called from %08x)", (unsigned)vram,
                     (unsigned)e->caller);
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
