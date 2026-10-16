//========= Mali (ARM GPU) optimizations for TOGL ================================//
//
// mali_opt.cpp
//
// See mali_opt.h. Contents:
//   1. GPU identity: Vendor ID / Device ID found automatically (system properties, device nodes, GL strings) and handed to
//      other code (environment variables, exported C functions, console command gl_gpu_info), plus the Mali-only guard
//      (this build refuses to run on Qualcomm Adreno). Everything new in this section is self-contained: no other file needs
//      to change, the existing MaliOpt_* API below is unchanged.
//   2. Mali detection (MaliOpt_IsMali); helper that loads the few GL entry points that are not in TOGL's function table
//   3. glInvalidateFramebuffer wrapper (skip tile write-back of dead depth/stencil)
//   4. occlusion query target selection
//   5. program binary cache (single append-only file, in-memory open-addressing index)
//
// Command line / environment switches added here:
//   -gl_allow_adreno  or  TOGL_ALLOW_ADRENO=1    run on an Adreno GPU anyway (unsupported, for testing)
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
// 1. GPU identity (Vendor ID / Device ID) and the Mali-only guard
//
// Source engine code wants PCI style ids (D3DADAPTER_IDENTIFIER9::VendorId / DeviceId). Android has no PCI bus, so they are
// worked out here, automatically, in two steps:
//   a) when the library is loaded, without any GL: system properties (ro.hardware.egl, ro.hardware.vulkan, ro.hardware,
//      ro.soc.manufacturer), the GPU device nodes (/dev/mali0, /dev/kgsl-3d0) and, where the app is allowed to read it, the
//      Mali kernel driver's sysfs "gpuinfo" file;
//   b) as soon as TOGL's GL entry points exist: the GL_VENDOR / GL_RENDERER strings. They are authoritative and win over a).
//
// VendorID: the real PCI / Vulkan vendor id (ARM 0x13B5, Qualcomm 0x5143, NVIDIA 0x10DE, AMD 0x1002, Intel 0x8086, ...).
// DeviceID: Mali and Adreno have no PCI device ids, so it is DERIVED: a 32 bit FNV-1a hash of the normalised GPU name
//   ("MALI-G57 MC2"). It is stable between runs and differs between models / core counts, but it is NOT an official id.
//   On Linux with a PCI GPU the real id from /sys/class/drm is used instead. TOGL_GPU_DEVICE_ID_SOURCE tells which one
//   you got: "pci" or "name-hash" ("none" = unknown, value 0).
//
// The result is handed to other code in three ways:
//   * environment variables (visible to every native library, to child processes and, through android.system.Os.getenv,
//     to Java). Set when the library loads (system probe) and updated when the GL context exists:
//       TOGL_GPU_VENDOR_ID=0x13B5  TOGL_GPU_DEVICE_ID=0x........  TOGL_GPU_DEVICE_ID_SOURCE=name-hash
//       TOGL_GPU_VENDOR=ARM  TOGL_GPU_NAME="Mali-G57 MC2"  TOGL_GPU_BRAND=mali|adreno|other|unknown
//       TOGL_GPU_SUPPORTED=1|0  TOGL_GPU_INFO_SOURCE=devnode|property|sysfs|pci|gl|conflict|none
//       TOGL_GPU_HW_PRODUCT_ID=0x....      (Mali only, only when the kernel driver's gpuinfo could be read)
//   * plain C functions exported by libtogl.so (declare them where you need them, or dlsym them):
//       extern "C" unsigned int Togl_GetGpuVendorId( void );
//       extern "C" unsigned int Togl_GetGpuDeviceId( void );
//       extern "C" const char  *Togl_GetGpuName( void );
//       extern "C" int          Togl_IsGpuSupported( void );     // 0 = this Mali-only build must not run on this GPU
//   * the console command gl_gpu_info.
//
// Mali-only guard: this build is made for ARM Mali GPUs. On a Qualcomm Adreno GPU it refuses to run: the first time TOGL
// reaches the code in this file (first shader program, query or Present; always after the GL context exists) the game is
// stopped with a clear message (engine log, logcat and Error()). Other GPUs (PowerVR, the desktop GPU of a Linux test
// machine, ...) keep working, only without the Mali optimizations. For testing on Adreno: -gl_allow_adreno on the command
// line or TOGL_ALLOW_ADRENO=1 in the environment. A launcher that wants to decide earlier can read TOGL_GPU_SUPPORTED /
// Togl_IsGpuSupported() right after loading the library.
//-----------------------------------------------------------------------------

#define MALIOPT_VENDOR_ARM			0x13B5u
#define MALIOPT_VENDOR_QUALCOMM		0x5143u
#define MALIOPT_VENDOR_NVIDIA		0x10DEu
#define MALIOPT_VENDOR_AMD			0x1002u
#define MALIOPT_VENDOR_INTEL		0x8086u
#define MALIOPT_VENDOR_APPLE		0x106Bu
#define MALIOPT_VENDOR_IMAGINATION	0x1010u

#define MALIOPT_PROP_MAX			96		// >= PROP_VALUE_MAX (92) of bionic

#if defined( __GNUC__ )
	#define MALIOPT_EXPORT	extern "C" __attribute__(( visibility( "default" ) ))
