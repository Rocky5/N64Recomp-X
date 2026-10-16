/* pi.c - cartridge DMA from the ROM (PLAN.md 3.3, 3.5).
 *
 * The ROM is the verified, normalised (z64) image, read only through
 * xhw_rom_read (xrom.h): in RAM whole for ROMs up to 16 MB, the page cache
 * (xhw_romcache.c) for bigger ones. osPiStartDma copies at once into the
 * word-swapped RDRAM image and sends the request's message; the PI manager
 * thread the game created is never needed for that, but its queue is set
 * up. osPiReadIo returns the big-endian word, as the bus did. Overlay
 * tracking by DMA source is M5 (no relocatable sections yet). */
#include "xrom.h"
#include "ultra.h"

static unsigned s_bad;

/* ---- the ROM image's readers: every one goes through xhw_rom_read ---- */

static void sink_rdram(void* user, uint32_t at, const uint8_t* src, uint32_t n) {
    xu_to_rdram(*(const uint32_t*)user + at, src, n);
}

static void sink_host(void* user, uint32_t at, const uint8_t* src, uint32_t n) {
    memcpy((uint8_t*)user + at, src, n);
}

uint32_t xu_rom_to_rdram(uint32_t off, uint32_t dram, uint32_t n) {
    return xhw_rom_read(off, n, sink_rdram, &dram) ? 0 : 1;
}

uint32_t xu_rom_read(uint32_t off, void* dst, uint32_t n) {
    return xhw_rom_read(off, n, sink_host, dst) ? 0 : 1;
}

/* The image in RAM when resident; NULL with the page cache (whose chunks may
 * go at any time): xu_rom_read or xu_rom_to_rdram instead. */
const uint8_t* xu_rom_ptr(uint32_t off, uint32_t n) {
    const uint8_t* rom = xhw_rom_resident();
    uint32_t size = xhw_rom_size();
    if (!rom || off >= size || n > size - off) return NULL;
    return rom + off;
}

void xu_to_rdram(uint32_t dst, const uint8_t* src, uint32_t n) {
    uint32_t i = 0;
    /* the unaligned head, a byte at a time */
    for (; i < n && ((dst + i) & 3); i++) wr8(dst + i, src[i]);
    /* whole words: big-endian bytes to a native word */
    for (; i + 4 <= n; i += 4)
        wr32(dst + i, ((uint32_t)src[i] << 24) | ((uint32_t)src[i + 1] << 16) | ((uint32_t)src[i + 2] << 8) | src[i + 3]);
    for (; i < n; i++) wr8(dst + i, src[i]);
}

void xu_from_rdram(uint8_t* dst, uint32_t src, uint32_t n) {
    uint32_t i;
    for (i = 0; i < n; i++) dst[i] = rd8(src + i);
}

/* osPiStartDma's and osPiReadIo's device addresses are offsets into the
 * cartridge domain: libultra ORs osRomBase (0xB0000000) in itself. A full
 * PI bus or KSEG1 address is accepted too. */
static uint32_t rom_offset(uint32_t dev) {
    dev &= 0x1FFFFFFFu;
    return dev >= 0x10000000u ? dev - 0x10000000u : dev;
}

/* ROM -> RDRAM for every DMA entry point (the handle family in extra.c too); 0 = from the ROM */
uint32_t xu_pi_dma(uint32_t dev, uint32_t dram, uint32_t n) {
    uint32_t i;
    if (xu_rom_to_rdram(rom_offset(dev), dram, n) == 0) {
        xrc_learn_dma(rom_offset(dev), dram, n);   /* learn.c: where code came from */
        return 0;
    }
    for (i = 0; i < n; i++) wr8(dram + i, 0);
    if (s_bad++ < 16)
        xhw_logf("[PI] DMA outside the ROM: dev %08x, %u bytes to %08x (zeros)", (unsigned)dev, (unsigned)n,
                 (unsigned)dram);
    return 1;
}

/* osPiStartDma(mb, pri, dir, devAddr, vAddr (0x10), nbytes (0x14), mq (0x18)) */
ULTRA(osPiStartDma) {
    uint32_t mb = ARG(0), dir = ARG(2), dev = ARG(3), dram = ARG(4), n = ARG(5), mq = ARG(6);
    (void)rdram;
    if (dir != 0) {   /* OS_WRITE: SRAM/Flash, M2+ */
        xhw_logf("[PI] write to the cart domain %08x (%u bytes): ignored", (unsigned)dev, (unsigned)n);
    } else {
        xu_pi_dma(dev, dram, n);
    }
    if (mb) {
        wr32(mb + IO_RETQUEUE, mq);
        wr32(mb + IO_DRAMADDR, dram);
        wr32(mb + IO_DEVADDR, dev);
        wr32(mb + IO_SIZE, n);
    }
    if (mq) xu_send(mq, mb, 0, 0);
    RET(0);
}

uint32_t xu_pi_word(uint32_t dev);
uint32_t xu_pi_word(uint32_t dev) {
    uint8_t p[4];
    if (xu_rom_read(rom_offset(dev) & ~3u, p, 4)) return 0;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t read_word(uint32_t dev) { return xu_pi_word(dev); }

/* osPiReadIo(devAddr, u32* data) */
ULTRA(osPiReadIo) {
    (void)rdram;
    wr32(ARG(1), read_word(ARG(0)));
    RET(0);
}

ULTRA(osPiRawReadIo) {
    (void)rdram;
    wr32(ARG(1), read_word(ARG(0)));
    RET(0);
}

/* osCreatePiManager(pri, cmdQ, cmdBuf, cmdMsgCnt): the queue, for anything
 * that looks at it; DMA does not go through it here */
ULTRA(osCreatePiManager) {
    uint32_t mq = ARG(1);
    (void)rdram;
    if (mq) {
        wr32(mq + MQ_MTQUEUE, 0);
        wr32(mq + MQ_FULLQUEUE, 0);
        wr32(mq + MQ_VALIDCOUNT, 0);
        wr32(mq + MQ_FIRST, 0);
        wr32(mq + MQ_MSGCOUNT, ARG(3));
        wr32(mq + MQ_MSG, ARG(2));
    }
}

ULTRA(osPiGetStatus) {
    (void)rdram;
    RET(0);
}
