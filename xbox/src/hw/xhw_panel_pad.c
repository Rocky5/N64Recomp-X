/* xhw_panel_pad.c - the BACK menu's Controls page (PLAN.md 5.2): an Xbox
 * controller drawn on the card, every input with a callout that names the
 * N64 control it gives, the selected one glowing, and what is held on the
 * real pad lit up live (the sticks lean with it). settings_menu.c fills the
 * page's part of xhw_panel; xhw_splash_draw.c draws the card around it and
 * calls this instead of the rows.
 *
 * The body is a smooth union of a rounded box and two grips (signed
 * distances), shaded once into a cached layer (a bevel from the distance
 * field's slope, a light from the top left). The wells, buttons, sticks and
 * callouts go into a second layer with the body, drawn again only when what
 * it shows changes (a press, a tilt, a mapping); each frame composites it
 * over the card's sky and adds what moves (the selection's pulse). Drawing
 * everything every frame ran the page at 14 fps on red (2026-10-18).
 * Floating point only in the one-time body
 * layer, as the splash's sine table; the host test draws what the console
 * draws (tests/xbox/test_splash.c, docs/look/controls-page-host.png). */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "xhw_splash.h"

#define PW XHW_PANEL_W
#define PH XHW_PANEL_H
#define GW 8
#define GH 16

/* the controller's centre on the card, and its layer */
#define CX 256
#define CY 210
#define LW 280              /* the body layer: LW x LH around the centre */
#define LH 200
#define LX0 (CX - LW / 2)
#define LY0 (CY - 80)

/* the callouts: two columns */
#define BOX_W 106
#define BOX_H 18
#define COL_L 18
#define COL_R (PW - 18 - BOX_W)

static uint32_t rgb(int r, int g, int b) {
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}
#define R_(c) ((int)(((c) >> 16) & 0xFF))
#define G_(c) ((int)(((c) >> 8) & 0xFF))
#define B_(c) ((int)((c) & 0xFF))

/* x / 255 rounded, for x up to 255 * 255, without a divide (the P3 pays
 * ~40 cycles for one, three a pixel: the page ran at 14 fps on red) */
#define DIV255(x) ((((x) + 128) + (((x) + 128) >> 8)) >> 8)

/* where the shapes may draw (rows s_ylo..s_yhi) and the row img starts at:
 * the whole card, or the static layer's band of it */
static int s_ylo = 0, s_yhi = PH, s_org = 0;

/* col over *p at alpha a. *p is premultiplied: on the card it is opaque (the
 * same as straight), in the layer it starts clear and gathers alpha */
static void mix(uint32_t* p, uint32_t col, int a) {
    uint32_t c = *p;
    if (a <= 0) return;
    if (a >= 255) {
        *p = 0xFF000000u | (col & 0xFFFFFFu);
        return;
    }
    *p = ((uint32_t)(a + DIV255((int)(c >> 24) * (255 - a))) << 24) |
         ((uint32_t)DIV255(R_(c) * (255 - a) + R_(col) * a) << 16) |
         ((uint32_t)DIV255(G_(c) * (255 - a) + G_(col) * a) << 8) | (uint32_t)DIV255(B_(c) * (255 - a) + B_(col) * a);
}

static uint32_t shade(uint32_t c, int k) {   /* k 256 = as is */
    return rgb(R_(c) * k >> 8, G_(c) * k >> 8, B_(c) * k >> 8);
}

/* ---- integer helpers ---- */
static int isqrt(int v) {   /* floor(sqrt(v)) */
    int r = 0, b = 1 << 30;
    if (v <= 0) return 0;
    while (b > v) b >>= 2;
    while (b) {
        if (v >= r + b) v -= r + b, r = (r >> 1) + b;
        else r >>= 1;
        b >>= 2;
    }
    return r;
}

/* a triangle wave 0..256..0 over `period` ms: the glows' pulse, no trig */
static int pulse(uint32_t t, uint32_t period) {
    uint32_t ph = t % period;
    return (int)(ph < period / 2 ? ph * 512u / period : (period - ph) * 512u / period);
}

