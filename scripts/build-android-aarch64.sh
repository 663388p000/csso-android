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
# Android NDK r10e
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
# Android sysroot
###############################################################################

ANDROID_SYSROOT="${NDK}/platforms/android-${ANDROID_API}/arch-arm64"

ANDROID_GCC_TOOLCHAIN="${NDK}/toolchains/aarch64-linux-android-4.9/prebuilt/linux-x86_64"

if [ ! -d "${ANDROID_SYSROOT}" ]; then
    echo
    echo "ERROR: Android AArch64 sysroot not found:"
    echo "  ${ANDROID_SYSROOT}"
    exit 1
fi

if [ ! -d "${ANDROID_GCC_TOOLCHAIN}" ]; then
    echo
    echo "ERROR: Android AArch64 GCC toolchain not found:"
    echo "  ${ANDROID_GCC_TOOLCHAIN}"
    exit 1
fi

echo "Android target:"
echo "  ${ANDROID_TARGET}"

echo "Android sysroot:"
echo "  ${ANDROID_SYSROOT}"

echo "Android GCC toolchain:"
echo "  ${ANDROID_GCC_TOOLCHAIN}"

###############################################################################
# LLVM / Clang 11.1.0
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
# LLVM LLD
###############################################################################

echo
echo "========================================"
echo " LLVM LLD"
echo "========================================"

ANDROID_LLD="${LLVM_DIR}/bin/ld.lld"

if [ ! -x "${ANDROID_LLD}" ]; then
    echo
    echo "ERROR: ld.lld not found:"
    echo "  ${ANDROID_LLD}"
    exit 1
fi

echo "LLD:"
"${ANDROID_LLD}" --version

###############################################################################
# Android binutils
###############################################################################

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

###############################################################################
# NDK r10e must NOT use modern CMake Android toolchain
###############################################################################

ANDROID_TOOLCHAIN_FILE="${NDK}/build/cmake/android.toolchain.cmake"

if [ -f "${ANDROID_TOOLCHAIN_FILE}" ]; then
    echo
    echo "ERROR: unexpected modern Android CMake toolchain found:"
    echo "  ${ANDROID_TOOLCHAIN_FILE}"
    exit 1
fi

###############################################################################
# Clear externally supplied CMake Android toolchain
###############################################################################

unset CMAKE_TOOLCHAIN_FILE || true
unset CMAKE_GENERATOR_PLATFORM || true
unset CMAKE_ANDROID_ARCH_ABI || true
unset CMAKE_ANDROID_API || true
unset CMAKE_ANDROID_NDK || true

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
# Verify ASTCENC commit
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
mkdir -p "${ASTCENC_INSTALL}/include"
mkdir -p "${ASTCENC_INSTALL}/lib"

###############################################################################
# Compiler flags
###############################################################################

ASTCENC_CFLAGS="--target=${ANDROID_TARGET} --sysroot=${ANDROID_SYSROOT} --gcc-toolchain=${ANDROID_GCC_TOOLCHAIN}"

ASTCENC_CXXFLAGS="--target=${ANDROID_TARGET} --sysroot=${ANDROID_SYSROOT} --gcc-toolchain=${ANDROID_GCC_TOOLCHAIN}"

ASTCENC_LDFLAGS="--target=${ANDROID_TARGET} --sysroot=${ANDROID_SYSROOT} --gcc-toolchain=${ANDROID_GCC_TOOLCHAIN} -fuse-ld=lld"

###############################################################################
# CMake configuration
###############################################################################

echo
echo "========================================"
echo " CMake ASTCENC configuration"
echo "========================================"

echo "Compiler:"
echo "  ${ANDROID_CC}"

echo "C++ compiler:"
echo "  ${ANDROID_CXX}"

echo "Target:"
echo "  ${ANDROID_TARGET}"

echo "Sysroot:"
echo "  ${ANDROID_SYSROOT}"

echo "GCC toolchain:"
echo "  ${ANDROID_GCC_TOOLCHAIN}"

echo "CFLAGS:"
echo "  ${ASTCENC_CFLAGS}"

echo "CXXFLAGS:"
echo "  ${ASTCENC_CXXFLAGS}"

echo "LDFLAGS:"
echo "  ${ASTCENC_LDFLAGS}"

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

###############################################################################
# Build ONLY the ASTCENC static codec library
###############################################################################

echo
echo "========================================"
echo " Building ASTC Encoder static library"
echo "========================================"

cmake \
    --build "${ASTCENC_BUILD}" \
    --target astcenc-neon-static \
    --config Release \
    --parallel "$(nproc)"

###############################################################################
# Find astcenc.h
###############################################################################

echo
echo "========================================"
echo " Collecting ASTCENC"
echo "========================================"

ASTCENC_HEADER_SOURCE=""

for HEADER in \
    "${ASTCENC_DIR}/Source/astcenc.h" \
    "${ASTCENC_DIR}/include/astcenc.h" \
    "${ASTCENC_BUILD}/Source/astcenc.h" \
    "${ASTCENC_BUILD}/include/astcenc.h"
