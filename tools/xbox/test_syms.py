#!/usr/bin/env python3
"""Every committed symbol file (games/*/*.syms.toml) against the rules the
runtime's function table relies on (PLAN.md 3.4, 4.6): it parses; functions
are 4-aligned, non-empty, inside their section and never overlap; sections
do not overlap in the ROM; every function start is in KSEG0's first 8 MB
(the page map's reach); the entrypoint function starts at ROM 0x1000; and
there are fewer than 65,535 functions (the table's index width)."""
import pathlib
import sys
import tomllib

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/xbox'))
import syms_tools  # noqa: E402


def main():
    bad = 0
    files = sorted(ROOT.glob('games/*/*.syms.toml'))
    if not files:
        sys.exit('test_syms: no symbol files')
    for f in files:
        n, errors = syms_tools.check(f)
        t = tomllib.loads(f.read_text(encoding='utf-8'))
        game = tomllib.loads((f.parent / 'game.toml').read_text(encoding='utf-8'))
        entry = int(str(game.get('cpu', {}).get('entrypoint', 0)), 0)
        for s in t.get('section', []):
            for fn in s.get('functions', []):
                if not 0x80000000 <= fn['vram'] < 0x80800000:
                    errors.append(f"{fn['name']} at {fn['vram']:08X}: outside the table's 8 MB of KSEG0")
            if s['vram'] <= entry < s['vram'] + s['size'] and s['rom'] + (entry - s['vram']) != 0x1000:
                errors.append(f'the entrypoint {entry:08X} is not at ROM 0x1000')
        if n >= 65535:
            errors.append(f'{n} functions: more than the table holds')
        for e in errors[:20]:
            print(f'  {f.relative_to(ROOT)}: {e}')
        print(f'{f.relative_to(ROOT)}: {n} functions, {len(errors)} problem(s)')
        bad += len(errors)
    print('test_syms: ' + ('FAILED' if bad else 'passed'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
