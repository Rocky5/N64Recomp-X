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
    if (x < 0 || x >= W || y < 0 || y >= H) return;
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
                    if (py < 0 || py >= H) continue;
                    for (zx = 0; zx < zoom; zx++) {
                        int px = x + gx * zoom + zx;
                        uint32_t c = col;
                        if (px < 0 || px >= W) continue;
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
        if (j < 0 || j >= H) continue;
        for (i = x; i < x + w; i++)
            if (i >= 0 && i < W) fb[(size_t)j * (size_t)pitch + i] = col;
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
    return "Hold BACK for 480i (safe video)";
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

void xhw_splash_render(uint32_t* fb, int pitch, const xhw_splash_state* s) {
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
