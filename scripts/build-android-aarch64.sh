#!/bin/sh
set -eu

###############################################################################
# Paths / versions
###############################################################################

ROOT="$(pwd)"

NDK_VERSION="android-ndk-r10e"
NDK_ZIP="${NDK_VERSION}-linux-x86_64.zip"

LLVM_VERSION="11.1.0"
LLVM_ARCHIVE="clang+llvm-${LLVM_VERSION}-x86_64-linux-gnu-ubuntu-16.04.tar.xz"
LLVM_DIR="${HOME}/llvm11"

ASTCENC_VERSION="4.8.0"
ASTCENC_DIR="${ROOT}/astc-encoder"
ASTCENC_BUILD="${ROOT}/build-astcenc"
ASTCENC_INSTALL="${ROOT}/astcenc-android"

ANDROID_API="21"
ANDROID_TARGET="aarch64-linux-android${ANDROID_API}"

###############################################################################
# Download Android NDK r10e
###############################################################################

echo
echo "========================================"
echo " Android NDK r10e"
echo "========================================"

if [ ! -d "${ROOT}/${NDK_VERSION}" ]; then
    if [ ! -f "${NDK_ZIP}" ]; then
        wget -nv \
            "https://dl.google.com/android/repository/${NDK_ZIP}" \
            -O "${NDK_ZIP}"
    fi

    unzip -q "${NDK_ZIP}"
fi

export NDK="${ROOT}/${NDK_VERSION}"

if [ ! -d "${NDK}" ]; then
    echo "ERROR: Android NDK not found:"
    echo "  ${NDK}"
    exit 1
fi

echo "NDK:"
echo "  ${NDK}"

###############################################################################
# Download LLVM / Clang 11.1.0
###############################################################################

echo
echo "========================================"
echo " LLVM / Clang ${LLVM_VERSION}"
echo "========================================"

if [ ! -x "${LLVM_DIR}/bin/clang" ]; then

    if [ ! -f "${LLVM_ARCHIVE}" ]; then
        wget -nv \
            "https://github.com/llvm/llvm-project/releases/download/llvmorg-${LLVM_VERSION}/${LLVM_ARCHIVE}" \
            -O "${LLVM_ARCHIVE}"
    fi

    rm -rf "${LLVM_DIR}"
    mkdir -p "${LLVM_DIR}"

    tar -xJf "${LLVM_ARCHIVE}" \
        --strip-components=1 \
        -C "${LLVM_DIR}"
fi

CLANG="${LLVM_DIR}/bin/clang"
CLANGXX="${LLVM_DIR}/bin/clang++"
LLD="${LLVM_DIR}/bin/ld.lld"
LLVM_AR="${LLVM_DIR}/bin/llvm-ar"
LLVM_RANLIB="${LLVM_DIR}/bin/llvm-ranlib"

if [ ! -x "${CLANG}" ]; then
    echo "ERROR: clang not found:"
    echo "  ${CLANG}"
    exit 1
fi

echo "clang:"
"${CLANG}" --version | head -n 1

###############################################################################
# Android r10e toolchain
###############################################################################

echo
echo "========================================"
echo " Android AArch64 toolchain"
echo "========================================"

ANDROID_SYSROOT="${NDK}/platforms/android-${ANDROID_API}/arch-arm64"

ANDROID_GCC_TOOLCHAIN="${NDK}/toolchains/aarch64-linux-android-4.9/prebuilt/linux-x86_64"

ANDROID_AR="${ANDROID_GCC_TOOLCHAIN}/bin/aarch64-linux-android-ar"
ANDROID_RANLIB="${ANDROID_GCC_TOOLCHAIN}/bin/aarch64-linux-android-ranlib"

if [ ! -d "${ANDROID_SYSROOT}" ]; then
    echo "ERROR: Android sysroot not found:"
    echo "  ${ANDROID_SYSROOT}"
    exit 1
fi

if [ ! -d "${ANDROID_GCC_TOOLCHAIN}" ]; then
    echo "ERROR: Android GCC toolchain not found:"
    echo "  ${ANDROID_GCC_TOOLCHAIN}"
    exit 1
fi

