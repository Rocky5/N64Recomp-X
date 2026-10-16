/* extra.c - the rest of the recompiler's reimplemented list (PLAN.md 3.3),
 * so a game built from the ROM alone (PLAN.md 3.14) links whatever part of
 * libultra it uses:
 *   - the cartridge handle family (osCartRomInit, osEPi* and __osEPi*): the
 *     handle lives in the boot block's free end (0x80000360, below the code
 *     at 0x80000400); the ROM goes through pi.c's reader, domain 2 (a
 *     handle at 0xA8000000: SRAM, FlashRAM) to the save (pi.c, savedev.c);
 *   - __osInitialize_common and the debugger probes of the later libultras:
 *     what osInitialize does (nothing to do here), no debugger;
 *   - the Rumble Pak: osMotorInit says a pak is there, start and stop drive
 *     the Xbox pad's motors (the settings' Rumble row);
 *   - osFlash*: the FlashRAM's commands on savedev.c's chip, as libultra
 *     sends them; the Flash's handle beside the cartridge's (0x800003D8,
 *     its first 20 bytes: what the handle family reads);
 *   - the Voice Recognition Unit: absent;
 *   - IDO's unsigned 64-bit to float conversions.
 * The Controller Pak's osPfs* are pfs.c's. DIVERGES: no VRU. */
#include "savedev.h"
#include "settings.h"
#include "ultra.h"

uint32_t xu_pi_dma(uint32_t dev, uint32_t dram, uint32_t n);   /* pi.c: ROM -> RDRAM, 0 = ok */
uint32_t xu_pi_word(uint32_t dev);
int xu_pi_save_dma(uint32_t addr, uint32_t dram, uint32_t n, int write);   /* pi.c: domain 2 */
int xu_pi_save_io(uint32_t addr, uint32_t* word, int write);

/* ---- the cartridge handle (OSPiHandle: type +4, latency +5, pageSize +6,
 * relDuration +7, pulse +8, domain +9, baseAddress +12, speed +16) ---- */
#define CART_HANDLE 0x80000360u
#define PH_BASE 12

ULTRA(osCartRomInit) {
    (void)rdram;
    wr32(CART_HANDLE + 0, 0);
    wr8(CART_HANDLE + 4, 2);           /* DEVICE_TYPE_CART */
    wr8(CART_HANDLE + 5, 0x40);
    wr8(CART_HANDLE + 6, 0x07);
    wr8(CART_HANDLE + 7, 0x03);
    wr8(CART_HANDLE + 8, 0x12);
    wr8(CART_HANDLE + 9, 0);           /* domain 1 */
    wr32(CART_HANDLE + PH_BASE, 0xB0000000u);
    RET(CART_HANDLE);
}

static uint32_t handle_addr(uint32_t h, uint32_t dev) {
    return (h ? rd32(h + PH_BASE) : 0xB0000000u) | dev;
}

/* a transfer through a handle: domain 2 to the save, the ROM read */
static void epi_dma(uint32_t addr, uint32_t dram, uint32_t n, int write) {
    static unsigned logged;
    if (xu_pi_save_dma(addr, dram, n, write)) return;
    if (!write) xu_pi_dma(addr, dram, n);
    else if (logged++ < 8)
        xhw_logf("[PI] write to the cart domain %08x (%u bytes): ignored", (unsigned)addr, (unsigned)n);
}

/* osEPiStartDma(handle, mb, direction): the OSIoMesg holds the rest */
ULTRA(osEPiStartDma) {
    uint32_t h = ARG(0), mb = ARG(1), dir = ARG(2);
    uint32_t mq = rd32(mb + IO_RETQUEUE), dram = rd32(mb + IO_DRAMADDR), dev = rd32(mb + IO_DEVADDR), n = rd32(mb + IO_SIZE);
    (void)rdram;
    epi_dma(handle_addr(h, dev), dram, n, dir != 0);
    if (mq) xu_send(mq, mb, 0, 0);
    RET(0);
}

