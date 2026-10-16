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

Used unmodified at the pin, on the host, at build time. n64rx writes the
pack's `recomp.toml` with the paths filled in to `build-xbox/<id>/cfg/`,
runs `N64Recomp` there, and gets `gen/` (`funcs.h`, `funcs_N.c`,
`recomp_overlays.inl`, `lookup.cpp`, which is not compiled). Changes to the
recompiler go to `patches/recomp/` with a `.md` each (none yet).

## LTO

`XBOX_LTO=1` (the default; `n64rx build --fast` turns it off): the game side
as ThinLTO bitcode, `tools/xbox/thinlto_link.py` in front of the link
(Melee-X's machinery through Wings64-X). `-opt:noicf`: the game compares
function pointers and the function table must not fold.
