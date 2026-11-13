//========= EGL (Android) optimizations for TOGL ============================================================
//
// egl_opt_impl.inl - implementation of egl_opt.h. This file is #included at the very end of mali_opt.cpp, which is already
// part of the module (so no build script has to change) and has already included the engine headers used here
// (tier0/dbg.h, tier1/convar.h, icommandline.h). Nothing else is needed from the engine.
//
// Contents:
//   1. entry points, extension detection, EglOpt_Init
//   2. per frame native fence (EGL_ANDROID_native_fence_sync) and the in-flight frame limiter
//   3. frame timestamps (EGL_ANDROID_get_frame_timestamps) + presentation time pacing (EGL_ANDROID_presentation_time)
//   4. front buffer mode (EGL_ANDROID_front_buffer_auto_refresh)
//   5. AHardwareBuffer storage for dynamic vertex / index buffers
//   6. AHardwareBuffer storage for dynamic textures
//   7. screen recorder (EGL_ANDROID_recordable + MediaCodec)
//   8. frame hooks and console commands
//
// Threading: all the GL / EGL work runs on the thread that owns the GL context (the same one that calls Present, Lock and
// Unlock), so the state below is plain data. The recorder has one extra thread that only drains the encoder.
//==========================================================================================================

#include "egl_opt.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined( __ANDROID__ ) || defined( __linux__ )
	#define EGLOPT_POSIX 1
	#include <dlfcn.h>
	#include <errno.h>
	#include <fcntl.h>
	#include <poll.h>
	#include <pthread.h>
	#include <time.h>
	#include <unistd.h>
	#include <sys/stat.h>
	#include <sys/types.h>
	#ifndef MALIOPT_RTLD_DEFAULT		// normally defined near the top of mali_opt.cpp
		#if defined( RTLD_DEFAULT )
			#define MALIOPT_RTLD_DEFAULT	RTLD_DEFAULT
		#else
			#define MALIOPT_RTLD_DEFAULT	( (void *)0 )
		#endif
	#endif
#else
	#define EGLOPT_POSIX 0
#endif

//-----------------------------------------------------------------------------
// switches
//-----------------------------------------------------------------------------
static ConVar gl_egl_opt( "gl_egl_opt", "1", FCVAR_ARCHIVE, "Master switch of the EGL layer (fences, frame timestamps, presentation time, AHardwareBuffer storage). Needs a restart. -gl_no_egl_opt on the command line does the same." );
static ConVar gl_egl_native_fence( "gl_egl_native_fence", "1", FCVAR_ARCHIVE, "Use EGL_ANDROID_native_fence_sync for the per frame GPU fence (0 = use a GL sync object instead)." );
static ConVar gl_egl_max_frames( "gl_egl_max_frames", "0", FCVAR_ARCHIVE, "Limit the number of frames the CPU may run ahead of the GPU (2 = lowest latency, 3 = same as the default queue, 0 = off). Uses the per frame fence." );
static ConVar gl_egl_timestamps( "gl_egl_timestamps", "1", FCVAR_ARCHIVE, "Collect EGL_ANDROID_get_frame_timestamps (GPU time, display latency, missed refreshes). See gl_egl_stats." );
static ConVar gl_egl_present_time( "gl_egl_present_time", "1", FCVAR_ARCHIVE, "With fps_max below the refresh rate and vsync off: present every frame at an evenly spaced time on the compositor grid (EGL_ANDROID_presentation_time)." );
static ConVar gl_egl_front_buffer( "gl_egl_front_buffer", "0", FCVAR_ARCHIVE, "Experimental low latency single buffer mode (EGL_KHR_mutable_render_buffer + EGL_ANDROID_front_buffer_auto_refresh). Can tear." );
static ConVar gl_egl_ahb_buffers( "gl_egl_ahb_buffers", "1", FCVAR_ARCHIVE, "Dynamic vertex / index buffers live in AHardwareBuffer memory shared with the GPU (needs GL_EXT_external_buffer). Needs a restart of the video system." );
static ConVar gl_egl_ahb_buffer_min_kb( "gl_egl_ahb_buffer_min_kb", "16", FCVAR_ARCHIVE, "Smallest dynamic buffer (KB) that gets AHardwareBuffer storage." );
static ConVar gl_egl_ahb_buffer_slots( "gl_egl_ahb_buffer_slots", "3", FCVAR_ARCHIVE, "AHardwareBuffer slots created up front per dynamic buffer (it can grow to 8 when the GPU is behind)." );
static ConVar gl_egl_ahb_textures( "gl_egl_ahb_textures", "1", FCVAR_ARCHIVE, "Dynamic RGBA textures live in an AHardwareBuffer (no copy, no ASTC encode on every update). Needs a restart of the video system." );
static ConVar gl_egl_ahb_tex_min( "gl_egl_ahb_tex_min", "16", FCVAR_ARCHIVE, "Smallest side (pixels) of a dynamic texture that gets AHardwareBuffer storage." );
static ConVar gl_egl_ahb_tex_wait( "gl_egl_ahb_tex_wait", "0", FCVAR_ARCHIVE, "1 = a write lock on an AHardwareBuffer texture first waits until the previous frame has finished on the GPU (no visible tearing, may stall)." );
static ConVar gl_egl_ahb_max_mb( "gl_egl_ahb_max_mb", "128", FCVAR_ARCHIVE, "Upper limit (MB) of all AHardwareBuffer memory this layer allocates." );
static ConVar gl_egl_stats_sec( "gl_egl_stats_sec", "0", FCVAR_ARCHIVE, "Print the EGL statistics every N seconds (0 = only with gl_egl_stats)." );
static ConVar gl_egl_record_height( "gl_egl_record_height", "720", FCVAR_ARCHIVE, "Height of the video made by gl_egl_record_start." );
static ConVar gl_egl_record_fps( "gl_egl_record_fps", "30", FCVAR_ARCHIVE, "Frame rate of the video made by gl_egl_record_start." );

#if EGLOPT_POSIX

namespace
{

//-----------------------------------------------------------------------------
// constants (own names: the engine headers may or may not define the real ones)
//-----------------------------------------------------------------------------
const int		EO_EGL_NONE									= 0x3038;
const int		EO_EGL_EXTENSIONS							= 0x3055;
const int		EO_EGL_DRAW									= 0x3059;
const int		EO_EGL_WIDTH								= 0x3057;
const int		EO_EGL_HEIGHT								= 0x3056;
const int		EO_EGL_CONFIG_ID							= 0x3028;
const int		EO_EGL_SURFACE_TYPE							= 0x3033;
const int		EO_EGL_RENDERABLE_TYPE						= 0x3040;
const int		EO_EGL_RED_SIZE								= 0x3024;
const int		EO_EGL_GREEN_SIZE							= 0x3023;
const int		EO_EGL_BLUE_SIZE							= 0x3022;
const int		EO_EGL_ALPHA_SIZE							= 0x3021;
const int		EO_EGL_DEPTH_SIZE							= 0x3025;
const int		EO_EGL_STENCIL_SIZE							= 0x3026;
const int		EO_EGL_SAMPLES								= 0x3031;
const int		EO_EGL_WINDOW_BIT							= 0x0004;
const int		EO_EGL_RENDER_BUFFER						= 0x3086;
const int		EO_EGL_BACK_BUFFER							= 0x3084;
const int		EO_EGL_SINGLE_BUFFER						= 0x3085;
const int		EO_EGL_NATIVE_BUFFER_ANDROID				= 0x3140;
const int		EO_EGL_IMAGE_PRESERVED_KHR					= 0x30D2;
const int		EO_EGL_GL_COLORSPACE_KHR					= 0x309D;
const int		EO_EGL_GL_COLORSPACE_SRGB_KHR				= 0x3089;
const int		EO_EGL_RECORDABLE_ANDROID					= 0x3142;
const int		EO_EGL_FRONT_BUFFER_AUTO_REFRESH_ANDROID	= 0x314C;
const int		EO_EGL_SYNC_NATIVE_FENCE_ANDROID			= 0x3144;
const int		EO_EGL_SYNC_NATIVE_FENCE_FD_ANDROID			= 0x3145;
const int		EO_EGL_NO_NATIVE_FENCE_FD_ANDROID			= -1;
const int		EO_EGL_TIMESTAMPS_ANDROID					= 0x3430;
const int		EO_EGL_COMPOSITE_DEADLINE_ANDROID			= 0x3431;
const int		EO_EGL_COMPOSITE_INTERVAL_ANDROID			= 0x3432;
const int		EO_EGL_COMPOSITE_TO_PRESENT_LATENCY_ANDROID	= 0x3433;
const int		EO_EGL_REQUESTED_PRESENT_TIME_ANDROID		= 0x3434;
const int		EO_EGL_RENDERING_COMPLETE_TIME_ANDROID		= 0x3435;
const int		EO_EGL_DISPLAY_PRESENT_TIME_ANDROID			= 0x343A;
const int64_t	EO_EGL_TIMESTAMP_PENDING_ANDROID			= -2;

const unsigned	EO_GL_EXTENSIONS							= 0x1F03;
const unsigned	EO_GL_TEXTURE_2D							= 0x0DE1;
const unsigned	EO_GL_TEXTURE_BASE_LEVEL					= 0x813C;
const unsigned	EO_GL_TEXTURE_MAX_LEVEL						= 0x813D;
const unsigned	EO_GL_TEXTURE_SWIZZLE_R						= 0x8E42;
const unsigned	EO_GL_TEXTURE_SWIZZLE_G						= 0x8E43;
const unsigned	EO_GL_TEXTURE_SWIZZLE_B						= 0x8E44;
const unsigned	EO_GL_TEXTURE_SWIZZLE_A						= 0x8E45;
const int		EO_GL_RED									= 0x1903;
const int		EO_GL_GREEN									= 0x1904;
const int		EO_GL_BLUE									= 0x1905;
const int		EO_GL_ALPHA									= 0x1906;
const int		EO_GL_ONE									= 1;
const unsigned	EO_GL_MAP_READ_BIT							= 0x0001;
const unsigned	EO_GL_MAP_WRITE_BIT							= 0x0002;
const unsigned	EO_GL_MAP_PERSISTENT_BIT					= 0x0040;
const unsigned	EO_GL_MAP_COHERENT_BIT						= 0x0080;
const unsigned	EO_GL_SYNC_GPU_COMMANDS_COMPLETE			= 0x9117;
const unsigned	EO_GL_ALREADY_SIGNALED						= 0x911A;
const unsigned	EO_GL_CONDITION_SATISFIED					= 0x911C;
const unsigned	EO_GL_SCISSOR_TEST							= 0x0C11;
const unsigned	EO_GL_COLOR_BUFFER_BIT						= 0x4000;
const unsigned	EO_GL_LINEAR								= 0x2601;
const unsigned	EO_GL_READ_FRAMEBUFFER						= 0x8CA8;
const unsigned	EO_GL_DRAW_FRAMEBUFFER						= 0x8CA9;

// AHardwareBuffer (NDK 26+). The NDK header is not used: this project builds against an older sysroot, the functions are
// found with dlsym and the structure is declared here (its layout is part of the Android ABI).
const uint32_t	EO_AHB_FORMAT_R8G8B8A8_UNORM				= 1;
const uint32_t	EO_AHB_FORMAT_BLOB							= 0x21;
const uint64_t	EO_AHB_USAGE_CPU_READ_RARELY				= 2ull;
const uint64_t	EO_AHB_USAGE_CPU_WRITE_OFTEN				= 3ull << 4;
const uint64_t	EO_AHB_USAGE_GPU_SAMPLED_IMAGE				= 1ull << 8;
const uint64_t	EO_AHB_USAGE_GPU_COLOR_OUTPUT				= 1ull << 9;
const uint64_t	EO_AHB_USAGE_GPU_DATA_BUFFER				= 1ull << 24;

struct EoAhbDesc
{
	uint32_t	width;
	uint32_t	height;
	uint32_t	layers;
	uint32_t	format;
	uint64_t	usage;
	uint32_t	stride;
	uint32_t	rfu0;
	uint64_t	rfu1;
};
typedef char EoAhbDescSizeCheck[ sizeof( EoAhbDesc ) == 40 ? 1 : -1 ];

//-----------------------------------------------------------------------------
// entry point types
//-----------------------------------------------------------------------------
typedef void *		( *EoPFN_GetProcAddress )( const char *name );
typedef void *		( *EoPFN_GetCurrentDisplay )( void );
typedef void *		( *EoPFN_GetCurrentContext )( void );
typedef void *		( *EoPFN_GetCurrentSurface )( int readdraw );
typedef const char *( *EoPFN_QueryString )( void *dpy, int name );
typedef unsigned	( *EoPFN_SurfaceAttrib )( void *dpy, void *surf, int attrib, int value );
typedef unsigned	( *EoPFN_QuerySurface )( void *dpy, void *surf, int attrib, int *value );
typedef unsigned	( *EoPFN_QueryContext )( void *dpy, void *ctx, int attrib, int *value );
typedef unsigned	( *EoPFN_GetConfigAttrib )( void *dpy, void *config, int attrib, int *value );
typedef unsigned	( *EoPFN_ChooseConfig )( void *dpy, const int *attribs, void **configs, int configSize, int *numConfig );
typedef void *		( *EoPFN_CreateWindowSurface )( void *dpy, void *config, void *window, const int *attribs );
typedef unsigned	( *EoPFN_DestroySurface )( void *dpy, void *surf );
typedef unsigned	( *EoPFN_MakeCurrent )( void *dpy, void *draw, void *read, void *ctx );
typedef unsigned	( *EoPFN_SwapBuffers )( void *dpy, void *surf );
typedef int			( *EoPFN_EglGetError )( void );

typedef unsigned	( *EoPFN_PresentationTime )( void *dpy, void *surf, int64_t time );
typedef unsigned	( *EoPFN_GetNextFrameId )( void *dpy, void *surf, uint64_t *frameId );
typedef unsigned	( *EoPFN_GetCompositorTiming )( void *dpy, void *surf, int numTimestamps, const int *names, int64_t *values );
typedef unsigned	( *EoPFN_GetFrameTimestamps )( void *dpy, void *surf, uint64_t frameId, int numTimestamps, const int *timestamps, int64_t *values );
typedef unsigned	( *EoPFN_GetFrameTimestampSupported )( void *dpy, void *surf, int timestamp );
typedef void *		( *EoPFN_CreateSync )( void *dpy, unsigned type, const int *attribs );
typedef unsigned	( *EoPFN_DestroySync )( void *dpy, void *sync );
typedef int			( *EoPFN_DupNativeFenceFD )( void *dpy, void *sync );
typedef void *		( *EoPFN_GetNativeClientBuffer )( const void *ahb );
typedef void *		( *EoPFN_CreateImage )( void *dpy, void *ctx, unsigned target, void *buffer, const int *attribs );
typedef unsigned	( *EoPFN_DestroyImage )( void *dpy, void *image );

typedef int			( *EoPFN_AhbAllocate )( const EoAhbDesc *desc, void **outBuffer );
typedef void		( *EoPFN_AhbRelease )( void *buffer );
typedef void		( *EoPFN_AhbDescribe )( const void *buffer, EoAhbDesc *outDesc );
typedef int			( *EoPFN_AhbLock )( void *buffer, uint64_t usage, int32_t fence, const void *rect, void **outAddress );
typedef int			( *EoPFN_AhbUnlock )( void *buffer, int32_t *fence );

typedef const unsigned char *( *EoPFN_GlGetString )( unsigned name );
typedef unsigned	( *EoPFN_GlGetError )( void );
typedef void		( *EoPFN_GlGenBuffers )( int n, unsigned *buffers );
typedef void		( *EoPFN_GlBindBuffer )( unsigned target, unsigned buffer );
typedef void		( *EoPFN_GlDeleteBuffers )( int n, const unsigned *buffers );
typedef void *		( *EoPFN_GlMapBufferRange )( unsigned target, intptr_t offset, intptr_t length, unsigned access );
typedef void		( *EoPFN_GlBufferStorageExternal )( unsigned target, intptr_t offset, intptr_t size, void *clientBuffer, unsigned flags );
typedef void		( *EoPFN_GlEGLImageTargetTexture2D )( unsigned target, void *image );
typedef void		( *EoPFN_GlTexParameteri )( unsigned target, unsigned pname, int param );
typedef void *		( *EoPFN_GlFenceSync )( unsigned condition, unsigned flags );
typedef unsigned	( *EoPFN_GlClientWaitSync )( void *sync, unsigned flags, uint64_t timeout );
typedef void		( *EoPFN_GlDeleteSync )( void *sync );
typedef void		( *EoPFN_GlFlush )( void );
typedef unsigned char ( *EoPFN_GlIsEnabled )( unsigned cap );
typedef void		( *EoPFN_GlEnable )( unsigned cap );
typedef void		( *EoPFN_GlDisable )( unsigned cap );
typedef void		( *EoPFN_GlBindFramebuffer )( unsigned target, unsigned framebuffer );
typedef void		( *EoPFN_GlGetIntegerv )( unsigned pname, int *data );
typedef void		( *EoPFN_GlBlitFramebuffer )( int srcX0, int srcY0, int srcX1, int srcY1, int dstX0, int dstY0, int dstX1, int dstY1, unsigned mask, unsigned filter );

//-----------------------------------------------------------------------------
// state (static storage: starts zeroed)
//-----------------------------------------------------------------------------
const int		kEoFenceRing	= 16;
const int		kEoMaxSlots		= 8;
const int		kEoMaxBuffers	= 256;
const int		kEoMaxTextures	= 512;

struct EoFence
{
	uint32_t	frame;
	int			fd;
	void		*glSync;
	bool		valid;
	bool		done;
};

struct EoFrameRec
{
	uint64_t	id;
	int64_t		swapNs;
	bool		used;
	bool		done;
	uint32_t	frame;
};

struct EoState
{
	// --- resolved once
	bool		resolved;
	bool		initDone;
	bool		active;
	int			initTries;
	void		*libEgl;

