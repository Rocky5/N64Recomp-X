/* xhw_splash_draw.c - the loading screen, one frame at a time (PLAN.md 5.5).
 *
 * The siblings' "TechProGabe Presents..." card (Melee-X's xhw_splash.c, from
 * OpenCrossing-Xbox's xbox_splash.c) kept as it was: unscii-16 at 3x, the
 * navy gradient rgb(14, 22, 52) -> rgb(6, 10, 28), the fade-in over ~500 ms,
 * the 400x8 bar in rgb(120, 200, 110) on rgb(30, 40, 60), "Hold BACK for 480i
 * (safe video)". Added, top to bottom:
 *
 *   background  a slow aurora over the gradient: three soft bands in the
 *               family's palette (navy, teal, violet) drifting diagonally,
 *               computed at 160x120 from a sine table and upscaled 4x with
 *               a 2x2 (bilinear) smoothing step; at most 48 slow rising
 *               sparks over it
 *   wordmark    the fade-in, then one light sweep (~700 ms), then settled
 *               with a shadow so it reads over the moving background
 *   game card   the pack's 64x64 icon and title, a line under it;
 *               "N64Recomp-X <version>" small at the bottom right
 *   stage line  the stage label and (test builds) its time, over the bar,
 *               which keeps the sibling's green with a soft gradient and a
 *               moving highlight
 *   video line  which mode is coming and how to hold BACK for 480i; what a
 *               hold has latched; the probation note
 *
 * Card mode (the error cards in the same dress) replaces the stage, bar and
 * video lines with the card's lines and footer.
 *
 * Pure: the frame pointer, the state and the font. No kernel calls, no
 * floating point beyond the progress value and the sine table's one-time
 * setup, so the host test draws exactly what the console draws. The frame
 * is drawn whole into a cached buffer and copied out in rows by the caller
 * (the framebuffer is write-combined: never read back). */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "xhw_splash.h"

const unsigned char xhw_font16[256 * 16] = {   /* also the overlay's (xhw_overlay.c) */
#include <hal/font_unscii_16.h>
};

#define W XHW_SPLASH_W
#define H XHW_SPLASH_H
#define GW 8
#define GH 16

/* the surface text, fills and sparks clip to: the 640x480 frame, or the
 * BACK menu's card image while xhw_panel_render draws */
static int s_clip_w = W, s_clip_h = H;

/* ---- colour ---- */
static uint32_t rgb(int r, int g, int b) {
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}
#define R_(c) ((int)(((c) >> 16) & 0xFF))
#define G_(c) ((int)(((c) >> 8) & 0xFF))
#define B_(c) ((int)((c) & 0xFF))

/* the siblings' gradient: rgb(14, 22, 52) at the top to rgb(6, 10, 28) */
static void bg_at(int y, int* r, int* g, int* b) {
    int t = (y * 255) / (H - 1);
    *r = 6 + (8 * (255 - t)) / 255;
    *g = 10 + (12 * (255 - t)) / 255;
    *b = 28 + (24 * (255 - t)) / 255;
}

/* ---- sine table: 1024 steps a turn, Q14 ---- */
#define SIN_N 1024
static int16_t s_sin[SIN_N];
static int s_sin_ready;

static void sin_init(void) {
    int i;
    if (s_sin_ready) return;
    for (i = 0; i < SIN_N; i++) {
        /* x in [-pi, pi): Taylor to x^11, error < 1e-5 there */
        double x = (double)(i < SIN_N / 2 ? i : i - SIN_N) * (6.283185307179586 / SIN_N), x2 = x * x;
        double v = x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72 * (1 - x2 / 110)))));
        s_sin[i] = (int16_t)(v * 16384.0 + (v < 0 ? -0.5 : 0.5));
    }
    s_sin_ready = 1;
}
static int sinq(int phase) { return s_sin[phase & (SIN_N - 1)]; }

/* ---- background: the gradient and the aurora at 160x120, upscaled 4x ---- */
#define LW 160
#define LH 120
#define BANDS 3
static const struct {
    int ax, ay;        /* phase per low-res pixel along the band's normal */
    int wx, wy, wamp;  /* the band's slow bend */
    int speed, wspeed; /* phase steps per second */
    int off;
    int r, g, b;       /* added at full strength, /256 */
} k_band[BANDS] = {
    { 3, 5, 2, -1, 70, 37, 23, 0, 4, 46, 52 },      /* teal */
    { 4, 3, -1, 2, 90, -29, 17, 400, 30, 10, 44 },  /* violet */
    { 2, 6, 3, 1, 60, 21, -31, 760, 6, 20, 48 },    /* navy-blue */
};

static uint8_t s_low[LH][LW][3];

static void aurora(uint32_t t_ms) {
    int x, y, i;
    int base_t[BANDS], wt[BANDS];
    for (i = 0; i < BANDS; i++) {
        base_t[i] = (int)((t_ms * (uint32_t)(k_band[i].speed < 0 ? -k_band[i].speed : k_band[i].speed)) / 1000u);
        if (k_band[i].speed < 0) base_t[i] = -base_t[i];
        wt[i] = (int)((t_ms * (uint32_t)(k_band[i].wspeed < 0 ? -k_band[i].wspeed : k_band[i].wspeed)) / 1000u);
        if (k_band[i].wspeed < 0) wt[i] = -wt[i];
    }
    for (y = 0; y < LH; y++) {
        int br, bgc, bb;
        bg_at(y * 4 + 2, &br, &bgc, &bb);
        /* the bands fade out towards the bottom, where the text sits */
        int fade = y < 60 ? 256 : 256 - ((y - 60) * 160) / 60;
        for (x = 0; x < LW; x++) {
            int r = br << 8, g = bgc << 8, b = bb << 8;
            for (i = 0; i < BANDS; i++) {
                int bend = (sinq(x * k_band[i].wx + y * k_band[i].wy + wt[i]) * k_band[i].wamp) >> 14;
                int s = sinq(x * k_band[i].ax + y * k_band[i].ay + base_t[i] + k_band[i].off + bend);
                int a;
                if (s <= 0) continue;
                a = (s * s) >> 14;          /* Q14, soft */
                a = (a * a) >> 14;          /* narrower */
                a = (a * fade) >> 8;
                r += (k_band[i].r * a) >> 6;  /* full strength: about col */
                g += (k_band[i].g * a) >> 6;
                b += (k_band[i].b * a) >> 6;
            }
            r >>= 8;
            g >>= 8;
            b >>= 8;
            s_low[y][x][0] = (uint8_t)(r > 255 ? 255 : r);
            s_low[y][x][1] = (uint8_t)(g > 255 ? 255 : g);
            s_low[y][x][2] = (uint8_t)(b > 255 ? 255 : b);
        }
    }
}

/* one low-res row widened to 640 (horizontal interpolation), 3 channels */
static void widen(int ly, int16_t out[W][3]) {
    int x;
    if (ly >= LH) ly = LH - 1;
    for (x = 0; x < W; x++) {
        int lx = x >> 2, f = x & 3, lx1 = lx + 1 < LW ? lx + 1 : lx, c;
        for (c = 0; c < 3; c++) out[x][c] = (int16_t)((s_low[ly][lx][c] * (4 - f) + s_low[ly][lx1][c] * f) >> 2);
    }
}

static void background(uint32_t* fb, int pitch, uint32_t t_ms) {
    static int16_t row0[W][3], row1[W][3];
    int ly, x, k;
    aurora(t_ms);
    widen(0, row1);
    for (ly = 0; ly < LH; ly++) {
        memcpy(row0, row1, sizeof row0);
        widen(ly + 1, row1);
        for (k = 0; k < 4; k++) {
            uint32_t* d = fb + (size_t)(ly * 4 + k) * (size_t)pitch;
            for (x = 0; x < W; x++)
                d[x] = 0xFF000000u | ((uint32_t)((row0[x][0] * (4 - k) + row1[x][0] * k) >> 2) << 16) |
                       ((uint32_t)((row0[x][1] * (4 - k) + row1[x][1] * k) >> 2) << 8) |
                       (uint32_t)((row0[x][2] * (4 - k) + row1[x][2] * k) >> 2);
        }
    }
}

/* ---- sparks: at most 48, rising slowly, pale teal, additive ---- */
#define SPARKS 48
static void add_px(uint32_t* fb, int pitch, int x, int y, int r, int g, int b) {
    uint32_t* p;
    if (x < 0 || x >= s_clip_w || y < 0 || y >= s_clip_h) return;
    p = fb + (size_t)y * (size_t)pitch + x;
    r += R_(*p);
    g += G_(*p);
    b += B_(*p);
    /* overlapping sparks stay pale teal, inside the palette (test_splash.c) */
    *p = rgb(r > 110 ? 110 : r, g > 210 ? 210 : g, b > 220 ? 220 : b);
}

static void sparks(uint32_t* fb, int pitch, uint32_t t_ms) {
    uint32_t seed = 0x9E3779B9u;
    int i;
    for (i = 0; i < SPARKS; i++) {
        uint32_t span = H + 40, speed, x0, ph, y, life;
        int x, a, dx, dy;
        seed = seed * 1664525u + 1013904223u;
        x0 = (seed >> 8) % W;
        seed = seed * 1664525u + 1013904223u;
        speed = 6 + ((seed >> 8) % 14);          /* px per second */
        seed = seed * 1664525u + 1013904223u;
        ph = (seed >> 8) % span;
        y = (t_ms / 1000u * speed + ((t_ms % 1000u) * speed) / 1000u + ph) % span;
        life = y;                                 /* 0 at the bottom edge .. span at the top */
        x = (int)x0 + (sinq((int)(t_ms / 8u) + i * 97) * 6 >> 14);
        y = (uint32_t)(H + 20) - y;
        /* brightest in the middle of the climb, gone at both ends */
        a = (int)(life < span / 2 ? life : span - life) * 2 * 48 / (int)span;
        for (dy = -1; dy <= 1; dy++)
            for (dx = -1; dx <= 1; dx++) {
                int k = (dx == 0 && dy == 0) ? 4 : (dx == 0 || dy == 0) ? 2 : 1;
                add_px(fb, pitch, x + dx, (int)y + dy, a * k * 2 / 8, a * k * 5 / 8, a * k * 6 / 8);
            }
    }
}

