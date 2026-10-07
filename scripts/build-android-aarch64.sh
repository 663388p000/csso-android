###############################################################################
# ASTCENC ONLY
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
# ASTCENC DONE
###############################################################################

echo
echo "ASTCENC build finished."


###############################################################################
# DESTROY ASTCENC COMPILER ENVIRONMENT
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
# WAF ENVIRONMENT
#
# NOTHING FROM ASTCENC IS REUSED HERE.
###############################################################################

export ANDROID_NDK="$PWD/android-ndk-r10e/"
export ANDROID_NDK_HOME="$PWD/android-ndk-r10e/"
export NDK_HOME="$PWD/android-ndk-r10e/"

chmod +x "$HOME/llvm11/bin/llvm-strip"

export PATH="$HOME/llvm11/bin:$PATH"

###############################################################################
# WAF ONLY
###############################################################################

export CC="$HOME/llvm11/bin/clang"
export CXX="$HOME/llvm11/bin/clang++"
export AR="$HOME/llvm11/bin/llvm-ar"
export RANLIB="$HOME/llvm11/bin/llvm-ranlib"
export STRIP="$HOME/llvm11/bin/llvm-strip"

export CFLAGS="-O2"
export CXXFLAGS="-O2"
export LDFLAGS="-s -flto"


###############################################################################
# WAF
###############################################################################

./waf configure \
    -T release \
    --build-games=csso \
    --togles \
    --android=aarch64,host,21 \
    --prefix=./output \
    --disable-warns

./waf build
./waf install
