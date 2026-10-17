/* xrc_game.h - what the runtime knows about the game built in (game triple).
 *
 * n64rx writes xrc_game.c next to the generated code from the game pack
 * (games/<id>/game.toml) and the recompiler's tables (recomp_overlays.inl);
 * a build without a game links xbox/src/rt/nogame/xrc_nogame.c instead, whose
 * xrc_game has no id. Facts only: no byte of the ROM is in here. */
#ifndef XRC_GAME_H
#define XRC_GAME_H

#include <stddef.h>
#include <stdint.h>

#include "librecomp/sections.h"
#include "recomp.h"

typedef struct XrcGame {
    const char* id;                    /* the pack's folder name; NULL: no game built in */
    const char* title;                 /* "Pilotwings 64" */
    uint32_t title_id;                 /* 0x4E52nnnn */
    uint32_t rdram_mb;                 /* 4, or 8 with the Expansion Pak */
    uint32_t entrypoint;               /* the boot segment's vram */
    uint32_t boot_size;                /* bytes copied from ROM 0x1000 to the entrypoint */
    int gpr_bits;                      /* 32 or 64: XRC_GPR32 as the code was compiled */
    int test_rom;                      /* the ROM-free end-to-end test (games/test) */
    recomp_func_t* entry;              /* recomp_entrypoint */
    const SectionTableEntry* sections; /* recomp_overlays.inl's section_table */
    size_t num_sections;
    const uint32_t* icon;              /* 64x64 ARGB from the pack's icon.png, or NULL */
    const char* code;                  /* the header's game code, "NPWE" */
    uint32_t rom_size;                 /* bytes; 0: any */
    const char* sha1[4];               /* the known dumps (normalised image), NULL-ended */
    uint32_t eeprom_bytes;             /* 512 (4 Kbit), 2048 (16 Kbit), 0: none */
    const uint32_t* rom_pins;          /* [rom] pin: offset, length pairs held in the page cache */
    uint32_t num_rom_pins;
    uint32_t cic;                      /* the boot chip, 6101..6106, from the ROM's IPL3: osCicId (0 = 6102) */
} XrcGame;

extern const XrcGame xrc_game;

#endif
