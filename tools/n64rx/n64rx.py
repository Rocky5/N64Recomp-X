#!/usr/bin/env python3
"""n64rx: build N64 games for the original Xbox from your own ROM (PLAN.md 5.1).
Run it from a checkout: python tools/n64rx/n64rx.py (or ./n64rx, n64rx.cmd)."""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from n64rx_lib.cli import main  # noqa: E402

if __name__ == '__main__':
    sys.exit(main())
