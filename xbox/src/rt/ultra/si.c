/* si.c - controllers and EEPROM (PLAN.md 3.3, 3.6, 3.9).
 *
 * Controllers: four ports, player N is physical port N. Every read the game
 * makes (osContStartReadData) polls under the pad lock and feeds the raw
 * state to the exit combos first (xhw_combo_check: L + R + BACK + BLACK /
 * WHITE, before any mapping); a pad that shows a combo reads neutral. The
 * default layout, by position (PLAN.md 3.9):
 *
 *   N64          Xbox
 *   stick        left stick: radial dead zone ([portN] stick_deadzone),
 *                the rest rescaled so full tilt reaches +-80 (the N64's rim)
 *   A / B        A / X
 *   C buttons    Y up, B right, WHITE left, BLACK down; the right stick past half way
 *   Z / R        left / right trigger
 *   Start        Start
 *   D-pad        D-pad
 *   L            none (remappable from M6)
 *
 * EEPROM: the pack's 4 Kbit or 16 Kbit image as E:\UDATA\<title id>\<id>.eep,
 * in the byte order Project64 and Mupen64Plus write (the cartridge's), so
 * saves move both ways. Writes mark it dirty; a writer thread flushes
 * through xhw_write_file_atomic within a second, and the exit combos flush
 * it before leaving (xhw_combo_set_flush). */
#include <stdio.h>

#include "settings.h"
#include "ultra.h"

void xrc_menu_feed(int port, const xhw_pad* p);
int xrc_menu_blocks(uint32_t raw_buttons);
int xrc_video_pad(int port, const xhw_pad* p);

enum {
    N_A = 0x8000, N_B = 0x4000, N_Z = 0x2000, N_START = 0x1000, N_DU = 0x0800, N_DD = 0x0400, N_DL = 0x0200,
    N_DR = 0x0100, N_L = 0x0020, N_R = 0x0010, N_CU = 0x0008, N_CD = 0x0004, N_CL = 0x0002, N_CR = 0x0001,
};
#define CONT_TYPE_NORMAL 0x0005
#define CONT_NO_RESPONSE_ERROR 0x8
#define TRIG_ON 128
#define CSTICK_ON 16384

typedef struct { uint16_t button; int8_t x, y; uint8_t err; } Pad;
static Pad s_pads[4];

/* the radial dead zone, then the rest onto 0..80, rounded by hand (nxdk's
 * lrintf is a stub that asserts) */
static void stick(int16_t sx, int16_t sy, int dz_pct, int8_t* ox, int8_t* oy) {
    float x = sx / 32768.0f, y = sy / 32768.0f, m2 = x * x + y * y, dz = dz_pct / 100.0f, m, k;
    if (m2 <= dz * dz) {
        *ox = *oy = 0;
        return;
    }
    m = __builtin_sqrtf(m2);
    k = ((m > 1.0f ? 1.0f : m) - dz) / (1.0f - dz) * 80.0f / m;
    x *= k;
    y *= k;
    *ox = (int8_t)(x >= 0 ? (int)(x + 0.5f) : -(int)(-x + 0.5f));
    *oy = (int8_t)(y >= 0 ? (int)(y + 0.5f) : -(int)(-y + 0.5f));
}

