/* save.c - the save images and their files (PLAN.md 3.6, 3.9).
 *
 * Every save a game can have lives in E:\UDATA\<title id>\, named by the
 * pack's id, in the byte order the cartridge or pak holds it (as
 * mupen64plus and ares write it), so saves move both ways:
 *   <id>.eep            EEPROM, 512 or 2048 bytes (si.c)
 *   <id>.mpk            Controller Pak A, 32 KB (port 1's unless the BACK
 *                       menu moved it); <id>-2.mpk .. <id>-4.mpk Paks B to D
 *                       (ports 2 to 4's)
 *   <id>.sra / <id>.fla SRAM (32 KB) or FlashRAM (128 KB) (savedev.c)
 * An image is read on its first use; one with no file yet starts blank
 * (EEPROM, SRAM and Flash all ones, a pak formatted empty) and gets a file
 * only when the game first writes it. A write marks it dirty; the writer
 * thread writes dirty images through xhw_write_file_atomic within a second,
 * and the exit combos flush before leaving (xhw_combo_set_flush). Each is
 * copied under the lock and the copy written (write_one), so a game's save
 * access never waits for the disk.
 *
 * Memory: 2 KB of EEPROM, 4 x 32 KB of paks, 128 KB for SRAM or Flash and
 * the 128 KB copy, static (386 KB of the 64 MB budget). */
#include <stdio.h>

#include "savedev.h"
#include "settings.h"
#include "ultra.h"

typedef struct {
    uint8_t* data;
    uint32_t size;
    uint8_t loaded;
    volatile uint8_t dirty;
    char path[96];
    uint32_t backoff_ms;   /* after a failed write: the next try waits this long */
    uint64_t retry_at;
} Img;

static uint8_t s_eep_buf[2048], s_pak_buf[4][XU_PAK_SIZE], s_cart_buf[XU_FLASH_SIZE];
static Img s_eep = {s_eep_buf}, s_pak[4] = {{s_pak_buf[0]}, {s_pak_buf[1]}, {s_pak_buf[2]}, {s_pak_buf[3]}},
           s_cart = {s_cart_buf};
static xhw_mutex* s_lock;

void xu_save_lock(void) {
    if (!s_lock) s_lock = xhw_mutex_create();
    xhw_mutex_lock(s_lock);
}

void xu_save_unlock(void) { xhw_mutex_unlock(s_lock); }

/* the image from its file; 1 if there was one */
static int load(Img* m, const char* ext, uint32_t size, uint8_t fill) {
    void* f;
    m->loaded = 1;
    m->size = size;
    memset(m->data, fill, size);
    snprintf(m->path, sizeof m->path, "%s%s%s", xhw_save_dir(), xrc_game.id ? xrc_game.id : "game", ext);
    f = xhw_file_open(m->path);
    if (!f) {
        xhw_logf("[SAVE] %s: none yet (blank)", m->path);
        return 0;
    }
    {
        uint32_t n = xhw_file_size(f);
        if (n > size) n = size;
        if (!xhw_file_read(f, 0, m->data, n)) xhw_logf("[SAVE] %s: read failed", m->path);
        xhw_logf("[SAVE] %s: %u bytes", m->path, (unsigned)n);
    }
    xhw_file_close(f);
    return 1;
}

static int exists(const char* ext) {
    char path[96];
    void* f;
    snprintf(path, sizeof path, "%s%s%s", xhw_save_dir(), xrc_game.id ? xrc_game.id : "game", ext);
    f = xhw_file_open(path);
    if (f) xhw_file_close(f);
    return f != NULL;
}

/* An image is copied under the save lock and the copy written outside it:
 * the lock was held across the write, the read-back, the rename and the
 * volume flush, so every save-device access (a pak poll, an EEPROM read)
 * waited behind the disk (docs/review-2026-10.md 5b.9). A failed write
 * marks the image dirty again and is tried again after 1, 2, 4 ... 30 s (the
 * exit combos' flush at once): a full or refused write was logged and the
 * save lost, the exit's flush seeing nothing dirty (5d.3). The copy is one
 * static buffer for the largest image (128 KB), used under s_flush.
 * -DXRC_SAVE_COPY=0: written under the lock, never retried, as before */