/* ---- text ---- */
int xhw_splash_text_width(const char* s, int zoom) { return (int)strlen(s) * GW * zoom; }

static void glyphs(uint32_t* fb, int pitch, const char* s, int x, int y, int zoom, uint32_t col, int sweep_x,
                   int sweep_w) {
    for (; *s; s++, x += GW * zoom) {
        const unsigned char* g = xhw_font16 + (unsigned char)*s * GH;
        int gy, gx, zy, zx;
        for (gy = 0; gy < GH; gy++)
            for (gx = 0; gx < GW; gx++) {
                if (!(g[gy] & (0x80 >> gx))) continue;
                for (zy = 0; zy < zoom; zy++) {
                    int py = y + gy * zoom + zy;
                    if (py < 0 || py >= s_clip_h) continue;
                    for (zx = 0; zx < zoom; zx++) {
                        int px = x + gx * zoom + zx;
                        uint32_t c = col;
                        if (px < 0 || px >= s_clip_w) continue;
                        if (sweep_w) {   /* the light sweep: a diagonal band, brightest in its middle */
                            int d = (px + (py - y) / 2) - sweep_x;
                            if (d < 0) d = -d;
                            if (d < sweep_w) {
                                int k = 255 - d * 255 / sweep_w;
                                c = rgb(R_(col) + ((255 - R_(col)) * k >> 8) + k / 16,
                                        G_(col) + ((255 - G_(col)) * k >> 8) + k / 16,
                                        B_(col) + ((255 - B_(col)) * k >> 8) + k / 8);
                            }
                        }
                        fb[(size_t)py * (size_t)pitch + px] = c;
                    }
                }
            }
    }
}

static void text(uint32_t* fb, int pitch, const char* s, int x, int y, int zoom, uint32_t col) {
    glyphs(fb, pitch, s, x, y, zoom, col, 0, 0);
}

/* centred, with a soft shadow so it reads over the aurora */
static void text_c(uint32_t* fb, int pitch, const char* s, int y, int zoom, uint32_t col) {
    int x = (W - xhw_splash_text_width(s, zoom)) / 2;
    glyphs(fb, pitch, s, x + 1, y + 1, zoom, rgb(2, 4, 12), 0, 0);
    glyphs(fb, pitch, s, x, y, zoom, col, 0, 0);
}

static void fill(uint32_t* fb, int pitch, int x, int y, int w, int h, uint32_t col) {
    int i, j;
    for (j = y; j < y + h; j++) {
        if (j < 0 || j >= s_clip_h) continue;
        for (i = x; i < x + w; i++)
            if (i >= 0 && i < s_clip_w) fb[(size_t)j * (size_t)pitch + i] = col;
    }
}

/* ---- layout (PLAN.md 5.5; test_splash.c checks it against the safe area) ---- */
#define WORDMARK_Y 104
#define CARD_Y 184
#define STAGE_Y 292
#define BAR_Y 314
#define VIDEO_Y 350
#define LINES_Y 248
#define FOOTER_Y 404

const char* xhw_splash_hold_text(int hold, int holding) {
    int h = holding > hold ? holding : hold;
    if (h >= XHW_HOLD_SAFE_RESET) return "Safe video and default settings";
    if (h == XHW_HOLD_SAFE) return "Safe video: this boot runs 480i 4:3 and saves it";
    return "Hold L + R for 480i (safe video)";
}

const char* xhw_splash_settings_text(int settings) {
    return settings >= 2 ? "Settings open before the game starts" : settings ? "Keep holding for settings"
                                                                             : "Hold BACK for settings";
}

static void wordmark(uint32_t* fb, int pitch, const xhw_splash_state* s) {
    const char* w = s->wordmark ? s->wordmark : "TechProGabe Presents...";
    int zoom = XHW_SPLASH_WORDMARK_ZOOM, tw = xhw_splash_text_width(w, zoom), x = (W - tw) / 2;
    uint32_t t = s->still ? 100000u : s->t_ms;
    int v = t >= 500 ? 236 : 40 + (int)(196 * t / 500);   /* the sibling's fade, settled a touch under white */
    uint32_t col = rgb(v, v, v + (t >= 500 ? 8 : 0));
    int sweep_w = 0, sweep_x = 0;
    if (t >= 500 && t < 1200) {   /* one light sweep across the glyphs, ~700 ms */
        sweep_w = 40;
        sweep_x = x - sweep_w + (int)((t - 500) * (uint32_t)(tw + 2 * sweep_w) / 700u);
    }
    if (t >= 500) glyphs(fb, pitch, w, x + 2, WORDMARK_Y + 2, zoom, rgb(2, 4, 12), 0, 0);
    glyphs(fb, pitch, w, x, WORDMARK_Y, zoom, col, sweep_x, sweep_w);
}

/* the card title's zoom: 2x when it fits the safe width next to the icon, else 1x */
int xhw_splash_title_zoom(const char* title, int icon) {
    int room = XHW_SPLASH_W - 2 * XHW_SPLASH_SAFE - (icon ? 64 + 16 : 0);
    return xhw_splash_text_width(title, XHW_SPLASH_TITLE_ZOOM) <= room ? XHW_SPLASH_TITLE_ZOOM : 1;
}

static void card(uint32_t* fb, int pitch, const xhw_splash_state* s) {
    int tz = xhw_splash_title_zoom(s->card_title, s->icon != NULL), tw = xhw_splash_text_width(s->card_title, tz);
    int sw = xhw_splash_text_width(s->card_sub, 1), textw = tw > sw ? tw : sw;
    int iw = s->icon ? 64 + 16 : 0, x = (W - (iw + textw)) / 2, tx = x + iw;
    if (s->icon) {
        int i, j;
        for (j = 0; j < 64; j++)
            for (i = 0; i < 64; i++) {
                uint32_t c = s->icon[j * 64 + i], a = c >> 24;
                uint32_t* d = fb + (size_t)(CARD_Y + j) * (size_t)pitch + x + i;
                if (!a) continue;
                *d = rgb((R_(c) * (int)a + R_(*d) * (int)(255 - a)) / 255, (G_(c) * (int)a + G_(*d) * (int)(255 - a)) / 255,
                         (B_(c) * (int)a + B_(*d) * (int)(255 - a)) / 255);
            }
    }
    if (!s->icon) tx = (W - tw) / 2;
    glyphs(fb, pitch, s->card_title, tx + 1, CARD_Y + 8 + 1, tz, rgb(2, 4, 12), 0, 0);
    glyphs(fb, pitch, s->card_title, tx, CARD_Y + 8, tz, rgb(235, 238, 248), 0, 0);
    if (s->card_sub[0]) {
        int sx = s->icon ? tx : (W - sw) / 2;
        text(fb, pitch, s->card_sub, sx, CARD_Y + 8 + GH * tz + 6, 1, rgb(150, 165, 190));
    }
}

static void bar(uint32_t* fb, int pitch, const xhw_splash_state* s) {
    float f = s->progress < 0 ? 0 : s->progress > 1 ? 1 : s->progress;
    int fillw = (int)(f * XHW_SPLASH_BAR_W), x, y;
    int hl = s->still ? -1000 : (int)((s->t_ms / 4u) % (uint32_t)(XHW_SPLASH_BAR_W + 80)) - 40;
    for (y = 0; y < XHW_SPLASH_BAR_H; y++) {
        uint32_t* row = fb + (size_t)(BAR_Y + y) * (size_t)pitch + XHW_SPLASH_BAR_X;
        /* the sibling's rgb(120, 200, 110) in the middle row, lighter above, deeper below */
        int dr = (3 - y) * 8, dg = (3 - y) * 6, db = (3 - y) * 7;
        for (x = 0; x < XHW_SPLASH_BAR_W; x++) {
            if (x < fillw) {
                int d = x - hl, k = 0;
                if (d < 0) d = -d;
                if (d < 40 && x < fillw) k = (40 - d) * 2;
                row[x] = rgb(120 + dr + k / 2, 200 + dg + k / 3, 110 + db + k / 2);
            } else {
                row[x] = rgb(30, 40, 60);
            }
        }
    }
}

static void classic_render(uint32_t* fb, int pitch, const xhw_splash_state* s) {
    int layers = s->layers ? s->layers : XHW_SPLASH_ALL;
    sin_init();
    if (layers & XHW_SPLASH_BG) {
        background(fb, pitch, s->still ? 0 : s->t_ms);
        sparks(fb, pitch, s->still ? 4321 : s->t_ms);
    } else {   /* the foreground alone, over the plain gradient (the host test's diff) */
        int y, r, g, b;
        for (y = 0; y < H; y++) {
            bg_at(y, &r, &g, &b);
            fill(fb, pitch, 0, y, W, 1, rgb(r, g, b));
        }
    }
    if (!(layers & XHW_SPLASH_FG)) return;
    wordmark(fb, pitch, s);
    if (s->card_title[0]) card(fb, pitch, s);
    if (s->version[0])
        text(fb, pitch, s->version, W - XHW_SPLASH_SAFE - xhw_splash_text_width(s->version, 1), H - XHW_SPLASH_SAFE - GH,
             1, rgb(90, 100, 125));
    if (s->card) {
        int i;
        for (i = 0; i < XHW_CARD_LINES; i++)
            if (s->card_lines[i][0]) text_c(fb, pitch, s->card_lines[i], LINES_Y + i * 18, 1, rgb(200, 208, 225));
        if (s->card_footer) text_c(fb, pitch, s->card_footer, FOOTER_Y, 1, rgb(110, 120, 140));
        return;
    }
    if (s->stage[0]) text(fb, pitch, s->stage, XHW_SPLASH_BAR_X, STAGE_Y, 1, rgb(185, 195, 215));
    if (s->stage_ms[0])   /* under the bar's right end: the label above may run past the bar */
        text(fb, pitch, s->stage_ms, XHW_SPLASH_BAR_X + XHW_SPLASH_BAR_W - xhw_splash_text_width(s->stage_ms, 1),
             BAR_Y + XHW_SPLASH_BAR_H + 4, 1, rgb(110, 120, 140));
    bar(fb, pitch, s);
    if (s->video[0]) text_c(fb, pitch, s->video, VIDEO_Y, 1, rgb(150, 160, 180));
    {
        int h = s->holding > s->hold ? s->holding : s->hold;
        uint32_t col = h == XHW_HOLD_NONE ? rgb(110, 120, 140) : s->hold ? rgb(255, 236, 150) : rgb(230, 220, 170);
        text_c(fb, pitch, xhw_splash_hold_text(s->hold, s->holding), VIDEO_Y + 20, 1, col);
    }
    if (s->video_note[0]) text_c(fb, pitch, s->video_note, VIDEO_Y + 40, 1, rgb(220, 200, 120));
}