static void map(int port, const xhw_pad* p, Pad* out) {
    uint16_t b = 0;
    memset(out, 0, sizeof *out);
    if (!p->connected) {
        out->err = CONT_NO_RESPONSE_ERROR;
        return;
    }
    if (p->buttons & XHW_BTN_A) b |= N_A;
    if (p->buttons & XHW_BTN_X) b |= N_B;
    if ((p->buttons & XHW_BTN_Y) || p->ry > CSTICK_ON) b |= N_CU;
    if ((p->buttons & XHW_BTN_B) || p->rx > CSTICK_ON) b |= N_CR;
    if ((p->buttons & XHW_BTN_WHITE) || p->rx < -CSTICK_ON) b |= N_CL;
    if ((p->buttons & XHW_BTN_BLACK) || p->ry < -CSTICK_ON) b |= N_CD;
    if (p->lt > TRIG_ON) b |= N_Z;
    if (p->rt > TRIG_ON) b |= N_R;
    if (p->buttons & XHW_BTN_START) b |= N_START;
    if (p->buttons & XHW_BTN_UP) b |= N_DU;
    if (p->buttons & XHW_BTN_DOWN) b |= N_DD;
    if (p->buttons & XHW_BTN_LEFT) b |= N_DL;
    if (p->buttons & XHW_BTN_RIGHT) b |= N_DR;
    out->button = b;
    stick(p->lx, p->ly, g_xrc_settings.stick_deadzone[port], &out->x, &out->y);
}

static void read_pads(void) {
    int i;
    uint32_t raw = 0;
    xhw_pad_lock();
    xhw_pad_poll();
    for (i = 0; i < 4; i++) {
        xhw_pad p;
        xhw_pad_get(i, &p);
        if (xhw_combo_check(i, &p)) {   /* the raw state, before any mapping (PLAN.md 3.9) */
            memset(&s_pads[i], 0, sizeof s_pads[i]);
            continue;
        }
        xrc_menu_feed(i, &p);           /* BACK taps open the menu */
        if (p.connected) raw |= p.buttons;
        map(i, &p, &s_pads[i]);
        if (xrc_video_pad(i, &p)) s_pads[i].button &= (uint16_t)~N_A;   /* the A that kept a video mode */
    }
    if (xrc_menu_blocks(raw))   /* the menu is open, or the press that closed it is still down */
        for (i = 0; i < 4; i++) s_pads[i].button = 0, s_pads[i].x = s_pads[i].y = 0;
    xhw_pad_unlock();
    xhw_combo_note_read(xu_retrace());
    xhw_combo_act();
}

static void write_status(uint32_t status) {
    int i;
    for (i = 0; i < 4; i++) {
        wr16(status + 4 * (uint32_t)i, CONT_TYPE_NORMAL);
        wr8(status + 4 * (uint32_t)i + 2, 0);
        wr8(status + 4 * (uint32_t)i + 3, i == 0 ? 0 : s_pads[i].err);   /* port 1 always answers */
    }
}

/* osContInit(mq, u8* bitpattern, OSContStatus* status) */
ULTRA(osContInit) {
    uint32_t bits = ARG(1), status = ARG(2);
    uint8_t pattern = 0;
    int i;
    (void)rdram;
    read_pads();
    for (i = 0; i < 4; i++)
        if (i == 0 || !s_pads[i].err) pattern |= (uint8_t)(1 << i);
    if (bits) wr8(bits, pattern);
    if (status) write_status(status);
    xhw_logf("[SI] controllers: pattern %x", pattern);
    RET(0);
}

ULTRA(osContStartReadData) {
    (void)rdram;
    read_pads();
    xu_post_event(OS_EVENT_SI);
    RET(0);
}

/* osContGetReadData(OSContPad pads[4]): 6 bytes each */
ULTRA(osContGetReadData) {
    uint32_t out = ARG(0);
    int i;
    (void)rdram;
    for (i = 0; i < 4; i++) {
        uint32_t a = out + 6 * (uint32_t)i;
        wr8(a, (uint8_t)(s_pads[i].button >> 8));
        wr8(a + 1, (uint8_t)s_pads[i].button);
        wr8(a + 2, (uint8_t)s_pads[i].x);
        wr8(a + 3, (uint8_t)s_pads[i].y);
        wr8(a + 4, i == 0 ? 0 : s_pads[i].err);
        wr8(a + 5, 0);
    }
}

ULTRA(osContStartQuery) {
    (void)rdram;
    xu_post_event(OS_EVENT_SI);
    RET(0);
}

ULTRA(osContGetQuery) {
    (void)rdram;
    write_status(ARG(0));
}

ULTRA(osContSetCh) {
    (void)rdram;
    RET(0);
}

