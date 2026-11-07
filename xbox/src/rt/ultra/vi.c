/* vi.c - the video interface: the retrace thread, the swap latch, the mode
 * the game set (PLAN.md 3.3).
 *
 * The retrace is xhw_wait_vblank (pbkit's vblank once the renderer runs at
 * 60 Hz, else 60.000 Hz from the ACPI timer). Every retrace: the swap
 * latches, the osViSetEvent message goes out every `retraceCount` retraces,
 * and the exit combos get their VI look (PLAN.md 3.9: the game has not read
 * the controllers for a while). The VI mode is kept for the interpreter's
 * content rect (M1+: 320x240, 640x480 interlaced, letterboxed modes).
 *
 * DIVERGES (Wings64-X's): the retrace message is skipped when it would
 * leave the game's queue fewer than XRC_VI_RESERVE free slots. Graphics
 * tasks run synchronously on the scheduler's thread, retraces pile up
 * meanwhile, and the task's own SP/DP done events (posted as it returns)
 * found the queue full and were lost; the game then waited for its RDP
 * timeout (Wings64-X console run 3). A late retrace message is harmless.
 *
 * DIVERGES: while the BACK menu is open no retrace message goes out, so the
 * game waits at its retrace (../settings_menu.c), and this thread drives
 * the menu. */
#include "ultra.h"

int xrc_menu_open(void);
void xrc_menu_vi(unsigned retrace);
void xrc_video_vi(unsigned retrace);
/* gfx.c: the frame drawn into fb presented (weak: frames presented at their task's end) */
__attribute__((weak)) void xrc_gfx_swap(uint32_t fb) { (void)fb; }
__attribute__((weak)) void xrc_gfx_swap_block(void) {}
__attribute__((weak)) void xrc_gfx_vi(unsigned retrace) { (void)retrace; }

#ifndef XRC_VI_RESERVE
#define XRC_VI_RESERVE 2
#endif
#ifndef XRC_VI_RESERVE_SMALL
#define XRC_VI_RESERVE_SMALL 1
#endif

static volatile uint32_t s_mq, s_msg, s_every = 1;
static volatile uint32_t s_cur, s_next, s_mode, s_black, s_features;
static volatile unsigned s_retrace;
static volatile int s_up;

unsigned xu_retrace(void) { return s_retrace; }
uint32_t xu_vi_mode(void) { return s_mode; }
uint32_t xu_vi_framebuffer(void) { return s_cur; }
uint32_t xu_vi_framebuffer_next(void) { return s_next; }

/* the rows of the framebuffer the mode shows (the renderer's screen size,
 * f3d_interp.c): field 0's yScale (2.10) over its lines, (vEnd - vStart) / 2
 * taken as 240 or 288 when that close (NTSC 0x25..0x1FF is 237); 0 when no
 * mode is set. OSViMode: type, 9 common registers from +4, then the fields
 * (origin +0x28, yScale +0x2C, vStart +0x30) */
/* osViSetYScale's value: fewer rows of the buffer stretched over the screen's lines. Madden 2000's title is 360 of its 480
 * rows, and showed in the top three quarters. -DXRC_VI_YSCALE=0: not looked at */
#ifndef XRC_VI_YSCALE
#define XRC_VI_YSCALE 1
#endif
static float s_yscale = 1.0f;
unsigned xu_vi_rows(void);
unsigned xu_vi_rows(void) {
    uint32_t m = s_mode, ys, vs;
    unsigned lines;
    if (!m) return 0;
    ys = rd32(m + 0x2C) & 0xFFFu;
    vs = rd32(m + 0x30);
    lines = (unsigned)(((vs & 0x3FFu) - ((vs >> 16) & 0x3FFu)) & 0x3FFu) / 2u;
    if (lines >= 230u && lines <= 250u) lines = 240u;
    else if (lines >= 278u && lines <= 298u) lines = 288u;
    if (!ys || !lines) return 0;
    if (XRC_VI_YSCALE && s_yscale != 1.0f && s_yscale >= 0.25f && s_yscale <= 2.0f) return (unsigned)((float)((ys * lines + 512u) >> 10) * s_yscale + 0.5f);
    return (ys * lines + 512u) >> 10;
}