/* osEPiRawStartDma, __osEPiRawStartDma(handle, direction, devAddr, dramAddr, size) */
ULTRA(osEPiRawStartDma) {
    (void)rdram;
    epi_dma(handle_addr(ARG(0), ARG(2)), ARG(3), ARG(4), ARG(1) != 0);
    RET(0);
}
ULTRA(__osEPiRawStartDma) {
    (void)rdram;
    epi_dma(handle_addr(ARG(0), ARG(2)), ARG(3), ARG(4), ARG(1) != 0);
    RET(0);
}

/* osEPiReadIo, osEPiRawReadIo, __osEPiRawReadIo(handle, devAddr, u32* data) */
static void epi_read(recomp_context* ctx) {
    uint32_t a = handle_addr(ARG(0), ARG(1)), w;
    if (!xu_pi_save_io(a, &w, 0)) w = xu_pi_word(a);
    wr32(ARG(2), w);
    RET(0);
}
ULTRA(osEPiReadIo) {
    (void)rdram;
    epi_read(ctx);
}
ULTRA(osEPiRawReadIo) {
    (void)rdram;
    epi_read(ctx);
}
ULTRA(__osEPiRawReadIo) {
    (void)rdram;
    epi_read(ctx);
}

/* osEPiWriteIo, osEPiRawWriteIo, __osEPiRawWriteIo(handle, devAddr, u32 data):
 * the Flash's commands, SRAM words; the ROM takes none */
static void epi_write(recomp_context* ctx) {
    uint32_t w = ARG(2);
    xu_pi_save_io(handle_addr(ARG(0), ARG(1)), &w, 1);
    RET(0);
}
ULTRA(osEPiWriteIo) {
    (void)rdram;
    epi_write(ctx);
}
ULTRA(osEPiRawWriteIo) {
    (void)rdram;
    epi_write(ctx);
}
ULTRA(__osEPiRawWriteIo) {
    (void)rdram;
    epi_write(ctx);
}

/* ---- init variants and the debugger probes ---- */
ULTRA(__osInitialize_common) { (void)rdram; (void)ctx; }
ULTRA(__osInitialize_isv) { (void)rdram; (void)ctx; }
ULTRA(__osInitialize_kmc) { (void)rdram; (void)ctx; }
ULTRA(__osInitialize_msp) { (void)rdram; (void)ctx; }
ULTRA(__checkHardware_isv) { (void)rdram; RET(0); }
ULTRA(__checkHardware_kmc) { (void)rdram; RET(0); }
ULTRA(__checkHardware_msp) { (void)rdram; RET(0); }
ULTRA(__osRdbSend) { (void)rdram; RET(0); }
ULTRA(is_proutSyncPrintf) { (void)rdram; RET(0); }

/* ---- the Rumble Pak (OSPfs: queue +4, channel +8) ---- */
/* osMotorInit(mq, pfs, channel) */
ULTRA(osMotorInit) {
    uint32_t pfs = ARG(1);
    (void)rdram;
    if (pfs) {
        wr32(pfs + 0, 0);
        wr32(pfs + 4, ARG(0));
        wr32(pfs + 8, ARG(2));
    }
    RET(0);
}

static void motor(uint32_t pfs, int on) {
    int ch = pfs ? (int)rd32(pfs + 8) : 0;
    if (ch < 0 || ch > 3) return;
    xhw_pad_rumble(ch, on && g_xrc_settings.rumble ? 0xA000 : 0, on ? 1000 : 0);
}

/* __osMotorAccess(pfs, flag): the later libultras' start and stop */
ULTRA(__osMotorAccess) {
    (void)rdram;
    motor(ARG(0), ARG(1) != 0);
    RET(0);
}
ULTRA(osMotorStart) {
    (void)rdram;
    motor(ARG(0), 1);
    RET(0);
}
ULTRA(osMotorStop) {
    (void)rdram;
    motor(ARG(0), 0);
    RET(0);
}

