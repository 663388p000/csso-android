//========= Mali (ARM GPU) optimizations for TOGL ================================//
//
// mali_opt.cpp
//
// See mali_opt.h. Contents:
//   1. Mali detection
//   2. dynamic loading of the few GL entry points that are not in TOGL's function table
//   3. glInvalidateFramebuffer wrapper (skip tile write-back of dead depth/stencil)
//   4. occlusion query target selection
//   5. program binary cache (single append-only file, in-memory open-addressing index)
//
// NOTE: no std::unordered_map / std::mutex here on purpose. The NDK r10e libc++ headers used by this project
// do not compile some of them (see the note in astc_texcompress.cpp); plain C structures and pthreads are used.
//
//===============================================================================

#include "togles/rendermechanism.h"

#include "mali_opt.h"

#include "tier0/dbg.h"
#include "tier0/icommandline.h"
#include "tier1/convar.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined( __ANDROID__ ) || defined( __linux__ )
	#define MALIOPT_POSIX 1
	#include <dlfcn.h>
	#include <fcntl.h>
	#include <unistd.h>
	#include <errno.h>
	#include <time.h>
	#include <pthread.h>
	#include <sys/stat.h>
	#include <sys/types.h>
#else
	#define MALIOPT_POSIX 0
#endif

#if MALIOPT_POSIX && !defined( O_CLOEXEC )
	#define O_CLOEXEC 0
#endif

//-----------------------------------------------------------------------------
// Convars
//-----------------------------------------------------------------------------

ConVar gl_mali_opt( "gl_mali_opt", "1", FCVAR_ARCHIVE,
	"Master switch for the Mali (ARM GPU) specific optimizations. 0 = off. Read at first use; -gl_no_mali_opt on the command line is more reliable." );
ConVar gl_mali_discard_depth( "gl_mali_discard_depth", "1", FCVAR_ARCHIVE,
	"Mali: invalidate the back buffer's depth/stencil right before Present so the GPU does not write them back to memory. Set to 0 if the next frame ever shows depth artifacts." );
ConVar gl_mali_conservative_oq( "gl_mali_conservative_oq", "1", FCVAR_ARCHIVE,
	"Mali: use GL_ANY_SAMPLES_PASSED_CONSERVATIVE for occlusion queries. Read once, at the first query." );
ConVar gl_mali_progcache( "gl_mali_progcache", "1", FCVAR_ARCHIVE,
	"Cache linked shader programs (glGetProgramBinary) in gl_mali_progcache_dir so later launches skip the driver's shader compiles. Read at first use; -gl_no_progcache on the command line is more reliable." );
ConVar gl_mali_progcache_dir( "gl_mali_progcache_dir", "shader_cache", FCVAR_ARCHIVE,
	"Directory of the program binary cache file (progcache.bin), relative to the game's working directory. Safe to delete at any time." );
ConVar gl_mali_progcache_max_mb( "gl_mali_progcache_max_mb", "256", FCVAR_ARCHIVE,
	"Size limit of progcache.bin in MB. When it is full nothing more is added (delete the file to start over)." );

//-----------------------------------------------------------------------------
// GL constants (declared locally so this file does not depend on which GL headers are in use)
//-----------------------------------------------------------------------------

#define MALIGL_ANY_SAMPLES_PASSED				0x8C2F
#define MALIGL_ANY_SAMPLES_PASSED_CONSERVATIVE	0x8D6A
#define MALIGL_LINK_STATUS						0x8B82
#define MALIGL_PROGRAM_BINARY_RETRIEVABLE_HINT	0x8257
#define MALIGL_PROGRAM_BINARY_LENGTH			0x8741
#define MALIGL_NUM_PROGRAM_BINARY_FORMATS		0x87FE
#define MALIGL_PROGRAM_BINARY_FORMATS			0x87FF
#define MALIGL_TRUE								1