/* the column the mode's first shown pixel is at (field 0's origin over the
 * pixel size, modulo the width): 0 for the usual origin of whole rows. Mario
 * Tennis's 384-wide buffer has origin 0x280 (320 pixels): each line starts
 * 320 pixels in, so its picture shows 64 pixels right of where a renderer
 * drawing from column 0 puts it (f3d_interp.c XGFX_VI_ORIGIN) */
unsigned xu_vi_origin_px(void);
unsigned xu_vi_origin_px(void) {
    uint32_t m = s_mode, w, o, bpp;
    if (!m) return 0;
    w = rd32(m + 8) & 0xFFFu;
    o = rd32(m + 0x28) & 0xFFFFFFu;
    bpp = (rd32(m + 4) & 3u) == 3u ? 4u : 2u;
    return w ? (unsigned)((o / bpp) % w) : 0u;
}

/* DIVERGES: a PAL game (osTvType 0, ../boot.c) gets 50 retraces a second
 * from the console's 60: every sixth vblank is skipped, so its game speed
 * and its music are right, its frame pacing a little uneven.
 * -DXRC_PAL_50HZ=0: 60 retraces for every game (a PAL game runs 20% fast) */
#ifndef XRC_PAL_50HZ
#define XRC_PAL_50HZ 1
#endif

void xu_ai_tick(void);   /* ai.c */

/* The BACK menu and the exit combos are read before the VI thread takes
 * the runtime lock: a game thread that holds it for long (interrupts off
 * around a host wait) no longer takes the menu's input with it (CLAUDE.md:
 * BACK in every game, in every state). -DXRC_MENU_VI_FIRST=0: after, as before */
