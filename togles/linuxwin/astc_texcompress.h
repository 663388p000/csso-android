//========= ASTC runtime recompression feature (added on top of Valve TOGL) =====//
//
// astc_texcompress.h
//
// Purpose
// -------
// Every legacy D3D9 "ARGB" / "RGBA" style texture format that TOGL knows about
// (_A8R8G8B8, _X8R8G8B8, _Q8W8V8U8, _A2R10G10B10, _A16B16G16R16,
//  _A16B16G16R16F, _A32B32G32R32F, ...) can optionally be re-encoded into an
// ASTC compressed texture at upload time instead of being pushed to the GPU
// as an uncompressed GL_RGBA/GL_BGRA image.
//
// ASTC has two wire-compatible "profiles" that use the *same* block layout
// and the *same* GL internal-format enums (GL_COMPRESSED_RGBA_ASTC_*_KHR) --
// the difference is entirely in how the 128-bit blocks are produced and in
// which decode profile the GPU/driver is told to use:
//
//   * LDR (Low Dynamic Range)  - values are clamped/normalized to [0,1] and
//                                decode as regular UNORM colors. This is the
//                                right choice for every 8/16-bit-normalized
//                                "byte" ARGB/RGBA format (_A8R8G8B8,
//                                _X8R8G8B8, _Q8W8V8U8, _A2R10G10B10,
//                                _A16B16G16R16, ...).
//
//   * HDR (High Dynamic Range) - values may exceed 1.0 and decode as floats.
//                                This is the right choice for the floating
//                                point / half-float ARGB formats
//                                (_A16B16G16R16F, _A32B32G32R32F, _R32F).
//
// This module decides LDR vs HDR automatically from the source D3DFORMAT,
// and hands the actual block encoding off to ARM's reference "astcenc"
// library (Apache-2.0, https://github.com/ARM-software/astc-encoder).
//
// astc-encoder is NOT vendored in this tree (no network access was available
// to fetch it while building this patch). Drop the astcenc "Source" folder
// under e.g. thirdparty/astcenc/, add it to the build, and define
// HAVE_ASTCENC=1 for your build target.
//
// ASTC recompression is MANDATORY wherever the GPU can decode it: every
// eligible ARGB/RGBA upload (2D, cube and 3D/volume) -- and every DXT1/3/5
// texture, decoded to RGBA first -- goes through ASTC_EncodeRequired().
// If astcenc isn't built in, or encoding fails, that function calls Error().
//
// Hardware support is detected from the GL extension string (see
// ASTC_InitCaps / ASTC_GetCaps):
//   GL_KHR_texture_compression_astc_ldr         -> LDR 2D / cube
//   GL_KHR_texture_compression_astc_hdr         -> HDR 2D / cube
//   GL_KHR_texture_compression_astc_sliced_3d   -> 3D textures (2D block, depth 1)
//   GL_OES_texture_compression_astc             -> superset: LDR + HDR + sliced 3D
// A profile the driver does not advertise cannot be compressed (the GPU would
// reject the upload), so that texture stays on the old uncompressed path and
// a one-time warning is printed. Nothing is ever silently half-compressed.
//
// 3D textures use the *sliced* layout: 2D blocks (e.g. 6x6x1), one block
// layer per depth slice. This is exactly what astcenc produces when called
// with block_z = 1 and dim_z = depth.
//
//===============================================================================

#ifndef ASTC_TEXCOMPRESS_H
#define ASTC_TEXCOMPRESS_H

#pragma once

#include <stdint.h>
#include "tier1/convar.h"	// needed for the extern ConVar declarations below (callers use .GetBool()/.GetInt()/.GetString())

