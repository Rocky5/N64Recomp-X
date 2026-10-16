/* settings.c - settings.ini (PLAN.md 5.2; Wings64-X's settings.c generalised,
 * Melee-X's file rules).
 *
 * Only E:\UDATA\<title id>\settings.ini is read. It is written with the
 * defaults on first boot and when a value changes, through
 * xhw_write_file_atomic (a temp file, flushed, read back, renamed over): a
 * failed write leaves the old file. Comments and unknown keys are dropped
 * on a rewrite; a file without the end marker (older, or cut short) is
 * rewritten whole.
 *
 *   [video]   output = auto|480i|480p|720p   progressive = 0|1
 *             widescreen = auto|on|off|stretch   texture_filter = n64|sharp
 *             fps = 0|1   video_confirmed = <mode>
 *   [audio]   volume = 0..100
 *   [input]   rumble = 0|1
 *   [port1]..[port4]  stick_deadzone = 0..60 (percent, fives)
 *             pak = auto|controller|rumble|none   pak_file = A..D (each pak
 *             in one port: a set that isn't goes back to A..D in order)
 *   [save]    slot = 1..4
 *   [system]  screenshots = 0|1
 *
 * The loading screen's holds (PLAN.md 5.5): BACK runs this boot at 480i
 * 4:3 and saves output = 480i, progressive = 0, widescreen = off; BACK +
 * START also rewrites everything else with the defaults, so a bad binding,
 * dead zone or volume can't lock a user out. 1080i is never offered: an
 * "output = 1080i" from elsewhere reads as 720p (xhw.h XHW_VIDEO_1080I). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "settings.h"
#include "xhw.h"

#define END_MARK "; end of settings"
/* n64rx build --video: the default output (XHW_OUT_*) */
#ifndef XRC_VIDEO_DEFAULT
#define XRC_VIDEO_DEFAULT XHW_OUT_AUTO
#endif

XrcSettings g_xrc_settings;

static const char* const k_out[] = { "auto", "480i", "480p", "720p", "1080i" };
static const char* const k_ws[] = { "off", "on", "auto", "stretch" };
static const char* const k_filter[] = { "n64", "sharp" };
static const char* const k_pak[] = { "auto", "controller", "rumble", "none" };

const char* xrc_settings_output_name(int o) { return o >= 0 && o < 5 ? k_out[o] : "auto"; }
const char* xrc_settings_widescreen_name(int w) { return w >= 0 && w < 4 ? k_ws[w] : "auto"; }
const char* xrc_settings_pak_name(int p) { return p >= 0 && p < 4 ? k_pak[p] : "auto"; }

void xrc_settings_defaults(XrcSettings* s) {
    int i;
    memset(s, 0, sizeof *s);
    s->output = XRC_VIDEO_DEFAULT;
    s->progressive = 1;
    s->widescreen = XRC_WS_AUTO;
    s->texture_filter = 0;
    s->fps = XHW_TEST_BUILD;
    s->volume = 100;
    s->rumble = 1;
    s->save_slot = 1;
    s->screenshots = 0;
    for (i = 0; i < XRC_PORTS; i++) s->stick_deadzone[i] = 20, s->pak_file[i] = i;
}

static char* trim(char* s) {
    char* e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = '\0';
    return s;
}