/* ---- the deep-space title card (PLAN.md 5.5; the maintainer's pick of
 * 2026-10-18, "A2" of the mockups) -------------------------------------
 *
 *   sky         a radial navy, three nebula clouds (violet, a cool magenta,
 *               teal) breathing and drifting slowly, computed at 160x120
 *               and upscaled like the aurora; ~120 stars drifting left at
 *               three depths and twinkling, each two lines tall (a 1-line
 *               dot twitters at 480i); a small constellation top right; a
 *               shooting star every few seconds
 *   planet      a large planet's limb across the bottom with a blue rim of
 *               atmosphere (static: drawn once into a cached layer)
 *   top row     "TECHPROGABE PRESENTS" (the family's wordmark, now a label),
 *               the N64Recomp-X mark and name right: the platform, never
 *               the game's art
 *   title       "NOW STARTING", the game's title in Sora Bold with a soft
 *               glow, chips: the ROM's code, region and revision; its size
 *               and where it is (inside this XBE or beside it); "SHA-1
 *               verified" once it is
 *   progress    the stage label, "step n of 4", a slim bar with a glowing
 *               head
 *   footer      the video mode; "Hold L R for 480i" (safe video); "Hold
 *               BACK for settings" (the BACK menu before the game starts);
 *               what a hold has latched (maintainer, 2026-10-18: the
 *               triggers are safe video, BACK the menu, both held)
 *
 * Card mode: the title is the card's, the lines below it, the footer line.
 * The type is Sora (SIL OFL 1.1), baked by tools/xbox/make_fonts.py into
 * 4-bit coverage (xhw_fonts.inc). -DXHW_SPLASH_CLASSIC=1 draws the earlier
 * unscii card instead. */
typedef struct {
    uint8_t w, h;
    int8_t x, y;      /* the box against the pen on the baseline */
    uint16_t adv;     /* quarter pixels */
    uint32_t off;
} xf_glyph;
typedef struct {
    int size, ascent, descent;
    const xf_glyph* g;
    const uint8_t* bits;
} xf_font;
#include "xhw_fonts.inc"

#ifndef XHW_SPLASH_CLASSIC
#define XHW_SPLASH_CLASSIC 0
#endif

static const xf_font* const k_faces[] = { &xf_label, &xf_body, &xf_stage, &xf_title2, &xf_title };

/* the next codepoint of a UTF-8 string (Latin-1 is all the faces hold; a
 * byte that is not UTF-8 is taken as Latin-1) */
static int utf8_next(const char** sp) {
    const unsigned char* s = (const unsigned char*)*sp;
    int c = *s;
    if (c >= 0xC0 && c < 0xE0 && (s[1] & 0xC0) == 0x80) {
        c = ((c & 0x1F) << 6) | (s[1] & 0x3F);
        *sp += 2;
    } else if (c >= 0xE0 && c < 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        c = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        *sp += 3;
    } else {
        *sp += 1;
    }
    return c;
}

static const xf_glyph* xf_get(const xf_font* f, int c) {
    if (c == 0x2019 || c == 0x2018) c = '\'';
    if (c == 0x2013 || c == 0x2014) c = '-';
    if (c < 32 || c > 255) c = '?';
    return &f->g[c - 32];
}

/* width in pixels, `track` extra pixels after every glyph but the last */
static int xf_width(const xf_font* f, const char* s, int track) {
    int q = 0, n = 0;
    while (*s) {
        q += xf_get(f, utf8_next(&s))->adv;
        n++;
    }
    return (q + 2) / 4 + (n > 1 ? (n - 1) * track : 0);
}

int xhw_splash_face_width(int face, const char* utf8, int track) {
    return xf_width(k_faces[face < 0 || face > XHW_FACE_TITLE ? 0 : face], utf8, track);
}

/* blend col over *p at coverage a (0..255) */
static void blend(uint32_t* p, uint32_t col, int a) {
    uint32_t c = *p;
    if (a <= 0) return;
    if (a >= 255) {
        *p = col | 0xFF000000u;
        return;
    }
    *p = 0xFF000000u | ((uint32_t)((R_(col) * a + R_(c) * (256 - a)) >> 8) << 16) |
         ((uint32_t)((G_(col) * a + G_(c) * (256 - a)) >> 8) << 8) | (uint32_t)((B_(col) * a + B_(c) * (256 - a)) >> 8);
}

/* additive, clamped (glows, stars) */
static void add(uint32_t* p, int r, int g, int b) {
    uint32_t c = *p;
    r += R_(c), g += G_(c), b += B_(c);
    *p = rgb(r, g, b);
}

/* a string with its pen starting at x, its baseline at y; alpha 0..256; returns the end x */
static int xf_draw(uint32_t* fb, int pitch, const xf_font* f, const char* s, int x, int y, uint32_t col, int track,
                   int alpha) {
    int q = x * 4;
    while (*s) {
        const xf_glyph* g = xf_get(f, utf8_next(&s));
        int gx = (q + 2) / 4 + g->x, gy = y + g->y, i, j, rowb = (g->w + 1) / 2;
        const uint8_t* b = f->bits + g->off;
        for (j = 0; j < g->h; j++) {
            int py = gy + j;
            if (py < 0 || py >= s_clip_h) continue;
            for (i = 0; i < g->w; i++) {
                int px = gx + i, v = b[j * rowb + (i >> 1)];
                v = (i & 1) ? v & 15 : v >> 4;
                if (!v || px < 0 || px >= s_clip_w) continue;
                blend(fb + (size_t)py * (size_t)pitch + px, col, v * 17 * alpha >> 8);
            }
        }
        q += g->adv + track * 4;
    }
    return (q + 2) / 4;
}

/* coverage of a filled rounded rectangle, antialiased at the corners */
static void rrect(uint32_t* fb, int pitch, int x, int y, int w, int h, int r, uint32_t col, int alpha) {
    int i, j;
    for (j = 0; j < h; j++) {
        int py = y + j;
        if (py < 0 || py >= s_clip_h) continue;
        for (i = 0; i < w; i++) {
            int px = x + i, a = alpha, dx = 0, dy = 0;
            if (px < 0 || px >= s_clip_w) continue;
            if (i < r) dx = r - i;
            else if (i >= w - r) dx = i - (w - r) + 1;
            if (j < r) dy = r - j;
            else if (j >= h - r) dy = j - (h - r) + 1;
            if (dx && dy) {   /* in a corner: the distance to its centre, in 1/16 px */
                int d2 = (dx * 16 - 8) * (dx * 16 - 8) + (dy * 16 - 8) * (dy * 16 - 8), rr = r * 16;
                if (d2 >= (rr + 8) * (rr + 8)) continue;
                if (d2 > (rr - 8) * (rr - 8)) {
                    int d = (int)__builtin_sqrtf((float)d2);
                    a = a * (rr + 8 - d) / 16;
                }
            }
            blend(fb + (size_t)py * (size_t)pitch + px, col, a);
        }
    }
}

/* an antialiased line `wid` pixels wide, round-ended */
static void aa_line(uint32_t* fb, int pitch, float x0, float y0, float x1, float y1, float wid, uint32_t col, int alpha) {
    int xa = (int)(x0 < x1 ? x0 : x1) - 3, xb = (int)(x0 > x1 ? x0 : x1) + 3;
    int ya = (int)(y0 < y1 ? y0 : y1) - 3, yb = (int)(y0 > y1 ? y0 : y1) + 3, x, y;
    float dx = x1 - x0, dy = y1 - y0, l2 = dx * dx + dy * dy, hw = wid * 0.5f;
    for (y = ya; y <= yb; y++) {
        if (y < 0 || y >= s_clip_h) continue;
        for (x = xa; x <= xb; x++) {
            float px = x + 0.5f - x0, py = y + 0.5f - y0, t = l2 > 0 ? (px * dx + py * dy) / l2 : 0, ex, ey, d;
            if (x < 0 || x >= s_clip_w) continue;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            ex = px - t * dx, ey = py - t * dy;
            d = __builtin_sqrtf(ex * ex + ey * ey) - hw;
            if (d >= 0.5f) continue;
            blend(fb + (size_t)y * (size_t)pitch + x, col, d <= -0.5f ? alpha : (int)((0.5f - d) * (float)alpha));
        }
    }
}

static void disc(uint32_t* fb, int pitch, float cx, float cy, float r, uint32_t col, int alpha) {
    aa_line(fb, pitch, cx, cy, cx, cy, 2 * r, col, alpha);
}

/* ---- the sky at 160x120 ---- */
static const struct {
    int cx, cy, rx, ry;   /* 640x480 pixels */
    int cs, sn;           /* the ellipse's rotation, Q14 */
    int r, g, b, a;       /* colour, strength /256 */
    int drift, period;    /* px, ms */
} k_cloud[3] = {
    { 140, 300, 280, 150, 15582, -5063, 120, 80, 230, 112, 14, 23000 },   /* violet, turned -18 degrees */
    { 370, 140, 190, 100, 15897, 3964, 200, 90, 220, 52, 10, 31000 },    /* cool magenta, +14 */
    { 590, 60, 230, 140, 16384, 0, 40, 190, 170, 80, 12, 27000 },        /* teal */
};

