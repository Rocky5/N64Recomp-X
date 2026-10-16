#!/usr/bin/env python3
"""Build and run tests/xbox/test_pad_combo.c on the host: the exit-combo
detector (xbox/src/hw/xhw_combo.c's core, PLAN.md 3.9). CC picks the
compiler (tools/xbox/hostcc.py)."""
import sys

import hostcc

if __name__ == '__main__':
    sys.exit(hostcc.run(['tests/xbox/test_pad_combo.c', 'xbox/src/hw/xhw_combo.c'],
                        includes=['xbox/include', 'xbox/src/hw'], defines=['XHW_COMBO_HOST=1']))
