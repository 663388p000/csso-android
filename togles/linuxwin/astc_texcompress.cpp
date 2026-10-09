//========= ASTC runtime recompression (added on top of Valve TOGL) - v4 =======//
//
// astc_texcompress.cpp
//
// See astc_texcompress.h for the overview. This file:
//   1. classifies D3DFORMATs (eligible / LDR / HDR / has-alpha)
//   2. normalizes the caller's (GL format, type) into what astcenc wants,
//      with zero-copy handling for 8-bit BGRA/RGBA sources
//   3. runs astcenc on a persistent thread pool (every CPU core) with shared contexts
//   4. reads and writes the single-file on-disk encode cache (astc_cache.bin)
//   5. builds solid-color blank ASTC images without the encoder
//
//===============================================================================

#include "astc_texcompress.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include "tier0/dbg.h"	// Error() / Warning()

#if defined( HAVE_ASTCENC )
	#include <astcenc.h>
	#include <thread>
	#include <mutex>
	#include <condition_variable>
	#include <functional>
	#include <vector>
	#include <atomic>
	#include <chrono>
	#include <unordered_set>
	#include <unordered_map>
	#include <string>
	#include <cstdint>
	#ifdef _WIN32
		#include <direct.h>
		#define ASTC_MKDIR( p ) _mkdir( p )
	#else
		#include <sys/stat.h>
		#include <sys/types.h>
		#include <sys/file.h>
		#include <fcntl.h>
		#include <unistd.h>
		#include <pthread.h>
		#include <errno.h>
		#include <dlfcn.h>
		#include <dirent.h>
		#if defined( __linux__ ) || defined( __ANDROID__ )
			#include <sys/prctl.h>
		#endif
		#define ASTC_MKDIR( p ) mkdir( p, 0755 )
		#define ASTC_PACK_SUPPORTED 1
	#endif
#endif

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
#define D3DFMT_R8G8B8        20
#define D3DFMT_R5G6B5        23
#define D3DFMT_A8            28
#define D3DFMT_L8            50
#define D3DFMT_A8L8          51

// GL source-layout enums. Guarded so this file builds with or without the GL headers.
#ifndef GL_UNSIGNED_BYTE
#define GL_UNSIGNED_BYTE				0x1401
#endif
#ifndef GL_UNSIGNED_SHORT
#define GL_UNSIGNED_SHORT				0x1403
#endif
#ifndef GL_FLOAT
#define GL_FLOAT						0x1406
#endif
#ifndef GL_HALF_FLOAT_ARB
#define GL_HALF_FLOAT_ARB				0x140B
#endif
#ifndef GL_HALF_FLOAT_OES
#define GL_HALF_FLOAT_OES				0x8D61
#endif
#ifndef GL_UNSIGNED_SHORT_4_4_4_4
#define GL_UNSIGNED_SHORT_4_4_4_4		0x8033
#endif
#ifndef GL_UNSIGNED_SHORT_5_5_5_1
#define GL_UNSIGNED_SHORT_5_5_5_1		0x8034
#endif
#ifndef GL_UNSIGNED_INT_8_8_8_8
#define GL_UNSIGNED_INT_8_8_8_8			0x8035
#endif
#ifndef GL_UNSIGNED_INT_10_10_10_2
#define GL_UNSIGNED_INT_10_10_10_2		0x8036
#endif
#ifndef GL_UNSIGNED_SHORT_5_6_5
#define GL_UNSIGNED_SHORT_5_6_5			0x8363
#endif
#ifndef GL_UNSIGNED_SHORT_4_4_4_4_REV
#define GL_UNSIGNED_SHORT_4_4_4_4_REV	0x8365
#endif
#ifndef GL_UNSIGNED_SHORT_1_5_5_5_REV
#define GL_UNSIGNED_SHORT_1_5_5_5_REV	0x8366
#endif
#ifndef GL_UNSIGNED_INT_8_8_8_8_REV
#define GL_UNSIGNED_INT_8_8_8_8_REV		0x8367
#endif
#ifndef GL_UNSIGNED_INT_2_10_10_10_REV
#define GL_UNSIGNED_INT_2_10_10_10_REV	0x8368
#endif
#ifndef GL_RED
#define GL_RED							0x1903
#endif
#ifndef GL_ALPHA
#define GL_ALPHA						0x1906
#endif
#ifndef GL_RGB
#define GL_RGB							0x1907
#endif
#ifndef GL_RGBA
#define GL_RGBA							0x1908
#endif
#ifndef GL_LUMINANCE
#define GL_LUMINANCE					0x1909
#endif
#ifndef GL_LUMINANCE_ALPHA
#define GL_LUMINANCE_ALPHA				0x190A
#endif
#ifndef GL_BGR
#define GL_BGR							0x80E0
#endif
#ifndef GL_BGRA
#define GL_BGRA							0x80E1
#endif
#ifndef GL_RG
#define GL_RG							0x8227
#endif

// ---------------------------------------------------------------------------
// ConVars
// ---------------------------------------------------------------------------

ConVar gl_astc_recompress( "gl_astc_recompress", "1", FCVAR_ARCHIVE,
	"Legacy toggle, kept so existing configs still parse. ASTC recompression is mandatory and this value is not read." );

ConVar gl_astc_block_ldr( "gl_astc_block_ldr", "6x6", FCVAR_ARCHIVE,
	"ASTC block footprint for 8/16-bit normalized sources (includes DXT1/3/5, which are decoded first). "
	"4x4 = best quality, largest. 8x8 = smallest. Invalid values fall back to 6x6." );

ConVar gl_astc_block_hdr( "gl_astc_block_hdr", "4x4", FCVAR_ARCHIVE,
	"ASTC block footprint for half/float (HDR) sources. Invalid values fall back to 4x4." );

ConVar gl_astc_quality( "gl_astc_quality", "60", FCVAR_ARCHIVE,
	"ASTC encoder effort, 0 (fastest) - 100 (exhaustive). Higher is slower on first encode; the disk cache removes that cost after the first launch." );

ConVar gl_astc_threads( "gl_astc_threads", "0", FCVAR_ARCHIVE,
	"Encoder worker threads. 0 = auto = every CPU core of the device. 1 = single threaded. Applied at first use; restart to change." );

ConVar gl_astc_cache( "gl_astc_cache", "1", FCVAR_ARCHIVE,
	"1 = cache encoded ASTC blocks on disk, keyed by source pixels and encode parameters. Later loads of the same texture skip encoding." );

ConVar gl_astc_cache_dir( "gl_astc_cache_dir", "astc_cache", FCVAR_ARCHIVE,
	"Directory for the ASTC encode cache file (astc_cache.bin), relative to the game's working directory. Safe to delete at any time." );

ConVar gl_astc_cache_max_mb( "gl_astc_cache_max_mb", "3584", FCVAR_ARCHIVE,
	"Size limit of the single ASTC cache file in MB (kept under 4 GB so it also works on FAT32 cards). When it is full the file is wiped and the hot entries are written back. Delete astc_cache.bin to start over." );

ConVar gl_astc_ram_cache_mb( "gl_astc_ram_cache_mb", "0", FCVAR_ARCHIVE,
	"RAM tier of the ASTC cache: hot (recently reused) encodes are kept in memory so they never touch the disk file again. "
	"0 = dynamic (up to 192 MB, shrinks to nothing when the device runs low on memory). -1 = off. >0 = fixed limit in MB." );

ConVar gl_astc_cache_cleanup_old( "gl_astc_cache_cleanup_old", "1", FCVAR_ARCHIVE,
	"1 = at startup, delete the thousands of old per-texture <16 hex digits>.astc files that earlier versions wrote into the cache directory. "
	"Only files with exactly that name pattern are touched." );

ConVar gl_astc_stats( "gl_astc_stats", "1", FCVAR_ARCHIVE,
	"1 = write encode throughput and cache hit statistics to the engine log every ~20 seconds while textures are being processed." );

ConVar gl_astc_alpha_weight( "gl_astc_alpha_weight", "1", FCVAR_ARCHIVE,
	"1 = alpha-weighted encoding for textures that have an alpha channel. Improves color accuracy in translucent areas." );

ConVar gl_astc_perceptual( "gl_astc_perceptual", "1", FCVAR_ARCHIVE,
	"1 = use the perceptual error metric for LDR (8/16-bit) encodes. Matches how the eye judges color error." );

ConVar gl_astc_debug( "gl_astc_debug", "1", FCVAR_ARCHIVE,
	"0 = off. 1 = check glGetError after the first ASTC level uploads and log failures to the engine log. "
	"2 = check after every upload and also log every upload." );

// ---------------------------------------------------------------------------
// Format classification
// ---------------------------------------------------------------------------

