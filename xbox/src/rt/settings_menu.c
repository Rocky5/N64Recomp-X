/* settings_menu.c - the BACK menu: the port's settings over the frozen game,
 * the same in every pack (PLAN.md 5.2). Melee-X's menu.c semantics (rows,
 * `*` for a restart, "720p -> 480i" where the dashboard disallows, the
 * held-direction repeat (timed by the clock: the retrace count is not a
 * clock while two threads present and wait), A changes, B returns), drawn
 * by the platform as a card in the loading screen's dress (xhw_panel_render:
 * its aurora, sparks, the title's light sweep, the bar's green) over the
 * game's last frame, dimmed (f3d_hold_last + f3d_present_held). It fades
 * and slides in over 200 ms and fades out over 100 ms; 30 frames a second
 * once it is up (the game is frozen: the CPU is the menu's).
 *
 * BACK opens it and closes it, on any port: a tap, taken when BACK is let
 * go with nothing else pressed meanwhile, so BACK + WHITE (screenshots) and
 * the exit combos (L + R + BACK + BLACK / WHITE) never open it on the way.
 * While it is open the VI thread reads the controllers once a retrace
 * (xrc_menu_vi: the exit combos are fed from there too) and changes the
 * settings; the menu's own thread (level 13) holds the renderer and draws
 * the card frame after frame, so a slow frame never costs a press.
 * Meanwhile:
 *   - no retrace messages go out (ultra/vi.c), so the game waits where it
 *     waits for the retrace; a graphics task that comes anyway waits in
 *     gfx.c until the menu closes;
 *   - controller reads give neutral input (ultra/si.c), and after it closes
 *     until the buttons that closed it are let go;
 *   - no new audio is made, so the AC97 ring drains to silence.
 * DIVERGES: a pause the N64 did not have.
 *
 * Rows (M2's): Video output (*), Widescreen (Auto, On, Off), Texture filter, FPS counter,
 * Volume, Stick dead zone (per port, with the stick's live tilt), Controls
 * (per port: A opens the Controls page, below), Controller Pak (per port:
 * A opens the Paks page, below), Restart,
 * Exit to dashboard, Return. settings.ini is written on Return (or B, or
 * BACK) when something changed. Kill switch -DXRC_MENU=0: BACK does
 * nothing, settings.ini still works. */
#include <stdio.h>

#include "f3d_interp.h"
#include "settings.h"
#include "ultra/savedev.h"
#include "ultra/ultra.h"
#include "xgfx.h"

#ifndef XRC_MENU
#define XRC_MENU 1
#endif

void xrc_gfx_lock(void);
void xrc_gfx_unlock(void);
int xrc_gfx_ready(void);

/* -DXRC_MENU_PAKS=0: no Controller Pak row and no Paks page (the paks stay
 * as settings.ini says) */
#ifndef XRC_MENU_PAKS
#define XRC_MENU_PAKS 1
#endif

enum {
    ROW_VIDEO, ROW_WIDE, ROW_FILTER, ROW_FPS, ROW_VOLUME, ROW_PORT, ROW_STICK, ROW_CONTROLS, ROW_PAK, ROW_RESTART,
    ROW_EXIT, ROW_RETURN, N_ROWS
};
static const char* const k_label[N_ROWS] = {
    "Video output", "Widescreen", "Texture filter", "FPS counter", "Volume", "Controller", "  Stick dead zone",
    "  Controls", "  Controller Pak", "Restart", "Exit to dashboard", "Return",
};
#define HAS_PAKS (XRC_MENU_PAKS && XRC_SAVE_PAK)

#define DIRS (XHW_BTN_UP | XHW_BTN_DOWN | XHW_BTN_LEFT | XHW_BTN_RIGHT)
#define STICK_DIR 16000      /* the left stick as a d-pad: about half tilt */
#define TRIG_CHORD 100       /* a trigger past this while BACK is down: not a tap (L + R + BACK starts the combos) */
#define REPEAT_DELAY_MS 300  /* a held direction: the first repeat, then every REPEAT_EVERY_MS */
#define REPEAT_EVERY_MS 85
#define RELEASE_RETRACES 60  /* after closing, the game reads neutral until release, at most this long */
#define FADE_IN_MS 200
#define FADE_OUT_MS 100
#define SLIDE_PX 14

static volatile int s_open, s_want, s_close, s_leave, s_apply;
static int s_sel, s_port, s_inited;
static uint32_t s_prev;
static uint64_t s_repeat_at;   /* us */
static int s_release;
static int s_dash_720p, s_dash_480p, s_dash_wide;
static XrcSettings s_boot, s_at_open;
static xhw_panel s_panel;
static uint32_t s_img[512 * 512];   /* the card, a power-of-two texture's worth (1 MB) */
static uint64_t s_open_us;
static xhw_pad s_raw[4];
static int s_raw_ok[4];
/* the BACK tap, per port: down, and nothing else meanwhile */
static uint8_t s_back_down[4], s_back_clean[4];

/* ---- the Paks page ----
 * Per port: what is in it (Auto, a Controller Pak, a Rumble Pak, nothing),
 * which of the game's four paks (A..D, save.c: each in one port, so taking
 * another port's pak swaps the two), the notes on that pak (a window of
 * NOTES_SHOWN, scrolled to the selection) with A to delete one and X to
 * copy it to another of the game's paks, Format pak, Back. A change is a
 * hot-swap the game sees (save.c xu_pak_insert); a delete, a copy or a
 * format asks first (A again; anything else keeps the note).
 * The VI thread only asks: the menu's thread does the work, holding the
 * save lock, and reads the note table again every frame. */
