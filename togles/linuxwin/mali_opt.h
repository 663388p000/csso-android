//========= Mali (ARM GPU) optimizations for TOGL ================================//
//
// mali_opt.h
//
// Small helpers that only do something on ARM Mali / Immortalis GPUs (GL_VENDOR "ARM"), or on any GPU when
// explicitly asked to. Everything is guarded by runtime checks and has a plain fallback, so the rest of TOGL
// behaves exactly as before when a feature is not available or is switched off.
//
// Command line switches (reliable, read at startup):
//   -gl_no_mali_opt      all Mali specific optimizations off
//   -gl_no_progcache     program binary cache off
//   -gl_progcache_all    allow the program binary cache on non-Mali GPUs too
//   -gl_no_lazy_xlat     translate every shader when the engine creates it (the old behaviour)
//   -gl_no_prof          hitch profiler off
// Convars (read at first use): gl_mali_opt, gl_mali_discard_depth, gl_mali_conservative_oq,
//   gl_mali_progcache, gl_mali_progcache_dir, gl_mali_progcache_max_mb,
//   gl_mali_lazy_xlat, gl_mali_uniform_fastpath, gl_mali_astc_decode_rgba8,
//   gl_mali_prof, gl_mali_hitch_ms, gl_mali_slow_ms, gl_mali_prof_stats_sec.
// Console command: gl_mali_stats.
//
// All GL parameters are plain unsigned int / int so this header needs no GL headers.
//
//===============================================================================

#ifndef MALI_OPT_H
#define MALI_OPT_H

#pragma once

#include <stdint.h>
#include <stddef.h>

// True when the GL driver is ARM's (Mali / Immortalis) and the Mali optimizations are not switched off.
// Only valid once the GL entry points exist (gGL); returns false before that.
bool MaliOpt_IsMali();

//-----------------------------------------------------------------------------
// Tile-based rendering: do not write dead depth/stencil data back to memory
//-----------------------------------------------------------------------------

// True if the depth/stencil attachments of the back-buffer FBO should be invalidated right before Present
// (Mali, convar gl_mali_discard_depth, and glInvalidateFramebuffer is available).
bool MaliOpt_DiscardDepthAtPresent();

// glInvalidateFramebuffer( target, numAttachments, attachments ). Returns false if the entry point is missing.
bool MaliOpt_InvalidateFramebuffer( unsigned int target, int numAttachments, const unsigned int *attachments );

//-----------------------------------------------------------------------------
// Occlusion queries
//-----------------------------------------------------------------------------

// GL_ANY_SAMPLES_PASSED_CONSERVATIVE on Mali (cheaper for the tiler, may report "visible" a little more often),
// GL_ANY_SAMPLES_PASSED everywhere else. The value is latched on first use so a query object always sees
// the same target for its whole life.
unsigned int MaliOpt_OcclusionQueryTarget();

//-----------------------------------------------------------------------------
// Program binary cache (glGetProgramBinary / glProgramBinary)
//
// Linking a Source shader pair costs the Mali driver several milliseconds. After the first run the linked binaries
// are kept in a single file (shader_cache/progcache.bin) and restored with glProgramBinary, so level loads stop
// hitching on shader compiles. The file is keyed by the GL vendor/renderer/version strings (a driver update
// invalidates it) and every entry carries a checksum; anything unusable falls back to compiling from source.
//-----------------------------------------------------------------------------

// True when the cache is in use. Decided once, at first call (needs a current GL context). While it is true,
// CGLMProgram::Compile() leaves shaders uncompiled until a pair actually needs them (see cglmprogram.cpp).
bool MaliOpt_ProgramCacheActive();

// Key of a vertex + fragment shader pair (hash of both GLSL texts and of the attribute binding layout).
uint64_t MaliOpt_ProgramKey( const char *vsText, size_t vsLen, const char *fsText, size_t fsLen );

// Tries to restore 'program' from the cache. Returns true if the program is now linked (glProgramBinary succeeded).
bool MaliOpt_ProgramLoad( unsigned int program, uint64_t key );

// Call before glLinkProgram() of a program that will be stored (sets GL_PROGRAM_BINARY_RETRIEVABLE_HINT).
void MaliOpt_ProgramPrepareLink( unsigned int program );

