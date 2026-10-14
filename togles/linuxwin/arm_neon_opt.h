//========= ARM NEON helpers for the Mali / ARM port of TOGL ====================//
//
// arm_neon_opt.h
//
// Small, self-contained AArch64 NEON kernels used by the texture upload paths
// (DXT decode in decompress.c, texel format conversion in astc_texcompress.cpp,
// RGB -> RGBA padding in cglmtex.cpp).
//
// RULES
//   * Every kernel is BIT-EXACT with the scalar code it accelerates. The scalar code is
//     kept in place and is what runs on anything that is not 64-bit ARM, and for the
//     tail (the last few texels) of every image.
//   * Kernels return how many texels they converted (always a multiple of their block
//     size); the caller finishes the rest with the original scalar loop.
//   * Only plain AArch64 AdvSIMD is used (no dot-product / FP16 arithmetic / SVE), so the
//     code runs on every Mali-equipped phone from Cortex-A53 up.
//   * Define TOGL_DISABLE_ARM_NEON (e.g. in the wscript "defines" list) to compile the
//     scalar paths only. That is the quickest way to rule NEON out when bisecting a bug.
//   * ARMOPT_FORCE_SIMDE is for host-side unit tests only (runs the NEON code on x86
//     through the SIMDe library). It is never defined by the engine build.
//
// This header is valid C89 + GNU extensions, so it can be included from .c and .cpp files.
//
//===============================================================================

#ifndef ARM_NEON_OPT_H
#define ARM_NEON_OPT_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#if defined( TOGL_DISABLE_ARM_NEON )
	#define ARMOPT_NEON64 0
#elif defined( ARMOPT_FORCE_SIMDE )
	#ifndef SIMDE_ENABLE_NATIVE_ALIASES
		#define SIMDE_ENABLE_NATIVE_ALIASES
	#endif
	#include <simde/arm/neon.h>
	#define ARMOPT_NEON64 1
#elif defined( __aarch64__ ) && defined( __ARM_NEON ) && defined( __BYTE_ORDER__ ) && defined( __ORDER_LITTLE_ENDIAN__ ) && ( __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ )
	#include <arm_neon.h>
	#define ARMOPT_NEON64 1
#else
	#define ARMOPT_NEON64 0
#endif

#if defined( __GNUC__ ) || defined( __clang__ )
	#define ARMOPT_FN		static __inline__
	#define ARMOPT_TINY		static __inline__ __attribute__(( always_inline ))
#elif defined( _MSC_VER )
	#define ARMOPT_FN		static __inline
	#define ARMOPT_TINY		static __inline
#else
	#define ARMOPT_FN		static
	#define ARMOPT_TINY		static
#endif

// Channel layouts of an 8-bit-per-channel source (memory order). Same meaning as ChanLayout in astc_texcompress.cpp.
enum
{
	ARMOPT_LAY_RGBA = 0,
	ARMOPT_LAY_BGRA,
	ARMOPT_LAY_RGB,
	ARMOPT_LAY_BGR,
	ARMOPT_LAY_RG,
	ARMOPT_LAY_R,
	ARMOPT_LAY_L,		// luminance  -> r = g = b
	ARMOPT_LAY_LA,		// luminance + alpha
	ARMOPT_LAY_A		// alpha only
};

//-----------------------------------------------------------------------------
// ArmOpt_U8ToRGBA
//
// Converts the first (n & ~15) pixels of an 8-bit-per-channel image to RGBA8 (memory order R,G,B,A).
// Channels the source does not have are 0 (r/g/b) or 255 (a); forceOpaque forces a = 255.
// This is exactly MapChannels() + the forceOpaque rule of ConvertPlanarUNorm<uint8_t>().
// Returns the number of pixels converted (multiple of 16); 0 when NEON is not compiled in.
//-----------------------------------------------------------------------------
ARMOPT_FN size_t ArmOpt_U8ToRGBA( const uint8_t* src, size_t n, int lay, int forceOpaque, uint8_t* dst )
{
#if ARMOPT_NEON64
	size_t i = 0;
	const uint8x16_t k255 = vdupq_n_u8( 255 );
	const uint8x16_t k0   = vdupq_n_u8( 0 );
	uint8x16x4_t o;

	switch ( lay )
	{
	case ARMOPT_LAY_RGBA:
		for ( ; i + 16 <= n; i += 16 )
		{
			o = vld4q_u8( src + i * 4 );
			if ( forceOpaque )
				o.val[3] = k255;
			vst4q_u8( dst + i * 4, o );
		}
		break;

	case ARMOPT_LAY_BGRA:
		for ( ; i + 16 <= n; i += 16 )
		{
			uint8x16x4_t v = vld4q_u8( src + i * 4 );
			o.val[0] = v.val[2];
			o.val[1] = v.val[1];
			o.val[2] = v.val[0];
			o.val[3] = forceOpaque ? k255 : v.val[3];
			vst4q_u8( dst + i * 4, o );
		}
		break;

	case ARMOPT_LAY_RGB:
		for ( ; i + 16 <= n; i += 16 )
		{
			uint8x16x3_t v = vld3q_u8( src + i * 3 );
			o.val[0] = v.val[0];
			o.val[1] = v.val[1];
			o.val[2] = v.val[2];
			o.val[3] = k255;
			vst4q_u8( dst + i * 4, o );
		}
		break;

	case ARMOPT_LAY_BGR:
		for ( ; i + 16 <= n; i += 16 )
		{
			uint8x16x3_t v = vld3q_u8( src + i * 3 );
			o.val[0] = v.val[2];
			o.val[1] = v.val[1];
			o.val[2] = v.val[0];
			o.val[3] = k255;
			vst4q_u8( dst + i * 4, o );
		}
		break;

	case ARMOPT_LAY_RG:
		for ( ; i + 16 <= n; i += 16 )
		{
			uint8x16x2_t v = vld2q_u8( src + i * 2 );
			o.val[0] = v.val[0];
			o.val[1] = v.val[1];
			o.val[2] = k0;
			o.val[3] = k255;
			vst4q_u8( dst + i * 4, o );
		}
		break;

	case ARMOPT_LAY_R:
		for ( ; i + 16 <= n; i += 16 )
		{
			o.val[0] = vld1q_u8( src + i );
			o.val[1] = k0;
			o.val[2] = k0;
			o.val[3] = k255;
			vst4q_u8( dst + i * 4, o );
		}
		break;

	case ARMOPT_LAY_L:
		for ( ; i + 16 <= n; i += 16 )
		{
			const uint8x16_t l = vld1q_u8( src + i );
			o.val[0] = l;
			o.val[1] = l;
			o.val[2] = l;
			o.val[3] = k255;
			vst4q_u8( dst + i * 4, o );
		}
		break;

	case ARMOPT_LAY_LA:
		for ( ; i + 16 <= n; i += 16 )
		{
			uint8x16x2_t v = vld2q_u8( src + i * 2 );
			o.val[0] = v.val[0];
			o.val[1] = v.val[0];
			o.val[2] = v.val[0];
			o.val[3] = forceOpaque ? k255 : v.val[1];
			vst4q_u8( dst + i * 4, o );
		}
		break;

	case ARMOPT_LAY_A:
		for ( ; i + 16 <= n; i += 16 )
		{
			const uint8x16_t a = vld1q_u8( src + i );
			o.val[0] = k0;
			o.val[1] = k0;
			o.val[2] = k0;
			o.val[3] = forceOpaque ? k255 : a;
			vst4q_u8( dst + i * 4, o );
		}
		break;

	default:
		break;
	}
	return i;
#else
	(void)src; (void)n; (void)lay; (void)forceOpaque; (void)dst;
	return 0;
#endif
}