static void space_sky(uint32_t t_ms) {
    int x, y, i;
    int cx[3], cy[3], amp[3];
    for (i = 0; i < 3; i++) {
        int ph = (int)((t_ms % (uint32_t)k_cloud[i].period) * (uint32_t)SIN_N / (uint32_t)k_cloud[i].period);
        cx[i] = k_cloud[i].cx + ((sinq(ph) * k_cloud[i].drift) >> 14);
        cy[i] = k_cloud[i].cy + ((sinq(ph + SIN_N / 4) * k_cloud[i].drift / 2) >> 14);
        amp[i] = k_cloud[i].a * (240 + ((sinq(ph * 2 + i * 300) * 24) >> 14)) >> 8;   /* breathing, +-10 % */
    }
    for (y = 0; y < LH; y++) {
        for (x = 0; x < LW; x++) {
            int px = x * 4 + 2, py = y * 4 + 2, r, g, b;
            /* the base: radial from (20 %, 0), 130 % x 100 %: #10284a -> #070f24 at 45 % -> #02040c */
            int dx = (px - 128) * 1000 / 832, dy = py * 1000 / 480;
            int d = (int)__builtin_sqrtf((float)(dx * dx + dy * dy));   /* 0..~1300 */
            if (d < 450) r = 16 - 9 * d / 450, g = 40 - 25 * d / 450, b = 74 - 38 * d / 450;
            else if (d < 1000) r = 7 - 5 * (d - 450) / 550, g = 15 - 11 * (d - 450) / 550, b = 36 - 24 * (d - 450) / 550;
            else r = 2, g = 4, b = 12;
            r <<= 8, g <<= 8, b <<= 8;
            for (i = 0; i < 3; i++) {   /* each cloud: closest-side falloff, rotated */
                int ex = px - cx[i], ey = py - cy[i];
                int u = (ex * k_cloud[i].cs + ey * k_cloud[i].sn) >> 14, v = (ey * k_cloud[i].cs - ex * k_cloud[i].sn) >> 14;
                int q = (u * 256 / k_cloud[i].rx) * (u * 256 / k_cloud[i].rx) + (v * 256 / k_cloud[i].ry) * (v * 256 / k_cloud[i].ry);
                int k;
                if (q >= 65536) continue;
                k = 256 - (int)__builtin_sqrtf((float)q);   /* 1 - distance, 0..256 */
                k = k * k >> 8;                             /* softer edge */
                k = k * amp[i] >> 8;
                r += k_cloud[i].r * k, g += k_cloud[i].g * k, b += k_cloud[i].b * k;
            }
            r >>= 8, g >>= 8, b >>= 8;
            if (b < r) b = r;   /* the sky stays cool */
            s_low[y][x][0] = (uint8_t)(r > 255 ? 255 : r);
            s_low[y][x][1] = (uint8_t)(g > 255 ? 255 : g);
            s_low[y][x][2] = (uint8_t)(b > 255 ? 255 : b);
        }
    }
}

static void space_background(uint32_t* fb, int pitch, uint32_t t_ms) {
    static int16_t row0[W][3], row1[W][3];
    int ly, x, k;
    space_sky(t_ms);
    widen(0, row1);
    for (ly = 0; ly < LH; ly++) {
        memcpy(row0, row1, sizeof row0);
        widen(ly + 1, row1);
        for (k = 0; k < 4; k++) {
            uint32_t* d = fb + (size_t)(ly * 4 + k) * (size_t)pitch;
            for (x = 0; x < W; x++)
                d[x] = 0xFF000000u | ((uint32_t)((row0[x][0] * (4 - k) + row1[x][0] * k) >> 2) << 16) |
                       ((uint32_t)((row0[x][1] * (4 - k) + row1[x][1] * k) >> 2) << 8) |
                       (uint32_t)((row0[x][2] * (4 - k) + row1[x][2] * k) >> 2);
        }
    }
}

/* ---- stars: three depths drifting left, twinkling; two lines tall ---- */
#define STARS 120
static int16_t s_horizon[W];   /* the planet's top edge per column, less its glow (planet_make) */

static void stars(uint32_t* fb, int pitch, uint32_t t_ms) {
    uint32_t seed = 0x51A7F00Du;
    int i;
    for (i = 0; i < STARS; i++) {
        int x0, y, depth, bright, tw, x, a, cool;
        seed = seed * 1664525u + 1013904223u;
        x0 = (int)((seed >> 8) % W);
        seed = seed * 1664525u + 1013904223u;
        y = (int)((seed >> 8) % 400);
        seed = seed * 1664525u + 1013904223u;
        depth = (int)((seed >> 8) % 8);                      /* 0..7: 0..4 far, 5..6 mid, 7 near */
        seed = seed * 1664525u + 1013904223u;
        bright = depth == 7 ? 230 : depth >= 5 ? 170 : 70 + (int)((seed >> 8) % 90);
        cool = (int)((seed >> 20) & 31);                     /* a touch of blue in some */
        /* drift: 1, 2 or 4 px a second by depth */
        x = x0 - (int)(t_ms * (uint32_t)(depth == 7 ? 4 : depth >= 5 ? 2 : 1) / 1000u);
        x = ((x % W) + W) % W;
        if (y + 2 >= s_horizon[x]) continue;   /* behind the planet */
        tw = sinq((int)(t_ms * (uint32_t)(40 + i % 50) / 1000u) * 8 + i * 157);
        a = bright * (220 + ((tw * 36) >> 14)) >> 8;         /* +-15 % */
        {
            int r = a - cool, g = a - cool / 2, b = a;
            uint32_t* p = fb + (size_t)y * (size_t)pitch + x;
            add(p, r, g, b);
            add(p + pitch, r * 3 / 4, g * 3 / 4, b * 3 / 4);
            if (depth == 7) {   /* the near ones: a soft cross */
                if (x > 0) add(p - 1, r / 3, g / 3, b / 3), add(p + pitch - 1, r / 4, g / 4, b / 4);
                if (x < W - 1) add(p + 1, r / 3, g / 3, b / 3), add(p + pitch + 1, r / 4, g / 4, b / 4);
                if (y > 0) add(p - pitch, r / 4, g / 4, b / 4);
                if (y + 2 < H) add(p + 2 * pitch, r / 5, g / 5, b / 5);
            }
        }
    }
}

/* the constellation top right, and a shooting star now and then */
static void constellation(uint32_t* fb, int pitch) {
    static const float k_pt[6][3] = { { 448, 156, 2.2f }, { 492, 128, 2.6f }, { 536, 140, 2.0f },
                                      { 568, 96, 3.0f },  { 602, 84, 2.0f },  { 580, 168, 2.2f } };
    static const int k_link[5][2] = { { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 4 }, { 2, 5 } };
    int i;
    for (i = 0; i < 5; i++)
        aa_line(fb, pitch, k_pt[k_link[i][0]][0], k_pt[k_link[i][0]][1], k_pt[k_link[i][1]][0], k_pt[k_link[i][1]][1],
                1.5f, rgb(127, 216, 200), 76);
    for (i = 0; i < 6; i++) {
        disc(fb, pitch, k_pt[i][0], k_pt[i][1], k_pt[i][2] + 2.5f, rgb(120, 170, 230), 40);
        disc(fb, pitch, k_pt[i][0], k_pt[i][1], k_pt[i][2], i & 1 ? rgb(255, 255, 255) : rgb(207, 233, 255), 255);
    }
}

/* a shooting star now and then: every 9 s, 700 ms, down to the left */
static void shooting_star(uint32_t* fb, int pitch, uint32_t t_ms, int still) {
    if (!still) {   /* every 9 s, 700 ms: from the upper right, down to the left */
        uint32_t ph = t_ms % 9000u, n = t_ms / 9000u;
        if (t_ms > 2500u && ph < 700u) {
            float x0 = 430.0f + (float)(n * 53u % 120u), y0 = 30.0f + (float)(n * 31u % 40u);
            float k = (float)ph / 700.0f, hx = x0 - 220.0f * k, hy = y0 + 98.0f * k;
            int fade = ph < 100u ? (int)ph * 255 / 100 : ph > 550u ? (int)(700u - ph) * 255 / 150 : 255, s;
            for (s = 0; s < 10; s++) {   /* the tail: ten pieces fading back */
                float a = s / 10.0f, b = (s + 1) / 10.0f;
                aa_line(fb, pitch, hx + 110.0f * a, hy - 49.0f * a, hx + 110.0f * b, hy - 49.0f * b, 2.0f - a,
                        rgb(220, 235, 255), fade * (10 - s) / 10);
            }
            disc(fb, pitch, hx, hy, 1.6f, rgb(255, 255, 255), fade);
        }
    }
}

/* ---- the planet: cached, blended over the sky ---- */
#define PL_TOP 336
#define PL_CX 320
#define PL_CY (372 + 740)
#define PL_R 740
static uint32_t s_planet[H - PL_TOP][W];   /* colour with its coverage in alpha */
static int s_planet_ready;

static uint32_t lerp3(int r0, int g0, int b0, int r1, int g1, int b1, int k) {   /* k 0..256 */
    return rgb(r0 + (r1 - r0) * k / 256, g0 + (g1 - g0) * k / 256, b0 + (b1 - b0) * k / 256);
}

static void planet_make(void) {
    int x, y;
    for (y = PL_TOP; y < H; y++)
        for (x = 0; x < W; x++) {
            float dx = x + 0.5f - PL_CX, dy = y + 0.5f - PL_CY, d = __builtin_sqrtf(dx * dx + dy * dy) - PL_R;
            uint32_t c = 0;
            if (d < 0.5f) {   /* the body: lit from the top, #1a3d63 -> #0b1c36 -> #040a17 */
                float gx = x + 0.5f - PL_CX, gy = y + 0.5f - 431.0f, gd = __builtin_sqrtf(gx * gx + gy * gy);
                int cov = d <= -0.5f ? 255 : (int)((0.5f - d) * 255.0f);
                if (gd < 90.0f) c = lerp3(26, 61, 99, 11, 28, 54, (int)(gd * 256.0f / 90.0f));
                else if (gd < 270.0f) c = lerp3(11, 28, 54, 4, 10, 23, (int)((gd - 90.0f) * 256.0f / 180.0f));
                else c = rgb(4, 10, 23);
                if (d > -3.0f) {   /* the rim, bright just inside the edge */
                    int k = (int)((3.0f + d) * 70.0f);
                    c = rgb(R_(c) + 100 * k / 255, G_(c) + 190 * k / 255, B_(c) + 205 * k / 255);
                }
                c = (c & 0xFFFFFFu) | ((uint32_t)cov << 24);
            } else if (d < 22.0f) {   /* the atmosphere's glow outside */
                float k = 1.0f - d / 22.0f;
                c = ((uint32_t)(k * k * 115.0f) << 24) | 0x5AC8FFu;
            }
            s_planet[y - PL_TOP][x] = c;
        }
    for (x = 0; x < W; x++) {
        float dx = x + 0.5f - PL_CX;
        s_horizon[x] = (int16_t)(PL_CY - __builtin_sqrtf((float)PL_R * PL_R - dx * dx) - 22.0f);
    }
    s_planet_ready = 1;
}

