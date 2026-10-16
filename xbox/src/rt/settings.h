/* settings.h - the port's settings, per game in E:\UDATA\<title id>\settings.ini
 * (PLAN.md 5.2, Melee-X's settings.c rules). The BACK menu (M2) edits the
 * same struct. */
#ifndef XRC_SETTINGS_H
#define XRC_SETTINGS_H

#include <stddef.h>

enum { XRC_WS_OFF = 0, XRC_WS_ON = 1, XRC_WS_AUTO = 2, XRC_WS_STRETCH = 3 };
#define XRC_PORTS 4

typedef struct XrcSettings {
    int output;                 /* XHW_OUT_*: auto, 480i, 480p, 720p */
    int progressive;            /* 0: 480i where the dashboard allows 480p */
    int widescreen;             /* XRC_WS_* */
    int texture_filter;         /* 0 N64 (bilinear), 1 sharp */
    int fps;                    /* the FPS counter */
    int volume;                 /* 0..100 */
    int rumble;
    int save_slot;              /* 1..4 */
    int screenshots;            /* BACK + WHITE screenshots (test builds) */
    int stick_deadzone[XRC_PORTS];   /* percent, 0..60 in fives */
    char video_confirmed[16];   /* the HD mode this console has shown and A kept */
} XrcSettings;

extern XrcSettings g_xrc_settings;

/* pure (tests/xbox/test_settings.c) */
void xrc_settings_defaults(XrcSettings* s);
/* parses settings.ini text in place (cuts it up); returns 1 when the file
 * was whole (its end marker present) */
int xrc_settings_parse(XrcSettings* s, char* text);
/* the whole file; returns its length (0 if cap is too small) */
size_t xrc_settings_format(const XrcSettings* s, char* out, size_t cap);
const char* xrc_settings_output_name(int output);
const char* xrc_settings_widescreen_name(int ws);

/* the console side */
void xrc_settings_load(void);
int xrc_settings_save(void);
/* the loading screen's latched hold (XHW_HOLD_*): safe video, and with
 * BACK + START the defaults too; saved, logged as [SETTINGS] */
void xrc_settings_apply_hold(int hold);

#endif