	EoPFN_GetProcAddress			pfnGetProcAddress;
	EoPFN_GetCurrentDisplay			pfnGetCurrentDisplay;
	EoPFN_GetCurrentContext			pfnGetCurrentContext;
	EoPFN_GetCurrentSurface			pfnGetCurrentSurface;
	EoPFN_QueryString				pfnQueryString;
	EoPFN_SurfaceAttrib				pfnSurfaceAttrib;
	EoPFN_QuerySurface				pfnQuerySurface;
	EoPFN_QueryContext				pfnQueryContext;
	EoPFN_GetConfigAttrib			pfnGetConfigAttrib;
	EoPFN_ChooseConfig				pfnChooseConfig;
	EoPFN_CreateWindowSurface		pfnCreateWindowSurface;
	EoPFN_DestroySurface			pfnDestroySurface;
	EoPFN_MakeCurrent				pfnMakeCurrent;
	EoPFN_SwapBuffers				pfnSwapBuffers;
	EoPFN_EglGetError				pfnEglGetError;

	EoPFN_PresentationTime			pfnPresentationTime;
	EoPFN_GetNextFrameId			pfnGetNextFrameId;
	EoPFN_GetCompositorTiming		pfnGetCompositorTiming;
	EoPFN_GetFrameTimestamps		pfnGetFrameTimestamps;
	EoPFN_GetFrameTimestampSupported pfnGetFrameTimestampSupported;
	EoPFN_CreateSync				pfnCreateSync;
	EoPFN_DestroySync				pfnDestroySync;
	EoPFN_DupNativeFenceFD			pfnDupNativeFence;
	EoPFN_GetNativeClientBuffer		pfnGetNativeClientBuffer;
	EoPFN_CreateImage				pfnCreateImage;
	EoPFN_DestroyImage				pfnDestroyImage;

	EoPFN_AhbAllocate				pfnAhbAllocate;
	EoPFN_AhbRelease				pfnAhbRelease;
	EoPFN_AhbDescribe				pfnAhbDescribe;
	EoPFN_AhbLock					pfnAhbLock;
	EoPFN_AhbUnlock					pfnAhbUnlock;

	EoPFN_GlGetError				pfnGlGetError;
	EoPFN_GlGenBuffers				pfnGlGenBuffers;
	EoPFN_GlBindBuffer				pfnGlBindBuffer;
	EoPFN_GlDeleteBuffers			pfnGlDeleteBuffers;
	EoPFN_GlMapBufferRange			pfnGlMapBufferRange;
	EoPFN_GlBufferStorageExternal	pfnGlBufferStorageExternal;
	EoPFN_GlEGLImageTargetTexture2D	pfnGlEGLImageTargetTexture2D;
	EoPFN_GlTexParameteri			pfnGlTexParameteri;
	EoPFN_GlFenceSync				pfnGlFenceSync;
	EoPFN_GlClientWaitSync			pfnGlClientWaitSync;
	EoPFN_GlDeleteSync				pfnGlDeleteSync;
	EoPFN_GlFlush					pfnGlFlush;
	EoPFN_GlIsEnabled				pfnGlIsEnabled;
	EoPFN_GlEnable					pfnGlEnable;
	EoPFN_GlDisable					pfnGlDisable;
	EoPFN_GlBindFramebuffer			pfnGlBindFramebuffer;
	EoPFN_GlGetIntegerv				pfnGlGetIntegerv;
	EoPFN_GlBlitFramebuffer			pfnGlBlitFramebuffer;

	// --- what the driver offers
	bool		extPresentTime, extFrameTs, extNativeFence, extGetNativeClientBuffer, extImageNativeBuffer;
	bool		extFrontBuffer, extRecordable, extMutableRenderBuffer, extFenceSync, extImageBase, extImageColorspace;
	bool		glEglImage, glExternalBuffer;
	bool		ahbApi;
	bool		ctxConfigRecordable;
	bool		nativeFenceBroken;
	int			nativeFenceDupFails;

	// --- the context that was current at EglOpt_Init
	void		*dpy;
	void		*ctx;
	void		*surface;						// the draw surface seen at the last swap

	// --- frame bookkeeping
	uint32_t	frame;							// index of the frame that is being built (it is swapped by the next EglOpt_AfterSwap)
	void		*pendingSync;
	void		*pendingGlSync;
	EoFence		fence[ kEoFenceRing ];

	// --- timestamps / pacing
	bool		tsEnabled;
	int			tsNameCount;
	int			tsNames[ 4 ];
	EoFrameRec	rec[ kEoFenceRing ];
	int64_t		lastDisplayPresent;
	int64_t		refreshNs;						// composite interval reported by the compositor (0 = unknown)
	int64_t		paceNext;						// next ideal presentation time (0 = not pacing)
	int			errStreak;
	bool		presentTimeBroken;
	bool		frontActive, frontTried;
	uint32_t	statsLastSec;
	int64_t		statsNextNs;

	// --- statistics
	uint32_t	stFrames, stTsFrames, stMissed, stIvCount, stFenceWaits, stPresentReq;
	double		stGpuSum, stGpuMax, stLatSum, stLatMax, stIvSum, stFenceWaitSum, stFenceWaitMax;
	uint32_t	stBufRenames, stBufFallbacks, stBufSlotsMade, stTexLocks;

	// --- AHardwareBuffer budget
	uint64_t	ahbBytes;
	int			nAhbBuffers, nAhbTextures;
	bool		bufDisabled, texDisabled;
};

EoState		s_eo;

//-----------------------------------------------------------------------------
// small helpers
//-----------------------------------------------------------------------------
int64_t EoNowNs()
{
	struct timespec ts;
	clock_gettime( CLOCK_MONOTONIC, &ts );
	return (int64_t)ts.tv_sec * 1000000000ll + (int64_t)ts.tv_nsec;
}

bool EoHasToken( const char *list, const char *token )
{
	if ( !list || !token || !token[0] )
		return false;
	const size_t len = strlen( token );
	const char *p = list;
	while ( ( p = strstr( p, token ) ) != NULL )
	{
		const bool bStart = ( p == list ) || ( p[-1] == ' ' );
		const bool bEnd = ( p[len] == ' ' ) || ( p[len] == '\0' );
		if ( bStart && bEnd )
			return true;
		p += len;
	}
	return false;
}

// Core entry points: the libraries are already loaded by the process (SDL), so dlsym( MALIOPT_RTLD_DEFAULT ) finds them. The
// libEGL handle is the fallback for a process where libEGL was loaded privately.
void *EoSym( const char *name )
{
	void *p = dlsym( MALIOPT_RTLD_DEFAULT, name );
	if ( !p )
	{
		if ( !s_eo.libEgl )
			s_eo.libEgl = dlopen( "libEGL.so", RTLD_NOW );
		if ( s_eo.libEgl )
			p = dlsym( s_eo.libEgl, name );
	}
	return p;
}

// Extension entry points: eglGetProcAddress is the proper way, dlsym the fallback.
void *EoExtProc( const char *name )
{
	void *p = s_eo.pfnGetProcAddress ? s_eo.pfnGetProcAddress( name ) : NULL;
	if ( !p )
		p = EoSym( name );
	return p;
}

// GL entry points: dlsym first (same order as the rest of the module), eglGetProcAddress as the fallback.
void *EoGlProc( const char *name )
{
	void *p = dlsym( MALIOPT_RTLD_DEFAULT, name );
	if ( !p )
		p = EoExtProc( name );
	return p;
}

void EoClearEglError()
{
	if ( s_eo.pfnEglGetError )
		s_eo.pfnEglGetError();
}

void EoClearGlError()
{
	if ( s_eo.pfnGlGetError )
	{
		for ( int i = 0; i < 8 && s_eo.pfnGlGetError() != 0; ++i ) {}
	}
}

// a GL error here means the driver refused what we asked for
bool EoGlFailed()
{
	if ( !s_eo.pfnGlGetError )
		return false;
	bool bFailed = false;
	for ( int i = 0; i < 8; ++i )
	{
		if ( s_eo.pfnGlGetError() == 0 )
			break;
		bFailed = true;
	}
	return bFailed;
}

void EoNoteEglFailure( const char *what )
{
	if ( s_eo.errStreak < 1000 )
		s_eo.errStreak++;
	const int err = s_eo.pfnEglGetError ? s_eo.pfnEglGetError() : 0;
	static int s_nLogged = 0;
	if ( s_nLogged < 12 )
	{
		s_nLogged++;
		Warning( "EGL layer: %s failed (EGL error 0x%X)\n", what, (unsigned)err );
	}
}

//-----------------------------------------------------------------------------
// 1. resolve the entry points, detect the extensions
//-----------------------------------------------------------------------------
bool EoResolveCore()
{
	if ( s_eo.resolved )
		return s_eo.pfnGetCurrentDisplay != NULL;
	s_eo.resolved = true;

	s_eo.pfnGetProcAddress		= (EoPFN_GetProcAddress)EoSym( "eglGetProcAddress" );
	s_eo.pfnGetCurrentDisplay	= (EoPFN_GetCurrentDisplay)EoSym( "eglGetCurrentDisplay" );
	s_eo.pfnGetCurrentContext	= (EoPFN_GetCurrentContext)EoSym( "eglGetCurrentContext" );
	s_eo.pfnGetCurrentSurface	= (EoPFN_GetCurrentSurface)EoSym( "eglGetCurrentSurface" );
	s_eo.pfnQueryString			= (EoPFN_QueryString)EoSym( "eglQueryString" );
	s_eo.pfnSurfaceAttrib		= (EoPFN_SurfaceAttrib)EoSym( "eglSurfaceAttrib" );
	s_eo.pfnQuerySurface		= (EoPFN_QuerySurface)EoSym( "eglQuerySurface" );
	s_eo.pfnQueryContext		= (EoPFN_QueryContext)EoSym( "eglQueryContext" );
	s_eo.pfnGetConfigAttrib		= (EoPFN_GetConfigAttrib)EoSym( "eglGetConfigAttrib" );
	s_eo.pfnChooseConfig		= (EoPFN_ChooseConfig)EoSym( "eglChooseConfig" );
	s_eo.pfnCreateWindowSurface	= (EoPFN_CreateWindowSurface)EoSym( "eglCreateWindowSurface" );
	s_eo.pfnDestroySurface		= (EoPFN_DestroySurface)EoSym( "eglDestroySurface" );
	s_eo.pfnMakeCurrent			= (EoPFN_MakeCurrent)EoSym( "eglMakeCurrent" );
	s_eo.pfnSwapBuffers			= (EoPFN_SwapBuffers)EoSym( "eglSwapBuffers" );
	s_eo.pfnEglGetError			= (EoPFN_EglGetError)EoSym( "eglGetError" );

	s_eo.pfnGlGetError			= (EoPFN_GlGetError)EoGlProc( "glGetError" );
	s_eo.pfnGlGenBuffers		= (EoPFN_GlGenBuffers)EoGlProc( "glGenBuffers" );
	s_eo.pfnGlBindBuffer		= (EoPFN_GlBindBuffer)EoGlProc( "glBindBuffer" );
	s_eo.pfnGlDeleteBuffers		= (EoPFN_GlDeleteBuffers)EoGlProc( "glDeleteBuffers" );
	s_eo.pfnGlMapBufferRange	= (EoPFN_GlMapBufferRange)EoGlProc( "glMapBufferRange" );
	s_eo.pfnGlTexParameteri		= (EoPFN_GlTexParameteri)EoGlProc( "glTexParameteri" );
	s_eo.pfnGlFenceSync			= (EoPFN_GlFenceSync)EoGlProc( "glFenceSync" );
	s_eo.pfnGlClientWaitSync	= (EoPFN_GlClientWaitSync)EoGlProc( "glClientWaitSync" );
	s_eo.pfnGlDeleteSync		= (EoPFN_GlDeleteSync)EoGlProc( "glDeleteSync" );
	s_eo.pfnGlFlush				= (EoPFN_GlFlush)EoGlProc( "glFlush" );
	s_eo.pfnGlIsEnabled			= (EoPFN_GlIsEnabled)EoGlProc( "glIsEnabled" );
	s_eo.pfnGlEnable			= (EoPFN_GlEnable)EoGlProc( "glEnable" );
	s_eo.pfnGlDisable			= (EoPFN_GlDisable)EoGlProc( "glDisable" );
	s_eo.pfnGlBindFramebuffer	= (EoPFN_GlBindFramebuffer)EoGlProc( "glBindFramebuffer" );
	s_eo.pfnGlGetIntegerv		= (EoPFN_GlGetIntegerv)EoGlProc( "glGetIntegerv" );
	s_eo.pfnGlBlitFramebuffer	= (EoPFN_GlBlitFramebuffer)EoGlProc( "glBlitFramebuffer" );

	return s_eo.pfnGetCurrentDisplay != NULL && s_eo.pfnQueryString != NULL && s_eo.pfnEglGetError != NULL;
}

void EoResolveAhb()
{
	void *lib = dlopen( "libnativewindow.so", RTLD_NOW );
	if ( !lib )
		lib = dlopen( "libandroid.so", RTLD_NOW );
	if ( !lib )
		return;

	s_eo.pfnAhbAllocate	= (EoPFN_AhbAllocate)dlsym( lib, "AHardwareBuffer_allocate" );
	s_eo.pfnAhbRelease	= (EoPFN_AhbRelease)dlsym( lib, "AHardwareBuffer_release" );
	s_eo.pfnAhbDescribe	= (EoPFN_AhbDescribe)dlsym( lib, "AHardwareBuffer_describe" );
	s_eo.pfnAhbLock		= (EoPFN_AhbLock)dlsym( lib, "AHardwareBuffer_lock" );
	s_eo.pfnAhbUnlock	= (EoPFN_AhbUnlock)dlsym( lib, "AHardwareBuffer_unlock" );
	s_eo.ahbApi = s_eo.pfnAhbAllocate && s_eo.pfnAhbRelease && s_eo.pfnAhbDescribe && s_eo.pfnAhbLock && s_eo.pfnAhbUnlock;
}

bool EoFenceAvailable()
{
	return ( s_eo.extNativeFence && !s_eo.nativeFenceBroken && gl_egl_native_fence.GetBool() ) || ( s_eo.pfnGlFenceSync && s_eo.pfnGlClientWaitSync && s_eo.pfnGlDeleteSync );
}

void EoPrintCaps( void )
{
	Msg( "EGL layer: display %p, extensions: native_fence_sync %s, presentation_time %s, get_frame_timestamps %s, front_buffer_auto_refresh %s, "
		"get_native_client_buffer %s, image_native_buffer %s, recordable %s (context config %s)\n",
		s_eo.dpy,
		s_eo.extNativeFence ? "yes" : "no", s_eo.extPresentTime ? "yes" : "no", s_eo.extFrameTs ? "yes" : "no", s_eo.extFrontBuffer ? "yes" : "no",
		s_eo.extGetNativeClientBuffer ? "yes" : "no", s_eo.extImageNativeBuffer ? "yes" : "no", s_eo.extRecordable ? "yes" : "no",
		s_eo.ctxConfigRecordable ? "recordable" : "not recordable" );
	Msg( "EGL layer: AHardwareBuffer API %s, GL_OES_EGL_image %s, GL_EXT_external_buffer %s, EGL_EXT_image_gl_colorspace %s -> dynamic buffers %s, dynamic textures %s\n",
		s_eo.ahbApi ? "yes" : "no", s_eo.glEglImage ? "yes" : "no", s_eo.glExternalBuffer ? "yes" : "no", s_eo.extImageColorspace ? "yes" : "no",
		( !s_eo.bufDisabled ) ? "AHardwareBuffer" : "off", ( !s_eo.texDisabled ) ? "AHardwareBuffer" : "off" );
}

} // anonymous namespace (the state above is shared with the sections below, which reopen it)