static int pick(const char* v, const char* const* names, int n, int dflt) {
    int i;
    for (i = 0; i < n; i++)
        if (!strcmp(v, names[i])) return i;
    return dflt;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

int xrc_settings_parse(XrcSettings* s, char* text) {
    char section[16] = "";
    char* line = text;
    int whole = strstr(text, END_MARK) != NULL;
    while (line && *line) {
        char* nl = strchr(line, '\n');
        char *k, *v, *eq, *c;
        if (nl) *nl = '\0';
        c = strchr(line, ';');
        if (c) *c = '\0';
        k = trim(line);
        if (k[0] == '[') {
            char* e = strchr(k, ']');
            if (e) *e = '\0';
            snprintf(section, sizeof section, "%s", k + 1);
        } else if ((eq = strchr(k, '=')) != NULL) {
            *eq = '\0';
            k = trim(k);
            v = trim(eq + 1);
            if (!strcmp(section, "video")) {
                if (!strcmp(k, "output")) {
                    s->output = pick(v, k_out, 5, XHW_OUT_AUTO);
                    if (s->output >= XHW_OUT_COUNT) s->output = XHW_OUT_720P;
                } else if (!strcmp(k, "progressive")) s->progressive = atoi(v) != 0;
                else if (!strcmp(k, "widescreen")) s->widescreen = pick(v, k_ws, 4, XRC_WS_AUTO);
                else if (!strcmp(k, "texture_filter")) s->texture_filter = pick(v, k_filter, 2, 0);
                else if (!strcmp(k, "fps")) s->fps = atoi(v) != 0;
                else if (!strcmp(k, "video_confirmed"))
                    snprintf(s->video_confirmed, sizeof s->video_confirmed, "%s", v);
            } else if (!strcmp(section, "audio")) {
                if (!strcmp(k, "volume")) s->volume = clampi(atoi(v), 0, 100);
            } else if (!strcmp(section, "input")) {
                if (!strcmp(k, "rumble")) s->rumble = atoi(v) != 0;
            } else if (!strncmp(section, "port", 4) && section[4] >= '1' && section[4] <= '0' + XRC_PORTS &&
                       !section[5]) {
                int port = section[4] - '1';
                if (!strcmp(k, "stick_deadzone")) s->stick_deadzone[port] = clampi(atoi(v), 0, 60) / 5 * 5;
                else if (!strcmp(k, "pak")) s->pak[port] = pick(v, k_pak, 4, XRC_PAK_AUTO);
                else if (!strcmp(k, "pak_file") && ((v[0] >= 'A' && v[0] <= 'D') || (v[0] >= 'a' && v[0] <= 'd')) && !v[1])
                    s->pak_file[port] = (v[0] | 0x20) - 'a';
            } else if (!strcmp(section, "save")) {
                if (!strcmp(k, "slot")) s->save_slot = clampi(atoi(v), 1, 4);
            } else if (!strcmp(section, "system")) {
                if (!strcmp(k, "screenshots")) s->screenshots = atoi(v) != 0;
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    {   /* every pak in one port */
        int seen = 0, i;
        for (i = 0; i < XRC_PORTS; i++) seen |= 1 << s->pak_file[i];
        if (seen != (1 << XRC_PORTS) - 1)
            for (i = 0; i < XRC_PORTS; i++) s->pak_file[i] = i;
    }
    return whole;
}

size_t xrc_settings_format(const XrcSettings* s, char* out, size_t cap) {
    size_t n = 0;
    int i, w;
#define PUT(...)                                                    \
    do {                                                            \
        w = snprintf(out + n, n < cap ? cap - n : 0, __VA_ARGS__); \
        if (w < 0 || n + (size_t)w >= cap) return 0;                \
        n += (size_t)w;                                             \
    } while (0)
    PUT("; N64Recomp-X settings: the BACK menu writes this file (docs/platform.md)\r\n");
    PUT("[video]\r\noutput = %s\r\nprogressive = %d\r\nwidescreen = %s\r\ntexture_filter = %s\r\nfps = %d\r\n"
        "video_confirmed = %s\r\n",
        xrc_settings_output_name(s->output), s->progressive, xrc_settings_widescreen_name(s->widescreen),
        k_filter[s->texture_filter ? 1 : 0], s->fps, s->video_confirmed);
    PUT("[audio]\r\nvolume = %d\r\n", s->volume);
    PUT("[input]\r\nrumble = %d\r\n", s->rumble);
    for (i = 0; i < XRC_PORTS; i++)
        PUT("[port%d]\r\nstick_deadzone = %d\r\npak = %s\r\npak_file = %c\r\n", i + 1, s->stick_deadzone[i],
            xrc_settings_pak_name(s->pak[i]), 'A' + (s->pak_file[i] & 3));
    PUT("[save]\r\nslot = %d\r\n", s->save_slot);
    PUT("[system]\r\nscreenshots = %d\r\n", s->screenshots);
    PUT(END_MARK "\r\n");
#undef PUT
    return n;
}

#ifndef XRC_SETTINGS_HOST
void xu_pak_apply_settings(int hot);   /* ultra/save.c */

static void path(char* p, size_t n) { snprintf(p, n, "%ssettings.ini", xhw_save_dir()); }

int xrc_settings_save(void) {
    char buf[1024], p[96];
    size_t n = xrc_settings_format(&g_xrc_settings, buf, sizeof buf);
    path(p, sizeof p);
    if (!n || !xhw_write_file_atomic(p, buf, (uint32_t)n)) {
        xhw_logf("[SETTINGS] could not write %s", p);
        return 0;
    }
    xhw_log("[SETTINGS] saved");
    return 1;
}

static void log_settings(void) {
    const XrcSettings* s = &g_xrc_settings;
    xhw_logf("[SETTINGS] output %s, progressive %d, widescreen %s, filter %s, fps %d, volume %d, rumble %d, slot %d, "
             "dead zone %d/%d/%d/%d%%, paks %c%c%c%c, confirmed %s",
             xrc_settings_output_name(s->output), s->progressive, xrc_settings_widescreen_name(s->widescreen),
             k_filter[s->texture_filter ? 1 : 0], s->fps, s->volume, s->rumble, s->save_slot, s->stick_deadzone[0],
             s->stick_deadzone[1], s->stick_deadzone[2], s->stick_deadzone[3], "ACRN"[s->pak[0] & 3] + 0, "ACRN"[s->pak[1] & 3] + 0,
             "ACRN"[s->pak[2] & 3] + 0, "ACRN"[s->pak[3] & 3] + 0,
             s->video_confirmed[0] ? s->video_confirmed : "none");
}

void xrc_settings_load(void) {
    static char text[2048];
    char p[96];
    void* f;
    int write = 0;
    xrc_settings_defaults(&g_xrc_settings);
    path(p, sizeof p);
    f = xhw_file_open(p);
    if (f) {
        uint32_t n = xhw_file_size(f);
        if (n > sizeof text - 1) n = sizeof text - 1;
        if (xhw_file_read(f, 0, text, n)) {
            text[n] = '\0';
            write = !xrc_settings_parse(&g_xrc_settings, text);
        } else {
            write = 1;
        }
        xhw_file_close(f);
    } else {
        write = 1;
        xhw_log("[SETTINGS] no settings.ini: defaults written");
    }
    log_settings();
    if (write) xrc_settings_save();
    xu_pak_apply_settings(0);
}

void xrc_settings_apply_hold(int hold) {
    XrcSettings* s = &g_xrc_settings;
    if (hold == XHW_HOLD_NONE) return;
    if (hold >= XHW_HOLD_SAFE_RESET) {
        xrc_settings_defaults(s);
        xu_pak_apply_settings(0);
        xhw_log("[SETTINGS] reset at boot (BACK + START): defaults");
    }
    s->output = XHW_OUT_480I;
    s->progressive = 0;
    s->widescreen = XRC_WS_OFF;
    xhw_log("[SETTINGS] BACK held at boot: 480i 4:3 (saved)");
    log_settings();
    xrc_settings_save();
}
#endif