/* ---- text: unscii-16, shadowed ---- */
static void glyphs(uint32_t* img, int pitch, const char* s, int x, int y, uint32_t col) {
    for (; *s; s++, x += GW) {
        const unsigned char* g = xhw_font16 + (unsigned char)*s * GH;
        int gy, gx;
        for (gy = 0; gy < GH; gy++)
            for (gx = 0; gx < GW; gx++) {
                int px = x + gx, py = y + gy;
                if (!(g[gy] & (0x80 >> gx)) || px < 0 || px >= PW || py < s_ylo || py >= s_yhi) continue;
                img[(size_t)(py - s_org) * (size_t)pitch + px] = col;
            }
    }
}
static void text(uint32_t* img, int pitch, const char* s, int x, int y, uint32_t col) {
    glyphs(img, pitch, s, x + 1, y + 1, rgb(2, 4, 12));
    glyphs(img, pitch, s, x, y, col);
}
static int tw(const char* s) { return (int)strlen(s) * GW; }

/* ---- antialiased shapes (coverage from the distance, in 1/16 px) ---- */
static void disc(uint32_t* img, int pitch, int cx16, int cy16, int r16, uint32_t col, int alpha) {
    int x0 = (cx16 - r16) / 16 - 1, x1 = (cx16 + r16) / 16 + 1, y0 = (cy16 - r16) / 16 - 1, y1 = (cy16 + r16) / 16 + 1;
    int x, y, in2 = r16 > 8 ? (r16 - 8) * (r16 - 8) : -1, out2 = (r16 + 8) * (r16 + 8);
    for (y = y0; y <= y1; y++) {
        if (y < s_ylo || y >= s_yhi) continue;
        for (x = x0; x <= x1; x++) {
            int dx = x * 16 + 8 - cx16, dy = y * 16 + 8 - cy16, d2 = dx * dx + dy * dy, d, cov;
            if (x < 0 || x >= PW || d2 >= out2) continue;
            if (d2 <= in2) {   /* inside: no square root */
                mix(&img[(size_t)(y - s_org) * (size_t)pitch + x], col, alpha);
                continue;
            }
            d = isqrt(d2) - r16;   /* 1/16 px, negative inside */
            cov = d <= -8 ? 256 : d >= 8 ? 0 : (8 - d) * 16;
            if (cov) mix(&img[(size_t)(y - s_org) * (size_t)pitch + x], col, alpha * cov >> 8);
        }
    }
}

/* a ring: r16 its middle, w16 its width */
static void ring(uint32_t* img, int pitch, int cx16, int cy16, int r16, int w16, uint32_t col, int alpha) {
    int ro = r16 + w16 / 2, x0 = (cx16 - ro) / 16 - 1, x1 = (cx16 + ro) / 16 + 1, y0 = (cy16 - ro) / 16 - 1,
        y1 = (cy16 + ro) / 16 + 1, x, y;
    int ri = r16 - w16 / 2 - 8, out2 = (ro + 8) * (ro + 8), in2 = ri > 0 ? ri * ri : -1;   /* no coverage past these */
    for (y = y0; y <= y1; y++) {
        if (y < s_ylo || y >= s_yhi) continue;
        for (x = x0; x <= x1; x++) {
            int dx = x * 16 + 8 - cx16, dy = y * 16 + 8 - cy16, d2 = dx * dx + dy * dy, d, cov;
            if (x < 0 || x >= PW || d2 >= out2 || d2 <= in2) continue;
            d = isqrt(d2) - r16;
            if (d < 0) d = -d;
            d -= w16 / 2;
            cov = d <= -8 ? 256 : d >= 8 ? 0 : (8 - d) * 16;
            if (cov) mix(&img[(size_t)(y - s_org) * (size_t)pitch + x], col, alpha * cov >> 8);
        }
    }
}

/* a line 1.5 px wide, its ends round */
static void line(uint32_t* img, int pitch, int x0, int y0, int x1, int y1, uint32_t col, int alpha) {
    int bx0 = (x0 < x1 ? x0 : x1) - 2, bx1 = (x0 > x1 ? x0 : x1) + 2;
    int by0 = (y0 < y1 ? y0 : y1) - 2, by1 = (y0 > y1 ? y0 : y1) + 2;
    int dx = (x1 - x0) * 16, dy = (y1 - y0) * 16, len2 = dx * dx + dy * dy, x, y;
    for (y = by0; y <= by1; y++) {
        int sx0 = bx0, sx1 = bx1;
        if (y < s_ylo || y >= s_yhi) continue;
        if (y1 != y0) {   /* the row's span: where the line is within 2 rows of it, 3 px more each side (not the whole box) */
            int ya = y < y0 && y < y1 ? (y0 < y1 ? y0 : y1) : y > y0 && y > y1 ? (y0 > y1 ? y0 : y1) : y;
            int xa = x0 + (ya - y0 - 2) * (x1 - x0) / (y1 - y0), xb = x0 + (ya - y0 + 2) * (x1 - x0) / (y1 - y0);
            sx0 = (xa < xb ? xa : xb) - 3, sx1 = (xa > xb ? xa : xb) + 3;
            if (sx0 < bx0) sx0 = bx0;
            if (sx1 > bx1) sx1 = bx1;
        }
        for (x = sx0; x <= sx1; x++) {
            int px = x * 16 + 8 - x0 * 16 - 8, py = y * 16 + 8 - y0 * 16 - 8, ex, ey, d, cov;
            long long tnum = (long long)px * dx + (long long)py * dy;
            if (x < 0 || x >= PW) continue;
            if (len2 && tnum > 0) {
                if (tnum >= len2) ex = px - dx, ey = py - dy;
                else ex = px - (int)(tnum * dx / len2), ey = py - (int)(tnum * dy / len2);
            } else {
                ex = px, ey = py;
            }
            d = isqrt(ex * ex + ey * ey) - 12;
            cov = d <= -8 ? 256 : d >= 8 ? 0 : (8 - d) * 16;
            if (cov) mix(&img[(size_t)(y - s_org) * (size_t)pitch + x], col, alpha * cov >> 8);
        }
    }
}

