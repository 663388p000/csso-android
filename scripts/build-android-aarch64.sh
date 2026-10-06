#!/bin/sh

set -e

ROOT="$PWD"

echo "========================================"
echo " Android AArch64 build"
echo "========================================"

# ------------------------------------------------------------
# Android NDK
# ------------------------------------------------------------

wget -nv \
	https://dl.google.com/android/repository/android-ndk-r10e-linux-x86_64.zip \
	-o /dev/null

unzip -q android-ndk-r10e-linux-x86_64.zip

export ANDROID_NDK_HOME="$ROOT/android-ndk-r10e"
export NDK_HOME="$ROOT/android-ndk-r10e"
export NDK="$NDK_HOME"

# ------------------------------------------------------------
# LLVM 11
# ------------------------------------------------------------

wget -nv \
	https://github.com/llvm/llvm-project/releases/download/llvmorg-11.1.0/clang+llvm-11.1.0-x86_64-linux-gnu-ubuntu-16.04.tar.xz

mkdir -p "$HOME/llvm11"

tar -xJf \
	clang+llvm-11.1.0-x86_64-linux-gnu-ubuntu-16.04.tar.xz \
	--strip-components=1 \
	-C "$HOME/llvm11"

chmod +x "$HOME/llvm11/bin/llvm-strip"

export PATH="$HOME/llvm11/bin:$PATH"

# ------------------------------------------------------------
# ASTC Encoder
# ------------------------------------------------------------
#
# IMPORTANT:
# This is built FOR Android AArch64.
#
# Do NOT use the host libastcenc-dev package.
#

ASTCENC_VERSION="4.8.0"
ASTCENC_DIR="$ROOT/astc-encoder"
ASTCENC_BUILD="$ROOT/build-astcenc"
ASTCENC_INSTALL="$ROOT/astcenc-android"

echo "========================================"
echo " Building ASTC Encoder $ASTCENC_VERSION"
echo "========================================"

if [ ! -d "$ASTCENC_DIR" ]; then
	git clone \
		--depth 1 \
		--branch "$ASTCENC_VERSION" \
		https://github.com/ARM-software/astc-encoder.git \
		"$ASTCENC_DIR"
fi

rm -rf "$ASTCENC_BUILD"
rm -rf "$ASTCENC_INSTALL"

mkdir -p "$ASTCENC_BUILD"
mkdir -p "$ASTCENC_INSTALL"

# ------------------------------------------------------------
# Build ASTCENC with Android NDK
# ------------------------------------------------------------

cmake \
	-S "$ASTCENC_DIR" \
	-B "$ASTCENC_BUILD" \
	-G "Unix Makefiles" \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX="$ASTCENC_INSTALL" \
	-DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
	-DANDROID_ABI=arm64-v8a \
	-DANDROID_PLATFORM=android-21 \
	-DANDROID_TOOLCHAIN=clang \
	-DANDROID_STL=c++_static \
	-DASTCENC_ISA_NEON=ON \
	-DASTCENC_ISA_SSE2=OFF \
	-DASTCENC_ISA_SSE41=OFF \
	-DASTCENC_ISA_AVX2=OFF \
	-DASTCENC_CLI=OFF \
	-DASTCENC_UNITTEST=OFF \
	-DASTCENC_SHAREDLIB=OFF

cmake \
	--build "$ASTCENC_BUILD" \
	--target install \
	--config Release \
	-j"$(nproc)"

# ------------------------------------------------------------
# Verify ASTCENC
# ------------------------------------------------------------

echo "========================================"
echo " Verifying ASTCENC"
echo "========================================"

if [ ! -f "$ASTCENC_INSTALL/include/astcenc.h" ]; then
	echo "ERROR: astcenc.h was not installed"
	exit 1
fi

if [ ! -f "$ASTCENC_INSTALL/lib/libastcenc.a" ] &&
   [ ! -f "$ASTCENC_INSTALL/lib/libastcenc-neon-static.a" ]; then
	echo "ERROR: ASTCENC static library was not installed"
	exit 1
fi

echo "ASTCENC header:"
echo "  $ASTCENC_INSTALL/include/astcenc.h"

echo "ASTCENC libraries:"
find "$ASTCENC_INSTALL/lib" -maxdepth 1 -type f -name '*astcenc*.a' -print

# ------------------------------------------------------------
# Export ASTCENC paths for Waf
# ------------------------------------------------------------

export ASTCENC_ROOT="$ASTCENC_INSTALL"

export CFLAGS="-O2"
export CXXFLAGS="-O2"
export LDFLAGS="-s -flto"

# ------------------------------------------------------------
# Waf
# ------------------------------------------------------------

chmod +x waf

echo "========================================"
echo " Configuring Waf"
echo "========================================"

./waf configure \
	-T release \
	--build-games=csso \
	--togles \
	--android=aarch64,host,21 \
	--prefix=./output \
	--disable-warns

echo "========================================"
echo " Building"
echo "========================================"

./waf build

echo "========================================"
echo " Installing"
echo "========================================"

./waf install

echo "========================================"
echo " DONE"
echo "========================================"
