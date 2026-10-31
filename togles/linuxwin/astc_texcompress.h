//========= ASTC runtime recompression (added on top of Valve TOGL) - v6 =======//
//
// astc_texcompress.h
//
// Every ARGB/RGBA-family texture (and every DXT1/3/5 texture, decoded to RGBA
// first) is re-encoded to ASTC at upload time. Encoding is done by ARM's
// astcenc (Apache-2.0, https://github.com/ARM-software/astc-encoder).
//
// What this module does today (the version notes below say when each part came in):
//   * One persistent encoder thread pool (every CPU core, gl_astc_threads 0). No std::thread is spawned per texture.
//   * astcenc contexts are built once per (profile, block, quality, flags, mode limit) and shared by every upload,
//     so the expensive lookup tables are never rebuilt per texture.
//   * Encoded blocks are cached: always in a dynamic RAM tier (gl_astc_cache), and, only when gl_astc_disk_cache is 1,
//     also in the single file astc_cache/astc_cache.bin (a hash table plus an append-only data area, opened once and
//     read with pread()). Identical source + parameters are never encoded twice.
//   * DXT sources are cached by their compressed bytes, so a cache hit skips the DXT decode as well as the encode.
//   * DXT levels are already lossy, so they may use a cheaper encoder search (gl_astc_dxt_mode_limit).
//   * Zero-copy fast path for BGRA/RGBA 8-bit sources. Channel order and forced alpha are handled with astcenc
//     swizzles, not by a CPU copy.
//   * Perceptual error metrics for LDR (gl_astc_perceptual). Alpha is never used to weight the color error.
//   * ASTC_MakeBlankTexture() is exported so the GL layer can allocate ASTC storage without data (placeholder /
//     ResetSRGB) instead of falling back to an uncompressed glTexImage2D, which would break later compressed
//     writes to the same texture.
//   * Textures that receive partial updates keep their CPU shadow copy (see ASTC_ShadowTex*) and only the touched
//     ASTC blocks are re-encoded.
//   * Periodic throughput / cache statistics in the engine log (gl_astc_stats); glGetError checks after uploads
//     (gl_astc_debug).
//
// v6 changes (speed):
//   * gl_astc_dxt_mode_limit: levels decoded from DXT use a smaller share of the ASTC block modes (default 50 %),
//     about 1.5-1.8x faster for roughly 0.15-0.2 dB lower PSNR. Not used for 4x4 / 5x4 blocks or non-DXT sources.
//   * gl_astc_quality is passed to astcenc as is (0..100) instead of snapping to five presets.
//   * Whole-level DXT decoders (decompress.c); the cache file is read with one system call for small levels.
//   * Fixes: tiny DXT mips (2x2, 4x1, ...) were mistaken for raw pixels; the RAM tier now really shrinks when the
//     device runs low on memory; a failed encoder context is not rebuilt (and logged) for every texture.
//
// v5: persistent RAM tier + optional single-file disk tier replace thousands of small .astc files.
// v3/v4: black screen / crash fixes: dynamic (PBO-backed) textures are never encoded from an unmapped PBO pointer,
//   ASTC uploads always happen with GL_PIXEL_UNPACK_BUFFER unbound, storage-only levels use ASTC_MakeBlankTexture(),
//   an encode failure is no longer fatal (blank ASTC level + warning).
//
// ASTC recompression is MANDATORY: ASTC_CompressTextureRequired() never lets an
// eligible texture reach the GPU uncompressed. If astcenc isn't built in
// (HAVE_ASTCENC) or encoding fails it hands back a blank ASTC image. The only textures that
// stay uncompressed are render targets (incl. MSAA) and formats outside
// ASTC_IsEligibleFormat().
//
//===============================================================================

#ifndef ASTC_TEXCOMPRESS_H
#define ASTC_TEXCOMPRESS_H

#pragma once

#include <stdint.h>
#include <stddef.h>
#include "tier1/convar.h"

// GL enums for ASTC (KHR_texture_compression_astc_ldr / _hdr). Guarded in case
// the local GL headers already provide them.
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

enum EASTCProfile
{
	kASTCProfile_LDR = 0,	// clamped [0,1], UNORM decode
	kASTCProfile_HDR = 1,	// unclamped, FP16 decode (GL_KHR_texture_compression_astc_hdr)
};

struct ASTCEncodeResult
{
	void*		m_pData;			// free with ASTC_FreeResult()
	uint32_t	m_nDataSize;		// bytes in m_pData
	uint32_t	m_glInternalFormat;	// pass to glCompressedTexImage2D
	int			m_blockW;
	int			m_blockH;
	EASTCProfile m_profile;
};

// d3dFormat is an int so this header doesn't need d3d9types.h.
bool ASTC_IsEligibleFormat( int d3dFormat );
bool ASTC_IsHDRFormat( int d3dFormat );
bool ASTC_FormatHasAlpha( int d3dFormat );

bool ASTC_IsValidBlockSize( int w, int h );