#ifndef XRC_SAVE_COPY
#define XRC_SAVE_COPY 1
#endif
#if XRC_SAVE_COPY
static uint8_t s_copy[XU_FLASH_SIZE];
static xhw_mutex* s_flush;
#endif

/* under the save lock, at once (a pak swapped, the cartridge's kind found):
 * the image is about to be replaced, so it is written from where it is */
static void write_now(Img* m) {
    if (!m->dirty) return;
    m->dirty = 0;
    if (xhw_write_file_atomic(m->path, m->data, m->size)) xhw_logf("[SAVE] %s written", m->path);
    else xhw_logf("[SAVE] %s: write FAILED", m->path);
}

static void write_one(Img* m, int force) {
#if XRC_SAVE_COPY
    uint32_t n;
    char path[96];
    xu_save_lock();
    if (!m->dirty || (!force && m->retry_at && xhw_time_us() < m->retry_at)) {
        xu_save_unlock();
        return;
    }
    m->dirty = 0;
    n = m->size <= sizeof s_copy ? m->size : (uint32_t)sizeof s_copy;
    memcpy(s_copy, m->data, n);
    memcpy(path, m->path, sizeof path);
    xu_save_unlock();
    if (xhw_write_file_atomic(path, s_copy, n)) {
        xhw_logf("[SAVE] %s written", path);
        m->backoff_ms = 0, m->retry_at = 0;
        return;
    }
    m->backoff_ms = m->backoff_ms ? (m->backoff_ms >= 15000u ? 30000u : m->backoff_ms * 2u) : 1000u;
    m->retry_at = xhw_time_us() + (uint64_t)m->backoff_ms * 1000u;
    m->dirty = 1;   /* written again later; a write the game made meanwhile is in the next copy */
    xhw_logf("[SAVE] %s: write FAILED, tried again in %u s", path, (unsigned)(m->backoff_ms / 1000u));
#else
    (void)force;
    write_now(m);
#endif
}

static void flush(int force) {
    int i;
    if (!s_eep.dirty && !s_pak[0].dirty && !s_pak[1].dirty && !s_pak[2].dirty && !s_pak[3].dirty && !s_cart.dirty)
        return;
#if XRC_SAVE_COPY
    if (!s_flush) s_flush = xhw_mutex_create();   /* made before the writer thread (dirty()) */
    xhw_mutex_lock(s_flush);
#else
    xu_save_lock();
#endif
    write_one(&s_eep, force);
    for (i = 0; i < 4; i++) write_one(&s_pak[i], force);
    write_one(&s_cart, force);
#if XRC_SAVE_COPY
    xhw_mutex_unlock(s_flush);
#else
    xu_save_unlock();
#endif
}

/* the exit combos' flush (xhw_combo_set_flush): everything dirty, now */
void xu_save_flush(void) { flush(1); }

static void writer(void* arg) {
    (void)arg;
    for (;;) {
        xhw_sleep_ms(1000);
        flush(0);
    }
}

static void dirty(Img* m) {
    static int up;
    m->dirty = 1;
    if (up) return;
    up = 1;
#if XRC_SAVE_COPY
    if (!s_flush) s_flush = xhw_mutex_create();
#endif
    xhw_combo_set_flush(xu_save_flush);
    xu_spawn(writer, NULL, 6, 32 * 1024);
}

/* ---- EEPROM (si.c) ---- */
uint8_t* xu_eep_image(uint32_t* size) {
    if (!s_eep.loaded) load(&s_eep, ".eep", xrc_game.eeprom_bytes == 2048 ? 2048 : 512, 0xFF);
    *size = s_eep.size;
    return s_eep.data;
}

void xu_eep_dirty(void) { dirty(&s_eep); }

/* ---- the paks (savedev.c) ----
 * Four paks per game, A to D, and each is always in one port (A in port 1
 * until the BACK menu's Paks page moves it; taking a pak another port holds
 * swaps the two), so s_pak[port] is the image of pak s_letter[port]. */
static int s_letter[4] = {0, 1, 2, 3}, s_kind_set[4] = {XU_PAK_AUTO, XU_PAK_AUTO, XU_PAK_AUTO, XU_PAK_AUTO};