// ---------------------------------------------------------------------------
// GL enums for ASTC (from KHR_texture_compression_astc_ldr / _hdr).
// Defined here defensively in case the local GL header set predates them --
// remove the #ifndef guards if your gl headers already provide these.
// ---------------------------------------------------------------------------
#ifndef GL_COMPRESSED_RGBA_ASTC_4x4_KHR
#define GL_COMPRESSED_RGBA_ASTC_4x4_KHR    0x93B0
#define GL_COMPRESSED_RGBA_ASTC_5x4_KHR    0x93B1
#define GL_COMPRESSED_RGBA_ASTC_5x5_KHR    0x93B2
#define GL_COMPRESSED_RGBA_ASTC_6x5_KHR    0x93B3
#define GL_COMPRESSED_RGBA_ASTC_6x6_KHR    0x93B4
#define GL_COMPRESSED_RGBA_ASTC_8x5_KHR    0x93B5
#define GL_COMPRESSED_RGBA_ASTC_8x6_KHR    0x93B6
#define GL_COMPRESSED_RGBA_ASTC_8x8_KHR    0x93B7
#define GL_COMPRESSED_RGBA_ASTC_10x5_KHR   0x93B8
#define GL_COMPRESSED_RGBA_ASTC_10x6_KHR   0x93B9
#define GL_COMPRESSED_RGBA_ASTC_10x8_KHR   0x93BA
#define GL_COMPRESSED_RGBA_ASTC_10x10_KHR  0x93BB
#define GL_COMPRESSED_RGBA_ASTC_12x10_KHR  0x93BC
#define GL_COMPRESSED_RGBA_ASTC_12x12_KHR  0x93BD
// sRGB variants (LDR-only; there is no sRGB+HDR combination in the spec)
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_4x4_KHR   0x93D0
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_5x4_KHR   0x93D1
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_5x5_KHR   0x93D2
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_6x5_KHR   0x93D3
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_6x6_KHR   0x93D4
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_8x5_KHR   0x93D5
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_8x6_KHR   0x93D6
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_8x8_KHR   0x93D7
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_10x5_KHR  0x93D8
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_10x6_KHR  0x93D9
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_10x8_KHR  0x93DA
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_10x10_KHR 0x93DB
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_12x10_KHR 0x93DC
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_12x12_KHR 0x93DD
#endif

// Note: HDR profile textures are uploaded with the *same* enums above --
// KHR_texture_compression_astc_hdr re-uses the LDR tokens. The driver knows
// it is HDR data purely because the astcenc encode profile that produced the
// bitstream (ASTCENC_PRF_HDR) is different; there is no separate GL enum.

enum EASTCProfile
{
	kASTCProfile_LDR = 0,	// clamped [0,1], UNORM decode
	kASTCProfile_HDR = 1,	// unclamped, FP16 decode (needs GL_KHR_texture_compression_astc_hdr or GL_OES_texture_compression_astc)
};

struct ASTCEncodeResult
{
	void*		m_pData;			// caller must free with ASTC_FreeResult()
	uint32_t	m_nDataSize;		// size in bytes of m_pData
	uint32_t	m_glInternalFormat;	// GL_COMPRESSED_(SRGB8_)ALPHA(8_)ASTC_WxH_KHR -- already sRGB-mapped when requested
	int			m_blockW;
	int			m_blockH;
	int			m_depth;			// 1 for 2D; slice count for sliced 3D
	EASTCProfile m_profile;
};

// ---------------------------------------------------------------------------
// Hardware capability detection
// ---------------------------------------------------------------------------
struct ASTCCaps
{
	bool m_bLDR;		// GL_KHR_texture_compression_astc_ldr  (or hdr / OES, which imply it)
	bool m_bHDR;		// GL_KHR_texture_compression_astc_hdr  or GL_OES_texture_compression_astc
	bool m_bSliced3D;	// GL_KHR_texture_compression_astc_sliced_3d or GL_OES_texture_compression_astc
	bool m_bValid;		// ASTC_InitCaps() has run
};

// Called once from COpenGLEntryPoints::COpenGLEntryPoints() with the cached
// GL_EXTENSIONS string. Safe to call again.
void ASTC_InitCaps( const char* pszGLExtensions );
const ASTCCaps& ASTC_GetCaps();