static void planet(uint32_t* fb, int pitch) {
    int x, y;
    if (!s_planet_ready) planet_make();
    for (y = PL_TOP; y < H; y++) {
        uint32_t* d = fb + (size_t)y * (size_t)pitch;
        const uint32_t* s = s_planet[y - PL_TOP];
        for (x = 0; x < W; x++)
            if (s[x] >> 24) blend(&d[x], s[x], (int)(s[x] >> 24));
    }
}

/* ---- the title's glow: its coverage blurred once per title ---- */
#define GLOW_W 576
#define GLOW_H 80
#define GLOW_R 8
static uint8_t s_glows[2][GLOW_H][GLOW_W];   /* two: a title on two lines */
static char s_glow_for[2][64];
static const xf_font* s_glow_face[2];
static int s_glow_next;

static void glow_make(uint8_t s_glow[GLOW_H][GLOW_W], const xf_font* f, const char* title) {
    static uint32_t tmp[GLOW_H][GLOW_W];
    static uint16_t acc[GLOW_H][GLOW_W];
    int x, y, pass, sw = s_clip_w, sh = s_clip_h;
    memset(tmp, 0, sizeof tmp);
    s_clip_w = GLOW_W, s_clip_h = GLOW_H;
    xf_draw(&tmp[0][0], GLOW_W, f, title, GLOW_R * 2, GLOW_H - 24, 0xFFFFFFu, 0, 256);
    s_clip_w = sw, s_clip_h = sh;
    for (y = 0; y < GLOW_H; y++)
        for (x = 0; x < GLOW_W; x++) s_glow[y][x] = (uint8_t)(tmp[y][x] & 0xFF);
    for (pass = 0; pass < 2; pass++) {   /* two box passes each way: about a gaussian */
        for (y = 0; y < GLOW_H; y++) {
            int sum = 0;
            for (x = -GLOW_R; x < GLOW_W + GLOW_R; x++) {
                int in = x + GLOW_R < GLOW_W ? s_glow[y][x + GLOW_R] : 0, out = x - GLOW_R - 1 >= 0 ? s_glow[y][x - GLOW_R - 1] : 0;
                if (x + GLOW_R < 0) in = 0;
                sum += in - out;
                if (x >= 0 && x < GLOW_W) acc[y][x] = (uint16_t)(sum / (2 * GLOW_R + 1));
            }
        }
        for (x = 0; x < GLOW_W; x++) {
            int sum = 0;
            for (y = -GLOW_R; y < GLOW_H + GLOW_R; y++) {
                int in = y + GLOW_R < GLOW_H && y + GLOW_R >= 0 ? acc[y + GLOW_R][x] : 0;
                int out = y - GLOW_R - 1 >= 0 ? acc[y - GLOW_R - 1][x] : 0;
                sum += in - out;
                if (y >= 0 && y < GLOW_H) s_glow[y][x] = (uint8_t)(sum / (2 * GLOW_R + 1));
            }
        }
    }
}

/* the glow under a title whose pen starts at x with its baseline at y */
static void glow(uint32_t* fb, int pitch, const xf_font* f, const char* title, int x, int y, int alpha) {
    int gx0 = x - GLOW_R * 2, gy0 = y - (GLOW_H - 24), i, j, k0;
    uint8_t (*s_glow)[GLOW_W];
    for (k0 = 0; k0 < 2 && (s_glow_face[k0] != f || strcmp(s_glow_for[k0], title)); k0++) {}
    if (k0 == 2) {
        k0 = s_glow_next, s_glow_next ^= 1;
        glow_make(s_glows[k0], f, title);
        snprintf(s_glow_for[k0], sizeof s_glow_for[k0], "%s", title);
        s_glow_face[k0] = f;
    }
    s_glow = s_glows[k0];
    for (j = 0; j < GLOW_H; j++) {
        int py = gy0 + j;
        if (py < 0 || py >= s_clip_h) continue;
        for (i = 0; i < GLOW_W; i++) {
            int px = gx0 + i, k = s_glow[j][i];
            if (!k || px < 0 || px >= s_clip_w) continue;
            k = (k * 90 >> 8) * alpha >> 8;   /* rgba(127, 180, 255, 0.35) */
            add(fb + (size_t)py * (size_t)pitch + px, 127 * k >> 8, 180 * k >> 8, 255 * k >> 8);
        }
    }
}

/* ---- the foreground ---- */
#define SP_X0 48             /* the text's left and right edges */
#define SP_X1 592
#define SP_TOP_Y 48          /* baselines */
#define SP_EYEBROW_Y 146
#define SP_TITLE_Y 198
#define SP_CHIP_Y 222        /* the chips' top */
#define SP_CHIP_H 24
#define SP_STAGE_Y 318
#define SP_BAR_Y 330
#define SP_BAR_H 6
#define SP_NOTE_Y 388
#define SP_FOOT_Y 428
#define SP_LINES_Y 236
#define SP_LINE_H 19

/* 0..256: how far a part that appeared at t0 has faded in */
static int appear(const xhw_splash_state* s, uint32_t t0, uint32_t dur) {
    if (s->still || s->t_ms >= t0 + dur) return 256;
    if (s->t_ms <= t0) return 0;
    return (int)((s->t_ms - t0) * 256u / dur);
}

int xhw_splash_title_face(const char* utf8) {
    return xf_width(&xf_title, utf8, 0) <= SP_X1 - SP_X0 ? XHW_FACE_TITLE : XHW_FACE_TITLE2;
}

/* a title too wide for the smaller face goes on two lines: broken after a
 * colon when the first part fits, else at the last space that fits */
int xhw_splash_title_lines(const char* t, char* l1, char* l2, int cap) {
    int n = (int)strlen(t), i, cut = -1;
    if (xhw_splash_title_face(t) == XHW_FACE_TITLE || xf_width(&xf_title2, t, 0) <= SP_X1 - SP_X0) {
        snprintf(l1, (size_t)cap, "%s", t);
        l2[0] = 0;
        return 1;
    }
    for (i = n - 1; i > 0; i--) {
        if (t[i] != ' ') continue;
        snprintf(l1, (size_t)cap, "%.*s", i, t);
        if (xf_width(&xf_title2, l1, 0) > SP_X1 - SP_X0) continue;
        if (cut < 0) cut = i;
        if (t[i - 1] == ':') {
            cut = i;
            break;
        }
    }
    if (cut < 0) cut = n;
    snprintf(l1, (size_t)cap, "%.*s", cut, t);
    snprintf(l2, (size_t)cap, "%s", t[cut] ? t + cut + 1 : "");
    return 2;
}

/* the X: the platform's mark beside its name */
static void mark(uint32_t* fb, int pitch, int x, int cy) {
    aa_line(fb, pitch, (float)x + 2, (float)cy - 5, (float)x + 12, (float)cy + 5, 2.4f, rgb(127, 216, 200), 255);
    aa_line(fb, pitch, (float)x + 12, (float)cy - 5, (float)x + 2, (float)cy + 5, 2.4f, rgb(127, 216, 200), 255);
}

static void top_row(uint32_t* fb, int pitch, const xhw_splash_state* s) {
    const char* name = s->version[0] ? s->version : "N64Recomp-X";
    int nw = xf_width(&xf_label, name, 0);
    xf_draw(fb, pitch, &xf_label, "TECHPROGABE PRESENTS", SP_X0, SP_TOP_Y, rgb(159, 179, 207), 3, 256);
    xf_draw(fb, pitch, &xf_label, name, SP_X1 - nw, SP_TOP_Y, rgb(159, 179, 207), 0, 256);
    mark(fb, pitch, SP_X1 - nw - 22, SP_TOP_Y - 4);
}

/* a chip: text on a rounded tint; returns its right edge */
static int chip(uint32_t* fb, int pitch, int x, int dy, const char* t, int ok, int alpha) {
    int w = xf_width(&xf_label, t, 0) + 20 + (ok ? 16 : 0), cy = SP_CHIP_Y + dy;
    rrect(fb, pitch, x, cy, w, SP_CHIP_H, 6, ok ? rgb(127, 216, 200) : rgb(150, 175, 220), (ok ? 40 : 26) * alpha >> 8);
    if (ok) {   /* the tick */
        aa_line(fb, pitch, (float)x + 11, (float)cy + 12.5f, (float)x + 14, (float)cy + 15.5f, 2.0f,
                rgb(159, 240, 224), 255 * alpha >> 8);
        aa_line(fb, pitch, (float)x + 14, (float)cy + 15.5f, (float)x + 20, (float)cy + 8.5f, 2.0f,
                rgb(159, 240, 224), 255 * alpha >> 8);
    }
    xf_draw(fb, pitch, &xf_label, t, x + 10 + (ok ? 16 : 0), cy + 16, ok ? rgb(159, 240, 224) : rgb(201, 214, 234),
            0, alpha);
    return x + w;
}

