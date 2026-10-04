//========= ASTC runtime recompression feature (added on top of Valve TOGL) =====//
//
// astc_texcompress.cpp
//
// See astc_texcompress.h for the feature overview. This file:
//   1. detects ASTC hardware support from the GL extension string
//   2. classifies D3DFORMATs into "eligible for ASTC" / "LDR" / "HDR" / "opaque"
//   3. feeds the source pixels to ARM's astcenc with as little copying as
//      possible (8-bit RGBA/BGRA, F16 and F32 RGBA are passed straight through;
//      channel order is fixed with the encoder swizzle, not by repacking)
//   4. encodes 2D images and sliced 3D volumes (2D blocks, one layer per slice)
//
// Performance notes
//   * astcenc contexts are expensive to create (block-mode tables, partition
//     tables). They are cached per thread and reused (astcenc_compress_reset).
//   * LDR sRGB sources are encoded with ASTCENC_PRF_LDR_SRGB so the error
//     metric matches how the GPU will decode them.
//
//===============================================================================

#include "astc_texcompress.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "tier0/dbg.h"	// Error() / Warning() / Msg()

#ifndef D3DFMT_A8R8G8B8
	#define D3DFMT_A8R8G8B8      21
	#define D3DFMT_X8R8G8B8      22
	#define D3DFMT_A4R4G4B4      26
	#define D3DFMT_A1R5G5B5      25
	#define D3DFMT_X1R5G5B5      24
	#define D3DFMT_A2R10G10B10   35
	#define D3DFMT_A2B10G10R10   31
	#define D3DFMT_Q8W8V8U8      63
	#define D3DFMT_A16B16G16R16   36
	#define D3DFMT_A16B16G16R16F 113
	#define D3DFMT_A32B32G32R32F 116
	#define D3DFMT_R32F          114
#endif

#if defined( HAVE_ASTCENC )
	#include <astcenc.h>
#endif

// Minimal local copies of the GL enums we branch on (guarded: the real GL
// headers define these as macros, so this is a no-op when they are present).
#ifndef GL_UNSIGNED_BYTE
#define GL_UNSIGNED_BYTE                   0x1401
#endif
#ifndef GL_UNSIGNED_SHORT
#define GL_UNSIGNED_SHORT                  0x1403
#endif
#ifndef GL_FLOAT
#define GL_FLOAT                           0x1406
#endif
#ifndef GL_HALF_FLOAT_ARB
#define GL_HALF_FLOAT_ARB                  0x140B
#endif
#ifndef GL_RED
#define GL_RED                             0x1903
#endif
#ifndef GL_RGBA
#define GL_RGBA                            0x1908
#endif
#ifndef GL_BGRA
#define GL_BGRA                            0x80E1
#endif
#ifndef GL_UNSIGNED_SHORT_4_4_4_4_REV
#define GL_UNSIGNED_SHORT_4_4_4_4_REV      0x8365
#endif
#ifndef GL_UNSIGNED_SHORT_1_5_5_5_REV
#define GL_UNSIGNED_SHORT_1_5_5_5_REV      0x8366
#endif
#ifndef GL_UNSIGNED_INT_8_8_8_8_REV
#define GL_UNSIGNED_INT_8_8_8_8_REV        0x8367
#endif

// ---------------------------------------------------------------------------
// ConVars
// ---------------------------------------------------------------------------

ConVar gl_astc_recompress( "gl_astc_recompress", "1", FCVAR_ARCHIVE,
	"Legacy toggle, kept only so existing configs/launch options that set "
	"this don't fail to parse. ASTC recompression is mandatory wherever the "
	"GPU supports it; this value is no longer read." );

ConVar gl_astc_block_ldr( "gl_astc_block_ldr", "6x6", FCVAR_ARCHIVE,
	"ASTC block footprint for 8/16-bit normalized sources (2D and 3D; also "
	"DXT1/3/5, which are decoded to RGBA first). Smaller = better quality, "
	"larger = smaller memory." );

ConVar gl_astc_block_hdr( "gl_astc_block_hdr", "4x4", FCVAR_ARCHIVE,
	"ASTC block footprint for floating point / half-float (HDR) sources "
	"(2D and 3D)." );

ConVar gl_astc_quality( "gl_astc_quality", "25", FCVAR_ARCHIVE,
	"astcenc quality, 0 (fastest) - 100 (exhaustive). Encoding happens while "
	"textures are uploaded, so the default favours speed over the last bit "
	"of quality (astcenc: 10 = fast, 60 = medium)." );