//-----------------------------------------------------------------------------
// 2. per frame fence (EGL_ANDROID_native_fence_sync)
//
// BeforeSwap inserts a fence behind everything this frame submitted, AfterSwap (the swap has flushed it) turns it into a
// sync_file descriptor. poll() on that descriptor says, without any GL call, whether the GPU has finished the frame.
// Users: the AHardwareBuffer dynamic buffer ring (a slot is reused only after the frame that last drew from it is done),
// the optional in-flight frame limiter (gl_egl_max_frames) and the optional texture write wait.
//-----------------------------------------------------------------------------
namespace
{

void EoFenceClear( EoFence &f )
{
	if ( f.valid && f.fd >= 0 )
		close( f.fd );
	if ( f.valid && f.glSync && s_eo.pfnGlDeleteSync )
		s_eo.pfnGlDeleteSync( f.glSync );
	f.fd = -1;
	f.glSync = NULL;
	f.valid = false;
	f.done = false;
}

// timeoutMs 0 = just look. true = the GPU has passed the fence.
bool EoFencePoll( EoFence &f, int timeoutMs )
{
	if ( f.done )
		return true;
	if ( !f.valid )
		return false;

	bool bSignaled = false;
	if ( f.fd >= 0 )
	{
		struct pollfd p;
		p.fd = f.fd;
		p.events = POLLIN;
		p.revents = 0;
		int r;
		do
		{
			r = poll( &p, 1, timeoutMs );
		} while ( r < 0 && errno == EINTR );
		bSignaled = ( r > 0 );		// POLLIN = signaled; an error / hangup also means it is no longer pending
	}
	else if ( f.glSync && s_eo.pfnGlClientWaitSync )
	{
		const unsigned r = s_eo.pfnGlClientWaitSync( f.glSync, timeoutMs > 0 ? 1u : 0u, (uint64_t)timeoutMs * 1000000ull );		// 1 = GL_SYNC_FLUSH_COMMANDS_BIT
		bSignaled = ( r == EO_GL_ALREADY_SIGNALED || r == EO_GL_CONDITION_SATISFIED );
	}

	if ( bSignaled )
	{
		f.done = true;
		if ( f.fd >= 0 )
		{
			close( f.fd );
			f.fd = -1;
		}
		if ( f.glSync && s_eo.pfnGlDeleteSync )
		{
			s_eo.pfnGlDeleteSync( f.glSync );
			f.glSync = NULL;
		}
	}
	return bSignaled;
}

// has the GPU finished frame 'frame'? (frames that are 16 or more swaps old cannot still be in flight: the swap blocks first)
bool EoFenceSignaled( uint32_t frame )
{
	if ( frame + (uint32_t)kEoFenceRing <= s_eo.frame )
		return true;
	if ( frame >= s_eo.frame )
		return false;		// the frame that is still being built
	EoFence &f = s_eo.fence[ frame & ( kEoFenceRing - 1 ) ];
	if ( !f.valid || f.frame != frame )
		return false;
	return EoFencePoll( f, 0 );
}

bool EoFenceWait( uint32_t frame, int timeoutMs )
{
	if ( EoFenceSignaled( frame ) )
		return true;
	if ( frame >= s_eo.frame )
		return false;
	EoFence &f = s_eo.fence[ frame & ( kEoFenceRing - 1 ) ];
	if ( !f.valid || f.frame != frame )
		return false;

	const int64_t t0 = EoNowNs();
	const bool bOk = EoFencePoll( f, timeoutMs );
	const double ms = (double)( EoNowNs() - t0 ) * 1.0e-6;
	s_eo.stFenceWaits++;
	s_eo.stFenceWaitSum += ms;
	if ( ms > s_eo.stFenceWaitMax )
		s_eo.stFenceWaitMax = ms;
	return bOk;
}

bool EoFenceWanted()
{
	return s_eo.nAhbBuffers > 0 || gl_egl_max_frames.GetInt() > 0 || ( s_eo.nAhbTextures > 0 && gl_egl_ahb_tex_wait.GetBool() );
}

// before the swap: put the fence behind this frame's commands
void EoFenceInsert( void *dpy )
{
	s_eo.pendingSync = NULL;
	s_eo.pendingGlSync = NULL;
	if ( !EoFenceWanted() )
		return;

	if ( s_eo.extNativeFence && !s_eo.nativeFenceBroken && gl_egl_native_fence.GetBool() )
	{
		const int attribs[] = { EO_EGL_SYNC_NATIVE_FENCE_FD_ANDROID, EO_EGL_NO_NATIVE_FENCE_FD_ANDROID, EO_EGL_NONE };
		s_eo.pendingSync = s_eo.pfnCreateSync( dpy, (unsigned)EO_EGL_SYNC_NATIVE_FENCE_ANDROID, attribs );
		if ( !s_eo.pendingSync )
			EoNoteEglFailure( "eglCreateSyncKHR(native fence)" );
	}
	if ( !s_eo.pendingSync && s_eo.pfnGlFenceSync )
		s_eo.pendingGlSync = s_eo.pfnGlFenceSync( EO_GL_SYNC_GPU_COMMANDS_COMPLETE, 0 );
}

// after the swap: the fence has been flushed, so the descriptor can be taken now
void EoFenceCollect( void *dpy )
{
	EoFence &f = s_eo.fence[ s_eo.frame & ( kEoFenceRing - 1 ) ];
	EoFenceClear( f );
	f.frame = s_eo.frame;

	if ( s_eo.pendingSync )
	{
		// the descriptor exists only once the fence has been flushed to the kernel: the swap normally did that, this makes sure
		// (nothing is pending after a swap, so it costs nothing)
		if ( s_eo.pfnGlFlush )
			s_eo.pfnGlFlush();
		const int fd = s_eo.pfnDupNativeFence( dpy, s_eo.pendingSync );
		s_eo.pfnDestroySync( dpy, s_eo.pendingSync );
		s_eo.pendingSync = NULL;
		if ( fd >= 0 )
		{
			f.fd = fd;
			f.valid = true;
			s_eo.nativeFenceDupFails = 0;
		}
		else
		{
			EoClearEglError();
			if ( ++s_eo.nativeFenceDupFails >= 3 && !s_eo.nativeFenceBroken )
			{
				s_eo.nativeFenceBroken = true;		// this driver cannot export the fence: use GL sync objects from now on
				Warning( "EGL layer: eglDupNativeFenceFDANDROID keeps failing, using GL sync objects for the frame fence instead\n" );
			}
		}
	}
	else if ( s_eo.pendingGlSync )
	{
		f.glSync = s_eo.pendingGlSync;
		f.fd = -1;
		f.valid = true;
		s_eo.pendingGlSync = NULL;
	}
}

// before the swap: do not let the CPU run more than gl_egl_max_frames frames ahead of the GPU
void EoFenceLimit()
{
	const int nMax = gl_egl_max_frames.GetInt();
	if ( nMax <= 0 || s_eo.frame < (uint32_t)nMax )
		return;
	EoFenceWait( s_eo.frame - (uint32_t)nMax, 40 );
}

//-----------------------------------------------------------------------------
// 3. frame timestamps (EGL_ANDROID_get_frame_timestamps) and presentation time (EGL_ANDROID_presentation_time)
//-----------------------------------------------------------------------------
bool EoVsyncOn()
{
	static ConVarRef *s_pRef = NULL;
	if ( !s_pRef )
		s_pRef = new ConVarRef( "mat_vsync", true );
	return s_pRef->IsValid() && s_pRef->GetInt() != 0;
}

float EoFpsMax()
{
	static ConVarRef *s_pRef = NULL;
	if ( !s_pRef )
		s_pRef = new ConVarRef( "fps_max", true );
	return s_pRef->IsValid() ? s_pRef->GetFloat() : 0.0f;
}

void EoTimestampsSetup( void *dpy, void *surf )
{
	s_eo.tsEnabled = false;
	s_eo.tsNameCount = 0;
	memset( s_eo.rec, 0, sizeof( s_eo.rec ) );
	s_eo.lastDisplayPresent = 0;
	s_eo.refreshNs = 0;

	if ( !s_eo.extFrameTs || !gl_egl_timestamps.GetBool() || !s_eo.pfnSurfaceAttrib )
		return;
	if ( !s_eo.pfnSurfaceAttrib( dpy, surf, EO_EGL_TIMESTAMPS_ANDROID, 1 ) )
	{
		EoNoteEglFailure( "eglSurfaceAttrib(EGL_TIMESTAMPS_ANDROID)" );
		return;
	}

	// which of the timestamps does this device record? (index 0 = requested present, 1 = rendering complete, 2 = display present)
	static const int wanted[ 3 ] = { EO_EGL_REQUESTED_PRESENT_TIME_ANDROID, EO_EGL_RENDERING_COMPLETE_TIME_ANDROID, EO_EGL_DISPLAY_PRESENT_TIME_ANDROID };
	for ( int i = 0; i < 3; ++i )
	{
		if ( s_eo.pfnGetFrameTimestampSupported && !s_eo.pfnGetFrameTimestampSupported( dpy, surf, wanted[ i ] ) )
			continue;
		s_eo.tsNames[ s_eo.tsNameCount++ ] = wanted[ i ];
	}
	s_eo.tsEnabled = s_eo.tsNameCount > 0;
}

// the compositor's own numbers: when the next composition starts and how long a refresh is
void EoCompositorTiming( void *dpy, void *surf, int64_t *pDeadline )
{
	*pDeadline = 0;
	if ( !s_eo.tsEnabled || !s_eo.pfnGetCompositorTiming )
		return;
	static const int names[ 3 ] = { EO_EGL_COMPOSITE_DEADLINE_ANDROID, EO_EGL_COMPOSITE_INTERVAL_ANDROID, EO_EGL_COMPOSITE_TO_PRESENT_LATENCY_ANDROID };
	int64_t v[ 3 ] = { 0, 0, 0 };
	if ( !s_eo.pfnGetCompositorTiming( dpy, surf, 3, names, v ) )
	{
		EoClearEglError();
		return;
	}
	if ( v[ 0 ] > 0 )
		*pDeadline = v[ 0 ];
	if ( v[ 1 ] > 1000000ll && v[ 1 ] < 100000000ll )		// 1 ms .. 100 ms
		s_eo.refreshNs = v[ 1 ];
}

// Even frame spacing for a frame rate cap that is below the refresh rate. A frame that is late is never held back
// (it keeps the earliest possible time), a frame that is early waits for its slot on the compositor grid.
void EoPace( void *dpy, void *surf, int64_t now )
{
	if ( !s_eo.extPresentTime || s_eo.presentTimeBroken || s_eo.frontActive || !gl_egl_present_time.GetBool() )
	{
		s_eo.paceNext = 0;
		return;
	}
	const float fps = EoFpsMax();
	if ( fps < 20.0f || fps > 500.0f || EoVsyncOn() )		// 0 = uncapped: nothing to space out; vsync: the swap itself waits
	{
		s_eo.paceNext = 0;
		return;
	}

	const int64_t interval = (int64_t)( 1.0e9 / (double)fps );
	int64_t deadline = 0;
	EoCompositorTiming( dpy, surf, &deadline );
	const int64_t period = s_eo.refreshNs;
	if ( period > 0 && interval <= period + period / 8 )	// at or above the refresh rate: every composition shows a new frame anyway
	{
		s_eo.paceNext = 0;
		return;
	}

	if ( s_eo.paceNext == 0 || now > s_eo.paceNext + interval )
		s_eo.paceNext = now;								// first frame, or we fell behind: start again from now

	int64_t t = s_eo.paceNext;
	if ( period > 0 && deadline > 0 )
	{
		if ( t > deadline )
		{
			const int64_t k = ( t - deadline + period - 1 ) / period;
			t = deadline + k * period;						// snap onto the compositor grid
		}
		else
		{
			t = now;										// before the next composition: nothing to wait for
		}
	}
	const int64_t horizon = 2 * ( interval > period ? interval : period );
	if ( t - now > horizon )
		t = now;											// never schedule far into the future
	s_eo.paceNext += interval;

	if ( s_eo.pfnPresentationTime( dpy, surf, t ) )
	{
		s_eo.errStreak = 0;
		s_eo.stPresentReq++;
	}
	else
	{
		EoNoteEglFailure( "eglPresentationTimeANDROID" );
		if ( s_eo.errStreak > 20 )
		{
			s_eo.presentTimeBroken = true;
			Warning( "EGL layer: eglPresentationTimeANDROID keeps failing, presentation time pacing is switched off\n" );
		}
	}
}

// before the swap: remember which frame id this swap will get
void EoTimestampsBegin( void *dpy, void *surf, int64_t now )
{
	EoFrameRec &r = s_eo.rec[ s_eo.frame & ( kEoFenceRing - 1 ) ];
	r.used = false;
	r.done = false;
	if ( !s_eo.tsEnabled || !s_eo.pfnGetNextFrameId )
		return;
	uint64_t id = 0;
	if ( s_eo.pfnGetNextFrameId( dpy, surf, &id ) )
	{
		r.id = id;
		r.swapNs = now;
		r.frame = s_eo.frame;
		r.used = true;
	}
	else
	{
		EoClearEglError();
	}
}

// after the swap: collect the timestamps of frames that are a few swaps old
void EoTimestampsPoll( void *dpy, void *surf )
{
	if ( !s_eo.tsEnabled || !s_eo.pfnGetFrameTimestamps )
		return;

	int nQueries = 0;
	for ( uint32_t d = 3; d <= 8 && nQueries < 2; ++d )
	{
		if ( s_eo.frame < d )
			break;
		const uint32_t frame = s_eo.frame - d;
		EoFrameRec &r = s_eo.rec[ frame & ( kEoFenceRing - 1 ) ];
		if ( !r.used || r.done || r.frame != frame )
			continue;

		nQueries++;
		int64_t v[ 4 ] = { 0, 0, 0, 0 };
		if ( !s_eo.pfnGetFrameTimestamps( dpy, surf, r.id, s_eo.tsNameCount, s_eo.tsNames, v ) )
		{
			EoClearEglError();
			r.done = true;
			continue;
		}

		bool bPending = false;
		for ( int i = 0; i < s_eo.tsNameCount; ++i )
		{
			if ( v[ i ] == EO_EGL_TIMESTAMP_PENDING_ANDROID )
				bPending = true;
		}
		if ( bPending && d < 8 )
			continue;						// not all there yet, ask again after the next swap
		r.done = true;

		int64_t complete = 0, present = 0;
		for ( int i = 0; i < s_eo.tsNameCount; ++i )
		{
			if ( v[ i ] <= 0 )
				continue;					// invalid (the frame was never displayed) or pending
			if ( s_eo.tsNames[ i ] == EO_EGL_RENDERING_COMPLETE_TIME_ANDROID )
				complete = v[ i ];
			else if ( s_eo.tsNames[ i ] == EO_EGL_DISPLAY_PRESENT_TIME_ANDROID )
				present = v[ i ];
		}

		s_eo.stTsFrames++;
		if ( complete > 0 )
		{
			double ms = (double)( complete - r.swapNs ) * 1.0e-6;
			if ( ms < 0.0 )
				ms = 0.0;
			s_eo.stGpuSum += ms;
			if ( ms > s_eo.stGpuMax )
				s_eo.stGpuMax = ms;
		}
		if ( present > 0 )
		{
			double ms = (double)( present - r.swapNs ) * 1.0e-6;
			if ( ms < 0.0 )
				ms = 0.0;
			s_eo.stLatSum += ms;
			if ( ms > s_eo.stLatMax )
				s_eo.stLatMax = ms;
			if ( s_eo.lastDisplayPresent > 0 && present > s_eo.lastDisplayPresent )
			{
				const int64_t iv = present - s_eo.lastDisplayPresent;
				s_eo.stIvSum += (double)iv * 1.0e-6;
				s_eo.stIvCount++;
				if ( s_eo.refreshNs > 0 && iv > s_eo.refreshNs + s_eo.refreshNs / 2 )
					s_eo.stMissed++;
			}
			if ( present > s_eo.lastDisplayPresent )
				s_eo.lastDisplayPresent = present;
		}
	}
}

//-----------------------------------------------------------------------------
// 4. front buffer mode (EGL_KHR_mutable_render_buffer + EGL_ANDROID_front_buffer_auto_refresh). Off by default.
//-----------------------------------------------------------------------------
void EoFrontBufferUpdate( void *dpy, void *surf )
{
	const bool bWant = gl_egl_front_buffer.GetBool() && s_eo.extFrontBuffer && s_eo.extMutableRenderBuffer && s_eo.pfnSurfaceAttrib;
	if ( bWant == s_eo.frontActive )
	{
		if ( !gl_egl_front_buffer.GetBool() )
			s_eo.frontTried = false;		// allow a new try the next time it is switched on
		return;
	}

	if ( bWant )
	{
		if ( s_eo.frontTried )
			return;
		s_eo.frontTried = true;
		if ( !s_eo.pfnSurfaceAttrib( dpy, surf, EO_EGL_RENDER_BUFFER, EO_EGL_SINGLE_BUFFER ) )
		{
			EoNoteEglFailure( "eglSurfaceAttrib(EGL_RENDER_BUFFER, EGL_SINGLE_BUFFER)" );
			Warning( "EGL layer: the window surface cannot switch to single buffer mode (its config has no EGL_MUTABLE_RENDER_BUFFER_BIT_KHR)\n" );
			return;
		}
		if ( !s_eo.pfnSurfaceAttrib( dpy, surf, EO_EGL_FRONT_BUFFER_AUTO_REFRESH_ANDROID, 1 ) )
			EoNoteEglFailure( "eglSurfaceAttrib(EGL_FRONT_BUFFER_AUTO_REFRESH_ANDROID)" );
		int mode = 0;
		if ( s_eo.pfnQuerySurface && s_eo.pfnQuerySurface( dpy, surf, EO_EGL_RENDER_BUFFER, &mode ) && mode != EO_EGL_SINGLE_BUFFER )
		{
			s_eo.pfnSurfaceAttrib( dpy, surf, EO_EGL_RENDER_BUFFER, EO_EGL_BACK_BUFFER );
			EoClearEglError();
			Warning( "EGL layer: the driver ignored the single buffer request\n" );
			return;
		}
		s_eo.frontActive = true;
		Msg( "EGL layer: front buffer mode ON (single buffer + auto refresh)\n" );
	}
	else
	{
		s_eo.pfnSurfaceAttrib( dpy, surf, EO_EGL_FRONT_BUFFER_AUTO_REFRESH_ANDROID, 0 );
		s_eo.pfnSurfaceAttrib( dpy, surf, EO_EGL_RENDER_BUFFER, EO_EGL_BACK_BUFFER );
		EoClearEglError();
		s_eo.frontActive = false;
		Msg( "EGL layer: front buffer mode OFF\n" );
	}
}

} // anonymous namespace