// Encodes an image. Returns false (outResult zeroed) if astcenc isn't built in,
// the source layout is unknown, or encoding failed. Prefer
// ASTC_CompressTextureRequired(), which falls back to a blank ASTC image instead of
// ever handing back uncompressed data.
// allowDiskCache = false skips the cache (RAM and file) for this call (used for dynamic textures,
// whose contents change constantly and would only litter the cache).
// dxtSource = true says the pixels were decoded from a DXT level (already lossy): the encoder may use the
// cheaper search of gl_astc_dxt_mode_limit. It never changes the result for other sources.
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
	bool allowDiskCache = true,
	bool dxtSource = false );

// Mandatory entry point used by every upload path. Uses the configured block
// size (gl_astc_block_ldr / gl_astc_block_hdr). Never returns an uncompressed
// result. If encoding fails it warns (engine log) and returns a blank ASTC image of
// the right size and format, so the texture stays ASTC and the game keeps running.
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
	bool allowDiskCache = true,
	bool* outFallback = 0,			// set to true if the encode failed and a blank image was returned
	bool dxtSource = false );		// the pixels come from a DXT level, see ASTC_CompressTexture()

// ---- Cache access for callers that can identify their source more cheaply than decoded pixels ----
// (DXT: the compressed blocks are 4-8x smaller than the decoded RGBA, and a cache hit skips the decode.)
// The key covers the source bytes, dimensions, the caller's format id and every encoder setting. Entries stored
// under such a key are encoded with the DXT policy (gl_astc_dxt_mode_limit), so use these only for DXT sources.
uint64_t ASTC_ExternalCacheKey(
	const void* srcBytes,
	size_t srcLen,
	unsigned int srcFormatId,
	int width,
	int height,
	bool isHDR,
	bool isSRGB,
	bool forceOpaque );

// On a hit fills outResult with a malloc'd ASTC image (free with ASTC_FreeResult) and returns true.
bool ASTC_CacheLookup( uint64_t key, int width, int height, bool isHDR, bool isSRGB, ASTCEncodeResult* outResult );

// Stores a successfully encoded image under 'key'.
void ASTC_CacheStore( uint64_t key, const ASTCEncodeResult* result );

// ---- Textures that need their CPU shadow copy kept after Unlock() ----
// (partial updates are merged into the shadow copy and re-encoded; see CGLMTex::Unlock)
void ASTC_ShadowTexAdd( const void* tex );
bool ASTC_ShadowTexHas( const void* tex );
void ASTC_ShadowTexRemove( const void* tex );

// Valid ASTC blocks that encode a solid color (opaque black, or transparent
// black if !opaqueAlpha). No encoder is needed. Use this for placeholder and
// no-data storage, so the texture stays compressed.
bool ASTC_MakeBlankTexture(
	bool isHDR,
	bool isSRGB,
	bool opaqueAlpha,
	int width,
	int height,
	ASTCEncodeResult* outResult );

void ASTC_FreeResult( ASTCEncodeResult* result );

void ASTC_GetConfiguredBlockSize( bool isHDR, int* outW, int* outH );

// Convars (defined in astc_texcompress.cpp).
extern ConVar gl_astc_recompress;	// legacy, read for config compatibility only
extern ConVar gl_astc_block_ldr;	// "4x4".."12x12"; default 6x6
extern ConVar gl_astc_block_hdr;	// default 4x4
extern ConVar gl_astc_quality;		// 0..100, passed to astcenc as is (0 fastest, 60 medium = default, 100 exhaustive)
extern ConVar gl_astc_dxt_mode_limit;	// 1..99 = cap on the share of ASTC block modes tried for DXT-sourced levels (default 50); 0 / 100 = off
extern ConVar gl_astc_threads;		// encoder worker threads; 0 = auto = every CPU core
extern ConVar gl_astc_cache;		// 1 = keep encoded blocks in the RAM cache
extern ConVar gl_astc_disk_cache;	// 1 = also keep them in astc_cache/astc_cache.bin (off by default)
extern ConVar gl_astc_ram_cache_mb;	// RAM cache size: 0 = dynamic, -1 = off, >0 = fixed MB
extern ConVar gl_astc_cache_dir;	// cache directory, relative to the game's working dir
extern ConVar gl_astc_cache_max_mb;	// size limit of astc_cache/astc_cache.bin
extern ConVar gl_astc_cache_cleanup_old;	// 1 = delete old per-texture cache files (and astc_cache.bin while the disk tier is off) at startup
extern ConVar gl_astc_alpha_weight;	// legacy, ignored
extern ConVar gl_astc_perceptual;	// 1 = perceptual error metric for LDR profiles
extern ConVar gl_astc_stats;		// 1 = periodic encode throughput + cache statistics in the engine log
extern ConVar gl_astc_debug;		// 0 = off, 1 = glGetError after the first ASTC uploads (failures logged), 2 = every upload, 3 = also log what each level is encoded from

#endif // ASTC_TEXCOMPRESS_H