// ---------------------------------------------------------------------------
// Hardware capabilities
// ---------------------------------------------------------------------------

static ASTCCaps s_astcCaps = { false, false, false, false };

static bool HasExtension( const char* pList, const char* pExt )
{
	if ( !pList || !pExt )
		return false;

	const size_t len = strlen( pExt );
	const char* p = pList;
	while ( ( p = strstr( p, pExt ) ) != NULL )
	{
		// whole-token match only
		if ( ( p == pList || p[-1] == ' ' ) && ( p[len] == ' ' || p[len] == '\0' ) )
			return true;
		p += len;
	}
	return false;
}

void ASTC_InitCaps( const char* pszGLExtensions )
{
	const bool bOES  = HasExtension( pszGLExtensions, "GL_OES_texture_compression_astc" );
	const bool bLDR  = HasExtension( pszGLExtensions, "GL_KHR_texture_compression_astc_ldr" );
	const bool bHDR  = HasExtension( pszGLExtensions, "GL_KHR_texture_compression_astc_hdr" );
	const bool bS3D  = HasExtension( pszGLExtensions, "GL_KHR_texture_compression_astc_sliced_3d" );

	s_astcCaps.m_bHDR      = bHDR || bOES;
	s_astcCaps.m_bLDR      = bLDR || bHDR || bOES;		// HDR / OES profiles are supersets of LDR
	s_astcCaps.m_bSliced3D = bS3D || bOES;
	s_astcCaps.m_bValid    = true;

	Msg( "ASTC: LDR=%s HDR=%s sliced-3D=%s (OES_astc=%s)  encoder=%s\n",
		s_astcCaps.m_bLDR ? "yes" : "no", s_astcCaps.m_bHDR ? "yes" : "no",
		s_astcCaps.m_bSliced3D ? "yes" : "no", bOES ? "yes" : "no",
#if defined( HAVE_ASTCENC )
		"built in"
#else
		"NOT built in (HAVE_ASTCENC undefined)"
#endif
		);
}

const ASTCCaps& ASTC_GetCaps()
{
	return s_astcCaps;
}

bool ASTC_CanCompress( bool isHDR, bool is3D )
{
#if !defined( HAVE_ASTCENC )
	(void)isHDR; (void)is3D;
	return false;
#else
	if ( !s_astcCaps.m_bValid )
		return false;
	if ( isHDR ? !s_astcCaps.m_bHDR : !s_astcCaps.m_bLDR )
		return false;
	if ( is3D && !s_astcCaps.m_bSliced3D )
		return false;
	return true;
#endif
}

// ---------------------------------------------------------------------------
// Format classification
// ---------------------------------------------------------------------------

bool ASTC_IsEligibleFormat( int d3dFormat )
{
	switch ( d3dFormat )
	{
		case D3DFMT_A8R8G8B8:
		case D3DFMT_X8R8G8B8:
		case D3DFMT_A4R4G4B4:
		case D3DFMT_A1R5G5B5:
		case D3DFMT_X1R5G5B5:
		case D3DFMT_A2R10G10B10:
		case D3DFMT_A2B10G10R10:
		case D3DFMT_Q8W8V8U8:
		case D3DFMT_A16B16G16R16:
		case D3DFMT_A16B16G16R16F:
		case D3DFMT_A32B32G32R32F:
		case D3DFMT_R32F:
			return true;
		default:
			return false;
	}
}

bool ASTC_IsHDRFormat( int d3dFormat )
{
	switch ( d3dFormat )
	{
		case D3DFMT_A16B16G16R16F:
		case D3DFMT_A32B32G32R32F:
		case D3DFMT_R32F:
			return true;
		default:
			return false;
	}
}

bool ASTC_IsOpaqueFormat( int d3dFormat )
{
	return d3dFormat == D3DFMT_X8R8G8B8 || d3dFormat == D3DFMT_X1R5G5B5;
}

// ---------------------------------------------------------------------------
// Block-size helpers
// ---------------------------------------------------------------------------

struct ASTCBlockEntry { int w, h; uint32_t glRGBA; uint32_t glSRGB; };