enum { PK_PORT, PK_IN, PK_FILE, PK_NOTES };   /* then the notes (or "no notes"), Format pak, Back */
enum { CONFIRM_NONE, CONFIRM_DELETE, CONFIRM_FORMAT, CONFIRM_COPY };
#define OP_COPY 100   /* s_pak_op: OP_COPY + slot copies a note to Pak s_copy_to */
#define NOTES_SHOWN 6
#define MSG_MS 2500
static int s_page;                  /* 0 the settings, 1 the Paks page, 2 the Controls page */
static int s_psel, s_pport, s_scroll, s_confirm;
/* for the menu's thread: apply the settings; delete (1 + slot), copy (OP_COPY + slot), format (-1) */
static volatile int s_pak_apply, s_pak_op;
static xu_pak_note s_notes[16];
static volatile int s_nnotes, s_free;
static char s_msg[64];
static uint64_t s_msg_us;

/* ---- the Controls page ----
 * Per port: the Xbox controller drawn on the card (xhw_panel_pad.c), each
 * input's callout naming the N64 control it gives (settings.ini [portN],
 * si.c maps through it). Up / Down walk the port line, then the left
 * column's inputs, then the right's, top to bottom; Left / Right (or A)
 * change what the selected one gives: a button any N64 button or nothing,
 * the d-pad and the sticks the N64's stick, the C buttons, its d-pad or
 * nothing. Y puts the port's defaults back. What is held on that port's
 * pad lights up on the drawing as it happens, so a change can be tried at
 * once. A change applies at once (the game reads through the table). */
#define CTL_ITEMS (1 + XHW_IN_COUNT)   /* the port line, then the inputs */
static const int8_t k_ctl_order[XHW_IN_COUNT] = {
    XHW_IN_LT, XHW_IN_LSTICK, XHW_IN_LCLICK, XHW_IN_START, XHW_IN_DPAD,   /* the left column */
    XHW_IN_RT, XHW_IN_Y, XHW_IN_X, XHW_IN_B, XHW_IN_A, XHW_IN_WHITE, XHW_IN_BLACK, XHW_IN_RSTICK, XHW_IN_RCLICK,
};
static int s_csel, s_cport;

static int controls_changed(int port) {
    int in, n = 0;
    for (in = 0; in < XHW_IN_COUNT; in++) n += g_xrc_settings.controls[port][in] != xrc_controls_default(in);
    return n;
}

static int pk_note_rows(void) { return s_nnotes ? s_nnotes : 1; }
static int pk_format(void) { return PK_NOTES + pk_note_rows(); }
static int pk_back(void) { return pk_format() + 1; }
static int pk_items(void) { return pk_back() + 1; }
static int pk_note_at(int item) { return item >= PK_NOTES && item < PK_NOTES + s_nnotes ? item - PK_NOTES : -1; }

int xrc_menu_open(void) { return s_open; }

/* every raw pad state the runtime reads (si.c, under the pad lock): the tap */
void xrc_menu_feed(int port, const xhw_pad* p) {
    uint32_t others;
    if (!XRC_MENU || port < 0 || port > 3) return;
    others = p->buttons & ~XHW_BTN_BACK;
    if (p->buttons & XHW_BTN_BACK) {
        if (!s_back_down[port]) s_back_down[port] = 1, s_back_clean[port] = 1;
        if (others || p->lt > TRIG_CHORD || p->rt > TRIG_CHORD) s_back_clean[port] = 0;
    } else {
        if (s_back_down[port] && s_back_clean[port] && !xhw_combo_pending()) s_want = 1;
        s_back_down[port] = 0;
    }
}

/* si.c: the game reads neutral (open, or the closing press still down) */
int xrc_menu_blocks(uint32_t raw_buttons) {
    if (s_open) return 1;
    if (s_release > 0) {
        if (raw_buttons) return 1;
        s_release = 0;
    }
    return 0;
}

static int step(int v, int dir, int inc, int lo, int hi, int wrap) {
    int old = v;
    v = dir > 0 ? (v / inc + 1) * inc : ((v + inc - 1) / inc - 1) * inc;
    if (v > hi) v = wrap ? lo : old > hi ? old : hi;
    if (v < lo) v = wrap ? hi : old < lo ? old : lo;
    return v;
}

static const char* const k_out[4] = { "Auto", "480i", "480p", "720p" };
/* the menu's order: Auto, 480i, 480p, 720p (XHW_OUT_*; progressive follows) */
static int out_index(const XrcSettings* st) {
    if (st->output == XHW_OUT_AUTO) return 0;
    if (st->output >= XHW_OUT_720P) return 3;
    if (st->output == XHW_OUT_480P && st->progressive) return 2;
    return 1;
}
/* what the console runs with these settings (the dashboard has the last word) */
static int out_used(const XrcSettings* st) {
    int i = out_index(st);
    if (i == 0) return s_dash_720p ? 3 : s_dash_480p ? 2 : 1;
    if (i == 3 && !s_dash_720p) return s_dash_480p && st->progressive ? 2 : 1;
    if (i == 2 && !s_dash_480p) return 1;
    return i;
}
static int restart_needed(void) { return out_index(&g_xrc_settings) != out_index(&s_boot); }

static int wide_now(const XrcSettings* st) {
    const xhw_video_mode* vm = xhw_video();
    if (st->widescreen == XRC_WS_ON || st->widescreen == XRC_WS_STRETCH) return 1;
    if (st->widescreen == XRC_WS_AUTO) return vm->height >= 720 || s_dash_wide;
    return 0;
}