static void progress(uint32_t* fb, int pitch, const xhw_splash_state* s) {
    float f = s->progress < 0 ? 0 : s->progress > 1 ? 1 : s->progress;
    int step = s->step < 1 ? 1 : s->step > XHW_SPLASH_STEPS ? XHW_SPLASH_STEPS : s->step;
    float all = s->shown > 0 ? s->shown : s->step ? ((float)(step - 1) + f) / (float)XHW_SPLASH_STEPS : f;
    int bw = SP_X1 - SP_X0, fw = (int)(all * (float)bw), x, y;
    char right[48];
    if (s->stage[0]) xf_draw(fb, pitch, &xf_stage, s->stage, SP_X0, SP_STAGE_Y, rgb(238, 243, 251), 0, appear(s, s->t_stage, 180));
    if (s->step) {
        if (s->stage_ms[0]) snprintf(right, sizeof right, "%s  \xC2\xB7  step %d of %d", s->stage_ms, step, XHW_SPLASH_STEPS);
        else snprintf(right, sizeof right, "step %d of %d", step, XHW_SPLASH_STEPS);
    } else {
        snprintf(right, sizeof right, "%s", s->stage_ms);
    }
    if (right[0])
        xf_draw(fb, pitch, &xf_label, right, SP_X1 - xf_width(&xf_label, right, 0), SP_STAGE_Y, rgb(159, 179, 207), 0, 256);
    rrect(fb, pitch, SP_X0, SP_BAR_Y, bw, SP_BAR_H, 3, rgb(255, 255, 255), 26);
    if (fw > 0) {
        /* the fill, #3ec9a7 -> #7fd8c8, with a soft glow round it */
        for (y = -4; y < SP_BAR_H + 4; y++) {
            uint32_t* row = fb + (size_t)(SP_BAR_Y + y) * (size_t)pitch + SP_X0;
            int dy = y < 0 ? -y : y >= SP_BAR_H ? y - SP_BAR_H + 1 : 0;
            for (x = 0; x < fw && x < bw; x++) {
                int k = x * 256 / (bw > 0 ? bw : 1);
                int r = 62 + (127 - 62) * k / 256, g = 201 + (216 - 201) * k / 256, b = 167 + (200 - 167) * k / 256;
                if (dy) {
                    int a = (5 - dy) * 14;
                    if (a > 0) add(&row[x], r * a >> 8, g * a >> 8, b * a >> 8);
                } else {
                    int a = 255;
                    if (x == 0 || x == fw - 1) a = 170;
                    if ((y == 0 || y == SP_BAR_H - 1) && (x < 2 || x >= fw - 2)) a = 90;
                    blend(&row[x], rgb(r, g, b), a);
                }
            }
        }
        /* the head: a white point with a glow, pulsing a little */
        {
            int pulse = s->still ? 0 : sinq((int)(s->t_ms / 2u)) * 10 >> 14;
            float hx = (float)(SP_X0 + fw), hy = (float)SP_BAR_Y + SP_BAR_H * 0.5f;
            disc(fb, pitch, hx, hy, 9.0f, rgb(160, 240, 225), 40 + pulse);
            disc(fb, pitch, hx, hy, 6.5f, rgb(160, 240, 225), 80 + pulse * 2);
            disc(fb, pitch, hx, hy, 5.0f, rgb(255, 255, 255), 255);
        }
    }
}

/* a keycap: light, the label dark; returns its right edge */
static int key(uint32_t* fb, int pitch, int x, const char* t, int lit) {
    int w = xf_width(&xf_label, t, 0) + 14;
    rrect(fb, pitch, x, SP_FOOT_Y - 14, w, 19, 5, lit ? rgb(255, 230, 150) : rgb(238, 243, 251), 255);
    xf_draw(fb, pitch, &xf_label, t, x + 7, SP_FOOT_Y, rgb(11, 23, 48), 0, 256);
    return x + w;
}

/* "Hold " key " for 480i": its width, or drawn when fb */
static int hint(uint32_t* fb, int pitch, int x, const char* const* keys, int nkeys, const char* tail, uint32_t col, int lit) {
    int i, x0 = x;
    uint32_t* f = fb;
    if (!f) {   /* measuring */
        x += xf_width(&xf_body, "Hold ", 0);
        for (i = 0; i < nkeys; i++) x += xf_width(&xf_label, keys[i], 0) + 14 + (i + 1 < nkeys ? 4 : 0);
        return x - x0 + xf_width(&xf_body, tail, 0) + 6;
    }
    x = xf_draw(f, pitch, &xf_body, "Hold ", x, SP_FOOT_Y, col, 0, 256);
    for (i = 0; i < nkeys; i++) x = key(f, pitch, x, keys[i], lit) + (i + 1 < nkeys ? 4 : 0);
    return xf_draw(f, pitch, &xf_body, tail, x + 6, SP_FOOT_Y, col, 0, 256) - x0;
}

/* "Video: 720p 16:9 (why)." -> "720p 16:9" */
static void video_mode(const char* line, char* out, size_t cap) {
    const char* p = line;
    size_t n = 0;
    if (!strncmp(p, "Video: ", 7)) p += 7;
    while (p[n] && p[n] != '(' && p[n] != '.' && n + 1 < cap) n++;
    while (n && p[n - 1] == ' ') n--;
    memcpy(out, p, n);
    out[n] = 0;
}

static void footer(uint32_t* fb, int pitch, const xhw_splash_state* s) {
    static const char* const k_back[1] = { "BACK" }, *const k_lr[2] = { "L", "R" };
    int h = s->holding > s->hold ? s->holding : s->hold;
    uint32_t dim = rgb(201, 214, 234);
    if (h != XHW_HOLD_NONE) {   /* what the triggers do, alone on the line, lit */
        const char* t = xhw_splash_hold_text(s->hold, s->holding);
        int w = (xf_width(&xf_label, "L", 0) + 14) * 2 + 4 + 10 + xf_width(&xf_body, t, 0), x = (W - w) / 2;
        x = key(fb, pitch, x, "L", 1) + 4;
        x = key(fb, pitch, x, "R", 1) + 10;
        xf_draw(fb, pitch, &xf_body, t, x, SP_FOOT_Y, s->hold ? rgb(255, 236, 150) : rgb(230, 220, 170), 0, 256);
        return;
    }
    if (s->settings) {   /* BACK: the BACK menu comes before the game */
        const char* t = xhw_splash_settings_text(s->settings);
        int w = xf_width(&xf_label, "BACK", 0) + 14 + 10 + xf_width(&xf_body, t, 0), x = (W - w) / 2;
        x = key(fb, pitch, x, "BACK", 1) + 10;
        xf_draw(fb, pitch, &xf_body, t, x, SP_FOOT_Y, s->settings == 2 ? rgb(255, 236, 150) : rgb(230, 220, 170), 0, 256);
        return;
    }
    {
        char mode[32];
        int x = SP_X0, rw;
        video_mode(s->video, mode, sizeof mode);
        if (mode[0]) {
            x = xf_draw(fb, pitch, &xf_body, "Video: ", x, SP_FOOT_Y, dim, 0, 256);
            xf_draw(fb, pitch, &xf_stage, mode, x, SP_FOOT_Y, rgb(238, 243, 251), 0, 256);
        }
        rw = hint(NULL, 0, 0, k_back, 1, "for settings", dim, 0);
        hint(fb, pitch, SP_X1 - rw, k_back, 1, "for settings", dim, 0);
        x = SP_X1 - rw - 22 - hint(NULL, 0, 0, k_lr, 2, "for 480i", dim, 0);
        hint(fb, pitch, x, k_lr, 2, "for 480i", dim, 0);
    }
}

/* the BACK menu's card over the screen, centred (before the game): the
 * frame under it dimmed to 45 % as under a game; while it fades in the
 * frame dims and the card rises 10 px */
void xhw_splash_panel_over(uint32_t* fb, int pitch, const xhw_splash_state* s, int fade) {
    int x0 = (W - XHW_PANEL_W) / 2, y0 = (H - XHW_PANEL_H) / 2 + (256 - fade) * 10 / 256, x, y;
    int dim = 256 - (141 * fade >> 8);
    if (!s->panel) return;
    for (y = 0; y < H; y++) {
        uint32_t* row = fb + (size_t)y * (size_t)pitch;
        for (x = 0; x < W; x++) {
            uint32_t c = row[x];
            row[x] = 0xFF000000u | ((((c & 0xFF00FFu) * (uint32_t)dim) >> 8) & 0xFF00FFu) |
                     ((((c & 0x00FF00u) * (uint32_t)dim) >> 8) & 0x00FF00u);
        }
    }
    for (y = 0; y < XHW_PANEL_H; y++) {
        const uint32_t* src = s->panel + (size_t)y * (size_t)s->panel_pitch;
        uint32_t* d = fb + (size_t)(y0 + y) * (size_t)pitch + x0;
        if (y0 + y >= H) break;
        for (x = 0; x < XHW_PANEL_W; x++) {
            int a = (int)(src[x] >> 24) * fade >> 8;
            if (a) blend(&d[x], src[x], a);
        }
    }
}


/* The sky that does not move (the nebula, the constellation, the planet)
 * is drawn once into a cache: the P3 draws it in about a tenth of a second,
 * far too slow a frame (on red the loading screen starved the boot thread:
 * the controllers took 10 s and the SHA-1 never finished). A frame copies
 * the cache, then the stars, a shooting star and the text: a few ms. */
static uint32_t* s_sky;

void xhw_splash_free_cache(void) {
    free(s_sky);
    s_sky = NULL;
}

