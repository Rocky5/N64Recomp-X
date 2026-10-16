#!/usr/bin/env python3
"""Build and run tests/xbox/test_splash.c on the host: the loading screen's
layout, palette and STILL determinism (xbox/src/hw/xhw_splash_draw.c,
PLAN.md 5.5). Needs nxdk for the font (NXDK_DIR). The frames it checked are
saved as build-host/splash_*.png when Pillow is there, for the eye."""
import pathlib
import sys

import hostcc

if __name__ == '__main__':
    nxdk = hostcc.nxdk_dir()
    if not nxdk:
        sys.exit('test_splash: no nxdk (set NXDK_DIR): the font is its hal/font_unscii_16.h')
    out = hostcc.ROOT / 'build-host'
    out.mkdir(exist_ok=True)
    rc = hostcc.run(['tests/xbox/test_splash.c', 'xbox/src/hw/xhw_splash_draw.c'],
                    includes=['xbox/include', 'xbox/src/hw', str(nxdk / 'lib')], args=[str(out)])
    try:
        from PIL import Image
        for raw in out.glob('splash_*.argb'):
            data = raw.read_bytes()
            Image.frombuffer('RGBA', (640, 480), data, 'raw', 'BGRA', 0, 1).convert('RGB').save(raw.with_suffix('.png'))
            raw.unlink()
    except ImportError:
        pass
    sys.exit(rc)
