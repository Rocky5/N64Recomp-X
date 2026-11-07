#!/usr/bin/env python3
"""sweep_diff.py - two sweeps side by side (docs/testing.md, the sweep).

  python tools/xbox/sweep_diff.py <old results.json> <new results.json> [--all]

Counts the games that present frames in each (of the games both ran) and
lists the ones that changed between presenting and not, with what each run
reached, and those still presenting at under half their frames per retrace
(two frames count as presenting: a game that played and now stops at its
second frame shows only here; a short run or a loaded host can do it too, so
rerun such a game alone before calling it lost); --all lists every game whose
category changed. Reads only the two results files
(build-host/sweep/<tag>/results.json: never tracked)."""
import argparse
import json
import pathlib
import sys


def cat(r):
    x = r.get('run')
    return (x['category'], x.get('presented', 0)) if isinstance(x, dict) else None


def rate(r):
    x = r['run']
    return x.get('presented', 0) / max(x.get('retrace', 0), 1)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('old')
    ap.add_argument('new')
    ap.add_argument('--all', action='store_true', help='every game whose category changed')
    a = ap.parse_args()
    old = json.loads(pathlib.Path(a.old).read_text(encoding='utf-8'))
    new = json.loads(pathlib.Path(a.new).read_text(encoding='utf-8'))
    both = sorted(k for k in new if k in old and cat(new[k]) and cat(old[k]))
    frames = lambda res, k: cat(res[k])[0] == 'frames'
    print(f'{len(both)} games run in both; presenting frames: '
          f'{sum(frames(old, k) for k in both)} -> {sum(frames(new, k) for k in both)}')
    gained = [k for k in both if frames(new, k) and not frames(old, k)]
    lost = [k for k in both if frames(old, k) and not frames(new, k)]
    for title, ks in (('now presenting', gained), ('no longer presenting', lost)):
        if ks:
            print(f'{title} ({len(ks)}):')
            for k in ks:
                print(f'  {k} {new[k]["title"][:30]:30} {cat(old[k])} -> {cat(new[k])}'
                      + (f' (learning rounds: {len(new[k]["rounds"])})' if new[k].get('rounds') else ''))
    fell = [k for k in both if frames(old, k) and frames(new, k) and cat(old[k])[1] > 50
            and rate(new[k]) < rate(old[k]) / 2]
    if fell:
        print(f'presenting at under half the rate ({len(fell)}):')
        for k in fell:
            print(f'  {k} {new[k]["title"][:30]:30} {cat(old[k])[1]} in {old[k]["run"].get("retrace", 0)} retraces -> '
                  f'{cat(new[k])[1]} in {new[k]["run"].get("retrace", 0)}')
    if a.all:
        ch = [k for k in both if cat(old[k])[0] != cat(new[k])[0] and k not in gained + lost]
        if ch:
            print(f'other category changes ({len(ch)}):')
            for k in ch:
                print(f'  {k} {new[k]["title"][:30]:30} {cat(old[k])[0]} -> {cat(new[k])[0]}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
