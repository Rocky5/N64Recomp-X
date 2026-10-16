/* test_settings.c - settings.ini on the host (PLAN.md 5.2, 4.6).
 *
 * Built by tools/xbox/test_settings.py with xbox/src/rt/settings.c and
 * -DXRC_SETTINGS_HOST (the pure parse and format only). Checks: the defaults
 * survive a round trip; every key round-trips; values out of range are
 * clamped, 1080i reads as 720p, unknown keys and sections are dropped; a
 * file without the end marker is reported as cut; per-port keys stay in
 * their port; comments are ignored. Exit code 0 when all passed. */
#include <stdio.h>
#include <string.h>

#include "settings.h"
#include "xhw.h"

static int s_fail, s_checks;
#define CHECK(cond, ...) do { \
        s_checks++; \
        if (!(cond)) { s_fail++; printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); \
                       printf("\n"); } \
    } while (0)

static int same(const XrcSettings* a, const XrcSettings* b) { return !memcmp(a, b, sizeof *a); }

static void round_trip(const XrcSettings* in, const char* what) {
    char buf[1024];
    XrcSettings out;
    size_t n = xrc_settings_format(in, buf, sizeof buf);
    CHECK(n > 0, "%s: format failed", what);
    xrc_settings_defaults(&out);
    CHECK(xrc_settings_parse(&out, buf) == 1, "%s: the written file is not whole", what);
    CHECK(same(in, &out), "%s: the round trip changed something", what);
}

int main(void) {
    XrcSettings s, t;
    char text[1024];
    xrc_settings_defaults(&s);
    round_trip(&s, "defaults");

    s.output = XHW_OUT_720P;
    s.progressive = 0;
    s.widescreen = XRC_WS_STRETCH;
    s.texture_filter = 1;
    s.fps = 1;
    s.volume = 35;
    s.rumble = 0;
    s.save_slot = 3;
    s.screenshots = 1;
    s.stick_deadzone[0] = 0;
    s.stick_deadzone[1] = 15;
    s.stick_deadzone[2] = 60;
    s.stick_deadzone[3] = 45;
    snprintf(s.video_confirmed, sizeof s.video_confirmed, "720p");
    round_trip(&s, "every key changed");

    /* clamping, 1080i, unknown keys, comments, sections */
    snprintf(text, sizeof text,
             "; a comment\r\n[video]\r\noutput = 1080i ; trailing comment\r\nwidescreen = sideways\r\nmystery = 7\r\n"
             "[audio]\r\nvolume = 250\r\n[port2]\r\nstick_deadzone = 33\r\n[port9]\r\nstick_deadzone = 5\r\n"
             "[save]\r\nslot = 0\r\n[nonsense]\r\nvolume = 1\r\n");
    xrc_settings_defaults(&t);
    CHECK(xrc_settings_parse(&t, text) == 0, "a file without the end marker read as whole");
    CHECK(t.output == XHW_OUT_720P, "1080i read as %d, not 720p", t.output);
    CHECK(t.widescreen == XRC_WS_AUTO, "an unknown widescreen value is not auto (%d)", t.widescreen);
    CHECK(t.volume == 100, "volume 250 read as %d, not 100", t.volume);
    CHECK(t.stick_deadzone[1] == 30, "dead zone 33 read as %d, not 30", t.stick_deadzone[1]);
    CHECK(t.stick_deadzone[0] == 20 && t.stick_deadzone[2] == 20 && t.stick_deadzone[3] == 20,
          "[port2] or [port9] reached another port");
    CHECK(t.save_slot == 1, "slot 0 read as %d, not 1", t.save_slot);

    /* a cut-short file keeps what it had, and says so */
    xrc_settings_defaults(&s);
    s.volume = 40;
    xrc_settings_format(&s, text, sizeof text);
    text[strlen(text) / 2] = '\0';
    xrc_settings_defaults(&t);
    CHECK(xrc_settings_parse(&t, text) == 0, "a cut file read as whole");
    CHECK(t.volume == 40, "a cut file lost the volume before the cut (%d)", t.volume);

    /* a buffer too small is refused, not overrun */
    CHECK(xrc_settings_format(&s, text, 64) == 0, "format into 64 bytes did not refuse");

    printf("test_settings: %d checks, %d failed\n", s_checks, s_fail);
    return s_fail != 0;
}
