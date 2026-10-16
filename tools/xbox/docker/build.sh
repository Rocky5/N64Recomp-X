#!/usr/bin/env bash
# Build default.xbe inside the SDK image (macOS, Linux, Windows Git Bash):
# the platform and runtime alone, or what n64rx asks for (the XRC_* knobs).
#   tools/xbox/docker/build.sh            # -> build-xbox/xbe/default.xbe
# The image (tools/xbox/docker/IMAGE) is pulled on first use; to build it:
#   docker build -t "$(cat tools/xbox/docker/IMAGE)" -f tools/xbox/docker/Dockerfile .
set -euo pipefail
root="$(cd "$(dirname "$0")/../../.." && pwd)"
image="$(cat "$root/tools/xbox/docker/IMAGE")"
vroot="$root"
# Git Bash on Windows: no MSYS path rewriting of /src etc., and a C:/ path for -v
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) export MSYS_NO_PATHCONV=1; vroot="$(cd "$root" && pwd -W)";; esac
tty=; [ -t 1 ] && tty=-t
docker run --rm $tty -v "$vroot":/src -w /src \
  -e XBOX_CFLAGS -e XBOX_CMAKE_ARGS -e XBOX_NINJA_ARGS -e XBOX_FORCE -e XBOX_LTO -e XBOX_BUILD_DIR \
  -e XRC_GEN_DIR -e XRC_TITLE_ID -e XRC_XBE_TITLE -e XRC_GAME_CFLAGS \
  "$image" bash -c 'xbox/build.sh'
