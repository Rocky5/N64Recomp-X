/* xhw_splash.h - the loading screen's renderer (xhw_splash_draw.c): one
 * whole 640x480x32 frame from a plain state, nothing but the frame pointer
 * and the font, so tests/xbox/test_splash.c draws it on the host
 * (PLAN.md 5.5). The thread, the timing and the holds are xhw_splash.c's. */
#ifndef XHW_SPLASH_H_INCLUDED
#define XHW_SPLASH_H_INCLUDED
#include <stdint.h>

#include "xhw.h"   /* XHW_HOLD_*; plain C, the host test includes it too */

#define XHW_SPLASH_W 640
#define XHW_SPLASH_H 480
#define XHW_SPLASH_SAFE 32   /* the TV-safe area: the frame minus 32 px on every side */

enum { XHW_SPLASH_BG = 1, XHW_SPLASH_FG = 2, XHW_SPLASH_ALL = 3 };
#define XHW_CARD_LINES 8

typedef struct xhw_splash_state {
    uint32_t t_ms;               /* since the loading screen started */
    int still;                   /* XHW_SPLASH_STILL: background frozen, wordmark settled, no highlight */
    int layers;                  /* XHW_SPLASH_BG | XHW_SPLASH_FG */
    const char* wordmark;        /* "TechProGabe Presents..." */
    char card_title[40];         /* the pack's title, "Looking for a ROM...", "No game built in" */
    char card_sub[64];           /* one small line under it */
    const uint32_t* icon;        /* 64x64 ARGB (the pack's icon.png), or NULL */
    char version[32];            /* "N64Recomp-X 0.1.0", bottom right */
    char stage[64];              /* "Reading ROM (8 MB)" */
    char stage_ms[16];           /* test builds: the stage's elapsed time, right of the label */
    float progress;              /* the bar, 0..1 */
    char video[72];              /* "Video: 720p 16:9 (the dashboard allows 720p)." */
    char video_note[72];         /* "First time at 720p here: ..." or empty */
    int hold;                    /* XHW_HOLD_*: what this boot has latched */
    int holding;                 /* XHW_HOLD_*: what is held down right now */
    /* the deep-space title card (PLAN.md 5.5, 2026-10-18) */
    char rom_id[40];             /* "NSME · USA · 1.0" (UTF-8), or empty */
    char rom_where[40];          /* "8 MB · built into this XBE" */
    int rom_ok;                  /* 0 not yet, 1 SHA-1 verified */
    int step;                    /* 1..XHW_SPLASH_STEPS: the stage's place (0: none) */
    int settings;                /* BACK held: 0 no, 1 held, 2 latched (the BACK menu opens before the game) */
    /* nothing pops in: when each part appeared (ms, as t_ms), faded in from then */
    uint32_t t_title, t_rom, t_ok, t_stage;
    float shown;                 /* the bar as drawn, eased towards the stage's place (0: from step and progress) */
    /* the BACK menu's card over the screen (before the game: xhw_splash_panel) */
    const uint32_t* panel;       /* XHW_PANEL_W x XHW_PANEL_H ARGB, alpha = coverage, or NULL */
    int panel_pitch;             /* pixels */
    /* card mode (xhw_error_card in the loading screen's dress): the stage,
     * bar and video lines give way to these */
    int card;
    char card_lines[XHW_CARD_LINES][72];
    const char* card_footer;     /* "Press any button or wait to return to the dashboard." */
} xhw_splash_state;

/* fb: 640x480 ARGB8888, pitch in pixels */
void xhw_splash_render(uint32_t* fb, int pitch_px, const xhw_splash_state* s);
/* the BACK menu's card (s->panel) over a frame already drawn, the frame
 * dimmed as under a game; fade 0..256 (the card fades in, the frame dims) */
void xhw_splash_panel_over(uint32_t* fb, int pitch_px, const xhw_splash_state* s, int fade);
/* the sky's cache (1.2 MB): freed when the loading screen is done */
void xhw_splash_free_cache(void);
/* width in pixels of s in unscii-16 at `zoom` */
int xhw_splash_text_width(const char* s, int zoom);
/* the card title's zoom (2x when it fits next to the icon, else 1x) */
int xhw_splash_title_zoom(const char* title, int icon);
/* the safe-video line for a hold state: what the screen says */
const char* xhw_splash_hold_text(int hold, int holding);
/* the settings hold's line (0 none, 1 held, 2 latched) */
const char* xhw_splash_settings_text(int settings);

/* the layout, for the host test */
#define XHW_SPLASH_WORDMARK_ZOOM 3
#define XHW_SPLASH_TITLE_ZOOM 2
#define XHW_SPLASH_BAR_X 120
#define XHW_SPLASH_BAR_W 400
#define XHW_SPLASH_BAR_H 8
#define XHW_SPLASH_STEPS 4       /* the ROM, memory, code, start */

/* the title card's type (Sora, xhw_fonts.inc): a string's width in pixels
 * in the face the renderer uses for `what` */
enum { XHW_FACE_LABEL, XHW_FACE_BODY, XHW_FACE_STAGE, XHW_FACE_TITLE2, XHW_FACE_TITLE };
int xhw_splash_face_width(int face, const char* utf8, int track);
/* the face the game's title is drawn in: TITLE when it fits the safe width, else TITLE2 */
int xhw_splash_title_face(const char* utf8);
/* the title as drawn: 1 line (l1), or 2 when even the smaller face is too wide */
int xhw_splash_title_lines(const char* utf8, char* l1, char* l2, int cap);

/* the BACK menu's Controls page: the controller and its callouts, drawn on
 * the card by xhw_panel_render (xhw_panel_pad.c) */
void xhw_panel_pad_render(uint32_t* img, int pitch_px, const xhw_panel* p);

extern const unsigned char xhw_font16[256 * 16];   /* unscii-16: 8x16, one byte a row, MSB left */

#endif
