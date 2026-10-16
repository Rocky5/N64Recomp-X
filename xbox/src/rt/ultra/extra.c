/* extra.c - the rest of the recompiler's reimplemented list (PLAN.md 3.3),
 * so a game built from the ROM alone (PLAN.md 3.14) links whatever part of
 * libultra it uses:
 *   - the cartridge handle family (osCartRomInit, osEPiStartDma,
 *     osEPiRawStartDma, osEPiReadIo): the handle lives in the boot block's
 *     free end (0x80000360, below the code at 0x80000400), DMA goes through
 *     pi.c's ROM reader;
 *   - __osInitialize_common and the debugger probes of the later libultras:
 *     what osInitialize does (nothing to do here), no debugger;
 *   - the Rumble Pak: osMotorInit says a pak is there, start and stop drive
 *     the Xbox pad's motors (the settings' Rumble row);
 *   - the Controller Pak (osPfs*): no pak (PFS_ERR_NOPACK) until M6's file-
 *     backed pak; the Flash and the Voice Recognition Unit: absent;
 *   - IDO's unsigned 64-bit to float conversions.
 * DIVERGES: no Controller Pak, Flash or VRU yet. */
#include "settings.h"
#include "ultra.h"

uint32_t xu_pi_dma(uint32_t dev, uint32_t dram, uint32_t n);   /* pi.c: ROM -> RDRAM, 0 = ok */
uint32_t xu_pi_word(uint32_t dev);

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

/* osEPiStartDma(handle, mb, direction): the OSIoMesg holds the rest */
ULTRA(osEPiStartDma) {
    uint32_t h = ARG(0), mb = ARG(1), dir = ARG(2);
    uint32_t mq = rd32(mb + IO_RETQUEUE), dram = rd32(mb + IO_DRAMADDR), dev = rd32(mb + IO_DEVADDR), n = rd32(mb + IO_SIZE);
    (void)rdram;
    if (dir == 0) xu_pi_dma(handle_addr(h, dev), dram, n);
    else xhw_logf("[PI] write to the cart domain %08x (%u bytes): ignored", (unsigned)handle_addr(h, dev), (unsigned)n);
    if (mq) xu_send(mq, mb, 0, 0);
    RET(0);
}

/* osEPiRawStartDma(handle, direction, devAddr, dramAddr, size) */
ULTRA(osEPiRawStartDma) {
    uint32_t h = ARG(0), dir = ARG(1), dev = ARG(2), dram = ARG(3), n = ARG(4);
    (void)rdram;
    if (dir == 0) xu_pi_dma(handle_addr(h, dev), dram, n);
    RET(0);
}

/* osEPiReadIo(handle, devAddr, u32* data) */
ULTRA(osEPiReadIo) {
    (void)rdram;
    wr32(ARG(2), xu_pi_word(handle_addr(ARG(0), ARG(1))));
    RET(0);
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

/* ---- the Controller Pak: none (PFS_ERR_NOPACK = 1) ---- */
#define NOPACK 1
ULTRA(osPfsInitPak) { (void)rdram; RET(NOPACK); }
ULTRA(osPfsFreeBlocks) { (void)rdram; RET(NOPACK); }
ULTRA(osPfsAllocateFile) { (void)rdram; RET(NOPACK); }
ULTRA(osPfsDeleteFile) { (void)rdram; RET(NOPACK); }
ULTRA(osPfsFileState) { (void)rdram; RET(NOPACK); }
ULTRA(osPfsFindFile) { (void)rdram; RET(NOPACK); }
ULTRA(osPfsReadWriteFile) { (void)rdram; RET(NOPACK); }

/* ---- Flash: absent (-1) ---- */
ULTRA(osFlashInit) { (void)rdram; RET(0); }
ULTRA(osFlashReadStatus) { (void)rdram; RET(-1); }
ULTRA(osFlashReadId) { (void)rdram; RET(-1); }
ULTRA(osFlashClearStatus) { (void)rdram; (void)ctx; }
ULTRA(osFlashAllErase) { (void)rdram; RET(-1); }
ULTRA(osFlashAllEraseThrough) { (void)rdram; RET(-1); }
ULTRA(osFlashSectorErase) { (void)rdram; RET(-1); }
ULTRA(osFlashSectorEraseThrough) { (void)rdram; RET(-1); }
ULTRA(osFlashCheckEraseEnd) { (void)rdram; RET(-1); }
ULTRA(osFlashWriteBuffer) { (void)rdram; RET(-1); }
ULTRA(osFlashWriteArray) { (void)rdram; RET(-1); }
ULTRA(osFlashReadArray) { (void)rdram; RET(-1); }
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
