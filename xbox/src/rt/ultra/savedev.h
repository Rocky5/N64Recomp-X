/* savedev.h - the save devices' protocols (PLAN.md 3.6), without the
 * platform: the Controller Pak and the Rumble Pak as the PIF sees them, the
 * cartridge's SRAM and FlashRAM as the PI sees them. savedev.c holds no file
 * and no lock; save.c owns the images (and their files), and the callers
 * (si.c, pi.c, pfs.c) hold xu_save_lock around every call. The host test
 * (tests/xbox/test_saves.c) supplies the callbacks itself. */
#ifndef XRC_SAVEDEV_H
#define XRC_SAVEDEV_H

#include <stdint.h>

/* kill switches: -DXRC_SAVE_PAK=0, no pak in any port (the PIF and osPfs*
 * say "no pak"); -DXRC_SAVE_CART=0, no SRAM or Flash (domain 2 reads zeros,
 * writes are dropped, osFlash* fail) */
#ifndef XRC_SAVE_PAK
#define XRC_SAVE_PAK 1
#endif
#ifndef XRC_SAVE_CART
#define XRC_SAVE_CART 1
#endif

/* ---- the paks ---- */
#define XU_PAK_SIZE 0x8000u   /* 256 Kbit: 128 pages of 256 bytes, 1024 blocks of 32 */
enum { XU_PAK_NONE, XU_PAK_MEMORY, XU_PAK_RUMBLE, XU_PAK_AUTO };

/* libultra's CRCs: the 5-bit one on a block address (the block number,
 * 0..0x7FF), the 8-bit one on 32 data bytes */
uint8_t xu_pak_addr_crc(uint16_t block);
uint8_t xu_pak_data_crc(const uint8_t* data);

/* which pak a port holds; AUTO is a Controller Pak that turns into a Rumble
 * Pak if the game probes for one before it ever reads the pak's file system */
void xu_pak_set_kind(int port, int kind);
int xu_pak_kind(int port);   /* NONE, MEMORY or RUMBLE (AUTO answers as MEMORY until it decides) */

/* a 32-byte block at a byte address (0..0xFFFF, 32-aligned), as the pak
 * answers it: the memory below 0x8000, the bank register at 0x8000, the
 * Rumble Pak's probe (0x8000) and motor (0xC000) */
void xu_pak_access(int port, uint16_t addr, uint8_t* data, int write);

/* the PIF commands 0x02 (read) and 0x03 (write) on controller channel
 * `port`: the command bytes, their count, the answer's room. 0: answered;
 * -1: no answer (the caller sets the no-response bit) */
int xu_pak_pif(int port, const uint8_t* cmd, int tx, uint8_t* resp, int rx);

/* a formatted, empty pak (libultra's layout: the ID in blocks 1, 3, 4 and 6,
 * the inode table in pages 1 and 2, the directory in pages 3 and 4) */
void xu_pak_format(uint8_t* img);
/* libultra's own checks: the ID's two sums, the inode page's sum */
int xu_pak_id_ok(const uint8_t* id32);
void xu_pak_make_id(uint8_t* id32, uint32_t random);
uint8_t xu_pak_inode_sum(const uint8_t* page, int start_page);

/* from save.c (the host test's own in tests): the port's 32 KB image, read
 * from its file on first use or formatted; a write landed in it; the motor */
uint8_t* xu_pak_image(int port);
void xu_pak_dirty(int port);
void xu_pak_rumble(int port, int on);

/* ---- the cartridge's SRAM (256 Kbit) and FlashRAM (1 Mbit) on PI domain 2 ---- */
#define XU_CART_BASE 0x08000000u
#define XU_SRAM_SIZE 0x8000u
#define XU_FLASH_SIZE 0x20000u
enum { XU_CART_UNKNOWN, XU_CART_SRAM, XU_CART_FLASH };
#define XU_FLASH_TYPE 0x11118001u
#define XU_FLASH_MAKER 0x00C2001Du   /* Macronix (libultra's NEW_FLASH: page addresses in bytes) */

int xu_cart_is_save(uint32_t phys);   /* a domain-2 address (0x05000000..0x0FFFFFFF) */
int xu_cart_kind(void);
/* bytes and words at a physical domain-2 address, in the cartridge's order */
void xu_cart_read(uint32_t phys, uint8_t* dst, uint32_t n);
void xu_cart_write(uint32_t phys, const uint8_t* src, uint32_t n);
uint32_t xu_cart_read_word(uint32_t phys);
void xu_cart_write_word(uint32_t phys, uint32_t v);
void xu_cart_reset(void);   /* tests: forget the kind and the Flash's state */

/* from save.c: the image for a kind (XU_CART_UNKNOWN: whichever file is on
 * the disk, else SRAM; *kind says which), and a write landed in it */
uint8_t* xu_cart_image(int* kind);
void xu_cart_dirty(void);

#endif
