#!/bin/sh

set -eu

ROOT="$(pwd)"

echo "========================================"
echo " Android AArch64 build"
echo "========================================"

###############################################################################
# Android NDK r10e
###############################################################################

NDK_VERSION="android-ndk-r10e"
NDK_ZIP="${NDK_VERSION}-linux-x86_64.zip"
NDK_URL="https://dl.google.com/android/repository/${NDK_ZIP}"

if [ ! -d "${ROOT}/${NDK_VERSION}" ]; then
    echo "========================================"
    echo " Downloading Android NDK r10e"
    echo "========================================"

    if [ ! -f "${ROOT}/${NDK_ZIP}" ]; then
        wget -nv "${NDK_URL}"
    fi

    unzip -q "${ROOT}/${NDK_ZIP}"
fi

export ANDROID_NDK_HOME="${ROOT}/${NDK_VERSION}"
export NDK_HOME="${ANDROID_NDK_HOME}"
export NDK="${ANDROID_NDK_HOME}"

echo "NDK: ${NDK}"


###############################################################################
# LLVM / Clang 11.1.0
###############################################################################

LLVM_VERSION="11.1.0"
LLVM_ARCHIVE="clang+llvm-${LLVM_VERSION}-x86_64-linux-gnu-ubuntu-16.04.tar.xz"
LLVM_URL="https://github.com/llvm/llvm-project/releases/download/llvmorg-${LLVM_VERSION}/${LLVM_ARCHIVE}"
LLVM_DIR="${HOME}/llvm11"

if [ ! -x "${LLVM_DIR}/bin/clang" ]; then
    echo "========================================"
    echo " Installing LLVM ${LLVM_VERSION}"
    echo "========================================"

    if [ ! -f "${ROOT}/${LLVM_ARCHIVE}" ]; then
        wget -nv "${LLVM_URL}"
    fi

    rm -rf "${LLVM_DIR}"
    mkdir -p "${LLVM_DIR}"

    tar -xJf "${ROOT}/${LLVM_ARCHIVE}" \
        --strip-components=1 \
        -C "${LLVM_DIR}"
fi

export PATH="${LLVM_DIR}/bin:${PATH}"

chmod +x "${LLVM_DIR}/bin/clang" 2>/dev/null || true
chmod +x "${LLVM_DIR}/bin/clang++" 2>/dev/null || true
chmod +x "${LLVM_DIR}/bin/llvm-strip" 2>/dev/null || true

echo "Clang:"
clang --version


###############################################################################
# Android AArch64 target
###############################################################################

ANDROID_API="21"
ANDROID_ARCH="arm64"
ANDROID_TRIPLE="aarch64-linux-android"

ANDROID_SYSROOT="${NDK}/platforms/android-${ANDROID_API}/arch-${ANDROID_ARCH}"

if [ ! -d "${ANDROID_SYSROOT}" ]; then
    echo "ERROR: Android sysroot does not exist:"
    echo "  ${ANDROID_SYSROOT}"
    exit 1
fi

ANDROID_TARGET="${ANDROID_TRIPLE}${ANDROID_API}"

echo "========================================"
echo " Android target"
echo "========================================"
echo "Target  : ${ANDROID_TARGET}"
echo "Sysroot : ${ANDROID_SYSROOT}"


###############################################################################
# Verify compiler
###############################################################################

ANDROID_CC="${LLVM_DIR}/bin/clang"
ANDROID_CXX="${LLVM_DIR}/bin/clang++"

if [ ! -x "${ANDROID_CC}" ]; then
    echo "ERROR: clang not found:"
    echo "  ${ANDROID_CC}"
    exit 1
fi

if [ ! -x "${ANDROID_CXX}" ]; then
    echo "ERROR: clang++ not found:"
    echo "  ${ANDROID_CXX}"
    exit 1
fi


###############################################################################
# ASTC Encoder 4.8.0
###############################################################################

ASTCENC_VERSION="4.8.0"
ASTCENC_DIR="${ROOT}/astc-encoder"
ASTCENC_BUILD="${ROOT}/build-astcenc"
ASTCENC_INSTALL="${ROOT}/astcenc-android"

echo "========================================"
echo " Building ASTC Encoder ${ASTCENC_VERSION}"
echo "========================================"

if [ ! -d "${ASTCENC_DIR}/.git" ]; then
    git clone \
        --depth 1 \
        --branch "${ASTCENC_VERSION}" \
        https://github.com/ARM-software/astc-encoder.git \
        "${ASTCENC_DIR}"
fi


###############################################################################
# Clean ASTCENC build
###############################################################################

rm -rf "${ASTCENC_BUILD}"
rm -rf "${ASTCENC_INSTALL}"

mkdir -p "${ASTCENC_BUILD}"
mkdir -p "${ASTCENC_INSTALL}"


###############################################################################
# ASTCENC compiler flags
#
# IMPORTANT:
#
# NDK r10e does NOT contain:
#
#   build/cmake/android.toolchain.cmake
#
# Therefore we deliberately do NOT use that toolchain file.
#
# Instead CMake is given the Android target and old NDK sysroot directly.
###############################################################################

ASTCENC_CFLAGS="
--target=${ANDROID_TARGET}
--sysroot=${ANDROID_SYSROOT}
"

ASTCENC_CXXFLAGS="
--target=${ANDROID_TARGET}
--sysroot=${ANDROID_SYSROOT}
"

ASTCENC_LDFLAGS="
--target=${ANDROID_TARGET}
--sysroot=${ANDROID_SYSROOT}
"