bool ASTC_IsEligibleFormat( int d3dFormat )
{
	switch ( d3dFormat )
	{
		case D3DFMT_R8G8B8:
		case D3DFMT_A8R8G8B8:
		case D3DFMT_X8R8G8B8:
		case D3DFMT_R5G6B5:
		case D3DFMT_X1R5G5B5:
		case D3DFMT_A1R5G5B5:
		case D3DFMT_A4R4G4B4:
		case D3DFMT_A8:
		case D3DFMT_A2B10G10R10:
		case D3DFMT_A2R10G10B10:
		case D3DFMT_A16B16G16R16:
		case D3DFMT_L8:
		case D3DFMT_A8L8:
		case D3DFMT_Q8W8V8U8:
		case D3DFMT_A16B16G16R16F:
		case D3DFMT_R32F:
		case D3DFMT_A32B32G32R32F:
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

bool ASTC_FormatHasAlpha( int d3dFormat )
{
	switch ( d3dFormat )
	{
		case D3DFMT_X8R8G8B8:
		case D3DFMT_X1R5G5B5:
		case D3DFMT_R5G6B5:
		case D3DFMT_R8G8B8:
		case D3DFMT_L8:
		case D3DFMT_R32F:
			return false;
		default:
			return true;
	}
}

// ---------------------------------------------------------------------------
// Block sizes
// ---------------------------------------------------------------------------

struct ASTCBlockEntry { int w, h; uint32_t linearEnum; uint32_t srgbEnum; };

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

static const ASTCBlockEntry* FindBlockEntry( int w, int h )
{
	for ( size_t i = 0; i < sizeof( s_blockTable ) / sizeof( s_blockTable[0] ); ++i )
	{
		if ( s_blockTable[i].w == w && s_blockTable[i].h == h )
			return &s_blockTable[i];
	}
	return NULL;
}

bool ASTC_IsValidBlockSize( int w, int h )
{
	return FindBlockEntry( w, h ) != NULL;
}

static void ParseBlockSize( const char* str, int defW, int defH, int* outW, int* outH )
{
	int w = 0, h = 0;
	if ( !str || sscanf( str, "%dx%d", &w, &h ) != 2 || !ASTC_IsValidBlockSize( w, h ) )
	{
		w = defW;
		h = defH;
	}
	*outW = w;
	*outH = h;
}

void ASTC_GetConfiguredBlockSize( bool isHDR, int* outW, int* outH )
{
	if ( isHDR )
		ParseBlockSize( gl_astc_block_hdr.GetString(), 4, 4, outW, outH );
	else
		ParseBlockSize( gl_astc_block_ldr.GetString(), 6, 6, outW, outH );
}

static uint32_t GLInternalFormatForBlock( int blockW, int blockH, bool srgb )
{
	const ASTCBlockEntry* e = FindBlockEntry( blockW, blockH );
	if ( !e )
		e = FindBlockEntry( 6, 6 );
	return srgb ? e->srgbEnum : e->linearEnum;
}

// ---------------------------------------------------------------------------
// Source layout helpers (used by the conversion fallback)
// ---------------------------------------------------------------------------

enum ChanLayout
{
	kLay_RGBA, kLay_BGRA, kLay_RGB, kLay_BGR, kLay_RG, kLay_R, kLay_L, kLay_LA, kLay_A, kLay_Invalid
};

static ChanLayout LayoutFromGLFormat( unsigned int fmt, int* outComps )
{
	switch ( fmt )
	{
		case GL_RGBA:				*outComps = 4; return kLay_RGBA;
		case GL_BGRA:				*outComps = 4; return kLay_BGRA;
		case GL_RGB:				*outComps = 3; return kLay_RGB;
		case GL_BGR:				*outComps = 3; return kLay_BGR;
		case GL_RG:					*outComps = 2; return kLay_RG;
		case GL_RED:				*outComps = 1; return kLay_R;
		case GL_LUMINANCE:			*outComps = 1; return kLay_L;
		case GL_LUMINANCE_ALPHA:	*outComps = 2; return kLay_LA;
		case GL_ALPHA:				*outComps = 1; return kLay_A;
		default:					*outComps = 0; return kLay_Invalid;
	}
}

template< typename T >
static inline T LoadT( const uint8_t* p )
{
	T v;
	memcpy( &v, p, sizeof( T ) );	// unaligned-safe
	return v;
}

static inline uint8_t ScaleBitsTo8( uint32_t v, int w )
{
	const uint32_t maxv = ( 1u << w ) - 1u;
	return (uint8_t)( ( v * 255u + ( maxv >> 1 ) ) / maxv );
}

static inline uint8_t ToU8( uint8_t v )  { return v; }
static inline uint8_t ToU8( uint16_t v ) { return (uint8_t)( ( (uint32_t)v * 255u + 32767u ) / 65535u ); }

template< typename V >
static inline void MapChannels( ChanLayout lay, const V* c, V one, V* r, V* g, V* b, V* a )
{
	V zero = (V)0;
	*r = zero; *g = zero; *b = zero; *a = one;
	switch ( lay )
	{
		case kLay_RGBA: *r = c[0]; *g = c[1]; *b = c[2]; *a = c[3]; break;
		case kLay_BGRA: *b = c[0]; *g = c[1]; *r = c[2]; *a = c[3]; break;
		case kLay_RGB:  *r = c[0]; *g = c[1]; *b = c[2]; break;
		case kLay_BGR:  *b = c[0]; *g = c[1]; *r = c[2]; break;
		case kLay_RG:   *r = c[0]; *g = c[1]; break;
		case kLay_R:    *r = c[0]; break;
		case kLay_L:    *r = c[0]; *g = c[0]; *b = c[0]; break;
		case kLay_LA:   *r = c[0]; *g = c[0]; *b = c[0]; *a = c[1]; break;
		case kLay_A:    *a = c[0]; break;
		default: break;
	}
}

template< typename T >
static void ConvertPlanarUNorm( const uint8_t* src, size_t n, ChanLayout lay, int comps, bool forceOpaque, uint8_t* dst )
{
	for ( size_t i = 0; i < n; ++i )
	{
		uint8_t c[4] = { 0, 0, 0, 0 };
		for ( int k = 0; k < comps; ++k )
			c[k] = ToU8( LoadT<T>( src + ( i * (size_t)comps + k ) * sizeof( T ) ) );

		uint8_t r, g, b, a;
		MapChannels<uint8_t>( lay, c, 255, &r, &g, &b, &a );
		if ( forceOpaque )
			a = 255;

		dst[i*4+0] = r; dst[i*4+1] = g; dst[i*4+2] = b; dst[i*4+3] = a;
	}
}

struct PackDesc { int nComps; int w[4]; bool rev; int totalBits; int storageBytes; };

static bool GetPackDesc( unsigned int type, PackDesc* d )
{
	switch ( type )
	{
		case GL_UNSIGNED_SHORT_5_6_5:			{ PackDesc t = { 3, {5,6,5,0},   false, 16, 2 }; *d = t; return true; }
		case GL_UNSIGNED_SHORT_4_4_4_4:			{ PackDesc t = { 4, {4,4,4,4},   false, 16, 2 }; *d = t; return true; }
		case GL_UNSIGNED_SHORT_4_4_4_4_REV:		{ PackDesc t = { 4, {4,4,4,4},   true,  16, 2 }; *d = t; return true; }
		case GL_UNSIGNED_SHORT_5_5_5_1:			{ PackDesc t = { 4, {5,5,5,1},   false, 16, 2 }; *d = t; return true; }
		case GL_UNSIGNED_SHORT_1_5_5_5_REV:		{ PackDesc t = { 4, {5,5,5,1},   true,  16, 2 }; *d = t; return true; }
		case GL_UNSIGNED_INT_8_8_8_8:			{ PackDesc t = { 4, {8,8,8,8},   false, 32, 4 }; *d = t; return true; }
		case GL_UNSIGNED_INT_8_8_8_8_REV:		{ PackDesc t = { 4, {8,8,8,8},   true,  32, 4 }; *d = t; return true; }
		case GL_UNSIGNED_INT_10_10_10_2:		{ PackDesc t = { 4, {10,10,10,2}, false, 32, 4 }; *d = t; return true; }
		case GL_UNSIGNED_INT_2_10_10_10_REV:	{ PackDesc t = { 4, {10,10,10,2}, true,  32, 4 }; *d = t; return true; }
		default: return false;
	}
}

static bool ConvertPackedUNorm( const uint8_t* src, size_t n, unsigned int fmt, unsigned int type, bool forceOpaque, uint8_t* dst )
{
	PackDesc d;
	if ( !GetPackDesc( type, &d ) )
		return false;

	ChanLayout lay;
	if ( d.nComps == 3 )
		lay = ( fmt == GL_BGR ) ? kLay_BGR : kLay_RGB;
	else
		lay = ( fmt == GL_BGRA ) ? kLay_BGRA : kLay_RGBA;

	int shift[4] = { 0, 0, 0, 0 };
	int acc = 0;
	for ( int k = 0; k < d.nComps; ++k )
	{
		shift[k] = d.rev ? acc : ( d.totalBits - acc - d.w[k] );
		acc += d.w[k];
	}

	for ( size_t i = 0; i < n; ++i )
	{
		uint32_t v = ( d.storageBytes == 2 ) ? (uint32_t)LoadT<uint16_t>( src + i * 2 ) : LoadT<uint32_t>( src + i * 4 );

		uint8_t c[4] = { 0, 0, 0, 0 };
		for ( int k = 0; k < d.nComps; ++k )
		{
			const uint32_t field = ( v >> shift[k] ) & ( ( 1u << d.w[k] ) - 1u );
			c[k] = ScaleBitsTo8( field, d.w[k] );
		}

		uint8_t r, g, b, a;
		MapChannels<uint8_t>( lay, c, 255, &r, &g, &b, &a );
		if ( forceOpaque )
			a = 255;

		dst[i*4+0] = r; dst[i*4+1] = g; dst[i*4+2] = b; dst[i*4+3] = a;
	}
	return true;
}

// Returns malloc'd n*4 bytes (RGBA8), or NULL if (fmt, type) isn't understood.
static uint8_t* ConvertToRGBA8( const void* srcData, size_t n, unsigned int glFormat, unsigned int glType, bool forceOpaque )
{
	uint8_t* dst = (uint8_t*)malloc( n * 4 );
	if ( !dst )
		return NULL;

	const uint8_t* src = (const uint8_t*)srcData;
	bool ok = false;

	int comps = 0;
	ChanLayout lay = LayoutFromGLFormat( glFormat, &comps );

	if ( glType == GL_UNSIGNED_BYTE && lay != kLay_Invalid )
	{
		ConvertPlanarUNorm<uint8_t>( src, n, lay, comps, forceOpaque, dst );
		ok = true;
	}
	else if ( glType == GL_UNSIGNED_SHORT && lay != kLay_Invalid )
	{
		ConvertPlanarUNorm<uint16_t>( src, n, lay, comps, forceOpaque, dst );
		ok = true;
	}
	else if ( glType != GL_UNSIGNED_BYTE && glType != GL_UNSIGNED_SHORT )
	{
		ok = ConvertPackedUNorm( src, n, glFormat, glType, forceOpaque, dst );
	}

	if ( !ok )
	{
		free( dst );
		return NULL;
	}
	return dst;
}

static float HalfToFloat( uint16_t h )
{
	uint32_t sign = ( h & 0x8000u ) << 16;
	uint32_t exp  = ( h >> 10 ) & 0x1F;
	uint32_t mant = h & 0x3FF;
	uint32_t bits;
	if ( exp == 0 )
	{
		if ( mant == 0 )
		{
			bits = sign;
		}
		else
		{
			int e = -1;
			do { e++; mant <<= 1; } while ( !( mant & 0x400 ) );
			mant &= 0x3FF;
			bits = sign | ( (uint32_t)( 127 - 15 - e ) << 23 ) | ( mant << 13 );
		}
	}
	else if ( exp == 31 )
	{
		bits = sign | 0x7F800000u | ( mant << 13 );
	}
	else
	{
		bits = sign | ( ( exp + 112 ) << 23 ) | ( mant << 13 );
	}
	float f;
	memcpy( &f, &bits, sizeof( f ) );
	return f;
}

static inline float SanitizeHDR( float v )
{
	if ( !( v >= 0.0f ) )
		return 0.0f;
	if ( v > 65504.0f )
		return 65504.0f;
	return v;
}

// Returns malloc'd n*4 floats, or NULL if (fmt, type) isn't understood.
static float* ConvertToRGBAF32( const void* srcData, size_t n, unsigned int glFormat, unsigned int glType, bool forceOpaque )
{
	float* dst = (float*)malloc( n * 4 * sizeof( float ) );
	if ( !dst )
		return NULL;

	const uint8_t* src = (const uint8_t*)srcData;

	int comps = 0;
	ChanLayout lay = LayoutFromGLFormat( glFormat, &comps );
	const bool isHalf  = ( glType == GL_HALF_FLOAT_ARB || glType == GL_HALF_FLOAT_OES );
	const bool isFloat = ( glType == GL_FLOAT );

	if ( lay != kLay_Invalid && ( isHalf || isFloat ) )
	{
		for ( size_t i = 0; i < n; ++i )
		{
			float c[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			for ( int k = 0; k < comps; ++k )
			{
				const size_t idx = i * (size_t)comps + k;
				c[k] = isHalf ? HalfToFloat( LoadT<uint16_t>( src + idx * 2 ) ) : LoadT<float>( src + idx * 4 );
			}

			float r, g, b, a;
			MapChannels<float>( lay, c, 1.0f, &r, &g, &b, &a );
			if ( forceOpaque )
				a = 1.0f;

			dst[i*4+0] = SanitizeHDR( r ); dst[i*4+1] = SanitizeHDR( g );
			dst[i*4+2] = SanitizeHDR( b ); dst[i*4+3] = SanitizeHDR( a );
		}
		return dst;
	}

	// Integer source for an HDR encode: widen to float.
	uint8_t* tmp = ConvertToRGBA8( srcData, n, glFormat, glType, forceOpaque );
	if ( !tmp )
	{
		free( dst );
		return NULL;
	}
	for ( size_t i = 0; i < n * 4; ++i )
		dst[i] = tmp[i] * ( 1.0f / 255.0f );
	free( tmp );
	return dst;
}

// Bytes per texel for (fmt, type). Used for cache keys.
static size_t SrcTexelBytes( unsigned int fmt, unsigned int type )
{
	PackDesc d;
	if ( GetPackDesc( type, &d ) )
		return (size_t)d.storageBytes;

	int comps = 0;
	LayoutFromGLFormat( fmt, &comps );
	size_t cs = 0;
	if ( type == GL_UNSIGNED_BYTE )
		cs = 1;
	else if ( type == GL_UNSIGNED_SHORT || type == GL_HALF_FLOAT_ARB || type == GL_HALF_FLOAT_OES )
		cs = 2;
	else if ( type == GL_FLOAT )
		cs = 4;
	return (size_t)comps * cs;
}

// ---------------------------------------------------------------------------
// astcenc glue
// ---------------------------------------------------------------------------
#if defined( HAVE_ASTCENC )

static const uint32_t kCacheVersion = 3;

static float QualityFromPreset( int qualityPreset )
{
	if ( qualityPreset <= 10 )		return ASTCENC_PRE_FASTEST;
	else if ( qualityPreset <= 35 )	return ASTCENC_PRE_FAST;
	else if ( qualityPreset <= 65 )	return ASTCENC_PRE_MEDIUM;
	else if ( qualityPreset <= 90 )	return ASTCENC_PRE_THOROUGH;
	return ASTCENC_PRE_EXHAUSTIVE;
}

// ---- fast 64-bit hash (xxHash64 algorithm) ---------------------------------
// Used for cache keys (hashing megabytes of source pixels) and for payload checksums.
static const uint64_t XXP1 = 11400714785074694791ULL;
static const uint64_t XXP2 = 14029467366897019727ULL;
static const uint64_t XXP3 = 1609587929392839161ULL;
static const uint64_t XXP4 = 9650029242287828579ULL;
static const uint64_t XXP5 = 2870177450012600261ULL;

static inline uint64_t Rotl64( uint64_t x, int r ) { return ( x << r ) | ( x >> ( 64 - r ) ); }
static inline uint64_t Read64( const uint8_t* p ) { uint64_t v; memcpy( &v, p, 8 ); return v; }
static inline uint32_t Read32( const uint8_t* p ) { uint32_t v; memcpy( &v, p, 4 ); return v; }
static inline uint64_t XXRound( uint64_t acc, uint64_t in ) { acc += in * XXP2; acc = Rotl64( acc, 31 ); acc *= XXP1; return acc; }
static inline uint64_t XXMerge( uint64_t acc, uint64_t v ) { v = XXRound( 0, v ); acc ^= v; return acc * XXP1 + XXP4; }

static uint64_t Hash64( const void* data, size_t len, uint64_t seed )
{
	const uint8_t* p = (const uint8_t*)data;
	const uint8_t* end = p + len;
	uint64_t h;

	if ( len >= 32 )
	{
		const uint8_t* limit = end - 32;
		uint64_t v1 = seed + XXP1 + XXP2;
		uint64_t v2 = seed + XXP2;
		uint64_t v3 = seed;
		uint64_t v4 = seed - XXP1;
		do
		{
			v1 = XXRound( v1, Read64( p ) ); p += 8;
			v2 = XXRound( v2, Read64( p ) ); p += 8;
			v3 = XXRound( v3, Read64( p ) ); p += 8;
			v4 = XXRound( v4, Read64( p ) ); p += 8;
		} while ( p <= limit );

		h = Rotl64( v1, 1 ) + Rotl64( v2, 7 ) + Rotl64( v3, 12 ) + Rotl64( v4, 18 );
		h = XXMerge( h, v1 );
		h = XXMerge( h, v2 );
		h = XXMerge( h, v3 );
		h = XXMerge( h, v4 );
	}
	else
	{
		h = seed + XXP5;
	}

	h += (uint64_t)len;

	while ( p + 8 <= end )
	{
		h ^= XXRound( 0, Read64( p ) );
		h = Rotl64( h, 27 ) * XXP1 + XXP4;
		p += 8;
	}
	if ( p + 4 <= end )
	{
		h ^= (uint64_t)Read32( p ) * XXP1;
		h = Rotl64( h, 23 ) * XXP2 + XXP3;
		p += 4;
	}
	while ( p < end )
	{
		h ^= (uint64_t)( *p ) * XXP5;
		h = Rotl64( h, 11 ) * XXP1;
		++p;
	}

	h ^= h >> 33;
	h *= XXP2;
	h ^= h >> 29;
	h *= XXP3;
	h ^= h >> 32;
	return h;
}

static int64_t NowMs()
{
	return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch() ).count();
}

// ---- Statistics ---------------------------------------------------------------
struct AstcStats
{
	std::atomic<uint64_t> encImages;
	std::atomic<uint64_t> encPixels;
	std::atomic<uint64_t> encMicros;
	std::atomic<uint64_t> hits;
	std::atomic<uint64_t> misses;
	std::atomic<uint64_t> stores;
	std::atomic<uint64_t> storedBytes;
	std::atomic<uint64_t> poolThreads;
	std::atomic<uint64_t> ramHits;
	std::atomic<uint64_t> diskHits;
};
static AstcStats s_stats;

// ---- Persistent worker pool -------------------------------------------------
// astcenc is built for this pattern: N threads call astcenc_compress_image() on
// the same context with distinct thread indices and share the work dynamically.
// The pool keeps those threads alive between images (no create/join per image),
// uses plain pthreads so a failed thread creation degrades the pool size instead
// of aborting, and is sized to every CPU core of the device.
class EncodePool
{
public:
	explicit EncodePool( unsigned n )
		: m_size( 1 ), m_gen( 0 ), m_pending( 0 ), m_active( 0 ), m_stop( false ), m_fn( NULL )
	{
		const unsigned want = n ? n : 1;

		pthread_attr_t attr;
		pthread_attr_init( &attr );
		pthread_attr_setstacksize( &attr, 2 * 1024 * 1024 );	// astcenc keeps sizeable buffers on the stack

		for ( unsigned t = 1; t < want; ++t )
		{
			ThreadArg* arg = new ThreadArg;
			arg->pool = this;
			arg->idx = t;

			pthread_t th;
			if ( pthread_create( &th, &attr, &EncodePool::ThreadEntry, arg ) != 0 )
			{
				delete arg;
				Warning( "ASTC: could only start %u of %u encoder threads\n", m_size, want );
				break;
			}
			m_workers.push_back( th );
			++m_size;
		}

		pthread_attr_destroy( &attr );
	}

	~EncodePool()
	{
		{
			std::lock_guard<std::mutex> lock( m_mtx );
			m_stop = true;
		}
		m_cv.notify_all();
		for ( size_t i = 0; i < m_workers.size(); ++i )
			pthread_join( m_workers[i], NULL );
	}

	unsigned Size() const { return m_size; }

	// Runs fn(0..active-1) concurrently. The calling thread is index 0.
	void Run( unsigned active, const std::function<void( unsigned )>& fn )
	{
		if ( active > m_size )
			active = m_size;
		if ( active < 1 )
			active = 1;

		std::lock_guard<std::mutex> runLock( m_runMtx );
		if ( active > 1 )
		{
			{
				std::lock_guard<std::mutex> lock( m_mtx );
				m_fn = &fn;
				m_active = active;
				m_pending = active - 1;
				++m_gen;
			}
			m_cv.notify_all();
		}

		fn( 0 );

		if ( active > 1 )
		{
			std::unique_lock<std::mutex> lock( m_mtx );
			m_doneCv.wait( lock, [this]() { return m_pending == 0; } );
			m_fn = NULL;
		}
	}

private:
	struct ThreadArg
	{
		EncodePool*	pool;
		unsigned	idx;
	};

	static void* ThreadEntry( void* p )
	{
		ThreadArg* a = (ThreadArg*)p;
		EncodePool* pool = a->pool;
		const unsigned idx = a->idx;
		delete a;

#if defined( __linux__ ) || defined( __ANDROID__ )
		char name[16];
		snprintf( name, sizeof( name ), "astc_enc%u", idx );
		prctl( PR_SET_NAME, (unsigned long)name, 0, 0, 0 );
#endif
		pool->WorkerMain( idx );
		return NULL;
	}

	void WorkerMain( unsigned idx )
	{
		uint64_t seen = 0;
		for ( ;; )
		{
			const std::function<void( unsigned )>* fn = NULL;
			{
				std::unique_lock<std::mutex> lock( m_mtx );
				m_cv.wait( lock, [this, &seen]() { return m_stop || m_gen != seen; } );
				if ( m_stop )
					return;
				seen = m_gen;
				if ( idx >= m_active )
					continue;
				fn = m_fn;
			}

			( *fn )( idx );

			std::lock_guard<std::mutex> lock( m_mtx );
			if ( --m_pending == 0 )
				m_doneCv.notify_one();
		}
	}

	unsigned					m_size;
	std::vector<pthread_t>		m_workers;
	std::mutex					m_mtx;
	std::mutex					m_runMtx;
	std::condition_variable		m_cv;
	std::condition_variable		m_doneCv;
	uint64_t					m_gen;
	unsigned					m_pending;
	unsigned					m_active;
	bool						m_stop;
	const std::function<void( unsigned )>* m_fn;
};

static unsigned ChooseWorkerCount()
{
	const int configured = gl_astc_threads.GetInt();
	if ( configured > 0 )
		return (unsigned)( configured > 32 ? 32 : configured );

	// Auto: every core. Cover phones that report fewer cores as "online" while idle.
	unsigned hw = std::thread::hardware_concurrency();
#if !defined( _WIN32 )
	const long onln = sysconf( _SC_NPROCESSORS_ONLN );
	const long conf = sysconf( _SC_NPROCESSORS_CONF );
	if ( onln > (long)hw ) hw = (unsigned)onln;
	if ( conf > (long)hw ) hw = (unsigned)conf;
#endif
	if ( hw == 0 )
		hw = 1;
	return hw > 32 ? 32 : hw;
}

static EncodePool* CreatePool()
{
	const unsigned n = ChooseWorkerCount();
	EncodePool* pool = new EncodePool( n );
	s_stats.poolThreads.store( pool->Size() );
	Msg( "ASTC: encoder thread pool started: %u thread(s) (requested %u)\n", pool->Size(), n );
#if !defined( _WIN32 )
	// Which shared library holds this copy of the encoder? (If the log shows two copies, this names them.)
	Dl_info info;
	memset( &info, 0, sizeof( info ) );
	if ( dladdr( (void*)&ChooseWorkerCount, &info ) && info.dli_fname )
		Msg( "ASTC: encoder module %s\n", info.dli_fname );
#endif
	return pool;
}

// Intentionally never destroyed: avoids static-destruction-order issues at exit.
static EncodePool* GetPool()
{
	static EncodePool* pool = CreatePool();
	return pool;
}

// ---- Shared, read-only-after-build encoder contexts --------------------------
// Building an astcenc context creates its large search tables. One context per
// (profile, block, quality, flags) is built, sized for the whole pool, and then
// reused by every texture that needs it. Any subset of the pool's thread indices
// may drive it, so small images use one thread and big ones use every core.
struct SharedCtx
{
	astcenc_profile	profile;
	int				blockW, blockH;
	int				qualityKey;		// quality * 1000, integer for exact matching
	uint32_t		flags;
	astcenc_context* ctx;
};

static std::mutex				s_ctxMutex;
static std::vector<SharedCtx>	s_ctxs;
static std::mutex				s_encodeMutex;	// an astcenc context must not be used by two calls at once

static astcenc_context* AcquireSharedContext( astcenc_profile profile, int bw, int bh, float quality, uint32_t flags )
{
	const unsigned threads = GetPool()->Size();

	std::lock_guard<std::mutex> lock( s_ctxMutex );

	const int qKey = (int)( quality * 1000.0f + 0.5f );
	for ( size_t i = 0; i < s_ctxs.size(); ++i )
	{
		const SharedCtx& c = s_ctxs[i];
		if ( c.profile == profile && c.blockW == bw && c.blockH == bh && c.qualityKey == qKey && c.flags == flags )
			return c.ctx;
	}

	astcenc_config config;
	astcenc_error status = astcenc_config_init( profile, bw, bh, 1, quality, flags, &config );
	if ( status != ASTCENC_SUCCESS )
	{
		Warning( "ASTC: astcenc_config_init(%dx%d) failed: %s\n", bw, bh, astcenc_get_error_string( status ) );
		return NULL;
	}

	astcenc_context* ctx = NULL;
	status = astcenc_context_alloc( &config, threads, &ctx );
	if ( status != ASTCENC_SUCCESS )
	{
		Warning( "ASTC: astcenc_context_alloc failed: %s\n", astcenc_get_error_string( status ) );
		return NULL;
	}

	SharedCtx entry = { profile, bw, bh, qKey, flags, ctx };
	s_ctxs.push_back( entry );
	Msg( "ASTC: encoder context #%d ready (profile %d, block %dx%d, quality %.0f, flags 0x%X, %u thread slot(s))\n",
		 (int)s_ctxs.size(), (int)profile, bw, bh, quality, (unsigned)flags, threads );
	return ctx;
}

// About how many ASTC blocks one thread should get before waking it pays off.
static const size_t kBlocksPerThread = 24;

static astcenc_error EncodeImage( astcenc_profile profile, int bw, int bh, float quality, uint32_t flags,
								  astcenc_image& image, const astcenc_swizzle& swz,
								  uint8_t* out, size_t outSize, size_t blockCount )
{
	EncodePool* pool = GetPool();

	astcenc_context* ctx = AcquireSharedContext( profile, bw, bh, quality, flags );
	if ( !ctx )
		return ASTCENC_ERR_BAD_PARAM;

	size_t want = blockCount / kBlocksPerThread;
	if ( want < 1 )
		want = 1;
	const unsigned active = want > pool->Size() ? pool->Size() : (unsigned)want;

	std::lock_guard<std::mutex> lock( s_encodeMutex );
	const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();

	std::vector<astcenc_error> status( active, ASTCENC_SUCCESS );
	if ( active == 1 )
	{
		status[0] = astcenc_compress_image( ctx, &image, &swz, out, outSize, 0 );
	}
	else
	{
		pool->Run( active, [&]( unsigned t )
		{
			status[t] = astcenc_compress_image( ctx, &image, &swz, out, outSize, t );
		} );
	}
	astcenc_compress_reset( ctx );	// required before the context can encode another image

	const uint64_t us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now() - t0 ).count();
	s_stats.encMicros.fetch_add( us );
	s_stats.encPixels.fetch_add( (uint64_t)image.dim_x * (uint64_t)image.dim_y );
	s_stats.encImages.fetch_add( 1 );

	for ( unsigned t = 0; t < active; ++t )
	{
		if ( status[t] != ASTCENC_SUCCESS )
			return status[t];
	}
	return ASTCENC_SUCCESS;
}

// Encoder flags derived from the settings. Shared by the encoder and the cache keys.
static uint32_t ComputeEncodeFlags( bool isHDR, bool forceOpaque )
{
	uint32_t flags = 0;
	if ( !forceOpaque && gl_astc_alpha_weight.GetBool() )
		flags |= ASTCENC_FLG_USE_ALPHA_WEIGHT;
	if ( !isHDR && gl_astc_perceptual.GetBool() )
		flags |= ASTCENC_FLG_USE_PERCEPTUAL;
	return flags;
}

// ---- Source preparation -----------------------------------------------------
struct SourcePlan
{
	const void*			ptr;		// what astcenc reads
	void*				owned;		// malloc'd conversion buffer, or NULL
	bool				isFloat;
	astcenc_swizzle		swz;
};

static void SetIdentitySwizzle( astcenc_swizzle* s )
{
	s->r = ASTCENC_SWZ_R;
	s->g = ASTCENC_SWZ_G;
	s->b = ASTCENC_SWZ_B;
	s->a = ASTCENC_SWZ_A;
}

// Fast path: 8-bit BGRA/RGBA memory order needs no copy. Channel order and
// forced alpha are handled by the swizzle. Everything else gets converted.
static bool PrepareSource( const void* src, size_t n, unsigned int fmt, unsigned int type,
						   bool isHDR, bool forceOpaque, SourcePlan* plan )
{
	memset( plan, 0, sizeof( *plan ) );
	SetIdentitySwizzle( &plan->swz );

	if ( !isHDR )
	{
		const bool byteOrder = ( type == GL_UNSIGNED_BYTE || type == GL_UNSIGNED_INT_8_8_8_8_REV )
							&& ( fmt == GL_RGBA || fmt == GL_BGRA );
		if ( byteOrder )
		{
			plan->ptr = src;
			if ( fmt == GL_BGRA )
			{
				// Memory is B,G,R,A. astcenc reads memory slots as r,g,b,a,
				// so swap the red and blue slots.
				plan->swz.r = ASTCENC_SWZ_B;
				plan->swz.b = ASTCENC_SWZ_R;
			}
			if ( forceOpaque )
				plan->swz.a = ASTCENC_SWZ_1;
			return true;
		}

		uint8_t* rgba = ConvertToRGBA8( src, n, fmt, type, forceOpaque );
		if ( !rgba )
			return false;
		plan->owned = rgba;
		plan->ptr = rgba;
		return true;
	}

	float* rgbaF = ConvertToRGBAF32( src, n, fmt, type, forceOpaque );
	if ( !rgbaF )
		return false;
	plan->owned = rgbaF;
	plan->ptr = rgbaF;
	plan->isFloat = true;
	return true;
}

// ---- Cache --------------------------------------------------------------------
// Two tiers:
//   RAM  - a dynamic LRU of the "essential" entries: the ones that are being reused. Its size follows the
//          free memory of the device and drops to nothing when the system is short on RAM.
//   FILE - everything else lives only in ONE file, astc_cache/astc_cache.bin: a hash table followed by an
//          append-only data area. It is opened once, read with pread() and appended to in place. No other
//          file is ever created (no per-texture files, no temp files, no renames).

static uint64_t ComputeCacheKey( const void* src, size_t srcBytes, const uint32_t params[12] )
{
	const uint64_t seed = Hash64( params, sizeof( uint32_t ) * 12, 0x43545341ULL );
	return Hash64( src, srcBytes, seed );
}

// MemAvailable from /proc/meminfo in kB, or 0 if it can't be read.
static uint64_t ReadMemAvailableKB()
{
#if !defined( _WIN32 )
	FILE* f = fopen( "/proc/meminfo", "r" );
	if ( !f )
		return 0;
	char line[160];
	uint64_t kb = 0;
	while ( fgets( line, sizeof( line ), f ) )
	{
		unsigned long long v = 0;
		if ( sscanf( line, "MemAvailable: %llu kB", &v ) == 1 )
		{
			kb = (uint64_t)v;
			break;
		}
	}
	fclose( f );
	return kb;
#else
	return 0;
#endif
}

// Dynamic size of the RAM tier in bytes. Re-evaluated every couple of seconds.
static size_t RamBudgetBytes()
{
	static std::atomic<int64_t> s_lastMs( 0 );
	static std::atomic<uint64_t> s_budget( 0 );

	const int64_t now = NowMs();
	const int64_t last = s_lastMs.load();
	if ( last == 0 || now - last > 2000 )
	{
		s_lastMs.store( now );

		const int mb = gl_astc_ram_cache_mb.GetInt();
		uint64_t budget = 0;
		if ( mb >= 0 )
		{
			budget = mb > 0 ? (uint64_t)mb * 1024ull * 1024ull : 192ull * 1024ull * 1024ull;

			const uint64_t availKB = ReadMemAvailableKB();
			if ( availKB > 0 )
			{
				const uint64_t avail = availKB * 1024ull;
				// At most an eighth of the free memory, and nothing at all when the device is nearly out of it.
				uint64_t byAvail = avail / 8;
				if ( avail < 400ull * 1024ull * 1024ull )
					byAvail = 0;
				if ( byAvail < budget )
					budget = byAvail;
			}
		}
		s_budget.store( budget );
	}
	return (size_t)s_budget.load();
}

// Entries at or below this size are cheap enough to keep in RAM from their first use.
static const size_t kRamTinyBytes = 32 * 1024;
// Entries above this size never go to the RAM tier.
static const size_t kRamEntryMax = 2 * 1024 * 1024;

class RamCache
{
public:
	RamCache() : m_head( NULL ), m_tail( NULL ), m_bytes( 0 ) {}

	bool Get( uint64_t key, uint8_t* dst, size_t size )
	{
		std::lock_guard<std::mutex> lk( m_mtx );
		std::unordered_map<uint64_t, Node*>::iterator it = m_map.find( key );
		if ( it == m_map.end() )
			return false;
		Node* n = it->second;
		if ( n->size != size )
			return false;
		memcpy( dst, n->data, size );
		MoveToFront( n );
		return true;
	}

	void Put( uint64_t key, const uint8_t* data, size_t size, size_t budget )
	{
		if ( budget == 0 || size == 0 || size > kRamEntryMax || size > budget )
			return;

		std::lock_guard<std::mutex> lk( m_mtx );
		std::unordered_map<uint64_t, Node*>::iterator it = m_map.find( key );
		if ( it != m_map.end() )
		{
			MoveToFront( it->second );
			return;
		}

		EvictTo( budget - size );

		Node* n = new Node;
		n->data = (uint8_t*)malloc( size );
		if ( !n->data )
		{
			delete n;
			return;
		}
		memcpy( n->data, data, size );
		n->key = key;
		n->size = (uint32_t)size;
		n->prev = NULL;
		n->next = m_head;
		if ( m_head )
			m_head->prev = n;
		m_head = n;
		if ( !m_tail )
			m_tail = n;
		m_map[key] = n;
		m_bytes += size;
	}

	void Trim( size_t budget )
	{
		std::lock_guard<std::mutex> lk( m_mtx );
		EvictTo( budget );
	}

	size_t Bytes()
	{
		std::lock_guard<std::mutex> lk( m_mtx );
		return m_bytes;
	}

	// Calls f(key, data, size) for the most recently used entries until maxBytes have been visited.
	void ForEachHot( size_t maxBytes, const std::function<void( uint64_t, const uint8_t*, size_t )>& f )
	{
		std::lock_guard<std::mutex> lk( m_mtx );
		size_t seen = 0;
		for ( Node* n = m_head; n && seen + n->size <= maxBytes; n = n->next )
		{
			f( n->key, n->data, n->size );
			seen += n->size;
		}
	}

private:
	struct Node
	{
		uint64_t	key;
		uint8_t*	data;
		uint32_t	size;
		Node*		prev;
		Node*		next;
	};

	void Unlink( Node* n )
	{
		if ( n->prev ) n->prev->next = n->next; else m_head = n->next;
		if ( n->next ) n->next->prev = n->prev; else m_tail = n->prev;
		n->prev = n->next = NULL;
	}

	void MoveToFront( Node* n )
	{
		if ( n == m_head )
			return;
		Unlink( n );
		n->next = m_head;
		if ( m_head )
			m_head->prev = n;
		m_head = n;
		if ( !m_tail )
			m_tail = n;
	}

	void EvictTo( size_t target )
	{
		while ( m_bytes > target && m_tail )
		{
			Node* n = m_tail;
			Unlink( n );
			m_map.erase( n->key );
			m_bytes -= n->size;
			free( n->data );
			delete n;
		}
	}

	std::mutex							m_mtx;
	std::unordered_map<uint64_t, Node*>	m_map;
	Node*								m_head;
	Node*								m_tail;
	size_t								m_bytes;
};

static RamCache s_ram;

#if defined( ASTC_PACK_SUPPORTED )

static const char		kPackMagic[8] = { 'A', 'S', 'T', 'C', 'P', 'A', 'K', '3' };
static const uint32_t	kPackVersion = 3;
static const uint32_t	kPackSlotBits = 19;						// 524288 slots
static const uint32_t	kPackSlotCount = 1u << kPackSlotBits;
static const uint32_t	kPackMaxEntries = ( kPackSlotCount / 10 ) * 7;	// keep the table <= 70% full
static const uint64_t	kPackHeaderBytes = 4096;
static const uint32_t	kRecMagic = 0x31434552u;				// "REC1"
static const uint64_t	kPayloadSeed = 0x4B4B4B4B31ULL;

#pragma pack( push, 1 )
struct PackHeader
{
	char		magic[8];
	uint32_t	version;
	uint32_t	slotBits;
	uint64_t	tableOffset;
	uint64_t	dataStart;
	uint8_t		pad[4096 - 8 - 4 - 4 - 8 - 8];
};
struct PackSlot
{
	uint64_t	key;		// 0 = empty
	uint32_t	off16;		// record offset / 16
	uint32_t	size;		// payload bytes
};
struct PackRec
{
	uint32_t	magic;
	uint32_t	size;
	uint64_t	key;
	uint64_t	hash;		// Hash64 of the payload
};
#pragma pack( pop )

static_assert( sizeof( PackHeader ) == 4096, "PackHeader must be 4096 bytes" );
static_assert( sizeof( PackSlot ) == 16, "PackSlot must be 16 bytes" );
static_assert( sizeof( PackRec ) == 24, "PackRec must be 24 bytes" );

static bool PReadFull( int fd, void* buf, size_t n, uint64_t off )
{
	uint8_t* p = (uint8_t*)buf;
	while ( n > 0 )
	{
		const ssize_t r = pread( fd, p, n, (off_t)off );
		if ( r < 0 )
		{
			if ( errno == EINTR )
				continue;
			return false;
		}
		if ( r == 0 )
			return false;
		p += r;
		off += (uint64_t)r;
		n -= (size_t)r;
	}
	return true;
}

static bool PWriteFull( int fd, const void* buf, size_t n, uint64_t off )
{
	const uint8_t* p = (const uint8_t*)buf;
	while ( n > 0 )
	{
		const ssize_t r = pwrite( fd, p, n, (off_t)off );
		if ( r < 0 )
		{
			if ( errno == EINTR )
				continue;
			return false;
		}
		p += r;
		off += (uint64_t)r;
		n -= (size_t)r;
	}
	return true;
}

static uint32_t PackHome( uint64_t key )
{
	return (uint32_t)( ( key * 0x9E3779B97F4A7C15ULL ) >> ( 64 - kPackSlotBits ) );
}

// One-time removal of the old per-texture cache files (<16 hex>.astc and their .tmp leftovers).
static bool IsOldCacheFileName( const char* name )
{
	for ( int i = 0; i < 16; ++i )
	{
		const char c = name[i];
		if ( !( ( c >= '0' && c <= '9' ) || ( c >= 'a' && c <= 'f' ) ) )
			return false;
	}
	if ( strcmp( name + 16, ".astc" ) == 0 )
		return true;
	if ( strncmp( name + 16, ".astc.", 6 ) == 0 )
	{
		const size_t len = strlen( name );
		return len > 6 + 16 + 4 && strcmp( name + len - 4, ".tmp" ) == 0;
	}
	return false;
}

static void* OldCacheCleanupMain( void* arg )
{
	char* dir = (char*)arg;
	std::vector<std::string> names;

	DIR* d = opendir( dir );
	if ( d )
	{
		struct dirent* e;
		while ( ( e = readdir( d ) ) != NULL )
		{
			if ( IsOldCacheFileName( e->d_name ) )
				names.push_back( e->d_name );
		}
		closedir( d );
	}

	size_t removed = 0;
	for ( size_t i = 0; i < names.size(); ++i )
	{
		char path[1200];
		snprintf( path, sizeof( path ), "%s/%s", dir, names[i].c_str() );
		if ( unlink( path ) == 0 )
			++removed;
		if ( ( i & 127 ) == 127 )
			usleep( 2000 );		// be gentle with the storage while the game is loading
	}
	if ( !names.empty() )
		Msg( "ASTC: removed %u old per-texture cache file(s); the cache is now the single file astc_cache.bin\n", (unsigned)removed );

	free( dir );
	return NULL;
}

static void StartOldCacheCleanup( const char* dirName )
{
	static std::atomic<bool> s_started( false );
	if ( !gl_astc_cache_cleanup_old.GetBool() || s_started.exchange( true ) )
		return;

	char* dir = strdup( dirName );
	if ( !dir )
		return;

	pthread_t th;
	pthread_attr_t attr;
	pthread_attr_init( &attr );
	pthread_attr_setdetachstate( &attr, PTHREAD_CREATE_DETACHED );
	if ( pthread_create( &th, &attr, &OldCacheCleanupMain, dir ) != 0 )
		free( dir );
	pthread_attr_destroy( &attr );
}

class AstcPack
{
public:
	AstcPack() : m_fd( -1 ), m_state( 0 ), m_count( 0 ), m_syncedSize( 0 ), m_maxBytes( 0 ), m_lastReloadMs( 0 ), m_resets( 0 ) {}

	bool Ready()
	{
		const int st = m_state.load();
		if ( st != 0 )
			return st > 0;
		return Open();
	}

	bool Lookup( uint64_t key, uint8_t* dst, size_t size );
	void Store( uint64_t key, const uint8_t* data, size_t size );

	uint64_t SizeBytes() { std::lock_guard<std::mutex> lk( m_mtx ); return m_syncedSize; }

private:
	enum AppendResult { kAppended, kAlready, kFull, kFailed };

	bool Open();
	bool FindMem( uint64_t key, PackSlot* out, uint32_t* outIdx );		// caller holds m_mtx
	bool ReloadTable();
	AppendResult AppendLocked( uint64_t key, const uint8_t* data, size_t size, PackSlot* outSlot, uint32_t* outIdx, uint64_t* outEnd, uint64_t* outEndBefore );
	bool ResetLocked();

	int						m_fd;
	std::atomic<int>		m_state;		// 0 = not tried yet, 1 = ready, -1 = unavailable
	std::mutex				m_mtx;			// guards m_slots, m_hot, m_count, m_syncedSize
	std::mutex				m_ioMtx;		// serializes this copy's file writes; flock() covers other copies
	std::vector<PackSlot>	m_slots;
	std::vector<uint8_t>	m_hot;			// per slot: how many times it was read from the file
	uint32_t				m_count;
	uint64_t				m_syncedSize;	// file size our in-memory table is known to match
	uint64_t				m_maxBytes;
	std::atomic<int64_t>	m_lastReloadMs;
	uint32_t				m_resets;
};

bool AstcPack::Open()
{
	std::lock_guard<std::mutex> lk( m_mtx );
	if ( m_state.load() != 0 )
		return m_state.load() > 0;
	m_state.store( -1 );

	if ( !gl_astc_cache.GetBool() || gl_astc_cache_dir.GetString()[0] == '\0' )
		return false;

	const char* dir = gl_astc_cache_dir.GetString();
	ASTC_MKDIR( dir );		// EEXIST is fine

	char path[1200];
	snprintf( path, sizeof( path ), "%s/astc_cache.bin", dir );

	const int fd = open( path, O_RDWR | O_CREAT, 0644 );
	if ( fd < 0 )
	{
		Warning( "ASTC: cannot open cache file %s (errno %d); the disk cache is off for this session\n", path, errno );
		return false;
	}

	const uint64_t tableOffset = kPackHeaderBytes;
	const uint64_t dataStart = tableOffset + (uint64_t)kPackSlotCount * sizeof( PackSlot );

	flock( fd, LOCK_EX );

	struct stat st;
	memset( &st, 0, sizeof( st ) );
	PackHeader hd;
	bool valid = false;
	if ( fstat( fd, &st ) == 0 && (uint64_t)st.st_size >= dataStart && PReadFull( fd, &hd, sizeof( hd ), 0 ) )
	{
		valid = memcmp( hd.magic, kPackMagic, 8 ) == 0 && hd.version == kPackVersion && hd.slotBits == kPackSlotBits
			 && hd.tableOffset == tableOffset && hd.dataStart == dataStart;
	}

	if ( !valid )
	{
		if ( st.st_size > 0 )
			Msg( "ASTC: cache file has an unknown layout, starting a fresh one\n" );

		memset( &hd, 0, sizeof( hd ) );
		memcpy( hd.magic, kPackMagic, 8 );
		hd.version = kPackVersion;
		hd.slotBits = kPackSlotBits;
		hd.tableOffset = tableOffset;
		hd.dataStart = dataStart;
		if ( ftruncate( fd, 0 ) != 0 || !PWriteFull( fd, &hd, sizeof( hd ), 0 ) || ftruncate( fd, (off_t)dataStart ) != 0 )
		{
			Warning( "ASTC: cannot initialize cache file %s (errno %d); the disk cache is off for this session\n", path, errno );
			flock( fd, LOCK_UN );
			close( fd );
			return false;
		}
	}

	flock( fd, LOCK_UN );

	m_slots.assign( kPackSlotCount, PackSlot() );
	memset( &m_slots[0], 0, (size_t)kPackSlotCount * sizeof( PackSlot ) );
	m_hot.assign( kPackSlotCount, 0 );

	// load the table in 1 MB pieces; a short file just leaves the rest empty
	const size_t total = (size_t)kPackSlotCount * sizeof( PackSlot );
	uint8_t* tbl = (uint8_t*)&m_slots[0];
	size_t done = 0;
	while ( done < total )
	{
		const size_t chunk = total - done > ( 1u << 20 ) ? ( 1u << 20 ) : total - done;
		if ( !PReadFull( fd, tbl + done, chunk, tableOffset + done ) )
			break;
		done += chunk;
	}

	uint32_t count = 0;
	for ( uint32_t i = 0; i < kPackSlotCount; ++i )
	{
		if ( m_slots[i].key )
			++count;
	}
	m_count = count;

	struct stat st2;
	m_syncedSize = ( fstat( fd, &st2 ) == 0 ) ? (uint64_t)st2.st_size : dataStart;

	long mb = gl_astc_cache_max_mb.GetInt();
	if ( mb < 64 )
		mb = 64;
	m_maxBytes = (uint64_t)mb * 1024ull * 1024ull;

	m_fd = fd;
	m_state.store( 1 );

	Msg( "ASTC: cache file %s ready: %u entries, %.1f MB (limit %ld MB)\n",
		 path, count, (double)m_syncedSize / ( 1024.0 * 1024.0 ), mb );

	StartOldCacheCleanup( dir );
	return true;
}

bool AstcPack::FindMem( uint64_t key, PackSlot* out, uint32_t* outIdx )
{
	const uint32_t mask = kPackSlotCount - 1;
	uint32_t i = PackHome( key );
	for ( uint32_t step = 0; step < kPackSlotCount; ++step, i = ( i + 1 ) & mask )
	{
		const PackSlot& s = m_slots[i];
		if ( s.key == key )
		{
			*out = s;
			if ( outIdx )
				*outIdx = i;
			return true;
		}
		if ( s.key == 0 )
			return false;
	}
	return false;
}

// Re-reads the whole slot table: another copy of this code (or another process) may have appended entries.
bool AstcPack::ReloadTable()
{
	std::lock_guard<std::mutex> io( m_ioMtx );
	std::lock_guard<std::mutex> lk( m_mtx );

	struct stat st;
	if ( fstat( m_fd, &st ) != 0 )
		return false;

	const size_t total = (size_t)kPackSlotCount * sizeof( PackSlot );
	uint8_t* tbl = (uint8_t*)&m_slots[0];
	size_t done = 0;
	while ( done < total )
	{
		const size_t chunk = total - done > ( 1u << 20 ) ? ( 1u << 20 ) : total - done;
		if ( !PReadFull( m_fd, tbl + done, chunk, kPackHeaderBytes + done ) )
		{
			memset( tbl + done, 0, total - done );
			break;
		}
		done += chunk;
	}

	uint32_t count = 0;
	for ( uint32_t i = 0; i < kPackSlotCount; ++i )
	{
		if ( m_slots[i].key )
			++count;
	}
	m_count = count;
	m_syncedSize = (uint64_t)st.st_size;
	return true;
}

bool AstcPack::Lookup( uint64_t key, uint8_t* dst, size_t size )
{
	if ( !key )
		key = 1;

	// Tier 1: RAM
	if ( s_ram.Get( key, dst, size ) )
	{
		s_stats.ramHits.fetch_add( 1 );
		return true;
	}

	if ( !Ready() )
		return false;

	PackSlot slot;
	uint32_t idx = 0;
	bool found;
	{
		std::lock_guard<std::mutex> lk( m_mtx );
		found = FindMem( key, &slot, &idx );
	}

	if ( !found )
	{
		// Maybe another copy appended entries (or reset the file) since our table was loaded.
		struct stat st;
		if ( fstat( m_fd, &st ) == 0 )
		{
			bool changed;
			{
				std::lock_guard<std::mutex> lk( m_mtx );
				changed = (uint64_t)st.st_size != m_syncedSize;
			}
			const int64_t now = NowMs();
			if ( changed && now - m_lastReloadMs.load() > 1000 )
			{
				m_lastReloadMs.store( now );
				if ( ReloadTable() )
				{
					std::lock_guard<std::mutex> lk( m_mtx );
					found = FindMem( key, &slot, &idx );
				}
			}
		}
	}

	if ( !found || slot.size != size )
		return false;

	PackRec rec;
	const uint64_t off = (uint64_t)slot.off16 * 16ull;
	if ( !PReadFull( m_fd, &rec, sizeof( rec ), off ) )
		return false;
	if ( rec.magic != kRecMagic || rec.key != key || rec.size != size )
		return false;
	if ( !PReadFull( m_fd, dst, size, off + sizeof( rec ) ) )
		return false;
	if ( Hash64( dst, size, kPayloadSeed ) != rec.hash )
		return false;

	s_stats.diskHits.fetch_add( 1 );

	// Tier 2 -> 1: promote what keeps getting reused (and anything tiny) into RAM.
	uint8_t hot;
	{
		std::lock_guard<std::mutex> lk( m_mtx );
		if ( m_hot[idx] < 255 )
			++m_hot[idx];
		hot = m_hot[idx];
	}
	if ( size <= kRamTinyBytes || hot >= 2 )
		s_ram.Put( key, dst, size, RamBudgetBytes() );

	return true;
}

// Appends one record and its table slot to the file. Caller holds m_ioMtx and the flock.
AstcPack::AppendResult AstcPack::AppendLocked( uint64_t key, const uint8_t* data, size_t size,
											   PackSlot* outSlot, uint32_t* outIdx, uint64_t* outEnd, uint64_t* outEndBefore )
{
	struct stat st;
	if ( fstat( m_fd, &st ) != 0 )
		return kFailed;

	const uint64_t endBefore = (uint64_t)st.st_size;
	*outEndBefore = endBefore;
	const uint64_t recOff = ( endBefore + 15ull ) & ~15ull;

	if ( recOff + sizeof( PackRec ) + size > m_maxBytes || recOff / 16ull > 0xFFFFFFF0ull )
		return kFull;

	// find the slot in the FILE's table: another copy may have filled slots we don't know about yet
	const uint32_t mask = kPackSlotCount - 1;
	uint32_t idx = PackHome( key );
	bool haveSlot = false;
	for ( uint32_t step = 0; step < 4096; ++step, idx = ( idx + 1 ) & mask )
	{
		PackSlot s;
		if ( !PReadFull( m_fd, &s, sizeof( s ), kPackHeaderBytes + (uint64_t)idx * sizeof( PackSlot ) ) )
			return kFailed;
		if ( s.key == key )
		{
			*outSlot = s;
			*outIdx = idx;
			return kAlready;
		}
		if ( s.key == 0 )
		{
			haveSlot = true;
			break;
		}
	}
	if ( !haveSlot )
		return kFull;

	PackRec rec;
	rec.magic = kRecMagic;
	rec.size = (uint32_t)size;
	rec.key = key;
	rec.hash = Hash64( data, size, kPayloadSeed );

	if ( !PWriteFull( m_fd, &rec, sizeof( rec ), recOff ) || !PWriteFull( m_fd, data, size, recOff + sizeof( rec ) ) )
		return kFailed;

	PackSlot slot;
	slot.key = key;
	slot.off16 = (uint32_t)( recOff / 16ull );
	slot.size = (uint32_t)size;
	// the slot goes in last: a record is never reachable before its bytes are in the file
	if ( !PWriteFull( m_fd, &slot, sizeof( slot ), kPackHeaderBytes + (uint64_t)idx * sizeof( PackSlot ) ) )
		return kFailed;

	*outSlot = slot;
	*outIdx = idx;
	*outEnd = recOff + sizeof( rec ) + size;
	return kAppended;
}

// Wipes the file back to an empty table. Caller holds m_ioMtx and the flock.
bool AstcPack::ResetLocked()
{
	const uint64_t dataStart = kPackHeaderBytes + (uint64_t)kPackSlotCount * sizeof( PackSlot );
	PackHeader hd;
	memset( &hd, 0, sizeof( hd ) );
	memcpy( hd.magic, kPackMagic, 8 );
	hd.version = kPackVersion;
	hd.slotBits = kPackSlotBits;
	hd.tableOffset = kPackHeaderBytes;
	hd.dataStart = dataStart;
	if ( ftruncate( m_fd, 0 ) != 0 || !PWriteFull( m_fd, &hd, sizeof( hd ), 0 ) || ftruncate( m_fd, (off_t)dataStart ) != 0 )
		return false;

	std::lock_guard<std::mutex> lk( m_mtx );
	memset( &m_slots[0], 0, (size_t)kPackSlotCount * sizeof( PackSlot ) );
	memset( &m_hot[0], 0, m_hot.size() );
	m_count = 0;
	m_syncedSize = dataStart;
	++m_resets;
	return true;
}

void AstcPack::Store( uint64_t key, const uint8_t* data, size_t size )
{
	if ( !key )
		key = 1;
	if ( !data || size == 0 || size > 0x7FFFFFF0u )
		return;

	// A freshly encoded level that is tiny is cheap to keep hot; bigger ones earn RAM when they get reused.
	if ( size <= kRamTinyBytes )
		s_ram.Put( key, data, size, RamBudgetBytes() );

	if ( !Ready() )
		return;

	std::lock_guard<std::mutex> io( m_ioMtx );
	{
		std::lock_guard<std::mutex> lk( m_mtx );
		PackSlot tmp;
		if ( FindMem( key, &tmp, NULL ) )
			return;
	}

	flock( m_fd, LOCK_EX );

	PackSlot slot;
	uint32_t idx = 0;
	uint64_t endAfter = 0, endBefore = 0;
	AppendResult r = AppendLocked( key, data, size, &slot, &idx, &endAfter, &endBefore );

	bool wasReset = false;
	if ( r == kFull )
	{
		// The file reached its size limit (or the table is full): start over, and carry the hot
		// (essential) entries from RAM across so they are not lost.
		Msg( "ASTC: cache file reached its limit (%.0f MB, %u entries); starting a fresh one and keeping the hot entries\n",
			 (double)m_maxBytes / ( 1024.0 * 1024.0 ), m_count );
		if ( ResetLocked() )
		{
			wasReset = true;
			const size_t keep = (size_t)( m_maxBytes / 8 );
			s_ram.ForEachHot( keep < ( 256u << 20 ) ? keep : ( 256u << 20 ),
				[&]( uint64_t k, const uint8_t* d, size_t n )
				{
					PackSlot s2;
					uint32_t i2 = 0;
					uint64_t e2 = 0, eb2 = 0;
					if ( AppendLocked( k, d, n, &s2, &i2, &e2, &eb2 ) == kAppended )
					{
						std::lock_guard<std::mutex> lk( m_mtx );
						m_slots[i2] = s2;
						++m_count;
					}
				} );
			endBefore = 0;
			r = AppendLocked( key, data, size, &slot, &idx, &endAfter, &endBefore );
		}
		else
		{
			r = kFailed;
		}
	}

	flock( m_fd, LOCK_UN );

	if ( r == kAppended || r == kAlready )
	{
		std::lock_guard<std::mutex> lk( m_mtx );
		if ( m_slots[idx].key != slot.key )
		{
			m_slots[idx] = slot;
			++m_count;
		}
		if ( r == kAppended )
		{
			if ( wasReset )
			{
				struct stat st;
				if ( fstat( m_fd, &st ) == 0 )
					m_syncedSize = (uint64_t)st.st_size;
			}
			else if ( endBefore == m_syncedSize )
			{
				m_syncedSize = endAfter;
			}
			s_stats.stores.fetch_add( 1 );
			s_stats.storedBytes.fetch_add( size );
		}
	}
	else if ( r == kFailed )
	{
		static std::atomic<int> s_warned( 0 );
		if ( s_warned.fetch_add( 1 ) < 3 )
			Warning( "ASTC: writing to the cache file failed (errno %d)\n", errno );
	}
}

static AstcPack* GetPack()
{
	static AstcPack* s_pack = new AstcPack();
	return s_pack;
}

#else // !ASTC_PACK_SUPPORTED

class AstcPack
{
public:
	bool Ready() { return false; }
	bool Lookup( uint64_t key, uint8_t* dst, size_t size )
	{
		return s_ram.Get( key ? key : 1, dst, size );
	}
	void Store( uint64_t key, const uint8_t* data, size_t size )
	{
		if ( size <= kRamTinyBytes )
			s_ram.Put( key ? key : 1, data, size, RamBudgetBytes() );
	}
	uint64_t SizeBytes() { return 0; }
};

static AstcPack* GetPack()
{
	static AstcPack* s_pack = new AstcPack();
	return s_pack;
}

#endif // ASTC_PACK_SUPPORTED

static bool CacheLoad( uint64_t key, uint8_t* dst, size_t dstSize )
{
	const bool hit = GetPack()->Lookup( key, dst, dstSize );
	if ( hit )
		s_stats.hits.fetch_add( 1 );
	else
		s_stats.misses.fetch_add( 1 );
	return hit;
}

static void CacheStore( uint64_t key, const uint8_t* data, size_t size )
{
	GetPack()->Store( key, data, size );
}

static void ReportStats( bool force )
{
	if ( !force && !gl_astc_stats.GetBool() )
		return;

	static std::atomic<int64_t> s_last( 0 );
	const int64_t now = NowMs();
	int64_t last = s_last.load();
	if ( last == 0 )
	{
		s_last.store( now );
		if ( !force )
			return;
		last = now;
	}
	if ( !force && now - last < 20000 )
		return;
	if ( !force && !s_last.compare_exchange_strong( last, now ) )
		return;

	const double sec = (double)s_stats.encMicros.load() / 1.0e6;
	const double mp = (double)s_stats.encPixels.load() / 1.0e6;
	Msg( "ASTC stats: %llu levels encoded (%.1f MP) in %.1f s = %.2f MP/s on %u thread(s) | cache: %llu hits (%llu RAM, %llu file), %llu misses, %llu stored (%.1f MB), RAM tier %.1f MB\n",
		 (unsigned long long)s_stats.encImages.load(), mp, sec, sec > 0.0 ? mp / sec : 0.0, (unsigned)s_stats.poolThreads.load(),
		 (unsigned long long)s_stats.hits.load(), (unsigned long long)s_stats.ramHits.load(), (unsigned long long)s_stats.diskHits.load(),
		 (unsigned long long)s_stats.misses.load(), (unsigned long long)s_stats.stores.load(),
		 (double)s_stats.storedBytes.load() / ( 1024.0 * 1024.0 ), (double)s_ram.Bytes() / ( 1024.0 * 1024.0 ) );
}

#endif // HAVE_ASTCENC

// ---------------------------------------------------------------------------
// Main entry point
// ---------------------------------------------------------------------------
bool ASTC_CompressTexture(
		const void* srcData,
		int width,
		int height,
		unsigned int srcGLFormat,
		unsigned int srcGLType,
		bool isHDR,
		bool isSRGB,
		bool forceOpaque,
		int blockW,
		int blockH,
		int qualityPreset,
		ASTCEncodeResult* outResult,
		bool allowDiskCache )
{
	if ( outResult )
		memset( outResult, 0, sizeof( *outResult ) );

#if !defined( HAVE_ASTCENC )
	(void)srcData; (void)width; (void)height; (void)srcGLFormat; (void)srcGLType;
	(void)isHDR; (void)isSRGB; (void)forceOpaque; (void)blockW; (void)blockH; (void)qualityPreset;
	(void)allowDiskCache;
	return false;
#else
	if ( !srcData || width <= 0 || height <= 0 || !outResult || !ASTC_IsValidBlockSize( blockW, blockH ) )
		return false;

	const size_t nPixels = (size_t)width * (size_t)height;
	const size_t xBlocks = ( (size_t)width  + blockW - 1 ) / blockW;
	const size_t yBlocks = ( (size_t)height + blockH - 1 ) / blockH;
	const size_t compSize = xBlocks * yBlocks * 16;	// every ASTC block is 16 bytes

	const astcenc_profile profile = isHDR ? ASTCENC_PRF_HDR : ( isSRGB ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR );
	const float quality = QualityFromPreset( qualityPreset );

	const uint32_t flags = ComputeEncodeFlags( isHDR, forceOpaque );

	uint8_t* compData = (uint8_t*)malloc( compSize );
	if ( !compData )
		return false;

	// Cache lookup (RAM tier, then the single cache file). Keyed on the exact source bytes plus every
	// parameter that changes the output. Dynamic textures (allowDiskCache == false) skip the cache entirely.
	const bool useCache = allowDiskCache && gl_astc_cache.GetBool();
	uint64_t key = 0;
	if ( useCache )
	{
		const uint32_t params[12] = {
			kCacheVersion, srcGLFormat, srcGLType, (uint32_t)width, (uint32_t)height,
			isHDR, isSRGB, forceOpaque, (uint32_t)blockW, (uint32_t)blockH,
			(uint32_t)qualityPreset, flags
		};
		key = ComputeCacheKey( srcData, nPixels * SrcTexelBytes( srcGLFormat, srcGLType ), params );

		if ( CacheLoad( key, compData, compSize ) )
		{
			outResult->m_pData = compData;
			outResult->m_nDataSize = (uint32_t)compSize;
			outResult->m_glInternalFormat = GLInternalFormatForBlock( blockW, blockH, isSRGB && !isHDR );
			outResult->m_blockW = blockW;
			outResult->m_blockH = blockH;
			outResult->m_profile = isHDR ? kASTCProfile_HDR : kASTCProfile_LDR;
			ReportStats( false );
			return true;
		}
	}

	// Cache miss: prepare the source and encode.
	SourcePlan plan;
	if ( !PrepareSource( srcData, nPixels, srcGLFormat, srcGLType, isHDR, forceOpaque, &plan ) )
	{
		Warning( "ASTC: unsupported source layout (GL format 0x%X, type 0x%X)\n", srcGLFormat, srcGLType );
		free( compData );
		return false;
	}

	void* sliceArray[1] = { const_cast<void*>( plan.ptr ) };
	astcenc_image image;
	memset( &image, 0, sizeof( image ) );
	image.dim_x = width;
	image.dim_y = height;
	image.dim_z = 1;
	image.data_type = plan.isFloat ? ASTCENC_TYPE_F32 : ASTCENC_TYPE_U8;
	image.data = sliceArray;

	astcenc_error status = EncodeImage( profile, blockW, blockH, quality, flags, image, plan.swz,
										compData, compSize, xBlocks * yBlocks );

	free( plan.owned );

	if ( status != ASTCENC_SUCCESS )
	{
		Warning( "ASTC: astcenc_compress_image failed: %s\n", astcenc_get_error_string( status ) );
		free( compData );
		return false;
	}

	if ( useCache )
		CacheStore( key, compData, compSize );

	ReportStats( false );

	outResult->m_pData = compData;
	outResult->m_nDataSize = (uint32_t)compSize;
	outResult->m_glInternalFormat = GLInternalFormatForBlock( blockW, blockH, isSRGB && !isHDR );
	outResult->m_blockW = blockW;
	outResult->m_blockH = blockH;
	outResult->m_profile = isHDR ? kASTCProfile_HDR : kASTCProfile_LDR;
	return true;
#endif
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
// Mandatory compression entry point (no uncompressed fallback)
// ---------------------------------------------------------------------------
void ASTC_CompressTextureRequired(
		bool isHDR,
		bool isSRGB,
		bool forceOpaque,
		const void* srcData,
		int width,
		int height,
		unsigned int srcGLFormat,
		unsigned int srcGLType,
		ASTCEncodeResult* outResult,
		bool allowDiskCache,
		bool* outFallback )
{
	if ( outResult )
		memset( outResult, 0, sizeof( *outResult ) );
	if ( outFallback )
		*outFallback = false;

	int blockW, blockH;
	ASTC_GetConfiguredBlockSize( isHDR, &blockW, &blockH );

	if ( ASTC_CompressTexture( srcData, width, height, srcGLFormat, srcGLType,
							   isHDR, isSRGB, forceOpaque, blockW, blockH, gl_astc_quality.GetInt(),
							   outResult, allowDiskCache ) )
		return;

	// v3: this used to be Error(), which killed the whole game on the first texture the encoder
	// could not handle. The texture must still be ASTC (never uncompressed), so hand back valid
	// solid-colour ASTC blocks of the right size/format and keep running; the warning tells us
	// which texture was affected.
	Warning( "ASTC_CompressTextureRequired: %s ASTC encode failed for a %dx%d texture (block %dx%d, "
			 "GL format 0x%X type 0x%X). Using a blank ASTC image for it. Is astcenc built in (HAVE_ASTCENC) and linked?\n",
			 isHDR ? "HDR" : "LDR", width, height, blockW, blockH, srcGLFormat, srcGLType );

	if ( outFallback )
		*outFallback = true;

	if ( !ASTC_MakeBlankTexture( isHDR, isSRGB, forceOpaque, width, height, outResult ) )
	{
		Error( "ASTC_CompressTextureRequired: out of memory creating a %dx%d ASTC image\n", width, height );
	}
}

// ---------------------------------------------------------------------------
// Solid-color ASTC image (void-extent blocks). No encoder needed.
//
// 2D void-extent block: bits[8:0] = 0x1FC, bit 9 = D (0 LDR, 1 HDR),
// bits[11:10] = 11, extent coords all ones, then R,G,B,A as 16-bit values
// (UNORM16 for LDR, IEEE half for HDR).
// ---------------------------------------------------------------------------
bool ASTC_MakeBlankTexture(
		bool isHDR,
		bool isSRGB,
		bool opaqueAlpha,
		int width,
		int height,
		ASTCEncodeResult* outResult )
{
	if ( !outResult )
		return false;
	memset( outResult, 0, sizeof( *outResult ) );

	if ( width <= 0 || height <= 0 )
		return false;

	int blockW, blockH;
	ASTC_GetConfiguredBlockSize( isHDR, &blockW, &blockH );

	const size_t xBlocks = ( (size_t)width  + blockW - 1 ) / blockW;
	const size_t yBlocks = ( (size_t)height + blockH - 1 ) / blockH;
	const size_t size = xBlocks * yBlocks * 16;

	uint8_t* data = (uint8_t*)malloc( size );
	if ( !data )
		return false;

	const uint16_t alphaBits = opaqueAlpha ? ( isHDR ? 0x3C00 : 0xFFFF ) : 0x0000;

	uint8_t block[16];
	block[0] = 0xFC;
	block[1] = isHDR ? 0xFF : 0xFD;
	memset( block + 2, 0xFF, 6 );
	memset( block + 8, 0x00, 6 );	// R = G = B = 0
	block[14] = (uint8_t)( alphaBits & 0xFF );
	block[15] = (uint8_t)( alphaBits >> 8 );

	for ( size_t i = 0; i < xBlocks * yBlocks; ++i )
		memcpy( data + i * 16, block, 16 );

	outResult->m_pData = data;
	outResult->m_nDataSize = (uint32_t)size;
	outResult->m_glInternalFormat = GLInternalFormatForBlock( blockW, blockH, isSRGB && !isHDR );
	outResult->m_blockW = blockW;
	outResult->m_blockH = blockH;
	outResult->m_profile = isHDR ? kASTCProfile_HDR : kASTCProfile_LDR;
	return true;
}


// ---------------------------------------------------------------------------
// Cache access for callers that identify their source more cheaply than decoded pixels
// ---------------------------------------------------------------------------
#if defined( HAVE_ASTCENC )

uint64_t ASTC_ExternalCacheKey( const void* srcBytes, size_t srcLen, unsigned int srcFormatId,
								int width, int height, bool isHDR, bool isSRGB, bool forceOpaque )
{
	int blockW, blockH;
	ASTC_GetConfiguredBlockSize( isHDR, &blockW, &blockH );

	const uint32_t flags = ComputeEncodeFlags( isHDR, forceOpaque );
	const uint32_t params[12] = {
		kCacheVersion, srcFormatId, 0xE57C0DE5u /* external-source marker */, (uint32_t)width, (uint32_t)height,
		isHDR, isSRGB, forceOpaque, (uint32_t)blockW, (uint32_t)blockH,
		(uint32_t)gl_astc_quality.GetInt(), flags
	};
	return ComputeCacheKey( srcBytes, srcLen, params );
}

bool ASTC_CacheLookup( uint64_t key, int width, int height, bool isHDR, bool isSRGB, ASTCEncodeResult* outResult )
{
	if ( !outResult )
		return false;
	memset( outResult, 0, sizeof( *outResult ) );

	if ( !gl_astc_cache.GetBool() || width <= 0 || height <= 0 )
		return false;

	int blockW, blockH;
	ASTC_GetConfiguredBlockSize( isHDR, &blockW, &blockH );

	const size_t xBlocks = ( (size_t)width  + blockW - 1 ) / blockW;
	const size_t yBlocks = ( (size_t)height + blockH - 1 ) / blockH;
	const size_t compSize = xBlocks * yBlocks * 16;

	uint8_t* compData = (uint8_t*)malloc( compSize );
	if ( !compData )
		return false;

	if ( !CacheLoad( key, compData, compSize ) )
	{
		free( compData );
		return false;
	}

	outResult->m_pData = compData;
	outResult->m_nDataSize = (uint32_t)compSize;
	outResult->m_glInternalFormat = GLInternalFormatForBlock( blockW, blockH, isSRGB && !isHDR );
	outResult->m_blockW = blockW;
	outResult->m_blockH = blockH;
	outResult->m_profile = isHDR ? kASTCProfile_HDR : kASTCProfile_LDR;
	ReportStats( false );
	return true;
}

void ASTC_CacheStore( uint64_t key, const ASTCEncodeResult* result )
{
	if ( !result || !result->m_pData || result->m_nDataSize == 0 || !gl_astc_cache.GetBool() )
		return;
	CacheStore( key, (const uint8_t*)result->m_pData, result->m_nDataSize );
}

// ---- textures whose CPU shadow copy must survive Unlock() ----
static std::mutex s_shadowMutex;
static std::unordered_set<const void*> s_shadowSet;

void ASTC_ShadowTexAdd( const void* tex )
{
	std::lock_guard<std::mutex> lk( s_shadowMutex );
	s_shadowSet.insert( tex );
}

bool ASTC_ShadowTexHas( const void* tex )
{
	std::lock_guard<std::mutex> lk( s_shadowMutex );
	return s_shadowSet.find( tex ) != s_shadowSet.end();
}

void ASTC_ShadowTexRemove( const void* tex )
{
	std::lock_guard<std::mutex> lk( s_shadowMutex );
	s_shadowSet.erase( tex );
}

#else // !HAVE_ASTCENC

uint64_t ASTC_ExternalCacheKey( const void*, size_t, unsigned int, int, int, bool, bool, bool ) { return 0; }
bool ASTC_CacheLookup( uint64_t, int, int, bool, bool, ASTCEncodeResult* outResult ) { if ( outResult ) memset( outResult, 0, sizeof( *outResult ) ); return false; }
void ASTC_CacheStore( uint64_t, const ASTCEncodeResult* ) {}
void ASTC_ShadowTexAdd( const void* ) {}
bool ASTC_ShadowTexHas( const void* ) { return false; }
void ASTC_ShadowTexRemove( const void* ) {}

#endif // HAVE_ASTCENC
