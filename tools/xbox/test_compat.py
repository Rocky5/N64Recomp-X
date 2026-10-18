#!/usr/bin/env python3
"""docs/compat.md's tiers against n64rx's table (tools/n64rx/n64rx_lib/
compat.py): every Gold and Silver row of the doc is in the table with the
same tier and name, the table holds nothing else, and an Experimental row
is not in it (n64rx calls any ROM it does not list Experimental)."""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/n64rx'))
from n64rx_lib import compat  # noqa: E402


def rows():
    """(tier, name, code, revision byte) of every row with a ROM"""
    out = []
    for line in (ROOT / 'docs/compat.md').read_text(encoding='utf-8').splitlines():
        cells = [c.strip() for c in line.strip().strip('|').split('|')]
        if len(cells) < 5:
            continue
        m = re.fullmatch(r'`([A-Z0-9]{4})` 1\.(\d+)', cells[2])
        if not m:
            continue
        out.append((cells[0].strip('*'), cells[1], m.group(1), int(m.group(2))))
    return out


def main():
    fails = []
    doc = rows()
    if len(doc) < 10:
        fails.append(f'only {len(doc)} rows with a ROM in docs/compat.md')
    seen = set()
    for tier, name, code, ver in doc:
        got = compat.TIERS.get((code, ver))
        if tier in (compat.GOLD, compat.SILVER):
            seen.add((code, ver))
            if got != (tier, name):
                fails.append(f'{code} 1.{ver}: docs/compat.md says {tier} "{name}", n64rx says {got}')
        elif tier == compat.EXPERIMENTAL:
            if got is not None:
                fails.append(f'{code} 1.{ver}: Experimental in docs/compat.md, {got[0]} in n64rx')
        else:
            fails.append(f'{code}: unknown tier {tier!r}')
    for key in compat.TIERS:
        if key not in seen:
            fails.append(f'{key[0]} 1.{key[1]}: in n64rx\'s table, not a Gold or Silver row of docs/compat.md')
    if compat.tier_of('ZZZZ', 0)[0] != compat.EXPERIMENTAL:
        fails.append('an unknown ROM is not Experimental')
    for f in fails:
        print('FAIL', f)
    print(f'test_compat: {len(doc)} rows, {len(fails)} failed')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
