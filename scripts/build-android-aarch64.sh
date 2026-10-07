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
ANDROID_TARGET="aarch64-linux-android${ANDROID_API}"

###############################################################################
# Android NDK
###############################################################################

echo
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

ANDROID_SYSROOT="${NDK}/platforms/android-${ANDROID_API}/arch-arm64"

ANDROID_GCC_TOOLCHAIN="${NDK}/toolchains/aarch64-linux-android-4.9/prebuilt/linux-x86_64"

ANDROID_AR="${ANDROID_GCC_TOOLCHAIN}/bin/aarch64-linux-android-ar"
ANDROID_RANLIB="${ANDROID_GCC_TOOLCHAIN}/bin/aarch64-linux-android-ranlib"

if [ ! -d "${ANDROID_SYSROOT}" ]; then
    echo
    echo "ERROR: Android sysroot not found:"
    echo "  ${ANDROID_SYSROOT}"
    exit 1
fi

if [ ! -d "${ANDROID_GCC_TOOLCHAIN}" ]; then
    echo
    echo "ERROR: Android GCC toolchain not found:"
    echo "  ${ANDROID_GCC_TOOLCHAIN}"
    exit 1
fi

###############################################################################
# LLVM 11.1
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

if [ ! -x "${CLANGXX}" ]; then
    echo "ERROR: clang++ not found:"
    echo "  ${CLANGXX}"
    exit 1
fi

if [ ! -x "${LLD}" ]; then
    echo "ERROR: ld.lld not found:"
    echo "  ${LLD}"
    exit 1
fi

echo
echo "Clang:"
"${CLANG}" --version | head -n 1

echo
echo "LLD:"
"${LLD}" --version

###############################################################################
# IMPORTANT
#
# Do NOT use:
#
#   NDK/sources/cxx-stl/llvm-libc++
#
# r10e does not have the layout assumed by the previous script.
#
# Instead use the libc++ headers shipped with the LLVM/Clang distribution,
# if available.
###############################################################################

echo
echo "========================================"
echo " Locating libc++"
echo "========================================"

###############################################################################
# Search libc++ headers
###############################################################################

LIBCXX_INCLUDE=""

for DIR in \
    "${LLVM_DIR}/include/c++/v1" \
    "${LLVM_DIR}/include/c++/11" \
    "${LLVM_DIR}/include/c++"
do

    if [ -f "${DIR}/algorithm" ] && \
       [ -f "${DIR}/utility" ] && \
       [ -f "${DIR}/vector" ]; then

        LIBCXX_INCLUDE="${DIR}"
        break

    fi

done

###############################################################################
# If LLVM archive doesn't contain libc++, fetch libc++ source separately.
###############################################################################

if [ -z "${LIBCXX_INCLUDE}" ]; then

    echo
    echo "LLVM binary does not contain libc++ headers."
    echo "Downloading LLVM libc++ 11.1.0 source..."

    LIBCXX_SOURCE="${ROOT}/llvm-project"

    if [ ! -d "${LIBCXX_SOURCE}/libcxx/include" ]; then

        rm -rf "${LIBCXX_SOURCE}"

        git clone \
            --depth 1 \
            --branch "llvmorg-${LLVM_VERSION}" \
            https://github.com/llvm/llvm-project.git \
            "${LIBCXX_SOURCE}"

    fi

    if [ -f "${LIBCXX_SOURCE}/libcxx/include/algorithm" ]; then
        LIBCXX_INCLUDE="${LIBCXX_SOURCE}/libcxx/include"
    fi

fi

if [ -z "${LIBCXX_INCLUDE}" ]; then

    echo
    echo "ERROR: libc++ headers could not be located."

    echo
    echo "Searching LLVM:"
    find "${LLVM_DIR}" \
        -type f \
        \( \
            -name algorithm \
            -o \
            -name utility \
        \) \
        -path '*/c++/*' \
        -print \
        2>/dev/null | head -n 50 || true

    exit 1

fi

echo
echo "libc++ headers:"
echo "  ${LIBCXX_INCLUDE}"

###############################################################################
# Verify libc++ headers
###############################################################################

if [ ! -f "${LIBCXX_INCLUDE}/algorithm" ]; then
    echo "ERROR: libc++ algorithm header missing."
    exit 1
fi

if [ ! -f "${LIBCXX_INCLUDE}/utility" ]; then
    echo "ERROR: libc++ utility header missing."
    exit 1
fi

###############################################################################
# ASTCENC source
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

ASTCENC_COMMIT="$(git -C "${ASTCENC_DIR}" rev-parse HEAD)"

echo
echo "ASTCENC commit:"
echo "  ${ASTCENC_COMMIT}"

###############################################################################
# Clean
###############################################################################

rm -rf "${ASTCENC_BUILD}"
rm -rf "${ASTCENC_INSTALL}"

mkdir -p "${ASTCENC_BUILD}"
mkdir -p "${ASTCENC_INSTALL}/include"
mkdir -p "${ASTCENC_INSTALL}/lib"

###############################################################################
# Compiler flags
###############################################################################

COMMON_FLAGS="\
--target=${ANDROID_TARGET} \
--sysroot=${ANDROID_SYSROOT} \
--gcc-toolchain=${ANDROID_GCC_TOOLCHAIN}"