static void apply_now(void) {
    const XrcSettings* g = &g_xrc_settings;
    f3d_set_widescreen(g->widescreen != XRC_WS_OFF && wide_now(g));
    xgfx_set_tex_filter(g->texture_filter ? 2 : 0);
    xgfx_set_fps_overlay(g->fps);
}

static int stick_pct(int16_t x, int16_t y) {
    float fx = x / 32767.0f, fy = y / 32767.0f, m = __builtin_sqrtf(fx * fx + fy * fy);
    return m >= 1.0f ? 100 : (int)(m * 100.0f);
}

/* what a port holds, as its rows say it */
static void pak_in_text(int port, char* v, size_t cap, int with_pak) {
    int k = g_xrc_settings.pak[port] & 3, l = 'A' + (g_xrc_settings.pak_file[port] & 3);
    if (k == XRC_PAK_AUTO) {
        int now = xu_pak_kind(port);   /* AUTO turns into a Rumble Pak if the game probed for one */
        if (now == XU_PAK_RUMBLE) snprintf(v, cap, "Auto, Rumble Pak");
        else if (with_pak) snprintf(v, cap, "Auto, Pak %c", l);
        else snprintf(v, cap, "Auto, Controller Pak");
    } else if (k == XRC_PAK_CONTROLLER) {
        if (with_pak) snprintf(v, cap, "Pak %c", l);
        else snprintf(v, cap, "Controller Pak");
    } else {
        snprintf(v, cap, "%s", k == XRC_PAK_RUMBLE ? "Rumble Pak" : "Nothing");
    }
}

/* a row's value ("" for the actions) and whether it waits for a restart */
static void row_value(int r, char* v, size_t cap, int* star) {
    const XrcSettings* g = &g_xrc_settings;
    *star = 0;
    v[0] = 0;
    switch (r) {
    case ROW_VIDEO: {
        int i = out_index(g), u = out_used(g);
        if (i == 0) snprintf(v, cap, "Auto, %s", k_out[u]);
        else if (u != i) snprintf(v, cap, "%s -> %s", k_out[i], k_out[u]);
        else snprintf(v, cap, "%s", k_out[i]);
        *star = i != out_index(&s_boot);
        break;
    }
    case ROW_WIDE:
        if (g->widescreen == XRC_WS_AUTO) snprintf(v, cap, "Auto, %s", wide_now(g) ? "16:9" : "4:3");
        else snprintf(v, cap, "%s", g->widescreen == XRC_WS_ON ? "On" : g->widescreen == XRC_WS_STRETCH ? "Stretch" : "Off");
        break;
    case ROW_FILTER: snprintf(v, cap, "%s", g->texture_filter ? "Sharp" : "N64 (bilinear)"); break;
    case ROW_FPS: snprintf(v, cap, "%s", g->fps ? "On" : "Off"); break;
    case ROW_VOLUME: snprintf(v, cap, "%d%%", g->volume); break;
    case ROW_PORT: snprintf(v, cap, "Port %d%s", s_port + 1, s_raw_ok[s_port] ? "" : " (none)"); break;
    case ROW_STICK: snprintf(v, cap, "%d%%", g->stick_deadzone[s_port]); break;
    case ROW_PAK: pak_in_text(s_port, v, cap, 1); break;
    case ROW_CONTROLS: {
        int n = controls_changed(s_port);
        if (n) snprintf(v, cap, "%d changed", n);
        else snprintf(v, cap, "Default");
        break;
    }
    }
}

static void info_text(char* out, size_t cap) {
    const XrcSettings* g = &g_xrc_settings;
    switch (s_sel) {
    case ROW_VIDEO:
        if (out_used(g) != out_index(g) && out_index(g)) snprintf(out, cap, "The dashboard does not allow %s", k_out[out_index(g)]);
        else snprintf(out, cap, "Applies after a restart");
        break;
    case ROW_WIDE:
        if (xhw_video()->height >= 720) snprintf(out, cap, "Off: the 4:3 picture in the middle");
        else if (!s_dash_wide) snprintf(out, cap, "At 480 lines 16:9 needs the dashboard's widescreen");
        else snprintf(out, cap, "More world at the sides; the HUD stays 4:3");
        break;
    case ROW_FILTER: snprintf(out, cap, "Sharp: no texture smoothing"); break;
    case ROW_FPS: snprintf(out, cap, "Frames per second, top-left corner"); break;
    case ROW_VOLUME: snprintf(out, cap, "The game's sound"); break;
    case ROW_PORT: snprintf(out, cap, "The row below is per port"); break;
    case ROW_STICK:
        if (!s_raw_ok[s_port]) snprintf(out, cap, "No controller in port %d", s_port + 1);
        else {
            int m = stick_pct(s_raw[s_port].lx, s_raw[s_port].ly);
            snprintf(out, cap, "Left stick now %d%% (%s)", m, m > g->stick_deadzone[s_port] ? "moves" : "ignored");
        }
        break;
    case ROW_PAK: snprintf(out, cap, "A: what is in port %d, and the notes on its pak", s_port + 1); break;
    case ROW_CONTROLS: snprintf(out, cap, "A: what each button of port %d gives the game", s_port + 1); break;
    case ROW_RESTART: snprintf(out, cap, "Saves settings.ini and restarts the game"); break;
    case ROW_EXIT: snprintf(out, cap, "L + R + BACK + BLACK does it from anywhere"); break;
    default: snprintf(out, cap, "Saves settings.ini (B and BACK do too)"); break;
    }
}

/* ---- the Paks page: the menu's thread ---- */
static int s_confirm_slot, s_copy_to;
static char s_confirm_name[24];

static void pak_msg(const char* m) {
    snprintf(s_msg, sizeof s_msg, "%s", m);
    s_msg_us = xhw_time_us();
    xhw_logf("[MENU] %s", m);
}