/* a rounded box, filled */
static void rbox(uint32_t* img, int pitch, int x, int y, int w, int h, int r, uint32_t col, int alpha) {
    int i, j;
    for (j = y; j < y + h; j++) {
        if (j < s_ylo || j >= s_yhi) continue;
        for (i = x; i < x + w; i++) {
            int qx = i < x + r ? x + r - i : i >= x + w - r ? i - (x + w - r - 1) : 0;
            int qy = j < y + r ? y + r - j : j >= y + h - r ? j - (y + h - r - 1) : 0, cov = 256;
            if (i < 0 || i >= PW) continue;
            if (qx && qy) {
                int d = isqrt((qx * qx + qy * qy) * 256) - r * 16;
                cov = d <= -8 ? 256 : d >= 8 ? 0 : (8 - d) * 16;
            }
            if (cov) mix(&img[(size_t)(j - s_org) * (size_t)pitch + i], col, alpha * cov >> 8);
        }
    }
}

/* ---- the body: computed once ---- */
static uint32_t s_body[LH][LW];   /* ARGB, alpha the coverage */
static int s_body_ready;

static float fabs_(float v) { return v < 0 ? -v : v; }
static float fmax_(float a, float b) { return a > b ? a : b; }
static float fmin_(float a, float b) { return a < b ? a : b; }
static float sd_rbox(float px, float py, float hx, float hy, float r) {
    float qx = fabs_(px) - hx + r, qy = fabs_(py) - hy + r;
    float ox = fmax_(qx, 0), oy = fmax_(qy, 0);
    return __builtin_sqrtf(ox * ox + oy * oy) + fmin_(fmax_(qx, qy), 0) - r;
}
static float sd_capsule(float px, float py, float ax, float ay, float bx, float by, float r) {
    float pax = px - ax, pay = py - ay, bax = bx - ax, bay = by - ay;
    float h = (pax * bax + pay * bay) / (bax * bax + bay * bay), dx, dy;
    h = h < 0 ? 0 : h > 1 ? 1 : h;
    dx = pax - bax * h, dy = pay - bay * h;
    return __builtin_sqrtf(dx * dx + dy * dy) - r;
}
static float smin(float a, float b, float k) {
    float h = 0.5f + 0.5f * (b - a) / k;
    h = h < 0 ? 0 : h > 1 ? 1 : h;
    return b + (a - b) * h - k * h * (1 - h);
}
/* the controller's outline, in pixels around (CX, CY) */
static float sd_body(float x, float y) {
    float c = sd_rbox(x, y + 16, 92, 44, 36);
    float gl = sd_capsule(x, y, -74, -6, -96, 68, 30);
    float gr = sd_capsule(x, y, 74, -6, 96, 68, 30);
    return smin(smin(c, gl, 16), gr, 16);
}
static float sd_trigger(float x, float y) {
    return fmin_(sd_rbox(x + 70, y + 60, 28, 12, 9), sd_rbox(x - 70, y + 60, 28, 12, 9));
}

