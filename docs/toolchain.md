# Toolchain

Pins: nxdk `58427c078b4ccb0121f359b6b6c536b3b2914976` (Melee-X's), LLVM 21.1.8,
N64Recomp `ffb39cd` (the `tools/recomp` submodule). `n64rx doctor` checks all
of them; `tools/xbox/setup.sh` installs them.

## Two triples, one link (PLAN.md 3.1)

| side | code | triple | built by |
|---|---|---|---|
| hw | `xbox/src/hw` (kernel, pbkit, USB, AC97, the loading screen) | nxdk's `i386-pc-win32` | CMake (`xbox/CMakeLists.txt`) |
| game | `xbox/src/rt` (runtime), `xbox/src/gfx` (interpreter), the generated C | `i686-pc-windows-gnu -mno-ms-bitfields` | `tools/xbox/compile_game.py` |

They share only the plain-scalar structs of `xhw.h`, `xgfx.h`, `xrom.h` and
the build facts of `xrc_build.h` (title ID, version), which CMake writes from
`xbox/xrc_build.h.in`. lld-link links both into `n64recomp_x.exe`; cxbe makes
`default.xbe`; `tools/xbox/xbe_sections.py` sets the title ID, the name and
the icon.

The game triple (from Wings64-X, minus its decomp flags):

```
--target=i686-pc-windows-gnu -mno-ms-bitfields -march=pentium3 -msse -mfpmath=sse
-ffreestanding -nostdlibinc -isystem <nxdk>/lib/pdclib/include ... -std=gnu11 -O2
-ffp-contract=off -fno-fast-math -fno-strict-aliasing -fwrapv
-fno-auto-import -mno-stack-arg-probe -fno-asynchronous-unwind-tables -ffunction-sections
-U_WIN32 -U__MINGW32__ ... -DNXDK -include xbox/include/xrc/xrc_prelude.h
-Werror=implicit-function-declaration -Werror=implicit-int
```

Never `-ftrivial-auto-var-init` or `-ffast-math` on the game side. The
generated code gets `-Wno-unused-*` for its labels and locals; the runtime
keeps `-Wall`. `XRC_GAME_CFLAGS` carries the pack's flags (`-DXRC_GPR32=1`)
to every game-side unit, the runtime included, since `recomp_context`'s
layout depends on the register width.

## Native, per OS

```sh
bash tools/xbox/setup.sh          # once (Git Bash on Windows); n64rx doctor --install runs it
python tools/n64rx/n64rx.py doctor
```

| OS | compiler, cmake, ninja | nxdk (`NXDK_DIR`) | notes |
|---|---|---|---|
| Windows | MSYS2 at `C:\msys64`: `mingw-w64-x86_64-clang` (21.1.8), MSYS2's own `cmake` and `ninja` (the mingw ones can't run nxdk's shell wrappers) | `C:\xdev\nxdk` | n64rx drives MSYS2's login bash itself; N64Recomp is built with `-static` |
| Linux | LLVM 21.1.8 release tarball in `/opt/llvm21` (`LLVM`), apt's cmake, ninja, bison, flex | `/opt/nxdk` | x86-64 and arm64 |
| macOS | Homebrew `llvm@21`, `lld@21`, cmake, ninja, bison, flex (keg-only, put on the PATH by setup.sh) | `/opt/nxdk` | not yet run here (CI's macOS job is the first) |

The platform and runtime alone (no game), from a checkout:

```sh
tools/xbox/msys/build.sh          # Windows, from Git Bash -> build-xbox/xbe/default.xbe
xbox/build.sh                     # Linux, macOS
XBOX_LTO=0 XBOX_CFLAGS=-DXHW_AUTOPAD=1 tools/xbox/msys/build.sh   # a test build, no LTO
```

`xbox/build.sh`'s knobs: `XBOX_CFLAGS` (both sides, reset every run),
`XBOX_LTO=0`, `XBOX_BUILD_DIR`, and n64rx's `XRC_GEN_DIR`, `XRC_TITLE_ID`,
`XRC_XBE_TITLE`, `XRC_GAME_CFLAGS`.

## Docker

`tools/xbox/docker/Dockerfile` is the SDK image (`tools/xbox/docker/IMAGE`
names its tag): Debian trixie, LLVM 21 from apt.llvm.org, nxdk at the pin,
N64Recomp from the checkout's submodule, Python with rabbitizer and Pillow;
linux/amd64 and linux/arm64. Build it from the repo root:

```sh
docker build -t "$(cat tools/xbox/docker/IMAGE)" -f tools/xbox/docker/Dockerfile .
```

`n64rx build --engine docker` runs the whole pipeline inside it;
`tools/xbox/docker/build.sh` runs `xbox/build.sh` inside it.

## N64Recomp

Used at the pin with `patches/recomp/*.patch` applied, on the host, at
build time. n64rx writes the pack's `recomp.toml` with the paths filled in
to `build-xbox/<id>/cfg/`, runs `N64Recomp` there, and gets `gen/`
(`funcs.h`, `funcs_N.c`, `recomp_overlays.inl`, `lookup.cpp`, which is not
compiled). Changes to the recompiler go to `patches/recomp/` with a `.md`
each; the submodule is never edited. `tools/xbox/recomp_src.py` copies it
to `build-host/recomp-src` and applies the patches in name order (its own
strict applier, LF; no `patch` needed); n64rx builds `build-host/recomp`
from there and keys the recompiler and the generated code's cache on the
pin plus the patches (`.n64rx-key`), so a new or changed patch rebuilds
both. The SDK image's prebuilt recompiler is unpatched: inside it n64rx
builds its own the first time.

| patch | what |
|---|---|
| `0001-jump-table-in-another-section` | a jump table outside the function's section is read from the section that holds it (GoldenEye's TLB-mapped code with its tables in KSEG0; with `learn.py`'s RDRAM table sections) |

## LTO

`XBOX_LTO=1` (the default; `n64rx build --fast` turns it off): the game side
as ThinLTO bitcode, `tools/xbox/thinlto_link.py` in front of the link
(Melee-X's machinery through Wings64-X). `-opt:noicf`: the game compares
function pointers and the function table must not fold.

## The prebuilt toolchain (the app's)

The app (a non-technical user, no Docker, no `setup.sh`) builds with one
bundle per OS and architecture and nothing else: no MSYS2 at `C:\msys64`, no
nxdk at `C:\xdev`, no system LLVM. `tools/xbox/pack_toolchain.py` makes it
from what `setup.sh` already installed on the packing machine. The app's
full build carries the bundle inside itself, already unpacked
(`resources/toolchain/`, "Inside the app" below), so the user downloads one
zip and nothing else; the app built without it (`npm run dist:no-tools`)
downloads the archive once instead and unpacks it to
`<cache>/toolchain/<key>/`.

What is in it (`<key>/`):

| folder | Windows x64 | Linux, macOS |
|---|---|---|
| `python/` | python-build-standalone 3.12.15 (`install_only`, pinned URL and sha256) with `rabbitizer` and `pillow` (pinned) | the same |
| `msys64/` | MSYS2 trimmed to the build: `usr/bin` bash, coreutils, make, sed, grep, gawk, **cmake**, **ninja** (MSYS2's own) and the DLLs they import (found by reading the PE import tables), `etc/`, cmake's modules; `mingw64/bin` clang, lld, `llvm-ar/lib/ranlib/nm/objcopy/strip/dlltool`, their DLLs, `mingw64/lib/clang/21` headers; a `usr/bin/python3` shim that runs the bundle's Python (the build's `python3` calls) | - |
| `llvm/` | - | LLVM 21.1.8 trimmed to clang, lld and the llvm-* tools nxdk calls, plus `lib/clang/21/include` (macOS: brew's dylibs copied to `lib/ext` and re-pointed, then ad-hoc signed) |
| `cmake/`, `bin/` | - | the official portable cmake 3.31.6 and ninja 1.12.1 (pinned sha256) |
| `nxdk/` | built nxdk at `NXDK_PIN`: `bin/` (wrappers), `lib/` (headers, built `.lib`s, the pbkit sources the game build compiles), `share/`, `tools/cxbe`, `tools/extract-xiso/build/extract-xiso`; no `.git`, objects, dependency files, samples or `tools/cg` (NVIDIA Cg is not redistributable and the build does not use it), and none of the upstream test suites and IDE projects the build never reads (`NXDK_PRUNE`: `lib/libcxx/test`, `lib/sdl/SDL2/test`, SDL's `Xcode*`, `VisualC*`, `visualtest`) | the same |
| `recomp/` | `N64Recomp`, `RSPRecomp` and `.n64rx-key` (refuses to pack if the key is not `engines.recompiler_pin()`; it says how to rebuild) | the same |
| `dryboot/` | `<source hash>/dryboot.exe`: the dry boot (`tools/dryboot/dryboot.c`, `N64RX_DRYBOOT_DIR`) built static with MSYS2's clang, since the bundle carries no C library to build it with | `<source hash>/dryboot`, built with the packing machine's `cc` |
| `licenses/` | the texts of everything above, with where the MSYS2 sources are | the same |
| `manifest.json` | read by the app | the same |

Pack it (one command; the stage folder and the archive go to `dist/toolchain`,
which is not tracked):

```sh
python tools/xbox/pack_toolchain.py                       # stage, manifest, archive (.tar.zst if `zstandard` is installed, else .tar.xz), sha256 + a .json for app/toolchain.json
python tools/xbox/pack_toolchain.py --no-archive          # the stage folder only
python tools/xbox/pack_toolchain.py --install-to "D:/odd path/tc"      # unpack the newest archive and run the checks
python tools/xbox/pack_toolchain.py --check "D:/odd path/tc"           # n64rx's Native checks with the manifest's env applied
python tools/xbox/pack_toolchain.py --selftest "D:/odd path/tc"        # the ROM-free test ROM, built with ONLY the bundle
python tools/xbox/pack_toolchain.py --key                              # the key this checkout packs (JSON), for app/scripts/stage-toolchain.js
```

`--selftest` runs the bundle's Python on `tools/n64rx/n64rx.py build`, applies
the manifest's `env` and `path`, drops everything else from the environment
(Windows: `PATH` is the bundle plus `System32`), and fails if the build log
names `C:/msys64`, `C:/xdev`, `/opt/nxdk` or `/opt/llvm21`. `.github/workflows/toolchain.yml`
(manual) does the same on Windows, macOS arm64 and x64 and Ubuntu 22.04 and can
upload the archives to a `toolchain-<key>` release. Other flags: `--python-from DIR`
(offline), `--msys`, `--llvm`, `--nxdk`, `--recomp`, `--stage`, `--out`, `--cache`.

`manifest.json`:

```json
{"key": "9bdcd5091aa1", "os": "windows", "arch": "x64", "python": "python/python.exe",
 "env": {"MSYS2_ROOT": "msys64", "NXDK_DIR": "nxdk", "N64RX_RECOMP": "recomp"},
 "path": [], "llvm": "21.1.8", "nxdk": "<NXDK_PIN>", "recompiler": "<recompiler_pin()>", "created": "2026-10-18"}
```

Every value in `env` and `path` is relative to the bundle root; the app makes
them absolute. Linux and macOS use `"LLVM": "llvm"` and `"path": ["python/bin",
"llvm/bin", "cmake/bin", "bin"]`. `key` is a hash of (OS, arch, LLVM version, nxdk
pin, recompiler pin, Python version, the packer's `FORMAT`). The app also exports
`N64RX_RECOMPILER_KEY=<manifest.recompiler>`: the app's copy of the sources has
no git, so `engines.recompiler_pin()` takes the key from that variable when set
(unset, it asks the checkout as before) and `Native.ensure_recompiler` accepts
the shipped `recomp/` instead of trying to build one.

Relocation: nothing is rewritten at unpack time. The built nxdk holds absolute
paths only in dependency files (not shipped) and in assertion/debug strings
inside `.lib` and `.obj` files (inert). `NXDK_DIR`, `MSYS2_ROOT`, `N64RX_RECOMP`
and (POSIX) `LLVM` are read from the environment by `engines.Native`; MSYS2's
root is found by `msys-2.0.dll` from its own location, so it runs from any
folder. One upstream script problem is fixed in the staged copy only
(`DIVERGES`): nxdk's `bin/nxdk-cc`, `-cxx`, `-as`, `-cmake` and
`share/toolchain-nxdk.cmake` use `${NXDK_DIR}` unquoted, which breaks on a path
with a space (a Windows user name like "Jo Smith" is enough); the packer quotes
those words and changes nothing else.

Measured on this machine (Windows 11, x64): staged 644 MB (msys64 297, nxdk 190,
python 148, recomp 9, licenses 1); `.tar.zst` level 19 124 MB; packing about
2 min; unpacking about 50 s with the checks; the ROM-free test ROM builds from
an unpacked bundle at a path with spaces in 19 s (ThinLTO, clean build folder).
Linux and macOS packing is written but was not run here; the CI workflow is its
first run. Since `NXDK_PRUNE` (2026-10-22) the stage is 562 MB (nxdk 107 MB
instead of 190) and its longest path 103 characters (was 176).

### Inside the app (the full build)

`npm run dist` in `app/` ships the staged folder itself, not the archive:
`app/scripts/stage-toolchain.js` reuses `app/stage/toolchain/` when its
`manifest.json` has the key `pack_toolchain.py --key` gives for this checkout,
else runs `pack_toolchain.py --no-archive --stage app/stage/toolchain`; then
electron-builder copies it to `resources/toolchain/` (Windows, Linux) or
`Contents/Resources/toolchain/` (macOS). The app reads that manifest exactly
as it reads a downloaded one (`paths.shippedBundle`) and prefers it to
anything in the data folder; nothing is unpacked or written into it, except
the Python's own `__pycache__` where the folder is writable. The packaged app
runs n64rx from its source copy, so `N64RX_RECOMPILER_KEY` (the manifest's
`recompiler`) is what makes `Native.ensure_recompiler` take the shipped
`recomp/`.

One n64rx fix came out of it: `Native.checks()` asked `git -C <nxdk> rev-parse
HEAD` of any nxdk, and a bundle has no `.git`, so inside a git work tree (the
app unzipped under `Documents\GitHub\...`, or the stage in the checkout) git
answered with that repository's commit and the nxdk check failed. The commit is
now asked only when `<nxdk>/.git` exists (`test_n64rx.Doctor`).

Measured on this machine (2026-10-22, Windows 11): `--check` and `--selftest`
of `app/stage/toolchain` OK (the test ROM in 43 s); the app zip 329 MB, 961 MB
unpacked; unzipped to a path with spaces, `--check` OK and `--selftest` 41 s
on its `resources/toolchain`, and the packaged app itself, PATH cut to
System32, built the test ROM through its UI in 45 s (`npm run test:packaged`).