###############################################################################
# ASTCENC libc++
#
# IMPORTANT:
# This libc++ is ONLY for ASTCENC.
# It is deliberately NOT exported to Waf.
###############################################################################

echo
echo "========================================"
echo " LLVM libc++ for ASTCENC"
echo "========================================"

ASTC_LIBCXX_INCLUDE="${LLVM_DIR}/include/c++/v1"

if [ ! -d "${ASTC_LIBCXX_INCLUDE}" ]; then
    echo "ERROR: LLVM libc++ headers not found:"
    echo "  ${ASTC_LIBCXX_INCLUDE}"
    exit 1
fi

echo "ASTCENC libc++:"
echo "  ${ASTC_LIBCXX_INCLUDE}"

###############################################################################
# Clone ASTC Encoder 4.8.0
###############################################################################

echo
echo "========================================"
echo " ASTC Encoder ${ASTCENC_VERSION}"
echo "========================================"

if [ ! -d "${ASTCENC_DIR}/.git" ]; then
    git clone \
        --branch "${ASTCENC_VERSION}" \
        --depth 1 \
        https://github.com/ARM-software/astc-encoder.git \
        "${ASTCENC_DIR}"
fi

cd "${ROOT}"

echo "ASTC Encoder commit:"
git -C "${ASTCENC_DIR}" rev-parse HEAD

###############################################################################
# Build ASTC Encoder for Android AArch64
###############################################################################

echo
echo "========================================"
echo " Build ASTC Encoder"
echo "========================================"

rm -rf "${ASTCENC_BUILD}"
rm -rf "${ASTCENC_INSTALL}"

mkdir -p "${ASTCENC_BUILD}"
mkdir -p "${ASTCENC_INSTALL}/include"
mkdir -p "${ASTCENC_INSTALL}/lib"

# IMPORTANT:
# These variables are shell-local and are NOT exported.
# Therefore they cannot leak into Waf.
ASTC_COMMON_FLAGS="\
--target=${ANDROID_TARGET} \
--sysroot=${ANDROID_SYSROOT} \
--gcc-toolchain=${ANDROID_GCC_TOOLCHAIN}"

ASTC_CFLAGS="${ASTC_COMMON_FLAGS}"

ASTC_CXXFLAGS="${ASTC_COMMON_FLAGS} \
-isystem${ASTC_LIBCXX_INCLUDE} \
-stdlib=libc++"

ASTC_LDFLAGS="${ASTC_COMMON_FLAGS} \
-fuse-ld=lld"

cmake \
    -S "${ASTCENC_DIR}" \
    -B "${ASTCENC_BUILD}" \
    -G "Unix Makefiles" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="${CLANG}" \
    -DCMAKE_CXX_COMPILER="${CLANGXX}" \
    -DCMAKE_C_FLAGS="${ASTC_CFLAGS}" \
    -DCMAKE_CXX_FLAGS="${ASTC_CXXFLAGS}" \
    -DCMAKE_EXE_LINKER_FLAGS="${ASTC_LDFLAGS}" \
    -DCMAKE_SHARED_LINKER_FLAGS="${ASTC_LDFLAGS}" \
    -DCMAKE_MODULE_LINKER_FLAGS="${ASTC_LDFLAGS}" \
    -DCMAKE_AR="${LLVM_AR}" \
    -DCMAKE_RANLIB="${LLVM_RANLIB}" \
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
    -DCMAKE_CXX_STANDARD=11 \
    -DCMAKE_CXX_STANDARD_REQUIRED=ON \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DASTCENC_ISA_AVX2=OFF \
    -DASTCENC_ISA_SSE41=OFF \
    -DASTCENC_ISA_SSE2=OFF \
    -DASTCENC_ISA_NEON=ON \
    -DASTCENC_ISA_NONE=OFF \
    -DASTCENC_ISA_NATIVE=OFF \
    -DASTCENC_SHAREDLIB=OFF \
    -DASTCENC_DECOMPRESSOR=OFF \
    -DASTCENC_DIAGNOSTICS=OFF \
    -DASTCENC_ASAN=OFF \
    -DASTCENC_UBSAN=OFF \
    -DASTCENC_UNITTEST=OFF \
    -DASTCENC_CLI=OFF