typedef void		( *MaliPFN_GetProgramBinary )( unsigned int program, int bufSize, int *length, unsigned int *binaryFormat, void *binary );
typedef void		( *MaliPFN_ProgramBinary )( unsigned int program, unsigned int binaryFormat, const void *binary, int length );
typedef void		( *MaliPFN_ProgramParameteri )( unsigned int program, unsigned int pname, int value );
typedef void		( *MaliPFN_GetProgramiv )( unsigned int program, unsigned int pname, int *params );
typedef void		( *MaliPFN_GetIntegerv )( unsigned int pname, int *params );
typedef unsigned	( *MaliPFN_GetError )( void );
typedef void		( *MaliPFN_InvalidateFramebuffer )( unsigned int target, int numAttachments, const unsigned int *attachments );

// dlsym( RTLD_DEFAULT ) first, eglGetProcAddress as fallback: the same order cglmtex.cpp uses for glCompressedTexSubImage2D.
#if MALIOPT_POSIX
	#if defined( RTLD_DEFAULT )
		#define MALIOPT_RTLD_DEFAULT	RTLD_DEFAULT
	#else
		#define MALIOPT_RTLD_DEFAULT	( (void *)0 )
	#endif
#endif

static void *MaliOpt_GetProc( const char *name )
{
#if MALIOPT_POSIX
	void *p = dlsym( MALIOPT_RTLD_DEFAULT, name );
	if ( !p )
	{
		typedef void *( *PFN_GetProcAddress )( const char * );
		PFN_GetProcAddress pfnGetProc = (PFN_GetProcAddress)dlsym( MALIOPT_RTLD_DEFAULT, "eglGetProcAddress" );
		if ( pfnGetProc )
			p = pfnGetProc( name );
	}
	return p;
#else
	(void)name;
	return NULL;
#endif
}

//-----------------------------------------------------------------------------
// 1. Detection
//-----------------------------------------------------------------------------

bool MaliOpt_IsMali()
{
	static int s_nState = -1;			// -1 = not decided yet
	if ( s_nState < 0 )
	{
		if ( !gGL )
			return false;				// too early, ask again later

		s_nState = 0;
		if ( gGL->m_nDriverProvider == cGLDriverProviderARM && gl_mali_opt.GetBool() && !CommandLine()->FindParm( "-gl_no_mali_opt" ) )
			s_nState = 1;

		Msg( "Mali optimizations: %s (GL_VENDOR=\"%s\")\n", s_nState ? "ENABLED" : "disabled", gGL->m_pGLDriverStrings[cGLVendorString] );
	}
	return s_nState > 0;
}

//-----------------------------------------------------------------------------
// 3. glInvalidateFramebuffer
//-----------------------------------------------------------------------------

static MaliPFN_InvalidateFramebuffer MaliOpt_GetInvalidate()
{
	static MaliPFN_InvalidateFramebuffer s_pfn = NULL;
	static bool s_bTried = false;
	if ( !s_bTried )
	{
		s_bTried = true;
		s_pfn = (MaliPFN_InvalidateFramebuffer)MaliOpt_GetProc( "glInvalidateFramebuffer" );
		Msg( "Mali optimizations: glInvalidateFramebuffer %s\n", s_pfn ? "available" : "NOT available (depth/stencil discard off)" );
	}
	return s_pfn;
}

bool MaliOpt_DiscardDepthAtPresent()
{
	return MaliOpt_IsMali() && gl_mali_discard_depth.GetBool() && MaliOpt_GetInvalidate() != NULL;
}

bool MaliOpt_InvalidateFramebuffer( unsigned int target, int numAttachments, const unsigned int *attachments )
{
	MaliPFN_InvalidateFramebuffer pfn = MaliOpt_GetInvalidate();
	if ( !pfn )
		return false;
	pfn( target, numAttachments, attachments );
	return true;
}

//-----------------------------------------------------------------------------
// 4. Occlusion query target
//-----------------------------------------------------------------------------