#if ARMOPT_NEON64

// HDR sanitising, identical to SanitizeHDR() in astc_texcompress.cpp:
//   !( v >= 0 ) -> 0 (negative numbers and NaN), v > 65504 -> 65504.
ARMOPT_TINY float32x4_t ArmOpt_SanitizeHDR( float32x4_t v )
{
	const uint32x4_t ok = vcgeq_f32( v, vdupq_n_f32( 0.0f ) );		// false for NaN
	v = vreinterpretq_f32_u32( vandq_u32( vreinterpretq_u32_f32( v ), ok ) );
	return vminq_f32( v, vdupq_n_f32( 65504.0f ) );
}

// [r,g,b,a] <-> [b,g,r,a]
ARMOPT_TINY float32x4_t ArmOpt_SwapRB( float32x4_t v )
{
	static const uint8_t kSwap[16] = { 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3, 12, 13, 14, 15 };
	return vreinterpretq_f32_u8( vqtbl1q_u8( vreinterpretq_u8_f32( v ), vld1q_u8( kSwap ) ) );
}

#endif // ARMOPT_NEON64

//-----------------------------------------------------------------------------
// ArmOpt_HalfRGBAToF32 / ArmOpt_F32RGBAToF32
//
// 4-component (RGBA or BGRA in memory) half / float source -> RGBA float32, sanitised for the HDR
// encoder. Same result as the per-texel loop in ConvertToRGBAF32().
// Return the number of pixels converted (multiple of 2 / 1); 0 when NEON is not compiled in.
//-----------------------------------------------------------------------------
ARMOPT_FN size_t ArmOpt_HalfRGBAToF32( const uint8_t* src, size_t n, int bgra, int forceOpaque, float* dst )
{
#if ARMOPT_NEON64
	size_t i = 0;
	for ( ; i + 2 <= n; i += 2 )
	{
		const uint16x8_t h = vreinterpretq_u16_u8( vld1q_u8( src + i * 8 ) );
		float32x4_t p0 = vcvt_f32_f16( vreinterpret_f16_u16( vget_low_u16( h ) ) );
		float32x4_t p1 = vcvt_f32_f16( vreinterpret_f16_u16( vget_high_u16( h ) ) );

		if ( bgra )
		{
			p0 = ArmOpt_SwapRB( p0 );
			p1 = ArmOpt_SwapRB( p1 );
		}
		if ( forceOpaque )
		{
			p0 = vsetq_lane_f32( 1.0f, p0, 3 );
			p1 = vsetq_lane_f32( 1.0f, p1, 3 );
		}
		vst1q_f32( dst + i * 4,     ArmOpt_SanitizeHDR( p0 ) );
		vst1q_f32( dst + i * 4 + 4, ArmOpt_SanitizeHDR( p1 ) );
	}
	return i;
#else
	(void)src; (void)n; (void)bgra; (void)forceOpaque; (void)dst;
	return 0;
#endif
}

ARMOPT_FN size_t ArmOpt_F32RGBAToF32( const uint8_t* src, size_t n, int bgra, int forceOpaque, float* dst )
{
#if ARMOPT_NEON64
	size_t i = 0;
	for ( ; i < n; ++i )
	{
		float32x4_t p = vreinterpretq_f32_u8( vld1q_u8( src + i * 16 ) );
		if ( bgra )
			p = ArmOpt_SwapRB( p );
		if ( forceOpaque )
			p = vsetq_lane_f32( 1.0f, p, 3 );
		vst1q_f32( dst + i * 4, ArmOpt_SanitizeHDR( p ) );
	}
	return i;
#else
	(void)src; (void)n; (void)bgra; (void)forceOpaque; (void)dst;
	return 0;
#endif
}

#endif // ARM_NEON_OPT_H
