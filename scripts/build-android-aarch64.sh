#!/bin/sh

set -eu

###############################################################################
# Configuration
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
ANDROID_TRIPLE="aarch64-linux-android"
ANDROID_TARGET="${ANDROID_TRIPLE}${ANDROID_API}"

###############################################################################
# Header
###############################################################################

echo
echo "========================================"
echo " Android AArch64 build"
echo "========================================"
echo

###############################################################################
# Download Android NDK r10e
###############################################################################

echo "========================================"
echo " Android NDK r10e"
echo "========================================"

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

echo "NDK:"
echo "  ${NDK}"

###############################################################################
# Verify NDK
###############################################################################

ANDROID_SYSROOT="${NDK}/platforms/android-${ANDROID_API}/arch-arm64"

ANDROID_GCC_TOOLCHAIN="${NDK}/toolchains/aarch64-linux-android-4.9/prebuilt/linux-x86_64"

if [ ! -d "${ANDROID_SYSROOT}" ]; then
    echo
    echo "ERROR:"
    echo "Android AArch64 sysroot was not found:"
    echo
    echo "  ${ANDROID_SYSROOT}"
    echo
    exit 1
fi

if [ ! -d "${ANDROID_GCC_TOOLCHAIN}" ]; then
    echo
    echo "ERROR:"
    echo "Android AArch64 GCC 4.9 toolchain was not found:"
    echo
    echo "  ${ANDROID_GCC_TOOLCHAIN}"
    echo
    exit 1
fi

echo "Android sysroot:"
echo "  ${ANDROID_SYSROOT}"

echo "Android GCC toolchain:"
echo "  ${ANDROID_GCC_TOOLCHAIN}"

###############################################################################
# LLVM 11.1.0
###############################################################################

echo
echo "========================================"
echo " LLVM ${LLVM_VERSION}"
echo "========================================"

if [ ! -x "${LLVM_DIR}/bin/clang" ]; then

    echo "Downloading LLVM ${LLVM_VERSION}..."

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

echo
echo "Clang:"
"${ANDROID_CC}" --version | head -n 1

###############################################################################
# Check LLVM LLD
###############################################################################

echo
echo "========================================"
echo " Checking LLVM linker"
echo "========================================"

if [ ! -x "${LLVM_DIR}/bin/ld.lld" ]; then
    echo
    echo "ERROR: LLVM lld was not found:"
    echo
    echo "  ${LLVM_DIR}/bin/ld.lld"
    echo
    echo "Contents of LLVM bin directory:"
    ls -la "${LLVM_DIR}/bin" | grep -E 'ld|lld' || true
    exit 1
fi

echo "LLD:"
"${LLVM_DIR}/bin/ld.lld" --version

###############################################################################
# Check GNU binutils from NDK r10e
###############################################################################

echo
echo "========================================"
echo " Checking Android binutils"
echo "========================================"

ANDROID_AR="${ANDROID_GCC_TOOLCHAIN}/bin/aarch64-linux-android-ar"
ANDROID_RANLIB="${ANDROID_GCC_TOOLCHAIN}/bin/aarch64-linux-android-ranlib"
ANDROID_STRIP="${ANDROID_GCC_TOOLCHAIN}/bin/aarch64-linux-android-strip"

if [ ! -x "${ANDROID_AR}" ]; then
    echo "ERROR: Android ar not found:"
    echo "  ${ANDROID_AR}"
    exit 1
fi

if [ ! -x "${ANDROID_RANLIB}" ]; then
    echo "ERROR: Android ranlib not found:"
    echo "  ${ANDROID_RANLIB}"
    exit 1
fi

echo "AR:"
echo "  ${ANDROID_AR}"

echo "RANLIB:"
echo "  ${ANDROID_RANLIB}"

###############################################################################
# IMPORTANT:
#
# NDK r10e does not contain:
#
#   ${NDK}/build/cmake/android.toolchain.cmake
#
# We deliberately DO NOT use that file.
###############################################################################

ANDROID_TOOLCHAIN_FILE="${NDK}/build/cmake/android.toolchain.cmake"

