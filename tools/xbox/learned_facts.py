#!/usr/bin/env python3
"""The facts a console's learning rounds found, for a pack-less game
(docs/roadmap.md B2; CLAUDE.md: packs hold facts, not bytes):
games/learned/<code>.<sha1[:12]>.txt holds the `func`, `dma`, `kuseg`,
`rdram 8` and `reloc` lines of learn files (addresses and sizes: never a
captured code record, which is the game's bytes), deduplicated. A ROM-only
build of the ROM with that SHA-1 reads it as if it were a --learn file
(pipeline.py; N64RX_LEARNED=0: not read), so a user's first build starts
where the console's rounds left off, for the code a DMA from the ROM put
there; code the CPU made comes from the dry boot (N64RX_DRYBOOT) or a
console round.

  python tools/xbox/learned_facts.py <rom> <learn file or serial log>...
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/n64rx'))
from n64rx_lib import rom as romlib  # noqa: E402

KEEP = re.compile(r'^(?:.*\[LEARN\]\s+)?((?:func|dma|kuseg|kuseg_versions|reloc)\s+[0-9A-Fa-fx ]+?(?:\s+from\s+[0-9A-Fa-fx]+)?|rdram\s+8)\s*$')


def facts(paths):
    out, seen = [], set()
    for p in paths:
        for line in pathlib.Path(p).read_text(encoding='utf-8', errors='replace').splitlines():
            m = KEEP.match(line.strip())
            if m and m.group(1) not in seen:
                seen.add(m.group(1))
                out.append(m.group(1))
    return out


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    rom = romlib.read(argv[1])
    lines = facts(argv[2:])
    out = ROOT / 'games' / 'learned' / f'{rom.code}.{rom.sha1[:12]}.txt'
    out.parent.mkdir(parents=True, exist_ok=True)
    head = [f'# {rom.title.strip()} ({rom.code}): what the console\'s learning rounds found (tools/xbox/learned_facts.py).',
            '# Facts only: function starts, the DMAs that put code there, the TLB regions. No code bytes.']
    out.write_text('\n'.join(head + lines) + '\n', encoding='utf-8', newline='\n')
    print(f'{out.relative_to(ROOT)}: {len(lines)} lines from {len(argv) - 2} files')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
