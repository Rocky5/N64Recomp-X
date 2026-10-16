#!/usr/bin/env python3
"""The ROM inside the XBE (PLAN.md 3.5): xbe_sections.py's $ROM section on a
synthetic XBE (and on the platform XBE when one is built), then
tests/xbox/test_xbe_rom.c finds it with the runtime's parser and checks the
embedded bytes with xrom_verify. The ROM is the synthetic test ROM
(make_test_rom.py), never a game's."""
import hashlib
import os
import pathlib
import struct
import sys
import tempfile

import hostcc
import make_test_rom
import xbe_sections as xs

ROOT = pathlib.Path(__file__).resolve().parents[2]
BASE = 0x10000


def synthetic_xbe():
    """a minimal cxbe-shaped XBE: header, certificate, .text and .data"""
    d = bytearray(0x1000)
    d[0:4] = b'XBEH'
    sections = [('.text', xs.PRELOAD | xs.EXECUTABLE, 0x11000, 0x3000), ('.data', xs.PRELOAD | xs.WRITABLE, 0x14000, 0x2000)]
    cert, tab = 0x178, 0x178 + 0x1D0
    names = tab + len(sections) * 0x38
    refs = names + 16
    struct.pack_into('<III', d, 0x104, BASE, refs + 4 * len(sections), 0x6000)
    struct.pack_into('<III', d, 0x118, BASE + cert, len(sections), BASE + tab)
    raw = 0x1000
    for i, (name, flags, va, size) in enumerate(sections):
        struct.pack_into('<9I', d, tab + i * 0x38, flags, va, size, raw, size, BASE + names + 8 * i, 0,
                         BASE + refs + 4 * i, BASE + refs + 4 * i + 2)
        d[names + 8 * i:names + 8 * i + len(name)] = name.encode()
        d += bytes([0x90 + i]) * size
        raw += size
    return bytes(d)


def check(xbe, rom, what):
    before = xs.find_section(xbe, '.text')
    out = xs.add_section(xbe, '$$XTIMAGE', bytes(0x2800)) or xbe
    img_before = struct.unpack_from('<I', out, 0x10C)[0]
    out = xs.add_rom(out, rom)
    assert out is not None, what
    flags, va, vsize, raw, rawsize = xs.find_section(out, xs.ROM_SECTION)
    assert not flags & xs.PRELOAD, f'{what}: $ROM is preloaded'
    assert raw % 0x1000 == 0 and raw + rawsize == len(out) and out[raw:] == rom, what
    base, hdrs, img = struct.unpack_from('<III', out, 0x104)
    assert hdrs <= 0x1000 and hdrs <= min(xs.find_section(out, n)[3] for n in ('.text', '$$XTIMAGE')), what
    # no virtual range: the image (and the kernel's page tables for it) as before
    assert vsize == 0 and img == img_before and va >= base + img, what
    assert xs.find_section(out, '.text') == before, f'{what}: .text moved'
    assert xs.add_rom(out, rom) is None, f'{what}: a second pass added it again'
    return out


def main():
    _, rom, _ = make_test_rom.build()
    sha1 = hashlib.sha1(rom).hexdigest()
    xbes = [('synthetic', synthetic_xbe())]
    built = ROOT / 'build-xbox/xbe/default.xbe'
    if built.exists():
        xbes.append(('build-xbox', built.read_bytes()))
    rc = 0
    with tempfile.TemporaryDirectory() as tmp:
        for what, xbe in xbes:
            path = os.path.join(tmp, f'{what}.xbe')
            with open(path, 'wb') as f:
                f.write(check(xbe, rom, what))
            rc |= hostcc.run(['tests/xbox/test_xbe_rom.c', 'xbox/src/hw/xhw_rom.c', 'xbox/src/hw/xhw_sha1.c'],
                             includes=['xbox/include'], defines=['XROM_HOST_TEST=1'], args=[path, sha1, 'NXTE'])
    print('test_xbe_rom:', 'FAILED' if rc else f'ok ({", ".join(w for w, _ in xbes)})')
    return rc


if __name__ == '__main__':
    sys.exit(main())
