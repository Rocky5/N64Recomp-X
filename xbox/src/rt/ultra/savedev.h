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
/* a hot-swap (the BACK menu's Paks page): the next status says the pak was
 * pulled; the status byte of a port (0x01 in, 0x02 pulled since asked) */
void xu_pak_pull(int port);
uint8_t xu_pak_status(int port);

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
void xu_pak_format_id(uint8_t* img, uint32_t random);   /* with this ID word (a pak of its own) */

/* the notes on a pak (its directory), for the BACK menu: up to 16, in
 * directory order. code: the game code and company as text ("NKTE01");
 * name: the note's name and extension, in ASCII; pages: its inode chain's
 * length (-1: broken) */
typedef struct xu_pak_note {
    uint8_t slot;        /* the directory entry, 0..15 */
    int16_t pages;
    uint16_t company;
    uint32_t game;
    char code[8];
    char name[24];
} xu_pak_note;
int xu_pak_notes(const uint8_t* img, xu_pak_note* out16, int* free_pages);
/* osPfsDeleteFile's work on entry `slot`: its pages free, the inode table's
 * sum and mirror, the entry cleared; -1 if the entry is not in use */
int xu_pak_note_delete(uint8_t* img, int slot);
/* entry `slot` of src copied onto dst with its pages, into dst's lowest
 * free pages: 0; -1 no such note (or a broken chain); -2 dst has it
 * already; -3 no room (pages or a directory entry) */
int xu_pak_note_copy(const uint8_t* src, int slot, uint8_t* dst);
char xu_pak_char(uint8_t c);   /* the pak's character set to ASCII (0: the end, '?': no ASCII) */
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

int xu_cart_is_save(uint32_t phys);   /* a save address (0x08000000..0x0FFFFFFF; savedev.c) */
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
