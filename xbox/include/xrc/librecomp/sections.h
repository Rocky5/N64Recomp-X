/* sections.h - the structs N64Recomp's recomp_overlays.inl fills in (its
 * generator prints #include "librecomp/sections.h"). Written here to the
 * field names and layout the generated tables use (src/main.cpp of the
 * pinned recompiler); the header of that name in N64ModernRuntime is GPL and
 * not used. */
#ifndef XRC_SECTIONS_H
#define XRC_SECTIONS_H

#include <stddef.h>
#include <stdint.h>

#include "recomp.h"

#define ARRLEN(x) (sizeof(x) / sizeof((x)[0]))

typedef enum {
    R_MIPS_NONE = 0,
    R_MIPS_16,
    R_MIPS_32,
    R_MIPS_REL32,
    R_MIPS_26,
    R_MIPS_HI16,
    R_MIPS_LO16,
    R_MIPS_GPREL16,
} RelocEntryType;

typedef struct {
    recomp_func_t* func;
    uint32_t offset;     /* from the section's start */
    uint32_t rom_size;
} FuncEntry;

typedef struct {
    uint32_t offset;                 /* the instruction, from the section's start */
    uint32_t target_section_offset;
    uint16_t target_section;
    RelocEntryType type;
} RelocEntry;

typedef struct {
    uint32_t rom_addr;
    uint32_t ram_addr;
    uint32_t size;
    FuncEntry* funcs;
    size_t num_funcs;
    RelocEntry* relocs;
    size_t num_relocs;
    size_t index;
} SectionTableEntry;

typedef struct {
    const char* name;
    uint32_t ram_addr;
} FunctionExport;

typedef struct {
    uint32_t original_vram;
    recomp_func_t* func;
} ManualPatchSymbol;

#endif
