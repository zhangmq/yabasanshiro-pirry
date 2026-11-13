#!/bin/sh
# Build the libretro core (yabasanshiro_libretro.so).
#
#   ./build-libretro.sh [--release] [--clean] [--build DIR] [--toolchain FILE] [cmake args...]
#
#   --build DIR       build directory              (default: ./build-libretro)
#   --clean           remove the build directory first
#   --release         wipe the dependency cache first, so every source is
#                     fetched online - this is what a release build must prove
#   --toolchain FILE  CMake toolchain file (default: none, i.e. build with the
#                     host compiler - only meaningful on an AArch64 Linux host,
#                     the project hard-codes AARCH64; see yabause/src/libretro)
#
# Dependencies are the SAME sources and pins the standalone uses (libchdr
# 074ff16, zlib 5089329 = 1.2.8), see yabause/src/libretro/CMakeLists.txt.
# They live in a cache directory (default: ./deps):
#   dev (default)  reuse the cache: fetch only what is missing, never touch the
#                  network for a source that is already there at the pin.
#   --release      wipe the cache first, so everything is fetched online.
#
# A cross build also needs the vendor GL/EGL sysroot: the toolchain file and the
# CMake project read H700_DEPS (see yabause/src/libretro/CMakeLists.txt).
set -eu
export CMAKE_POLICY_VERSION_MINIMUM=${CMAKE_POLICY_VERSION_MINIMUM:-3.5}
ROOT=$(cd "$(dirname "$0")" && pwd)
BUILD=$ROOT/build-libretro
DEPS=${YAB_DEPS_CACHE:-$ROOT/deps}
TOOLCHAIN=${YAB_TOOLCHAIN_FILE:-}
RELEASE=0
while [ $# -gt 0 ]; do
  case "$1" in
    --release)   RELEASE=1; shift ;;
    --clean)     rm -rf "$BUILD"; shift ;;
    --build)     BUILD=$2; shift 2 ;;
    --toolchain) TOOLCHAIN=$2; shift 2 ;;
    *) break ;;
  esac
done

LIBCHDR_URL=https://github.com/devmiyax/libchdr.git
LIBCHDR_PIN=074ff1614f2a685f2b5a95b0e788bff6297d5680
ZLIB_URL=https://github.com/madler/zlib.git
ZLIB_PIN=50893291621658f355bc5b4d450a8d06a563053d

if [ "$RELEASE" = 1 ]; then
  echo "release build: clearing the dependency cache at $DEPS"
  rm -rf "$DEPS"
fi
mkdir -p "$DEPS"

fetch() {   # fetch <dir> <url> <pin>
  d=$1; url=$2; pin=$3
  if [ -f "$d/CMakeLists.txt" ] && git -C "$d" rev-parse --verify -q HEAD >/dev/null 2>&1; then
    cur=$(git -C "$d" rev-parse HEAD)
    if [ "$cur" = "$pin" ]; then
      echo "$(basename "$d"): cache hit $(echo "$cur" | cut -c1-7)"
      return
    fi
  fi
  rm -rf "$d"
  git clone --quiet "$url" "$d"
  git -C "$d" checkout --quiet "$pin"
  echo "$(basename "$d"): fetched $(git -C "$d" rev-parse --short HEAD)"
}

fetch "$DEPS/libchdr" "$LIBCHDR_URL" "$LIBCHDR_PIN"
fetch "$DEPS/zlib"    "$ZLIB_URL"    "$ZLIB_PIN"

# musashi's generated sources (m68kops*.c / m68kop*.c) are a pure function of
# m68k_in.c: build m68kmake with the host compiler and run it. No network and no
# other build tree involved.
if [ ! -f "$DEPS/musashi/m68kops.c" ]; then
  mkdir -p "$DEPS/musashi" "$BUILD/.tools"
  HOSTCC=${HOSTCC:-cc}
  "$HOSTCC" -O2 -o "$BUILD/.tools/m68kmake" "$ROOT/yabause/src/musashi/m68kmake.c"
  "$BUILD/.tools/m68kmake" "$DEPS/musashi/" "$ROOT/yabause/src/musashi/m68k_in.c"
  echo "musashi: generated m68kops*.c in $DEPS/musashi"
fi

# CMake resolves -DCMAKE_TOOLCHAIN_FILE and -B relative to the build directory,
# so make both absolute here.
abs() { case $1 in /*) echo "$1" ;; *) echo "$(cd "$(dirname "$1")" && pwd)/$(basename "$1")" ;; esac; }
case $TOOLCHAIN in "") ;; *) TOOLCHAIN=$(abs "$TOOLCHAIN") ;; esac
BUILD=$(abs "$BUILD")

JOBS=${JOBS:-$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}

set -- -S "$ROOT/yabause/src/libretro" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release \
       -DYAB_DEPS_CACHE="$DEPS" "$@"
if [ -n "$TOOLCHAIN" ]; then
  set -- "$@" -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN"
fi

cmake "$@"
cmake --build "$BUILD" -j"$JOBS"
ls -l "$BUILD/yabasanshiro_libretro.so"