//-----------------------------------------------------------------------------
// 5. AHardwareBuffer storage for dynamic vertex / index buffers
//
// EGL_ANDROID_get_native_client_buffer turns an AHardwareBuffer (format BLOB) into an EGLClientBuffer, GL_EXT_external_buffer
// makes a GL buffer object out of it (glBufferStorageExternalEXT) and the buffer is mapped once, persistently and coherently.
// The engine writes the vertices straight into memory the GPU reads: the driver no longer has to copy client side arrays on
// every draw call.
//
// A GPU that reads later than the CPU writes needs more than one copy: every buffer owns a small ring of slots. A DISCARD
// lock (or a plain lock of the whole buffer) moves on to a slot that the GPU is done with, which the per frame fence tells.
// NOOVERWRITE locks stay in the current slot (the engine promises not to touch what the GPU may still read). When no slot
// is free the buffer goes back to its CPU copy (the old "pseudo buffer" path) until its next DISCARD.
//-----------------------------------------------------------------------------
namespace
{

struct EoBufSlot
{
	void		*ahb;
	unsigned	glName;
	char		*cpu;
	int			state;				// 0 = idle, 1 = current, 2 = retired (the GPU may still be reading it)
	uint32_t	retireFrame;
};

struct EoBuf
{
	const void	*owner;
	unsigned	target;
	unsigned	size;
	void		*pseudoBase;
	int			nSlots;
	int			cur;
	bool		pseudoMode;
	EoBufSlot	slot[ kEoMaxSlots ];
};

EoBuf		*s_bufs[ kEoMaxBuffers ];
int			s_nBufHigh;
EoBuf		*s_pLastBuf;

EoBuf *EoBufFind( const void *owner )
{
	if ( s_pLastBuf && s_pLastBuf->owner == owner )
		return s_pLastBuf;
	for ( int i = 0; i < s_nBufHigh; ++i )
	{
		if ( s_bufs[ i ] && s_bufs[ i ]->owner == owner )
		{
			s_pLastBuf = s_bufs[ i ];
			return s_bufs[ i ];
		}
	}
	return NULL;
}

enum EoSlotResult { kEoSlotOk = 0, kEoSlotBudget = 1, kEoSlotFailed = 2 };

// buffer binding that was active before we touched the target: put it back so TOGL's own bookkeeping stays right
unsigned EoCurrentBinding( unsigned target )
{
	int prev = 0;
	if ( s_eo.pfnGlGetIntegerv )
		s_eo.pfnGlGetIntegerv( target == 0x8893u ? 0x8895u : 0x8894u, &prev );		// ELEMENT_ARRAY_BUFFER_BINDING / ARRAY_BUFFER_BINDING
	return (unsigned)prev;
}

EoSlotResult EoBufMakeSlot( EoBuf *b )
{
	if ( b->nSlots >= kEoMaxSlots )
		return kEoSlotBudget;
	if ( s_eo.ahbBytes + b->size > (uint64_t)gl_egl_ahb_max_mb.GetInt() * 1024ull * 1024ull )
		return kEoSlotBudget;

	EoAhbDesc d;
	memset( &d, 0, sizeof( d ) );
	d.width = b->size;
	d.height = 1;
	d.layers = 1;
	d.format = EO_AHB_FORMAT_BLOB;
	d.usage = EO_AHB_USAGE_CPU_READ_RARELY | EO_AHB_USAGE_CPU_WRITE_OFTEN | EO_AHB_USAGE_GPU_DATA_BUFFER;

	void *ahb = NULL;
	if ( s_eo.pfnAhbAllocate( &d, &ahb ) != 0 || !ahb )
		return kEoSlotFailed;
	void *client = s_eo.pfnGetNativeClientBuffer( ahb );
	if ( !client )
	{
		s_eo.pfnAhbRelease( ahb );
		return kEoSlotFailed;
	}

	const unsigned prevBinding = EoCurrentBinding( b->target );
	unsigned name = 0;
	EoClearGlError();
	s_eo.pfnGlGenBuffers( 1, &name );
	s_eo.pfnGlBindBuffer( b->target, name );
	s_eo.pfnGlBufferStorageExternal( b->target, 0, (intptr_t)b->size, client,
		EO_GL_MAP_READ_BIT | EO_GL_MAP_WRITE_BIT | EO_GL_MAP_PERSISTENT_BIT | EO_GL_MAP_COHERENT_BIT );
	void *p = NULL;
	if ( !EoGlFailed() )
		p = s_eo.pfnGlMapBufferRange( b->target, 0, (intptr_t)b->size, EO_GL_MAP_WRITE_BIT | EO_GL_MAP_PERSISTENT_BIT | EO_GL_MAP_COHERENT_BIT );
	const bool bBad = ( !p ) || EoGlFailed();
	s_eo.pfnGlBindBuffer( b->target, prevBinding );
	if ( bBad )
	{
		if ( name )
			s_eo.pfnGlDeleteBuffers( 1, &name );
		s_eo.pfnAhbRelease( ahb );
		return kEoSlotFailed;
	}

	EoBufSlot &s = b->slot[ b->nSlots ];
	memset( &s, 0, sizeof( s ) );
	s.ahb = ahb;
	s.glName = name;
	s.cpu = (char *)p;
	b->nSlots++;
	s_eo.ahbBytes += b->size;
	s_eo.stBufSlotsMade++;
	return kEoSlotOk;
}

// a slot (not the current one) that the GPU is finished with; makes a new one when none is
int EoBufAcquire( EoBuf *b )
{
	for ( int i = 0; i < b->nSlots; ++i )
	{
		const EoBufSlot &s = b->slot[ i ];
		if ( s.state == 0 )
			return i;
		if ( s.state == 2 && s.retireFrame != s_eo.frame && EoFenceSignaled( s.retireFrame ) )
			return i;
	}
	if ( EoBufMakeSlot( b ) == kEoSlotOk )
		return b->nSlots - 1;
	return -1;
}

void EoBufFree( EoBuf *b )
{
	for ( int i = 0; i < b->nSlots; ++i )
	{
		EoBufSlot &s = b->slot[ i ];
		if ( s.glName )
			s_eo.pfnGlDeleteBuffers( 1, &s.glName );		// deleting the buffer also unmaps it
		if ( s.ahb )
			s_eo.pfnAhbRelease( s.ahb );
		if ( s_eo.ahbBytes >= b->size )
			s_eo.ahbBytes -= b->size;
	}
}

} // anonymous namespace