static void space_render(uint32_t* fb, int pitch, const xhw_splash_state* s) {
    int layers = s->layers ? s->layers : XHW_SPLASH_ALL, y;
    sin_init();
    if (layers & XHW_SPLASH_BG) {
        if (!s_sky && (s_sky = (uint32_t*)malloc((size_t)W * H * 4)) != NULL) {
            space_background(s_sky, W, 0);
            constellation(s_sky, W);
            planet(s_sky, W);
        }
        if (s_sky) {
            for (y = 0; y < H; y++) memcpy(fb + (size_t)y * (size_t)pitch, s_sky + (size_t)y * W, W * 4);
        } else {   /* no room: drawn every frame */
            space_background(fb, pitch, 0);
            constellation(fb, pitch);
            planet(fb, pitch);
        }
        stars(fb, pitch, s->still ? 0 : s->t_ms);
        shooting_star(fb, pitch, s->t_ms, s->still);
    } else {   /* the plain base (the host test's diff) */
        for (y = 0; y < H; y++) fill(fb, pitch, 0, y, W, 1, rgb(4, 8, 20));
    }
    if (!(layers & XHW_SPLASH_FG)) return;
    top_row(fb, pitch, s);
    if (s->card) {
        int i, face = xf_width(&xf_title2, s->card_title, 0) <= SP_X1 - SP_X0 ? 3 : 2;
        int lw = 0;
        xf_draw(fb, pitch, &xf_label, "CAN'T START", SP_X0, SP_EYEBROW_Y, rgb(255, 184, 107), 2, 256);
        xf_draw(fb, pitch, k_faces[face], s->card_title, SP_X0, SP_TITLE_Y - 8, rgb(238, 243, 251), 0, 256);
        for (i = 0; i < XHW_CARD_LINES; i++) {
            int w = xf_width(&xf_body, s->card_lines[i], 0);
            if (w > lw) lw = w;
        }
        for (i = 0; i < XHW_CARD_LINES; i++)
            if (s->card_lines[i][0])
                xf_draw(fb, pitch, lw <= SP_X1 - SP_X0 ? &xf_body : &xf_label, s->card_lines[i], SP_X0,
                        SP_LINES_Y + i * SP_LINE_H, rgb(200, 210, 228), 0, 256);
        if (s->card_footer)
            xf_draw(fb, pitch, &xf_body, s->card_footer, (W - xf_width(&xf_body, s->card_footer, 0)) / 2, SP_FOOT_Y,
                    rgb(159, 179, 207), 0, 256);
        return;
    }
    if (s->card_title[0]) {
        const xf_font* f = k_faces[xhw_splash_title_face(s->card_title)];
        char l1[64], l2[64];
        int lines = xhw_splash_title_lines(s->card_title, l1, l2, sizeof l1);
        int ta = appear(s, s->t_title, 350), ra = appear(s, s->t_rom, 300), oa = appear(s, s->t_ok, 300);
        int rise = (256 - ta) * (256 - ta) * 10 >> 16;   /* the title settles 10 px, eased */
        int ty = SP_TITLE_Y - (f == &xf_title ? 0 : 8) - (lines == 2 ? 26 : 0) + rise, x = SP_X0, cy = lines == 2 ? 30 : 0;
        xf_draw(fb, pitch, &xf_label, "NOW STARTING", SP_X0, SP_EYEBROW_Y, rgb(127, 216, 200), 2, ta);
        glow(fb, pitch, f, l1, SP_X0, ty, ta);
        xf_draw(fb, pitch, f, l1, SP_X0, ty, rgb(238, 243, 251), 0, ta);
        if (lines == 2) {
            glow(fb, pitch, f, l2, SP_X0, ty + 38, ta);
            xf_draw(fb, pitch, f, l2, SP_X0, ty + 38, rgb(238, 243, 251), 0, ta);
        }
        if (s->rom_id[0]) x = chip(fb, pitch, x, cy, s->rom_id, 0, ra) + 8;
        if (s->rom_where[0]) x = chip(fb, pitch, x, cy, s->rom_where, 0, ra) + 8;
        if (s->rom_ok) chip(fb, pitch, x, cy, "SHA-1 verified", 1, oa);
        else if (!s->rom_id[0] && s->card_sub[0])
            xf_draw(fb, pitch, &xf_body, s->card_sub, SP_X0, SP_CHIP_Y + cy + 16, rgb(159, 179, 207), 0, 256);
    }
    progress(fb, pitch, s);
    if (s->video_note[0])
        xf_draw(fb, pitch, &xf_body, s->video_note, (W - xf_width(&xf_body, s->video_note, 0)) / 2, SP_NOTE_Y,
                rgb(220, 200, 120), 0, 256);
    footer(fb, pitch, s);
    if (!s->still && s->t_ms < 400u) {   /* up from black, out of the dashboard */
        int k = (int)(s->t_ms * 256u / 400u), x, y;
        k = k * k >> 8;
        for (y = 0; y < H; y++) {
            uint32_t* row = fb + (size_t)y * (size_t)pitch;
            for (x = 0; x < W; x++) {
                uint32_t c = row[x];
                row[x] = 0xFF000000u | ((((c & 0xFF00FFu) * (uint32_t)k) >> 8) & 0xFF00FFu) |
                         ((((c & 0x00FF00u) * (uint32_t)k) >> 8) & 0x00FF00u);
            }
        }
    }
}

void xhw_splash_render(uint32_t* fb, int pitch, const xhw_splash_state* s) {
    if (XHW_SPLASH_CLASSIC) classic_render(fb, pitch, s);
    else space_render(fb, pitch, s);
}

/* ---- the BACK menu's card (xhw_panel_render, PLAN.md 5.2) ----------------
 * The loading screen in miniature: its aurora (sampled where the card sits
 * on a 640x480 screen, so it is the same sky), its sparks, the title in
 * the wordmark's white with the light sweep coming round every few
 * seconds, the bar's green as a line under it with the bar's moving
 * highlight, then the rows. The selected row sits on a teal glow that
 * slides to it, with the bar's green as its marker and a glint crossing
 * it. Round the rounded body: a thin teal edge and a soft glow, both in
 * the alpha channel, so the frozen game shows through around the card. */
#define PW XHW_PANEL_W
#define PH XHW_PANEL_H
#define PG 10                   /* the glow ring outside the body */
#define PR 12                   /* corner radius */
#define PX0 ((W - PW) / 2)      /* the card's place on a 640x480 screen */
#define PY0 ((H - PH) / 2)
#define P_TITLE_Y (PG + 16)
#define P_LINE_Y (P_TITLE_Y + 40)
#define P_SUB_Y (P_LINE_Y + 10)
#define P_ROWS_Y (P_SUB_Y + 28)
#define P_ROW_H 22
#define P_ROW_H_TIGHT 20        /* more than P_ROWS_ROOMY rows: closer together */
#define P_ROWS_ROOMY 10
#define P_ACTION_GAP 8
#define P_X0 (PG + 24)          /* text margins */
#define P_X1 (PW - PG - 24)

static uint8_t s_pmask[PH][PW];   /* body coverage 0..255 */
static uint8_t s_pglow[PH][PW];   /* the glow outside it, 0..255 */
static uint8_t s_pedge[PH][PW];   /* the edge line just inside it */
static int s_pmask_ready;

static void panel_mask(void) {
    int x, y;
    for (y = 0; y < PH; y++)
        for (x = 0; x < PW; x++) {
            /* distance to the rounded body in pixels, negative inside */
            float hx = (PW - 2 * PG) * 0.5f - PR, hy = (PH - 2 * PG) * 0.5f - PR;
            float qx = (x + 0.5f - PW * 0.5f), qy = (y + 0.5f - PH * 0.5f), d;
            qx = (qx < 0 ? -qx : qx) - hx;
            qy = (qy < 0 ? -qy : qy) - hy;
            if (qx > 0 && qy > 0) d = __builtin_sqrtf(qx * qx + qy * qy) - PR;
            else d = (qx > qy ? qx : qy) - PR;
            s_pmask[y][x] = (uint8_t)(d <= -0.5f ? 255 : d >= 0.5f ? 0 : (int)((0.5f - d) * 255.0f));
            if (d > 0 && d < PG) {
                float g = 1.0f - d / PG;
                s_pglow[y][x] = (uint8_t)(g * g * 255.0f);
            } else {
                s_pglow[y][x] = 0;
            }
            s_pedge[y][x] = (uint8_t)(d > -2.0f && d <= 0.5f ? 255 : d > -3.0f && d <= -2.0f ? 110 : 0);
        }
    s_pmask_ready = 1;
}

static void mix(uint32_t* p, int r, int g, int b, int a) {
    uint32_t c = *p;
    if (a > 255) a = 255;
    *p = (c & 0xFF000000u) | ((uint32_t)((R_(c) * (255 - a) + r * a) / 255) << 16) |
         ((uint32_t)((G_(c) * (255 - a) + g * a) / 255) << 8) | (uint32_t)((B_(c) * (255 - a) + b * a) / 255);
}

/* the aurora under the card: the same 160x120 sky as the loading screen, its
 * upper three quarters squeezed onto the card (the bands fade out below) */
static void panel_sky(uint32_t* img, int pitch, uint32_t t_ms) {
    static int16_t row0[W][3], row1[W][3];
    int y, x, ly_prev = -1, br, bgc, bb;
    aurora(t_ms);
    for (y = 0; y < PH; y++) {
        int sy = 8 + y * 3 / 4, ly = sy >> 2, k = sy & 3;   /* the upper sky, where the bands are */
        uint32_t* d = img + (size_t)y * (size_t)pitch;
        if (ly != ly_prev) {
            widen(ly, row0);
            widen(ly + 1, row1);
            ly_prev = ly;
        }
        bg_at(sy, &br, &bgc, &bb);
        for (x = 0; x < PW; x++) {
            int sx = PX0 + x;
            int r = (row0[sx][0] * (4 - k) + row1[sx][0] * k) >> 2, g = (row0[sx][1] * (4 - k) + row1[sx][1] * k) >> 2,
                b = (row0[sx][2] * (4 - k) + row1[sx][2] * k) >> 2;
            /* the bands twice as strong as on the loading screen: the card is
             * small and the frozen game around it is dimmed */
            r = br + (r - br) * 2, g = bgc + (g - bgc) * 2, b = bb + (b - bb) * 2;
            d[x] = rgb(r, g, b);
        }
    }
}