static void body_layer(void) {
    int x, y;
    for (y = 0; y < LH; y++)
        for (x = 0; x < LW; x++) {
            float px = (float)(LX0 + x - CX) + 0.5f, py = (float)(LY0 + y - CY) + 0.5f;
            float d = sd_body(px, py), dt = sd_trigger(px, py), e = 0.5f;
            float gx = (sd_body(px + e, py) - sd_body(px - e, py)), gy = (sd_body(px, py + e) - sd_body(px, py - e));
            float cov = d <= -0.5f ? 1.0f : d >= 0.5f ? 0.0f : 0.5f - d;
            float tcov = dt <= -0.5f ? 1.0f : dt >= 0.5f ? 0.0f : 0.5f - dt;
            float r, g, b, a;
            if (cov > 0) {
                /* the bevel: the slope's light from the top left, strongest at the edge */
                float edge = 1.0f - fmin_(-d / 14.0f, 1.0f), lit = -(gx * -0.45f + gy * -0.89f);
                float top = 1.0f - (py + 64) / 190.0f, k;
                if (edge < 0) edge = 0;
                k = 0.78f + 0.22f * top + lit * edge * 0.55f;
                r = 44 * k, g = 49 * k, b = 56 * k;
                if (d > -2.2f) r += 30, g += 34, b += 38;   /* the rim */
                /* a soft green under-glow along the bottom edge, the family's colour */
                if (gy > 0.3f && edge > 0.5f) g += 26 * (edge - 0.5f) * gy, b += 8 * (edge - 0.5f) * gy;
                a = cov;
            } else if (tcov > 0) {
                float k = 0.9f - (py + 72) / 40.0f * 0.3f;
                r = 70 * k, g = 76 * k, b = 84 * k;
                if (dt > -2.0f) r += 26, g += 28, b += 30;
                a = tcov;
            } else {
                /* the drop shadow, down and right */
                float ds = sd_body(px - 4, py - 7);
                a = ds < 10 ? (1.0f - ds / 10.0f) * 0.45f : 0;
                if (a < 0) a = 0;
                r = g = b = 0;
            }
            if (cov > 0 && tcov > 0 && cov < 1) {   /* the trigger behind the body's edge */
                r = r * cov + 80 * (1 - cov), g = g * cov + 86 * (1 - cov), b = b * cov + 94 * (1 - cov);
                a = 1.0f;
            }
            s_body[y][x] = ((uint32_t)(a * 255.0f) << 24) | (rgb((int)r, (int)g, (int)b) & 0xFFFFFFu);
        }
    s_body_ready = 1;
}

/* ---- the inputs: where each is drawn and where its callout points ---- */
typedef struct { int x, y, r; } Spot;
static const Spot k_spot[XHW_IN_COUNT] = {
    /* A B X Y */ { 70, -2, 9 }, { 88, -20, 9 }, { 52, -20, 9 }, { 70, -38, 9 },
    /* BLACK WHITE */ { 70, 20, 7 }, { 50, 12, 7 },
    /* LT RT */ { -70, -64, 0 }, { 70, -64, 0 },
    /* START */ { 12, -8, 0 },
    /* LS click, RS click: on the sticks */ { -64, -18, 0 }, { 30, 32, 0 },
    /* D-pad, left stick, right stick */ { -34, 32, 19 }, { -64, -18, 21 }, { 30, 32, 19 },
};
#define BACK_X (-12)
#define BACK_Y (-8)

/* the callouts: column (0 left, 1 right) and slot (top to bottom) */
static const struct { int8_t col, slot; } k_call[XHW_IN_COUNT] = {
    /* A */ { 1, 4 }, /* B */ { 1, 3 }, /* X */ { 1, 2 }, /* Y */ { 1, 1 }, /* BLACK */ { 1, 6 }, /* WHITE */ { 1, 5 },
    /* LT */ { 0, 0 }, /* RT */ { 1, 0 }, /* START */ { 0, 4 }, /* LS click */ { 0, 2 }, /* RS click */ { 1, 8 },
    /* D-pad */ { 0, 5 }, /* left stick */ { 0, 1 }, /* right stick */ { 1, 7 },
};
/* the BACK callout ("Menu", fixed) takes the left column's slot 3 */
#define BACK_SLOT 3
static int slot_y(int col, int slot) { return col ? 100 + slot * 23 : 100 + slot * 36; }

/* the badge in a callout: the Xbox input's own look */
static const char* const k_badge[XHW_IN_COUNT] = { "A", "B", "X", "Y", "BL", "WH", "LT", "RT", "ST", "L3", "R3", "+", "LS", "RS" };
static uint32_t badge_col(int in) {
    switch (in) {
    case XHW_IN_A: return rgb(70, 190, 70);
    case XHW_IN_B: return rgb(220, 50, 50);
    case XHW_IN_X: return rgb(50, 110, 230);
    case XHW_IN_Y: return rgb(235, 200, 40);
    case XHW_IN_BLACK: return rgb(20, 20, 24);
    case XHW_IN_WHITE: return rgb(232, 232, 236);
    default: return rgb(96, 104, 118);
    }
}

