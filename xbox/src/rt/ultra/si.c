/* si.c - controllers and EEPROM (PLAN.md 3.3, 3.6, 3.9).
 *
 * Controllers: four ports, player N is physical port N. Every read the game
 * makes (osContStartReadData) polls under the pad lock and feeds the raw
 * state to the exit combos first (xhw_combo_check: L + R + BACK + BLACK /
 * WHITE, before any mapping); a pad that shows a combo reads neutral. The
 * default layout, by position (PLAN.md 3.9; settings.c k_default, changed
 * per port on the BACK menu's Controls page):
 *
 *   N64          Xbox
 *   stick        left stick: radial dead zone ([portN] stick_deadzone),
 *                the rest rescaled so full tilt reaches +-80 (the N64's rim)
 *   A / B        A / X
 *   C buttons    Y up, B right, WHITE left, BLACK down; the right stick past half way
 *   Z / R        left / right trigger
 *   Start        Start
 *   D-pad        D-pad
 *   L            none (the Controls page gives it to any button)
 *
 * EEPROM: the pack's 4 Kbit or 16 Kbit image (save.c keeps it and its
 * file, E:\UDATA\<title id>\<id>.eep, and flushes it). The paks behind each
 * controller answer at the PIF (savedev.c); the status byte says a pak is
 * in where one is. */
#include <stdio.h>

#include "savedev.h"
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

/* the port's controls (settings.ini, the BACK menu's Controls page): each
 * Xbox input gives what its row says. A button gives one N64 button; the
 * d-pad and the sticks give a direction set: the analog stick (the d-pad
 * at full tilt, a stick through the dead zone; two on the stick: the
 * longer wins), the C buttons or the N64's d-pad (a stick past half way).
 * Kill switch -DXRC_CONTROLS=0: the default layout whatever the file says. */
#ifndef XRC_CONTROLS
#define XRC_CONTROLS 1
#endif
static const uint16_t k_nbit[XRC_N64_BUTTONS] = {
    0, N_A, N_B, N_Z, N_L, N_R, N_START, N_CU, N_CD, N_CL, N_CR, N_DU, N_DD, N_DL, N_DR,
};

static int in_down(const xhw_pad* p, int in) {
    static const uint32_t k_btn[XHW_IN_COUNT] = {
        XHW_BTN_A, XHW_BTN_B, XHW_BTN_X, XHW_BTN_Y, XHW_BTN_BLACK, XHW_BTN_WHITE, 0, 0, XHW_BTN_START,
        XHW_BTN_LSTICK, XHW_BTN_RSTICK,
    };
    if (in == XHW_IN_LT) return p->lt > TRIG_ON;
    if (in == XHW_IN_RT) return p->rt > TRIG_ON;
    return in < XHW_IN_DPAD && (p->buttons & k_btn[in]) != 0;
}

/* a direction set (up, down, left, right) onto the C buttons or the d-pad */
static uint16_t dir_bits(int t, int u, int d, int l, int r) {
    if (t == XRC_N64_CBUTTONS) return (uint16_t)((u ? N_CU : 0) | (d ? N_CD : 0) | (l ? N_CL : 0) | (r ? N_CR : 0));
    if (t == XRC_N64_DPAD) return (uint16_t)((u ? N_DU : 0) | (d ? N_DD : 0) | (l ? N_DL : 0) | (r ? N_DR : 0));
    return 0;
}