unsigned int MaliOpt_OcclusionQueryTarget()
{
	static unsigned int s_nTarget = 0;
	if ( s_nTarget == 0 )
	{
		if ( !gGL )
			return MALIGL_ANY_SAMPLES_PASSED;		// cannot decide yet (and cannot be called without GL anyway)

		s_nTarget = MALIGL_ANY_SAMPLES_PASSED;
		if ( MaliOpt_IsMali() && gl_mali_conservative_oq.GetBool() )
			s_nTarget = MALIGL_ANY_SAMPLES_PASSED_CONSERVATIVE;
	}
	return s_nTarget;
}

//-----------------------------------------------------------------------------
// 5. Program binary cache
//-----------------------------------------------------------------------------

// ---- fast 64-bit hash (xxHash64 algorithm, same as astc_texcompress.cpp) -------
static const uint64_t MXXP1 = 11400714785074694791ULL;
static const uint64_t MXXP2 = 14029467366897019727ULL;
static const uint64_t MXXP3 = 1609587929392839161ULL;
static const uint64_t MXXP4 = 9650029242287828579ULL;
static const uint64_t MXXP5 = 2870177450012600261ULL;

static inline uint64_t MRotl64( uint64_t x, int r ) { return ( x << r ) | ( x >> ( 64 - r ) ); }
static inline uint64_t MRead64( const uint8_t *p ) { uint64_t v; memcpy( &v, p, 8 ); return v; }
static inline uint32_t MRead32( const uint8_t *p ) { uint32_t v; memcpy( &v, p, 4 ); return v; }
static inline uint64_t MXXRound( uint64_t acc, uint64_t in ) { acc += in * MXXP2; acc = MRotl64( acc, 31 ); acc *= MXXP1; return acc; }
static inline uint64_t MXXMerge( uint64_t acc, uint64_t v ) { v = MXXRound( 0, v ); acc ^= v; return acc * MXXP1 + MXXP4; }

static uint64_t MaliHash64( const void *data, size_t len, uint64_t seed )
{
	const uint8_t *p = (const uint8_t *)data;
	const uint8_t *end = p + len;
	uint64_t h;

	if ( len >= 32 )
	{
		const uint8_t *limit = end - 32;
		uint64_t v1 = seed + MXXP1 + MXXP2;
		uint64_t v2 = seed + MXXP2;
		uint64_t v3 = seed;
		uint64_t v4 = seed - MXXP1;
		do
		{
			v1 = MXXRound( v1, MRead64( p ) ); p += 8;
			v2 = MXXRound( v2, MRead64( p ) ); p += 8;
			v3 = MXXRound( v3, MRead64( p ) ); p += 8;
			v4 = MXXRound( v4, MRead64( p ) ); p += 8;
		} while ( p <= limit );

		h = MRotl64( v1, 1 ) + MRotl64( v2, 7 ) + MRotl64( v3, 12 ) + MRotl64( v4, 18 );
		h = MXXMerge( h, v1 );
		h = MXXMerge( h, v2 );
		h = MXXMerge( h, v3 );
		h = MXXMerge( h, v4 );
	}
	else
	{
		h = seed + MXXP5;
	}

	h += (uint64_t)len;

	while ( p + 8 <= end )
	{
		h ^= MXXRound( 0, MRead64( p ) );
		h = MRotl64( h, 27 ) * MXXP1 + MXXP4;
		p += 8;
	}
	if ( p + 4 <= end )
	{
		h ^= (uint64_t)MRead32( p ) * MXXP1;
		h = MRotl64( h, 23 ) * MXXP2 + MXXP3;
		p += 4;
	}
	while ( p < end )
	{
		h ^= (uint64_t)( *p ) * MXXP5;
		h = MRotl64( h, 11 ) * MXXP1;
		++p;
	}

	h ^= h >> 33;
	h *= MXXP2;
	h ^= h >> 29;
	h *= MXXP3;
	h ^= h >> 32;
	return h;
}

// Part of every key: how CGLMShaderPair::SetProgramPair() binds the vertex attributes before linking
// (v0..v15 -> locations 0..15). Change this string if that code ever changes.
static const char kAttribBindingSignature[] = "glBindAttribLocation v0-v15 -> 0-15";

