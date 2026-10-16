#!/usr/bin/env python3
"""Which register width a game can run at, and where it builds KSEG1
addresses (PLAN.md 3.2, 3.2.1).

  tools/xbox/recomp_scan64.py <rom.z64> <syms.toml>

Disassembles every function of the symbol file with rabbitizer and lists the
64-bit-ISA instructions in it (doubleword arithmetic and shifts, ld/sd and
their unaligned forms, dmfc1/dmtc1, the .l conversions, lwu). Zero hits mean
the game's own code never leaves O32's sign-extended registers, so 32-bit
registers are exact (-DXRC_GPR32=1). The libultra functions the runtime
replaces (the recompiler's reimplemented_funcs list: __osException,
osGetTime's math ...) are not recompiled and not counted. It also reports
every `lui` of 0xA0xx-0xBFxx: the sites that build KSEG1 or cartridge-domain
addresses for the CPU, which a pack patches (Zelda64Recomp's way).

Used by n64rx (gpr = "auto"); prints a report on its own.
"""
import pathlib
import struct
import sys
import tomllib

OPS64 = {
    'dadd', 'daddu', 'daddi', 'daddiu', 'dsub', 'dsubu', 'dsll', 'dsll32', 'dsrl', 'dsrl32', 'dsra', 'dsra32',
    'dsllv', 'dsrlv', 'dsrav', 'dmult', 'dmultu', 'ddiv', 'ddivu', 'ld', 'sd', 'ldl', 'ldr', 'sdl', 'sdr', 'lld',
    'scd', 'lwu', 'dmfc1', 'dmtc1', 'cvt.s.l', 'cvt.d.l', 'cvt.l.s', 'cvt.l.d', 'trunc.l.s', 'trunc.l.d',
    'round.l.s', 'round.l.d', 'ceil.l.s', 'ceil.l.d', 'floor.l.s', 'floor.l.d',
}


def reimplemented():
    """the recompiler's reimplemented and ignored lists, read from its source
    at the pin, so the scan skips exactly what it skips"""
    src = pathlib.Path(__file__).resolve().parents[2] / 'tools/recomp/src/symbol_lists.cpp'
    names = set()
    if not src.exists():
        return names
    text = src.read_text(encoding='utf-8', errors='replace')
    for list_name in ('reimplemented_funcs', 'ignored_funcs'):
        at = text.find(list_name)
        if at < 0:
            continue
        end = text.find('};', at)
        names.update(part.strip().strip('"') for part in text[at:end].split('"')[1::2])
    return names


def scan(rom, syms):
    """{'hits': [(func, vram, mnemonic)], 'kseg1': [(func, vram, imm)], 'functions': n}"""
    import rabbitizer
    skip = reimplemented()
    hits, kseg1, nfunc = [], [], 0
    for sec in syms.get('section', []):
        rom0, vram0 = sec['rom'], sec['vram']
        for f in sec.get('functions', []):
            if f['name'] in skip:
                continue
            nfunc += 1
            for off in range(0, f['size'], 4):
                vram = f['vram'] + off
                at = rom0 + (vram - vram0)
                if at + 4 > len(rom):
                    break
                word = struct.unpack_from('>I', rom, at)[0]
                ins = rabbitizer.Instruction(word, vram=vram)
                name = ins.getOpcodeName()
                if name in OPS64:
                    hits.append((f['name'], vram, name))
                if name == 'lui':
                    imm = word & 0xFFFF
                    if 0xA000 <= imm <= 0xBFFF:
                        kseg1.append((f['name'], vram, imm))
    return {'hits': hits, 'kseg1': kseg1, 'functions': nfunc}


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    rom = pathlib.Path(sys.argv[1]).read_bytes()
    syms = tomllib.loads(pathlib.Path(sys.argv[2]).read_text(encoding='utf-8'))
    r = scan(rom, syms)
    print(f"{r['functions']} functions scanned")
    for f, v, n in r['hits']:
        print(f'  64-bit: {f} {v:08X} {n}')
    for f, v, i in r['kseg1']:
        print(f'  KSEG1:  {f} {v:08X} lui 0x{i:04X}')
    print(f"registers: {'32-bit (exact)' if not r['hits'] else '64-bit'}; {len(r['kseg1'])} KSEG1 sites")


if __name__ == '__main__':
    main()
