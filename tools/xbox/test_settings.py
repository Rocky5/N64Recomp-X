#!/usr/bin/env python3
"""Build and run tests/xbox/test_settings.c on the host: settings.ini's
parse and format (xbox/src/rt/settings.c, PLAN.md 5.2)."""
import sys

import hostcc

if __name__ == '__main__':
    sys.exit(hostcc.run(['tests/xbox/test_settings.c', 'xbox/src/rt/settings.c'],
                        includes=['xbox/include', 'xbox/src/rt'], defines=['XRC_SETTINGS_HOST=1', 'XHW_TEST_BUILD=0']))