/* fewer sparks than the loading screen, rising through the card */
static void panel_sparks(uint32_t* img, int pitch, uint32_t t_ms) {
    uint32_t seed = 0x2545F491u;
    int i;
    for (i = 0; i < 20; i++) {
        uint32_t span = PH + 20, speed, x0, ph, y, life;
        int x, a, dx, dy;
        seed = seed * 1664525u + 1013904223u;
        x0 = PG + (seed >> 8) % (PW - 2 * PG);
        seed = seed * 1664525u + 1013904223u;
        speed = 8 + ((seed >> 8) % 14);
        seed = seed * 1664525u + 1013904223u;
        ph = (seed >> 8) % span;
        y = (t_ms / 1000u * speed + ((t_ms % 1000u) * speed) / 1000u + ph) % span;
        life = y;
        x = (int)x0 + (sinq((int)(t_ms / 8u) + i * 131) * 5 >> 14);
        y = (uint32_t)(PH + 10) - y;
        a = (int)(life < span / 2 ? life : span - life) * 2 * 40 / (int)span;
        for (dy = -1; dy <= 1; dy++)
            for (dx = -1; dx <= 1; dx++) {
                int k = (dx == 0 && dy == 0) ? 4 : (dx == 0 || dy == 0) ? 2 : 1;
                if (x + dx < 0 || x + dx >= PW || (int)y + dy < 0 || (int)y + dy >= PH) continue;
                if (s_pmask[(int)y + dy][x + dx] < 255) continue;
                add_px(img, pitch, x + dx, (int)y + dy, a * k * 2 / 8, a * k * 5 / 8, a * k * 6 / 8);
            }
    }
}

static void shadowed(uint32_t* img, int pitch, const char* s, int x, int y, int zoom, uint32_t col) {
    glyphs(img, pitch, s, x + 1, y + 1, zoom, rgb(2, 4, 12), 0, 0);
    glyphs(img, pitch, s, x, y, zoom, col, 0, 0);
}

static int row_h(const xhw_panel* p) { return p->rows > P_ROWS_ROOMY ? P_ROW_H_TIGHT : P_ROW_H; }

/* a row's top: the actions sit a little apart from the settings */
static int row_y(const xhw_panel* p, int r) {
    int y = P_ROWS_Y + r * row_h(p), i;
    for (i = 0; i < r && i + 1 < p->rows; i++)
        if (p->kind[i] != XHW_PANEL_ACTION && p->kind[i + 1] == XHW_PANEL_ACTION) y += P_ACTION_GAP;
    return y;
}

void xhw_panel_render(uint32_t* img, int pitch, const xhw_panel* p) {
    static int s_hl_y = -1;   /* the highlight's place: it slides to the selected row */
    uint32_t t = p->t_ms;
    int x, y, r, tz = 2, tw, rows = p->rows > XHW_PANEL_ROWS ? XHW_PANEL_ROWS : p->rows;
    sin_init();
    if (!s_pmask_ready) panel_mask();
    s_clip_w = PW;
    s_clip_h = PH;
    panel_sky(img, pitch, t + 4000u);   /* a few seconds into the sky: the bands are up */

    /* the title: white, the wordmark's sweep every 3.5 s */
    if (xhw_splash_text_width(p->title, 2) > P_X1 - P_X0) tz = 1;
    tw = xhw_splash_text_width(p->title, tz);
    {
        uint32_t ph = (t + 3300u) % 3500u;
        int sweep_w = 0, sweep_x = 0, tx = (PW - tw) / 2, ty = P_TITLE_Y + (tz == 1 ? 8 : 0);
        if (ph < 700u) {
            sweep_w = 36;
            sweep_x = tx - sweep_w + (int)(ph * (uint32_t)(tw + 2 * sweep_w) / 700u);
        }
        glyphs(img, pitch, p->title, tx + 2, ty + 2, tz, rgb(2, 4, 12), 0, 0);
        glyphs(img, pitch, p->title, tx, ty, tz, rgb(236, 236, 244), sweep_x, sweep_w);
    }
    /* the bar's green as a line, growing out from the middle as the menu opens */
    {
        int len = P_X1 - P_X0, grow = t >= 300u ? len : (int)((uint32_t)len * t / 300u);
        int x0 = P_X0 + (len - grow) / 2, hl = (int)((t / 4u) % (uint32_t)(len + 80)) - 40;
        for (y = 0; y < 3; y++) {
            uint32_t* row = img + (size_t)(P_LINE_Y + y) * (size_t)pitch;
            int dr = (1 - y) * 14, dg = (1 - y) * 10, db = (1 - y) * 12;
            for (x = x0; x < x0 + grow; x++) {
                int d = x - P_X0 - hl, k = 0;
                if (d < 0) d = -d;
                if (d < 40) k = (40 - d) * 2;
                row[x] = rgb(120 + dr + k / 2, 200 + dg + k / 3, 110 + db + k / 2);
            }
        }
    }
    if (p->sub_left[0]) shadowed(img, pitch, p->sub_left, P_X0, P_SUB_Y, 1, rgb(150, 200, 140));
    if (p->sub_right[0])
        shadowed(img, pitch, p->sub_right, P_X1 - xhw_splash_text_width(p->sub_right, 1), P_SUB_Y, 1, rgb(120, 132, 158));

    if (p->page == XHW_PANEL_CONTROLS_PAGE) {   /* the controller instead of the rows (xhw_panel_pad.c) */
        xhw_panel_pad_render(img, pitch, p);
        if (p->info[0]) shadowed(img, pitch, p->info, (PW - xhw_splash_text_width(p->info, 1)) / 2, PH - PG - 82, 1,
                                 rgb(150, 165, 190));
        if (p->note[0]) shadowed(img, pitch, p->note, (PW - xhw_splash_text_width(p->note, 1)) / 2, PH - PG - 62, 1,
                                 rgb(255, 214, 120));
        if (p->footer[0])
            shadowed(img, pitch, p->footer, (PW - xhw_splash_text_width(p->footer, 1)) / 2, PH - PG - 26, 1,
                     rgb(110, 120, 140));
        rows = 0;
    }
    /* the highlight: a teal glow fading to the right, the green marker, a glint */
    if (p->sel >= 0 && p->sel < rows) {
        int target = row_y(p, p->sel) - 2, hx0 = PG + 12, hx1 = PW - PG - 12, gl;
        if (s_hl_y < 0 || t < 40u) s_hl_y = target;
        if (s_hl_y != target) s_hl_y += (target - s_hl_y + (target > s_hl_y ? 1 : -1)) / 2;
        gl = (int)((t / 3u) % (uint32_t)(hx1 - hx0 + 400)) - 60;
        for (y = s_hl_y; y < s_hl_y + 20; y++) {
            uint32_t* row = img + (size_t)y * (size_t)pitch;
            int edge = (y == s_hl_y || y == s_hl_y + 19) ? 2 : 1;
            for (x = hx0; x < hx1; x++) {
                int a = 120 - (x - hx0) * 90 / (hx1 - hx0), d = x + (y - s_hl_y) / 2 - hx0 - gl;
                if (d < 0) d = -d;
                if (d < 30) a += (30 - d) * 3;
                mix(&row[x], 40, 140, 160, a / edge);
            }
            for (x = hx0; x < hx0 + 3; x++) row[x] = y < s_hl_y + 2 ? rgb(170, 235, 160) : rgb(120, 200, 110);
        }
    }
    /* the rows; a thin rule before the actions */
    for (r = 0; r < rows; r++) {
        int ry = row_y(p, r), sel = r == p->sel, act = p->kind[r] == XHW_PANEL_ACTION;
        uint32_t lc = sel ? rgb(255, 236, 150) : act ? rgb(176, 192, 224) : rgb(220, 226, 240);
        if (r > 0 && act && p->kind[r - 1] != XHW_PANEL_ACTION) {
            uint32_t* row = img + (size_t)(ry - P_ACTION_GAP / 2 - 2) * (size_t)pitch;
            for (x = P_X0; x < P_X1; x++) mix(&row[x], 90, 110, 150, 120);
        }
        shadowed(img, pitch, p->label[r], P_X0 + (sel ? 6 : 0), ry, 1, lc);
        if (p->value[r][0]) {
            int arrows = sel && p->kind[r] == XHW_PANEL_SETTING;
            int vw = xhw_splash_text_width(p->value[r], 1), vx = P_X1 - vw - (arrows ? 16 : 0) - (p->star[r] ? 16 : 0);
            shadowed(img, pitch, p->value[r], vx, ry, 1, sel ? rgb(250, 250, 255) : rgb(150, 165, 190));
            if (arrows) {
                int pulse = (sinq((int)(t / 2u)) * 3) >> 14;
                shadowed(img, pitch, "<", vx - 16 - pulse, ry, 1, rgb(150, 220, 140));
                shadowed(img, pitch, ">", P_X1 - 8 - (p->star[r] ? 16 : 0) + pulse, ry, 1, rgb(150, 220, 140));
            }
            if (p->star[r]) shadowed(img, pitch, "*", P_X1 - 8, ry, 1, rgb(255, 200, 110));
        }
    }
    if (rows) {
        int iy = row_y(p, rows - 1) + row_h(p) + 10;
        if (p->info[0]) shadowed(img, pitch, p->info, P_X0, iy, 1, rgb(150, 165, 190));
        if (p->note[0]) shadowed(img, pitch, p->note, P_X0, iy + 20, 1, rgb(255, 214, 120));
        if (p->footer[0])
            shadowed(img, pitch, p->footer, (PW - xhw_splash_text_width(p->footer, 1)) / 2, PH - PG - 26, 1,
                     rgb(110, 120, 140));
    }
    panel_sparks(img, pitch, t);

    /* the shape: the teal edge, the body's coverage and the glow in alpha */
    {
        int pulse = 200 + ((sinq((int)(t / 3u)) * 55) >> 14);
        for (y = 0; y < PH; y++) {
            uint32_t* row = img + (size_t)y * (size_t)pitch;
            for (x = 0; x < PW; x++) {
                int m = s_pmask[y][x];
                if (s_pedge[y][x] && m) mix(&row[x], 90, 190, 205, s_pedge[y][x] * 3 / 4);
                if (m == 255) continue;
                if (m) {
                    row[x] = (row[x] & 0xFFFFFFu) | ((uint32_t)m << 24);
                } else {
                    int g = s_pglow[y][x] * pulse / 255 * 150 / 255;
                    row[x] = ((uint32_t)g << 24) | 0x3CA8BEu;   /* teal glow */
                }
            }
        }
    }
    s_clip_w = W;
    s_clip_h = H;
}