###############################################################################
# Configure ASTCENC
###############################################################################

cmake \
    -S "${ASTCENC_DIR}" \
    -B "${ASTCENC_BUILD}" \
    -G "Unix Makefiles" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${ASTCENC_INSTALL}" \
    -DCMAKE_C_COMPILER="${ANDROID_CC}" \
    -DCMAKE_CXX_COMPILER="${ANDROID_CXX}" \
    -DCMAKE_C_FLAGS="${ASTCENC_CFLAGS}" \
    -DCMAKE_CXX_FLAGS="${ASTCENC_CXXFLAGS}" \
    -DCMAKE_EXE_LINKER_FLAGS="${ASTCENC_LDFLAGS}" \
    -DCMAKE_SHARED_LINKER_FLAGS="${ASTCENC_LDFLAGS}" \
    -DCMAKE_MODULE_LINKER_FLAGS="${ASTCENC_LDFLAGS}" \
    -DASTCENC_ISA_NEON=ON \
    -DASTCENC_ISA_SSE2=OFF \
    -DASTCENC_ISA_SSE41=OFF \
    -DASTCENC_ISA_AVX2=OFF \
    -DASTCENC_ISA_NATIVE=OFF \
    -DASTCENC_ISA_NONE=OFF \
    -DASTCENC_SHAREDLIB=OFF \
    -DASTCENC_CLI=OFF \
    -DASTCENC_UNITTEST=OFF


###############################################################################
# Build + install ASTCENC
###############################################################################

cmake \
    --build "${ASTCENC_BUILD}" \
    --target install \
    --config Release \
    --parallel "$(nproc)"


###############################################################################
# Locate ASTCENC output
###############################################################################

echo "========================================"
echo " Checking ASTC Encoder output"
echo "========================================"

ASTCENC_HEADER="${ASTCENC_INSTALL}/include/astcenc.h"

if [ ! -f "${ASTCENC_HEADER}" ]; then
    echo "ERROR: astcenc.h was not installed."
    echo
    echo "Expected:"
    echo "  ${ASTCENC_HEADER}"
    echo
    echo "ASTCENC install tree:"
    find "${ASTCENC_INSTALL}" -maxdepth 4 -type f -print || true
    exit 1
fi


###############################################################################
# Find static ASTCENC library
#
# 4.x Arm builds normally use the NEON static library name:
#
#   libastcenc-neon-static.a
#
# Keep libastcenc.a as a fallback.
###############################################################################

ASTCENC_LIBRARY=""

if [ -f "${ASTCENC_INSTALL}/lib/libastcenc-neon-static.a" ]; then
    ASTCENC_LIBRARY="${ASTCENC_INSTALL}/lib/libastcenc-neon-static.a"
elif [ -f "${ASTCENC_INSTALL}/lib/libastcenc.a" ]; then
    ASTCENC_LIBRARY="${ASTCENC_INSTALL}/lib/libastcenc.a"
elif [ -f "${ASTCENC_INSTALL}/lib64/libastcenc-neon-static.a" ]; then
    ASTCENC_LIBRARY="${ASTCENC_INSTALL}/lib64/libastcenc-neon-static.a"
elif [ -f "${ASTCENC_INSTALL}/lib64/libastcenc.a" ]; then
    ASTCENC_LIBRARY="${ASTCENC_INSTALL}/lib64/libastcenc.a"
fi

if [ -z "${ASTCENC_LIBRARY}" ]; then
    echo "ERROR: ASTCENC static library was not installed."
    echo
    echo "ASTCENC install tree:"
    find "${ASTCENC_INSTALL}" -maxdepth 5 -type f -print || true
    exit 1
fi


###############################################################################
# Determine library directory/name
###############################################################################

ASTCENC_LIB_DIR="$(dirname "${ASTCENC_LIBRARY}")"
ASTCENC_LIB_FILE="$(basename "${ASTCENC_LIBRARY}")"

case "${ASTCENC_LIB_FILE}" in
    lib*.a)
        ASTCENC_LIB_NAME="${ASTCENC_LIB_FILE#lib}"
        ASTCENC_LIB_NAME="${ASTCENC_LIB_NAME%.a}"
        ;;
    *)
        echo "ERROR: Unexpected ASTCENC library name:"
        echo "  ${ASTCENC_LIB_FILE}"
        exit 1
        ;;
esac


###############################################################################
# Verify ASTCENC library architecture
###############################################################################

echo
echo "ASTCENC header:"
echo "  ${ASTCENC_HEADER}"

echo "ASTCENC library:"
echo "  ${ASTCENC_LIBRARY}"

echo
echo "ASTCENC library architecture:"

file "${ASTCENC_LIBRARY}" || true


###############################################################################
# Export ASTCENC_ROOT for Waf
###############################################################################

export ASTCENC_ROOT="${ASTCENC_INSTALL}"

echo
echo "========================================"
echo " ASTCENC ready"
echo "========================================"
echo "ASTCENC_ROOT : ${ASTCENC_ROOT}"
echo "Include      : ${ASTCENC_INSTALL}/include"
echo "Library      : ${ASTCENC_LIBRARY}"
echo "Library name : ${ASTCENC_LIB_NAME}"
echo


###############################################################################
# Waf
###############################################################################

chmod +x waf

export CFLAGS="-O2"
export CXXFLAGS="-O2"
export LDFLAGS="-s -flto"

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
echo " BUILD COMPLETE"
echo "========================================"