uint64_t MaliOpt_ProgramKey( const char *vsText, size_t vsLen, const char *fsText, size_t fsLen )
{
	uint64_t h = MaliHash64( vsText, vsLen, 0x4C415450ULL );
	h = MaliHash64( fsText, fsLen, h ^ 0x9E3779B97F4A7C15ULL );

	uint64_t lens[2] = { (uint64_t)vsLen, (uint64_t)fsLen };
	h = MaliHash64( lens, sizeof( lens ), h );
	h = MaliHash64( kAttribBindingSignature, sizeof( kAttribBindingSignature ), h );
	return h ? h : 1;			// 0 marks an empty slot in the index
}

#if MALIOPT_POSIX

namespace
{
	const uint32_t kRecMagic		= 0x31525043u;			// "CPR1"
	const uint32_t kFileVersion		= 1;
	const uint32_t kHeaderBytes		= 64;
	const uint32_t kRecHdrBytes		= 32;
	const uint32_t kMaxBinaryBytes	= 32u * 1024u * 1024u;	// sanity limit for one program

	struct FileHeader			// 64 bytes
	{
		char		magic[8];		// "TGLPBIN1"
		uint32_t	version;
		uint32_t	headerBytes;
		uint64_t	envHash;		// hash of the GL strings + binary formats: a different driver means a different cache
		uint8_t		reserved[40];
	};

	struct RecHeader			// 32 bytes, followed by dataLen bytes of binary, padded to a multiple of 8
	{
		uint32_t	magic;
		uint32_t	format;			// binaryFormat for glProgramBinary
		uint32_t	dataLen;
		uint32_t	reserved;
		uint64_t	key;
		uint64_t	dataHash;
	};

	struct Entry
	{
		uint64_t	key;			// 0 = empty slot
		uint64_t	offset;			// file offset of the binary
		uint64_t	hash;
		uint32_t	size;			// 0 = entry was found unusable, do not try again
		uint32_t	format;
	};

	typedef char CheckFileHeaderSize[ sizeof( FileHeader ) == 64 ? 1 : -1 ];
	typedef char CheckRecHeaderSize[ sizeof( RecHeader ) == 32 ? 1 : -1 ];

	struct ProgCache
	{
		int							state;			// 0 = not initialised, 1 = active, -1 = off
		bool						storeOff;		// writing failed or the file is full: restore only
		int							fd;
		uint64_t					fileEnd;
		uint64_t					maxBytes;
		uint64_t					envHash;
		Entry						*table;
		uint32_t					cap;
		uint32_t					count;
		uint8_t						*buf;
		size_t						bufCap;
		MaliPFN_GetProgramBinary	glGetProgramBinary;
		MaliPFN_ProgramBinary		glProgramBinary;
		MaliPFN_ProgramParameteri	glProgramParameteri;
		MaliPFN_GetProgramiv		glGetProgramiv;
		MaliPFN_GetIntegerv			glGetIntegerv;
		MaliPFN_GetError			glGetError;
		uint32_t					hits, misses, loadFails, stores, storeSkips;
		uint64_t					loadUs, storeUs, storedBytes;
	};

	ProgCache s_pc;											// static storage: starts zeroed (state 0 = not initialised yet)
	pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

	uint64_t NowUs()
	{
		struct timespec ts;
		clock_gettime( CLOCK_MONOTONIC, &ts );
		return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
	}

	struct Lock
	{
		Lock()	{ pthread_mutex_lock( &s_lock ); }
		~Lock()	{ pthread_mutex_unlock( &s_lock ); }
	};

	bool PReadFull( int fd, void *dst, size_t n, uint64_t off )
	{
		uint8_t *p = (uint8_t *)dst;
		while ( n )
		{
			const ssize_t r = pread( fd, p, n, (off_t)off );
			if ( r < 0 && errno == EINTR )
				continue;
			if ( r <= 0 )
				return false;
			p += r; off += (uint64_t)r; n -= (size_t)r;
		}
		return true;
	}

