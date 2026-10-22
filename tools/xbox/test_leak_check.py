#!/usr/bin/env python3
"""rom_leak_check.py against made-up offenders (docs/review-2026-10.md 3.2):
a fake 4 KB block that starts with the N64 header's first 8 bytes (no ROM:
the bytes are the format's magic and a clock rate), zipped, gzipped,
shifted by a byte, as hex and as base64 in each alignment; generated code
under tests/ (the recompiler's comments); save and learning files; an
image nobody listed; an SVG with a raster inside; a large binary. Each must
be caught, and an ordinary file must pass."""
import base64
import gzip
import io
import sys
import zipfile

import rom_leak_check as rlc

HEAD = b"\x80\x37\x12\x40\x00\x00\x00\x0f"


def main():
    fails = []

    def want(rel, data, what):
        if not rlc.scan(rel, data, {}):
            fails.append(f'not caught: {what}')

    block = HEAD + bytes(4096 - len(HEAD))
    z = io.BytesIO()
    with zipfile.ZipFile(z, 'w') as zf:
        zf.writestr('game.z64', block)
    want('docs/thing.dat', z.getvalue(), 'a zipped ROM block')
    want('docs/thing.dat', gzip.compress(block), 'a gzipped ROM block')
    want('docs/thing.dat', b'\x00' + block, 'a ROM block shifted by a byte')
    want('docs/notes.md', b'look: ' + HEAD.hex().encode() + b'\n', 'the header as hex text')
    for shift in range(3):
        b64 = base64.b64encode(b'x' * shift + block).decode()
        want('docs/notes.md', b64.encode(), f'the header as base64 (alignment {shift})')
    v64 = bytes(b for i in range(0, len(HEAD), 2) for b in (HEAD[i + 1], HEAD[i]))
    want('docs/notes.md', base64.b64encode(b'yy' + v64 + bytes(16)), 'the v64 header as base64')
    gen = b''.join(b'    // 0x%08X: addiu       $sp, $sp, -0x18\n    ctx->r29 = ADD32(ctx->r29, -0X18);\n' % (0x80000400 + 4 * i)
                   for i in range(30))
    want('tests/xbox/funcs_0.c', gen, 'generated code under tests/')
    want('tests/xbox/funcs_0.h', b'void f(uint8_t* rdram, recomp_context *ctx);\n' + gen, 'generated code in a header')
    for name in ('saves/game.srm', 'saves/game.sav', 'build-x/learn_code.bin', 'pack/analysis.cache'):
        want(name, b'\x01\x02', name)
    want('docs/look/frame.png', b'\x89PNG\r\n\x1a\nrest', 'an image not listed')
    want('docs/look/logo/x.svg', b'<svg><image href="data:image/png;base64,AAAA"/></svg>', 'an SVG with a raster')
    want('docs/blob.dat', bytes([1]) * (1 << 20) + b'\0', 'a 1 MB binary')
    clean = rlc.scan('docs/notes.md', b'# Notes\n\nThe header magic is a word; nothing here.\n', {})
    if clean:
        fails.append(f'an ordinary file was caught: {clean}')
    png = b'\x89PNG\r\n\x1a\nlisted'
    import hashlib
    if rlc.scan('docs/look/ok.png', png, {'docs/look/ok.png': hashlib.sha256(png).hexdigest()}):
        fails.append('a listed image was caught')
    for f in fails:
        print(f'  FAIL {f}')
    print('test_leak_check: ' + ('FAILED' if fails else 'passed'))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