/* the asked changes: the settings applied as a hot-swap, a delete, a
 * format, a copy */
static void pak_do(void) {
    int op = s_pak_op, port = s_pport, ok = 0, r = 0, to = s_copy_to;
    char m[64];
    if (s_pak_apply) s_pak_apply = 0, xu_pak_apply_settings(1);
    if (!op) return;
    s_pak_op = 0;
    xu_save_lock();
    {
        uint8_t* img = xu_pak_image(port);
        if (op >= OP_COPY) {   /* onto the port that holds that pak (every pak is in one) */
            int q;
            for (q = 0; q < 3 && xu_pak_letter(q) != to; q++) {}
            r = xu_pak_note_copy(img, op - OP_COPY, xu_pak_image(q));
            if (!r) xu_pak_dirty(q), xu_pak_pull(q);
        } else {
            if (op > 0) ok = xu_pak_note_delete(img, op - 1) == 0;
            else xu_pak_format_id(img, (uint32_t)xhw_time_us() * 2654435761u), ok = 1;   /* a pak with a new ID */
            if (ok) xu_pak_dirty(port), xu_pak_pull(port);   /* written by the save writer; the game re-reads it */
        }
    }
    xu_save_unlock();
    if (op >= OP_COPY) {
        if (!r) snprintf(m, sizeof m, "Copied %s to Pak %c", s_confirm_name, 'A' + to);
        else if (r == -2) snprintf(m, sizeof m, "Pak %c has that note already", 'A' + to);
        else if (r == -3) snprintf(m, sizeof m, "No room on Pak %c", 'A' + to);
        else snprintf(m, sizeof m, "%s can't be copied (broken)", s_confirm_name);
    } else if (op > 0) {
        snprintf(m, sizeof m, ok ? "Deleted %s" : "%s was already gone", s_confirm_name);
    } else {
        snprintf(m, sizeof m, "Pak %c formatted: no notes", 'A' + (g_xrc_settings.pak_file[port] & 3));
    }
    pak_msg(m);
}

/* the note table of the port's pak, every frame */
static void pak_refresh(void) {
    int n, f;
    xu_save_lock();
    n = xu_pak_notes(xu_pak_image(s_pport), s_notes, &f);
    xu_save_unlock();
    s_nnotes = n;
    s_free = f;
}

static void paks_info(char* out, size_t cap) {
    const XrcSettings* g = &g_xrc_settings;
    int port = s_pport, note = pk_note_at(s_psel), l = 'A' + (g->pak_file[port] & 3);
    if (s_psel == PK_PORT) snprintf(out, cap, "Left / Right: another controller's paks");
    else if (s_psel == PK_IN)
        snprintf(out, cap, "%s", (g->pak[port] & 3) == XRC_PAK_AUTO ? "Auto: a Rumble Pak if the game asks for one first"
                                                                    : "As if the pak were pulled out and put in");
    else if (s_psel == PK_FILE) {
        static const char* ext[4] = {".mpk", "-2.mpk", "-3.mpk", "-4.mpk"};
        snprintf(out, cap, "%s%s; another port's pak swaps with it", xrc_game.id ? xrc_game.id : "game",
                 ext[g->pak_file[port] & 3]);
    } else if (note >= 0) {
        const xu_pak_note* n = &s_notes[note];
        snprintf(out, cap, "Note %d of %d: %s, %d pages   A: delete  X: copy", note + 1, s_nnotes, n->code, n->pages);
    } else if (s_psel == pk_format()) snprintf(out, cap, "Erases every note on Pak %c", l);
    else if (s_psel == pk_back()) snprintf(out, cap, "B does it too");
    else snprintf(out, cap, "Nothing on Pak %c", l);
}

