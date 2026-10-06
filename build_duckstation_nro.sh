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
    # uam carries its own (older) copy of Mesa's compiler and util code, which
    # clashes with the Mesa linked for EGL, OpenGL and NVK: everything it
    # defines but its API (uam.h) is renamed uamint_*, its uses included.
    UAM_OBJS=$(find "$DEPS_DIR/uam-build" -name "*.o" | sort)
    "$DEVKITA64/bin/aarch64-none-elf-nm" -g --defined-only $UAM_OBJS 2>/dev/null \
        | awk 'NF == 3 { print $3 }' | sort -u \
        | grep -v -x -e uam_init -e uam_deinit -e uam_compileDksh \
        | awk '{ print $1 " uamint_" $1 }' > "$DEPS_DIR/uam-build/rename.syms"
    for obj in $UAM_OBJS; do
        "$DEVKITA64/bin/aarch64-none-elf-objcopy" --redefine-syms="$DEPS_DIR/uam-build/rename.syms" "$obj"
    done
    "$DEVKITA64/bin/aarch64-none-elf-ar" rcs "$UAM_LIB" $UAM_OBJS
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
# Step 2b: Mesa's NVK (Vulkan), linked statically
# ============================================================
# Point MESA_NVK_DIR at builddir-switch of a mesa-switch tree; without one,
# the switch-dev image's Horizon-native NVK in portlibs is used. Mesa merges
# NVK's archives with the host ar, which leaves the Rust members out of the
# symbol index: the copy is re-indexed with the devkitA64 archiver.
MESA_NVK_DIR="${MESA_NVK_DIR:-/nvk-build}"
NVK_ARCHIVE_SRC="$MESA_NVK_DIR/src/nouveau/vulkan/libvulkan.a"
NVK_DEPS="-ldrm_nouveau;-lexpat"
if [ ! -f "$NVK_ARCHIVE_SRC" ]; then
    NVK_ARCHIVE_SRC="$PORTLIBS/lib/libvulkan.a"
    NVK_DEPS="-lexpat" # no libdrm_nouveau in that build; see its vulkan.pc
fi
if [ ! -f "$NVK_ARCHIVE_SRC" ]; then
    echo "Error: no NVK libvulkan.a (set MESA_NVK_DIR)"
    exit 1
fi
NVK_ARCHIVE="$DEPS_DIR/lib/libvulkan_nvk.a"
if [ ! -f "$NVK_ARCHIVE" ] || [ "$NVK_ARCHIVE_SRC" -nt "$NVK_ARCHIVE" ]; then
    cp "$NVK_ARCHIVE_SRC" "$NVK_ARCHIVE"
    "$DEVKITA64/bin/aarch64-none-elf-ranlib" "$NVK_ARCHIVE"
fi
echo "NVK: $NVK_ARCHIVE_SRC"

# ============================================================
# Step 3: the DuckStation core and its renderers
# ============================================================
echo "--- Step 3: Building the core ---"
if [ "${DUCK_CLEAN:-0}" = "1" ]; then
    rm -rf "$CORE_BUILD"
fi
"$PORTLIBS/bin/aarch64-none-elf-cmake" -S "$ROOT_DIR" -B "$CORE_BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_NOGUI_FRONTEND=ON -DBUILD_QT_FRONTEND=OFF -DBUILD_REGTEST=OFF -DBUILD_TESTS=OFF \
    -DENABLE_OPENGL=ON -DENABLE_VULKAN=ON -DENABLE_CUBEB=OFF \
    -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DTICO_DEPS_DIR="$DEPS_DIR" \
    -DTICO_GLSLANG_SOURCE="$TICO_DIR/deps/glslang" -DTICO_GLSLANG_BUILD="$GLSLANG_BUILD" \
    -DTICO_NVK_ARCHIVE="$NVK_ARCHIVE" -DTICO_NVK_DEPS="$NVK_DEPS"
ninja -C "$CORE_BUILD" tico-duckstation.nro

# ============================================================
# Step 4: the tico module
# ============================================================
# A module is a directory that extracts to sdmc:/tico/modules/<id>/: tico
# reads module.json, the settings definition and the strings from it, and
# launches the NRO beside them.
echo "--- Step 4: Packaging the module ---"
BUILD_DIR="$ROOT_DIR/build_tico"
MODULE_SRC="$TICO_DIR/module"
MODULE_ID=$(sed -n 's/.*"id"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$MODULE_SRC/module.json" | head -1)
MODULE_OUT="$BUILD_DIR/module/$MODULE_ID"
rm -rf "$BUILD_DIR"
mkdir -p "$MODULE_OUT"
cp -r "$MODULE_SRC/." "$MODULE_OUT/"
cp "$CORE_BUILD/tico-duckstation.nro" "$MODULE_OUT/"
cp -R "$TICO_DIR/lang" "$MODULE_OUT/"
gzip -f -9 "$MODULE_OUT"/gamelists/*.json 2>/dev/null || true
BUNDLE="$BUILD_DIR/tico-$MODULE_ID-module.zip"
( cd "$BUILD_DIR/module" && zip -qr "$BUNDLE" "$MODULE_ID" )

echo "=== Built ==="
echo "  NRO:    $CORE_BUILD/tico-duckstation.nro"
echo "  Module: $BUNDLE"
echo "          extracts to sdmc:/tico/modules/$MODULE_ID/"
find "$MODULE_OUT" -type f | sed "s|$BUILD_DIR/module/|    |"
