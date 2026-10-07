###############################################################################
# Build ASTC Encoder
###############################################################################

ASTC_CFLAGS="\
--target=${ANDROID_TARGET} \
--sysroot=${ANDROID_SYSROOT} \
--gcc-toolchain=${ANDROID_GCC_TOOLCHAIN}"

ASTC_CXXFLAGS="\
${ASTC_CFLAGS} \
-isystem${LIBCXX_INCLUDE} \
-stdlib=libc++"

ASTC_LDFLAGS="\
${ASTC_CFLAGS} \
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
# ASTCENC IS COMPLETELY FINISHED
#
# Keep ASTCENC_ROOT.
# Everything related to its compiler environment is discarded.
###############################################################################

unset ASTC_CFLAGS
unset ASTC_CXXFLAGS
unset ASTC_LDFLAGS

unset CFLAGS
unset CXXFLAGS
unset CPPFLAGS
unset LDFLAGS

unset CC
unset CXX
unset CPP
unset AR
unset AS
unset LD
unset NM
unset OBJCOPY
unset OBJDUMP
unset RANLIB
unset STRIP
unset READELF

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
# Prepare Waf environment
###############################################################################

echo
echo "========================================"
echo " Prepare Waf environment"
echo "========================================"

chmod +x waf

export ANDROID_NDK="$PWD/android-ndk-r10e/"
export ANDROID_NDK_HOME="$PWD/android-ndk-r10e/"
export NDK_HOME="$PWD/android-ndk-r10e/"

chmod +x "$HOME/llvm11/bin/llvm-strip"

export PATH="$HOME/llvm11/bin:$PATH"

export CC="$HOME/llvm11/bin/clang"
export CXX="$HOME/llvm11/bin/clang++"
export AR="$HOME/llvm11/bin/llvm-ar"
export RANLIB="$HOME/llvm11/bin/llvm-ranlib"
export STRIP="$HOME/llvm11/bin/llvm-strip"
export LD="$HOME/llvm11/bin/ld.lld"

export CFLAGS="-O2"
export CXXFLAGS="-O2"
export LDFLAGS="-s -flto"

echo
echo "Waf Android environment:"
echo "  ANDROID_NDK      = ${ANDROID_NDK}"
echo "  ANDROID_NDK_HOME = ${ANDROID_NDK_HOME}"
echo "  NDK_HOME         = ${NDK_HOME}"
echo "  ASTCENC_ROOT     = ${ASTCENC_ROOT}"

echo
echo "Compiler:"
echo "  CC  = ${CC}"
echo "  CXX = ${CXX}"


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
