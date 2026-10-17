/* strand.c - a game's own setjmp/longjmp, as coroutines (docs/decisions.md
 * "Coroutines through setjmp and longjmp").
 *
 * Mario Party runs its game objects as processes that switch stacks with
 * setjmp/longjmp (Hudson's HuPrc: the scheduler setjmps, longjmps into a
 * process; a process setjmps its own buffer and longjmps back). In
 * recompiled code every N64 call is a host C call, so a longjmp cannot jump
 * into another stack: each context the game switches to runs on a host
 * thread of its own (a strand), all of them the same N64 thread to the
 * runtime, one running at a time.
 *
 * n64rx names the game's pair (romonly.py: n64_setjmp, n64_longjmp, routed
 * here) and turns each call to setjmp into XRC_SETJMP (recomp.h): a host
 * __builtin_setjmp in the calling function, kept per N64 buffer. A longjmp
 * to a buffer whose strand is waiting wakes it, and it host-longjmps back
 * to that setjmp, whose frame is still live below where it waited, with the
 * buffer's registers and the value. A longjmp to a buffer no setjmp filled
 * as it is (a new process: the game wrote its entry and stack; a dying
 * one: its entry changed) starts a strand there; the strand that waited on
 * that buffer before is ended.
 * DIVERGES: a context that returns from its entry waits for good (the N64
 * would jump to the buffer's ra again).
 * Kill switch: N64RX_STRANDS=0 (n64rx: the game's own pair, recompiled). */
#include <stdio.h>
#include <string.h>

#include "rt.h"
#include "xhw.h"

void* xu_self_thr(void);
void xu_adopt_thr(void* thr);
int xu_thr_level(void* thr);

#define MAX_STRANDS 96
#define MAX_BUFS 512
#define STRAND_STACK (128u * 1024u)

typedef struct Strand {
    int used;
    void* thr;                       /* the N64 thread (thread.c) */
    xhw_event* ev;                   /* wakes it */
    volatile uint32_t resume_buf, resume_v;
    volatile int die;
    int has_base;
    void* base_jb[5];                /* its top: dying unwinds here (not the first strand of a thread) */
    uint32_t entry, sp, v, regs[11], fregs[12];   /* a new one: where it starts */
} Strand;

typedef struct {
    uint32_t buf;                    /* the N64 jmp_buf */
    Strand* owner;                   /* the strand that setjmp'd it */
    uint32_t ra, sp;                 /* as that setjmp wrote them */
    void* jb[5];                     /* its host setjmp */
} Buf;

static Strand s_strands[MAX_STRANDS];
static Buf s_bufs[MAX_BUFS];
static uint32_t s_tls = 0xFFFFFFFFu;
static volatile long s_busy;

static void lock(void) { while (__sync_lock_test_and_set(&s_busy, 1)) xhw_sleep_ms(0); }
static void unlock(void) { __sync_lock_release(&s_busy); }

static Strand* strand_new(void* thr) {
    unsigned i;
    for (i = 0; i < MAX_STRANDS; i++)
        if (!s_strands[i].used) {
            Strand* st = &s_strands[i];
            xhw_event* ev = st->ev ? st->ev : xhw_event_create();
            memset(st, 0, sizeof *st);
            st->used = 1, st->thr = thr, st->ev = ev;
            return st;
        }
    xrc_fatal_vram("Too many coroutines (setjmp contexts)", 0, MAX_STRANDS);
    return NULL;
}

/* the calling host thread's strand; an N64 thread's own one the first time */
static Strand* current(void) {
    Strand* st;
    if (s_tls == 0xFFFFFFFFu) s_tls = xhw_tls_alloc();
    st = (Strand*)xhw_tls_get(s_tls);
    if (!st) {
        lock();
        st = strand_new(xu_self_thr());
        unlock();
        xhw_tls_set(s_tls, st);
    }
    return st;
}

static Buf* find(uint32_t buf, int make) {
    unsigned i, h = (buf >> 3) % MAX_BUFS;
    for (i = 0; i < MAX_BUFS; i++) {
        Buf* b = &s_bufs[(h + i) % MAX_BUFS];
        if (b->buf == buf) return b;
        if (!b->buf) {
            if (!make) return NULL;
            memset(b, 0, sizeof *b);
            b->buf = buf;
            return b;
        }
    }
    xrc_fatal_vram("Too many setjmp buffers", buf, MAX_BUFS);
    return NULL;
}

/* XRC_SETJMP's host buffer for the N64 buffer `buf` */
void** xrc_strand_jb(uint32_t buf) {
    Buf* b;
    lock();
    b = find(buf, 1);
    unlock();
    return b->jb;
}

static const int SREGS[11] = {29, 31, 16, 17, 18, 19, 20, 21, 22, 23, 30};   /* sp, ra, s0-s7, fp: the buffer's order */

static uint32_t greg(recomp_context* ctx, int r) { return (uint32_t)(&ctx->r0)[r]; }
static void sreg(recomp_context* ctx, int r, uint32_t v) { (&ctx->r0)[r] = (gpr)(int32_t)v; }
/* f20 + i as swc1 / lwc1 see it: an odd register is its pair's high half (interp.c's fw) */
static uint32_t* freg(recomp_context* ctx, int i) {
    int n = 20 + i;
    return (n & 1) ? &ctx->f_odd[(n - 1) * 2] : &(&ctx->f0)[n].u32l;
}

