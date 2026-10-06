#!/bin/sh
set -eu

ROOT="$(pwd)"

echo "========================================"
echo " Android AArch64 + ASTCENC build"
echo "========================================"

###############################################################################
# NDK r10e
###############################################################################

NDK_VERSION="android-ndk-r10e"
NDK_ZIP="${NDK_VERSION}-linux-x86_64.zip"

if [ ! -d "${ROOT}/${NDK_VERSION}" ]; then
    echo "Downloading NDK r10e..."

    if [ ! -f "${ROOT}/${NDK_ZIP}" ]; then
        wget -nv \
            "https://dl.google.com/android/repository/${NDK_ZIP}"
    fi

    unzip -q "${ROOT}/${NDK_ZIP}"
fi

export NDK="${ROOT}/${NDK_VERSION}"
export NDK_HOME="${NDK}"
export ANDROID_NDK_HOME="${NDK}"

###############################################################################
# LLVM 11.1.0
###############################################################################

LLVM_VERSION="11.1.0"
LLVM_ARCHIVE="clang+llvm-${LLVM_VERSION}-x86_64-linux-gnu-ubuntu-16.04.tar.xz"
LLVM_DIR="${HOME}/llvm11"

if [ ! -x "${LLVM_DIR}/bin/clang" ]; then
    echo "Installing LLVM ${LLVM_VERSION}..."

    if [ ! -f "${ROOT}/${LLVM_ARCHIVE}" ]; then
        wget -nv \
            "https://github.com/llvm/llvm-project/releases/download/llvmorg-${LLVM_VERSION}/${LLVM_ARCHIVE}"
    fi

    rm -rf "${LLVM_DIR}"
    mkdir -p "${LLVM_DIR}"

    tar -xJf "${ROOT}/${LLVM_ARCHIVE}" \
        --strip-components=1 \
        -C "${LLVM_DIR}"
fi

export PATH="${LLVM_DIR}/bin:${PATH}"

###############################################################################
# Android target
###############################################################################

ANDROID_API=21
ANDROID_TARGET="aarch64-linux-android${ANDROID_API}"
ANDROID_SYSROOT="${NDK}/platforms/android-${ANDROID_API}/arch-arm64"

echo
echo "NDK:"
echo "  ${NDK}"

echo
echo "Clang:"
clang --version | head -n 1

echo
echo "Android target:"
echo "  ${ANDROID_TARGET}"

echo
echo "Sysroot:"
echo "  ${ANDROID_SYSROOT}"

if [ ! -d "${ANDROID_SYSROOT}" ]; then
    echo "ERROR: Android sysroot does not exist:"
    echo "${ANDROID_SYSROOT}"
    exit 1
fi

###############################################################################
# IMPORTANT:
# NDK r10e does NOT have build/cmake/android.toolchain.cmake.
#
# We deliberately do NOT use it.
###############################################################################

ANDROID_TOOLCHAIN_FILE="${NDK}/build/cmake/android.toolchain.cmake"

if [ -f "${ANDROID_TOOLCHAIN_FILE}" ]; then
    echo "ERROR: unexpected Android CMake toolchain detected:"
    echo "${ANDROID_TOOLCHAIN_FILE}"
    exit 1
fi

###############################################################################
# ASTC Encoder
###############################################################################

ASTCENC_VERSION="4.8.0"
ASTCENC_DIR="${ROOT}/astc-encoder"
ASTCENC_BUILD="${ROOT}/build-astcenc"
ASTCENC_INSTALL="${ROOT}/astcenc-android"

echo
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

rm -rf "${ASTCENC_BUILD}"
rm -rf "${ASTCENC_INSTALL}"

mkdir -p "${ASTCENC_BUILD}"
mkdir -p "${ASTCENC_INSTALL}"

###############################################################################
# Android compiler
###############################################################################

ANDROID_CC="${LLVM_DIR}/bin/clang"
ANDROID_CXX="${LLVM_DIR}/bin/clang++"

ASTCENC_CFLAGS="--target=${ANDROID_TARGET} --sysroot=${ANDROID_SYSROOT}"
ASTCENC_CXXFLAGS="--target=${ANDROID_TARGET} --sysroot=${ANDROID_SYSROOT}"
ASTCENC_LDFLAGS="--target=${ANDROID_TARGET} --sysroot=${ANDROID_SYSROOT}"

echo
echo "========================================"
echo " CMake ASTCENC configuration"
echo "========================================"

echo "Compiler:"
echo "  ${ANDROID_CC}"

echo "Target:"
echo "  ${ANDROID_TARGET}"

echo "CFLAGS:"
echo "  ${ASTCENC_CFLAGS}"

###############################################################################
# IMPORTANT:
#
# There is intentionally NO:
#
#   -DCMAKE_TOOLCHAIN_FILE=...
#
# here.
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
    -DASTCENC_ISA_NEON=ON \
    -DASTCENC_ISA_SSE2=OFF \
    -DASTCENC_ISA_SSE41=OFF \
    -DASTCENC_ISA_AVX2=OFF \
    -DASTCENC_CLI=OFF \
    -DASTCENC_UNITTEST=OFF

###############################################################################
# Build
###############################################################################

echo
echo "========================================"
echo " Building ASTCENC"
echo "========================================"

cmake \
    --build "${ASTCENC_BUILD}" \
    --target install \
    --config Release \
    --parallel "$(nproc)"

###############################################################################
# Verify
###############################################################################

echo
echo "========================================"
echo " Verifying ASTCENC"
echo "========================================"

ASTCENC_HEADER="${ASTCENC_INSTALL}/include/astcenc.h"

if [ ! -f "${ASTCENC_HEADER}" ]; then
    echo "ERROR: astcenc.h was not installed:"
    echo "${ASTCENC_HEADER}"
    find "${ASTCENC_INSTALL}" -type f -print || true
    exit 1
fi

ASTCENC_LIBRARY=""

for lib in \
    "${ASTCENC_INSTALL}/lib/libastcenc-neon-static.a" \
    "${ASTCENC_INSTALL}/lib/libastcenc.a" \
    "${ASTCENC_INSTALL}/lib64/libastcenc-neon-static.a" \
    "${ASTCENC_INSTALL}/lib64/libastcenc.a"
do
    if [ -f "${lib}" ]; then
        ASTCENC_LIBRARY="${lib}"
        break
    fi
done

if [ -z "${ASTCENC_LIBRARY}" ]; then
    echo "ERROR: ASTCENC static library not found."

    echo
    echo "Installed files:"
    find "${ASTCENC_INSTALL}" -type f -print || true

    exit 1
fi

echo "Header:"
echo "  ${ASTCENC_HEADER}"

echo "Library:"
echo "  ${ASTCENC_LIBRARY}"

echo
echo "Library architecture:"
file "${ASTCENC_LIBRARY}"

###############################################################################
# ASTCENC_ROOT
###############################################################################

export ASTCENC_ROOT="${ASTCENC_INSTALL}"

echo
echo "ASTCENC_ROOT:"
echo "  ${ASTCENC_ROOT}"

###############################################################################
# Waf
###############################################################################

chmod +x waf

export CFLAGS="-O2"
export CXXFLAGS="-O2"
export LDFLAGS="-s -flto"

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

echo
echo "========================================"
echo " Waf build"
echo "========================================"

./waf build

echo
echo "========================================"
echo " Waf install"
echo "========================================"

./waf install

echo
echo "========================================"
echo " BUILD COMPLETE"
echo "========================================"