/* ---- FlashRAM (osFlash*): libultra's command sequences on savedev.c's chip ---- */
#define FLASH_HANDLE 0x800003D8u
#define FLASH_REG (XU_CART_BASE + 0x10000u)

static void flash_cmd(uint32_t v) {
    xu_save_lock();
    xu_cart_write_word(FLASH_REG, v);
    xu_save_unlock();
}

static uint32_t flash_status(void) {
    uint32_t w;
    xu_save_lock();
    w = xu_cart_read_word(XU_CART_BASE);
    xu_save_unlock();
    return w;
}

/* the status read after an erase or a program, then cleared: 0 ok, -1 not */
static int flash_done(uint32_t bit) {
    uint32_t st = flash_status();
    flash_cmd(0xD2000000u);
    xu_save_lock();
    xu_cart_write_word(XU_CART_BASE, 0);
    xu_save_unlock();
    return (st & bit) ? 0 : -1;
}

/* osFlashInit(): the handle (DEVICE_TYPE_FLASH, domain 2, 0xA8000000) */
ULTRA(osFlashInit) {
    (void)rdram;
    wr32(FLASH_HANDLE + 0, 0);
    wr8(FLASH_HANDLE + 4, 8);
    wr8(FLASH_HANDLE + 5, 0x05);
    wr8(FLASH_HANDLE + 6, 0x0F);
    wr8(FLASH_HANDLE + 7, 0x02);
    wr8(FLASH_HANDLE + 8, 0x0C);
    wr8(FLASH_HANDLE + 9, 1);
    wr32(FLASH_HANDLE + PH_BASE, 0xA8000000u);
    wr32(FLASH_HANDLE + 16, 0);
    RET(FLASH_HANDLE);
}

/* osFlashReadStatus(u8* status) */
ULTRA(osFlashReadStatus) {
    (void)rdram;
    if (!XRC_SAVE_CART) return;
    flash_cmd(0xD2000000u);
    wr8(ARG(0), (uint8_t)flash_status());
}

/* osFlashReadId(u32* type, u32* maker) */
ULTRA(osFlashReadId) {
    uint8_t id[8];
    (void)rdram;
    if (!XRC_SAVE_CART) return;
    flash_cmd(0xE1000000u);
    xu_save_lock();
    xu_cart_read(XU_CART_BASE, id, 8);
    xu_save_unlock();
    wr32(ARG(0), ((uint32_t)id[0] << 24) | ((uint32_t)id[1] << 16) | ((uint32_t)id[2] << 8) | id[3]);
    wr32(ARG(1), ((uint32_t)id[4] << 24) | ((uint32_t)id[5] << 16) | ((uint32_t)id[6] << 8) | id[7]);
}

ULTRA(osFlashClearStatus) {
    (void)rdram;
    (void)ctx;
    if (!XRC_SAVE_CART) return;
    flash_cmd(0xD2000000u);
    xu_save_lock();
    xu_cart_write_word(XU_CART_BASE, 0);
    xu_save_unlock();
}

static int erase(uint32_t setup) {
    if (!XRC_SAVE_CART) return -1;
    flash_cmd(setup);
    flash_cmd(0x78000000u);
    return flash_done(0x08);
}

ULTRA(osFlashAllErase) {
    (void)rdram;
    RET(erase(0x3C000000u));
}
/* the Through forms start the erase and return; osFlashCheckEraseEnd asks */
ULTRA(osFlashAllEraseThrough) {
    (void)rdram;
    if (XRC_SAVE_CART) flash_cmd(0x3C000000u), flash_cmd(0x78000000u);
    RET(XRC_SAVE_CART ? 0 : -1);
}
/* osFlashSectorErase(page) */
ULTRA(osFlashSectorErase) {
    (void)rdram;
    RET(erase(0x4B000000u | (ARG(0) & 0xFFFF)));
}
ULTRA(osFlashSectorEraseThrough) {
    (void)rdram;
    if (XRC_SAVE_CART) flash_cmd(0x4B000000u | (ARG(0) & 0xFFFF)), flash_cmd(0x78000000u);
    RET(XRC_SAVE_CART ? 0 : -1);
}
/* osFlashCheckEraseEnd(): FLASH_STATUS_ERASE_OK (0), never busy */
ULTRA(osFlashCheckEraseEnd) {
    (void)rdram;
    RET(XRC_SAVE_CART ? flash_done(0x08) : -1);
}

