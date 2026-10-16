#!/usr/bin/env python3
"""The ROM analyser against every decomp + ROM pair present (PLAN.md 6 "V"):
tools/xbox/decomp_check.py per pair, failing on any wrong libultra name the
runtime replaces, on a function the decomp's names keep live that the
analysis leaves dead, or on a result below the pair's floor (the last
results, raised as the analyser improves). Pairs whose ROM or decomp is
missing are skipped: CI has no ROM, so there it checks nothing.

  N64RX_ROMS      folder with the ROMs (default build-host/roms)
  N64RX_DECOMPS   folder with the decomp checkouts (default build-host/decomps)
"""
import os
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/xbox'))
import decomp_check   # noqa: E402

ROMS = pathlib.Path(os.environ.get('N64RX_ROMS', ROOT / 'build-host/roms'))
DECOMPS = pathlib.Path(os.environ.get('N64RX_DECOMPS', ROOT / 'build-host/decomps'))

# (name, ROM file names tried, key: path relative to the tree (@) or the decomps folder, floors)
PAIRS = [
    ('pw64', ['Pilotwings 64 (U) [!].z64', 'Pilotwings 64 (USA).z64'], '@games/pw64/pw64.us.syms.toml',
     {'found': 1952, 'libultra_right': 75}),
    ('pm', ['Paper Mario (U) [!].v64', 'Paper Mario (USA).z64'], 'papermario/ver/us',
     {'found': 2443, 'libultra_right': 56}),
    ('sm64', ['Super Mario 64 (U) [!].v64', 'Super Mario 64 (USA).z64'], 'sm64',
     {'placed': 139, 'libultra_right': 66}),
    ('mk64', ['Mario Kart 64 (U) [!].v64', 'Mario Kart 64 (USA).z64'], 'mk64',
     {'placed': 3164, 'libultra_right': 66}),
    ('snap', ['Pokemon Snap (U) [!].v64', 'Pokemon Snap (USA).z64'], 'pokemonsnap',
     {'found': 670, 'libultra_right': 75}),
    ('drmario64', ['Dr. Mario 64 (U) [!].v64', 'Dr. Mario 64 (USA).z64'], 'drmario64/config/us',
     {'found': 262, 'libultra_right': 56}),
]


def main():
    only = set(sys.argv[1:])
    ran, fails = 0, []
    for name, roms, key, floor in PAIRS:
        if only and name not in only:
            continue
        rom = next((ROMS / r for r in roms if (ROMS / r).is_file()), None)
        kp = ROOT / key[1:] if key.startswith('@') else DECOMPS / key
        if rom is None or not kp.exists():
            print(f'{name}: skipped ({"no ROM" if rom is None else "no decomp"})')
            continue
        r = decomp_check.compare(rom, kp)
        ran += 1
        print(decomp_check.summary(r))
        for x in r['libultra_wrong']:
            fails.append(f'{name}: wrong libultra name {x}')
        for x in r['dead_but_live'][:20]:
            fails.append(f'{name}: live by the decomp\'s names, dead in the analysis: {x}')
        for k, v in floor.items():
            if r[k] < v:
                fails.append(f'{name}: {k} {r[k]} is below its floor {v}')
    for f in fails:
        print('FAIL', f)
    print(f'{ran} pair(s) checked, {len(fails)} failure(s)')
    sys.exit(1 if fails else 0)


if __name__ == '__main__':
    main()
