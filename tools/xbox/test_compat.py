#!/usr/bin/env python3
"""docs/compat.md's tiers against n64rx's table (tools/n64rx/n64rx_lib/
compat.py): every row of the doc with a ROM is in the table with the same
tier and name, the table holds nothing else, a Gold row's known problems
(its minor issues, maybe none) match the doc's, every other row lists at
least one (the row's "keeps it from the next tier" in a player's words),
and an unlisted ROM is Experimental."""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/n64rx'))
from n64rx_lib import compat  # noqa: E402


def rows():
    """(tier, name, code, revision byte, keeps it from the next tier) of every row with a ROM"""
    out = []
    for line in (ROOT / 'docs/compat.md').read_text(encoding='utf-8').splitlines():
        cells = [c.strip() for c in line.strip().strip('|').split('|')]
        if len(cells) < 5:
            continue
        m = re.fullmatch(r'`([A-Z0-9]{4})` 1\.(\d+)', cells[2])
        if not m:
            continue
        out.append((cells[0].strip('*'), cells[1], m.group(1), int(m.group(2)), cells[4]))
    return out


def main():
    fails = []
    doc = rows()
    if len(doc) < 10:
        fails.append(f'only {len(doc)} rows with a ROM in docs/compat.md')
    seen = set()
    for tier, name, code, ver, keeps in doc:
        got = compat.TIERS.get((code, ver))
        seen.add((code, ver))
        if tier not in compat.MEANING:
            fails.append(f'{code}: unknown tier {tier!r}')
        elif got is None or got[:2] != (tier, name):
            fails.append(f'{code} 1.{ver}: docs/compat.md says {tier} "{name}", n64rx says {got and got[:2]}')
        elif tier == compat.GOLD and bool(got[2]) != (keeps != '-'):
            fails.append(f'{code} 1.{ver}: Gold with minor issues in one of docs/compat.md and n64rx, not the other')
        elif tier != compat.GOLD and not got[2]:
            fails.append(f'{code} 1.{ver}: {tier} with no known problem in n64rx (docs/compat.md: {keeps})')
    for key in compat.TIERS:
        if key not in seen:
            fails.append(f'{key[0]} 1.{key[1]}: in n64rx\'s table, not a row of docs/compat.md')
    if compat.tier_of('ZZZZ', 0)[0] != compat.EXPERIMENTAL:
        fails.append('an unknown ROM is not Experimental')
    r = compat.report('NKTP', 0)   # Mario Kart 64 (Europe): not listed, the US row named
    if r['tier'] != compat.EXPERIMENTAL or r['issues'] or not (r['other_version'] or '').startswith('Mario Kart 64'):
        fails.append(f'another region of a listed game: {r}')
    if compat.report('NKTE', 0)['issues'] != list(compat.TIERS[('NKTE', 0)][2]):
        fails.append('report() does not carry the known problems')
    for f in fails:
        print('FAIL', f)
    print(f'test_compat: {len(doc)} rows, {len(fails)} failed')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