static const ASTCBlockEntry s_blockTable[] =
{
	{ 4,  4,  GL_COMPRESSED_RGBA_ASTC_4x4_KHR,   GL_COMPRESSED_SRGB8_ALPHA8_ASTC_4x4_KHR   },
	{ 5,  4,  GL_COMPRESSED_RGBA_ASTC_5x4_KHR,   GL_COMPRESSED_SRGB8_ALPHA8_ASTC_5x4_KHR   },
	{ 5,  5,  GL_COMPRESSED_RGBA_ASTC_5x5_KHR,   GL_COMPRESSED_SRGB8_ALPHA8_ASTC_5x5_KHR   },
	{ 6,  5,  GL_COMPRESSED_RGBA_ASTC_6x5_KHR,   GL_COMPRESSED_SRGB8_ALPHA8_ASTC_6x5_KHR   },
	{ 6,  6,  GL_COMPRESSED_RGBA_ASTC_6x6_KHR,   GL_COMPRESSED_SRGB8_ALPHA8_ASTC_6x6_KHR   },
	{ 8,  5,  GL_COMPRESSED_RGBA_ASTC_8x5_KHR,   GL_COMPRESSED_SRGB8_ALPHA8_ASTC_8x5_KHR   },
	{ 8,  6,  GL_COMPRESSED_RGBA_ASTC_8x6_KHR,   GL_COMPRESSED_SRGB8_ALPHA8_ASTC_8x6_KHR   },
	{ 8,  8,  GL_COMPRESSED_RGBA_ASTC_8x8_KHR,   GL_COMPRESSED_SRGB8_ALPHA8_ASTC_8x8_KHR   },
	{ 10, 5,  GL_COMPRESSED_RGBA_ASTC_10x5_KHR,  GL_COMPRESSED_SRGB8_ALPHA8_ASTC_10x5_KHR  },
	{ 10, 6,  GL_COMPRESSED_RGBA_ASTC_10x6_KHR,  GL_COMPRESSED_SRGB8_ALPHA8_ASTC_10x6_KHR  },
	{ 10, 8,  GL_COMPRESSED_RGBA_ASTC_10x8_KHR,  GL_COMPRESSED_SRGB8_ALPHA8_ASTC_10x8_KHR  },
	{ 10, 10, GL_COMPRESSED_RGBA_ASTC_10x10_KHR, GL_COMPRESSED_SRGB8_ALPHA8_ASTC_10x10_KHR },
	{ 12, 10, GL_COMPRESSED_RGBA_ASTC_12x10_KHR, GL_COMPRESSED_SRGB8_ALPHA8_ASTC_12x10_KHR },
	{ 12, 12, GL_COMPRESSED_RGBA_ASTC_12x12_KHR, GL_COMPRESSED_SRGB8_ALPHA8_ASTC_12x12_KHR },
};

static const ASTCBlockEntry* FindBlock( int w, int h )
{
	for ( size_t i = 0; i < sizeof( s_blockTable ) / sizeof( s_blockTable[0] ); ++i )
	{
		if ( s_blockTable[i].w == w && s_blockTable[i].h == h )
			return &s_blockTable[i];
	}
	return NULL;
}

void ASTC_GetConfiguredBlockSize( bool isHDR, int* outW, int* outH )
{
	// Shipped defaults: LDR 6x6, HDR 4x4. Only used if the convar string is
	// not one of the 14 footprints the ASTC spec defines.
	const int defW = isHDR ? 4 : 6;
	const int defH = isHDR ? 4 : 6;

	int w = defW, h = defH;
	const char* str = isHDR ? gl_astc_block_hdr.GetString() : gl_astc_block_ldr.GetString();
	if ( !str || sscanf( str, "%dx%d", &w, &h ) != 2 || !FindBlock( w, h ) )
	{
		w = defW;
		h = defH;
	}
	*outW = w;
	*outH = h;
}

uint32_t ASTC_CompressedSize( int width, int height, int depth, int blockW, int blockH )
{
	const uint32_t xb = ( uint32_t )( ( width  + blockW - 1 ) / blockW );
	const uint32_t yb = ( uint32_t )( ( height + blockH - 1 ) / blockH );
	return xb * yb * ( uint32_t )( depth > 0 ? depth : 1 ) * 16u;	// every ASTC block is 128 bit
}

// ---------------------------------------------------------------------------
// Encoder
// ---------------------------------------------------------------------------
#if defined( HAVE_ASTCENC )

