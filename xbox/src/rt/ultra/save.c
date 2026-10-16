/* save.c - the save images and their files (PLAN.md 3.6, 3.9).
 *
 * Every save a game can have lives in E:\UDATA\<title id>\, named by the
 * pack's id, in the byte order the cartridge or pak holds it (as
 * mupen64plus and ares write it), so saves move both ways:
 *   <id>.eep            EEPROM, 512 or 2048 bytes (si.c)
 *   <id>.mpk            the Controller Pak in port 1, 32 KB; <id>-2.mpk ..
 *                       <id>-4.mpk for ports 2 to 4
 *   <id>.sra / <id>.fla SRAM (32 KB) or FlashRAM (128 KB) (savedev.c)
 * An image is read on its first use; one with no file yet starts blank
 * (EEPROM, SRAM and Flash all ones, a pak formatted empty) and gets a file
 * only when the game first writes it. A write marks it dirty; the writer
 * thread writes dirty images through xhw_write_file_atomic within a second,
 * and the exit combos flush before leaving (xhw_combo_set_flush). The lock
 * is held while a file is written, so no copy is needed (a game that saves
 * again meanwhile waits for the disk, as it waited for the cartridge).
 *
 * Memory: 2 KB of EEPROM, 4 x 32 KB of paks and 128 KB for SRAM or Flash,
 * static (258 KB of the 64 MB budget). */
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

static void write_one(Img* m) {
    if (!m->dirty) return;
    m->dirty = 0;
    if (xhw_write_file_atomic(m->path, m->data, m->size)) xhw_logf("[SAVE] %s written", m->path);
    else xhw_logf("[SAVE] %s: write FAILED", m->path);
}

void xu_save_flush(void) {
    int i;
    if (!s_eep.dirty && !s_pak[0].dirty && !s_pak[1].dirty && !s_pak[2].dirty && !s_pak[3].dirty && !s_cart.dirty)
        return;
    xu_save_lock();
    write_one(&s_eep);
    for (i = 0; i < 4; i++) write_one(&s_pak[i]);
    write_one(&s_cart);
    xu_save_unlock();
}

static void writer(void* arg) {
    (void)arg;
    for (;;) {
        xhw_sleep_ms(1000);
        xu_save_flush();
    }
}

static void dirty(Img* m) {
    static int up;
    m->dirty = 1;
    if (up) return;
    up = 1;
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

/* ---- the paks (savedev.c) ---- */
uint8_t* xu_pak_image(int port) {
    Img* m = &s_pak[port & 3];
    if (!m->loaded) {
        static const char* ext[4] = {".mpk", "-2.mpk", "-3.mpk", "-4.mpk"};
        if (!load(m, ext[port & 3], XU_PAK_SIZE, 0)) xu_pak_format(m->data);
    }
    return m->data;
}

void xu_pak_dirty(int port) { dirty(&s_pak[port & 3]); }

void xu_pak_rumble(int port, int on) {
    xhw_pad_rumble(port, on && g_xrc_settings.rumble ? 0xA000 : 0, on ? 1000 : 0);
}

/* ---- SRAM or Flash (savedev.c) ---- */
uint8_t* xu_cart_image(int* kind) {
    int k = *kind;
    if (k == XU_CART_UNKNOWN) k = exists(".fla") ? XU_CART_FLASH : XU_CART_SRAM;
    if (s_cart.loaded) {   /* SRAM until now, and the game spoke to the Flash: it has Flash */
        write_one(&s_cart);
        xhw_logf("[SAVE] the cartridge has %s after all", k == XU_CART_FLASH ? "FlashRAM" : "SRAM");
    }
    load(&s_cart, k == XU_CART_FLASH ? ".fla" : ".sra", k == XU_CART_FLASH ? XU_FLASH_SIZE : XU_SRAM_SIZE, 0xFF);
    xhw_logf("[SAVE] cartridge save: %s", k == XU_CART_FLASH ? "FlashRAM 1 Mbit" : "SRAM 256 Kbit");
    *kind = k;
    return s_cart.data;
}

void xu_cart_dirty(void) { dirty(&s_cart); }
