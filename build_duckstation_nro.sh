#!/bin/bash
# Builds tico-duckstation for tico: the core and its renderers (deko3D,
# Vulkan on Mesa's NVK, OpenGL and Zink on Mesa, and Software), then the tico
# frontend and the NRO. Runs inside the switch-dev image:
#
#   docker run --rm -v "$PWD:/work" -w /work ghcr.io/autorunhq/switch-dev:2026.10.05 ./build_duckstation_nro.sh
#
# DUCK_CLEAN=1 rebuilds the core from scratch; build_deps/ (uam, glslang) is
# kept until it is deleted.

set -e

export DEVKITPRO=/opt/devkitpro
export DEVKITA64=$DEVKITPRO/devkitA64
PORTLIBS=$DEVKITPRO/portlibs/switch
LIBNX=$DEVKITPRO/libnx

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
TICO_DIR="$ROOT_DIR/tico"
DEPS_DIR="$ROOT_DIR/build_deps"
CORE_BUILD="$ROOT_DIR/build_core"

echo "=== Building DuckStation for tico ==="
git config --global --add safe.directory "$ROOT_DIR" 2>/dev/null || true

# ============================================================
# Step 1: uam, deko3D's GLSL compiler, as a library
# ============================================================
# RSDuck's library-target branch; the deko3D renderer compiles its shaders
# with it at run time.
UAM_LIB="$DEPS_DIR/lib/libuam.a"
if [ ! -f "$UAM_LIB" ]; then
    echo "--- Step 1: Building uam ---"
    rm -rf "$DEPS_DIR/uam-src" "$DEPS_DIR/uam-build"
    mkdir -p "$DEPS_DIR/lib" "$DEPS_DIR/include"
    cp -r "$TICO_DIR/deps/uam" "$DEPS_DIR/uam-src"
    (
        cd "$DEPS_DIR/uam-src"
        # the branch builds a library only when asked
        printf "%s\n" "option('build_as_library', type: 'boolean', value: false)" > meson_options.txt
        meson setup "$DEPS_DIR/uam-build" -Dbuild_as_library=true --cross-file=crossfile > /dev/null
        ninja -C "$DEPS_DIR/uam-build" > /dev/null
    )
    "$DEVKITA64/bin/aarch64-none-elf-ar" rcs "$UAM_LIB" $(find "$DEPS_DIR/uam-build" -name "*.o" | sort)
    install -m644 "$DEPS_DIR/uam-src/source/uam.h" "$DEPS_DIR/include/uam.h"
fi

# ============================================================
# Step 2: glslang (GLSL to SPIR-V for the Vulkan renderer, and for slang
# shaders)
# ============================================================
# Don't pass CMAKE_CXX_FLAGS here: it replaces the toolchain's -mtp=soft, and
# glslang's thread_locals then read a null thread pointer.
GLSLANG_BUILD="$DEPS_DIR/glslang-build"
echo "--- Step 2: Building glslang ---"
cmake -S "$TICO_DIR/deps/glslang" -B "$GLSLANG_BUILD" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_OPT=OFF -DENABLE_HLSL=OFF -DENABLE_GLSLANG_BINARIES=OFF -DGLSLANG_TESTS=OFF \
    -DBUILD_EXTERNAL=OFF -DENABLE_SPVREMAPPER=OFF -DBUILD_SHARED_LIBS=OFF \
    -DGLSLANG_ENABLE_INSTALL=OFF > /dev/null
cmake --build "$GLSLANG_BUILD"

# ============================================================
# Step 3: the DuckStation core and its renderers
# ============================================================
echo "--- Step 3: Building the core ---"
if [ "${DUCK_CLEAN:-0}" = "1" ]; then
    rm -rf "$CORE_BUILD"
fi
"$PORTLIBS/bin/aarch64-none-elf-cmake" -S "$ROOT_DIR" -B "$CORE_BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_NOGUI_FRONTEND=OFF -DBUILD_QT_FRONTEND=OFF -DBUILD_REGTEST=OFF -DBUILD_TESTS=OFF \
    -DENABLE_OPENGL=ON -DENABLE_VULKAN=ON -DENABLE_CUBEB=OFF \
    -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DTICO_DEPS_DIR="$DEPS_DIR" \
    -DTICO_GLSLANG_SOURCE="$TICO_DIR/deps/glslang" -DTICO_GLSLANG_BUILD="$GLSLANG_BUILD"
ninja -C "$CORE_BUILD" core util common

echo "=== Core built ==="
