#!/usr/bin/env bash
# One-time native toolchain for N64Recomp-X (PLAN.md 4.1, 5.1): what
# `n64rx doctor --install` runs. CI runs it on all three OSes.
#
#   Linux (Debian/Ubuntu, x86-64 or arm64)  apt packages; LLVM 21.1.8 into $LLVM (/opt/llvm21)
#   macOS (Homebrew)                        brew llvm@21 lld@21 cmake ninja bison flex
#   Windows (from Git Bash or MSYS2)        MSYS2's pacman packages (C:\msys64 must exist)
#
# then, on all three:
#   nxdk at the pin -> $NXDK_DIR (Linux/macOS /opt/nxdk, Windows C:\xdev\nxdk),
#                      built with LLVM 21 (`make tools`, `make NXDK_ONLY=y NXDK_SDL=y NXDK_CXX=y`)
#   N64Recomp       -> build-host/recomp (the submodule at its pin; RSPRecomp too)
#   Python modules  -> rabbitizer, pillow
#
# LLVM 21, not the distro clang: nxdk warns that 19.x-20.1.2 miscompile
# (llvm/llvm-project#134607). No ROM is ever an input here.
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
LLVM_VERSION=21.1.8
NXDK_SHA=58427c078b4ccb0121f359b6b6c536b3b2914976
os="$(uname -s)"
case "$os" in
  Linux) kind=linux; jobs="$(nproc)" ;;
  Darwin) kind=mac; jobs="$(sysctl -n hw.ncpu)" ;;
  MINGW*|MSYS*|CYGWIN*) kind=windows; jobs="${NUMBER_OF_PROCESSORS:-4}" ;;
  *) echo "setup.sh: $os is not supported (Linux, macOS, Windows)"; exit 1 ;;
esac
say() { echo "== $*"; }

# ---- the compiler, cmake, ninja, bison, flex ----
if [ "$kind" = linux ]; then
  LLVM="${LLVM:-/opt/llvm21}"
  NXDK_DIR="${NXDK_DIR:-/opt/nxdk}"
  if command -v apt-get >/dev/null; then
    say "apt packages"
    sudo=; [ "$(id -u)" = 0 ] || sudo=sudo
    $sudo apt-get update -q
    $sudo apt-get install -yq build-essential cmake ninja-build git bison flex patch curl xz-utils \
      libzstd-dev zlib1g-dev libxml2-dev python3-pip
    # a 32-bit host compiler for test_recomp_h (our header on i686); x86-64 hosts only
    if [ "$(uname -m)" = x86_64 ]; then $sudo apt-get install -yq gcc-multilib || true; fi
  fi
  if [ ! -x "$LLVM/bin/clang" ]; then
    arch="$(uname -m)"; case "$arch" in x86_64) tarch=X64 ;; aarch64|arm64) tarch=ARM64 ;; *) echo "no LLVM build for $arch"; exit 1 ;; esac
    say "LLVM $LLVM_VERSION -> $LLVM"
    mkdir -p "$LLVM" 2>/dev/null || sudo mkdir -p "$LLVM"
    [ -w "$LLVM" ] || sudo chown "$(id -u)" "$LLVM"
    curl -sSL "https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VERSION/LLVM-$LLVM_VERSION-Linux-$tarch.tar.xz" \
      | tar -xJ -C "$LLVM" --strip-components=1
  fi
elif [ "$kind" = mac ]; then
  command -v brew >/dev/null || { echo "install Homebrew first: https://brew.sh"; exit 1; }
  say "Homebrew packages"
  brew install llvm@21 lld@21 cmake ninja bison flex git python@3.12 || brew install llvm@21 cmake ninja bison flex git
  LLVM="${LLVM:-$(brew --prefix llvm@21)}"
  NXDK_DIR="${NXDK_DIR:-/opt/nxdk}"
  # Homebrew's bison and flex are keg-only (macOS ships old ones), lld may be its own keg
  export PATH="$(brew --prefix bison)/bin:$(brew --prefix flex)/bin:$PATH"
  if brew --prefix lld@21 >/dev/null 2>&1; then export PATH="$(brew --prefix lld@21)/bin:$PATH"; fi