if [ -f "${ANDROID_TOOLCHAIN_FILE}" ]; then
    echo
    echo "ERROR: unexpected Android CMake toolchain file found:"
    echo
    echo "  ${ANDROID_TOOLCHAIN_FILE}"
    echo
    exit 1
fi

###############################################################################
# ASTC Encoder source
###############################################################################

echo
echo "========================================"
echo " ASTC Encoder ${ASTCENC_VERSION}"
echo "========================================"

if [ ! -d "${ASTCENC_DIR}/.git" ]; then

    git clone \
        --depth 1 \
        --branch "${ASTCENC_VERSION}" \
        https://github.com/ARM-software/astc-encoder.git \
        "${ASTCENC_DIR}"
fi

###############################################################################
# Verify ASTCENC version
###############################################################################

ASTCENC_COMMIT="$(git -C "${ASTCENC_DIR}" rev-parse HEAD)"

echo
echo "ASTCENC commit:"
echo "  ${ASTCENC_COMMIT}"

###############################################################################
# Clean ASTCENC build
###############################################################################

rm -rf "${ASTCENC_BUILD}"
rm -rf "${ASTCENC_INSTALL}"

mkdir -p "${ASTCENC_BUILD}"
mkdir -p "${ASTCENC_INSTALL}"

###############################################################################
# ASTCENC compiler flags
###############################################################################

ASTCENC_CFLAGS="
--target=${ANDROID_TARGET}
--sysroot=${ANDROID_SYSROOT}
--gcc-toolchain=${ANDROID_GCC_TOOLCHAIN}
"

ASTCENC_CXXFLAGS="
--target=${ANDROID_TARGET}
--sysroot=${ANDROID_SYSROOT}
--gcc-toolchain=${ANDROID_GCC_TOOLCHAIN}
"

ASTCENC_LDFLAGS="
--target=${ANDROID_TARGET}
--sysroot=${ANDROID_SYSROOT}
--gcc-toolchain=${ANDROID_GCC_TOOLCHAIN}
-fuse-ld=lld
"

###############################################################################
# Show compiler target
###############################################################################

echo
echo "========================================"
echo " Compiler target"
echo "========================================"

echo "Target:"
echo "  ${ANDROID_TARGET}"

echo
echo "Sysroot:"
echo "  ${ANDROID_SYSROOT}"

echo
echo "GCC toolchain:"
echo "  ${ANDROID_GCC_TOOLCHAIN}"

echo
echo "CFLAGS:"
echo "  ${ASTCENC_CFLAGS}"

echo
echo "CXXFLAGS:"
echo "  ${ASTCENC_CXXFLAGS}"

echo
echo "LDFLAGS:"
echo "  ${ASTCENC_LDFLAGS}"

###############################################################################
# CMake ASTCENC configuration
###############################################################################

echo
echo "========================================"
echo " CMake ASTCENC configuration"
echo "========================================"

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
    -DCMAKE_AR="${ANDROID_AR}" \
    -DCMAKE_RANLIB="${ANDROID_RANLIB}" \
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
    -DASTCENC_ISA_NEON=ON \
    -DASTCENC_ISA_SSE2=OFF \
    -DASTCENC_ISA_SSE41=OFF \
    -DASTCENC_ISA_AVX2=OFF \
    -DASTCENC_CLI=OFF \
    -DASTCENC_UNITTEST=OFF

###############################################################################
# Build ASTCENC
###############################################################################

echo
echo "========================================"
echo " Building ASTC Encoder"
echo "========================================"

cmake \
    --build "${ASTCENC_BUILD}" \
    --target install \
    --config Release \
    --parallel "$(nproc)"

###############################################################################
# Find ASTCENC header
###############################################################################

echo
echo "========================================"
echo " Verifying ASTC Encoder"
echo "========================================"

ASTCENC_HEADER="${ASTCENC_INSTALL}/include/astcenc.h"

if [ ! -f "${ASTCENC_HEADER}" ]; then

    echo
    echo "ERROR: astcenc.h was not installed."
    echo
    echo "Expected:"
    echo "  ${ASTCENC_HEADER}"
    echo
    echo "ASTCENC installation tree:"
    find "${ASTCENC_INSTALL}" -maxdepth 5 -type f -print || true
    echo

    exit 1