cmake --build "${ASTCENC_BUILD}" \
    --target astcenc-neon-static \
    --config Release \
    --parallel "$(nproc)"

###############################################################################
# Install ASTC Encoder
###############################################################################

echo
echo "========================================"
echo " Install ASTC Encoder"
echo "========================================"

ASTCENC_HEADER="${ASTCENC_DIR}/Source/astcenc.h"

if [ ! -f "${ASTCENC_HEADER}" ]; then
    echo "ERROR: ASTCENC header not found:"
    echo "  ${ASTCENC_HEADER}"
    exit 1
fi

cp "${ASTCENC_HEADER}" \
    "${ASTCENC_INSTALL}/include/astcenc.h"

ASTCENC_LIBRARY="$(find "${ASTCENC_BUILD}" \
    -type f \
    -name 'libastcenc-neon-static.a' \
    -print -quit)"

if [ -z "${ASTCENC_LIBRARY}" ]; then
    echo "ERROR: ASTCENC static library not found."
    exit 1
fi

cp "${ASTCENC_LIBRARY}" \
    "${ASTCENC_INSTALL}/lib/libastcenc-neon-static.a"

echo
echo "ASTCENC files:"
find "${ASTCENC_INSTALL}" -type f -print

###############################################################################
# Export ASTCENC_ROOT
###############################################################################

export ASTCENC_ROOT="${ASTCENC_INSTALL}"

echo
echo "ASTCENC_ROOT:"
echo "  ${ASTCENC_ROOT}"

if [ ! -f "${ASTCENC_ROOT}/include/astcenc.h" ]; then
    echo "ERROR: astcenc.h missing."
    exit 1
fi

if [ ! -f "${ASTCENC_ROOT}/lib/libastcenc-neon-static.a" ]; then
    echo "ERROR: libastcenc-neon-static.a missing."
    exit 1
fi

###############################################################################
# IMPORTANT:
# ASTCENC compiler environment ENDS HERE.
#
# Nothing from ASTCENC's compiler flags is allowed to leak into Waf.
###############################################################################

echo
echo "========================================"
echo " Reset compiler environment"
echo "========================================"

unset CC
unset CXX
unset CPP
unset AR
unset AS
unset LD
unset RANLIB
unset STRIP
unset OBJC
unset OBJCXX

unset CFLAGS
unset CXXFLAGS
unset CPPFLAGS
unset LDFLAGS

unset SYSROOT
unset CMAKE_C_COMPILER
unset CMAKE_CXX_COMPILER
unset CMAKE_C_FLAGS
unset CMAKE_CXX_FLAGS
unset CMAKE_EXE_LINKER_FLAGS
unset CMAKE_SHARED_LINKER_FLAGS
unset CMAKE_MODULE_LINKER_FLAGS

unset CMAKE_TOOLCHAIN_FILE
unset CMAKE_ANDROID_NDK
unset CMAKE_ANDROID_NDK_VERSION
unset CMAKE_ANDROID_ARCH_ABI
unset CMAKE_ANDROID_API

###############################################################################
# Waf environment
#
# IMPORTANT:
# Waf uses Android NDK r10e libc++.
# Do NOT use LLVM_DIR/include/c++/v1 here.
###############################################################################

echo
echo "========================================"
echo " Prepare Waf environment"
echo "========================================"

chmod +x waf

# Remove GitHub Actions / externally supplied modern NDK variables.
unset ANDROID_NDK
unset ANDROID_NDK_HOME
unset ANDROID_NDK_ROOT
unset ANDROID_NDK_LATEST_HOME

# Waf must use our downloaded NDK r10e.
export ANDROID_NDK="${NDK}"
export ANDROID_NDK_HOME="${NDK}"
export ANDROID_NDK_ROOT="${NDK}"
export NDK_HOME="${NDK}"

###############################################################################
# NDK r10e libc++
###############################################################################

