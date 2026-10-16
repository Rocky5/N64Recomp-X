#!/usr/bin/env bash
# Build default.xbe -> $XBOX_BUILD_DIR/xbe/default.xbe (default build-xbox).
# Needs the toolchain from tools/xbox/setup.sh (NXDK_DIR, LLVM). n64rx runs
# this per game; without XRC_GEN_DIR the XBE is the platform and the runtime
# alone, which boots to the "No game built in" card.
#   XBOX_CFLAGS      extra C flags for every unit (platform and game), e.g.
#                    "-DXHW_AUTOPAD=1"; reset on every run, so a debug knob
#                    never sticks in the cache (docs/testing.md: the switches)
#   XBOX_LTO=0       plain objects; unset or anything else: ThinLTO over the
#                    game side (tools/xbox/thinlto_link.py; PLAN.md 4.4)
#   XBOX_BUILD_DIR   the build folder (n64rx: build-xbox/<pack id>)
#   XRC_GEN_DIR      the game's generated C and xrc_game.c (n64rx)
#   XRC_TITLE_ID     8 hex digits (default 4E520000)
#   XRC_XBE_TITLE    the XBE's title (default N64Recomp-X)
#   XRC_GAME_CFLAGS  the pack's game-side flags (-DXRC_GPR32=1 ...)
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
export NXDK_DIR="${NXDK_DIR:-/opt/nxdk}"
export LLVM="${LLVM:-/opt/llvm21}"
export PATH="$LLVM/bin:$PATH"
eval "$("$NXDK_DIR/bin/activate" -s)"
build="${XBOX_BUILD_DIR:-$root/build-xbox}"
case "${XBOX_LTO:-1}" in 0) lto=OFF ;; *) lto=ON ;; esac
cmake -S "$root/xbox" -B "$build" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$NXDK_DIR/share/toolchain-nxdk.cmake" -DCMAKE_C_FLAGS="${XBOX_CFLAGS:-}" \
  -DXBOX_LTO="$lto" \
  -DXRC_GEN_DIR="${XRC_GEN_DIR:-}" -DXRC_TITLE_ID="${XRC_TITLE_ID:-4E520000}" \
  -DXRC_XBE_TITLE="${XRC_XBE_TITLE:-N64Recomp-X}" -DXRC_GAME_CFLAGS="${XRC_GAME_CFLAGS:-}" \
  ${XBOX_CMAKE_ARGS:-} >/dev/null
ninja -C "$build" ${XBOX_NINJA_ARGS:-}
ls -la "$build/xbe/default.xbe"