bool EglOpt_BufferCreate( const void *pOwner, unsigned int nGLTarget, unsigned int nSize, void *pPseudoBase, unsigned int *pGLName )
{
	EglOpt_Init();
	if ( !s_eo.active || s_eo.bufDisabled || !gl_egl_ahb_buffers.GetBool() )
		return false;
	if ( !pOwner || !pPseudoBase || !pGLName || nSize == 0 )
		return false;
	if ( nSize < (unsigned)gl_egl_ahb_buffer_min_kb.GetInt() * 1024u || nSize > 64u * 1024u * 1024u )
		return false;
	if ( nGLTarget != 0x8892u && nGLTarget != 0x8893u )		// GL_ARRAY_BUFFER / GL_ELEMENT_ARRAY_BUFFER only
		return false;
	if ( !EoFenceAvailable() || EoBufFind( pOwner ) )
		return false;

	int nFree = -1;
	for ( int i = 0; i < kEoMaxBuffers; ++i )
	{
		if ( !s_bufs[ i ] )
		{
			nFree = i;
			break;
		}
	}
	if ( nFree < 0 )
		return false;

	EoBuf *b = (EoBuf *)calloc( 1, sizeof( EoBuf ) );
	if ( !b )
		return false;
	b->owner = pOwner;
	b->target = nGLTarget;
	b->size = nSize;
	b->pseudoBase = pPseudoBase;

	const EoSlotResult first = EoBufMakeSlot( b );
	if ( first != kEoSlotOk )
	{
		free( b );
		if ( first == kEoSlotFailed )
		{
			// the driver does not take it: stop trying for the other buffers
			s_eo.bufDisabled = true;
			Warning( "EGL layer: AHardwareBuffer backed vertex / index buffers are not supported by this driver, using client side arrays\n" );
		}
		return false;
	}

	int nWant = gl_egl_ahb_buffer_slots.GetInt();
	if ( nWant < 2 )
		nWant = 2;
	if ( nWant > kEoMaxSlots )
		nWant = kEoMaxSlots;
	while ( b->nSlots < nWant && EoBufMakeSlot( b ) == kEoSlotOk ) {}
	if ( b->nSlots < 2 )			// a ring of one cannot rename: nothing gained
	{
		EoBufFree( b );
		free( b );
		return false;
	}

	b->slot[ 0 ].state = 1;
	b->cur = 0;
	b->pseudoMode = false;
	s_bufs[ nFree ] = b;
	if ( nFree >= s_nBufHigh )
		s_nBufHigh = nFree + 1;
	s_eo.nAhbBuffers++;
	*pGLName = b->slot[ 0 ].glName;
	return true;
}

bool EglOpt_BufferTracked( const void *pOwner )
{
	return s_nBufHigh > 0 && EoBufFind( pOwner ) != NULL;
}

bool EglOpt_BufferInAhbMode( const void *pOwner )
{
	const EoBuf *b = ( s_nBufHigh > 0 ) ? EoBufFind( pOwner ) : NULL;
	return b != NULL && !b->pseudoMode;
}

int EglOpt_BufferLock( const void *pOwner, bool bDiscard, bool bNoOverwrite, unsigned int nOffset, unsigned int nSize, void **ppBase, unsigned int *pGLName )
{
	EoBuf *b = ( s_nBufHigh > 0 ) ? EoBufFind( pOwner ) : NULL;
	if ( !b )
		return kEglOptBufNone;

	// A plain lock that rewrites the whole buffer is a DISCARD in disguise. A plain partial lock cannot be told apart from
	// NOOVERWRITE here, so it is treated like one (the engine only does this on dynamic buffers it appends to).
	const bool bWhole = ( nOffset == 0 && nSize >= b->size );
	const bool bRename = bDiscard || ( !bNoOverwrite && bWhole );

	if ( bRename )
	{
		const int idx = EoBufAcquire( b );
		if ( !b->pseudoMode )
		{
			// leave the current slot: the GPU may still read what was drawn from it up to now
			EoBufSlot &old = b->slot[ b->cur ];
			old.state = 2;
			old.retireFrame = s_eo.frame;
		}
		if ( idx >= 0 )
		{
			b->cur = idx;
			b->slot[ idx ].state = 1;
			b->pseudoMode = false;
			s_eo.stBufRenames++;
		}
		else
		{
			b->pseudoMode = true;
			s_eo.stBufFallbacks++;
		}
	}

	if ( b->pseudoMode )
	{
		*ppBase = b->pseudoBase;
		*pGLName = 0;
		return kEglOptBufPseudo;
	}
	*ppBase = b->slot[ b->cur ].cpu;
	*pGLName = b->slot[ b->cur ].glName;
	return kEglOptBufAhb;
}

void EglOpt_BufferUnlock( const void *pOwner )
{
	(void)pOwner;
	__sync_synchronize();		// the vertices are in memory before the draw that uses them is issued
}

bool EglOpt_BufferDestroy( const void *pOwner )
{
	if ( s_nBufHigh <= 0 )
		return false;
	EoBuf *b = EoBufFind( pOwner );
	if ( !b )
		return false;

	for ( int i = 0; i < s_nBufHigh; ++i )
	{
		if ( s_bufs[ i ] == b )
		{
			s_bufs[ i ] = NULL;
			break;
		}
	}
	if ( s_pLastBuf == b )
		s_pLastBuf = NULL;
	EoBufFree( b );
	free( b );
	if ( s_eo.nAhbBuffers > 0 )
		s_eo.nAhbBuffers--;
	return true;
}

//-----------------------------------------------------------------------------
// 6. AHardwareBuffer storage for dynamic textures
//
// EGL_ANDROID_get_native_client_buffer + EGL_ANDROID_image_native_buffer: AHardwareBuffer -> EGLClientBuffer -> EGLImage,
// GL_OES_EGL_image: the EGLImage becomes the storage of the GL texture. The texture is RGBA8 and the engine writes BGRA, so
// the sampler swaps red and blue (texture swizzle, free on the GPU). Only dynamic, single level, non sRGB 32 bit textures
// come here: static textures keep the ASTC path, which this code never touches.
//-----------------------------------------------------------------------------
namespace
{

struct EoTex
{
	const void	*owner;
	void		*ahb;
	void		*image;
	unsigned	w, h;
	unsigned	strideBytes;
	int			lockCount;
	char		*cpu;
	uint64_t	bytes;
};

EoTex		*s_texs[ kEoMaxTextures ];
int			s_nTexHigh;
EoTex		*s_pLastTex;

EoTex *EoTexFind( const void *owner )
{
	if ( s_pLastTex && s_pLastTex->owner == owner )
		return s_pLastTex;
	for ( int i = 0; i < s_nTexHigh; ++i )
	{
		if ( s_texs[ i ] && s_texs[ i ]->owner == owner )
		{
			s_pLastTex = s_texs[ i ];
			return s_texs[ i ];
		}
	}
	return NULL;
}

void EoTexResetSwizzle()
{
	s_eo.pfnGlTexParameteri( EO_GL_TEXTURE_2D, EO_GL_TEXTURE_SWIZZLE_R, EO_GL_RED );
	s_eo.pfnGlTexParameteri( EO_GL_TEXTURE_2D, EO_GL_TEXTURE_SWIZZLE_G, EO_GL_GREEN );
	s_eo.pfnGlTexParameteri( EO_GL_TEXTURE_2D, EO_GL_TEXTURE_SWIZZLE_B, EO_GL_BLUE );
	s_eo.pfnGlTexParameteri( EO_GL_TEXTURE_2D, EO_GL_TEXTURE_SWIZZLE_A, EO_GL_ALPHA );
}

} // anonymous namespace

bool EglOpt_TexWanted( unsigned int nWidth, unsigned int nHeight )
{
	EglOpt_Init();
	if ( !s_eo.active || s_eo.texDisabled || !gl_egl_ahb_textures.GetBool() )
		return false;
	const unsigned nMin = (unsigned)gl_egl_ahb_tex_min.GetInt();
	return nWidth >= nMin && nHeight >= nMin && nWidth <= 4096u && nHeight <= 4096u;
}

bool EglOpt_TexCreate( const void *pOwner, unsigned int nWidth, unsigned int nHeight, bool bOpaque, bool bSRGB )
{
	if ( !pOwner || !EglOpt_TexWanted( nWidth, nHeight ) || EoTexFind( pOwner ) )
		return false;
	// With GL_EXT_texture_sRGB_decode TOGL gives every sRGB capable texture the sRGB flag and the sampler switches the decode on
	// and off. That only keeps working when the EGLImage itself is sRGB, which needs EGL_EXT_image_gl_colorspace.
	if ( bSRGB && !s_eo.extImageColorspace )
		return false;

	int nFree = -1;
	for ( int i = 0; i < kEoMaxTextures; ++i )
	{
		if ( !s_texs[ i ] )
		{
			nFree = i;
			break;
		}
	}
	if ( nFree < 0 )
		return false;

	const uint64_t nEstimate = (uint64_t)nWidth * nHeight * 4ull;
	if ( s_eo.ahbBytes + nEstimate > (uint64_t)gl_egl_ahb_max_mb.GetInt() * 1024ull * 1024ull )
		return false;

	EoAhbDesc d;
	memset( &d, 0, sizeof( d ) );
	d.width = nWidth;
	d.height = nHeight;
	d.layers = 1;
	d.format = EO_AHB_FORMAT_R8G8B8A8_UNORM;
	d.usage = EO_AHB_USAGE_GPU_SAMPLED_IMAGE | EO_AHB_USAGE_GPU_COLOR_OUTPUT | EO_AHB_USAGE_CPU_READ_RARELY | EO_AHB_USAGE_CPU_WRITE_OFTEN;

	void *ahb = NULL;
	if ( s_eo.pfnAhbAllocate( &d, &ahb ) != 0 || !ahb )
	{
		// some drivers refuse colour output together with CPU access: a sampled-only texture is enough for the normal use
		d.usage &= ~EO_AHB_USAGE_GPU_COLOR_OUTPUT;
		ahb = NULL;
		if ( s_eo.pfnAhbAllocate( &d, &ahb ) != 0 || !ahb )
			return false;
	}

	EoAhbDesc got;
	memset( &got, 0, sizeof( got ) );
	s_eo.pfnAhbDescribe( ahb, &got );
	const unsigned stridePixels = ( got.stride >= nWidth ) ? got.stride : nWidth;

	// transparent black, like the blank placeholder the normal path uploads
	void *addr = NULL;
	if ( s_eo.pfnAhbLock( ahb, EO_AHB_USAGE_CPU_WRITE_OFTEN, -1, NULL, &addr ) != 0 || !addr )
	{
		s_eo.pfnAhbRelease( ahb );
		return false;
	}
	memset( addr, 0, (size_t)stridePixels * nHeight * 4u );
	s_eo.pfnAhbUnlock( ahb, NULL );

	void *client = s_eo.pfnGetNativeClientBuffer( ahb );
	void *image = NULL;
	if ( client )
	{
		const int attribsLinear[] = { EO_EGL_IMAGE_PRESERVED_KHR, 1, EO_EGL_NONE };
		const int attribsSRGB[] = { EO_EGL_IMAGE_PRESERVED_KHR, 1, EO_EGL_GL_COLORSPACE_KHR, EO_EGL_GL_COLORSPACE_SRGB_KHR, EO_EGL_NONE };
		image = s_eo.pfnCreateImage( s_eo.dpy, NULL, (unsigned)EO_EGL_NATIVE_BUFFER_ANDROID, client, bSRGB ? attribsSRGB : attribsLinear );
		if ( !image )
			EoNoteEglFailure( "eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID)" );
	}
	if ( !image )
	{
		s_eo.pfnAhbRelease( ahb );
		return false;
	}

	// the texture is bound to GL_TEXTURE_2D on the active TMU (the caller just did that)
	EoClearGlError();
	s_eo.pfnGlEGLImageTargetTexture2D( EO_GL_TEXTURE_2D, image );
	bool bOk = !EoGlFailed();
	if ( bOk )
	{
		// D3D A8R8G8B8 is B,G,R,A in memory, the AHardwareBuffer says R,G,B,A: swap red and blue when sampling
		s_eo.pfnGlTexParameteri( EO_GL_TEXTURE_2D, EO_GL_TEXTURE_SWIZZLE_R, EO_GL_BLUE );
		s_eo.pfnGlTexParameteri( EO_GL_TEXTURE_2D, EO_GL_TEXTURE_SWIZZLE_G, EO_GL_GREEN );
		s_eo.pfnGlTexParameteri( EO_GL_TEXTURE_2D, EO_GL_TEXTURE_SWIZZLE_B, EO_GL_RED );
		s_eo.pfnGlTexParameteri( EO_GL_TEXTURE_2D, EO_GL_TEXTURE_SWIZZLE_A, bOpaque ? EO_GL_ONE : EO_GL_ALPHA );
		s_eo.pfnGlTexParameteri( EO_GL_TEXTURE_2D, EO_GL_TEXTURE_BASE_LEVEL, 0 );
		s_eo.pfnGlTexParameteri( EO_GL_TEXTURE_2D, EO_GL_TEXTURE_MAX_LEVEL, 0 );
		bOk = !EoGlFailed();
		if ( !bOk )
			EoTexResetSwizzle();
	}
	if ( !bOk )
	{
		s_eo.pfnDestroyImage( s_eo.dpy, image );
		s_eo.pfnAhbRelease( ahb );
		EoClearEglError();
		return false;
	}

	EoTex *t = (EoTex *)calloc( 1, sizeof( EoTex ) );
	if ( !t )
	{
		s_eo.pfnDestroyImage( s_eo.dpy, image );
		s_eo.pfnAhbRelease( ahb );
		return false;
	}
	t->owner = pOwner;
	t->ahb = ahb;
	t->image = image;
	t->w = nWidth;
	t->h = nHeight;
	t->strideBytes = stridePixels * 4u;
	t->bytes = (uint64_t)t->strideBytes * nHeight;
	s_texs[ nFree ] = t;
	if ( nFree >= s_nTexHigh )
		s_nTexHigh = nFree + 1;
	s_eo.ahbBytes += t->bytes;
	s_eo.nAhbTextures++;
	return true;
}