// Call after a successful glLinkProgram() to save the binary under 'key'.
void MaliOpt_ProgramStore( unsigned int program, uint64_t key );

//-----------------------------------------------------------------------------
// Hitch profiler (stutter diagnosis, works on every GPU)
//
// Times the steps of Togles that are expensive but rare (shader translation, shader compile, program link, program
// validation, texture uploads, the swap) and attributes every long frame to them. A frame that takes much longer than the
// recent average is written to the log as one line, e.g.
//   Togles hitch: frame 83.4 ms (avg 9.8) | xlat 6x 12.1 | link 4x 41.2 (cache hit 3, miss 1) | validate 4x 3.1 | tex 2x 5.0 | swap 1.2 | unaccounted 20.8
// "unaccounted" is time that is not in any of the measured steps: the game, the audio, other threads or a driver stall in a draw call. If
// it dominates a hitch line, Togles is not the cause of that hitch. A single step slower than gl_mali_slow_ms is logged on its own with the
// names of the shaders. Every gl_mali_prof_stats_sec seconds a summary line is written.
//-----------------------------------------------------------------------------

enum MaliProfCat
{
	kMaliProfXlat = 0,		// D3D shader byte code -> GLSL (CreateVertexShader / CreatePixelShader, or on first use with lazy translation)
	kMaliProfCompile,		// glCompileShader (+ status)
	kMaliProfLink,			// CGLMShaderPair::SetProgramPair: glLinkProgram or glProgramBinary
	kMaliProfValidate,		// CGLMShaderPair::ValidateProgramPair: link status (the driver finishes its work here) + uniform lookups
	kMaliProfTexture,		// texture creation / upload (incl. ASTC / DXT conversion)
	kMaliProfSwap,			// the buffer swap at the end of the frame
	kMaliProfNumCats
};

enum MaliProfCounter
{
	kMaliCtrLazyCreated = 0,	// shaders created without translating them
	kMaliCtrLazyTranslated,		// ... of which were translated later because a draw needed them
	kMaliCtrLazyDropped,		// ... of which were deleted without ever being used (the translations that were saved)
	kMaliCtrUniformFast,		// shader pairs whose uniform array locations came from the fast path
	kMaliCtrUniformSlow,		// shader pairs that needed the per-element lookups
	kMaliCtrUniformSaved,		// glGetUniformLocation calls that were not made
	kMaliProfNumCounters
};

extern int g_nMaliProfOn;			// 1 while the profiler is on (read by the inline helpers below)

uint64_t MaliProf_NowUs();			// monotonic clock, microseconds, never 0

// Adds 'us' microseconds (one occurrence) to 'cat'. If the step took longer than gl_mali_slow_ms it is logged right away, together with
// the optional strings (shader names and the like; only read during the call).
void MaliProf_Add( int cat, uint64_t us, const char *a = NULL, const char *b = NULL, const char *c = NULL );

void MaliProf_Counter( int counter, unsigned int n = 1 );

// Call once per frame, right after the swap. Closes the frame, writes the hitch / summary lines.
void MaliProf_FrameEnd();

// Times a scope. Costs two clock reads when the profiler is on and nothing but a test when it is off.
class CMaliProfScope
{
public:
	// pInfo (a shader name, say) must stay valid until the end of the scope; it is only used if the step turns out to be slow
	explicit CMaliProfScope( int cat, const char *pInfo = NULL ) : m_nCat( cat ), m_pInfo( pInfo ), m_nStart( g_nMaliProfOn ? MaliProf_NowUs() : 0 ) {}
	~CMaliProfScope()	{ if ( m_nStart ) MaliProf_Add( m_nCat, MaliProf_NowUs() - m_nStart, m_pInfo ); }
private:
	CMaliProfScope( const CMaliProfScope & );
	CMaliProfScope &operator=( const CMaliProfScope & );
	int			m_nCat;
	const char	*m_pInfo;
	uint64_t	m_nStart;
};