// Per-thread cache of encoder contexts. Creating a context is far more
// expensive than encoding a small mip, so reuse them.
struct ASTCCtxEntry
{
	astcenc_context*	m_pCtx;
	astcenc_profile		m_profile;
	int					m_blockW, m_blockH;
	float				m_quality;
};

struct ASTCCtxCache
{
	enum { kEntries = 4 };
	ASTCCtxEntry	m_entries[kEntries];
	int				m_next;

	ASTCCtxCache() : m_next( 0 ) { memset( m_entries, 0, sizeof( m_entries ) ); }
	~ASTCCtxCache()
	{
		for ( int i = 0; i < kEntries; ++i )
		{
			if ( m_entries[i].m_pCtx )
				astcenc_context_free( m_entries[i].m_pCtx );
		}
	}

	astcenc_context* Get( astcenc_profile profile, int bw, int bh, float quality )
	{
		for ( int i = 0; i < kEntries; ++i )
		{
			ASTCCtxEntry& e = m_entries[i];
			if ( e.m_pCtx && e.m_profile == profile && e.m_blockW == bw && e.m_blockH == bh && e.m_quality == quality )
			{
				astcenc_compress_reset( e.m_pCtx );
				return e.m_pCtx;
			}
		}

		astcenc_config config;
		if ( astcenc_config_init( profile, bw, bh, 1, quality, 0, &config ) != ASTCENC_SUCCESS )
			return NULL;

		astcenc_context* pCtx = NULL;
		if ( astcenc_context_alloc( &config, 1, &pCtx ) != ASTCENC_SUCCESS )
			return NULL;

		ASTCCtxEntry& slot = m_entries[m_next];
		m_next = ( m_next + 1 ) % kEntries;
		if ( slot.m_pCtx )
			astcenc_context_free( slot.m_pCtx );

		slot.m_pCtx = pCtx;
		slot.m_profile = profile;
		slot.m_blockW = bw;
		slot.m_blockH = bh;
		slot.m_quality = quality;
		return pCtx;
	}
};

static thread_local ASTCCtxCache s_ctxCache;

// Converts packed / 16-bit sources into a freshly allocated RGBA8 buffer
// (R,G,B,A byte order). Returns NULL for unsupported (format,type) pairs.
static uint8_t* ExpandToRGBA8( const ASTCSource& s )
{
	const size_t n = ( size_t )s.m_width * s.m_height * s.m_depth;
	uint8_t* dst = ( uint8_t* )malloc( n * 4 );
	if ( !dst )
		return NULL;

	switch ( s.m_glType )
	{
		case GL_UNSIGNED_SHORT:		// A16B16G16R16 -> RGBA16 in memory order R,G,B,A
		{
			const uint16_t* src = ( const uint16_t* )s.m_pData;
			for ( size_t i = 0; i < n * 4; ++i )
				dst[i] = ( uint8_t )( ( ( uint32_t )src[i] * 255u + 32767u ) / 65535u );
			return dst;
		}
		case GL_UNSIGNED_SHORT_4_4_4_4_REV:	// BGRA/REV: A<<12 | R<<8 | G<<4 | B
		{
			const uint16_t* src = ( const uint16_t* )s.m_pData;
			for ( size_t i = 0; i < n; ++i )
			{
				const uint32_t v = src[i];
				dst[i*4+0] = ( uint8_t )( ( ( v >> 8 ) & 0xF ) * 17 );
				dst[i*4+1] = ( uint8_t )( ( ( v >> 4 ) & 0xF ) * 17 );
				dst[i*4+2] = ( uint8_t )( (   v        & 0xF ) * 17 );
				dst[i*4+3] = ( uint8_t )( ( ( v >> 12 ) & 0xF ) * 17 );
			}
			return dst;
		}
		case GL_UNSIGNED_SHORT_1_5_5_5_REV:	// BGRA/REV: A<<15 | R<<10 | G<<5 | B
		{
			const uint16_t* src = ( const uint16_t* )s.m_pData;
			for ( size_t i = 0; i < n; ++i )
			{
				const uint32_t v = src[i];
				const uint32_t r = ( v >> 10 ) & 0x1F, g = ( v >> 5 ) & 0x1F, b = v & 0x1F;
				dst[i*4+0] = ( uint8_t )( ( r << 3 ) | ( r >> 2 ) );
				dst[i*4+1] = ( uint8_t )( ( g << 3 ) | ( g >> 2 ) );
				dst[i*4+2] = ( uint8_t )( ( b << 3 ) | ( b >> 2 ) );
				dst[i*4+3] = ( v & 0x8000 ) ? 255 : 0;
			}
			return dst;
		}
		default:
			free( dst );
			return NULL;
	}
}