bool EglOpt_TexTracked( const void *pOwner )
{
	return s_nTexHigh > 0 && EoTexFind( pOwner ) != NULL;
}

char *EglOpt_TexLock( const void *pOwner, bool bReadOnly, int *pPitchBytes )
{
	EoTex *t = ( s_nTexHigh > 0 ) ? EoTexFind( pOwner ) : NULL;
	if ( !t )
		return NULL;

	if ( t->lockCount == 0 )
	{
		// optional: no write while the GPU may still be sampling what the previous frame drew
		if ( !bReadOnly && gl_egl_ahb_tex_wait.GetBool() && s_eo.frame > 0 )
			EoFenceWait( s_eo.frame - 1, 40 );

		void *addr = NULL;
		const uint64_t usage = bReadOnly ? EO_AHB_USAGE_CPU_READ_RARELY : EO_AHB_USAGE_CPU_WRITE_OFTEN;
		if ( s_eo.pfnAhbLock( t->ahb, usage, -1, NULL, &addr ) != 0 || !addr )
			return NULL;
		t->cpu = (char *)addr;
		s_eo.stTexLocks++;
	}
	t->lockCount++;
	*pPitchBytes = (int)t->strideBytes;
	return t->cpu;
}

void EglOpt_TexUnlock( const void *pOwner )
{
	EoTex *t = ( s_nTexHigh > 0 ) ? EoTexFind( pOwner ) : NULL;
	if ( !t || t->lockCount <= 0 )
		return;
	if ( --t->lockCount == 0 )
	{
		s_eo.pfnAhbUnlock( t->ahb, NULL );		// CPU writes are done: this is where the cache is made visible to the GPU
		t->cpu = NULL;
	}
}

bool EglOpt_TexDestroy( const void *pOwner )
{
	if ( s_nTexHigh <= 0 )
		return false;
	EoTex *t = EoTexFind( pOwner );
	if ( !t )
		return false;

	for ( int i = 0; i < s_nTexHigh; ++i )
	{
		if ( s_texs[ i ] == t )
		{
			s_texs[ i ] = NULL;
			break;
		}
	}
	if ( s_pLastTex == t )
		s_pLastTex = NULL;

	if ( t->lockCount > 0 )
		s_eo.pfnAhbUnlock( t->ahb, NULL );
	if ( t->image )
		s_eo.pfnDestroyImage( s_eo.dpy, t->image );
	if ( t->ahb )
		s_eo.pfnAhbRelease( t->ahb );
	if ( s_eo.ahbBytes >= t->bytes )
		s_eo.ahbBytes -= t->bytes;
	if ( s_eo.nAhbTextures > 0 )
		s_eo.nAhbTextures--;
	free( t );
	return true;
}