fi

###############################################################################
# Find ASTCENC library
###############################################################################

ASTCENC_LIBRARY=""

for LIB in \
    "${ASTCENC_INSTALL}/lib/libastcenc-neon-static.a" \
    "${ASTCENC_INSTALL}/lib/libastcenc.a" \
    "${ASTCENC_INSTALL}/lib64/libastcenc-neon-static.a" \
    "${ASTCENC_INSTALL}/lib64/libastcenc.a"
do
    if [ -f "${LIB}" ]; then
        ASTCENC_LIBRARY="${LIB}"
        break
    fi
done

if [ -z "${ASTCENC_LIBRARY}" ]; then

    echo
    echo "ERROR: ASTCENC static library was not found."
    echo
    echo "ASTCENC installation tree:"
    find "${ASTCENC_INSTALL}" -maxdepth 5 -type f -print || true
    echo

    exit 1
fi

###############################################################################
# Determine library directory
###############################################################################

ASTCENC_LIB_DIR="$(dirname "${ASTCENC_LIBRARY}")"
ASTCENC_LIB_FILE="$(basename "${ASTCENC_LIBRARY}")"

case "${ASTCENC_LIB_FILE}" in

    lib*.a)
        ASTCENC_LIB_NAME="${ASTCENC_LIB_FILE#lib}"
        ASTCENC_LIB_NAME="${ASTCENC_LIB_NAME%.a}"
        ;;

    *)
        echo
        echo "ERROR: unexpected ASTCENC library:"
        echo "  ${ASTCENC_LIB_FILE}"
        exit 1
        ;;

esac

###############################################################################
# Verify architecture
###############################################################################

echo
echo "ASTCENC header:"
echo "  ${ASTCENC_HEADER}"

echo
echo "ASTCENC library:"
echo "  ${ASTCENC_LIBRARY}"

echo
echo "ASTCENC library type:"
file "${ASTCENC_LIBRARY}" || true

###############################################################################
# Verify that the library is actually AArch64
###############################################################################

if command -v llvm-readelf >/dev/null 2>&1; then

    echo
    echo "ASTCENC object architecture:"
    llvm-readelf -h "${ASTCENC_LIBRARY}" 2>/dev/null || true

fi

###############################################################################
# Export ASTCENC_ROOT
###############################################################################

export ASTCENC_ROOT="${ASTCENC_INSTALL}"

echo
echo "========================================"
echo " ASTCENC ready"
echo "========================================"

echo "ASTCENC_ROOT:"
echo "  ${ASTCENC_ROOT}"

echo
echo "ASTCENC include:"
echo "  ${ASTCENC_INSTALL}/include"

echo
echo "ASTCENC lib:"
echo "  ${ASTCENC_LIB_DIR}"

echo
echo "ASTCENC library:"
echo "  ${ASTCENC_LIB_FILE}"

###############################################################################
# Clean possible CMake environment variables
#
# This prevents GitHub Actions or the runner environment from injecting an
# old CMAKE_TOOLCHAIN_FILE.
###############################################################################

unset CMAKE_TOOLCHAIN_FILE || true
unset CMAKE_GENERATOR_PLATFORM || true

###############################################################################
# Waf
###############################################################################

echo
echo "========================================"
echo " Waf"
echo "========================================"

chmod +x waf

###############################################################################
# Build flags
###############################################################################

export CFLAGS="-O2"
export CXXFLAGS="-O2"
export LDFLAGS="-s -flto"

###############################################################################
# Waf configure
###############################################################################

echo
echo "========================================"
echo " Waf configure"
echo "========================================"

echo "ASTCENC_ROOT:"
echo "  ${ASTCENC_ROOT}"

./waf configure \
    -T release \
    --build-games=csso \
    --toggles \
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
# Done
###############################################################################

echo
echo "========================================"
echo " BUILD COMPLETE"
echo "========================================"

echo
echo "ASTCENC:"
echo "  ${ASTCENC_LIBRARY}"

echo
echo "Output:"
echo "  ${ROOT}/output"

echo