#else
	#define MALIOPT_EXPORT	extern "C"
#endif

enum
{
	kMaliBrandUnknown = 0,
	kMaliBrandMali,
	kMaliBrandAdreno,
	kMaliBrandOther
};

struct MaliGpuIdentity					// plain data; published snapshots of it are never modified or freed
{
	uint32_t	vendorId;				// PCI style vendor id, 0 = unknown
	uint32_t	deviceId;				// 0 = unknown, see deviceIdSource
	uint32_t	hwProductId;			// Mali: product id reported by the kernel driver (sysfs gpuinfo), 0 = not available
	int			brand;					// kMaliBrand*
	int			cores;					// shader cores (MC<n> / MP<n>), 0 = unknown
	int			supported;				// 0 = this Mali-only build must not run on this GPU
	char		vendorName[24];
	char		name[80];
	char		deviceIdSource[12];		// "none", "pci", "name-hash"
	char		source[12];				// "none", "devnode", "property", "sysfs", "pci", "gl", "conflict"
};

struct MaliSysProbe						// raw facts collected from the system, before any decision
{
	char		egl[MALIOPT_PROP_MAX];			// ro.hardware.egl
	char		vulkan[MALIOPT_PROP_MAX];		// ro.hardware.vulkan
	char		hardware[MALIOPT_PROP_MAX];		// ro.hardware
	char		bootHardware[MALIOPT_PROP_MAX];	// ro.boot.hardware
	char		socMfr[MALIOPT_PROP_MAX];		// ro.soc.manufacturer
	char		socModel[MALIOPT_PROP_MAX];		// ro.soc.model (informational)
	char		platform[MALIOPT_PROP_MAX];		// ro.board.platform (informational)
	bool		devMali;						// /dev/mali0 exists
	bool		devKgsl;						// /dev/kgsl-3d0 exists
	char		gpuinfo[160];					// first line of the Mali kernel driver's sysfs "gpuinfo", "" if not readable
	uint32_t	drmVendor, drmDevice;			// PCI ids from /sys/class/drm (Linux desktop), 0 = none
};