/* ---- EEPROM ---- */
#define EEP_MAX 2048
static uint8_t s_eep[EEP_MAX];
static uint32_t s_eep_size;   /* 512 (4 Kbit) or 2048 (16 Kbit) */
static volatile int s_eep_dirty, s_eep_loaded;
static xhw_mutex* s_eep_lock;
static char s_eep_path[96];

static void eep_load(void) {
    void* f;
    if (s_eep_loaded) return;
    s_eep_loaded = 1;
    s_eep_lock = xhw_mutex_create();
    s_eep_size = xrc_game.eeprom_bytes ? xrc_game.eeprom_bytes : 512;
    memset(s_eep, 0xFF, sizeof s_eep);   /* a blank EEPROM reads all ones */
    snprintf(s_eep_path, sizeof s_eep_path, "%s%s.eep", xhw_save_dir(), xrc_game.id);
    f = xhw_file_open(s_eep_path);
    if (f) {
        uint32_t n = xhw_file_size(f);
        if (n > s_eep_size) n = s_eep_size;
        if (!xhw_file_read(f, 0, s_eep, n)) xhw_logf("[SAVE] %s: read failed", s_eep_path);
        xhw_file_close(f);
        xhw_logf("[SAVE] %s: %u bytes", s_eep_path, (unsigned)n);
    } else {
        xhw_logf("[SAVE] %s: none yet (blank EEPROM)", s_eep_path);
    }
}

void xu_save_flush(void) {
    uint8_t copy[EEP_MAX];
    if (!s_eep_dirty) return;
    xhw_mutex_lock(s_eep_lock);
    memcpy(copy, s_eep, s_eep_size);
    s_eep_dirty = 0;
    xhw_mutex_unlock(s_eep_lock);
    if (xhw_write_file_atomic(s_eep_path, copy, s_eep_size)) xhw_logf("[SAVE] %s written", s_eep_path);
    else xhw_logf("[SAVE] %s: write FAILED", s_eep_path);
}

static void writer(void* arg) {
    (void)arg;
    for (;;) {
        xhw_sleep_ms(1000);
        xu_save_flush();
    }
}

static void eep_start_writer(void) {
    static int up;
    if (up) return;
    up = 1;
    xhw_combo_set_flush(xu_save_flush);
    xu_spawn(writer, NULL, 6, 32 * 1024);
}

/* osEepromProbe(mq): EEPROM_TYPE_4K 1, _16K 2, none 0 */
ULTRA(osEepromProbe) {
    (void)rdram;
    if (!xrc_game.eeprom_bytes) {
        RET(0);
        return;
    }
    eep_load();
    RET(s_eep_size == 2048 ? 2 : 1);
}

static int eep_io(uint32_t block, uint32_t buf, uint32_t n, int write) {
    uint32_t off = block * 8u;
    eep_load();
    if (off + n > s_eep_size) return -1;
    xhw_mutex_lock(s_eep_lock);
    if (write) {
        xu_from_rdram(s_eep + off, buf, n);
        s_eep_dirty = 1;
    } else {
        xu_to_rdram(buf, s_eep + off, n);
    }
    xhw_mutex_unlock(s_eep_lock);
    if (write) eep_start_writer();
    return 0;
}

/* osEepromLongRead(mq, u8 address, u8* buffer, int nbytes) */
ULTRA(osEepromLongRead) {
    (void)rdram;
    RET(eep_io(ARG(1) & 0xFF, ARG(2), ARG(3), 0));
}
ULTRA(osEepromLongWrite) {
    (void)rdram;
    RET(eep_io(ARG(1) & 0xFF, ARG(2), ARG(3), 1));
}
ULTRA(osEepromRead) {
    (void)rdram;
    RET(eep_io(ARG(1) & 0xFF, ARG(2), 8, 0));
}
ULTRA(osEepromWrite) {
    (void)rdram;
    RET(eep_io(ARG(1) & 0xFF, ARG(2), 8, 1));
}