/* the N64 control's colour, from its name: A blue, B green, C yellow, Start red */
static uint32_t n64_col(const char* m) {
    if (!strcmp(m, "A")) return rgb(110, 150, 255);
    if (!strcmp(m, "B")) return rgb(100, 220, 120);
    if (m[0] == 'C' && m[1] == '-') return rgb(255, 214, 80);
    if (!strcmp(m, "Start")) return rgb(255, 110, 100);
    if (!strcmp(m, "None")) return rgb(110, 118, 136);
    return rgb(226, 230, 240);
}

static int held(const xhw_panel* p, int in) {
    static const uint32_t k_btn[XHW_IN_COUNT] = {
        XHW_BTN_A, XHW_BTN_B, XHW_BTN_X, XHW_BTN_Y, XHW_BTN_BLACK, XHW_BTN_WHITE, 0, 0, XHW_BTN_START,
        XHW_BTN_LSTICK, XHW_BTN_RSTICK, XHW_BTN_UP | XHW_BTN_DOWN | XHW_BTN_LEFT | XHW_BTN_RIGHT,
    };
    if (!p->pad_ok) return 0;
    if (in == XHW_IN_LT) return p->pad_lt > 128;
    if (in == XHW_IN_RT) return p->pad_rt > 128;
    if (in == XHW_IN_LSTICK) return p->pad_lx * p->pad_lx + p->pad_ly * p->pad_ly > 12000 * 12000;
    if (in == XHW_IN_RSTICK) return p->pad_rx * p->pad_rx + p->pad_ry * p->pad_ry > 12000 * 12000;
    return (p->pad_buttons & k_btn[in]) != 0;
}

/* a glossy face button */
static void face(uint32_t* img, int pitch, int x, int y, int r, uint32_t c, int lit) {
    int x16 = x * 16, y16 = y * 16;
    disc(img, pitch, x16 + 16, y16 + 24, r * 16 + 8, rgb(0, 0, 0), 120);   /* shadow */
    disc(img, pitch, x16, y16, r * 16, shade(c, lit ? 330 : 200), 255);
    disc(img, pitch, x16 - r * 3, y16 - r * 4, r * 9, shade(c, lit ? 400 : 290), 190);
    disc(img, pitch, x16 - r * 5, y16 - r * 6, r * 4, rgb(255, 255, 255), lit ? 150 : 90);
    ring(img, pitch, x16, y16, r * 16 - 6, 12, shade(c, 110), 200);
}

/* a stick: its well, then the cap leaning with the live tilt */
static void stick(uint32_t* img, int pitch, int x, int y, int r, int tx, int ty, int lit) {
    int x16 = x * 16, y16 = y * 16, cx = x16 + tx * 7 * 16 / 32767, cy = y16 - ty * 7 * 16 / 32767;
    disc(img, pitch, x16, y16, r * 16, rgb(14, 16, 20), 255);
    ring(img, pitch, x16, y16, r * 16 - 8, 16, rgb(70, 76, 86), 200);
    disc(img, pitch, cx + 24, cy + 32, (r - 6) * 16, rgb(0, 0, 0), 140);
    disc(img, pitch, cx, cy, (r - 6) * 16, lit ? rgb(120, 200, 130) : rgb(92, 98, 108), 255);
    disc(img, pitch, cx, cy, (r - 10) * 16, lit ? rgb(150, 230, 160) : rgb(72, 78, 88), 255);   /* the dish */
    ring(img, pitch, cx, cy, (r - 6) * 16 - 8, 14, rgb(150, 156, 168), 120);
}

static void dpad(uint32_t* img, int pitch, int x, int y, int r, uint32_t buttons, int ok) {
    int a = 6, l = 14, i;
    disc(img, pitch, x * 16, y * 16, r * 16, rgb(14, 16, 20), 255);
    rbox(img, pitch, x - a + 1, y - l + 2, 2 * a, 2 * l, 3, rgb(0, 0, 0), 120);
    rbox(img, pitch, x - l + 1, y - a + 2, 2 * l, 2 * a, 3, rgb(0, 0, 0), 120);
    rbox(img, pitch, x - a, y - l, 2 * a, 2 * l, 3, rgb(96, 102, 112), 255);
    rbox(img, pitch, x - l, y - a, 2 * l, 2 * a, 3, rgb(96, 102, 112), 255);
    for (i = 0; i < 4 && ok; i++) {   /* a held direction lights its arm */
        static const uint32_t k_dir[4] = { XHW_BTN_UP, XHW_BTN_DOWN, XHW_BTN_LEFT, XHW_BTN_RIGHT };
        static const int8_t k_dx[4] = { 0, 0, -1, 1 }, k_dy[4] = { -1, 1, 0, 0 };
        if (buttons & k_dir[i])
            rbox(img, pitch, x + k_dx[i] * 9 - 4, y + k_dy[i] * 9 - 4, 8, 8, 2, rgb(150, 230, 160), 255);
    }
    disc(img, pitch, x * 16, y * 16, 3 * 16, rgb(70, 76, 86), 255);
}

