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
// Convars (read at first use): gl_mali_opt, gl_mali_discard_depth, gl_mali_conservative_oq,
//   gl_mali_progcache, gl_mali_progcache_dir, gl_mali_progcache_max_mb. Console command: gl_mali_stats.
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

#endif // MALI_OPT_H