static void fill_paks(xhw_panel* p) {
    const XrcSettings* g = &g_xrc_settings;
    int port = s_pport, n = s_nnotes, shown = n < NOTES_SHOWN ? n : NOTES_SHOWN, r, i, note;
    int l = 'A' + (g->pak_file[port] & 3), k = g->pak[port] & 3;
    if (s_psel >= pk_items()) s_psel = pk_items() - 1;
    note = pk_note_at(s_psel);
    if (note >= 0 && note < s_scroll) s_scroll = note;
    if (note >= s_scroll + NOTES_SHOWN) s_scroll = note - NOTES_SHOWN + 1;
    if (s_scroll > n - shown) s_scroll = n - shown;
    if (s_scroll < 0) s_scroll = 0;
    snprintf(p->sub_left, sizeof p->sub_left, "CONTROLLER PAKS");
    snprintf(p->sub_right, sizeof p->sub_right, "Pak %c: %d note%s, %d pages free", l, n, n == 1 ? "" : "s", s_free);
    snprintf(p->label[0], sizeof p->label[0], "Port");
    snprintf(p->value[0], sizeof p->value[0], "Port %d%s", port + 1, s_raw_ok[port] ? "" : " (none)");
    snprintf(p->label[1], sizeof p->label[1], "In the port");
    pak_in_text(port, p->value[1], sizeof p->value[1], 0);
    snprintf(p->label[2], sizeof p->label[2], "Pak");
    snprintf(p->value[2], sizeof p->value[2], "Pak %c", l);
    r = PK_NOTES;
    for (i = 0; i < shown; i++, r++) {
        const xu_pak_note* nt = &s_notes[s_scroll + i];
        /* the window's ends say there is more */
        int more = (i == 0 && s_scroll > 0) || (i == shown - 1 && s_scroll + shown < n);
        snprintf(p->label[r], sizeof p->label[r], "%s%s", more ? (i == 0 ? "^ " : "v ") : "  ",
                 nt->name[0] ? nt->name : "(no name)");
        if (nt->pages < 0) snprintf(p->value[r], sizeof p->value[r], "broken");
        else snprintf(p->value[r], sizeof p->value[r], "%d page%s", nt->pages, nt->pages == 1 ? "" : "s");
        p->kind[r] = XHW_PANEL_ITEM;
    }
    if (!n) snprintf(p->label[r], sizeof p->label[r], "  No notes on this pak"), p->kind[r++] = XHW_PANEL_ITEM;
    snprintf(p->label[r], sizeof p->label[r], "Format pak");
    p->kind[r++] = XHW_PANEL_ACTION;
    snprintf(p->label[r], sizeof p->label[r], "Back");
    p->kind[r++] = XHW_PANEL_ACTION;
    p->rows = r;
    if (s_psel < PK_NOTES) p->sel = s_psel;
    else if (note >= 0) p->sel = PK_NOTES + note - s_scroll;
    else if (s_psel == pk_format()) p->sel = r - 2;
    else if (s_psel == pk_back()) p->sel = r - 1;
    else p->sel = PK_NOTES;
    paks_info(p->info, sizeof p->info);
    if (s_confirm == CONFIRM_DELETE) snprintf(p->note, sizeof p->note, "Delete %s?  A: delete   B: keep", s_confirm_name);
    else if (s_confirm == CONFIRM_COPY)
        snprintf(p->note, sizeof p->note, "Copy %s to < Pak %c >?  A: copy", s_confirm_name, 'A' + s_copy_to);
    else if (s_confirm == CONFIRM_FORMAT) snprintf(p->note, sizeof p->note, "Erase all of Pak %c?  A: erase   B: keep", l);
    else if (s_msg[0] && xhw_time_us() - s_msg_us < MSG_MS * 1000ull) snprintf(p->note, sizeof p->note, "%s", s_msg);
    else if (k == XRC_PAK_NONE || k == XRC_PAK_RUMBLE || xu_pak_kind(port) == XU_PAK_RUMBLE)
        snprintf(p->note, sizeof p->note, "Pak %c is not in port %d now", l, port + 1);
    snprintf(p->footer, sizeof p->footer, "A select     Left / Right change     B back");
}

/* the Controls page's part of the card */
static void fill_controls(xhw_panel* p) {
    const unsigned char* c = g_xrc_settings.controls[s_cport];
    int in, sel = s_csel > 0 ? k_ctl_order[s_csel - 1] : -1;
    p->page = XHW_PANEL_CONTROLS_PAGE;
    p->sel = -1;
    p->pad_sel = sel;
    p->pad_port_sel = s_csel == 0;
    snprintf(p->sub_left, sizeof p->sub_left, "CONTROLS");
    p->sub_right[0] = 0;
    snprintf(p->pad_port, sizeof p->pad_port, "<  Port %d%s  >", s_cport + 1, s_raw_ok[s_cport] ? "" : " (none)");
    for (in = 0; in < XHW_IN_COUNT; in++) {
        snprintf(p->pad_map[in], sizeof p->pad_map[in], "%s", xrc_controls_name(c[in]));
        p->pad_changed[in] = c[in] != xrc_controls_default(in);
    }
    p->pad_ok = s_raw_ok[s_cport];
    if (p->pad_ok) {
        const xhw_pad* r = &s_raw[s_cport];
        p->pad_buttons = r->buttons, p->pad_lx = r->lx, p->pad_ly = r->ly, p->pad_rx = r->rx, p->pad_ry = r->ry;
        p->pad_lt = r->lt, p->pad_rt = r->rt;
    }
    if (sel < 0) snprintf(p->info, sizeof p->info, "Left / Right: another controller");
    else if (c[sel] == XRC_N64_NONE) snprintf(p->info, sizeof p->info, "%s gives nothing", xrc_controls_input_name(sel));
    else snprintf(p->info, sizeof p->info, "%s gives the N64's %s", xrc_controls_input_name(sel), xrc_controls_name(c[sel]));
    if (s_msg[0] && xhw_time_us() - s_msg_us < MSG_MS * 1000ull) snprintf(p->note, sizeof p->note, "%s", s_msg);
    else if (sel >= 0) snprintf(p->note, sizeof p->note, "Left / Right: what it gives");
    snprintf(p->footer, sizeof p->footer, "Y defaults     B back");
}

/* the card's frame: fade 0..256, slide in pixels */
static void draw(int fade, int slide) {
    const xhw_video_mode* vm = xhw_video();
    int r;
    memset(&s_panel, 0, sizeof s_panel);
    s_panel.t_ms = (uint32_t)((xhw_time_us() - s_open_us) / 1000u);
    s_panel.sel = s_sel;
    s_panel.rows = N_ROWS;
    snprintf(s_panel.title, sizeof s_panel.title, "%s", xrc_game.title ? xrc_game.title : "N64Recomp-X");
    snprintf(s_panel.sub_left, sizeof s_panel.sub_left, "SETTINGS");
    snprintf(s_panel.sub_right, sizeof s_panel.sub_right, "running %s %s",
             vm->height >= 720 ? "720p" : vm->progressive ? "480p" : "480i",
             vm->height >= 720 || (vm->widescreen && s_dash_wide) ? "16:9" : "4:3");
    if (s_page == 2) {
        fill_controls(&s_panel);
    } else if (s_page) {
        fill_paks(&s_panel);
    } else {
        int n = 0;
        for (r = 0; r < N_ROWS; r++) {
            int star;
            if (r == ROW_PAK && !HAS_PAKS) continue;
            if (r == s_sel) s_panel.sel = n;
            snprintf(s_panel.label[n], sizeof s_panel.label[n], "%s", k_label[r]);
            row_value(r, s_panel.value[n], sizeof s_panel.value[n], &star);
            s_panel.star[n] = star;
            s_panel.kind[n] = r >= ROW_RESTART                     ? XHW_PANEL_ACTION
                              : r == ROW_PAK || r == ROW_CONTROLS ? XHW_PANEL_ITEM
                                                                  : XHW_PANEL_SETTING;
            n++;
        }
        s_panel.rows = n;
        info_text(s_panel.info, sizeof s_panel.info);
        if (restart_needed()) snprintf(s_panel.note, sizeof s_panel.note, "* used after a restart");
        snprintf(s_panel.footer, sizeof s_panel.footer, "A / Left / Right change     B return");
    }
    xhw_panel_render(s_img, 512, &s_panel);
    f3d_present_held(s_img, 512, 512, XHW_PANEL_W, XHW_PANEL_H, fade, slide);
}