	bool PWriteFull( int fd, const void *src, size_t n, uint64_t off )
	{
		const uint8_t *p = (const uint8_t *)src;
		while ( n )
		{
			const ssize_t r = pwrite( fd, p, n, (off_t)off );
			if ( r < 0 && errno == EINTR )
				continue;
			if ( r <= 0 )
				return false;
			p += r; off += (uint64_t)r; n -= (size_t)r;
		}
		return true;
	}

	void DrainGLErrors()
	{
		if ( !s_pc.glGetError )
			return;
		for ( int i = 0; i < 16 && s_pc.glGetError() != 0; ++i )
		{
		}
	}

	bool EnsureBuf( size_t n )
	{
		if ( n <= s_pc.bufCap )
			return true;
		size_t cap = s_pc.bufCap ? s_pc.bufCap : 256 * 1024;
		while ( cap < n )
			cap *= 2;
		uint8_t *nb = (uint8_t *)realloc( s_pc.buf, cap );
		if ( !nb )
			return false;
		s_pc.buf = nb;
		s_pc.bufCap = cap;
		return true;
	}

	// ---- open-addressing index ------------------------------------------------
	inline uint32_t SlotOf( uint64_t key, uint32_t cap )
	{
		return (uint32_t)( key ^ ( key >> 32 ) ) & ( cap - 1 );
	}

	Entry *TableFind( uint64_t key )
	{
		if ( !s_pc.cap )
			return NULL;
		uint32_t i = SlotOf( key, s_pc.cap );
		for ( uint32_t n = 0; n < s_pc.cap; ++n, i = ( i + 1 ) & ( s_pc.cap - 1 ) )
		{
			Entry *e = &s_pc.table[i];
			if ( e->key == 0 )
				return NULL;
			if ( e->key == key )
				return e;
		}
		return NULL;
	}

	bool TableGrow()
	{
		const uint32_t newCap = s_pc.cap ? s_pc.cap * 2 : 4096;
		Entry *nt = (Entry *)calloc( newCap, sizeof( Entry ) );
		if ( !nt )
			return false;
		for ( uint32_t i = 0; i < s_pc.cap; ++i )
		{
			const Entry &e = s_pc.table[i];
			if ( e.key == 0 )
				continue;
			uint32_t j = SlotOf( e.key, newCap );
			while ( nt[j].key != 0 )
				j = ( j + 1 ) & ( newCap - 1 );
			nt[j] = e;
		}
		free( s_pc.table );
		s_pc.table = nt;
		s_pc.cap = newCap;
		return true;
	}

	// Adds or replaces (the newest record of a key wins).
	bool TablePut( uint64_t key, uint64_t offset, uint64_t hash, uint32_t size, uint32_t format )
	{
		Entry *e = TableFind( key );
		if ( !e )
		{
			if ( ( s_pc.count + 1 ) * 2 > s_pc.cap && !TableGrow() )
				return false;
			uint32_t j = SlotOf( key, s_pc.cap );
			while ( s_pc.table[j].key != 0 )
				j = ( j + 1 ) & ( s_pc.cap - 1 );
			e = &s_pc.table[j];
			e->key = key;
			++s_pc.count;
		}
		e->offset = offset;
		e->hash = hash;
		e->size = size;
		e->format = format;
		return true;
	}