static void small(uint32_t* img, int pitch, int x, int y, uint32_t c, int lit) {   /* BACK, START */
    rbox(img, pitch, x - 6, y - 3, 12, 7, 3, rgb(0, 0, 0), 120);
    rbox(img, pitch, x - 7, y - 4, 12, 7, 3, lit ? rgb(150, 230, 160) : c, 255);
}

/* the callout: a pill with the Xbox input's badge and the N64 control */
static void callout(uint32_t* img, int pitch, int bx, int by, const char* badge, uint32_t bcol, const char* m, int sel,
                    int changed, int lit, uint32_t t) {
    uint32_t fillc = sel ? rgb(26, 70, 84) : rgb(10, 14, 26), edge = sel ? rgb(120, 230, 210) : rgb(60, 74, 100);
    int k = sel ? pulse(t, 1200) : 0, bw = (int)strlen(badge) > 1 ? 22 : 16, tx;
    rbox(img, pitch, bx - 1, by - 1, BOX_W + 2, BOX_H + 2, 7, edge, sel ? 170 + k / 4 : 150);
    rbox(img, pitch, bx, by, BOX_W, BOX_H, 6, fillc, 225);
    if (lit) rbox(img, pitch, bx, by, BOX_W, BOX_H, 6, rgb(80, 200, 110), 90);
    /* the badge */
    rbox(img, pitch, bx + 2, by + 2, bw, BOX_H - 4, 5, bcol, 255);
    if (R_(bcol) + G_(bcol) + B_(bcol) < 120) {   /* BLACK's badge: an outline so it reads on the dark pill */
        rbox(img, pitch, bx + 2, by + 2, bw, 1, 0, rgb(130, 138, 150), 255);
        rbox(img, pitch, bx + 2, by + BOX_H - 3, bw, 1, 0, rgb(130, 138, 150), 255);
        rbox(img, pitch, bx + 2, by + 3, 1, BOX_H - 6, 0, rgb(130, 138, 150), 255);
        rbox(img, pitch, bx + 1 + bw, by + 3, 1, BOX_H - 6, 0, rgb(130, 138, 150), 255);
    }
    {
        int dark = R_(bcol) + G_(bcol) + B_(bcol) > 420;
        int gx = bx + 2 + (bw - tw(badge)) / 2, gy = by + 1;
        glyphs(img, pitch, badge, gx, gy, dark ? rgb(20, 22, 30) : rgb(250, 250, 255));
    }
    tx = bx + bw + 8;
    text(img, pitch, m, tx, by + 1, sel ? rgb(255, 240, 160) : n64_col(m));
    if (changed) disc(img, pitch, (bx + BOX_W - 7) * 16, (by + BOX_H / 2) * 16, 40, rgb(255, 190, 90), 255);
}

/* ---- the static layer: everything but the pulses, redrawn on a change ---- */
#define BAND_Y0 64             /* the rows it covers: the port line to the body's foot */
#define BAND_Y1 (LY0 + LH)
#define BAND_H (BAND_Y1 - BAND_Y0)
#ifndef XHW_PAD_LAYER
#define XHW_PAD_LAYER 1        /* -DXHW_PAD_LAYER=0: every shape drawn every frame, as before */
#endif

static uint32_t s_layer[BAND_H][PW];   /* premultiplied ARGB */

/* what the layer shows: when any of it changes the layer is drawn again */
typedef struct {
    int sel, port_sel, ok, lcx, lcy, rcx, rcy;
    uint32_t held, dirs;
    unsigned char changed[XHW_IN_COUNT];
    char map[XHW_IN_COUNT][12], port[24];
} LayerKey;
static LayerKey s_key;
static int s_key_ok;

/* a stick's cap, in 1/16 px from the well's centre (as stick() places it) */
static int lean(int v) { return v * 7 * 16 / 32767; }