/* the opening frames: faded and slid in, eased */
static void draw_now(void) {
    uint32_t ms = (uint32_t)((xhw_time_us() - s_open_us) / 1000u);
    if (ms >= FADE_IN_MS) {
        draw(256, 0);
    } else {
        int k = (int)(ms * 256u / FADE_IN_MS), e = 256 - ((256 - k) * (256 - k) >> 8);   /* ease out */
        draw(e, SLIDE_PX * (256 - e) / 256);
    }
}

static int save_if_changed(void) {
    if (!memcmp(&g_xrc_settings, &s_at_open, sizeof s_at_open)) return 1;
    if (!xrc_settings_save()) return 0;
    s_at_open = g_xrc_settings;
    return 2;
}

static void open_menu(void) {
    if (!s_inited) s_inited = 1, s_boot = g_xrc_settings;
    s_sel = 0;
    s_port = 0;
    s_page = 0;
    s_confirm = 0;
    s_prev = ~0u;     /* the press that opened it counts as held */
    s_close = s_leave = s_apply = 0;
    s_at_open = g_xrc_settings;
    s_dash_720p = xhw_video_720p_allowed();
    s_dash_480p = xhw_video_480p_allowed();
    s_dash_wide = xhw_video_widescreen_set();
    f3d_hold_last(1);
    s_open_us = xhw_time_us();
    s_open = 1;
    xhw_logf("[MENU] opened");
}

static void close_menu(void) {
    int ok = save_if_changed();
    uint64_t t0 = xhw_time_us(), t;
    while ((t = xhw_time_us() - t0) < FADE_OUT_MS * 1000ull) {   /* fade out (each present waits a vblank) */
        int k = 256 - (int)(t * 256u / (FADE_OUT_MS * 1000u));
        draw(k, (256 - k) * SLIDE_PX / 512);
    }
    f3d_present_held(NULL, 0, 0, 0, 0, 0, 0);
    s_open = 0;
    s_release = RELEASE_RETRACES;
    f3d_hold_last(0);
    xhw_logf("[MENU] closed (%s)", !ok ? "settings.ini could not be saved" : ok == 2 ? "settings.ini saved" : "nothing changed");
}

static void leave(int restart) {
    xu_save_flush();
    if (!save_if_changed()) xhw_log("[MENU] settings.ini could not be saved");
    xhw_logf("[MENU] %s", restart ? "Restart" : "Exit to dashboard");
    xhw_exit_reason(restart ? "restart from the BACK menu" : "exit to the dashboard from the BACK menu");
    xhw_pad_lock();   /* for good: nobody else enters SDL on the way out */
    if (restart) xhw_reboot_self();
    xhw_quit_to_dashboard();
}

static void change(int dir, int wrap) {
    XrcSettings* g = &g_xrc_settings;
    char v[40];
    int star;
    switch (s_sel) {
    case ROW_VIDEO: {
        int i = step(out_index(g), dir, 1, 0, 3, wrap);
        g->output = i == 0 ? XHW_OUT_AUTO : i == 1 ? XHW_OUT_480I : i == 2 ? XHW_OUT_480P : XHW_OUT_720P;
        if (i == 1) g->progressive = 0;
        else if (i == 2) g->progressive = 1;
        break;
    }
    case ROW_WIDE: {
        /* Auto, On, Off (Stretch comes with M6's interpreter work) */
        static const int order[3] = { XRC_WS_AUTO, XRC_WS_ON, XRC_WS_OFF };
        int i = 0;
        while (i < 3 && order[i] != g->widescreen) i++;
        g->widescreen = order[step(i % 3, dir, 1, 0, 2, 1)];
        break;
    }
    case ROW_FILTER: g->texture_filter = !g->texture_filter; break;
    case ROW_FPS: g->fps = !g->fps; break;
    case ROW_VOLUME: g->volume = step(g->volume, dir, 10, 0, 100, wrap); break;
    case ROW_PORT: s_port = step(s_port, dir, 1, 0, 3, 1); break;
    case ROW_STICK: g->stick_deadzone[s_port] = step(g->stick_deadzone[s_port], dir, 5, 0, 60, wrap); break;
    default: return;
    }
    s_apply = 1;   /* the menu's thread applies it, holding the renderer */
    row_value(s_sel, v, sizeof v, &star);
    xhw_logf("[MENU] %s: %s", k_label[s_sel] + (s_sel == ROW_STICK ? 2 : 0), v);
}

/* the four ports, raw, under the pad lock (try: never wait on a game read);
 * the exit combos are fed from here too. 0: the lock was busy */
