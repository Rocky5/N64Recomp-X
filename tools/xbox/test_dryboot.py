#!/usr/bin/env python3
"""The dry boot's CPU on the host (tools/dryboot/dryboot.c, docs/roadmap.md
B2), no ROM: the self-checking test program make_test_rom.py assembles (a
loop with a delay slot, calls, a function pointer, a jump table, mult/div,
shifts and compares, byte/halfword/unaligned memory, single and double
floats with rounding-mode changes) run in the dry boot, whose result block
at 0x80100000 must say "NRX1" with no failures; and the learn files it
writes in the console's formats; and a raw program that runs the FPU with
CU1 clear, which must trap as the VR4300 does."""
import os
import pathlib
import struct
import subprocess
import sys
import tempfile

import hostcc

ROOT = hostcc.ROOT


def _r(fn, rs=0, rt=0, rd=0, sa=0):
    return (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | fn


def _i(op, rs, rt, imm):
    return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)


def cpu_trap_rom():
    """the FPU with CU1 clear must trap (coprocessor unusable, CE 1) before it runs, as libultra's lazy FP
    save relies on: a handler copied to 0x80000180 sets CU1 and counts; mtc1 then runs again. Result block
    at 0x80100000: "NRX1", the value mfc1 read back, the handler's Cause, the trap count"""
    T0, T1, T2, T3, T4, T5, T6, T7, S0, S1, S2, S3, K0, K1 = 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 26, 27
    mtc0 = lambda rt, rd: 0x40800000 | (rt << 16) | (rd << 11)   # noqa: E731
    mfc0 = lambda rt, rd: 0x40000000 | (rt << 16) | (rd << 11)   # noqa: E731
    code = [
        _i(15, 0, T0, 0x8000), _i(9, T0, T1, 0x500), _i(9, T0, T2, 0x180), _i(9, 0, T3, 8),
        _i(35, T1, T4, 0), _i(43, T2, T4, 0), _i(9, T1, T1, 4), _i(9, T2, T2, 4),   # the handler to 0x180
        _i(9, T3, T3, -1), _i(5, T3, 0, -6), 0,
        _i(9, 0, S3, 0), _i(15, 0, T5, 0x0400), mtc0(T5, 12), 0,                     # SR: FR only, CU1 clear
        _i(9, 0, S0, 0x1234), 0x44800000 | (S0 << 16) | (2 << 11),                    # mtc1 s0, f2: traps
        0x44000000 | (S1 << 16) | (2 << 11),                                          # mfc1 s1, f2
        _i(15, 0, T6, 0x8010), _i(43, T6, S1, 4), _i(43, T6, S2, 8), _i(43, T6, S3, 12),
        _i(15, 0, T7, 0x4E52), _i(13, T7, T7, 0x5831), _i(43, T6, T7, 0),
    ]
    spin = 0x80000400 + 4 * len(code)
    code += [(2 << 26) | ((spin >> 2) & 0x3FFFFFF), 0]
    code += [0] * (0x40 - len(code))
    code += [mfc0(S2, 13), mfc0(K0, 12), _i(15, 0, K1, 0x2000), _r(37, K0, K1, K0), mtc0(K0, 12),
             _i(9, S3, S3, 1), 0, 0x42000018]                                          # the handler at +0x100
    head = bytearray(0x1000)
    head[0:4] = b'\x80\x37\x12\x40'
    head[8:12] = struct.pack('>I', 0x80000400)
    return bytes(head) + b''.join(struct.pack('>I', w & 0xFFFFFFFF) for w in code) + bytes(0x100000)


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
        trap = tmp / 'trap.z64'
        trap.write_bytes(cpu_trap_rom())
        out2 = tmp / 'out2'
        out2.mkdir()
        dump2 = tmp / 'trap.bin'
        r = subprocess.run([str(exe), str(trap), str(out2), '--entry', '80000400', '--frames', '5',
                            '--dump', '80100000', '16', str(dump2)], capture_output=True, text=True, env=env)
        res = dump2.read_bytes() if dump2.exists() else b''
        if len(res) < 16 or res[:4] != b'NRX1':
            fails.append(f'the CU1 trap program did not finish (magic {res[:4]!r}, exit {r.returncode})')
        else:
            val, cause, n = struct.unpack('>III', res[4:16])
            if val != 0x1234 or cause & 0x3000007C != 0x1000002C or n != 1:
                fails.append(f'the CU1 trap: mfc1 read {val:#x}, Cause {cause:#x}, {n} traps (want 0x1234, CE 1 code 11, 1)')
    for f in fails:
        print(f'  FAIL {f}')
    print('test_dryboot: ' + ('FAILED' if fails else 'passed'))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