CFLAGS="${COMMON_FLAGS}"

CXXFLAGS="${COMMON_FLAGS} \
-isystem${LIBCXX_INCLUDE} \
-stdlib=libc++"

LDFLAGS="${COMMON_FLAGS} \
-fuse-ld=lld"

###############################################################################
# CMake
###############################################################################

echo
echo "========================================"
echo " CMake ASTCENC"
echo "========================================"

cmake \
    -S "${ASTCENC_DIR}" \
    -B "${ASTCENC_BUILD}" \
    -G "Unix Makefiles" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="${CLANG}" \
    -DCMAKE_CXX_COMPILER="${CLANGXX}" \
    -DCMAKE_C_FLAGS="${CFLAGS}" \
    -DCMAKE_CXX_FLAGS="${CXXFLAGS}" \
    -DCMAKE_EXE_LINKER_FLAGS="${LDFLAGS}" \
    -DCMAKE_SHARED_LINKER_FLAGS="${LDFLAGS}" \
    -DCMAKE_MODULE_LINKER_FLAGS="${LDFLAGS}" \
    -DCMAKE_AR="${LLVM_AR}" \
    -DCMAKE_RANLIB="${LLVM_RANLIB}" \
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
    -DCMAKE_CXX_STANDARD=11 \
    -DCMAKE_CXX_STANDARD_REQUIRED=ON \
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
# Build
###############################################################################

echo
echo "========================================"
echo " Building ASTCENC"
echo "========================================"

cmake \
    --build "${ASTCENC_BUILD}" \
    --target astcenc-neon-static \
    --config Release \
    --parallel "$(nproc)"

###############################################################################
# Find header
###############################################################################

echo
echo "========================================"
echo " Collecting ASTCENC"
echo "========================================"

ASTCENC_HEADER=""

for FILE in \
    "${ASTCENC_DIR}/Source/astcenc.h" \
    "${ASTCENC_DIR}/include/astcenc.h" \
    "${ASTCENC_BUILD}/Source/astcenc.h" \
    "${ASTCENC_BUILD}/include/astcenc.h"
do

    if [ -f "${FILE}" ]; then
        ASTCENC_HEADER="${FILE}"
        break
    fi

done

if [ -z "${ASTCENC_HEADER}" ]; then

    echo
    echo "ERROR: astcenc.h not found."

    find "${ASTCENC_DIR}" \
        "${ASTCENC_BUILD}" \
        -name astcenc.h \
        -print \
        2>/dev/null || true

    exit 1

fi

cp \
    "${ASTCENC_HEADER}" \
    "${ASTCENC_INSTALL}/include/astcenc.h"

###############################################################################
# Find library
###############################################################################

ASTCENC_LIBRARY=""

ASTCENC_LIBRARY="$(
    find "${ASTCENC_BUILD}" \
        -type f \
        -name 'libastcenc-neon-static.a' \
        -print \
        | head -n 1
)"

if [ -z "${ASTCENC_LIBRARY}" ]; then

    ASTCENC_LIBRARY="$(
        find "${ASTCENC_BUILD}" \
            -type f \
            -name 'libastcenc.a' \
            -print \
            | head -n 1
    )"

fi

if [ -z "${ASTCENC_LIBRARY}" ]; then

    echo
    echo "ERROR: ASTCENC library not found."

    echo
    echo "Static libraries found:"
    find "${ASTCENC_BUILD}" \
        -type f \
        -name '*.a' \
        -print \
        2>/dev/null || true

    exit 1

fi

###############################################################################
# Copy library
###############################################################################

ASTCENC_LIBRARY_NAME="$(basename "${ASTCENC_LIBRARY}")"

cp \
    "${ASTCENC_LIBRARY}" \
    "${ASTCENC_INSTALL}/lib/${ASTCENC_LIBRARY_NAME}"

###############################################################################
# Verify
###############################################################################

echo
echo "========================================"
echo " ASTCENC READY"
echo "========================================"

echo
echo "Header:"
echo "  ${ASTCENC_INSTALL}/include/astcenc.h"

echo
echo "Library:"
echo "  ${ASTCENC_INSTALL}/lib/${ASTCENC_LIBRARY_NAME}"

echo
echo "Library type:"
file "${ASTCENC_INSTALL}/lib/${ASTCENC_LIBRARY_NAME}" || true

###############################################################################
# Export
###############################################################################

export ASTCENC_ROOT="${ASTCENC_INSTALL}"

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

./waf configure \
    -T release \
    --build-games=csso \
    --togles \
    --android=aarch64,host,21 \
    --prefix=./output \
    --disable-warns

###############################################################################
# Build
###############################################################################

echo
echo "========================================"
echo " Waf build"
echo "========================================"

./waf build

###############################################################################
# Install
###############################################################################

echo
echo "========================================"
echo " Waf install"
echo "========================================"

./waf install

echo
echo "========================================"
echo " BUILD COMPLETE"
echo "========================================"

echo
echo "ASTCENC_ROOT:"
echo "  ${ASTCENC_ROOT}"

echo
echo "Output:"
echo "  ${ROOT}/output"