#endif // HAVE_ASTCENC

bool ASTC_Encode( const ASTCSource& s, int blockW, int blockH, float quality, ASTCEncodeResult* outResult )
{
#if !defined( HAVE_ASTCENC )
	(void)s; (void)blockW; (void)blockH; (void)quality; (void)outResult;
	return false;
#else
	if ( !s.m_pData || s.m_width <= 0 || s.m_height <= 0 || s.m_depth <= 0 || !outResult )
		return false;

	const ASTCBlockEntry* pBlock = FindBlock( blockW, blockH );
	if ( !pBlock )
		return false;

	if ( quality < 0.0f )   quality = 0.0f;
	if ( quality > 100.0f ) quality = 100.0f;

	// --- Work out the encoder input: pointer, data type, swizzle ----------
	const void*			pInput  = s.m_pData;		// default: zero-copy
	void*				pOwned  = NULL;				// freed at the end if we had to convert
	astcenc_type		dataType;
	astcenc_swizzle		swz = { ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A };

	if ( !s.m_bHDR )
	{
		dataType = ASTCENC_TYPE_U8;

		const bool bByteLayout = ( s.m_glType == GL_UNSIGNED_BYTE || s.m_glType == GL_UNSIGNED_INT_8_8_8_8_REV );
		if ( bByteLayout )
		{
			// Memory order is B,G,R,A for BGRA (and for 8_8_8_8_REV on little endian).
			// astcenc swizzle selects *input slots*: slot0=first byte ... slot3=fourth byte.
			if ( s.m_glFormat == GL_BGRA )
			{
				swz.r = ASTCENC_SWZ_B;
				swz.b = ASTCENC_SWZ_R;
			}
			else if ( s.m_glFormat != GL_RGBA )
			{
				return false;
			}
		}
		else
		{
			pOwned = ExpandToRGBA8( s );
			if ( !pOwned )
				return false;
			pInput = pOwned;
		}
	}
	else
	{
		if ( s.m_glFormat == GL_RGBA && s.m_glType == GL_HALF_FLOAT_ARB )
		{
			dataType = ASTCENC_TYPE_F16;		// zero-copy: no float conversion
		}
		else if ( s.m_glFormat == GL_RGBA && s.m_glType == GL_FLOAT )
		{
			dataType = ASTCENC_TYPE_F32;		// zero-copy
		}
		else if ( s.m_glFormat == GL_RED && s.m_glType == GL_FLOAT )
		{
			// R32F: one channel per texel -> (r,0,0,1)
			const size_t n = ( size_t )s.m_width * s.m_height * s.m_depth;
			float* dst = ( float* )malloc( n * 4 * sizeof( float ) );
			if ( !dst )
				return false;
			const float* src = ( const float* )s.m_pData;
			for ( size_t i = 0; i < n; ++i )
			{
				dst[i*4+0] = src[i];
				dst[i*4+1] = 0.0f;
				dst[i*4+2] = 0.0f;
				dst[i*4+3] = 1.0f;
			}
			pOwned = dst;
			pInput = dst;
			dataType = ASTCENC_TYPE_F32;
		}
		else
		{
			return false;
		}
	}

	if ( s.m_bForceOpaque )
		swz.a = ASTCENC_SWZ_1;

	// --- Encoder context (cached) -----------------------------------------
	const astcenc_profile profile = s.m_bHDR ? ASTCENC_PRF_HDR
									: ( s.m_bSRGB ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR );

	astcenc_context* pCtx = s_ctxCache.Get( profile, blockW, blockH, quality );
	if ( !pCtx )
	{
		free( pOwned );
		return false;
	}

	// --- Slice pointer table (astcenc wants one pointer per depth layer) ---
	size_t bytesPerTexel;
	if ( dataType == ASTCENC_TYPE_U8 )       bytesPerTexel = 4;
	else if ( dataType == ASTCENC_TYPE_F16 ) bytesPerTexel = 8;
	else                                     bytesPerTexel = 16;
	const size_t sliceBytes = ( size_t )s.m_width * s.m_height * bytesPerTexel;

	void* stackSlices[16];
	void** slices = ( s.m_depth <= 16 ) ? stackSlices : ( void** )malloc( sizeof( void* ) * s.m_depth );
	if ( !slices )
	{
		free( pOwned );
		return false;
	}
	for ( int z = 0; z < s.m_depth; ++z )
		slices[z] = ( void* )( ( const uint8_t* )pInput + sliceBytes * z );

	astcenc_image image;
	image.dim_x = s.m_width;
	image.dim_y = s.m_height;
	image.dim_z = s.m_depth;
	image.data_type = dataType;
	image.data = slices;

	const size_t compSize = ASTC_CompressedSize( s.m_width, s.m_height, s.m_depth, blockW, blockH );
	uint8_t* compData = ( uint8_t* )malloc( compSize );

	astcenc_error status = ASTCENC_ERR_OUT_OF_MEM;
	if ( compData )
		status = astcenc_compress_image( pCtx, &image, &swz, compData, compSize, 0 );

	if ( slices != stackSlices )
		free( slices );
	free( pOwned );

	if ( status != ASTCENC_SUCCESS )
	{
		free( compData );
		return false;
	}

	outResult->m_pData = compData;
	outResult->m_nDataSize = ( uint32_t )compSize;
	outResult->m_glInternalFormat = ( s.m_bSRGB && !s.m_bHDR ) ? pBlock->glSRGB : pBlock->glRGBA;
	outResult->m_blockW = blockW;
	outResult->m_blockH = blockH;
	outResult->m_depth = s.m_depth;
	outResult->m_profile = s.m_bHDR ? kASTCProfile_HDR : kASTCProfile_LDR;
	return true;
#endif
}