//-----------------------------------------------------------------------------
// 7. screen recorder (EGL_ANDROID_recordable)
//
// gl_egl_record_start makes an H.264 / MP4 file in ./recordings. The encoder (MediaCodec, through the NDK, found with dlopen)
// gives an input Surface; an EGLConfig with EGL_RECORDABLE_ANDROID that matches the game's own config makes it an EGLSurface;
// every few frames the finished window image is blitted into it and swapped with its presentation time
// (EGL_ANDROID_presentation_time) as the video timestamp. A second thread drains the encoder into the muxer.
// Everything runs on the render thread at the swap, so the GL state is never touched from another thread.
//-----------------------------------------------------------------------------
namespace
{

struct EoCodecInfo
{
	int32_t		offset;
	int32_t		size;
	int64_t		presentationTimeUs;
	uint32_t	flags;
};

typedef void *		( *EoPFN_CodecCreateEncoder )( const char *mime );
typedef int32_t		( *EoPFN_CodecConfigure )( void *codec, const void *format, void *surface, void *crypto, uint32_t flags );
typedef int32_t		( *EoPFN_CodecCreateInputSurface )( void *codec, void **surface );
typedef int32_t		( *EoPFN_CodecSimple )( void *codec );
typedef intptr_t	( *EoPFN_CodecDequeueOutput )( void *codec, EoCodecInfo *info, int64_t timeoutUs );
typedef uint8_t *	( *EoPFN_CodecGetOutputBuffer )( void *codec, size_t idx, size_t *outSize );
typedef void *		( *EoPFN_CodecGetOutputFormat )( void *codec );
typedef int32_t		( *EoPFN_CodecReleaseOutput )( void *codec, size_t idx, bool render );
typedef void *		( *EoPFN_FormatNew )( void );
typedef int32_t		( *EoPFN_FormatDelete )( void *format );
typedef void		( *EoPFN_FormatSetString )( void *format, const char *name, const char *value );
typedef void		( *EoPFN_FormatSetInt32 )( void *format, const char *name, int32_t value );
typedef void *		( *EoPFN_MuxerNew )( int fd, int outputFormat );
typedef intptr_t	( *EoPFN_MuxerAddTrack )( void *muxer, const void *format );
typedef int32_t		( *EoPFN_MuxerSimple )( void *muxer );
typedef int32_t		( *EoPFN_MuxerWrite )( void *muxer, size_t track, const uint8_t *data, const EoCodecInfo *info );
typedef void		( *EoPFN_WindowRelease )( void *window );

struct EoMedia
{
	bool	tried, ok;
	EoPFN_CodecCreateEncoder		codecCreateEncoder;
	EoPFN_CodecConfigure			codecConfigure;
	EoPFN_CodecCreateInputSurface	codecCreateInputSurface;
	EoPFN_CodecSimple				codecStart, codecStop, codecDelete, codecSignalEnd;
	EoPFN_CodecDequeueOutput		codecDequeueOutput;
	EoPFN_CodecGetOutputBuffer		codecGetOutputBuffer;
	EoPFN_CodecGetOutputFormat		codecGetOutputFormat;
	EoPFN_CodecReleaseOutput		codecReleaseOutput;
	EoPFN_FormatNew					formatNew;
	EoPFN_FormatDelete				formatDelete;
	EoPFN_FormatSetString			formatSetString;
	EoPFN_FormatSetInt32			formatSetInt32;
	EoPFN_MuxerNew					muxerNew;
	EoPFN_MuxerAddTrack				muxerAddTrack;
	EoPFN_MuxerSimple				muxerStart, muxerStop, muxerDelete;
	EoPFN_MuxerWrite				muxerWrite;
	EoPFN_WindowRelease				windowRelease;
};

struct EoRec
{
	volatile int	reqStart, reqStop;
	float			reqBitrateMbps;
	bool			active;
	void			*codec, *muxer, *window, *surface;
	int				fd;
	int				track;
	int				muxerStarted;
	int				encW, encH;
	int64_t			startNs, lastFrameNs;
	pthread_t		thread;
	int				threadStarted;
	volatile int	stopThread;
	uint32_t		framesIn, framesOut;
	int				failStreak;
	char			path[ 256 ];
};

EoMedia		s_media;
EoRec		s_rec;

bool EoMediaResolve()
{
	if ( s_media.tried )
		return s_media.ok;
	s_media.tried = true;

	void *lib = dlopen( "libmediandk.so", RTLD_NOW );
	if ( !lib )
		return false;
	void *libAndroid = dlopen( "libandroid.so", RTLD_NOW );

	s_media.codecCreateEncoder		= (EoPFN_CodecCreateEncoder)dlsym( lib, "AMediaCodec_createEncoderByType" );
	s_media.codecConfigure			= (EoPFN_CodecConfigure)dlsym( lib, "AMediaCodec_configure" );
	s_media.codecCreateInputSurface	= (EoPFN_CodecCreateInputSurface)dlsym( lib, "AMediaCodec_createInputSurface" );		// API 26+
	s_media.codecStart				= (EoPFN_CodecSimple)dlsym( lib, "AMediaCodec_start" );
	s_media.codecStop				= (EoPFN_CodecSimple)dlsym( lib, "AMediaCodec_stop" );
	s_media.codecDelete				= (EoPFN_CodecSimple)dlsym( lib, "AMediaCodec_delete" );
	s_media.codecSignalEnd			= (EoPFN_CodecSimple)dlsym( lib, "AMediaCodec_signalEndOfInputStream" );				// API 26+
	s_media.codecDequeueOutput		= (EoPFN_CodecDequeueOutput)dlsym( lib, "AMediaCodec_dequeueOutputBuffer" );
	s_media.codecGetOutputBuffer	= (EoPFN_CodecGetOutputBuffer)dlsym( lib, "AMediaCodec_getOutputBuffer" );
	s_media.codecGetOutputFormat	= (EoPFN_CodecGetOutputFormat)dlsym( lib, "AMediaCodec_getOutputFormat" );
	s_media.codecReleaseOutput		= (EoPFN_CodecReleaseOutput)dlsym( lib, "AMediaCodec_releaseOutputBuffer" );
	s_media.formatNew				= (EoPFN_FormatNew)dlsym( lib, "AMediaFormat_new" );
	s_media.formatDelete			= (EoPFN_FormatDelete)dlsym( lib, "AMediaFormat_delete" );
	s_media.formatSetString			= (EoPFN_FormatSetString)dlsym( lib, "AMediaFormat_setString" );
	s_media.formatSetInt32			= (EoPFN_FormatSetInt32)dlsym( lib, "AMediaFormat_setInt32" );
	s_media.muxerNew				= (EoPFN_MuxerNew)dlsym( lib, "AMediaMuxer_new" );
	s_media.muxerAddTrack			= (EoPFN_MuxerAddTrack)dlsym( lib, "AMediaMuxer_addTrack" );
	s_media.muxerStart				= (EoPFN_MuxerSimple)dlsym( lib, "AMediaMuxer_start" );
	s_media.muxerStop				= (EoPFN_MuxerSimple)dlsym( lib, "AMediaMuxer_stop" );
	s_media.muxerDelete				= (EoPFN_MuxerSimple)dlsym( lib, "AMediaMuxer_delete" );
	s_media.muxerWrite				= (EoPFN_MuxerWrite)dlsym( lib, "AMediaMuxer_writeSampleData" );
	s_media.windowRelease			= libAndroid ? (EoPFN_WindowRelease)dlsym( libAndroid, "ANativeWindow_release" ) : NULL;

	s_media.ok =
		s_media.codecCreateEncoder && s_media.codecConfigure && s_media.codecCreateInputSurface && s_media.codecStart && s_media.codecStop &&
		s_media.codecDelete && s_media.codecSignalEnd && s_media.codecDequeueOutput && s_media.codecGetOutputBuffer && s_media.codecGetOutputFormat &&
		s_media.codecReleaseOutput && s_media.formatNew && s_media.formatDelete && s_media.formatSetString && s_media.formatSetInt32 &&
		s_media.muxerNew && s_media.muxerAddTrack && s_media.muxerStart && s_media.muxerStop && s_media.muxerDelete && s_media.muxerWrite &&
		s_media.windowRelease;
	return s_media.ok;
}

// the EGLConfig of the current context
void *EoContextConfig( void *dpy, void *ctx )
{
	if ( !ctx || !s_eo.pfnQueryContext || !s_eo.pfnChooseConfig )
		return NULL;
	int id = 0;
	if ( !s_eo.pfnQueryContext( dpy, ctx, EO_EGL_CONFIG_ID, &id ) )
	{
		EoClearEglError();
		return NULL;
	}
	const int byId[] = { EO_EGL_CONFIG_ID, id, EO_EGL_NONE };
	void *cfg = NULL;
	int n = 0;
	if ( !s_eo.pfnChooseConfig( dpy, byId, &cfg, 1, &n ) || n < 1 )
	{
		EoClearEglError();
		return NULL;
	}
	return cfg;
}

// A config that can feed the encoder and is compatible with the context: the context's own config when it is recordable,
// otherwise one with exactly the same buffer sizes plus EGL_RECORDABLE_ANDROID.
void *EoRecordableConfig( void *dpy, void *ctx )
{
	void *cur = EoContextConfig( dpy, ctx );
	if ( !cur || !s_eo.pfnGetConfigAttrib )
		return NULL;

	int rec = 0;
	if ( s_eo.pfnGetConfigAttrib( dpy, cur, EO_EGL_RECORDABLE_ANDROID, &rec ) && rec )
		return cur;

	static const int sameAttribs[ 7 ] = { EO_EGL_RED_SIZE, EO_EGL_GREEN_SIZE, EO_EGL_BLUE_SIZE, EO_EGL_ALPHA_SIZE, EO_EGL_DEPTH_SIZE, EO_EGL_STENCIL_SIZE, EO_EGL_SAMPLES };
	int have[ 7 ];
	for ( int i = 0; i < 7; ++i )
	{
		have[ i ] = 0;
		s_eo.pfnGetConfigAttrib( dpy, cur, sameAttribs[ i ], &have[ i ] );
	}
	int renderable = 0;
	s_eo.pfnGetConfigAttrib( dpy, cur, EO_EGL_RENDERABLE_TYPE, &renderable );

	const int want[] =
	{
		EO_EGL_RED_SIZE, have[ 0 ], EO_EGL_GREEN_SIZE, have[ 1 ], EO_EGL_BLUE_SIZE, have[ 2 ], EO_EGL_ALPHA_SIZE, have[ 3 ],
		EO_EGL_DEPTH_SIZE, have[ 4 ], EO_EGL_STENCIL_SIZE, have[ 5 ],
		EO_EGL_RENDERABLE_TYPE, renderable, EO_EGL_SURFACE_TYPE, EO_EGL_WINDOW_BIT, EO_EGL_RECORDABLE_ANDROID, 1, EO_EGL_NONE
	};
	void *cfgs[ 32 ];
	int n = 0;
	if ( !s_eo.pfnChooseConfig( dpy, want, cfgs, 32, &n ) )
	{
		EoClearEglError();
		return NULL;
	}
	for ( int c = 0; c < n; ++c )
	{
		bool bSame = true;
		for ( int i = 0; i < 7 && bSame; ++i )
		{
			int v = 0;
			s_eo.pfnGetConfigAttrib( dpy, cfgs[ c ], sameAttribs[ i ], &v );
			bSame = ( v == have[ i ] );
		}
		if ( bSame )
			return cfgs[ c ];
	}
	return NULL;
}

void *EoRecThread( void *arg )
{
	(void)arg;
	int idleMs = 0;
	for ( ;; )
	{
		EoCodecInfo info;
		memset( &info, 0, sizeof( info ) );
		const intptr_t idx = s_media.codecDequeueOutput( s_rec.codec, &info, 10000 );		// 10 ms
		if ( idx >= 0 )
		{
			idleMs = 0;
			if ( !( info.flags & 2u ) && info.size > 0 && s_rec.muxerStarted )		// 2 = codec config: already in the track format
			{
				size_t cap = 0;
				uint8_t *p = s_media.codecGetOutputBuffer( s_rec.codec, (size_t)idx, &cap );
				if ( p && info.offset >= 0 && (size_t)( info.offset + info.size ) <= cap )
				{
					s_media.muxerWrite( s_rec.muxer, (size_t)s_rec.track, p, &info );
					s_rec.framesOut++;
				}
			}
			const bool bEnd = ( info.flags & 4u ) != 0;									// 4 = end of stream
			s_media.codecReleaseOutput( s_rec.codec, (size_t)idx, false );
			if ( bEnd )
				break;
		}
		else if ( idx == -2 )		// output format changed: now the track can be added and the muxer started
		{
			if ( !s_rec.muxerStarted )
			{
				void *fmt = s_media.codecGetOutputFormat( s_rec.codec );
				if ( fmt )
				{
					const intptr_t track = s_media.muxerAddTrack( s_rec.muxer, fmt );
					s_media.formatDelete( fmt );
					if ( track >= 0 && s_media.muxerStart( s_rec.muxer ) == 0 )
					{
						s_rec.track = (int)track;
						s_rec.muxerStarted = 1;
					}
				}
			}
		}
		else if ( idx == -1 )		// nothing yet
		{
			idleMs += 10;
			if ( s_rec.stopThread && idleMs > 3000 )
				break;
		}
	}
	return NULL;
}

void EoRecStop()
{
	if ( !s_rec.active )
		return;
	s_rec.active = false;

	if ( s_rec.codec )
		s_media.codecSignalEnd( s_rec.codec );
	s_rec.stopThread = 1;
	if ( s_rec.threadStarted )
		pthread_join( s_rec.thread, NULL );
	s_rec.threadStarted = 0;

	if ( s_rec.muxer )
	{
		if ( s_rec.muxerStarted )
			s_media.muxerStop( s_rec.muxer );
		s_media.muxerDelete( s_rec.muxer );
	}
	if ( s_rec.codec )
	{
		s_media.codecStop( s_rec.codec );
		s_media.codecDelete( s_rec.codec );
	}
	if ( s_rec.window )
		s_media.windowRelease( s_rec.window );
	if ( s_rec.surface && s_eo.pfnDestroySurface )
		s_eo.pfnDestroySurface( s_eo.dpy, s_rec.surface );
	if ( s_rec.fd >= 0 )
		close( s_rec.fd );

	Msg( "EGL layer: recording finished: %s (%u frames sent to the encoder, %u written)\n", s_rec.path, s_rec.framesIn, s_rec.framesOut );
	s_rec.codec = s_rec.muxer = s_rec.window = s_rec.surface = NULL;
	s_rec.fd = -1;
	s_rec.muxerStarted = 0;
}

void EoRecStart( void *dpy, void *winSurf )
{
	s_rec.reqStart = 0;
	if ( s_rec.active )
	{
		Msg( "EGL layer: already recording (%s)\n", s_rec.path );
		return;
	}
	if ( !s_eo.extRecordable )
	{
		Warning( "EGL layer: EGL_ANDROID_recordable is not available, cannot record\n" );
		return;
	}
	if ( !EoMediaResolve() )
	{
		Warning( "EGL layer: the NDK media codec API (libmediandk, Android 8+) is not available, cannot record\n" );
		return;
	}
	void *cfg = EoRecordableConfig( dpy, s_eo.ctx );
	if ( !cfg )
	{
		Warning( "EGL layer: no recordable EGLConfig matches the game's context, cannot record\n" );
		return;
	}

	int sw = 0, sh = 0;
	if ( !s_eo.pfnQuerySurface || !s_eo.pfnQuerySurface( dpy, winSurf, EO_EGL_WIDTH, &sw ) || !s_eo.pfnQuerySurface( dpy, winSurf, EO_EGL_HEIGHT, &sh ) || sw <= 0 || sh <= 0 )
	{
		EoClearEglError();
		return;
	}

	// the video is at most gl_egl_record_height pixels tall; sizes are multiples of 16 (what hardware encoders like best)
	int th = gl_egl_record_height.GetInt();
	if ( th < 144 )
		th = 144;
	if ( th > sh )
		th = sh;
	int tw = (int)( (double)sw * (double)th / (double)sh );
	tw = ( tw + 15 ) & ~15;
	th = ( th + 15 ) & ~15;
	int fps = gl_egl_record_fps.GetInt();
	if ( fps < 5 )
		fps = 5;
	if ( fps > 120 )
		fps = 120;
	float mbps = s_rec.reqBitrateMbps;
	if ( mbps < 1.0f || mbps > 100.0f )
		mbps = 12.0f;

	void *fmt = s_media.formatNew();
	if ( !fmt )
		return;
	s_media.formatSetString( fmt, "mime", "video/avc" );
	s_media.formatSetInt32( fmt, "width", tw );
	s_media.formatSetInt32( fmt, "height", th );
	s_media.formatSetInt32( fmt, "bitrate", (int32_t)( mbps * 1000000.0f ) );
	s_media.formatSetInt32( fmt, "frame-rate", fps );
	s_media.formatSetInt32( fmt, "i-frame-interval", 1 );
	s_media.formatSetInt32( fmt, "color-format", 0x7F000789 );		// COLOR_FormatSurface

	void *codec = s_media.codecCreateEncoder( "video/avc" );
	if ( !codec )
	{
		s_media.formatDelete( fmt );
		Warning( "EGL layer: no H.264 encoder\n" );
		return;
	}
	const int32_t cs = s_media.codecConfigure( codec, fmt, NULL, NULL, 1 );		// 1 = encode
	s_media.formatDelete( fmt );
	void *window = NULL;
	if ( cs != 0 || s_media.codecCreateInputSurface( codec, &window ) != 0 || !window )
	{
		s_media.codecDelete( codec );
		Warning( "EGL layer: the H.264 encoder rejected %dx%d @ %d fps, %.1f Mbit/s\n", tw, th, fps, mbps );
		return;
	}
	if ( s_media.codecStart( codec ) != 0 )
	{
		s_media.windowRelease( window );
		s_media.codecDelete( codec );
		Warning( "EGL layer: the encoder did not start\n" );
		return;
	}

	const int none[] = { EO_EGL_NONE };
	void *surface = s_eo.pfnCreateWindowSurface( dpy, cfg, window, none );
	if ( !surface )
	{
		EoNoteEglFailure( "eglCreateWindowSurface(encoder input)" );
		s_media.codecStop( codec );
		s_media.windowRelease( window );
		s_media.codecDelete( codec );
		return;
	}

	mkdir( "recordings", 0755 );
	time_t t = time( NULL );
	struct tm lt;
	memset( &lt, 0, sizeof( lt ) );
	localtime_r( &t, &lt );
	char stamp[ 48 ];
	strftime( stamp, sizeof( stamp ), "%Y%m%d_%H%M%S", &lt );
	snprintf( s_rec.path, sizeof( s_rec.path ), "recordings/rec_%s.mp4", stamp );
	const int fd = open( s_rec.path, O_CREAT | O_TRUNC | O_RDWR, 0644 );
	void *muxer = ( fd >= 0 ) ? s_media.muxerNew( fd, 0 ) : NULL;		// 0 = MPEG-4
	if ( !muxer )
	{
		if ( fd >= 0 )
			close( fd );
		s_eo.pfnDestroySurface( dpy, surface );
		s_media.codecStop( codec );
		s_media.windowRelease( window );
		s_media.codecDelete( codec );
		Warning( "EGL layer: cannot create %s\n", s_rec.path );
		return;
	}

	s_rec.codec = codec;
	s_rec.muxer = muxer;
	s_rec.window = window;
	s_rec.surface = surface;
	s_rec.fd = fd;
	s_rec.track = -1;
	s_rec.muxerStarted = 0;
	s_rec.encW = tw;
	s_rec.encH = th;
	s_rec.framesIn = s_rec.framesOut = 0;
	s_rec.failStreak = 0;
	s_rec.stopThread = 0;
	s_rec.startNs = EoNowNs();
	s_rec.lastFrameNs = 0;
	s_rec.threadStarted = ( pthread_create( &s_rec.thread, NULL, EoRecThread, NULL ) == 0 ) ? 1 : 0;
	if ( !s_rec.threadStarted )
	{
		s_rec.active = true;		// let EoRecStop clean everything up
		EoRecStop();
		Warning( "EGL layer: cannot start the encoder thread\n" );
		return;
	}
	s_rec.active = true;
	Msg( "EGL layer: recording %dx%d @ %d fps, %.1f Mbit/s to %s (gl_egl_record_stop ends it)\n", tw, th, fps, mbps, s_rec.path );
}

// one frame: blit the finished window image into the encoder surface and swap it
void EoRecFrame( void *dpy, void *winSurf, int64_t now )
{
	if ( !s_rec.active )
		return;
	if ( s_eo.pfnGetCurrentContext && s_eo.pfnGetCurrentContext() != s_eo.ctx )
	{
		EoRecStop();		// the GL context was replaced
		return;
	}

	int fps = gl_egl_record_fps.GetInt();
	if ( fps < 5 )
		fps = 5;
	if ( fps > 120 )
		fps = 120;
	const int64_t gap = 1000000000ll / fps - 1000000ll;
	if ( s_rec.lastFrameNs != 0 && now - s_rec.lastFrameNs < gap )
		return;

	int sw = 0, sh = 0;
	if ( !s_eo.pfnQuerySurface || !s_eo.pfnQuerySurface( dpy, winSurf, EO_EGL_WIDTH, &sw ) || !s_eo.pfnQuerySurface( dpy, winSurf, EO_EGL_HEIGHT, &sh ) || sw <= 0 || sh <= 0 )
	{
		EoClearEglError();
		return;
	}
	if ( !s_eo.pfnGlBindFramebuffer || !s_eo.pfnGlBlitFramebuffer || !s_eo.pfnGlGetIntegerv || !s_eo.pfnGlIsEnabled || !s_eo.pfnGlDisable || !s_eo.pfnGlEnable )
		return;

	int prevRead = 0, prevDraw = 0;
	s_eo.pfnGlGetIntegerv( 0x8CAAu, &prevRead );		// GL_READ_FRAMEBUFFER_BINDING
	s_eo.pfnGlGetIntegerv( 0x8CA6u, &prevDraw );		// GL_DRAW_FRAMEBUFFER_BINDING

	// draw = encoder surface, read = the game's window (its back buffer still holds the finished frame)
	if ( !s_eo.pfnMakeCurrent( dpy, s_rec.surface, winSurf, s_eo.ctx ) )
	{
		EoNoteEglFailure( "eglMakeCurrent(encoder surface)" );
		s_eo.pfnMakeCurrent( dpy, winSurf, winSurf, s_eo.ctx );		// make sure the game keeps its context
		if ( ++s_rec.failStreak >= 5 )
		{
			Warning( "EGL layer: the encoder surface is not compatible with the game's context, recording stopped\n" );
			EoRecStop();
		}
		return;
	}
	s_rec.failStreak = 0;

	s_eo.pfnGlBindFramebuffer( EO_GL_READ_FRAMEBUFFER, 0 );
	s_eo.pfnGlBindFramebuffer( EO_GL_DRAW_FRAMEBUFFER, 0 );
	const bool bScissor = s_eo.pfnGlIsEnabled( EO_GL_SCISSOR_TEST ) != 0;
	if ( bScissor )
		s_eo.pfnGlDisable( EO_GL_SCISSOR_TEST );
	s_eo.pfnGlBlitFramebuffer( 0, 0, sw, sh, 0, 0, s_rec.encW, s_rec.encH, EO_GL_COLOR_BUFFER_BIT, EO_GL_LINEAR );
	if ( bScissor )
		s_eo.pfnGlEnable( EO_GL_SCISSOR_TEST );

	if ( s_eo.extPresentTime && !s_eo.presentTimeBroken && s_eo.pfnPresentationTime )
		s_eo.pfnPresentationTime( dpy, s_rec.surface, now - s_rec.startNs );		// the video timestamp
	s_eo.pfnSwapBuffers( dpy, s_rec.surface );

	// back to the game's window
	if ( !s_eo.pfnMakeCurrent( dpy, winSurf, winSurf, s_eo.ctx ) )
	{
		EoNoteEglFailure( "eglMakeCurrent(window)" );
		s_eo.pfnMakeCurrent( dpy, winSurf, winSurf, s_eo.ctx );
	}
	s_eo.pfnGlBindFramebuffer( EO_GL_READ_FRAMEBUFFER, (unsigned)prevRead );
	s_eo.pfnGlBindFramebuffer( EO_GL_DRAW_FRAMEBUFFER, (unsigned)prevDraw );

	s_rec.lastFrameNs = now;
	s_rec.framesIn++;
}

void EoRecorderService( void *dpy, void *winSurf, int64_t now )
{
	if ( s_rec.reqStop )
	{
		s_rec.reqStop = 0;
		EoRecStop();
	}
	if ( s_rec.reqStart )
		EoRecStart( dpy, winSurf );
	if ( s_rec.active )
		EoRecFrame( dpy, winSurf, now );
}

//-----------------------------------------------------------------------------
// statistics
//-----------------------------------------------------------------------------
void EoStatsPrint()
{
	const double nTs = s_eo.stTsFrames ? (double)s_eo.stTsFrames : 1.0;
	Msg( "EGL stats: %u frames swapped, %u with timestamps; GPU finished %.2f ms avg / %.2f ms max after the swap call; "
		"on screen %.2f ms avg / %.2f ms max after it\n",
		s_eo.stFrames, s_eo.stTsFrames, s_eo.stGpuSum / nTs, s_eo.stGpuMax, s_eo.stLatSum / nTs, s_eo.stLatMax );
	Msg( "EGL stats: display interval %.2f ms avg (compositor refresh %.2f ms), %u missed refreshes; %u presentation times requested, pacing %s\n",
		s_eo.stIvCount ? s_eo.stIvSum / (double)s_eo.stIvCount : 0.0, (double)s_eo.refreshNs * 1.0e-6, s_eo.stMissed, s_eo.stPresentReq,
		s_eo.paceNext ? "ON" : "off" );
	Msg( "EGL stats: frame fence waits %u (%.2f ms avg, %.2f ms max); AHardwareBuffer memory %.1f MB\n",
		s_eo.stFenceWaits, s_eo.stFenceWaits ? s_eo.stFenceWaitSum / (double)s_eo.stFenceWaits : 0.0, s_eo.stFenceWaitMax, (double)s_eo.ahbBytes / ( 1024.0 * 1024.0 ) );
	Msg( "EGL stats: dynamic buffers on AHardwareBuffer: %d (%u renames, %u falls back to the CPU copy, %u slots made); dynamic textures on AHardwareBuffer: %d (%u locks)\n",
		s_eo.nAhbBuffers, s_eo.stBufRenames, s_eo.stBufFallbacks, s_eo.stBufSlotsMade, s_eo.nAhbTextures, s_eo.stTexLocks );
	Msg( "EGL stats: front buffer mode %s, recording %s\n", s_eo.frontActive ? "ON" : "off", s_rec.active ? s_rec.path : "off" );
}

void EoStatsTick()
{
	const int nSec = gl_egl_stats_sec.GetInt();
	if ( nSec <= 0 )
		return;
	const int64_t now = EoNowNs();
	if ( s_eo.statsNextNs == 0 )
		s_eo.statsNextNs = now + (int64_t)nSec * 1000000000ll;
	if ( now >= s_eo.statsNextNs )
	{
		s_eo.statsNextNs = now + (int64_t)nSec * 1000000000ll;
		EoStatsPrint();
	}
}

void EoSurfaceChanged( void *dpy, void *surf )
{
	s_eo.surface = surf;
	s_eo.frontActive = false;
	s_eo.frontTried = false;
	s_eo.paceNext = 0;
	EoTimestampsSetup( dpy, surf );
}

} // anonymous namespace