/* setjmp(buf): the registers into the buffer as the game's own does; the buffer is this strand's */
void xrc_setjmp_recomp(uint8_t* rdram, recomp_context* ctx);
void xrc_setjmp_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint32_t buf = (uint32_t)ctx->r4;
    Strand* me = current();
    Buf* b;
    int i;
    for (i = 0; i < 11; i++) MEM_W(4 * i, buf) = (int32_t)greg(ctx, SREGS[i]);
    for (i = 0; i < 12; i++) MEM_W(0x2C + 4 * i, buf) = (int32_t)*freg(ctx, i);
    lock();
    b = find(buf, 1);
    b->owner = me, b->ra = greg(ctx, 31), b->sp = greg(ctx, 29);
    unlock();
    ctx->r2 = 0;
}

/* XRC_SETJMP's second return: the buffer's registers, the value the longjmp gave */
void xrc_strand_resumed(uint8_t* rdram, recomp_context* ctx, uint32_t buf) {
    Strand* me = current();
    int i;
    for (i = 0; i < 11; i++) sreg(ctx, SREGS[i], (uint32_t)MEM_W(4 * i, buf));
    for (i = 0; i < 12; i++) *freg(ctx, i) = (uint32_t)MEM_W(0x2C + 4 * i, buf);
    ctx->r2 = (gpr)(int32_t)me->resume_v;
    me->resume_buf = 0;
}

/* wait until another strand longjmps to a buffer this one owns, then go back to that setjmp */
static void park(Strand* me) {
    for (;;) {
        xhw_event_wait(me->ev, XHW_INFINITE);
        if (me->die && me->has_base) __builtin_longjmp(me->base_jb, 1);
        if (xhw_thread_self()) xhw_thread_set_level(xhw_thread_self(), xu_thr_level(me->thr));   /* osSetThreadPri meanwhile */
        if (me->resume_buf) {
            Buf* b;
            lock();
            b = find(me->resume_buf, 0);
            unlock();
            if (b) __builtin_longjmp(b->jb, 1);
        }
    }
}

static void strand_main(void* p) {
    Strand* st = (Strand*)p;
    recomp_context ctx;
    int i;
    if (s_tls == 0xFFFFFFFFu) s_tls = xhw_tls_alloc();
    xhw_tls_set(s_tls, st);
    xu_adopt_thr(st->thr);
    if (__builtin_setjmp(st->base_jb)) {   /* ended: its buffer went to a new context */
        lock();
        st->used = 0;
        unlock();
        return;
    }
    st->has_base = 1;
    xrc_ctx_init(&ctx, st->sp);
    for (i = 0; i < 11; i++) sreg(&ctx, SREGS[i], st->regs[i]);
    for (i = 0; i < 12; i++) *freg(&ctx, i) = st->fregs[i];
    ctx.r2 = (gpr)(int32_t)st->v;
    get_function((int32_t)st->entry)(xrc_rdram, &ctx);
    xhw_logf("[STRAND] the context started at %08x returned; it waits", (unsigned)st->entry);
    park(st);
}

/* longjmp(buf, v) */
void xrc_longjmp_recomp(uint8_t* rdram, recomp_context* ctx);
void xrc_longjmp_recomp(uint8_t* rdram, recomp_context* ctx) {
    static unsigned s_new;
    uint32_t buf = (uint32_t)ctx->r4, v = (uint32_t)ctx->r5 ? (uint32_t)ctx->r5 : 1u;
    uint32_t sp = (uint32_t)MEM_W(0, buf), ra = (uint32_t)MEM_W(4, buf);
    Strand* me = current();
    Strand *to, *old = NULL;
    Buf* b;
    int i;
    if (XHW_TEST_BUILD) {   /* test builds: how often contexts switch */
        static unsigned s_n;
        if (++s_n % 3000 == 0) xhw_logf("[STRAND] %u longjmps (%u new contexts), last to %08x", s_n, s_new, (unsigned)ra);
    }
    lock();
    b = find(buf, 1);
    if (b->owner && b->ra == ra && b->sp == sp) {   /* back to the setjmp that filled it */
        to = b->owner;
        to->resume_buf = buf, to->resume_v = v;
        unlock();
        if (to == me) __builtin_longjmp(b->jb, 1);
        xhw_event_signal(to->ev);
        park(me);
    }
    /* a new context at the buffer's ra and sp (a process's entry, or its end) */
    if (b->owner && b->owner != me) old = b->owner;
    b->owner = NULL;
    to = strand_new(me->thr);
    to->entry = ra, to->sp = sp, to->v = v;
    for (i = 0; i < 11; i++) to->regs[i] = (uint32_t)MEM_W(4 * i, buf);
    for (i = 0; i < 12; i++) to->fregs[i] = (uint32_t)MEM_W(0x2C + 4 * i, buf);
    unlock();
    if (s_new++ < 16) xhw_logf("[STRAND] a new context at %08x, stack %08x", (unsigned)ra, (unsigned)sp);
    if (old) {   /* the one that waited on this buffer will never be resumed */
        old->die = 1;
        xhw_event_signal(old->ev);
    }
    if (!xhw_thread_spawn(strand_main, to, xu_thr_level(me->thr), STRAND_STACK))
        xrc_fatal_vram("A coroutine's thread could not be started", ra, 0);
    park(me);
}