/* osFlashWriteBuffer(mb, pri, dramAddr, mq): 128 bytes into the page buffer */
ULTRA(osFlashWriteBuffer) {
    uint32_t mb = ARG(0), dram = ARG(2), mq = ARG(3);
    (void)rdram;
    if (!XRC_SAVE_CART) {
        RET(-1);
        return;
    }
    flash_cmd(0xB4000000u);
    xu_pi_save_dma(XU_CART_BASE, dram, 128, 1);
    if (mb) {
        wr32(mb + IO_RETQUEUE, mq);
        wr32(mb + IO_DRAMADDR, dram);
        wr32(mb + IO_DEVADDR, 0);
        wr32(mb + IO_SIZE, 128);
    }
    if (mq) xu_send(mq, mb, 0, 0);
    RET(0);
}

/* osFlashWriteArray(page): the buffer into the page */
ULTRA(osFlashWriteArray) {
    (void)rdram;
    if (!XRC_SAVE_CART) {
        RET(-1);
        return;
    }
    flash_cmd(0xA5000000u | (ARG(0) & 0xFFFF));
    RET(flash_done(0x04));
}

/* osFlashReadArray(mb, pri, page, dramAddr, nPages, mq) */
ULTRA(osFlashReadArray) {
    uint32_t mb = ARG(0), page = ARG(2), dram = ARG(3), pages = ARG(4), mq = ARG(5);
    (void)rdram;
    if (!XRC_SAVE_CART) {
        RET(-1);
        return;
    }
    flash_cmd(0xF0000000u);
    xu_pi_save_dma(XU_CART_BASE + page * 128u, dram, pages * 128u, 0);
    if (mb) {
        wr32(mb + IO_RETQUEUE, mq);
        wr32(mb + IO_DRAMADDR, dram);
        wr32(mb + IO_DEVADDR, page * 128u);
        wr32(mb + IO_SIZE, pages * 128u);
    }
    if (mq) xu_send(mq, mb, 0, 0);
    RET(0);
}

/* osFlashChange(n): one chip */
ULTRA(osFlashChange) { (void)rdram; (void)ctx; }

/* ---- the Voice Recognition Unit: none (CONT_ERR_NO_CONTROLLER = 1) ---- */
ULTRA(osVoiceInit) { (void)rdram; RET(1); }
ULTRA(osVoiceSetWord) { (void)rdram; RET(1); }
ULTRA(osVoiceCheckWord) { (void)rdram; RET(1); }
ULTRA(osVoiceStopReadData) { (void)rdram; RET(1); }
ULTRA(osVoiceMaskDictionary) { (void)rdram; RET(1); }
ULTRA(osVoiceStartReadData) { (void)rdram; RET(1); }
ULTRA(osVoiceControlGain) { (void)rdram; RET(1); }
ULTRA(osVoiceGetReadData) { (void)rdram; RET(1); }
ULTRA(osVoiceClearDictionary) { (void)rdram; RET(1); }

/* ---- IDO: unsigned 64-bit to double / float, the result in $f0 ---- */
ULTRA(__ull_to_d) {
    (void)rdram;
    ctx->f0.d = (double)xu_arg64(ctx, 0);
}
ULTRA(__ull_to_f) {
    (void)rdram;
    ctx->f0.fl = (float)xu_arg64(ctx, 0);
}
