//========= EGL (Android) optimizations for TOGL ============================================================
//
// Public interface of the EGL layer. The implementation is egl_opt_impl.inl, which is #included at the end of
// mali_opt.cpp (mali_opt.cpp is already part of the module, so no build script has to change).
//
// Everything here is optional: every function is safe to call when EGL is missing, an extension is missing or a switch
// is off - the callers keep their old code path as the fallback, so nothing changes for a device that cannot do it.
//
// Extensions used (all looked up at run time, no EGL header of the NDK is needed):
//   EGL_ANDROID_native_fence_sync        per-frame GPU fence, tells when a frame has really finished on the GPU
//   EGL_ANDROID_presentation_time        evenly spaced presentation times when fps_max is below the refresh rate
//   EGL_ANDROID_get_frame_timestamps     GPU / display latency statistics, refresh period, composite deadline
//   EGL_ANDROID_front_buffer_auto_refresh  optional low latency single buffer mode (gl_egl_front_buffer 1)
//   EGL_ANDROID_get_native_client_buffer AHardwareBuffer -> EGLClientBuffer
//   EGL_ANDROID_image_native_buffer      EGLClientBuffer -> EGLImage -> GL texture (dynamic textures)
//   EGL_ANDROID_recordable               EGLConfig that can feed a MediaCodec surface (gl_egl_record_start)
//
// AHardwareBuffer is used ONLY for dynamic textures and dynamic vertex / index buffers. Static textures (the ASTC
// path), render targets and every other buffer keep their normal GL storage.
//==========================================================================================================

#ifndef EGL_OPT_H
#define EGL_OPT_H

#ifdef _WIN32
#pragma once
#endif

#include <stdint.h>
#include <stddef.h>

//-----------------------------------------------------------------------------------------------------------
// life cycle. EglOpt_Init() needs a current GL context; calling it again is free.
//-----------------------------------------------------------------------------------------------------------
void		EglOpt_Init( void );
bool		EglOpt_Active( void );							// an EGL display was found and the master switch is on
uint32_t	EglOpt_FrameIndex( void );						// number of frames swapped so far

//-----------------------------------------------------------------------------------------------------------
// frame hooks, around the call that swaps the window (GLMContext::Present -> ShowPixels)
//-----------------------------------------------------------------------------------------------------------
void		EglOpt_BeforeSwap( void );
void		EglOpt_AfterSwap( void );

//-----------------------------------------------------------------------------------------------------------
// dynamic vertex / index buffers (CGLMBuffer). nGLTarget is GL_ARRAY_BUFFER or GL_ELEMENT_ARRAY_BUFFER.
//
// The buffer keeps its CPU copy (the "pseudo" memory) as the fallback and additionally gets a small ring of
// AHardwareBuffer backed GL buffers. A DISCARD lock moves to the next ring slot that the GPU has finished with; when
// no slot is free the buffer falls back to the CPU copy until the next DISCARD.
//-----------------------------------------------------------------------------------------------------------
enum EglOptBufferMode
{
	kEglOptBufNone = 0,			// not tracked: use the old code
	kEglOptBufAhb = 1,			// *ppBase = CPU address of the buffer start inside the AHardwareBuffer, *pGLName = its GL buffer
	kEglOptBufPseudo = 2		// *ppBase = the CPU copy, *pGLName = 0 (client side arrays)
};

bool		EglOpt_BufferCreate( const void *pOwner, unsigned int nGLTarget, unsigned int nSize, void *pPseudoBase, unsigned int *pGLName );
bool		EglOpt_BufferTracked( const void *pOwner );
int			EglOpt_BufferLock( const void *pOwner, bool bDiscard, bool bNoOverwrite, unsigned int nOffset, unsigned int nSize, void **ppBase, unsigned int *pGLName );
bool		EglOpt_BufferInAhbMode( const void *pOwner );
void		EglOpt_BufferUnlock( const void *pOwner );
bool		EglOpt_BufferDestroy( const void *pOwner );		// true: it was tracked, its GL buffers are gone, the caller frees the CPU copy

//-----------------------------------------------------------------------------------------------------------
// dynamic 2D textures (CGLMTex, kGLMTexDynamic, A8R8G8B8 / X8R8G8B8, one mip level, not a render target). With the sRGB flag
// (TOGL sets it on every such texture when GL_EXT_texture_sRGB_decode exists) the EGLImage is created sRGB (EGL_EXT_image_gl_colorspace).
// The pixels live in an AHardwareBuffer that is also the storage of the GL texture: Lock hands out the shared memory,
// Unlock only unlocks it - no copy and no ASTC encode.
//-----------------------------------------------------------------------------------------------------------
bool		EglOpt_TexWanted( unsigned int nWidth, unsigned int nHeight );
bool		EglOpt_TexCreate( const void *pOwner, unsigned int nWidth, unsigned int nHeight, bool bOpaque, bool bSRGB );	// the texture is bound to GL_TEXTURE_2D on the active TMU
bool		EglOpt_TexTracked( const void *pOwner );
char		*EglOpt_TexLock( const void *pOwner, bool bReadOnly, int *pPitchBytes );
void		EglOpt_TexUnlock( const void *pOwner );
bool		EglOpt_TexDestroy( const void *pOwner );		// true: it was an AHardwareBuffer texture, the caller deletes the GL name itself

#endif // EGL_OPT_H
