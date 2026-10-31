#ifndef _GL4ES_DECOMPRESS_H_
#define _GL4ES_DECOMPRESS_H_

#include <stdint.h>

void DecompressBlockDXT1(uint32_t x, uint32_t y, uint32_t width,
	const uint8_t* blockStorage,
	int transparent0, int* simpleAlpha, int *complexAlpha,
	uint32_t* image);

void DecompressBlockDXT3(uint32_t x, uint32_t y, uint32_t width,
	const uint8_t* blockStorage,
	int transparent0, int* simpleAlpha, int *complexAlpha,
	uint32_t* image);

void DecompressBlockDXT5(uint32_t x, uint32_t y, uint32_t width,
	const uint8_t* blockStorage,
	int transparent0, int* simpleAlpha, int *complexAlpha,
	uint32_t* image);

// Whole-level decoders: blocks = ceil(width/4) * ceil(height/4) DXT blocks in row-major order,
// rgba = width * height tightly packed RGBA8 pixels (row pitch = width pixels). Nothing outside
// that range is written, also when width / height are not multiples of 4.
// DXT1 with transparent0 == 0 is returned as RGBA with alpha 255 (no packed-RGB intermediate).
// simpleAlpha / complexAlpha may be NULL; when given they are only ever set to 1 (never cleared),
// with the same meaning as in the per-block functions above.
void DecompressImageDXT1(uint32_t width, uint32_t height, const uint8_t* blocks,
	int transparent0, int* simpleAlpha, int* complexAlpha,
	uint32_t* rgba);

void DecompressImageDXT3(uint32_t width, uint32_t height, const uint8_t* blocks,
	int transparent0, int* simpleAlpha, int* complexAlpha,
	uint32_t* rgba);

void DecompressImageDXT5(uint32_t width, uint32_t height, const uint8_t* blocks,
	int transparent0, int* simpleAlpha, int* complexAlpha,
	uint32_t* rgba);

#endif // _GL4ES_DECOMPRESS_H_