WAF_LIBCXX_INCLUDE="${NDK}/sources/cxx-stl/llvm-libc++/libcxx/include"
WAF_LIBCXXABI_INCLUDE="${NDK}/sources/cxx-stl/llvm-libc++abi/libcxxabi/include"
WAF_ANDROID_SUPPORT_INCLUDE="${NDK}/sources/android/support/include"

if [ ! -d "${WAF_LIBCXX_INCLUDE}" ]; then
    echo "ERROR: NDK r10e libc++ headers not found:"
    echo "  ${WAF_LIBCXX_INCLUDE}"
    exit 1
fi

if [ ! -d "${WAF_LIBCXXABI_INCLUDE}" ]; then
    echo "ERROR: NDK r10e libc++abi headers not found:"
    echo "  ${WAF_LIBCXXABI_INCLUDE}"
    exit 1
fi

if [ ! -d "${WAF_ANDROID_SUPPORT_INCLUDE}" ]; then
    echo "ERROR: Android support headers not found:"
    echo "  ${WAF_ANDROID_SUPPORT_INCLUDE}"
    exit 1
fi

###############################################################################
# Waf compiler tools
###############################################################################

# Keep LLVM 11 first.
export PATH="${LLVM_DIR}/bin:${PATH}"

# Make llvm-strip executable as requested.
chmod +x "${LLVM_DIR}/bin/llvm-strip"

###############################################################################
# Waf C/C++ flags
#
# These are Waf-only flags.
# They are created AFTER the ASTCENC environment was completely reset.
#
# The critical part for the '<new>' error is the NDK r10e libc++ include.
###############################################################################

export CFLAGS="-O2 \
-isystem${WAF_ANDROID_SUPPORT_INCLUDE}"

export CXXFLAGS="-O2 \
-stdlib=libc++ \
-isystem${WAF_LIBCXX_INCLUDE} \
-isystem${WAF_LIBCXXABI_INCLUDE} \
-isystem${WAF_ANDROID_SUPPORT_INCLUDE}"

export LDFLAGS="-s -flto"

###############################################################################
# Waf environment information
###############################################################################

echo
echo "Waf Android environment:"
echo "  ANDROID_NDK      = ${ANDROID_NDK}"
echo "  ANDROID_NDK_HOME = ${ANDROID_NDK_HOME}"
echo "  ANDROID_NDK_ROOT = ${ANDROID_NDK_ROOT}"
echo "  NDK_HOME         = ${NDK_HOME}"
echo "  ASTCENC_ROOT     = ${ASTCENC_ROOT}"

echo
echo "Waf LLVM:"
echo "  LLVM_DIR         = ${LLVM_DIR}"
echo "  clang            = ${CLANG}"
echo "  clang++          = ${CLANGXX}"

echo
echo "Waf libc++:"
echo "  libc++           = ${WAF_LIBCXX_INCLUDE}"
echo "  libc++abi        = ${WAF_LIBCXXABI_INCLUDE}"
echo "  Android support  = ${WAF_ANDROID_SUPPORT_INCLUDE}"

if [ "${ANDROID_NDK}" != "${NDK}" ]; then
    echo
    echo "ERROR: Waf NDK environment is incorrect."
    echo "Expected:"
    echo "  ${NDK}"
    echo "Got:"
    echo "  ${ANDROID_NDK}"
    exit 1
fi

###############################################################################
# Waf configure
###############################################################################

echo
echo "========================================"
echo " Waf configure"
echo "========================================"

./waf configure \
    -T release \
    --build-games=csso \
    --togles \
    --android=aarch64,host,21 \
    --prefix=./output \
    --disable-warns

###############################################################################
# Waf build
###############################################################################

echo
echo "========================================"
echo " Waf build"
echo "========================================"

./waf build

###############################################################################
# Waf install
###############################################################################

echo
echo "========================================"
echo " Waf install"
echo "========================================"

./waf install

###############################################################################
# Success
###############################################################################

echo
echo "========================================"
echo " BUILD SUCCESS"
echo "========================================"

echo "Output:"
echo "  ${ROOT}/output"

echo
echo "ASTCENC:"
echo "  ${ASTCENC_ROOT}/include/astcenc.h"
echo "  ${ASTCENC_ROOT}/lib/libastcenc-neon-static.a"