#ifndef XRC_MENU_VI_FIRST
#define XRC_MENU_VI_FIRST 1
#endif
static void vi_thread(void* arg) {
    unsigned vbl = 0;
    (void)arg;
    for (;;) {
        uint32_t mq;
        xhw_wait_vblank();
        if (XRC_PAL_50HZ && xrc_tv_type == 0 && ++vbl % 6u == 0) continue;
#ifdef XRC_VI_DIV   /* diagnosis: one retrace every n vblanks, n times the CPU per retrace (is a game's fault a matter of speed? Donkey Kong 64's audio in xemu) */
        {
            static unsigned s_div;
            if (++s_div % (unsigned)(XRC_VI_DIV)) continue;
        }
#endif
        if (s_cur != s_next) s_cur = s_next;
        s_retrace++;
        if (XRC_MENU_VI_FIRST) xrc_menu_vi(s_retrace);
        xu_lock();
        xu_rcp_flush(NULL);   /* sp.c: done events whose thread is blocked (or two retraces old) */
        xu_unlock();
        mq = s_mq;
        if (!XRC_MENU_VI_FIRST) xrc_menu_vi(s_retrace);
        xrc_video_vi(s_retrace);   /* the video-mode prompt: A, or the timeout (../video.c) */
        xrc_gfx_vi(s_retrace);     /* a frame that waited too long for its swap (../gfx.c) */
#ifdef XRC_PEEK
#ifndef XRC_PEEK_EVERY
#define XRC_PEEK_EVERY 30   /* retraces between peeks; with 1, only when the first word changes */
#endif
        static uint32_t s_peek_last = 0xFFFFFFFFu;
        if (s_retrace % XRC_PEEK_EVERY == 0 && (XRC_PEEK_EVERY > 1 || rd32((uint32_t)(XRC_PEEK)) != s_peek_last)) {   /* diagnosis: -DXRC_PEEK=<N64 address>, 32 bytes twice a second */
            s_peek_last = rd32((uint32_t)(XRC_PEEK));
            uint32_t a = (uint32_t)(XRC_PEEK);
            xhw_logf("[PEEK] %08x: %08x %08x %08x %08x %08x %08x %08x %08x", (unsigned)a, (unsigned)rd32(a),
                     (unsigned)rd32(a + 4), (unsigned)rd32(a + 8), (unsigned)rd32(a + 12), (unsigned)rd32(a + 16),
                     (unsigned)rd32(a + 20), (unsigned)rd32(a + 24), (unsigned)rd32(a + 28));
#ifdef XRC_PEEK2   /* a second address, the same way */
            a = (uint32_t)(XRC_PEEK2);
            xhw_logf("[PEEK] %08x: %08x %08x %08x %08x %08x %08x %08x %08x", (unsigned)a, (unsigned)rd32(a),
                     (unsigned)rd32(a + 4), (unsigned)rd32(a + 8), (unsigned)rd32(a + 12), (unsigned)rd32(a + 16),
                     (unsigned)rd32(a + 20), (unsigned)rd32(a + 24), (unsigned)rd32(a + 28));
#endif
        }
#endif
        if (mq && s_retrace % s_every == 0 && !xrc_menu_open()) {
            int32_t count = (int32_t)rd32(mq + MQ_MSGCOUNT), valid = (int32_t)rd32(mq + MQ_VALIDCOUNT);
            /* a queue no bigger than the reserve keeps none: a 1- or 2-slot retrace queue
             * got no message at all, and a game blocking on it each frame hung
             * (docs/review-2026-10.md 5b.1). -DXRC_VI_RESERVE_SMALL=0: as before */
            int32_t r = XRC_VI_RESERVE_SMALL && count <= XRC_VI_RESERVE ? 0 : XRC_VI_RESERVE;
            if (!r || valid + 1 + r <= count) xu_send(mq, s_msg, 0, 0);
        }
        xu_post_event(OS_EVENT_VI);
        xu_ai_tick();    /* the DAC's buffers end on their own: OS_EVENT_AI (ai.c) */
#ifdef XRC_DIAG_WATCH   /* diagnosis: a word of the game's, each time a retrace finds it changed */
        {
            static uint32_t s_was;
            static unsigned s_n;
            uint32_t now = rd32(XRC_DIAG_WATCH);
            if (now != s_was && s_n++ < 400) xhw_logf("[WATCH] %08x: %08x -> %08x (retrace %u)", (unsigned)(XRC_DIAG_WATCH), (unsigned)s_was, (unsigned)now, xu_retrace());
            s_was = now;
        }
#endif
        xrc_mmio_vi();   /* the register pages' status words, for recompiled pollers (../mmio.c) */
        xrc_guard_retrace();   /* versioned captured code checked again (../guard.c) */
        xhw_combo_vi_look(s_retrace);
        if (XHW_TEST_BUILD) {   /* no graphics task for a second: say once what everyone waits on */
            static unsigned s_seen, s_still;
            unsigned g = xu_gfx_tasks();
            if (g != s_seen) s_seen = g, s_still = 0;
            else if ((++s_still == 60 || (s_still % 1800 == 0 && s_still <= 7200)) && g)
                xu_stall_report(s_still == 60 ? "no graphics task for 60 retraces" : "still no graphics task (30 s more)");
            else if (s_still == 300 && !g) xu_stall_report("no graphics task at all after 300 retraces");
            {   /* audio tasks that stop after running: say once what everyone waits on */
                static unsigned s_aseen, s_astill, s_asaid;
                unsigned a = xu_aud_tasks();
                if (a != s_aseen) s_aseen = a, s_astill = 0;
                else if (++s_astill == 300 && a && s_asaid < 3) {
                    s_asaid++;
                    xu_stall_report("audio tasks stopped (none for 300 retraces)");
                }
            }
            if (s_retrace % 1800 == 0)   /* every 30 s */
                xhw_logf("[VI] retrace %u: %u gfx tasks, queue %08x %u/%u", s_retrace, g, (unsigned)s_mq,
                         s_mq ? (unsigned)rd32(s_mq + MQ_VALIDCOUNT) : 0u, s_mq ? (unsigned)rd32(s_mq + MQ_MSGCOUNT) : 0u);
        }
    }
}

/* the retrace thread; also when a game registers for OS_EVENT_VI with its
 * own VI manager (thread.c: a compressed boot's libultra, unnamed, waits on
 * the event the N64's VI interrupt posts) */
