#!/usr/bin/env python3
"""The runtime's time on any console (PLAN.md 3.13): builds
tests/xbox/test_time.c with xbox/src/rt/ultra/time.c against a fake ACPI
clock, and scans the sources so nothing but the clock module reads the TSC
(rdtsc, __rdtsc, KeQueryPerformanceCounter, nxdk's QueryPerformance*):
an upgraded or overclocked CPU must change nothing but headroom."""
import re
import sys

import hostcc

ROOT = hostcc.ROOT
ALLOWED = {'xbox/src/hw/xhw_clock.c', 'xbox/src/hw/xhw_code_repair.c'}
TSC = re.compile(r'\brdtsc\b|__rdtsc|KeQueryPerformanceCounter|QueryPerformanceCounter|QueryPerformanceFrequency')


def scan():
    bad = []
    for p in sorted((ROOT / 'xbox').rglob('*.[ch]')):
        rel = p.relative_to(ROOT).as_posix()
        if rel in ALLOWED:
            continue
        for i, line in enumerate(p.read_text(encoding='utf-8', errors='replace').splitlines(), 1):
            code = line.split('//')[0]
            if TSC.search(code) and not code.lstrip().startswith(('*', '/*')):
                bad.append(f'{rel}:{i}: {line.strip()}')
    return bad


def main():
    bad = scan()
    for b in bad:
        print(f'  reads the TSC: {b}')
    rc = hostcc.run(['tests/xbox/test_time.c', 'xbox/src/rt/ultra/time.c'],
                    includes=['xbox/include', 'xbox/include/xrc', 'xbox/src/rt'],
                    flags=['-Wno-unused-function', '-include', str(ROOT / 'xbox/include/xrc/xrc_prelude.h')])
    print('test_time: ' + ('FAILED' if bad or rc else 'passed'))
    return 1 if bad or rc else 0


if __name__ == '__main__':
    sys.exit(main())