// True when this build + this GPU can produce and decode the requested kind.
//   isHDR : float/half-float source (HDR profile)
//   is3D  : GL_TEXTURE_3D upload (needs sliced_3d)
bool ASTC_CanCompress( bool isHDR, bool is3D );

// ---------------------------------------------------------------------------
// Format classification (int is a D3DFORMAT; the .cpp does the real switch)
// ---------------------------------------------------------------------------
bool ASTC_IsEligibleFormat( int d3dFormat );
bool ASTC_IsHDRFormat( int d3dFormat );
// X8R8G8B8 / X1R5G5B5: the "X" channel is undefined in D3D; the encoder is told
// to force alpha = 1 (swizzle, zero-cost) so garbage X bytes never reach the
// alpha channel or waste endpoint precision.
bool ASTC_IsOpaqueFormat( int d3dFormat );

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------
struct ASTCSource
{
	const void*		m_pData;		// tightly packed, x fastest, then y, then z
	int				m_width;
	int				m_height;
	int				m_depth;		// 1 for 2D / cube face
	unsigned int	m_glFormat;		// GL_RGBA, GL_BGRA, GL_RED
	unsigned int	m_glType;		// GL_UNSIGNED_BYTE, GL_UNSIGNED_INT_8_8_8_8_REV, GL_UNSIGNED_SHORT,
									// GL_UNSIGNED_SHORT_4_4_4_4_REV, GL_UNSIGNED_SHORT_1_5_5_5_REV,
									// GL_HALF_FLOAT_ARB, GL_FLOAT
	bool			m_bHDR;
	bool			m_bSRGB;		// LDR only; encoder runs in the sRGB profile and the sRGB GL enum is returned
	bool			m_bForceOpaque;	// alpha := 1
};

// Core encoder. Returns false (outResult untouched) if astcenc isn't compiled
// in, the (format,type) pair is unsupported, or encoding failed.
bool ASTC_Encode( const ASTCSource& src, int blockW, int blockH, float quality, ASTCEncodeResult* outResult );

// Mandatory entry point: looks up the configured block size (gl_astc_block_ldr
// / gl_astc_block_hdr) and quality (gl_astc_quality) and calls Error() on
// failure instead of returning a failure code.
void ASTC_EncodeRequired( const ASTCSource& src, ASTCEncodeResult* outResult );

// Size in bytes of a sliced-3D / 2D ASTC image.
uint32_t ASTC_CompressedSize( int width, int height, int depth, int blockW, int blockH );

// ---------------------------------------------------------------------------
// Legacy 2D entry points (kept so other code in the tree keeps compiling)
// ---------------------------------------------------------------------------
bool ASTC_CompressTexture(
	const void* srcData, int width, int height,
	unsigned int srcGLFormat, unsigned int srcGLType,
	bool isHDR, int blockW, int blockH, int qualityPreset,
	ASTCEncodeResult* outResult );

void ASTC_CompressTextureRequired(
	bool isHDR, bool isSRGB, const void* srcData, int width, int height,
	unsigned int srcGLFormat, unsigned int srcGLType, ASTCEncodeResult* outResult );

void ASTC_FreeResult( ASTCEncodeResult* result );

// Reads gl_astc_block_ldr / gl_astc_block_hdr and parses them into block
// dimensions for the given profile. Invalid strings fall back to the
// shipped defaults (LDR 6x6, HDR 4x4).
void ASTC_GetConfiguredBlockSize( bool isHDR, int* outW, int* outH );

extern ConVar gl_astc_recompress;	// legacy, no longer read
extern ConVar gl_astc_block_ldr;	// default "6x6"
extern ConVar gl_astc_block_hdr;	// default "4x4"
extern ConVar gl_astc_quality;		// 0-100, astcenc quality (default 25 = between FASTEST and FAST: this runs at texture-upload time)

#endif // ASTC_TEXCOMPRESS_H
