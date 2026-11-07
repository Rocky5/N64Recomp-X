# Licensing

N64Recomp-X is MIT licensed (section 1). It is built from code other people
wrote under their own terms, listed in section 2, and it reads other projects
for behaviour without taking their code (section 3). No game code or data is
in this repository (section 4).

## 1. The port's own code: MIT

Everything in this repository that is not listed in section 2 is the port's
own code (the runtime, the platform layer, the tools, the game packs' facts
and the documentation). It is MIT licensed:

```
MIT License

Copyright (c) 2026 Gabe Conway

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## 2. Code from other projects

| component | where | terms |
|---|---|---|
| **N64Recomp** (N64Recomp/N64Recomp, Copyright (c) 2024 Wiseguy) at `ffb39cd`: the recompiler, run on the host at build time, unmodified. `xbox/include/xrc/recomp.h` implements the contract of its `include/recomp.h` and follows its memory helpers closely | `tools/recomp/` (submodule), `xbox/include/xrc/recomp.h` | MIT |
| **Wings64-X** (TechProGabe/Wings64-X), the maintainer's Pilotwings 64 port: the platform layer, the interpreter's base, the settings, the build and console tooling | `xbox/src/hw/`, `xbox/include/`, `xbox/src/rt/settings.c`, `xbox/src/rt/audio.c` (its glue), `tools/xbox/`, `tests/xbox/test_asp_hle.c` and `asp_ref.c` | Copyright (c) 2026 Gabe Conway; MIT |
| **Melee-X** (TechProGabe/Melee-X): the splash this loading screen is built on, the video mode selection, the NV2A code, the build tooling those files were carried from (each file's header says so) | `xbox/src/hw/` (`xhw_splash*.c`, `xhw_video.c`, `nv2a*.c`), `tools/xbox/` | Copyright (c) Gabe Conway; MIT, as relicensed by the author. Melee-X's Xbox code is GPL-3.0-or-later only because it builds on melee-pc, and none of melee-pc is used here |
| **OpenCrossing-Xbox** (GabeConway/OpenCrossing-Xbox): the audio driver, crash reporter, code repair, RAM lock and clock code | `xbox/src/hw/` (`xhw_audio.c`, `xhw_crash.c`, `xhw_code_repair.c`, `xhw_ramlock.c`, `xhw_clock.c`) | MIT |
| **Birdman64** (NoahVl/Birdman64, Copyright (c) 2026 Birdman64 contributors): the audio ABI that `asp_hle.c` ports | `xbox/src/hw/asp_hle.c`, the vectors in `tests/xbox/test_asp_hle.c` | MIT |
| **unscii-16** (via nxdk's `hal/font_unscii_16.h`): the BACK menu's and the overlay's font | included from nxdk at build time | public domain |
| **Sora** (`xbox/assets/fonts/Sora[wght].ttf`, Copyright 2019 The Sora Project Authors): the loading screen's type, rasterised by `tools/xbox/make_fonts.py` into `xbox/src/hw/xhw_fonts.inc` | the font file unmodified, and bitmaps made from it | SIL Open Font License 1.1 (`xbox/assets/fonts/OFL.txt`) |
| **`xhw_sha1.c`**: SHA-1, written for Wings64-X | `xbox/src/hw/xhw_sha1.c` | public domain |
| **nxdk** (XboxDev/nxdk): the SDK, pbkit, SDL, pdclib | not vendored; built by `tools/xbox/setup.sh` or in the SDK image | its own licenses |
| **LLVM 21** | not vendored | Apache-2.0 WITH LLVM-exception |
| **rabbitizer**, **Pillow** | Python packages the tools use, installed by `setup.sh` | MIT; HPND |

Where a file names more than one origin in its header, the header is the
authority.

## 3. Read for behaviour, no code taken

These projects are GPL-3.0 (or carry no licence). They were read to learn
what the generated code expects and how games behave; none of their code is
in this repository, and none is linked into anything it builds.

- **N64ModernRuntime** (N64Recomp/N64ModernRuntime, GPL-3.0): the list of
  libultra entry points and their argument conventions, the "structs live in
  RDRAM" rule, overlay tracking by DMA source, the RSP task model, the save
  types. `xbox/include/xrc/librecomp/sections.h` is written here to the
  field names the recompiler's generated tables use.
- **Zelda64Recomp** (GPL-3.0): how a game is configured and patched.
- **The community recomps** (Snap64Recomp, Starfox64Recomp, MarioKart64Recomp,
  BanjoRecomp, DK64, Goemon64Recomp, HarvestMoon64Recomp; GPL-3.0): which
  functions each game needed stubbed or patched, read per game when its pack
  is made.
- **Zelda64RecompSyms** (no licence file): the symbol-file format.
- **mupen64plus-rsp-hle** (GPL-2.0): the JPEG decoder task's behaviour as
  Ocarina of Time runs it (the order of dequantization, de-zigzag and the
  inverse transform, the scale, the colour constants and clamps), written
  up in prose and implemented here (`xbox/src/rt/jpeg.c`).
- **GLideN64** (GPL-2.0): what Conker's Bad Fur Day's F3DEXBG microcode does
  differently from F3DEX2 (its command fields, its vertex normals and lights),
  taken down in prose and implemented here (`xbox/src/gfx/f3d_interp.c`).

## 3a. What the toolchain bundle redistributes

The prebuilt toolchain the app downloads (`tools/xbox/pack_toolchain.py`,
`docs/toolchain.md`) is a build of other people's software, each under its
own licence; the texts travel in the bundle's `licenses/` folder. It holds no
Nintendo or Microsoft code and no ROM material.

- **LLVM** (clang, lld, llvm-ar and friends, the clang resource headers):
  Apache-2.0 with LLVM exceptions. MinGW-w64 build on Windows (MSYS2).
- **nxdk** (headers, libraries, `cxbe`, `extract-xiso`) at the pinned commit:
  MIT, Apache-2.0, CC0, NCSA and others per file (its `LICENSES/`). The
  staged `bin/nxdk-cc`, `nxdk-cxx`, `nxdk-as`, `nxdk-cmake` and
  `share/toolchain-nxdk.cmake` have `${NXDK_DIR}` quoted so a path with a
  space works; nothing else is changed. The NVIDIA Cg tools are not shipped.
- **N64Recomp and RSPRecomp**, built at the submodule's pin with
  `patches/recomp`: MIT.
- **MSYS2** (Windows only): bash, coreutils, make, sed, grep, gawk, cmake,
  ninja, the MSYS2 runtime and their libraries, trimmed to what the build
  runs. GPL, LGPL, BSD and other licences per package; sources are
  available from https://packages.msys2.org (PKGBUILDs under
  https://github.com/msys2/MSYS2-packages and MINGW-packages).
- **cmake** (BSD-3-Clause) and **ninja** (Apache-2.0): the official portable
  releases on Linux and macOS (pinned by version and sha256).
- **CPython** from python-build-standalone (PSF licence), with **Pillow**
  (HPND) and **rabbitizer** (MIT) installed into it.

## 4. Game code and game data: not licensed, not here

The games are their publishers'. This repository holds no ROM, no byte of
one, nothing derived from one (no recompiled code, no extracted asset, no
screenshot of a game), and no built XBE: the user supplies their own ROM and
builds on their own machine. A game pack holds facts about a ROM (addresses,
names, sizes, hashes). Nothing here grants a licence to any game.

Nintendo 64 is a trademark of Nintendo. Xbox is a trademark of Microsoft.
This project is not affiliated with or endorsed by either company.