void xu_vi_start(void) {
    if (s_up) return;
    s_up = 1;
    xu_spawn(vi_thread, NULL, 14, 32 * 1024);
    xhw_log("[OS] VI manager up");
}

ULTRA(osCreateViManager) {
    (void)rdram;
    (void)ctx;
    xu_vi_start();
}

/* osViSetEvent(mq, msg, retraceCount) */
ULTRA(osViSetEvent) {
    (void)rdram;
    s_msg = ARG(1);
    s_every = ARG(2) ? ARG(2) : 1;
    s_mq = ARG(0);
    xhw_logf("[OS] osViSetEvent: queue %08x message %08x every %u retraces", (unsigned)ARG(0), (unsigned)ARG(1),
             (unsigned)s_every);
}

/* the watchdog's heartbeat, test builds: a game with no retrace yet has no stall report from the VI thread (it reports each retrace) */
void xrc_beat(unsigned secs);
void xrc_beat(unsigned secs) {
    static int s_said;
    if (XHW_TEST_BUILD && secs >= 15 && !s_retrace && !s_said++) xu_stall_report("no retrace after 15 s: the VI manager never made");
}

ULTRA(osViSwapBuffer) {
    uint32_t fb = ARG(0);
    (void)rdram;
#ifdef XRC_DIAG_SWAP   /* diagnosis: each swap, with a word of the game's */
    {
        static unsigned s_n;
        if (s_n++ < 300) xhw_logf("[SWAP] %08x retrace %u, word %08x", (unsigned)fb, s_retrace, (unsigned)rd32(XRC_DIAG_SWAP));
    }
#endif
    s_next = fb;
    /* The first swap too shows at the next retrace. Star Wars Episode I Racer swaps, then waits while the current framebuffer is
     * the one it will draw next: with the first swap current at once it waited for ever, after one frame.
     * -DXRC_VI_SWAP_NOW=1: the first swap current at once, as before */
#ifndef XRC_VI_SWAP_NOW
#define XRC_VI_SWAP_NOW 0
#endif
    if (XRC_VI_SWAP_NOW && !s_cur) s_cur = fb;
    xrc_gfx_swap(xu_phys(fb));   /* the frame drawn into fb presented (../gfx.c) */
}

/* before the first swap libultra's __osViInit has both at K0BASE
 * (0x80000000; vi.c in the decomps): a game that reads them then gets RDRAM's
 * start, not NULL. -DXRC_VI_K0BASE=0: 0, as before */
#ifndef XRC_VI_K0BASE
#define XRC_VI_K0BASE 1
#endif
#define VI_FB(v) ((v) || !XRC_VI_K0BASE ? (v) : 0x80000000u)

ULTRA(osViGetCurrentFramebuffer) {
    (void)rdram;
    RET(VI_FB(s_cur));
}

ULTRA(osViGetCurrentMode) {   /* the mode's type: its first byte */
    (void)rdram;
    RET(s_mode ? rd8(s_mode) : 0);
}

ULTRA(osViGetNextFramebuffer) {
    (void)rdram;
    RET(VI_FB(s_next));
}

ULTRA(osViSetMode) {
    (void)rdram;
    if (s_mode != ARG(0)) xhw_logf("[OS] VI mode at %08x", (unsigned)ARG(0));
    s_mode = ARG(0);
}

ULTRA(osViBlack) {
    (void)rdram;
    s_black = ARG(0) & 0xFF;
}

ULTRA(osViSetSpecialFeatures) {
    (void)rdram;
    s_features = ARG(0);
}

ULTRA(osViSetXScale) {
    (void)rdram;
    (void)ctx;
}

ULTRA(osViSetYScale) {
    (void)rdram;
    if (ctx->f12.fl != s_yscale) {
        static unsigned s_n;
        if (s_n++ < 8) xhw_logf("[OS] VI Y scale %d/1000 (retrace %u)", (int)(ctx->f12.fl * 1000.0f), xu_retrace());
    }
    s_yscale = ctx->f12.fl;
}

ULTRA(osViRepeatLine) {
    (void)rdram;
    (void)ctx;
}