//-----------------------------------------------------------------------------
// 8. public functions
//-----------------------------------------------------------------------------
void EglOpt_Init( void )
{
	if ( s_eo.initDone )
		return;
	if ( !gl_egl_opt.GetBool() || CommandLine()->FindParm( "-gl_no_egl_opt" ) )
	{
		s_eo.initDone = true;
		return;
	}
	if ( s_eo.initTries >= 30 )
		return;
	s_eo.initTries++;

	if ( !EoResolveCore() )
	{
		s_eo.initDone = true;		// no EGL in this process
		return;
	}
	void *dpy = s_eo.pfnGetCurrentDisplay();
	if ( !dpy )
		return;						// no current EGL context yet: the next hook tries again
	s_eo.dpy = dpy;
	s_eo.ctx = s_eo.pfnGetCurrentContext ? s_eo.pfnGetCurrentContext() : NULL;

	const char *ext = s_eo.pfnQueryString( dpy, EO_EGL_EXTENSIONS );
	s_eo.extFenceSync			= EoHasToken( ext, "EGL_KHR_fence_sync" );
	s_eo.extNativeFence			= s_eo.extFenceSync && EoHasToken( ext, "EGL_ANDROID_native_fence_sync" );
	s_eo.extPresentTime			= EoHasToken( ext, "EGL_ANDROID_presentation_time" );
	s_eo.extFrameTs				= EoHasToken( ext, "EGL_ANDROID_get_frame_timestamps" );
	s_eo.extFrontBuffer			= EoHasToken( ext, "EGL_ANDROID_front_buffer_auto_refresh" );
	s_eo.extMutableRenderBuffer	= EoHasToken( ext, "EGL_KHR_mutable_render_buffer" );
	s_eo.extGetNativeClientBuffer = EoHasToken( ext, "EGL_ANDROID_get_native_client_buffer" );
	s_eo.extImageBase			= EoHasToken( ext, "EGL_KHR_image_base" ) || EoHasToken( ext, "EGL_KHR_image" );
	s_eo.extImageNativeBuffer	= s_eo.extImageBase && EoHasToken( ext, "EGL_ANDROID_image_native_buffer" );
	s_eo.extRecordable			= EoHasToken( ext, "EGL_ANDROID_recordable" );
	s_eo.extImageColorspace		= EoHasToken( ext, "EGL_EXT_image_gl_colorspace" );

	if ( s_eo.extFenceSync || s_eo.extNativeFence )
	{
		s_eo.pfnCreateSync = (EoPFN_CreateSync)EoExtProc( "eglCreateSyncKHR" );
		s_eo.pfnDestroySync = (EoPFN_DestroySync)EoExtProc( "eglDestroySyncKHR" );
		if ( !s_eo.pfnCreateSync || !s_eo.pfnDestroySync )
			s_eo.extFenceSync = s_eo.extNativeFence = false;
	}
	if ( s_eo.extNativeFence )
	{
		s_eo.pfnDupNativeFence = (EoPFN_DupNativeFenceFD)EoExtProc( "eglDupNativeFenceFDANDROID" );
		if ( !s_eo.pfnDupNativeFence )
			s_eo.extNativeFence = false;
	}
	if ( s_eo.extPresentTime )
	{
		s_eo.pfnPresentationTime = (EoPFN_PresentationTime)EoExtProc( "eglPresentationTimeANDROID" );
		if ( !s_eo.pfnPresentationTime )
			s_eo.extPresentTime = false;
	}
	if ( s_eo.extFrameTs )
	{
		s_eo.pfnGetNextFrameId = (EoPFN_GetNextFrameId)EoExtProc( "eglGetNextFrameIdANDROID" );
		s_eo.pfnGetCompositorTiming = (EoPFN_GetCompositorTiming)EoExtProc( "eglGetCompositorTimingANDROID" );
		s_eo.pfnGetFrameTimestamps = (EoPFN_GetFrameTimestamps)EoExtProc( "eglGetFrameTimestampsANDROID" );
		s_eo.pfnGetFrameTimestampSupported = (EoPFN_GetFrameTimestampSupported)EoExtProc( "eglGetFrameTimestampSupportedANDROID" );
		if ( !s_eo.pfnGetFrameTimestampSupported )
			s_eo.pfnGetFrameTimestampSupported = (EoPFN_GetFrameTimestampSupported)EoExtProc( "eglGetFrameTimestampsSupportedANDROID" );
		if ( !s_eo.pfnGetNextFrameId || !s_eo.pfnGetFrameTimestamps || !s_eo.pfnSurfaceAttrib )
			s_eo.extFrameTs = false;
	}
	if ( s_eo.extGetNativeClientBuffer )
	{
		s_eo.pfnGetNativeClientBuffer = (EoPFN_GetNativeClientBuffer)EoExtProc( "eglGetNativeClientBufferANDROID" );
		if ( !s_eo.pfnGetNativeClientBuffer )
			s_eo.extGetNativeClientBuffer = false;
	}
	if ( s_eo.extImageNativeBuffer )
	{
		s_eo.pfnCreateImage = (EoPFN_CreateImage)EoExtProc( "eglCreateImageKHR" );
		s_eo.pfnDestroyImage = (EoPFN_DestroyImage)EoExtProc( "eglDestroyImageKHR" );
		if ( !s_eo.pfnCreateImage || !s_eo.pfnDestroyImage )
			s_eo.extImageNativeBuffer = false;
	}

	// the GL side (the context is current)
	EoPFN_GlGetString pfnGetString = (EoPFN_GlGetString)EoGlProc( "glGetString" );
	const char *glExt = pfnGetString ? (const char *)pfnGetString( EO_GL_EXTENSIONS ) : NULL;
	s_eo.glEglImage = EoHasToken( glExt, "GL_OES_EGL_image" );
	s_eo.glExternalBuffer = EoHasToken( glExt, "GL_EXT_external_buffer" ) && EoHasToken( glExt, "GL_EXT_buffer_storage" );
	if ( s_eo.glEglImage )
	{
		s_eo.pfnGlEGLImageTargetTexture2D = (EoPFN_GlEGLImageTargetTexture2D)EoExtProc( "glEGLImageTargetTexture2DOES" );
		if ( !s_eo.pfnGlEGLImageTargetTexture2D )
			s_eo.glEglImage = false;
	}
	if ( s_eo.glExternalBuffer )
	{
		s_eo.pfnGlBufferStorageExternal = (EoPFN_GlBufferStorageExternal)EoExtProc( "glBufferStorageExternalEXT" );
		if ( !s_eo.pfnGlBufferStorageExternal || !s_eo.pfnGlGenBuffers || !s_eo.pfnGlBindBuffer || !s_eo.pfnGlDeleteBuffers || !s_eo.pfnGlMapBufferRange || !s_eo.pfnGlGetError )
			s_eo.glExternalBuffer = false;
	}

	EoResolveAhb();

	const bool bGlCommon = s_eo.pfnGlGetError && s_eo.pfnGlTexParameteri;
	s_eo.bufDisabled = !( s_eo.ahbApi && s_eo.extGetNativeClientBuffer && s_eo.glExternalBuffer );
	s_eo.texDisabled = !( s_eo.ahbApi && s_eo.extGetNativeClientBuffer && s_eo.extImageNativeBuffer && s_eo.glEglImage && bGlCommon );

	// is the game's own config recordable? (the recorder can use another one, this is only for the log)
	if ( s_eo.extRecordable && s_eo.ctx && s_eo.pfnGetConfigAttrib )
	{
		void *cfg = EoContextConfig( dpy, s_eo.ctx );
		int rec = 0;
		s_eo.ctxConfigRecordable = cfg && s_eo.pfnGetConfigAttrib( dpy, cfg, EO_EGL_RECORDABLE_ANDROID, &rec ) && rec != 0;
	}

	s_eo.active = true;
	s_eo.initDone = true;
	EoPrintCaps();
}

bool EglOpt_Active( void )
{
	return s_eo.active;
}

uint32_t EglOpt_FrameIndex( void )
{
	return s_eo.frame;
}

void EglOpt_BeforeSwap( void )
{
	if ( !s_eo.initDone )
		EglOpt_Init();
	if ( !s_eo.active )
		return;

	void *dpy = s_eo.dpy;
	void *surf = s_eo.pfnGetCurrentSurface ? s_eo.pfnGetCurrentSurface( EO_EGL_DRAW ) : NULL;
	if ( !surf )
		return;
	if ( !s_rec.active && s_eo.pfnGetCurrentContext )
	{
		void *ctx = s_eo.pfnGetCurrentContext();		// the engine may have made a new context since EglOpt_Init
		if ( ctx )
			s_eo.ctx = ctx;
	}
	if ( surf != s_eo.surface )
		EoSurfaceChanged( dpy, surf );

	EoFenceLimit();										// may wait: take the clock afterwards
	const int64_t now = EoNowNs();
	EoFrontBufferUpdate( dpy, surf );
	EoRecorderService( dpy, surf, now );				// switches the current surface and back: before anything that needs the window surface
	EoTimestampsBegin( dpy, surf, now );
	EoPace( dpy, surf, now );
	EoFenceInsert( dpy );								// last: right behind everything this frame submitted
}

void EglOpt_AfterSwap( void )
{
	if ( !s_eo.active )
		return;
	if ( s_eo.dpy )
		EoFenceCollect( s_eo.dpy );
	EoTimestampsPoll( s_eo.dpy, s_eo.surface );
	s_eo.stFrames++;
	s_eo.frame++;
	EoStatsTick();
}

//-----------------------------------------------------------------------------
// console commands
//-----------------------------------------------------------------------------
CON_COMMAND( gl_egl_info, "Print which EGL / AHardwareBuffer features this device offers and which of them the EGL layer uses." )
{
	EglOpt_Init();
	if ( !s_eo.active )
	{
		ConMsg( "EGL layer is not active (switched off with gl_egl_opt 0 / -gl_no_egl_opt, or this build has no EGL context)\n" );
		return;
	}
	EoPrintCaps();
	ConMsg( "EGL layer: fence %s, in-flight limit %d, timestamps %s, presentation time pacing %s, front buffer %s\n",
		( s_eo.extNativeFence && !s_eo.nativeFenceBroken ) ? "native (sync_file)" : ( s_eo.pfnGlFenceSync ? "GL sync" : "none" ),
		gl_egl_max_frames.GetInt(), s_eo.tsEnabled ? "ON" : "off",
		( s_eo.extPresentTime && !s_eo.presentTimeBroken && gl_egl_present_time.GetBool() ) ? "armed (needs fps_max below the refresh rate, vsync off)" : "off",
		s_eo.frontActive ? "ON" : "off" );
}

CON_COMMAND( gl_egl_stats, "Print the EGL layer statistics (GPU time, display latency, missed refreshes, fence waits, AHardwareBuffer use)." )
{
	if ( !s_eo.active )
	{
		ConMsg( "EGL layer is not active\n" );
		return;
	}
	EoStatsPrint();
}

CON_COMMAND( gl_egl_record_start, "gl_egl_record_start [Mbit/s]: record the game to ./recordings/rec_<time>.mp4 (EGL_ANDROID_recordable + MediaCodec). Size / rate: gl_egl_record_height, gl_egl_record_fps." )
{
	if ( !s_eo.active )
	{
		ConMsg( "EGL layer is not active\n" );
		return;
	}
	s_rec.reqBitrateMbps = ( args.ArgC() > 1 ) ? (float)atof( args.Arg( 1 ) ) : 12.0f;
	s_rec.reqStart = 1;		// done by the render thread at the next swap
}

CON_COMMAND( gl_egl_record_stop, "Stop the recording started with gl_egl_record_start." )
{
	s_rec.reqStop = 1;
}

#else // !EGLOPT_POSIX

//-----------------------------------------------------------------------------
// no EGL here: everything is switched off and the callers keep their old code
//-----------------------------------------------------------------------------
void		EglOpt_Init( void )												{}
bool		EglOpt_Active( void )											{ return false; }
uint32_t	EglOpt_FrameIndex( void )										{ return 0; }
void		EglOpt_BeforeSwap( void )										{}
void		EglOpt_AfterSwap( void )										{}
bool		EglOpt_BufferCreate( const void *, unsigned int, unsigned int, void *, unsigned int * )	{ return false; }
bool		EglOpt_BufferTracked( const void * )							{ return false; }
int			EglOpt_BufferLock( const void *, bool, bool, unsigned int, unsigned int, void **, unsigned int * )	{ return kEglOptBufNone; }
bool		EglOpt_BufferInAhbMode( const void * )							{ return false; }
void		EglOpt_BufferUnlock( const void * )								{}
bool		EglOpt_BufferDestroy( const void * )							{ return false; }
bool		EglOpt_TexWanted( unsigned int, unsigned int )					{ return false; }
bool		EglOpt_TexCreate( const void *, unsigned int, unsigned int, bool, bool )	{ return false; }
bool		EglOpt_TexTracked( const void * )								{ return false; }
char		*EglOpt_TexLock( const void *, bool, int * )					{ return NULL; }
void		EglOpt_TexUnlock( const void * )								{}
bool		EglOpt_TexDestroy( const void * )								{ return false; }

#endif // EGLOPT_POSIX