	// Called with the lock held, once.
	bool ProgCacheInit()
	{
		s_pc.state = -1;

		const bool bForce = CommandLine()->FindParm( "-gl_progcache_all" ) != 0;
		if ( CommandLine()->FindParm( "-gl_no_progcache" ) )
			return false;
		if ( !bForce && !MaliOpt_IsMali() )
			return false;
		if ( !gl_mali_progcache.GetBool() )
			return false;

		s_pc.glGetProgramBinary		= (MaliPFN_GetProgramBinary)MaliOpt_GetProc( "glGetProgramBinary" );
		s_pc.glProgramBinary		= (MaliPFN_ProgramBinary)MaliOpt_GetProc( "glProgramBinary" );
		s_pc.glProgramParameteri	= (MaliPFN_ProgramParameteri)MaliOpt_GetProc( "glProgramParameteri" );
		s_pc.glGetProgramiv			= (MaliPFN_GetProgramiv)MaliOpt_GetProc( "glGetProgramiv" );
		s_pc.glGetIntegerv			= (MaliPFN_GetIntegerv)MaliOpt_GetProc( "glGetIntegerv" );
		s_pc.glGetError				= (MaliPFN_GetError)MaliOpt_GetProc( "glGetError" );
		if ( !s_pc.glGetProgramBinary || !s_pc.glProgramBinary || !s_pc.glProgramParameteri ||
			 !s_pc.glGetProgramiv || !s_pc.glGetIntegerv || !s_pc.glGetError )
		{
			Msg( "Mali program cache: GL program binary entry points are missing, cache off\n" );
			return false;
		}

		DrainGLErrors();

		int nFormats = 0;
		s_pc.glGetIntegerv( MALIGL_NUM_PROGRAM_BINARY_FORMATS, &nFormats );
		if ( nFormats <= 0 )
		{
			DrainGLErrors();
			Msg( "Mali program cache: the driver reports no program binary formats, cache off\n" );
			return false;
		}
		int formats[32];
		memset( formats, 0, sizeof( formats ) );
		const int nKeep = nFormats < 32 ? nFormats : 32;
		s_pc.glGetIntegerv( MALIGL_PROGRAM_BINARY_FORMATS, formats );
		DrainGLErrors();

		// The cache is only valid for the exact driver that wrote it.
		char env[1024];
		const int envLen = snprintf( env, sizeof( env ), "%s|%s|%s|v%u|%d",
			gGL->m_pGLDriverStrings[cGLVendorString], gGL->m_pGLDriverStrings[cGLRendererString],
			gGL->m_pGLDriverStrings[cGLVersionString], kFileVersion, nFormats );
		uint64_t envHash = MaliHash64( env, envLen > 0 ? (size_t)envLen : 0, 0x454E5631ULL );
		envHash = MaliHash64( formats, sizeof( int ) * (size_t)nKeep, envHash );
		s_pc.envHash = envHash;

		const char *dir = gl_mali_progcache_dir.GetString();
		if ( !dir || !dir[0] )
			return false;
		mkdir( dir, 0755 );		// EEXIST is fine

		char path[1200];
		snprintf( path, sizeof( path ), "%s/progcache.bin", dir );
		int fd = open( path, O_RDWR | O_CREAT | O_CLOEXEC, 0644 );
		if ( fd < 0 )
		{
			Warning( "Mali program cache: cannot open %s (errno %d), cache off\n", path, errno );
			return false;
		}

		struct stat st;
		memset( &st, 0, sizeof( st ) );
		fstat( fd, &st );
		uint64_t fileSize = (uint64_t)st.st_size;

		FileHeader hd;
		bool valid = false;
		if ( fileSize >= kHeaderBytes && PReadFull( fd, &hd, sizeof( hd ), 0 ) )
			valid = memcmp( hd.magic, "TGLPBIN1", 8 ) == 0 && hd.version == kFileVersion && hd.headerBytes == kHeaderBytes && hd.envHash == envHash;

		if ( !valid )
		{
			if ( fileSize > 0 )
				Msg( "Mali program cache: %s was written by another driver or version, starting a new one\n", path );

			memset( &hd, 0, sizeof( hd ) );
			memcpy( hd.magic, "TGLPBIN1", 8 );
			hd.version = kFileVersion;
			hd.headerBytes = kHeaderBytes;
			hd.envHash = envHash;
			if ( ftruncate( fd, 0 ) != 0 || !PWriteFull( fd, &hd, sizeof( hd ), 0 ) )
			{
				Warning( "Mali program cache: cannot initialize %s (errno %d), cache off\n", path, errno );
				close( fd );
				return false;
			}
			fileSize = kHeaderBytes;
		}

		// Index the records. A damaged tail is cut off.
		uint64_t off = kHeaderBytes;
		while ( off + kRecHdrBytes <= fileSize )
		{
			RecHeader rh;
			if ( !PReadFull( fd, &rh, sizeof( rh ), off ) )
				break;
			const uint64_t total = (uint64_t)kRecHdrBytes + ( ( (uint64_t)rh.dataLen + 7ull ) & ~7ull );
			if ( rh.magic != kRecMagic || rh.key == 0 || rh.dataLen == 0 || rh.dataLen > kMaxBinaryBytes || off + total > fileSize )
				break;
			if ( !TablePut( rh.key, off + kRecHdrBytes, rh.dataHash, rh.dataLen, rh.format ) )
				break;
			off += total;
		}
		if ( off < fileSize )
		{
			Msg( "Mali program cache: dropping %llu damaged byte(s) at the end of %s\n", (unsigned long long)( fileSize - off ), path );
			if ( ftruncate( fd, (off_t)off ) != 0 )
				s_pc.storeOff = true;
		}

		long mb = gl_mali_progcache_max_mb.GetInt();
		if ( mb < 16 )
			mb = 16;
		s_pc.maxBytes = (uint64_t)mb * 1024ull * 1024ull;
		s_pc.fileEnd = off;
		s_pc.fd = fd;
		s_pc.state = 1;

		Msg( "Mali program cache: %s ready, %u programs, %.1f MB (limit %ld MB)\n",
			 path, s_pc.count, (double)off / ( 1024.0 * 1024.0 ), mb );
		return true;
	}
} // namespace