do

    if [ -f "${HEADER}" ]; then
        ASTCENC_HEADER_SOURCE="${HEADER}"
        break
    fi

done

if [ -z "${ASTCENC_HEADER_SOURCE}" ]; then

    echo
    echo "ERROR: astcenc.h not found."

    echo
    echo "Searching:"
    find "${ASTCENC_DIR}" \
        "${ASTCENC_BUILD}" \
        -name astcenc.h \
        -print \
        2>/dev/null || true

    exit 1
fi

cp \
    "${ASTCENC_HEADER_SOURCE}" \
    "${ASTCENC_INSTALL}/include/astcenc.h"

echo
echo "Header:"
echo "  ${ASTCENC_INSTALL}/include/astcenc.h"

###############################################################################
# Find ASTCENC static library
###############################################################################

ASTCENC_LIBRARY_SOURCE=""

for LIB in \
    "${ASTCENC_BUILD}/libastcenc-neon-static.a" \
    "${ASTCENC_BUILD}/Source/libastcenc-neon-static.a" \
    "${ASTCENC_BUILD}/lib/libastcenc-neon-static.a" \
    "${ASTCENC_BUILD}/libastcenc.a" \
    "${ASTCENC_BUILD}/Source/libastcenc.a" \
    "${ASTCENC_BUILD}/lib/libastcenc.a"
do

    if [ -f "${LIB}" ]; then
        ASTCENC_LIBRARY_SOURCE="${LIB}"
        break
    fi

done

###############################################################################
# Fallback search
###############################################################################

if [ -z "${ASTCENC_LIBRARY_SOURCE}" ]; then

    ASTCENC_LIBRARY_SOURCE="$(
        find "${ASTCENC_BUILD}" \
            -type f \
            \( \
                -name 'libastcenc-neon-static.a' \
                -o \
                -name 'libastcenc.a' \
            \) \
            -print \
            | head -n 1
    )"

fi

###############################################################################
# Library must exist
###############################################################################

if [ -z "${ASTCENC_LIBRARY_SOURCE}" ]; then

    echo
    echo "ERROR: ASTCENC static library not found."

    echo
    echo "All static libraries in build tree:"
    find "${ASTCENC_BUILD}" \
        -type f \
        -name '*.a' \
        -print \
        2>/dev/null || true

    echo
    echo "All build files:"
    find "${ASTCENC_BUILD}" \
        -maxdepth 4 \
        -type f \
        -print \
        2>/dev/null \
        | head -n 200 || true

    exit 1
fi

###############################################################################
# Copy library
###############################################################################

ASTCENC_LIBRARY_NAME="$(basename "${ASTCENC_LIBRARY_SOURCE}")"

cp \
    "${ASTCENC_LIBRARY_SOURCE}" \
    "${ASTCENC_INSTALL}/lib/${ASTCENC_LIBRARY_NAME}"

ASTCENC_LIBRARY="${ASTCENC_INSTALL}/lib/${ASTCENC_LIBRARY_NAME}"

###############################################################################
# Verify header
###############################################################################

if [ ! -f "${ASTCENC_INSTALL}/include/astcenc.h" ]; then

    echo
    echo "ERROR: ASTCENC header installation failed."

    exit 1
fi

###############################################################################
# Verify library
###############################################################################

if [ ! -f "${ASTCENC_LIBRARY}" ]; then

    echo
    echo "ERROR: ASTCENC library installation failed."

    exit 1
fi

###############################################################################
# Verify architecture
###############################################################################

echo
echo "========================================"
echo " ASTCENC verification"
echo "========================================"

echo
echo "Header:"
echo "  ${ASTCENC_INSTALL}/include/astcenc.h"

echo
echo "Library:"
echo "  ${ASTCENC_LIBRARY}"

echo
echo "Library file:"
file "${ASTCENC_LIBRARY}" || true

###############################################################################
# Verify AArch64 object inside archive
###############################################################################

if command -v llvm-ar >/dev/null 2>&1; then

    echo
    echo "Archive members:"
    llvm-ar t "${ASTCENC_LIBRARY}" | head -n 10 || true

fi

###############################################################################
# Export ASTCENC_ROOT
###############################################################################

export ASTCENC_ROOT="${ASTCENC_INSTALL}"

echo
echo "========================================"
echo " ASTCENC READY"
echo "========================================"

echo "ASTCENC_ROOT:"
echo "  ${ASTCENC_ROOT}"

echo "Include:"
echo "  ${ASTCENC_INSTALL}/include"

echo "Library:"
echo "  ${ASTCENC_LIBRARY}"

###############################################################################
# Waf
###############################################################################

echo
echo "========================================"
echo " Waf configure"
echo "========================================"

chmod +x waf

export CFLAGS="-O2"
export CXXFLAGS="-O2"
export LDFLAGS="-s -flto"

###############################################################################
# Waf configure
###############################################################################

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
# Complete
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
