#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
IMAGE="${IMAGE:-devkitpro/devkita64}"
BUILD_DIR="${BUILD_DIR:-build-switch}"
MODE="${1:-core}"

case "$MODE" in
  core)
    TARGET="src/core/libcore.a"
    ;;
  util)
    TARGET="src/util/libutil.a"
    ;;
  elf)
    TARGET="duckstation-nogui.elf"
    ;;
  nro)
    TARGET="tico-duckstation.nro"
    ;;
  all)
    TARGET=""
    ;;
  clean)
    rm -rf "$ROOT/$BUILD_DIR"
    echo "Removed $BUILD_DIR"
    exit 0
    ;;
  *)
    TARGET="$MODE"
    ;;
esac

docker run --rm \
  -e HOST_UID="$(id -u)" \
  -e HOST_GID="$(id -g)" \
  -e BUILD_DIR="$BUILD_DIR" \
  -e TARGET="$TARGET" \
  -v "$ROOT:/project/source" \
  --workdir /project/source \
  "$IMAGE" \
  bash -lc '
set -euo pipefail

export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export DEBIAN_FRONTEND=noninteractive

apt-get update >/dev/null
apt-get install -y --no-install-recommends \
  ca-certificates \
  git \
  meson \
  ninja-build \
  bison \
  flex \
  pkg-config \
  python3-mako >/dev/null

if [ ! -f "$DEVKITPRO/portlibs/switch/include/uam.h" ] || [ ! -f "$DEVKITPRO/portlibs/switch/lib/libuam.a" ]; then
  echo "==> Installing uam into devkitPro portlibs"
  rm -rf /tmp/uam
  git clone --depth 1 -b library-target https://github.com/RSDuck/uam.git /tmp/uam >/dev/null
  cd /tmp/uam
  printf "%s\n" "option('\''build_as_library'\'', type: '\''boolean'\'', value: false)" > meson_options.txt
  meson setup build-switch -Dbuild_as_library=true --cross-file=crossfile >/dev/null
  ninja -C build-switch >/dev/null
  rm -f "$DEVKITPRO/portlibs/switch/lib/libuam.a"
  "$DEVKITPRO/devkitA64/bin/aarch64-none-elf-ar" rcs \
    "$DEVKITPRO/portlibs/switch/lib/libuam.a" \
    $(find build-switch -name "*.o" | sort)
  install -m644 /tmp/uam/source/uam.h "$DEVKITPRO/portlibs/switch/include/uam.h"
  cd /project/source
fi

git config --global --add safe.directory /project/source

if [ ! -f "$BUILD_DIR/build.ninja" ]; then
  echo "==> Configuring $BUILD_DIR"
  "$DEVKITPRO/portlibs/switch/bin/aarch64-none-elf-cmake" \
    -S . \
    -B "$BUILD_DIR" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_NOGUI_FRONTEND=ON \
    -DBUILD_QT_FRONTEND=OFF \
    -DENABLE_OPENGL=OFF \
    -DENABLE_VULKAN=OFF \
    -DENABLE_CUBEB=OFF \
    -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON
fi

if [ -n "$TARGET" ]; then
  echo "==> Building $TARGET"
  ninja -C "$BUILD_DIR" "$TARGET"
else
  echo "==> Building default target"
  ninja -C "$BUILD_DIR"
fi

chown -R "$HOST_UID:$HOST_GID" "$BUILD_DIR"
'
