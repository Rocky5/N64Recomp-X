/* test_splash.c - the loading screen's layout on the host (PLAN.md 5.5).
 *
 * Built by tools/xbox/test_splash.py with xbox/src/hw/xhw_splash_draw.c and
 * nxdk's unscii-16 font; the renderer needs nothing but a frame pointer, so
 * this draws exactly what the console draws. Checks:
 *   - nothing but the background outside the TV-safe area (the frame minus
 *     32 px): every pixel where the full frame differs from the background
 *     alone lies inside it, for the loading screen with the longest strings
 *     it can be given, for every hold state, and for a full error card
 *   - every stage label, with the longest title a pack may have, fits its
 *     line; the card title at the zoom the renderer picks fits the frame
 *   - the background stays in the family's palette (dark and cool: navy,
 *     teal, violet; sparks pale teal)
 *   - -DXHW_SPLASH_STILL frames are the same whatever the time
 * Writes the frames it checked as raw ARGB for the .py to save as PNGs
 * (build-host/splash_*.png), for the eye. Exit code 0 when all passed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xhw_splash.h"

#define W XHW_SPLASH_W
#define H XHW_SPLASH_H
#define SAFE XHW_SPLASH_SAFE

static int s_fail, s_checks;
#define CHECK(cond, ...) do { \
        s_checks++; \
        if (!(cond)) { s_fail++; printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); \
                       printf("\n"); } \
    } while (0)

static uint32_t s_full[W * H], s_bg[W * H];
static const char* s_outdir;

static void save(const char* name, const uint32_t* fb) {
    char path[512];
    FILE* f;
    if (!s_outdir) return;
    snprintf(path, sizeof path, "%s/%s.argb", s_outdir, name);
    f = fopen(path, "wb");
    if (!f) return;
    fwrite(fb, 4, W * H, f);
    fclose(f);
}

/* the full frame against the background alone: what differs is foreground */
static void check_safe(const char* what, xhw_splash_state st) {
    int x, y, bad = 0, bx = 0, by = 0, any = 0;
    st.layers = XHW_SPLASH_ALL;
    xhw_splash_render(s_full, W, &st);
    st.layers = XHW_SPLASH_BG;
    xhw_splash_render(s_bg, W, &st);
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            if (s_full[y * W + x] == s_bg[y * W + x]) continue;
            any = 1;
            if (x < SAFE || x >= W - SAFE || y < SAFE || y >= H - SAFE) {
                if (!bad) bx = x, by = y;
                bad++;
            }
        }
    CHECK(any, "%s: no foreground drawn", what);
    CHECK(!bad, "%s: %d foreground pixels outside the safe area, the first at (%d, %d)", what, bad, bx, by);
}

static xhw_splash_state loading(void) {
    xhw_splash_state st;
    memset(&st, 0, sizeof st);
    st.t_ms = 1800;
    st.wordmark = "TechProGabe Presents...";
    /* the longest title a pack may have (card_title holds 39 characters) */
    snprintf(st.card_title, sizeof st.card_title, "%s", "The Legend of Zelda: Majora's Mask (US)X");
    snprintf(st.card_sub, sizeof st.card_sub, "%s", "the ROM-free end-to-end test, longest sub-line here");
    snprintf(st.version, sizeof st.version, "%s", "N64Recomp-X 10.20.30-dev");
    snprintf(st.stage, sizeof st.stage, "Starting %s", st.card_title);
    snprintf(st.stage_ms, sizeof st.stage_ms, "%s", "99999 ms");
    st.progress = 0.6f;
    snprintf(st.video, sizeof st.video, "%s", "Video: 720p 16:9 (the last boot did not confirm its mode).");
    snprintf(st.video_note, sizeof st.video_note, "%s", "First time at 720p here: press A within 15 s to keep it.");
    return st;
}

static uint32_t s_icon[64 * 64];

