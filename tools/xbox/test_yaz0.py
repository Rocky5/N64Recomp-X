#!/usr/bin/env python3
"""Yaz0 ROMs (tools/n64rx/n64rx_lib/yaz0.py) on a synthetic ROM: no game
bytes. A ROM with a Zelda-style DMA table (vrom start, vrom end, prom start,
prom end) whose files are stored as is or Yaz0-compressed; the table is
found by behaviour, every file lands at its vrom inflated, and the table
then says "not compressed"."""
import pathlib
import random
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/n64rx'))
from n64rx_lib import yaz0  # noqa: E402

fails = 0


def check(cond, what):
    global fails
    if not cond:
        fails += 1
        print(f'test_yaz0: FAIL {what}')


def compress(data):
    """a simple Yaz0 encoder: back-references to the longest match in the
    last 4 KB (greedy), else literals; every code path of the decoder"""
    out = bytearray(b'Yaz0' + struct.pack('>I', len(data)) + bytes(8))
    i = 0
    while i < len(data):
        code_at = len(out)
        out.append(0)
        code = 0
        for bit in range(7, -1, -1):
            if i >= len(data):
                break
            best_n, best_d = 0, 0
            for d in range(1, min(i, 0x1000) + 1):
                n = 0
                while n < 0x111 and i + n < len(data) and data[i + n] == data[i + n - d]:
                    n += 1
                if n > best_n:
                    best_n, best_d = n, d
                    if n == 0x111:
                        break
            if best_n >= 3:
                d = best_d - 1
                if best_n >= 0x12:
                    out += bytes([d >> 8, d & 0xFF, best_n - 0x12])
                else:
                    out += bytes([((best_n - 2) << 4) | (d >> 8), d & 0xFF])
                i += best_n
            else:
                code |= 1 << bit
                out.append(data[i])
                i += 1
        out[code_at] = code
    while len(out) % 16:
        out.append(0)
    return bytes(out)


def main():
    rng = random.Random(64)
    # round trips: random, runs, long runs (the 3-byte form), short repeats
    for name, data in (('random', bytes(rng.randrange(256) for _ in range(3000))),
                       ('runs', b'ab' * 900 + b'xyz' * 300),
                       ('long run', bytes(5000)),
                       ('text', b'the quick brown fox ' * 120)):
        check(yaz0.decompress(compress(data)) == data, f'round trip: {name}')
    check(yaz0.decompress(b'\0' * 4 + compress(b'hello hello hello'), 4) == b'hello hello hello', 'offset')

    # a ROM: boot (0..0x1060), the table (at 0x1060), then 30 files, half compressed
    files = []
    for k in range(30):
        n = 0x400 + rng.randrange(0x2000)
        body = bytes((k * 7 + (j // 3)) & 0xFF for j in range(n)) if k % 2 else bytes(rng.randrange(256) for _ in range(n))
        files.append(body)
    table_at = 0x1060
    nent = len(files) + 2
    vrom = table_at + ((nent * 16 + 0xF) & ~0xF)
    ents = [(0, 0x1060, 0, 0), (table_at, vrom, table_at, 0)]
    rom = bytearray(0x1060 + nent * 16)
    rom[0x40:0x44] = b'BOOT'
    prom = vrom
    stored = []
    for k, body in enumerate(files):
        vs, ve = vrom, vrom + len(body)
        vrom = (ve + 0xF) & ~0xF
        if k % 2:
            blob = compress(body)
            ents.append((vs, ve, prom, prom + len(blob)))
        else:
            blob = body + bytes((-len(body)) % 16)
            ents.append((vs, ve, prom, 0))
        stored.append((prom, blob))
        prom += len(blob)
    rom = bytearray(prom + 0x100)
    rom[0x40:0x44] = b'BOOT'
    for i, e in enumerate(ents):
        struct.pack_into('>IIII', rom, table_at + i * 16, *e)
    for at, blob in stored:
        rom[at:at + len(blob)] = blob
    rom = bytes(rom)

    found = yaz0.find_table(rom)
    check(found is not None and found[0] == table_at and len(found[1]) == len(ents), 'the table found by behaviour')
    out = yaz0.inflate_rom(rom)
    check(out is not None, 'inflated')
    if out:
        img, at, n = out
        check(at == table_at and n == len(files) // 2, 'table place and compressed count')
        check(len(img) % 0x100000 == 0, 'whole megabytes')
        check(img[0x40:0x44] == b'BOOT', 'the boot area kept')
        for k, body in enumerate(files):
            vs, ve, ps, pe = struct.unpack_from('>IIII', img, table_at + (k + 2) * 16)
            check(img[vs:ve] == body, f'file {k} at its vrom')
            check(ps == vs and pe == 0, f'file {k} table entry rewritten')
    check(yaz0.inflate_rom(bytes(0x200000)) is None, 'a ROM with no table is left alone')

    print(f'test_yaz0: {"passed" if not fails else f"{fails} failed"}')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
