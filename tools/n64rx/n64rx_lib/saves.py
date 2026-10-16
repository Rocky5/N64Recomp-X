"""What the runtime's saves need from the game's own libultra (PLAN.md 3.6,
3.14): facts found in the user's ROM at build time, passed as -D flags to
the runtime, never kept.

libultra's controller globals: the runtime replaces osContInit, but a game
whose Controller Pak code is recompiled (its osPfs* were not named) counts
the channels it asks the PIF about by __osMaxControllers (older libultras:
_osContNumControllers), which only the real osContInit sets, to
MAXCONTROLLERS (4). Left at zero, osPfsIsPlug sees no pak in any port. The
address is found in the game's osContInit: the byte store of the constant 4
to a lui-based address. A rule about libultra's code, not about one game."""
import pathlib
import tomllib


def _sext16(v):
    return v - 0x10000 if v & 0x8000 else v


def cont_max_store(code: bytes, limit=0x200):
    """(address, size) of the constant-4 store to a global in osContInit's
    code (big-endian MIPS), or None"""
    hi, const = {}, {}
    for i in range(0, min(len(code), limit) - 3, 4):
        w = int.from_bytes(code[i:i + 4], 'big')
        op, rs, rt, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
        if w == 0x03E00008:   # jr ra
            break
        if op in (0x28, 0x29, 0x2B):   # sb/sh/sw
            if rs in hi and const.get(rt) == 4:
                return (hi[rs] + _sext16(imm)) & 0xFFFFFFFF, {0x28: 1, 0x29: 2, 0x2B: 4}[op]
            continue
        dst = (w >> 11) & 31 if op == 0 else 31 if op == 3 else rt if 0x08 <= op <= 0x0F or 0x20 <= op <= 0x27 else None
        if dst is None:
            continue
        hi.pop(dst, None), const.pop(dst, None)
        if op == 0x0F:
            hi[rt] = (imm << 16) & 0xFFFFFFFF
        elif op in (0x09, 0x0D) and rs == 0:   # addiu/ori rt, zero, imm
            const[rt] = (_sext16(imm) if op == 0x09 else imm) & 0xFFFFFFFF
    return None


def _syms(pack):
    """the pack's symbols file: sections (rom, vram, size) and functions"""
    p = pathlib.Path(pack.dir) / 'syms.toml'
    cfg = pathlib.Path(pack.dir) / 'recomp.toml'
    if cfg.is_file():
        rel = tomllib.loads(cfg.read_text(encoding='utf-8')).get('input', {}).get('symbols_file_path')
        if rel:
            p = pathlib.Path(pack.dir) / rel
    if not p.is_file():
        return []
    return tomllib.loads(p.read_text(encoding='utf-8')).get('section', [])


def cflags(pack, rom_data: bytes):
    """-D flags for the runtime: -DXRC_CONT_MAX_ADDR / _SIZE when found"""
    for sec in _syms(pack):
        for f in sec.get('functions', []):
            if f.get('name') != 'osContInit':
                continue
            off = sec['rom'] + (f['vram'] - sec['vram'])
            found = cont_max_store(rom_data[off:off + f.get('size', 0x200)])
            if found:
                addr, size = found
                return [f'-DXRC_CONT_MAX_ADDR=0x{addr:08X}u', f'-DXRC_CONT_MAX_SIZE={size}']
    return []