//-----------------------------------------------------------------------------
// Uniform array locations (CGLMShaderPair::ValidateProgramPair)
//
// A linked shader pair needs the location of every element of its "vc[]" / "pc[]" uniform arrays. Asking the driver for each element by
// name costs up to a few hundred glGetUniformLocation calls per pair, on the render thread, the first time the pair is drawn. This asks
// for element 0 and verifies a few more; if they are consecutive (which they are for arrays on every driver seen so far) the rest are
// computed. The first pairs are checked against the driver completely, and anything unexpected switches back to asking for every
// element, so the result is always what the plain loop would have produced.
//-----------------------------------------------------------------------------

// Fills locs[0..count-1] with the locations of "<prefix>c[0]" .. "<prefix>c[count-1]" of 'program' (prefix 'v' or 'p'). Returns the number of
// leading elements that exist, like the old loop did (locs[n] is -1 when n < count).
int MaliOpt_UniformArrayLocations( unsigned int program, char prefix, int count, int *locs );

//-----------------------------------------------------------------------------
// Lazy shader translation
//
// The engine creates every dynamic combo of a static shader combo as soon as the static combo is first needed: dozens to hundreds of
// shaders at once, of which a draw uses one or two. Translating each of them from Direct3D byte code to GLSL on the spot is what
// makes the first draw of a new material hitch. With lazy translation Togles keeps a copy of the byte code and translates a shader the first
// time a shader pair that contains it is needed (see dxabstract.cpp, MaliLazy_*). Everything that is derived from the translation
// (constant counts, attribute map, sampler masks) is produced at that time and nothing reads it earlier.
//-----------------------------------------------------------------------------

// True when shaders are created lazily (Mali, convar gl_mali_lazy_xlat, not -gl_no_lazy_xlat). Decided on first call.
bool MaliOpt_LazyXlatActive();

// Size in bytes of a Direct3D shader model 2.0 / 3.0 byte code stream including the END token, 0 if it does not look like one
// (wrong version token for 'bVertexShader', no END within 256 KB, ...). Reads at most 256 KB.
size_t MaliOpt_D3DBytecodeBytes( const uint32_t *pFunction, bool bVertexShader );

// The text a shader carries until it has been translated.
#define MALI_LAZY_MARK		"//LAZYXL\n"
static inline bool MaliLazy_IsPendingText( const char *pText )
{
	return pText && pText[0] == '/' && pText[1] == '/' && pText[2] == 'L' && pText[3] == 'A' && pText[4] == 'Z' && pText[5] == 'Y' && pText[6] == 'X' && pText[7] == 'L';
}

// While true, new CGLMProgram objects do not create their GL shader object (CreateShader costs a driver call; lazily translated shaders
// get theirs when they are translated).
void MaliLazy_SetDeferShaderObject( bool bDefer );
bool MaliLazy_DeferShaderObject();

class CGLMProgram;

// Translates the program if it is still waiting for it (no-op otherwise, and for programs that were never lazy). Returns false if it is
// pending and could not be translated. Defined in dxabstract.cpp.
bool MaliLazy_EnsureProgram( CGLMProgram *pProg );

// Throws away the pending translation of a program that is being deleted. Defined in dxabstract.cpp.
void MaliLazy_ForgetProgram( CGLMProgram *pProg );

// Puts the translated text into a lazily created program, creates its GL shader object and compiles it, the way GLMContext::NewProgram()
// does for an ordinary program. Defined in cglmprogram.cpp.
void MaliLazy_FinishProgram( CGLMProgram *pProg, char *pText );

//-----------------------------------------------------------------------------
// ASTC decode mode (GL_EXT_texture_compression_astc_decode_mode)
//
// The ASTC textures Togles makes hold 8 bit LDR data. Telling the Mali texture unit to decode them to RGBA8 instead of fp16 halves the
// size of what its filters and its texture cache work with, for no change in the picture.
//-----------------------------------------------------------------------------

// Sets the decode mode of the texture currently bound to 'target' (GL_TEXTURE_2D or GL_TEXTURE_CUBE_MAP) to RGBA8. Does nothing, and
// returns false, when the extension or the switch (gl_mali_astc_decode_rgba8) is off.
bool MaliOpt_AstcDecodeRGBA8( unsigned int target );

#endif // MALI_OPT_H