int main(int argc, char** argv) {
    static const char* const stages[] = { "Looking for a ROM...", "Reading ROM (64 MB)", "Checking the ROM (SHA-1)",
                                          "Preparing memory", "Loading code", "No game in this build" };
    xhw_splash_state st = loading();
    size_t i;
    int x, y, h;
    s_outdir = argc > 1 ? argv[1] : NULL;
    for (i = 0; i < 64 * 64; i++) s_icon[i] = 0xFF2080C0u;

    /* the safe area: with and without the icon, every hold state */
    check_safe("loading screen", st);
    st.icon = s_icon;
    check_safe("loading screen with the icon", st);
    for (h = 0; h <= XHW_HOLD_SAFE_RESET; h++) {
        char what[64];
        st.hold = h;
        st.holding = h;
        snprintf(what, sizeof what, "hold %d", h);
        check_safe(what, st);
    }
    st.layers = XHW_SPLASH_ALL;
    xhw_splash_render(s_full, W, &st);
    save("splash_loading", s_full);
    {   /* a card with every line full */
        xhw_splash_state c = loading();
        c.card = 1;
        snprintf(c.card_title, sizeof c.card_title, "%s", "Not a supported game (bad dump)");
        for (i = 0; i < XHW_CARD_LINES; i++)
            snprintf(c.card_lines[i], sizeof c.card_lines[i], "%s",
                     "SHA-1 0123456789abcdef0123456789abcdef01234567 (yours) 012345678901");
        c.card_footer = "Press any button or wait to return to the dashboard.";
        check_safe("error card", c);
        c.layers = XHW_SPLASH_ALL;
        xhw_splash_render(s_full, W, &c);
        save("splash_card", s_full);
    }

    /* the stage labels fit the line from the bar's left end to the safe edge */
    for (i = 0; i < sizeof stages / sizeof stages[0]; i++)
        CHECK(XHW_SPLASH_BAR_X + xhw_splash_text_width(stages[i], 1) <= W - SAFE, "stage '%s' too wide", stages[i]);
    CHECK(XHW_SPLASH_BAR_X + xhw_splash_text_width(st.stage, 1) <= W - SAFE, "'%s' too wide", st.stage);
    CHECK(xhw_splash_text_width(st.video, 1) <= W - 2 * SAFE, "the video line is too wide");
    CHECK(xhw_splash_text_width(st.video_note, 1) <= W - 2 * SAFE, "the probation note is too wide");
    for (h = 0; h <= XHW_HOLD_SAFE_RESET; h++)
        CHECK(xhw_splash_text_width(xhw_splash_hold_text(h, h), 1) <= W - 2 * SAFE, "hold line %d too wide", h);
    CHECK(xhw_splash_title_zoom("Pilotwings 64", 1) == XHW_SPLASH_TITLE_ZOOM, "a short title is not 2x");
    CHECK(xhw_splash_title_zoom(st.card_title, 1) == 1, "the longest title is not 1x");

    /* the background's palette: dark and cool */
    {
        int warm = 0, bright = 0;
        st.layers = XHW_SPLASH_BG;
        for (i = 0; i < 4; i++) {
            st.t_ms = (uint32_t)(i * 7919);
            xhw_splash_render(s_bg, W, &st);
            for (y = 0; y < H; y++)
                for (x = 0; x < W; x++) {
                    uint32_t c = s_bg[y * W + x];
                    int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
                    if (b < r) warm++;
                    if (r > 120 || g > 220 || b > 230) bright++;
                }
        }
        CHECK(!warm, "%d background pixels warmer than the palette (blue under red)", warm);
        CHECK(!bright, "%d background pixels brighter than the palette allows", bright);
    }

    /* STILL: the same frame at any time */
    {
        xhw_splash_state a = loading(), b = loading();
        a.still = b.still = 1;
        a.t_ms = 0;
        b.t_ms = 123456;
        a.layers = b.layers = XHW_SPLASH_ALL;
        xhw_splash_render(s_full, W, &a);
        xhw_splash_render(s_bg, W, &b);
        CHECK(!memcmp(s_full, s_bg, sizeof s_full), "STILL frames differ with the time");
    }

    printf("test_splash: %d checks, %d failed\n", s_checks, s_fail);
    return s_fail != 0;
}
