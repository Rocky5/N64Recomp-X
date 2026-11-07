/* A Bug's Life (NBYE, US): MusFxBankInitialize of Software Creations' N64 music
 * library (0x8007AD60), written here (MIT). n64rx builds it in place of the
 * recompiled routine (N64RX_PATCHES=0: not used).
 *
 * The game gives the library its wave table, in the cartridge at 0xB08D1800 read
 * through KSEG1, as the default effects bank too. The routine relocates a bank in
 * place: {count, ..., offsets}, with count the word at +0. Here that word is the
 * table's "N64 " name, 1.3 billion. On the N64 the stores go nowhere and the loop's
 * read of the count straight after a store returns the word just stored (the PI bus
 * holds it), an address with its top bit set, negative: the loop ends after one
 * pass, and nothing changed. Here the read gives "N64 " and it walks off the cartridge.
 *
 * DIVERGES: a bank in the cartridge is left alone without the stores or the pass
 * they end, which is what they come to. A bank in RDRAM is relocated as the game's
 * code does.
 */
#include "recomp.h"

#define CURRENT_BANK 0x8009194Cu

XRC_PATCH(0x8007AD60) {
    gpr bank = ctx->r4, cur = (gpr)(int32_t)CURRENT_BANK;
    uint32_t b = (uint32_t)bank;
    int32_t n, i;
    if (MEM_W(0xC, bank) & 1) return;   /* relocated already */
    if (!MEM_W(0, cur)) MEM_W(0, cur) = (int32_t)b;
    if (b >= 0x90000000u && b < 0xC0000000u && (b < 0xA0000000u || b >= 0xB0000000u)) return;   /* the cartridge */
    n = MEM_W(0, bank);
    MEM_W(0xC, bank) = 1;
    MEM_W(0x10, bank) = 0;
    MEM_W(0x14, bank) = (int32_t)(b + (uint32_t)MEM_W(0x14, bank));
    for (i = 0; i < n; i++) MEM_W(0x18 + 8 * i, bank) = (int32_t)(b + (uint32_t)MEM_W(0x18 + 8 * i, bank));
}
