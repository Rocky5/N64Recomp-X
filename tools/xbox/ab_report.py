#!/usr/bin/env python3
"""Compare boots by their [PERF] / [PERF2] lines (the M3 gate, docs/roadmap.md
B6; PLAN.md 3.11): each serial log's lines after the first (the boot's
warm-up) averaged, one column per log, the change against the first log,
and whether two runs of one build agree within 3 % (PLAN.md's rule for a
number to count).

  python tools/xbox/ab_report.py base.log restrict.log [more.log ...] [--skip N]

xemu is not hardware: only a console's logs say what a change is worth."""
import argparse
import pathlib
import re
import sys

NUM = r'(\d+(?:\.\d+)?)'
FIELDS = [   # (label, line tag, regex)
    ('fps', 'PERF', rf'\[PERF\] {NUM} fps'),
    ('ms a frame', 'PERF', rf'fps, {NUM} ms a frame'),
    ('display lists', 'PERF', rf'display lists {NUM}'),
    ('textures', 'PERF', rf'\(textures {NUM}\)'),
    ('present', 'PERF', rf'present {NUM}'),
    ('GPU/flip waits in present', 'PERF', rf'GPU and flip waits {NUM}'),
    ('audio', 'PERF', rf'audio {NUM}'),
    ('waits at frame start', 'PERF', rf"waits at the frame's start {NUM}"),
    ('the rest (game, idle)', 'PERF', rf'the rest {NUM}'),
    ('GPU waits in lists', 'PERF2', rf'inside display lists {NUM}'),
    ('interpreter', 'PERF2', rf'the interpreter {NUM}'),
    ('texture hashing', 'PERF2', rf'texture hashing {NUM}'),
    ('lock takes', 'PERF2', rf'{NUM} lock takes'),
    ('ints-off level changes', 'PERF2', rf'{NUM} interrupts-off'),
]


def averages(path, skip):
    text = pathlib.Path(path).read_text(encoding='utf-8', errors='replace')
    out = {}
    for label, tag, rx in FIELDS:
        vals = [float(m.group(1)) for line in text.splitlines() if f'[{tag}]' in line
                for m in [re.search(rx, line)] if m]
        vals = vals[skip:]
        out[label] = (sum(vals) / len(vals), len(vals)) if vals else None
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('logs', nargs='+')
    ap.add_argument('--skip', type=int, default=1, help='the first N lines of each kind left out (warm-up)')
    a = ap.parse_args()
    cols = [averages(p, a.skip) for p in a.logs]
    names = [pathlib.Path(p).stem[:16] for p in a.logs]
    print(f"{'':28}" + ''.join(f'{n:>18}' for n in names))
    for label, _tag, _rx in FIELDS:
        if all(c[label] is None for c in cols):
            continue
        row = f'{label:28}'
        base = cols[0][label][0] if cols[0][label] else None
        for c in cols:
            v = c[label]
            if v is None:
                row += f"{'-':>18}"
                continue
            d = f' ({(v[0] - base) / base * 100:+.1f}%)' if base and c is not cols[0] else ''
            row += f'{v[0]:>10.1f}{d:>8}'
        print(row)
    ms = [c['ms a frame'][0] for c in cols if c['ms a frame']]
    if len(ms) >= 2:
        spread = (max(ms) - min(ms)) / min(ms) * 100
        print(f'\nframe time spread across the logs: {spread:.1f}% '
              f'({"within" if spread <= 3 else "past"} the 3 % two runs of one build must agree to)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
