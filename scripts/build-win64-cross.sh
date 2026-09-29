#!/bin/sh
# Cross-compile a statically linked openFPGALoader.exe for Windows (x86_64)
# from Linux using MinGW-w64.
#
# Debian/Ubuntu prerequisites:
#   sudo apt install mingw-w64 libz-mingw-w64-dev cmake pkg-config p7zip-full
#
# Output: build-win64-cross/openFPGALoader.exe
set -e

cd "$(dirname "$0")/.."

BUILD_DIR=${BUILD_DIR:-build-win64-cross}
MINGW_PREFIX=${MINGW_PREFIX:-/usr/x86_64-w64-mingw32}

# CMake < 3.24 ignores ZLIB_USE_STATIC_LIBS and selects libz.dll.a, which
# fails the WINDOWS_STATIC_ZLIB check: point it at the static archive.
ZLIB_ARGS=
if [ -f "$MINGW_PREFIX/lib/libz.a" ]; then
	ZLIB_ARGS="-DZLIB_LIBRARY=$MINGW_PREFIX/lib/libz.a -DZLIB_LIBRARY_RELEASE=$MINGW_PREFIX/lib/libz.a"
fi

cmake -S . -B "$BUILD_DIR" \
	-DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/Toolchain-x86_64-w64-mingw32.cmake" \
	-DWINDOWS_CROSSCOMPILE=ON \
	-DCROSS_COMPILE_DEPS=ON \
	-DCMAKE_BUILD_TYPE=Release \
	$ZLIB_ARGS

cmake --build "$BUILD_DIR" --parallel "$(nproc)"
x86_64-w64-mingw32-strip "$BUILD_DIR/openFPGALoader.exe"