// ---- small ASCII helpers (no locale, no <ctype.h>) ---------------------------------------------
static inline char MaliLower( char c )		{ return ( c >= 'A' && c <= 'Z' ) ? (char)( c + 32 ) : c; }
static inline char MaliUpper( char c )		{ return ( c >= 'a' && c <= 'z' ) ? (char)( c - 32 ) : c; }
static inline bool MaliIsDigit( char c )	{ return c >= '0' && c <= '9'; }
static inline bool MaliIsAlnum( char c )	{ return MaliIsDigit( c ) || ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ); }
static inline bool MaliIsSpace( char c )	{ return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static inline const char *MaliSafe( const char *s )	{ return s ? s : ""; }

static const char *MaliStrIStr( const char *hay, const char *needle )		// case-insensitive strstr, NULL safe
{
	if ( !hay || !needle || !needle[0] )
		return NULL;
	const size_t nl = strlen( needle );
	for ( ; *hay; ++hay )
	{
		size_t i = 0;
		while ( i < nl && hay[i] && MaliLower( hay[i] ) == MaliLower( needle[i] ) )
			++i;
		if ( i == nl )
			return hay;
		if ( !hay[i] )
			return NULL;				// the rest is shorter than the needle
	}
	return NULL;
}

static bool MaliEqualsI( const char *a, const char *b )
{
	if ( !a || !b )
		return false;
	for ( ; *a && *b; ++a, ++b )
		if ( MaliLower( *a ) != MaliLower( *b ) )
			return false;
	return *a == *b;
}

// Copy with truncation. Leading / trailing blanks are dropped, control characters become blanks (the text ends up in
// environment variables and log lines).
static void MaliCopyClean( char *dst, size_t cap, const char *src )
{
	if ( !cap )
		return;
	dst[0] = 0;
	if ( !src )
		return;
	while ( *src && MaliIsSpace( *src ) )
		++src;
	size_t n = 0;
	for ( ; *src && n + 1 < cap; ++src, ++n )
		dst[n] = ( (unsigned char)*src < 0x20 || (unsigned char)*src == 0x7F ) ? ' ' : *src;
	while ( n > 0 && dst[n - 1] == ' ' )
		--n;
	dst[n] = 0;
}

static uint32_t MaliFnv32Upper( const char *s )		// FNV-1a over the upper-cased text; never returns 0
{
	uint32_t h = 2166136261u;
	for ( ; s && *s; ++s )
	{
		h ^= (uint32_t)(uint8_t)MaliUpper( *s );
		h *= 16777619u;
	}
	return h ? h : 1u;
}

// "0x13b5", "13B5" -> value. 0 if there are no hex digits or more than 8 of them.
static uint32_t MaliParseHex( const char *s )
{
	if ( !s )
		return 0;
	while ( *s && MaliIsSpace( *s ) )
		++s;
	if ( s[0] == '0' && ( s[1] == 'x' || s[1] == 'X' ) )
		s += 2;
	uint32_t v = 0;
	int nDigits = 0;
	for ( ; ; ++s, ++nDigits )
	{
		const char c = MaliLower( *s );
		uint32_t d;
		if ( MaliIsDigit( c ) )						d = (uint32_t)( c - '0' );
		else if ( c >= 'a' && c <= 'f' )			d = (uint32_t)( c - 'a' + 10 );
		else										break;
		if ( nDigits >= 8 )
			return 0;
		v = ( v << 4 ) | d;
	}
	return nDigits ? v : 0;
}

// Next run of letters / digits.
static bool MaliNextToken( const char **pp, const char **tok, size_t *len )
{
	const char *p = *pp;
	while ( *p && !MaliIsAlnum( *p ) )
		++p;
	if ( !*p )
	{
		*pp = p;
		return false;
	}
	*tok = p;
	while ( MaliIsAlnum( *p ) )
		++p;
	*len = (size_t)( p - *tok );
	*pp = p;
	return true;
}

// ---- Mali model names ---------------------------------------------------------------------------
struct MaliName
{
	char	brand[16];		// "Mali" or "Immortalis"
	char	family[24];		// upper case: "G57", "T880", "400", "G715"
	int		cores;			// 0 = unknown
};

// "Mali-G57 MC2" -> Mali / G57 / 2.  Also "Immortalis-G715 MC11", "Mali-T880 MP12", "Mali-G52 2EE MC2", "Mali-G71 20 cores",
// "Mali-400 MP". Anything else (no "Mali-" / "Immortalis-") returns false.
static bool MaliParseName( const char *s, MaliName *out )
{
	memset( out, 0, sizeof( *out ) );
	if ( !s )
		return false;

	const char *brand = "Mali";
	size_t skip = 5;
	const char *p = MaliStrIStr( s, "mali-" );
	const char *q = MaliStrIStr( s, "immortalis-" );
	if ( q && ( !p || q < p ) )
	{
		p = q;
		brand = "Immortalis";
		skip = 11;
	}
	if ( !p )
		return false;
	p += skip;

	size_t n = 0;
	while ( p[n] && ( MaliIsAlnum( p[n] ) || p[n] == '-' ) && n + 1 < sizeof( out->family ) )
	{
		out->family[n] = MaliUpper( p[n] );
		++n;
	}
	out->family[n] = 0;
	if ( n == 0 )
		return false;
	p += n;
	snprintf( out->brand, sizeof( out->brand ), "%s", brand );

	// the core count: a token MC<n> / MP<n>, or "<n> cores"
	const char *tok = NULL;
	size_t len = 0;
	int prevNumber = 0;
	while ( MaliNextToken( &p, &tok, &len ) )
	{
		int number = 0;
		bool allDigits = true;
		for ( size_t i = 0; i < len; ++i )
		{
			if ( !MaliIsDigit( tok[i] ) )
			{
				allDigits = false;
				break;
			}
			if ( number < 100000 )
				number = number * 10 + ( tok[i] - '0' );
		}

		if ( len >= 3 && MaliUpper( tok[0] ) == 'M' && ( MaliUpper( tok[1] ) == 'C' || MaliUpper( tok[1] ) == 'P' ) )
		{
			int v = 0;
			bool ok = true;
			for ( size_t i = 2; i < len && ok; ++i )
			{
				ok = MaliIsDigit( tok[i] );
				if ( ok && v < 100000 )
					v = v * 10 + ( tok[i] - '0' );
			}
			if ( ok && v > 0 && v <= 1024 )
			{
				out->cores = v;
				break;
			}
		}
		else if ( ( len == 5 && MaliStrIStr( tok, "cores" ) == tok ) || ( len == 4 && MaliStrIStr( tok, "core" ) == tok ) )
		{
			if ( prevNumber > 0 && prevNumber <= 1024 )
			{
				out->cores = prevNumber;
				break;
			}
		}
		prevNumber = allDigits ? number : 0;
	}
	return true;
}

// Normalised name used for the device id hash: "MALI-G57 MC2", "IMMORTALIS-G715".
static void MaliBuildKey( const MaliName &n, char *out, size_t cap )
{
	char brand[sizeof( n.brand )];
	size_t i = 0;
	for ( ; n.brand[i] && i + 1 < sizeof( brand ); ++i )
		brand[i] = MaliUpper( n.brand[i] );
	brand[i] = 0;
	if ( n.cores > 0 )
		snprintf( out, cap, "%s-%s MC%d", brand, n.family, n.cores );
	else
		snprintf( out, cap, "%s-%s", brand, n.family );
}

// First line of the Mali kernel driver's "gpuinfo", e.g. "Mali-G57 MC2 r0p1 0x9091" or "Mali-G71 20 cores r0p0 0x6000".
// The exact format differs between driver versions, so this only looks for a Mali model name and a 0x.... token.
static bool MaliParseGpuInfo( const char *line, MaliName *name, uint32_t *hwProduct )
{
	*hwProduct = 0;
	if ( !MaliParseName( line, name ) )
		return false;
	const char *p = line;
	const char *tok = NULL;
	size_t len = 0;
	while ( MaliNextToken( &p, &tok, &len ) )
	{
		if ( len >= 3 && tok[0] == '0' && ( tok[1] == 'x' || tok[1] == 'X' ) )
		{
			char tmp[16];
			if ( len < sizeof( tmp ) )
			{
				memcpy( tmp, tok, len );
				tmp[len] = 0;
				const uint32_t v = MaliParseHex( tmp );
				if ( v )
					*hwProduct = v;
			}
		}
	}
	return true;
}

static const char *MaliBrandName( int brand )
{
	switch ( brand )
	{
		case kMaliBrandMali:	return "mali";
		case kMaliBrandAdreno:	return "adreno";
		case kMaliBrandOther:	return "other";
		default:				return "unknown";
	}
}

static const char *MaliVendorName( uint32_t vendorId )
{
	switch ( vendorId )
	{
		case MALIOPT_VENDOR_ARM:			return "ARM";
		case MALIOPT_VENDOR_QUALCOMM:		return "Qualcomm";
		case MALIOPT_VENDOR_NVIDIA:			return "NVIDIA";
		case MALIOPT_VENDOR_AMD:			return "AMD";
		case MALIOPT_VENDOR_INTEL:			return "Intel";
		case MALIOPT_VENDOR_APPLE:			return "Apple";
		case MALIOPT_VENDOR_IMAGINATION:	return "Imagination";
		default:							return "";
	}
}

static uint32_t MaliVendorFromProvider( int provider )
{
	switch ( provider )
	{
		case cGLDriverProviderNVIDIA:			return MALIOPT_VENDOR_NVIDIA;
		case cGLDriverProviderAMD:				return MALIOPT_VENDOR_AMD;
		case cGLDriverProviderIntelOpenSource:	return MALIOPT_VENDOR_INTEL;
		case cGLDriverProviderApple:			return MALIOPT_VENDOR_APPLE;
		case cGLDriverProviderQualcomm:			return MALIOPT_VENDOR_QUALCOMM;
		case cGLDriverProviderARM:				return MALIOPT_VENDOR_ARM;
		case cGLDriverProviderImagination:		return MALIOPT_VENDOR_IMAGINATION;
		default:								return 0;
	}
}

// ---- reading the system -------------------------------------------------------------------------
// The three functions below are the only places that touch the system; they are called through pointers so that the host
// tests can feed in the properties / files of other devices.
typedef int		( *MaliPFN_SysProp )( const char *name, char *value );					// value >= MALIOPT_PROP_MAX bytes; returns its length (0 = unset)
typedef bool	( *MaliPFN_PathExists )( const char *path );
typedef bool	( *MaliPFN_ReadFile )( const char *path, char *buf, size_t cap );		// NUL terminated, false if unreadable / empty

#if MALIOPT_POSIX

static int MaliReal_SysProp( const char *name, char *value )
{
	typedef int ( *PFN_PropGet )( const char *, char * );
	static PFN_PropGet s_pfn = NULL;
	static bool s_bTried = false;
	value[0] = 0;
	if ( !s_bTried )
	{
		s_pfn = (PFN_PropGet)dlsym( MALIOPT_RTLD_DEFAULT, "__system_property_get" );		// bionic only; NULL elsewhere
		s_bTried = true;
	}
	if ( !s_pfn )
		return 0;
	const int n = s_pfn( name, value );
	value[MALIOPT_PROP_MAX - 1] = 0;
	if ( n <= 0 )
	{
		value[0] = 0;
		return 0;
	}
	return n;
}

static bool MaliReal_PathExists( const char *path )
{
	struct stat st;
	return stat( path, &st ) == 0;		// only a successful stat counts: "permission denied" proves nothing
}

static bool MaliReal_ReadFile( const char *path, char *buf, size_t cap )
{
	if ( !cap )
		return false;
	buf[0] = 0;
	const int fd = open( path, O_RDONLY | O_CLOEXEC );
	if ( fd < 0 )
		return false;
	size_t got = 0;
	while ( got + 1 < cap )
	{
		const ssize_t r = read( fd, buf + got, cap - 1 - got );
		if ( r < 0 && errno == EINTR )
			continue;
		if ( r <= 0 )
			break;
		got += (size_t)r;
	}
	close( fd );
	buf[got] = 0;
	return got > 0;
}

#else // !MALIOPT_POSIX

static int MaliReal_SysProp( const char *, char *value )				{ value[0] = 0; return 0; }
static bool MaliReal_PathExists( const char * )							{ return false; }
static bool MaliReal_ReadFile( const char *, char *buf, size_t cap )	{ if ( cap ) buf[0] = 0; return false; }

#endif // MALIOPT_POSIX

static MaliPFN_SysProp		s_pfnSysProp	= MaliReal_SysProp;
static MaliPFN_PathExists	s_pfnPathExists	= MaliReal_PathExists;
static MaliPFN_ReadFile		s_pfnReadFile	= MaliReal_ReadFile;

static void MaliProbeSystem( MaliSysProbe *p )
{
	memset( p, 0, sizeof( *p ) );

	s_pfnSysProp( "ro.hardware.egl", p->egl );
	s_pfnSysProp( "ro.hardware.vulkan", p->vulkan );
	s_pfnSysProp( "ro.hardware", p->hardware );
	s_pfnSysProp( "ro.boot.hardware", p->bootHardware );
	s_pfnSysProp( "ro.soc.manufacturer", p->socMfr );
	s_pfnSysProp( "ro.soc.model", p->socModel );
	s_pfnSysProp( "ro.board.platform", p->platform );

	// The userspace GL driver itself opens these nodes, so the app can always see them when the GPU is there.
	p->devMali = s_pfnPathExists( "/dev/mali0" );
	p->devKgsl = s_pfnPathExists( "/dev/kgsl-3d0" );

	// Mali kernel driver: model + product id. Often not readable by apps (SELinux); then this stays empty.
	if ( s_pfnReadFile( "/sys/class/misc/mali0/device/gpuinfo", p->gpuinfo, sizeof( p->gpuinfo ) ) )
	{
		for ( size_t i = 0; p->gpuinfo[i]; ++i )
		{
			if ( p->gpuinfo[i] == '\n' || p->gpuinfo[i] == '\r' )
			{
				p->gpuinfo[i] = 0;
				break;
			}
		}
	}

	// Linux desktop: the real PCI ids.
	for ( int i = 0; i < 4 && !p->drmVendor; ++i )
	{
		char path[64], buf[32];
		snprintf( path, sizeof( path ), "/sys/class/drm/card%d/device/vendor", i );
		if ( !s_pfnReadFile( path, buf, sizeof( buf ) ) )
			continue;
		const uint32_t vendor = MaliParseHex( buf );
		if ( !vendor )
			continue;
		uint32_t device = 0;
		snprintf( path, sizeof( path ), "/sys/class/drm/card%d/device/device", i );
		if ( s_pfnReadFile( path, buf, sizeof( buf ) ) )
			device = MaliParseHex( buf );
		p->drmVendor = vendor;
		p->drmDevice = device;
	}
}

// ---- deciding -----------------------------------------------------------------------------------

static void MaliFillName( MaliGpuIdentity *id, const MaliName &n )		// "Mali-G57 MC2"
{
	if ( n.cores > 0 )
		snprintf( id->name, sizeof( id->name ), "%s-%s MC%d", n.brand, n.family, n.cores );
	else
		snprintf( id->name, sizeof( id->name ), "%s-%s", n.brand, n.family );
	id->cores = n.cores;
}

// Step a): what the system says, no GL needed. Conflicting hints decide nothing (brand unknown): the guard never acts on
// a guess, it only acts on the GL strings.
static void MaliClassifySystem( const MaliSysProbe &p, MaliGpuIdentity *id )
{
	memset( id, 0, sizeof( *id ) );
	snprintf( id->deviceIdSource, sizeof( id->deviceIdSource ), "none" );
	snprintf( id->source, sizeof( id->source ), "none" );

	MaliName gi;
	memset( &gi, 0, sizeof( gi ) );
	uint32_t hw = 0;
	const bool bGpuInfo = p.gpuinfo[0] && MaliParseGpuInfo( p.gpuinfo, &gi, &hw );

	const bool bAdreno =
		p.devKgsl || MaliEqualsI( p.egl, "adreno" ) || MaliEqualsI( p.vulkan, "adreno" ) ||
		MaliEqualsI( p.hardware, "qcom" ) || MaliEqualsI( p.bootHardware, "qcom" ) ||
		MaliEqualsI( p.socMfr, "qti" ) || MaliStrIStr( p.socMfr, "qualcomm" ) != NULL;		// every Qualcomm SoC has an Adreno
	const bool bMali = p.devMali || MaliEqualsI( p.egl, "mali" ) || MaliEqualsI( p.vulkan, "mali" ) || bGpuInfo;

	if ( bMali && !bAdreno )
	{
		id->brand = kMaliBrandMali;
		id->vendorId = MALIOPT_VENDOR_ARM;
		snprintf( id->vendorName, sizeof( id->vendorName ), "ARM" );
		snprintf( id->source, sizeof( id->source ), "%s", bGpuInfo ? "sysfs" : ( p.devMali ? "devnode" : "property" ) );
		if ( bGpuInfo )
		{
			char key[64];
			MaliFillName( id, gi );
			MaliBuildKey( gi, key, sizeof( key ) );
			id->deviceId = MaliFnv32Upper( key );
			id->hwProductId = hw;
			snprintf( id->deviceIdSource, sizeof( id->deviceIdSource ), "name-hash" );
		}
	}
	else if ( bAdreno && !bMali )
	{
		id->brand = kMaliBrandAdreno;
		id->vendorId = MALIOPT_VENDOR_QUALCOMM;
		snprintf( id->vendorName, sizeof( id->vendorName ), "Qualcomm" );
		snprintf( id->source, sizeof( id->source ), "%s", p.devKgsl ? "devnode" : "property" );
	}
	else if ( bAdreno && bMali )
	{
		snprintf( id->source, sizeof( id->source ), "conflict" );
	}
	else if ( p.drmVendor )
	{
		id->brand = kMaliBrandOther;
		id->vendorId = p.drmVendor;
		id->deviceId = p.drmDevice;
		MaliCopyClean( id->vendorName, sizeof( id->vendorName ), MaliVendorName( p.drmVendor ) );
		snprintf( id->source, sizeof( id->source ), "pci" );
		if ( p.drmDevice )
			snprintf( id->deviceIdSource, sizeof( id->deviceIdSource ), "pci" );
	}
}

// Step b): the GL strings. 'early' is the result of step a); its Mali product id and PCI ids are kept when they still fit.
static void MaliClassifyGL( const MaliGpuIdentity &early, int provider, const char *vendorStr, const char *rendererStr, MaliGpuIdentity *id )
{
	*id = early;
	id->brand = kMaliBrandUnknown;
	id->vendorId = 0;
	id->deviceId = 0;
	id->hwProductId = 0;
	id->cores = 0;
	id->vendorName[0] = 0;
	snprintf( id->deviceIdSource, sizeof( id->deviceIdSource ), "none" );
	snprintf( id->source, sizeof( id->source ), "gl" );
	MaliCopyClean( id->name, sizeof( id->name ), rendererStr );

	const bool bAdreno = provider == cGLDriverProviderQualcomm || MaliStrIStr( vendorStr, "qualcomm" ) != NULL || MaliStrIStr( rendererStr, "adreno" ) != NULL;
	MaliName mn;
	const bool bMaliName = MaliParseName( rendererStr, &mn );

	if ( bAdreno )
	{
		id->brand = kMaliBrandAdreno;
		id->vendorId = MALIOPT_VENDOR_QUALCOMM;
	}
	else if ( provider == cGLDriverProviderARM || bMaliName )
	{
		id->brand = kMaliBrandMali;
		id->vendorId = MALIOPT_VENDOR_ARM;
		if ( early.brand == kMaliBrandMali )
			id->hwProductId = early.hwProductId;			// the kernel driver's product id, if the system probe could read it
	}
	else
	{
		id->brand = kMaliBrandOther;
		id->vendorId = MaliVendorFromProvider( provider );
	}
	MaliCopyClean( id->vendorName, sizeof( id->vendorName ), MaliVendorName( id->vendorId ) );

	if ( id->brand == kMaliBrandMali && bMaliName )
	{
		char key[64];
		id->cores = mn.cores;
		MaliBuildKey( mn, key, sizeof( key ) );
		id->deviceId = MaliFnv32Upper( key );
		snprintf( id->deviceIdSource, sizeof( id->deviceIdSource ), "name-hash" );
	}
	else if ( id->brand == kMaliBrandOther && strcmp( early.deviceIdSource, "pci" ) == 0 && early.vendorId == id->vendorId && early.deviceId )
	{
		id->deviceId = early.deviceId;												// real PCI id, same GPU
		snprintf( id->deviceIdSource, sizeof( id->deviceIdSource ), "pci" );
	}
	else if ( id->name[0] )
	{
		id->deviceId = MaliFnv32Upper( id->name );
		snprintf( id->deviceIdSource, sizeof( id->deviceIdSource ), "name-hash" );
	}
}

// ---- policy -------------------------------------------------------------------------------------

static bool MaliAdrenoOverride( bool bCommandLineReady )
{
	const char *e = getenv( "TOGL_ALLOW_ADRENO" );
	if ( e && e[0] && e[0] != '0' )
		return true;
	return bCommandLineReady && CommandLine()->FindParm( "-gl_allow_adreno" ) != 0;
}

// ---- state, publishing ---------------------------------------------------------------------------

#if MALIOPT_POSIX
	static pthread_mutex_t s_idLock = PTHREAD_MUTEX_INITIALIZER;
	struct MaliIdLock
	{
		MaliIdLock()	{ pthread_mutex_lock( &s_idLock ); }
		~MaliIdLock()	{ pthread_mutex_unlock( &s_idLock ); }
	};
#else
	struct MaliIdLock { MaliIdLock() {} ~MaliIdLock() {} };
#endif

static bool					s_bIdProbed = false;		// step a) done
static bool					s_bIdGLDone = false;		// step b) done
static MaliSysProbe			s_idProbe;
static MaliGpuIdentity		s_idEarly;					// result of step a)
static const MaliGpuIdentity *s_pIdCur = NULL;			// what everybody sees; replaced (never modified) when step b) is done
static MaliGpuIdentity		s_idFallback;				// only used if malloc fails

// Logcat, so the line is there even if the engine's log is not up yet (library constructor) or is lost in a crash.
static void MaliAndroidLog( int prio, const char *text )		// prio: 4 = info, 5 = warning, 6 = error
{
#if MALIOPT_POSIX
	typedef int ( *PFN_LogWrite )( int, const char *, const char * );
	static PFN_LogWrite s_pfn = NULL;
	static bool s_bTried = false;
	if ( !s_bTried )
	{
		s_pfn = (PFN_LogWrite)dlsym( MALIOPT_RTLD_DEFAULT, "__android_log_write" );
		s_bTried = true;
	}
	if ( s_pfn )
		s_pfn( prio, "Togles", text );
#else
	(void)prio;
	(void)text;
#endif
}

static void MaliLogIdentity( const MaliGpuIdentity &id, bool bEngineLog )
{
	char line[360];
	snprintf( line, sizeof( line ), "GPU identity (%s): \"%s\" vendor=%s id=0x%04X device id=0x%04X (%s) brand=%s supported=%d",
		id.source, id.name, id.vendorName[0] ? id.vendorName : "?", (unsigned)id.vendorId, (unsigned)id.deviceId,
		id.deviceIdSource, MaliBrandName( id.brand ), id.supported );
	MaliAndroidLog( 4, line );
	if ( bEngineLog )
		Msg( "%s\n", line );
}

static void MaliPublishEnv( const MaliGpuIdentity &id )
{
#if MALIOPT_POSIX
	char v[24];
	snprintf( v, sizeof( v ), "0x%04X", (unsigned)id.vendorId );
	setenv( "TOGL_GPU_VENDOR_ID", v, 1 );
	snprintf( v, sizeof( v ), "0x%04X", (unsigned)id.deviceId );
	setenv( "TOGL_GPU_DEVICE_ID", v, 1 );
	setenv( "TOGL_GPU_DEVICE_ID_SOURCE", id.deviceIdSource, 1 );
	if ( id.hwProductId )
	{
		snprintf( v, sizeof( v ), "0x%04X", (unsigned)id.hwProductId );
		setenv( "TOGL_GPU_HW_PRODUCT_ID", v, 1 );
	}
	else
	{
		unsetenv( "TOGL_GPU_HW_PRODUCT_ID" );
	}
	setenv( "TOGL_GPU_VENDOR", id.vendorName, 1 );
	setenv( "TOGL_GPU_NAME", id.name, 1 );
	setenv( "TOGL_GPU_BRAND", MaliBrandName( id.brand ), 1 );
	setenv( "TOGL_GPU_SUPPORTED", id.supported ? "1" : "0", 1 );
	setenv( "TOGL_GPU_INFO_SOURCE", id.source, 1 );
#else
	(void)id;
#endif
}

// Snapshots are tiny and there are at most two per process; they are never freed so a pointer handed out once stays valid.
static const MaliGpuIdentity *MaliMakeSnapshot( const MaliGpuIdentity &src, bool bCommandLineReady )
{
	MaliGpuIdentity *p = (MaliGpuIdentity *)malloc( sizeof( MaliGpuIdentity ) );
	if ( !p )
		p = &s_idFallback;
	*p = src;
	p->supported = ( p->brand != kMaliBrandAdreno || MaliAdrenoOverride( bCommandLineReady ) ) ? 1 : 0;
	return p;
}

// The identity as far as it is known right now. Detects on first use (system probe) and refines as soon as the GL strings
// exist. Never returns NULL; the result is immutable. Thread safe; does not call into the engine while holding the lock.
static const MaliGpuIdentity *MaliOpt_Identity()
{
	bool bLogSystem = false, bLogGL = false;
	int nSystemBrand = kMaliBrandUnknown;
	const MaliGpuIdentity *cur;

	{
		MaliIdLock lk;
		if ( !s_bIdProbed )
		{
			MaliProbeSystem( &s_idProbe );
			MaliClassifySystem( s_idProbe, &s_idEarly );
			s_pIdCur = MaliMakeSnapshot( s_idEarly, false );		// the command line may not exist yet (library constructor)
			MaliPublishEnv( *s_pIdCur );
			s_bIdProbed = true;
			bLogSystem = true;
		}
		if ( gGL && !s_bIdGLDone )
		{
			MaliGpuIdentity gl;
			MaliClassifyGL( s_idEarly, gGL->m_nDriverProvider, gGL->m_pGLDriverStrings[cGLVendorString], gGL->m_pGLDriverStrings[cGLRendererString], &gl );
			nSystemBrand = s_idEarly.brand;
			s_pIdCur = MaliMakeSnapshot( gl, true );
			MaliPublishEnv( *s_pIdCur );
			s_bIdGLDone = true;
			bLogGL = true;
		}
		cur = s_pIdCur;
	}

	if ( bLogGL )
	{
		MaliLogIdentity( *cur, true );
		if ( nSystemBrand != kMaliBrandUnknown && nSystemBrand != cur->brand )
			Warning( "Togles: the system probe guessed GPU brand \"%s\" but the GL driver says \"%s\"; using the GL driver's answer.\n",
				MaliBrandName( nSystemBrand ), MaliBrandName( cur->brand ) );
	}
	else if ( bLogSystem )
	{
		MaliLogIdentity( *cur, false );				// may run from the library constructor: logcat only
	}
	return cur;
}

MALIOPT_EXPORT unsigned int Togl_GetGpuVendorId( void )		{ return MaliOpt_Identity()->vendorId; }
MALIOPT_EXPORT unsigned int Togl_GetGpuDeviceId( void )		{ return MaliOpt_Identity()->deviceId; }
MALIOPT_EXPORT const char  *Togl_GetGpuName( void )			{ return MaliOpt_Identity()->name; }
MALIOPT_EXPORT int          Togl_IsGpuSupported( void )		{ return MaliOpt_Identity()->supported; }

#if MALIOPT_POSIX
// Runs when libtogl.so is loaded: publishes the environment variables early, before any GL exists. Only libc is used here
// (the engine is not initialised yet), and nothing in it can fail the load.
static void MaliOpt_OnLoad() __attribute__(( constructor ));
static void MaliOpt_OnLoad()
{
	MaliOpt_Identity();
}
#endif

// Called before the first use of any Mali feature (so: after the GL context exists). Stops the game on a GPU this build
// does not support. Idempotent.
static void MaliOpt_CheckGpuPolicy()
{
	static bool s_bChecked = false;
	if ( s_bChecked || !gGL )
		return;
	s_bChecked = true;

	const MaliGpuIdentity *id = MaliOpt_Identity();				// includes the GL strings now
	if ( id->brand == kMaliBrandAdreno )
	{
		if ( id->supported )
		{
			Warning( "Togles: Adreno GPU \"%s\" detected. This Togles build is made for ARM Mali GPUs only; it keeps running because of "
				"-gl_allow_adreno / TOGL_ALLOW_ADRENO (unsupported).\n", id->name );
			return;
		}

		char msg[420];
		snprintf( msg, sizeof( msg ),
			"This Togles build is made for ARM Mali GPUs only, but this device has a Qualcomm Adreno GPU (\"%s\"). Togles does not run on Adreno. "
			"Use the renderer that supports Adreno. (Testing only: add -gl_allow_adreno to the command line or set TOGL_ALLOW_ADRENO=1.)",
			id->name );
		MaliAndroidLog( 6, msg );
		Error( "Togles: %s\n", msg );							// does not return
#if MALIOPT_POSIX
		_exit( 3 );												// in case an Error() handler ever returns: Togles must not go on
#endif
	}
	else if ( id->brand != kMaliBrandMali )
	{
		Msg( "Togles: GPU \"%s\" is not an ARM Mali GPU: the Mali optimizations stay off, everything else works as usual.\n", id->name );
	}
}

//-----------------------------------------------------------------------------
// 2. Mali detection
//-----------------------------------------------------------------------------

bool MaliOpt_IsMali()
{
	static int s_nState = -1;			// -1 = not decided yet
	if ( s_nState < 0 )
	{
		if ( !gGL )
			return false;				// too early, ask again later

		MaliOpt_CheckGpuPolicy();		// Adreno: does not return

		s_nState = 0;
		if ( gGL->m_nDriverProvider == cGLDriverProviderARM && gl_mali_opt.GetBool() && !CommandLine()->FindParm( "-gl_no_mali_opt" ) )
			s_nState = 1;

		const MaliGpuIdentity *id = MaliOpt_Identity();
		Msg( "Mali optimizations: %s (GL_VENDOR=\"%s\", GPU \"%s\", VendorID 0x%04X, DeviceID 0x%04X)\n", s_nState ? "ENABLED" : "disabled",
			MaliSafe( gGL->m_pGLDriverStrings[cGLVendorString] ), id->name, (unsigned)id->vendorId, (unsigned)id->deviceId );
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
			MaliSafe( gGL->m_pGLDriverStrings[cGLVendorString] ), MaliSafe( gGL->m_pGLDriverStrings[cGLRendererString] ),
			MaliSafe( gGL->m_pGLDriverStrings[cGLVersionString] ), kFileVersion, nFormats );
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

	MaliOpt_CheckGpuPolicy();	// Adreno: does not return (also covers -gl_progcache_all, which skips MaliOpt_IsMali())

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
	{
		const MaliGpuIdentity *id = MaliOpt_Identity();
		ConMsg( "GPU: \"%s\"  VendorID 0x%04X  DeviceID 0x%04X  (gl_gpu_info for details)\n", id->name, (unsigned)id->vendorId, (unsigned)id->deviceId );
	}
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

CON_COMMAND( gl_gpu_info, "Print the GPU identity Togles detected (VendorID / DeviceID) and whether this Mali-only build supports it." )
{
	const MaliGpuIdentity *id = MaliOpt_Identity();
	const char *e;

	ConMsg( "GPU: \"%s\" (%s)\n", id->name[0] ? id->name : "unknown", id->vendorName[0] ? id->vendorName : "unknown vendor" );
	ConMsg( "  VendorID  = 0x%04X\n", (unsigned)id->vendorId );
	ConMsg( "  DeviceID  = 0x%04X  (%s)\n", (unsigned)id->deviceId,
		strcmp( id->deviceIdSource, "pci" ) == 0 ? "real PCI id" :
		strcmp( id->deviceIdSource, "name-hash" ) == 0 ? "derived from the GPU name, not an official id" : "unknown" );
	if ( id->hwProductId )
		ConMsg( "  kernel GPU product id = 0x%04X\n", (unsigned)id->hwProductId );
	if ( id->cores )
		ConMsg( "  shader cores = %d\n", id->cores );
	ConMsg( "  brand = %s, found by = %s\n", MaliBrandName( id->brand ), id->source );
	ConMsg( "  supported by this Mali-only build: %s%s\n", id->supported ? "yes" : "NO",
		( id->brand == kMaliBrandAdreno && id->supported ) ? " (Adreno allowed by -gl_allow_adreno / TOGL_ALLOW_ADRENO)" : "" );

	e = getenv( "TOGL_GPU_VENDOR_ID" );
	ConMsg( "  environment: TOGL_GPU_VENDOR_ID=%s", e ? e : "(unset)" );
	e = getenv( "TOGL_GPU_DEVICE_ID" );
	ConMsg( " TOGL_GPU_DEVICE_ID=%s", e ? e : "(unset)" );
	e = getenv( "TOGL_GPU_SUPPORTED" );
	ConMsg( " TOGL_GPU_SUPPORTED=%s\n", e ? e : "(unset)" );
}