bool MaliOpt_ProgramCacheActive()
{
	if ( s_pc.state != 0 )
		return s_pc.state > 0;
	if ( !gGL )
		return false;			// no GL yet; decide later

	Lock lk;
	if ( s_pc.state == 0 )
		ProgCacheInit();
	return s_pc.state > 0;
}

bool MaliOpt_ProgramLoad( unsigned int program, uint64_t key )
{
	if ( s_pc.state <= 0 )
		return false;

	Lock lk;
	const uint64_t t0 = NowUs();

	Entry *e = TableFind( key );
	if ( !e || e->size == 0 )
	{
		++s_pc.misses;
		return false;
	}

	if ( !EnsureBuf( e->size ) || !PReadFull( s_pc.fd, s_pc.buf, e->size, e->offset ) ||
		 MaliHash64( s_pc.buf, e->size, 0x42494E31ULL ) != e->hash )
	{
		e->size = 0;			// unreadable or corrupt: never try this one again, it will be re-linked and stored anew
		++s_pc.loadFails;
		return false;
	}

	DrainGLErrors();
	s_pc.glProgramBinary( program, e->format, s_pc.buf, (int)e->size );
	int linked = 0;
	s_pc.glGetProgramiv( program, MALIGL_LINK_STATUS, &linked );
	DrainGLErrors();

	if ( !linked )
	{
		e->size = 0;			// driver refused it (e.g. a different GPU model): fall back to source
		++s_pc.loadFails;
		return false;
	}

	++s_pc.hits;
	s_pc.loadUs += NowUs() - t0;
	return true;
}

void MaliOpt_ProgramPrepareLink( unsigned int program )
{
	if ( s_pc.state <= 0 || s_pc.storeOff )
		return;
	s_pc.glProgramParameteri( program, MALIGL_PROGRAM_BINARY_RETRIEVABLE_HINT, MALIGL_TRUE );
	DrainGLErrors();
}