void ASTC_EncodeRequired( const ASTCSource& src, ASTCEncodeResult* outResult )
{
	int blockW, blockH;
	ASTC_GetConfiguredBlockSize( src.m_bHDR, &blockW, &blockH );

	if ( !ASTC_Encode( src, blockW, blockH, ( float )gl_astc_quality.GetInt(), outResult ) )
	{
		// Callers only get here after ASTC_CanCompress() said yes, so this is
		// either "astcenc not linked in" (impossible: CanCompress is false
		// then) or a real encoder failure / unsupported source layout.
		Error( "ASTC_EncodeRequired: mandatory %s ASTC encode failed for a %dx%dx%d texture "
			   "(block %dx%d, glFormat 0x%x, glType 0x%x). Uncompressed upload is disabled.\n",
			   src.m_bHDR ? "HDR" : "LDR", src.m_width, src.m_height, src.m_depth,
			   blockW, blockH, src.m_glFormat, src.m_glType );
	}
}

void ASTC_FreeResult( ASTCEncodeResult* result )
{
	if ( result && result->m_pData )
	{
		free( result->m_pData );
		result->m_pData = NULL;
		result->m_nDataSize = 0;
	}
}

// ---------------------------------------------------------------------------
// Legacy 2D wrappers
// ---------------------------------------------------------------------------
bool ASTC_CompressTexture(
	const void* srcData, int width, int height,
	unsigned int srcGLFormat, unsigned int srcGLType,
	bool isHDR, int blockW, int blockH, int qualityPreset,
	ASTCEncodeResult* outResult )
{
	ASTCSource s;
	s.m_pData = srcData;
	s.m_width = width;
	s.m_height = height;
	s.m_depth = 1;
	s.m_glFormat = srcGLFormat;
	s.m_glType = srcGLType;
	s.m_bHDR = isHDR;
	s.m_bSRGB = false;
	s.m_bForceOpaque = false;
	return ASTC_Encode( s, blockW, blockH, ( float )qualityPreset, outResult );
}

void ASTC_CompressTextureRequired(
	bool isHDR, bool isSRGB, const void* srcData, int width, int height,
	unsigned int srcGLFormat, unsigned int srcGLType, ASTCEncodeResult* outResult )
{
	ASTCSource s;
	s.m_pData = srcData;
	s.m_width = width;
	s.m_height = height;
	s.m_depth = 1;
	s.m_glFormat = srcGLFormat;
	s.m_glType = srcGLType;
	s.m_bHDR = isHDR;
	s.m_bSRGB = isSRGB && !isHDR;
	s.m_bForceOpaque = false;
	ASTC_EncodeRequired( s, outResult );
}
