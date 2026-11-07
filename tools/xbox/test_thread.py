#!/usr/bin/env python3
"""The thread layer on the host (xbox/src/rt/ultra/thread.c; PLAN.md 3.3,
docs/roadmap.md B1): tests/xbox/test_thread.c on pthreads, built twice.
With the run token (-DXRC_RUN_TOKEN=1): libultra's order (a higher thread
started or woken runs at once, a block runs the next highest, osYieldThread
goes behind its equals), one game thread in game code at a time, a host wait
inside game code keeping the turn, an interrupt that waits for interrupts on.
Without the token also with -DXRC_START_DEFER=0 (a thread started runs at
once). All builds: a thread stopped with interrupts off lets the lock go (review
5b.5), osSetThreadPri inside a critical section keeps the boost (5b.7);
without the token, a polling higher thread on the starter's level no longer
keeps it in osStartThread for good (5b.6)."""
import sys

import hostcc

ROOT = hostcc.ROOT


def main():
    bad = 0
    for defines in (['XRC_RUN_TOKEN=1'], ['XRC_RUN_TOKEN=0'], ['XRC_RUN_TOKEN=0', 'XRC_START_DEFER=0']):
        rc = hostcc.run(['tests/xbox/test_thread.c', 'xbox/src/rt/ultra/thread.c'],
                        includes=['xbox/include', 'xbox/include/xrc', 'xbox/src/rt', 'xbox/src/rt/ultra'],
                        defines=defines,
                        flags=['-Wno-unused-function', '-pthread', '-include', str(ROOT / 'xbox/include/xrc/xrc_prelude.h')],
                        args=sys.argv[1:])
        bad += rc != 0
    print('test_thread: ' + ('FAILED' if bad else 'passed'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