static void map(int port, const xhw_pad* p, Pad* out) {
    const unsigned char* c = g_xrc_settings.controls[port];
    unsigned char def[XHW_IN_COUNT];
    uint16_t b = 0;
    int in, best = -1, dz = g_xrc_settings.stick_deadzone[port];
    memset(out, 0, sizeof *out);
    if (!p->connected) {
        out->err = CONT_NO_RESPONSE_ERROR;
        return;
    }
    if (!XRC_CONTROLS) {
        for (in = 0; in < XHW_IN_COUNT; in++) def[in] = xrc_controls_default(in);
        c = def;
    }
    for (in = 0; in < XHW_IN_DPAD; in++)
        if (c[in] < XRC_N64_BUTTONS && in_down(p, in)) b |= k_nbit[c[in]];
    for (in = XHW_IN_DPAD; in < XHW_IN_COUNT; in++) {
        int t = c[in], u, d, l, r;
        int8_t x = 0, y = 0;
        if (in == XHW_IN_DPAD) {
            u = (p->buttons & XHW_BTN_UP) != 0, d = (p->buttons & XHW_BTN_DOWN) != 0;
            l = (p->buttons & XHW_BTN_LEFT) != 0, r = (p->buttons & XHW_BTN_RIGHT) != 0;
            x = (int8_t)(r ? 80 : l ? -80 : 0), y = (int8_t)(u ? 80 : d ? -80 : 0);
        } else {
            int16_t sx = in == XHW_IN_LSTICK ? p->lx : p->rx, sy = in == XHW_IN_LSTICK ? p->ly : p->ry;
            u = sy > CSTICK_ON, d = sy < -CSTICK_ON, l = sx < -CSTICK_ON, r = sx > CSTICK_ON;
            if (t == XRC_N64_STICK) stick(sx, sy, dz, &x, &y);
        }
        if (t == XRC_N64_STICK) {
            int m = x * x + y * y;
            if (m > best) best = m, out->x = x, out->y = y;
        } else {
            b |= dir_bits(t, u, d, l, r);
        }
    }
    out->button = b;
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

/* the status byte: CONT_CARD_ON, a pak is in; CONT_CARD_PULL once after
 * the BACK menu swapped it (savedev.c; XRC_SAVE_PAK=0: none) */
static uint8_t pak_status(int port) {
    return XRC_SAVE_PAK ? xu_pak_status(port) : 0x00;
}

static void write_status(uint32_t status) {
    int i;
    for (i = 0; i < 4; i++) {
        wr16(status + 4 * (uint32_t)i, CONT_TYPE_NORMAL);
        wr8(status + 4 * (uint32_t)i + 2, pak_status(i));
        wr8(status + 4 * (uint32_t)i + 3, i == 0 ? 0 : s_pads[i].err);   /* port 1 always answers */
    }
}

/* osContInit(mq, u8* bitpattern, OSContStatus* status) */
ULTRA(osContInit) {
    uint32_t bits = ARG(1), status = ARG(2);
    uint8_t pattern = 0;
    int i;
    (void)rdram;
#ifdef XRC_CONT_MAX_ADDR
    /* libultra's __osMaxControllers, which the real osContInit sets: the
     * game's own pak code (recompiled) asks the PIF about that many ports
     * (n64rx finds it in the game's osContInit: tools/n64rx/n64rx_lib/saves.py) */
    if (XRC_CONT_MAX_SIZE == 1) wr8(XRC_CONT_MAX_ADDR, 4);
    else if (XRC_CONT_MAX_SIZE == 2) wr16(XRC_CONT_MAX_ADDR, 4);
    else wr32(XRC_CONT_MAX_ADDR, 4);
#endif
    read_pads();
    for (i = 0; i < 4; i++)
        if (i == 0 || !s_pads[i].err) pattern |= (uint8_t)(1 << i);
    if (bits) wr8(bits, pattern);
    if (status) write_status(status);
    xhw_logf("[SI] controllers: pattern %x", pattern);
    RET(0);
}

/* The SI takes time: reading four controllers is about 2 ms on the N64, a thread waits for its done event, and threads
 * below it run meanwhile. Posted at once, a pad thread that reads again as soon as it can never slept (Body Harvest's,
 * one priority above its main thread, which then never ran with the run token, and shared the CPU with it otherwise).
 * -DXRC_SI_US=0: at once, as before */
#ifndef XRC_SI_US
#define XRC_SI_US 2000
#endif
void xu_si_later(uint32_t us);   /* time.c */
ULTRA(osContStartReadData) {
    (void)rdram;
    read_pads();
    if (XRC_SI_US) xu_si_later(XRC_SI_US);
    else xu_post_event(OS_EVENT_SI);
    RET(0);
}

/* osContGetReadData(OSContPad pads[4]): 6 bytes each */
ULTRA(osContGetReadData) {
    uint32_t out = ARG(0);
    int i;
    (void)rdram;
    if (XHW_TEST_BUILD && (s_pads[0].x || s_pads[0].y)) {   /* where a tilted stick goes (Pokemon Snap's camera) */
        static unsigned said;
        if (said++ < 4)
            xhw_logf("[SI] osContGetReadData: stick %d %d buttons %04x into %08x", s_pads[0].x, s_pads[0].y,
                     s_pads[0].button, (unsigned)out);
    }
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
    if (XRC_SI_US) xu_si_later(XRC_SI_US);
    else xu_post_event(OS_EVENT_SI);
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

/* ---- EEPROM (the image and its file: save.c) ---- */
static uint8_t* s_eep;
static uint32_t s_eep_size;   /* 512 (4 Kbit) or 2048 (16 Kbit) */

static void eep_load(void) {
    if (!s_eep) s_eep = xu_eep_image(&s_eep_size);
}

/* osEepromProbe(mq): EEPROM_TYPE_4K 1, _16K 2, none 0 */
ULTRA(osEepromProbe) {
    (void)rdram;
    if (!xrc_game.eeprom_bytes) {
        RET(0);
        return;
    }
    xu_save_lock();
    eep_load();
    xu_save_unlock();
    RET(s_eep_size == 2048 ? 2 : 1);
}

static int eep_io(uint32_t block, uint32_t buf, uint32_t n, int write) {
    uint32_t off = block * 8u;
    xu_save_lock();
    eep_load();
    if (n > s_eep_size || off > s_eep_size - n) {   /* off + n wrapped for a huge nbytes: 4 GB copied (5b.8) */
        xu_save_unlock();
        return -1;
    }
    if (write) {
        xu_from_rdram(s_eep + off, buf, n);
        xu_eep_dirty();
    } else {
        xu_to_rdram(buf, s_eep + off, n);
    }
    xu_save_unlock();
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

/* ---- the PIF, for the game's own SI code (PLAN.md 3.14) ----
 * A game built from the ROM alone may run libultra's controller code that
 * the rules did not name (the Controller Pak's, a game's own pad reader):
 * it builds the 64-byte PIF command block itself and moves it with
 * __osSiRawStartDma. The PIF is emulated on that block: per channel a
 * transmit and a receive length, then the command; controllers 0-3 answer
 * status (0x00/0xFF: a standard pad, and whether a pak is in), buttons
 * (0x01) and the pak's read and write (0x02/0x03, savedev.c); channel 4 is
 * the EEPROM (status, read and write a block of 8). Absent ports and
 * anything else answer "no response" (0x80 in the receive length byte), as
 * an empty port does. Every transfer raises the SI event. */
static uint8_t s_pif[64];

static void pif_channel(int ch, const uint8_t* cmd, int tx, uint8_t* resp, int rx, uint8_t* rxbyte) {
    if (tx < 1) return;
    if (ch < 4) {
        if (ch > 0 && s_pads[ch].err) {
            *rxbyte |= 0x80;
            return;
        }
        if ((cmd[0] == 0x00 || cmd[0] == 0xFF) && rx >= 3) {
            resp[0] = 0x05, resp[1] = 0x00, resp[2] = pak_status(ch);   /* CONT_TYPE_NORMAL */
        } else if (cmd[0] == 0x01 && rx >= 4) {
            resp[0] = (uint8_t)(s_pads[ch].button >> 8), resp[1] = (uint8_t)s_pads[ch].button;
            resp[2] = (uint8_t)s_pads[ch].x, resp[3] = (uint8_t)s_pads[ch].y;
            if (XHW_TEST_BUILD && (resp[2] || resp[3])) {
                static unsigned said;
                if (said++ < 4) xhw_logf("[SI] PIF read: port %d stick %d %d", ch + 1, s_pads[ch].x, s_pads[ch].y);
            }
        } else if ((cmd[0] == 0x02 || cmd[0] == 0x03) && XRC_SAVE_PAK) {
            static uint8_t seen[4];
            int r;
            if (!seen[ch]) {
                seen[ch] = 1;
                xhw_logf("[SI] port %d: the game's own code reaches its pak (PIF command %02x)", ch + 1, cmd[0]);
            }
            xu_save_lock();
            r = xu_pak_pif(ch, cmd, tx, resp, rx);
            xu_save_unlock();
            if (r) *rxbyte |= 0x80;
        } else {
            *rxbyte |= 0x80;
        }
        return;
    }
    if (ch == 4 && xrc_game.eeprom_bytes) {
        xu_save_lock();
        eep_load();
        if (cmd[0] == 0x00 && rx >= 3) {
            resp[0] = 0x00, resp[1] = s_eep_size == 2048 ? 0xC0 : 0x80, resp[2] = 0x00;
        } else if (cmd[0] == 0x04 && tx >= 2 && rx >= 8 && cmd[1] * 8u + 8 <= s_eep_size) {
            memcpy(resp, s_eep + cmd[1] * 8u, 8);
        } else if (cmd[0] == 0x05 && tx >= 10 && cmd[1] * 8u + 8 <= s_eep_size) {
            memcpy(s_eep + cmd[1] * 8u, cmd + 2, 8);
            xu_eep_dirty();
            if (rx >= 1) resp[0] = 0x00;
        } else {
            *rxbyte |= 0x80;
        }
        xu_save_unlock();
        return;
    }
    *rxbyte |= 0x80;
}

static void pif_run(void) {
    int i = 0, ch = 0, buttons = 0;
    for (i = 0; i < 63; i++)
        if (s_pif[i] == 0xFE) break;
        else if (s_pif[i] == 0x01 && i > 0) buttons = 1;
    if (buttons) read_pads();
    i = 0;
    while (i < 63) {
        int t = s_pif[i], tx, rx;
        if (t == 0xFE) break;
        if (t == 0xFF || t == 0xFD) {   /* padding */
            i++;
            continue;
        }
        if (t == 0) {                   /* this channel skipped */
            ch++, i++;
            continue;
        }
        if (i + 1 >= 63) break;
        tx = t & 0x3F, rx = s_pif[i + 1] & 0x3F;
        if (i + 2 + tx + rx > 63) break;
        pif_channel(ch, s_pif + i + 2, tx, s_pif + i + 2 + tx, rx, s_pif + i + 1);
        i += 2 + tx + rx, ch++;
    }
}

/* __osSiRawStartDma(direction, dramAddr): OS_WRITE (1) gives the PIF its
 * block and runs it, OS_READ (0) reads the answers back */
ULTRA(__osSiRawStartDma) {
    uint32_t dir = ARG(0), dram = ARG(1);
    (void)rdram;
    if (dir == 1) {
        xu_from_rdram(s_pif, dram, 64);
        pif_run();
    } else {
        xu_to_rdram(dram, s_pif, 64);
    }
    /* (posted at once, it reached a thread already waiting on the queue before the caller got there: Wipeout 64's pad thread took
     * the done of main's rumble pak probe, and main waited for ever, in some runs) */
    if (XRC_SI_US) xu_si_later(XRC_SI_US);
    else xu_post_event(OS_EVENT_SI);
    RET(0);
}

ULTRA(__osSiGetAccess) { (void)rdram; (void)ctx; }
ULTRA(__osSiRelAccess) { (void)rdram; (void)ctx; }
ULTRA(__osSiCreateAccessQueue) { (void)rdram; (void)ctx; }
ULTRA(__osSiDeviceBusy) { (void)rdram; RET(0); }