static void layer_key(LayerKey* k, const xhw_panel* p) {
    int in;
    memset(k, 0, sizeof *k);
    k->sel = p->pad_sel, k->port_sel = p->pad_port_sel, k->ok = p->pad_ok;
    if (p->pad_ok) {
        k->lcx = lean(p->pad_lx), k->lcy = lean(p->pad_ly), k->rcx = lean(p->pad_rx), k->rcy = lean(p->pad_ry);
        k->dirs = p->pad_buttons & (XHW_BTN_UP | XHW_BTN_DOWN | XHW_BTN_LEFT | XHW_BTN_RIGHT);
    }
    for (in = 0; in < XHW_IN_COUNT; in++) k->held |= (uint32_t)held(p, in) << in;
    memcpy(k->changed, p->pad_changed, sizeof k->changed);
    memcpy(k->map, p->pad_map, sizeof k->map);
    memcpy(k->port, p->pad_port, sizeof k->port);
}

/* the body, the leaders, the controls and the callouts not selected; the
 * port line's text when it is not selected */
static void draw_static(uint32_t* img, int pitch, const xhw_panel* p) {
    int x, y, in;
    if (!p->pad_port_sel) text(img, pitch, p->pad_port, (PW - tw(p->pad_port)) / 2, 76, rgb(220, 226, 240));

    /* the body */
    for (y = 0; y < LH; y++) {
        uint32_t* row = img + (size_t)(LY0 + y - s_org) * (size_t)pitch + LX0;
        for (x = 0; x < LW; x++) {
            uint32_t c = s_body[y][x];
            if (c >> 24) mix(&row[x], c | 0xFF000000u, (int)(c >> 24));
        }
    }
    /* the jewel */
    disc(img, pitch, CX * 16, (CY - 36) * 16, 15 * 16, rgb(10, 12, 16), 255);
    disc(img, pitch, CX * 16, (CY - 36) * 16, 12 * 16, rgb(60, 160, 60), 255);
    disc(img, pitch, (CX - 3) * 16, (CY - 40) * 16, 6 * 16, rgb(140, 220, 120), 160);
    line(img, pitch, CX - 6, CY - 42, CX + 6, CY - 30, rgb(20, 60, 20), 230);
    line(img, pitch, CX + 6, CY - 42, CX - 6, CY - 30, rgb(20, 60, 20), 230);

    /* the leader lines, under everything else on the body */
    for (in = -1; in < XHW_IN_COUNT; in++) {
        int col = in < 0 ? 0 : k_call[in].col, slot = in < 0 ? BACK_SLOT : k_call[in].slot;
        int ax = CX + (in < 0 ? BACK_X : k_spot[in].x), ay = CY + (in < 0 ? BACK_Y : k_spot[in].y);
        int by = slot_y(col, slot) + BOX_H / 2, bx = col ? COL_R : COL_L + BOX_W, ex = col ? bx - 14 : bx + 14;
        int sel = in >= 0 && in == p->pad_sel;
        uint32_t lc = sel ? rgb(140, 240, 220) : rgb(90, 120, 150);
        int la = sel ? 230 : 110;
        if (in == XHW_IN_LT || in == XHW_IN_RT) ay -= 4;
        line(img, pitch, bx, by, ex, by, lc, la);
        line(img, pitch, ex, by, ax, ay, lc, la);
        disc(img, pitch, ax * 16, ay * 16, sel ? 40 : 28, lc, 255);
    }

    /* the wells, sticks, d-pad, buttons */
    dpad(img, pitch, CX + k_spot[XHW_IN_DPAD].x, CY + k_spot[XHW_IN_DPAD].y, k_spot[XHW_IN_DPAD].r, p->pad_buttons,
         p->pad_ok);
    stick(img, pitch, CX + k_spot[XHW_IN_LSTICK].x, CY + k_spot[XHW_IN_LSTICK].y, k_spot[XHW_IN_LSTICK].r,
          p->pad_ok ? p->pad_lx : 0, p->pad_ok ? p->pad_ly : 0,
          held(p, XHW_IN_LSTICK) || held(p, XHW_IN_LCLICK));
    stick(img, pitch, CX + k_spot[XHW_IN_RSTICK].x, CY + k_spot[XHW_IN_RSTICK].y, k_spot[XHW_IN_RSTICK].r,
          p->pad_ok ? p->pad_rx : 0, p->pad_ok ? p->pad_ry : 0,
          held(p, XHW_IN_RSTICK) || held(p, XHW_IN_RCLICK));
    for (in = XHW_IN_A; in <= XHW_IN_WHITE; in++)
        face(img, pitch, CX + k_spot[in].x, CY + k_spot[in].y, k_spot[in].r, badge_col(in), held(p, in));
    small(img, pitch, CX + BACK_X, CY + BACK_Y, rgb(150, 156, 168), 0);
    small(img, pitch, CX + k_spot[XHW_IN_START].x, CY + k_spot[XHW_IN_START].y, rgb(150, 156, 168),
          held(p, XHW_IN_START));
    for (in = XHW_IN_LT; in <= XHW_IN_RT; in++)   /* a pulled trigger lights */
        if (held(p, in)) rbox(img, pitch, CX + k_spot[in].x - 22, CY - 68, 44, 8, 4, rgb(150, 230, 160), 200);

    /* the callouts but the selected one */
    callout(img, pitch, COL_L, slot_y(0, BACK_SLOT), "BK", rgb(96, 104, 118), "Menu", 0, 0, 0, 0);
    for (in = 0; in < XHW_IN_COUNT; in++) {
        int col = k_call[in].col;
        if (in == p->pad_sel) continue;
        callout(img, pitch, col ? COL_R : COL_L, slot_y(col, k_call[in].slot), k_badge[in], badge_col(in),
                p->pad_map[in], 0, p->pad_changed[in], held(p, in), 0);
    }
}

