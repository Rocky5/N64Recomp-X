#!/usr/bin/env python3
"""The dry boot's CPU on the host (tools/dryboot/dryboot.c, docs/roadmap.md
B2), no ROM: the self-checking test program make_test_rom.py assembles (a
loop with a delay slot, calls, a function pointer, a jump table, mult/div,
shifts and compares, byte/halfword/unaligned memory, single and double
floats with rounding-mode changes) run in the dry boot, whose result block
at 0x80100000 must say "NRX1" with no failures; and the learn files it
writes in the console's formats."""
import os
import pathlib
import struct
import subprocess
import sys
import tempfile

import hostcc

ROOT = hostcc.ROOT


def main():
    fails = []
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        rom = tmp / 'test.z64'
        if subprocess.run([sys.executable, str(ROOT / 'tools/xbox/make_test_rom.py'), str(rom)],
                          capture_output=True).returncode:
            print('test_dryboot: make_test_rom.py failed')
            return 1
        data = rom.read_bytes()
        if len(data) < 0x101000:   # the dry boot wants the 1 MB the IPL3 copies
            data += bytes(0x101000 - len(data))
            rom.write_bytes(data)
        entry = struct.unpack('>I', data[8:12])[0]
        exe = tmp / ('dryboot.exe' if os.name == 'nt' else 'dryboot')
        cc = hostcc.host_cc()
        cmd = [*cc, '-O2', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-o', str(exe), str(ROOT / 'tools/dryboot/dryboot.c')]
        cmd += ['-static'] if os.name == 'nt' else ['-lm']
        if subprocess.run(cmd).returncode:
            print('test_dryboot: FAILED (does not build)')
            return 1
        env = dict(os.environ)
        if os.name == 'nt':
            env['PATH'] = str(pathlib.Path(cc[0]).parent) + os.pathsep + env.get('PATH', '')
        out = tmp / 'out'
        out.mkdir()
        dump = tmp / 'result.bin'
        r = subprocess.run([str(exe), str(rom), str(out), '--entry', f'{entry:08x}', '--frames', '30',
                            '--dump', '80100000', '16', str(dump)], capture_output=True, text=True, env=env)
        if r.returncode:
            fails.append(f'exit {r.returncode}: {r.stderr.strip()[-200:]}')
        res = dump.read_bytes() if dump.exists() else b''
        if len(res) < 16 or res[:4] != b'NRX1':
            fails.append(f'no result block (magic {res[:4]!r})')
        else:
            bad, steps, last = struct.unpack('>III', res[4:16])
            if bad or steps < 10:
                fails.append(f'{bad} failures in {steps} steps (the last failing value {last:#x})')
        text = (out / 'learn.txt').read_text(encoding='utf-8') if (out / 'learn.txt').exists() else ''
        if not text.startswith('# n64rx dry boot') or '# 30 frames' not in text:
            fails.append('learn.txt: not the dry boot\'s, or the frame count missing')
        if not (out / 'learn_code.bin').exists():
            fails.append('no learn_code.bin')
    for f in fails:
        print(f'  FAIL {f}')
    print('test_dryboot: ' + ('FAILED' if fails else 'passed'))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
