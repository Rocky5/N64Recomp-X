#!/usr/bin/env python3
"""The ROM page cache's core (PLAN.md 3.5, xbox/src/hw/xhw_romcache.c) on the
host: tests/xbox/test_romcache.c drives it with a fake file and a fake lazy
reservation (chunk boundaries, spanning DMAs, word reads, the LRU, pins,
failed fills, the counters, per-chunk normalisation, a random run). The
"ROM" is a synthetic pattern, never a game's."""
import sys

import hostcc


def main():
    rc = hostcc.run(['tests/xbox/test_romcache.c', 'xbox/src/hw/xhw_romcache.c', 'xbox/src/hw/xhw_rom.c',
                     'xbox/src/hw/xhw_sha1.c'], includes=['xbox/include'], defines=['XROM_HOST_TEST=1'])
    return rc


if __name__ == '__main__':
    sys.exit(main())