void xhw_panel_pad_render(uint32_t* img, int pitch, const xhw_panel* p) {
    uint32_t t = p->t_ms;
    int x, y;
    if (!s_body_ready) body_layer();

    if (XHW_PAD_LAYER) {
        LayerKey k;
        layer_key(&k, p);
        if (!s_key_ok || memcmp(&k, &s_key, sizeof k)) {
            memset(s_layer, 0, sizeof s_layer);
            s_ylo = s_org = BAND_Y0, s_yhi = BAND_Y1;
            draw_static(&s_layer[0][0], PW, p);
            s_ylo = s_org = 0, s_yhi = PH;
            s_key = k, s_key_ok = 1;
        }
    }

    /* the port line's glow, under its text */
    if (p->pad_port_sel) {
        int w = tw(p->pad_port), px = (PW - w) / 2, py = 76, k = pulse(t, 1200);
        rbox(img, pitch, px - 14, py - 3, w + 28, 22, 8, rgb(40, 140, 160), 110 + k / 5);
        text(img, pitch, p->pad_port, px, py, rgb(255, 236, 150));
    }

    if (XHW_PAD_LAYER) {   /* the layer over the sky: premultiplied, one multiply a channel */
        for (y = 0; y < BAND_H; y++) {
            uint32_t* d = img + (size_t)(BAND_Y0 + y) * (size_t)pitch;
            const uint32_t* l = s_layer[y];
            for (x = 0; x < PW; x++) {
                uint32_t c = l[x], a = c >> 24, b;
                if (!a) continue;
                if (a == 255) {
                    d[x] = c;
                    continue;
                }
                b = d[x], a = 255 - a;
                d[x] = (b & 0xFF000000u) | ((uint32_t)(R_(c) + DIV255(R_(b) * (int)a)) << 16) |
                       ((uint32_t)(G_(c) + DIV255(G_(b) * (int)a)) << 8) | (uint32_t)(B_(c) + DIV255(B_(b) * (int)a));
            }
        }
    } else {
        draw_static(img, pitch, p);
    }

    /* the selected input: a pulsing ring around it, and its callout */
    if (p->pad_sel >= 0 && p->pad_sel < XHW_IN_COUNT) {
        int in = p->pad_sel, col = k_call[in].col;
        const Spot* s = &k_spot[in];
        int k = pulse(t, 1200), r = s->r ? s->r + 5 : 11, sx = CX + s->x, sy = CY + s->y;
        if (in == XHW_IN_LT || in == XHW_IN_RT) {
            rbox(img, pitch, sx - 34, sy - 10, 68, 22, 10, rgb(120, 240, 220), 60 + k / 5);
        } else {
            if (in == XHW_IN_LCLICK || in == XHW_IN_RCLICK) r = 10;
            ring(img, pitch, sx * 16, sy * 16, r * 16 + k / 16, 40, rgb(120, 240, 220), 150 + k / 4);
            ring(img, pitch, sx * 16, sy * 16, r * 16 + 48 + k / 10, 56, rgb(60, 180, 190), 50 + k / 8);
        }
        callout(img, pitch, col ? COL_R : COL_L, slot_y(col, k_call[in].slot), k_badge[in], badge_col(in),
                p->pad_map[in], 1, p->pad_changed[in], held(p, in), t);
    }
}