void MaliOpt_ProgramStore( unsigned int program, uint64_t key )
{
	if ( s_pc.state <= 0 )
		return;

	Lock lk;
	if ( s_pc.storeOff )
		return;

	const uint64_t t0 = NowUs();

	int len = 0;
	DrainGLErrors();
	s_pc.glGetProgramiv( program, MALIGL_PROGRAM_BINARY_LENGTH, &len );
	if ( len <= 0 || (uint32_t)len > kMaxBinaryBytes )
	{
		DrainGLErrors();
		++s_pc.storeSkips;
		return;
	}

	const uint64_t recBytes = (uint64_t)kRecHdrBytes + ( ( (uint64_t)len + 7ull ) & ~7ull );
	if ( s_pc.fileEnd + recBytes > s_pc.maxBytes )
	{
		s_pc.storeOff = true;
		Msg( "Mali program cache: the cache file reached its %.0f MB limit, no more programs are added (gl_mali_progcache_max_mb)\n",
			 (double)s_pc.maxBytes / ( 1024.0 * 1024.0 ) );
		return;
	}

	if ( !EnsureBuf( (size_t)recBytes ) )
	{
		++s_pc.storeSkips;
		return;
	}

	uint8_t *payload = s_pc.buf + kRecHdrBytes;
	int outLen = 0;
	unsigned int format = 0;
	s_pc.glGetProgramBinary( program, len, &outLen, &format, payload );
	DrainGLErrors();
	if ( outLen <= 0 || outLen > len )
	{
		++s_pc.storeSkips;
		return;
	}

	// pad to 8 bytes with zeros
	const size_t padded = ( (size_t)outLen + 7u ) & ~(size_t)7u;
	memset( payload + outLen, 0, padded - (size_t)outLen );

	RecHeader rh;
	memset( &rh, 0, sizeof( rh ) );
	rh.magic = kRecMagic;
	rh.format = format;
	rh.dataLen = (uint32_t)outLen;
	rh.key = key;
	rh.dataHash = MaliHash64( payload, (size_t)outLen, 0x42494E31ULL );
	memcpy( s_pc.buf, &rh, sizeof( rh ) );

	const size_t total = kRecHdrBytes + padded;
	if ( !PWriteFull( s_pc.fd, s_pc.buf, total, s_pc.fileEnd ) )
	{
		Warning( "Mali program cache: write failed (errno %d), no more programs are added\n", errno );
		if ( ftruncate( s_pc.fd, (off_t)s_pc.fileEnd ) != 0 )		// drop a partial record
			errno = 0;
		s_pc.storeOff = true;
		return;
	}

	TablePut( key, s_pc.fileEnd + kRecHdrBytes, rh.dataHash, rh.dataLen, rh.format );
	s_pc.fileEnd += total;
	++s_pc.stores;
	s_pc.storedBytes += total;
	s_pc.storeUs += NowUs() - t0;
}

CON_COMMAND( gl_mali_stats, "Print the Mali optimization statistics (program binary cache)." )
{
	ConMsg( "Mali optimizations: %s\n", MaliOpt_IsMali() ? "enabled" : "disabled / not a Mali GPU" );
	if ( s_pc.state <= 0 )
	{
		ConMsg( "Program binary cache: off\n" );
		return;
	}

	Lock lk;
	ConMsg( "Program binary cache: %u programs in the file, %.1f MB, limit %.0f MB%s\n",
		s_pc.count, (double)s_pc.fileEnd / ( 1024.0 * 1024.0 ), (double)s_pc.maxBytes / ( 1024.0 * 1024.0 ),
		s_pc.storeOff ? " (read-only now)" : "" );
	ConMsg( "  restored from cache: %u (%.1f ms total, %.2f ms each)\n", s_pc.hits, s_pc.loadUs / 1000.0, s_pc.hits ? s_pc.loadUs / 1000.0 / s_pc.hits : 0.0 );
	ConMsg( "  not in cache (compiled): %u, refused/corrupt entries: %u\n", s_pc.misses, s_pc.loadFails );
	ConMsg( "  newly stored: %u (%.1f MB, %.1f ms total), skipped: %u\n", s_pc.stores, s_pc.storedBytes / ( 1024.0 * 1024.0 ), s_pc.storeUs / 1000.0, s_pc.storeSkips );
}

#else // !MALIOPT_POSIX

bool MaliOpt_ProgramCacheActive()									{ return false; }
bool MaliOpt_ProgramLoad( unsigned int, uint64_t )					{ return false; }
void MaliOpt_ProgramPrepareLink( unsigned int )						{}
void MaliOpt_ProgramStore( unsigned int, uint64_t )					{}

#endif // MALIOPT_POSIX