static int poll(uint32_t* buttons) {
    uint32_t b = 0;
    int i;
    if (!xhw_pad_trylock()) return 0;
    xhw_pad_poll();
    for (i = 0; i < 4; i++) {
        xhw_pad* p = &s_raw[i];
        s_raw_ok[i] = xhw_pad_get(i, p) && p->connected;
        if (!s_raw_ok[i]) continue;
        if (xhw_combo_check(i, p)) continue;
        xrc_menu_feed(i, p);
        b |= p->buttons & ~XHW_BTN_BACK;
        if (p->ly > STICK_DIR) b |= XHW_BTN_UP;
        else if (p->ly < -STICK_DIR) b |= XHW_BTN_DOWN;
        if (p->lx > STICK_DIR) b |= XHW_BTN_RIGHT;
        else if (p->lx < -STICK_DIR) b |= XHW_BTN_LEFT;
    }
    xhw_pad_unlock();
    xhw_combo_act();
    *buttons = b;
    return 1;
}

/* the menu's thread: opens the menu between two frames (it takes the
 * renderer, so a graphics task in flight finishes first and the next one
 * waits), draws until it is told to close, then lets go */
static void menu_thread(void* arg) {
    (void)arg;
    for (;;) {
        unsigned frames = 0;
        if (!s_want) {
            xhw_sleep_ms(8);
            continue;
        }
        s_want = 0;
        xrc_gfx_lock();
        open_menu();
        while (!s_close && !s_leave) {
            uint64_t t0 = xhw_time_us();
            if (s_apply) s_apply = 0, apply_now();
            if (HAS_PAKS) {
                pak_do();
                if (s_page) pak_refresh();
            }
            draw_now();
            if (XHW_TEST_BUILD && ++frames <= 3)
                xhw_logf("[MENU] frame %u: %u ms", frames, (unsigned)((xhw_time_us() - t0) / 1000u));
        }
        if (s_apply) s_apply = 0, apply_now();
        if (HAS_PAKS) pak_do();
        if (s_leave) leave(s_leave == 2);
        close_menu();
        s_close = 0;
        xrc_gfx_unlock();
    }
}

/* ---- the Paks page: the VI thread's presses ---- */
static void pak_change(int dir) {
    XrcSettings* g = &g_xrc_settings;
    int port = s_pport, q;
    char v[40];
    if (s_psel == PK_PORT) {
        s_pport = step(s_pport, dir, 1, 0, 3, 1);
        s_scroll = 0;
        xhw_logf("[MENU] Paks page, port %d", s_pport + 1);
        return;
    }
    if (s_psel == PK_IN) {
        g->pak[port] = step(g->pak[port] & 3, dir, 1, 0, 3, 1);
        pak_in_text(port, v, sizeof v, 0);
    } else {   /* PK_FILE: the port that holds the new pak takes this one's */
        int l = (g->pak_file[port] + dir) & 3;
        for (q = 0; q < XRC_PORTS && g->pak_file[q] != l; q++) {}
        if (q < XRC_PORTS) g->pak_file[q] = g->pak_file[port];
        g->pak_file[port] = l;
        snprintf(v, sizeof v, "Pak %c", 'A' + l);
    }
    s_pak_apply = 1;
    xhw_logf("[MENU] port %d %s: %s", port + 1, s_psel == PK_IN ? "holds" : "pak", v);
}

static void paks_input(uint32_t press) {
    int items = pk_items(), note;
    if (s_psel >= items) s_psel = items - 1;
    note = pk_note_at(s_psel);
    if (s_confirm) {   /* A does it; Left / Right pick a copy's pak; anything else keeps the note */
        int own = g_xrc_settings.pak_file[s_pport] & 3;
        uint32_t lr = XHW_BTN_LEFT | XHW_BTN_RIGHT;
        if (s_confirm == CONFIRM_COPY && (press & lr) && !(press & ~lr)) {
            int d = press & XHW_BTN_LEFT ? 3 : 1;
            s_copy_to = (s_copy_to + d) & 3;
            if (s_copy_to == own) s_copy_to = (s_copy_to + d) & 3;
            return;
        }
        if (press & XHW_BTN_A)
            s_pak_op = s_confirm == CONFIRM_FORMAT ? -1
                       : s_confirm == CONFIRM_COPY ? OP_COPY + s_confirm_slot
                                                   : 1 + s_confirm_slot;
        if (press) s_confirm = 0;
        return;
    }
    if (press & XHW_BTN_B) {
        s_page = 0, s_sel = ROW_PAK;
    } else if (press & XHW_BTN_UP) {
        s_psel = (s_psel + items - 1) % items;
    } else if (press & XHW_BTN_DOWN) {
        s_psel = (s_psel + 1) % items;
    } else if (press & (XHW_BTN_LEFT | XHW_BTN_RIGHT)) {
        if (s_psel <= PK_FILE) pak_change(press & XHW_BTN_LEFT ? -1 : +1);
    } else if ((press & XHW_BTN_X) && note >= 0) {
        s_confirm_slot = s_notes[note].slot;
        snprintf(s_confirm_name, sizeof s_confirm_name, "%s", s_notes[note].name[0] ? s_notes[note].name : "the note");
        s_copy_to = ((g_xrc_settings.pak_file[s_pport] & 3) + 1) & 3;
        s_confirm = CONFIRM_COPY;
    } else if (press & XHW_BTN_A) {
        if (s_psel <= PK_FILE) {
            pak_change(+1);
        } else if (note >= 0) {
            s_confirm_slot = s_notes[note].slot;
            snprintf(s_confirm_name, sizeof s_confirm_name, "%s", s_notes[note].name[0] ? s_notes[note].name : "the note");
            s_confirm = CONFIRM_DELETE;
        } else if (s_psel == pk_format()) {
            s_confirm = CONFIRM_FORMAT;
        } else if (s_psel == pk_back()) {
            s_page = 0, s_sel = ROW_PAK;
        }
    }
}

