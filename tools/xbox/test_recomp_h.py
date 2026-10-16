#!/usr/bin/env python3
"""Our recomp.h against upstream's (PLAN.md 3.2, 4.6): builds
tests/xbox/test_recomp_h.c four ways and compares what each prints.

  upstream  tools/recomp/include/recomp.h, a 64-bit host (it needs __int128)
  ours64    xbox/include/xrc/recomp.h, the same host
  ours32    ours on i686 with the Pentium III's float setup (-march=pentium3
            -msse -mfpmath=sse: SSE1 floats, x87 doubles), when a 32-bit
            host compiler is there (CC32, gcc -m32, MSYS2's mingw32 gcc)
  gpr32     ours32 with -DXRC_GPR32=1: must agree with ours on every line
            whose inputs are sign-extended 32-bit values ("32 ..." lines)

Every line of upstream must equal ours64's and ours32's; every "32" line of
ours32 must equal gpr32's. A missing 32-bit compiler is reported, not
failed (CI installs one)."""
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

import hostcc

ROOT = hostcc.ROOT
SRC = ROOT / 'tests/xbox/test_recomp_h.c'
UP = ROOT / 'tools/recomp/include'
OURS = ROOT / 'xbox/include/xrc'
FLAGS = ['-O2', '-std=gnu11', '-fno-strict-aliasing', '-fwrapv', '-ffp-contract=off', '-w']


def cc32():
    if os.environ.get('CC32'):
        return [os.environ['CC32']]
    if os.name == 'nt':
        for c in ('C:/msys64/mingw32/bin/gcc.exe',):
            if pathlib.Path(c).exists():
                return [c]
        return None
    for c in ('gcc', 'cc', 'clang'):
        p = shutil.which(c)
        if p:
            with tempfile.TemporaryDirectory() as t:
                src = pathlib.Path(t) / 'a.c'
                src.write_text('int main(void){return 0;}')
                if subprocess.run([p, '-m32', str(src), '-o', str(pathlib.Path(t) / 'a')], capture_output=True).returncode == 0:
                    return [p, '-m32']
    return None


def run(cc, include, extra, tmp, name):
    exe = pathlib.Path(tmp) / (name + ('.exe' if os.name == 'nt' else ''))
    cmd = [*cc, *FLAGS, *extra, f'-I{include}', str(SRC), '-o', str(exe)]
    if os.name == 'nt':
        cmd.append('-static')
    else:
        cmd.append('-lm')
    env = dict(os.environ)   # MinGW's compilers need their own bin for their DLLs
    env['PATH'] = str(pathlib.Path(cc[0]).parent) + os.pathsep + env.get('PATH', '')
    r = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if r.returncode:
        sys.exit(f'{name}: build failed ({r.returncode})\n{r.stdout[-2000:]}{r.stderr[-3000:]}')
    out = subprocess.run([str(exe)], capture_output=True, text=True, env=env)
    if out.returncode:
        sys.exit(f'{name}: exited {out.returncode}\n{out.stderr[-2000:]}')
    return out.stdout.splitlines()


def compare(a_name, a, b_name, b, only32=False):
    if only32:
        a = [l for l in a if l.startswith('32 ')]
        b = [l for l in b if l.startswith('32 ')]
    bad = 0
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            if bad < 10:
                print(f'  line {i}: {a_name}: {x}\n          {b_name}: {y}')
            bad += 1
    if len(a) != len(b):
        print(f'  {a_name} has {len(a)} lines, {b_name} {len(b)}')
        bad += 1
    print(f'{a_name} vs {b_name}: {min(len(a), len(b))} lines, {bad} differ')
    return bad


def main():
    if not (UP / 'recomp.h').exists():
        sys.exit('no tools/recomp/include/recomp.h: git submodule update --init')
    cc = hostcc.host_cc()
    bad = 0
    with tempfile.TemporaryDirectory() as tmp:
        up = run(cc, UP, ['-DXRC_TEST_UPSTREAM=1'], tmp, 'upstream')
        ours = run(cc, OURS, [], tmp, 'ours64')
        bad += compare('upstream', up, 'ours64', ours)
        c32 = cc32()
        if not c32:
            print('no 32-bit host compiler (set CC32): ours32 and gpr32 not run')
        else:
            p3 = ['-march=pentium3', '-msse', '-mfpmath=sse']
            o32 = run(c32, OURS, p3, tmp, 'ours32')
            g32 = run(c32, OURS, p3 + ['-DXRC_GPR32=1'], tmp, 'gpr32')
            bad += compare('upstream', up, 'ours32', o32)
            bad += compare('ours32', o32, 'gpr32', g32, only32=True)
    print('test_recomp_h: ' + ('FAILED' if bad else 'passed'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
