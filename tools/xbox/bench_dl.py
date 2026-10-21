#!/usr/bin/env python3
"""One graphics task replayed on the host, timed and profiled
(tests/xbox/bench_dl.c, docs/testing.md).

    bench_dl.py <serial.log> [--runs N] [--top N] [-O2|-O1]

The log is a test build's, made with -DXRC_DLDUMP=<s> (rt/gfx.c): the
first graphics task after <s> seconds and the whole RDRAM image. The
interpreter runs that list N times over a counting back end; on Windows a
sampler takes the thread's pc every millisecond and the top functions are
listed with their share. The RDRAM image is the game's (ROM-derived): it is
written to a temporary folder only and never kept."""
import argparse
import base64
import collections
import os
import pathlib
import subprocess
import sys
import tempfile
import zlib

import hostcc

ROOT = hostcc.ROOT


def blobs(log):
    """[(task, ram)]: each [DLTASK] (plain) with the RDRAM image it ran over:
    the [DLRAM] after it (deflated), or the image before it with the pages of
    the [DLPAGES] after it (deflated: u32 index, 4096 bytes each)"""
    out, cur, parts, z = [], None, [], False
    with open(log, 'rb') as f:
        for raw in f:
            line = raw.decode('latin-1').rstrip('\r\n')
            for tag in ('DLTASK', 'DLRAM', 'DLPAGES'):
                p = f'[{tag}] '
                if not line.startswith(p):
                    continue
                rest = line[len(p):]
                if rest.startswith('BEGIN') or rest.startswith('ZBEGIN'):
                    cur, parts, z = tag, [], rest.startswith('Z')
                elif rest == 'END' and cur == tag:
                    data = base64.b64decode(''.join(parts))
                    data = zlib.decompress(data) if z else data
                    if tag == 'DLTASK':
                        out.append([data, None])
                    elif tag == 'DLRAM' and out and out[-1][1] is None:
                        out[-1][1] = data
                    elif tag == 'DLPAGES' and len(out) > 1 and out[-1][1] is None and out[-2][1] is not None:
                        ram = bytearray(out[-2][1])
                        for k in range(0, len(data) - 4099, 4100):
                            i = int.from_bytes(data[k:k + 4], 'little')
                            ram[i * 4096:(i + 1) * 4096] = data[k + 4:k + 4100]
                        out[-1][1] = bytes(ram)
                    cur = None
                elif cur == tag:
                    parts.append(rest)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log')
    ap.add_argument('--runs', type=int, default=200)
    ap.add_argument('--tasks', type=int, default=16, help='replay at most this many of the dumped tasks, in order')
    ap.add_argument('--top', type=int, default=30)
    ap.add_argument('--opt', default='-O2')
    ap.add_argument('--define', '-D', action='append', default=[])
    ap.add_argument('--png', help='the first run drawn in software, written here (keep it out of the tree: a game frame)')
    ap.add_argument('--draws', action='store_true', help='every draw of the first run as a [BDRAW] line')
    ap.add_argument('--trace', type=int, help="the interpreter's [DRAW] trace of task N (each task presented as a frame)")
    a = ap.parse_args()
    b = [p for p in blobs(a.log) if p[1] is not None][:a.tasks]
    if not b:
        sys.exit('bench_dl: no [DLTASK]/[DLRAM] dump in the log (build with -DXRC_DLDUMP=<seconds>)')
    cc = hostcc.host_cc()
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        for k, (task, ram) in enumerate(b):
            (tmp / f'task{k}.bin').write_bytes(task)
            (tmp / f'ram{k}.bin').write_bytes(ram)
        exe = tmp / ('bench.exe' if os.name == 'nt' else 'bench')
        srcs = ['tests/xbox/bench_dl.c', 'xbox/src/gfx/f3d_interp.c', 'xbox/src/gfx/texconv.c',
                'xbox/src/gfx/ucode.c', 'xbox/src/gfx/fbfx.c']
        cmd = [*cc, *a.opt.split(), '-g', '-std=gnu11', '-fno-strict-aliasing', '-fwrapv', '-ffp-contract=off',
               '-Wno-unused-function', '-Wno-unused-variable', '-include', str(ROOT / 'xbox/include/xrc/xrc_prelude.h'),
               '-o', str(exe), *[str(ROOT / s) for s in srcs],
               *[f'-I{ROOT / i}' for i in ('xbox/include', 'xbox/include/xrc', 'xbox/src/rt', 'xbox/src/gfx')],
               '-DXGFX_FB_READBACK=1', *[f'-D{d}' for d in a.define]]
        if a.trace is not None:
            cmd += ['-DBENCH_PRESENT_EACH=1', f'-DXGFX_TRACE_FRAME={a.trace}']
        cmd += ['-static', '-lwinmm'] if os.name == 'nt' else ['-lm']
        r = subprocess.run(cmd)
        if r.returncode:
            return r.returncode
        env = dict(os.environ)
        if os.name == 'nt':
            env['PATH'] = str(pathlib.Path(cc[0]).parent) + os.pathsep + env.get('PATH', '')
        samples = tmp / 'samples.txt'
        ppm = tmp / 'pic.ppm'
        r = subprocess.run([str(exe), str(tmp), str(len(b)), str(a.runs), str(samples), str(ppm),
                            *(['draws'] if a.draws else [])], env=env)
        if a.png and ppm.exists():
            from PIL import Image
            Image.open(ppm).save(a.png)
        if r.returncode or not samples.exists():
            return r.returncode
        lines = samples.read_text().split()
        base, pcs = int(lines[1], 16), [int(x, 16) for x in lines[2:]]
        nm = pathlib.Path(cc[0]).parent / ('nm.exe' if os.name == 'nt' else 'nm')
        syms = []
        for ln in subprocess.run([str(nm), '-n', '-C', str(exe)], capture_output=True, text=True).stdout.splitlines():
            p = ln.split(None, 2)
            if len(p) == 3 and p[1] in 'tT':
                syms.append((int(p[0], 16), p[2]))
        import bisect
        addrs = [s[0] for s in syms]
        # the image may be loaded at another base than the link one
        link_base = 0x140000000 if os.name == 'nt' else 0
        hist = collections.Counter()
        for pc in pcs:
            v = pc - base + link_base
            i = bisect.bisect_right(addrs, v) - 1
            inside = 0 <= i < len(syms) - 1 or (i == len(syms) - 1 and v - addrs[i] < 0x10000)
            hist[syms[i][1] if i >= 0 and inside else '(outside the program: the C library, the system)'] += 1
        n = max(1, len(pcs))
        print(f'[BENCH] {len(pcs)} samples')
        for name, c in hist.most_common(a.top):
            print(f'  {100.0 * c / n:5.1f}%  {name}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