else
  msys="${MSYS2_ROOT:-/c/msys64}"
  [ -x "$msys/usr/bin/bash" ] || { echo "install MSYS2 to C:\\msys64 first: https://www.msys2.org"; exit 1; }
  say "MSYS2 packages"
  "$msys/usr/bin/bash" -lc 'pacman -S --needed --noconfirm make git bison flex cmake ninja patch \
    mingw-w64-x86_64-clang mingw-w64-x86_64-lld mingw-w64-x86_64-llvm mingw-w64-x86_64-python \
    mingw-w64-x86_64-python-pillow mingw-w64-x86_64-python-pip mingw-w64-i686-gcc'
  LLVM=/mingw64
  NXDK_DIR="${NXDK_DIR:-/c/xdev/nxdk}"
fi

# everything below runs in a shell that has the toolchain on its PATH; on
# Windows that is MSYS2's login shell (Git Bash's environment can't run nxdk's
# wrappers), elsewhere this one
run() {
  if [ "$kind" = windows ]; then
    "$msys/usr/bin/bash" -lc "export PATH=/usr/bin:/mingw64/bin:\$PATH; cd '$(cd "$root" && pwd)'; $*"
  else
    (export PATH="$LLVM/bin:$PATH"; cd "$root"; eval "$*")
  fi
}

# ---- nxdk at the pin ----
if [ ! -f "$NXDK_DIR/lib/libpdclib.lib" ]; then
  say "nxdk $NXDK_SHA -> $NXDK_DIR"
  parent="$(dirname "$NXDK_DIR")"
  mkdir -p "$parent" 2>/dev/null || sudo mkdir -p "$parent"
  [ -d "$NXDK_DIR/.git" ] || git clone --recursive https://github.com/XboxDev/nxdk.git "$NXDK_DIR" \
    || { sudo mkdir -p "$NXDK_DIR"; sudo chown "$(id -u)" "$NXDK_DIR"; git clone --recursive https://github.com/XboxDev/nxdk.git "$NXDK_DIR"; }
  git -C "$NXDK_DIR" checkout -q "$NXDK_SHA"
  git -C "$NXDK_DIR" submodule update --init --recursive
  run "make -C '$NXDK_DIR' tools -j$jobs"
  run "export NXDK_DIR='$NXDK_DIR'; eval \"\$('$NXDK_DIR/bin/activate' -s)\"; make -C '$NXDK_DIR' NXDK_ONLY=y NXDK_SDL=y NXDK_CXX=y -j$jobs"
  # extract-xiso for n64rx xiso (built with nxdk's tools on most hosts; built here when it isn't)
  if [ ! -x "$NXDK_DIR/tools/extract-xiso/build/extract-xiso" ] && [ ! -x "$NXDK_DIR/tools/extract-xiso/build/extract-xiso.exe" ]; then
    run "cmake -S '$NXDK_DIR/tools/extract-xiso' -B '$NXDK_DIR/tools/extract-xiso/build' >/dev/null && cmake --build '$NXDK_DIR/tools/extract-xiso/build' -j$jobs" || true
  fi
else
  say "nxdk: $NXDK_DIR (built)"
fi

# ---- N64Recomp at the submodule's pin ----
git -C "$root" submodule update --init --recursive tools/recomp
if [ ! -x "$root/build-host/recomp/N64Recomp" ] && [ ! -x "$root/build-host/recomp/N64Recomp.exe" ]; then
  say "N64Recomp -> build-host/recomp"
  if [ "$kind" = windows ]; then
    run "cmake -S tools/recomp -B build-host/recomp -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_EXE_LINKER_FLAGS=-static && ninja -C build-host/recomp N64RecompCLI RSPRecomp"
  else
    (cd "$root" && cmake -S tools/recomp -B build-host/recomp -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null \
      && ninja -C build-host/recomp N64RecompCLI RSPRecomp)
  fi
fi

# ---- Python modules for n64rx ----
py=python3; command -v python3 >/dev/null && python3 -c 'import sys' 2>/dev/null || py=python
say "Python modules (rabbitizer, pillow) for $py"
"$py" -m pip install --user -q rabbitizer pillow 2>/dev/null || "$py" -m pip install --user -q --break-system-packages rabbitizer pillow || true

say "done: NXDK_DIR=$NXDK_DIR LLVM=$LLVM; run n64rx doctor"