uint8_t* xu_pak_image(int port) {
    Img* m = &s_pak[port & 3];
    if (!m->loaded) {
        static const char* ext[4] = {".mpk", "-2.mpk", "-3.mpk", "-4.mpk"};
        int l = s_letter[port & 3];
        if (!load(m, ext[l], XU_PAK_SIZE, 0)) xu_pak_format_id(m->data, 0x4E525821u + (uint32_t)l);   /* "NRX!" + l */
    }
    return m->data;
}

int xu_pak_letter(int port) { return s_letter[port & 3]; }

void xu_pak_insert(int port, int kind, int letter, int hot) {
    int q;
    port &= 3, letter &= 3;
    xu_save_lock();
    if (s_letter[port] != letter) {
        for (q = 0; q < 4 && s_letter[q] != letter; q++) {}
        q &= 3;
        write_now(&s_pak[port]);
        write_now(&s_pak[q]);
        s_letter[q] = s_letter[port];
        s_letter[port] = letter;
        s_pak[port].loaded = s_pak[q].loaded = 0;   /* each port reads its new pak on the next access */
        if (hot) xu_pak_pull(port), xu_pak_pull(q);
        xhw_logf("[SAVE] port %d: Pak %c, port %d: Pak %c", port + 1, 'A' + letter, q + 1, 'A' + s_letter[q]);
    }
    if (s_kind_set[port] != kind) {
        s_kind_set[port] = kind;
        xu_pak_set_kind(port, kind);
        if (hot) xu_pak_pull(port);
        xhw_logf("[SAVE] port %d holds %s", port + 1,
                 kind == XU_PAK_AUTO     ? "Auto"
                 : kind == XU_PAK_MEMORY ? "a Controller Pak"
                 : kind == XU_PAK_RUMBLE ? "a Rumble Pak"
                                         : "nothing");
    }
    xu_save_unlock();
}

void xu_pak_apply_settings(int hot) {
    static const int kinds[4] = {XU_PAK_AUTO, XU_PAK_MEMORY, XU_PAK_RUMBLE, XU_PAK_NONE};
    int i;
    for (i = 0; i < 4; i++) xu_pak_insert(i, kinds[g_xrc_settings.pak[i] & 3], g_xrc_settings.pak_file[i], hot);
}

void xu_pak_dirty(int port) { dirty(&s_pak[port & 3]); }

void xu_pak_rumble(int port, int on) {
    static int s_on[4], s_said;
    /* each change, the first 64: when a game rumbles (Ocarina of Time buzzed
     * at its scene changes on red, 2026-10-18: the game's, or a hitch's?) */
    if (s_on[port & 3] != !!on && s_said < 64) {
        s_said++;
        xhw_logf("[PAK] port %d: motor %s at %u ms", port + 1, on ? "on" : "off", (unsigned)(xhw_time_us() / 1000u));
    }
    s_on[port & 3] = !!on;
    xhw_pad_rumble(port, on && g_xrc_settings.rumble ? 0xA000 : 0, on ? 1000 : 0);
}

/* ---- SRAM or Flash (savedev.c) ---- */
uint8_t* xu_cart_image(int* kind) {
    int k = *kind;
    if (k == XU_CART_UNKNOWN) k = exists(".fla") ? XU_CART_FLASH : XU_CART_SRAM;
    if (s_cart.loaded) {   /* SRAM until now, and the game spoke to the Flash: it has Flash */
        write_now(&s_cart);
        xhw_logf("[SAVE] the cartridge has %s after all", k == XU_CART_FLASH ? "FlashRAM" : "SRAM");
    }
    load(&s_cart, k == XU_CART_FLASH ? ".fla" : ".sra", k == XU_CART_FLASH ? XU_FLASH_SIZE : XU_SRAM_SIZE, 0xFF);
    xhw_logf("[SAVE] cartridge save: %s", k == XU_CART_FLASH ? "FlashRAM 1 Mbit" : "SRAM 256 Kbit");
    *kind = k;
    return s_cart.data;
}

void xu_cart_dirty(void) { dirty(&s_cart); }