/* ---- the Controls page: the VI thread's presses ---- */
static void ctl_change(int dir) {
    unsigned char* c = g_xrc_settings.controls[s_cport];
    int in, v, lo, hi;
    if (s_csel == 0) {
        s_cport = step(s_cport, dir, 1, 0, 3, 1);
        xhw_logf("[MENU] Controls page, port %d", s_cport + 1);
        return;
    }
    in = k_ctl_order[s_csel - 1];
    /* the values that input can give, in a ring with nothing in it */
    if (xrc_controls_is_dir(in)) lo = XRC_N64_STICK, hi = XRC_N64_DPAD;
    else lo = XRC_N64_A, hi = XRC_N64_DR;
    v = c[in] == XRC_N64_NONE ? (dir > 0 ? lo : hi) : c[in] + dir;
    if (v > hi || v < lo) v = XRC_N64_NONE;
    c[in] = (unsigned char)v;
    xhw_logf("[MENU] port %d %s: %s", s_cport + 1, xrc_controls_input_name(in), xrc_controls_name(v));
}

static void controls_input(uint32_t press) {
    if (press & XHW_BTN_B) {
        s_page = 0, s_sel = ROW_CONTROLS;
    } else if (press & XHW_BTN_UP) {
        s_csel = (s_csel + CTL_ITEMS - 1) % CTL_ITEMS;
    } else if (press & XHW_BTN_DOWN) {
        s_csel = (s_csel + 1) % CTL_ITEMS;
    } else if (press & (XHW_BTN_LEFT | XHW_BTN_RIGHT)) {
        ctl_change(press & XHW_BTN_LEFT ? -1 : +1);
    } else if (press & XHW_BTN_A) {
        ctl_change(+1);
    } else if (press & XHW_BTN_Y) {
        int in;
        char m[64];
        for (in = 0; in < XHW_IN_COUNT; in++) g_xrc_settings.controls[s_cport][in] = xrc_controls_default(in);
        snprintf(m, sizeof m, "Port %d: the default controls", s_cport + 1);
        pak_msg(m);
    }
}

/* the VI thread, once a retrace: the controllers while the menu is open */
void xrc_menu_vi(unsigned retrace) {
    static int s_started;
    uint32_t cur = 0, press;
    uint64_t now;
    if (!XRC_MENU || !xrc_gfx_ready()) return;
    if (!s_started) s_started = 1, xu_spawn(menu_thread, NULL, 13, 64 * 1024);
    if (!s_open || s_close || s_leave) {
        if (!s_open && s_release > 0 && !s_want) s_release--;
        return;
    }
    if (!poll(&cur)) return;
    now = xhw_time_us();
    if (XHW_TEST_BUILD) {   /* retraces seen in the menu's first two seconds */
        static uint64_t s_t0;
        static unsigned s_r0, s_said;
        if (s_said != s_open_us) {
            if (!s_t0 || s_t0 < s_open_us) s_t0 = now, s_r0 = retrace;
            else if (now - s_t0 >= 2000000u) {
                xhw_logf("[MENU] %u retraces in %u ms", retrace - s_r0, (unsigned)((now - s_t0) / 1000u));
                s_said = (unsigned)s_open_us;
            }
        }
    }
    press = cur & ~s_prev;
    if ((cur & DIRS) && (cur & DIRS) == (s_prev & DIRS)) {
        if (now >= s_repeat_at) {
            press |= cur & DIRS;
            s_repeat_at = now + REPEAT_EVERY_MS * 1000u;
        }
    } else {
        s_repeat_at = now + REPEAT_DELAY_MS * 1000u;
    }
    s_prev = cur;
    if (XHW_TEST_BUILD && press) xhw_logf("[MENU] press %04x at retrace %u", (unsigned)press, retrace);
    if (s_page == 2 && !s_want) {   /* the Controls page (B goes back to the settings) */
        controls_input(press);
        return;
    }
    if (s_page && !s_want) {   /* the Paks page (B goes back to the settings) */
        paks_input(press);
        return;
    }
    if (s_want || (press & XHW_BTN_B)) {   /* a BACK tap or B: close */
        s_want = 0;
        s_close = 1;
        return;
    }
    if (press & XHW_BTN_UP) {
        s_sel = (s_sel + N_ROWS - 1) % N_ROWS;
        if (s_sel == ROW_PAK && !HAS_PAKS) s_sel--;
    } else if (press & XHW_BTN_DOWN) {
        s_sel = (s_sel + 1) % N_ROWS;
        if (s_sel == ROW_PAK && !HAS_PAKS) s_sel++;
    }
    else if (press & XHW_BTN_LEFT) change(-1, 0);
    else if (press & XHW_BTN_RIGHT) change(+1, 0);
    else if (press & XHW_BTN_A) {
        if (s_sel == ROW_RETURN) s_close = 1;
        else if (s_sel == ROW_RESTART) s_leave = 2;
        else if (s_sel == ROW_EXIT) s_leave = 1;
        else if (s_sel == ROW_CONTROLS) {
            s_cport = s_port, s_csel = 1, s_msg[0] = 0;
            s_page = 2;
            xhw_logf("[MENU] Controls page, port %d", s_cport + 1);
        } else if (s_sel == ROW_PAK) {
            s_pport = s_port, s_psel = PK_PORT, s_scroll = 0, s_confirm = 0, s_msg[0] = 0;
            s_page = 1;
            xhw_logf("[MENU] Paks page, port %d", s_pport + 1);
        } else change(+1, 1);
    }
}
