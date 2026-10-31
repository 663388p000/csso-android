#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "arm_neon_opt.h"

#if defined( __GNUC__ ) || defined( __clang__ )
	#define DXT_SCALAR_FN static __attribute__(( unused ))
#else
	#define DXT_SCALAR_FN static
#endif

/*
DXT1/DXT3/DXT5 texture decompression

ARM / Mali port: on AArch64 the public entry points below run NEON decoders (see the "NEON block decoders"
and "NEON whole-image kernels" sections). They produce exactly the same bytes and the same simpleAlpha /
complexAlpha flags as the scalar code in this file, which is kept as the reference (DecompressBlockDXT*_Scalar)
and is what runs on every other CPU or when TOGL_DISABLE_ARM_NEON is defined.
DecompressImageDXT1/3/5 decode a whole mip level per call; that is what the texture upload path uses.

The original code is from Benjamin Dobell, see below for details. Compared to
the original this one adds DXT3 decompression, is valid C89, and is x64 
compatible as it uses fixed size integers everywhere. It also uses a different
PackRGBA order.

---

Copyright (c) 2012, Matth�us G. "Anteru" Chajdas (http://anteru.net)

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in 
the Software without restriction, including without limitation the rights to 
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies 
of the Software, and to permit persons to whom the Software is furnished to do 
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all 
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR 
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, 
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE 
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER 
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, 
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE 
SOFTWARE.

---

Copyright (C) 2009 Benjamin Dobell, Glass Echidna

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in 
the Software without restriction, including without limitation the rights to 
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies 
of the Software, and to permit persons to whom the Software is furnished to do 
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all 
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR 
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, 
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE 
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER 
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, 
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE 
SOFTWARE.

---
*/
static uint32_t PackRGBA (uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
	return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24);
}

static void DecompressBlockDXT1Internal (const uint8_t* block,
	uint32_t* output,
	uint32_t outputStride,
	int transparent0, int* simpleAlpha, int *complexAlpha,
	const uint8_t* alphaValues)
{
	uint32_t temp, code;

	uint16_t color0, color1;
	uint8_t r0, g0, b0, r1, g1, b1;

	int i, j;

	color0 = *(const uint16_t*)(block);
	color1 = *(const uint16_t*)(block + 2);

	temp = (color0 >> 11) * 255 + 16;
	r0 = (uint8_t)((temp/32 + temp)/32);
	temp = ((color0 & 0x07E0) >> 5) * 255 + 32;
	g0 = (uint8_t)((temp/64 + temp)/64);
	temp = (color0 & 0x001F) * 255 + 16;
	b0 = (uint8_t)((temp/32 + temp)/32);

	temp = (color1 >> 11) * 255 + 16;
	r1 = (uint8_t)((temp/32 + temp)/32);
	temp = ((color1 & 0x07E0) >> 5) * 255 + 32;
	g1 = (uint8_t)((temp/64 + temp)/64);
	temp = (color1 & 0x001F) * 255 + 16;
	b1 = (uint8_t)((temp/32 + temp)/32);

	code = *(const uint32_t*)(block + 4);

	for (j = 0; j < 4; ++j) {
		for (i = 0; i < 4; ++i) {
			uint32_t finalColor, positionCode;
			uint8_t alpha;

			finalColor = 0; positionCode = (code >> 2*(4*j+i)) & 0x03;
			alpha = alphaValues [j*4+i];

			if (color0 > color1) {
				switch (positionCode)
				{
				case 0:
					finalColor = PackRGBA(r0, g0, b0, alpha);
					break;
				case 1:
					finalColor = PackRGBA(r1, g1, b1, alpha);
					break;
				case 2:
					finalColor = PackRGBA((2*r0+r1)/3, (2*g0+g1)/3, (2*b0+b1)/3, alpha);
					break;
				case 3:
					finalColor = PackRGBA((r0+2*r1)/3, (g0+2*g1)/3, (b0+2*b1)/3, alpha);
					break;
				}
			} else {
				switch (positionCode) {
				case 0:
					finalColor = PackRGBA(r0, g0, b0, alpha);
					break;
				case 1:
					finalColor = PackRGBA(r1, g1, b1, alpha);
					break;
				case 2:
					finalColor = PackRGBA((r0+r1)/2, (g0+g1)/2, (b0+b1)/2, alpha);
					break;
				case 3:
					if(transparent0) alpha=0;
					finalColor = PackRGBA(0, 0, 0, alpha);
					break;
				}
			}

			if(!alpha)
				*simpleAlpha = 1;
			else if(alpha<0xff)
				*complexAlpha = 1;

			output [j*outputStride + i] = finalColor;
		}
	}
}

static void DecompressBlockDXT1InternalRGB(const uint8_t* block, uint8_t* output, uint32_t outputStride)
{
	uint32_t temp, code;

	uint16_t color0, color1;
	uint8_t r0, g0, b0, r1, g1, b1;

	int i, j;

	color0 = *(const uint16_t*)(block);
	color1 = *(const uint16_t*)(block + 2);

	temp = (color0 >> 11) * 255 + 16;
	r0 = (uint8_t)((temp/32 + temp)/32);
	temp = ((color0 & 0x07E0) >> 5) * 255 + 32;
	g0 = (uint8_t)((temp/64 + temp)/64);
	temp = (color0 & 0x001F) * 255 + 16;
	b0 = (uint8_t)((temp/32 + temp)/32);

	temp = (color1 >> 11) * 255 + 16;
	r1 = (uint8_t)((temp/32 + temp)/32);
	temp = ((color1 & 0x07E0) >> 5) * 255 + 32;
	g1 = (uint8_t)((temp/64 + temp)/64);
	temp = (color1 & 0x001F) * 255 + 16;
	b1 = (uint8_t)((temp/32 + temp)/32);

	code = *(const uint32_t*)(block + 4);

	for (j = 0; j < 4; ++j) {
		for (i = 0; i < 4; ++i) {
			uint8_t positionCode, finalR, finalG, finalB;

			positionCode = (code >>  2*(4*j+i)) & 0x03;

			if (color0 > color1) {

				switch (positionCode) {
				case 0:
					finalR = r0; finalG = g0; finalB = b0;
					break;
				case 1:
					finalR = r1; finalG = g1; finalB = b1;
					break;
				case 2:

					finalR = (2*r0+r1)/3; finalG = (2*g0+g1)/3; finalB = (2*b0+b1)/3;
					break;
				case 3:
					finalR = (r0+2*r1)/3; finalG = (g0+2*g1)/3; finalB = (b0+2*b1)/3;
					break;
				}
			} else {
				switch (positionCode) {
				case 0:
					finalR = r0; finalG = g0; finalB = b0;
					break;
				case 1:
					finalR = r1; finalG = g1; finalB = b1;
					break;
				case 2:
					finalR = (r0+r1)/2; finalG = (g0+g1)/2; finalB = (b0+b1)/2;
					break;
				case 3:
					finalR = finalG = finalB = 0;
					break;
				}

			}

			output[j*outputStride*3 + i*3] = finalR;
			output[j*outputStride*3 + i*3+1] = finalG;
			output[j*outputStride*3 + i*3+2] = finalB;
		}
	}
}


/*
void DecompressBlockDXT1(): Decompresses one block of a DXT1 texture and stores the resulting pixels at the appropriate offset in 'image'.

uint32_t x:						x-coordinate of the first pixel in the block.
uint32_t y:						y-coordinate of the first pixel in the block.
uint32_t width: 				width of the texture being decompressed.
const uint8_t *blockStorage:	pointer to the block to decompress.
uint32_t *image:				pointer to image where the decompressed pixel data should be stored.
*/ 
DXT_SCALAR_FN void DecompressBlockDXT1_Scalar(uint32_t x, uint32_t y, uint32_t width,
	const uint8_t* blockStorage,
	int transparent0, int* simpleAlpha, int *complexAlpha,
	uint32_t* image)
{

	static const uint8_t const_alpha [] = {
		255, 255, 255, 255,
		255, 255, 255, 255,
		255, 255, 255, 255,
		255, 255, 255, 255
	};


	if( transparent0 )
		DecompressBlockDXT1Internal (blockStorage,
			image + x + (y * width), width, transparent0, simpleAlpha, complexAlpha, const_alpha);
	else
		DecompressBlockDXT1InternalRGB(blockStorage, ((uint8_t*)image) + x*3 + (y*3 * width), width);
}

/*
void DecompressBlockDXT5(): Decompresses one block of a DXT5 texture and stores the resulting pixels at the appropriate offset in 'image'.

uint32_t x:						x-coordinate of the first pixel in the block.
uint32_t y:						y-coordinate of the first pixel in the block.
uint32_t width: 				width of the texture being decompressed.
const uint8_t *blockStorage:	pointer to the block to decompress.
uint32_t *image:				pointer to image where the decompressed pixel data should be stored.
*/ 
DXT_SCALAR_FN void DecompressBlockDXT5_Scalar(uint32_t x, uint32_t y, uint32_t width,
	const uint8_t* blockStorage,
	int transparent0, int* simpleAlpha, int *complexAlpha,
	uint32_t* image)
{
	uint8_t alpha0, alpha1;
	const uint8_t* bits;
	uint32_t alphaCode1;
	uint16_t alphaCode2;

	uint16_t color0, color1;
	uint8_t r0, g0, b0, r1, g1, b1;

	int i, j;

	uint32_t temp, code;

	alpha0 = *(blockStorage);
	alpha1 = *(blockStorage + 1);

	bits = blockStorage + 2;
	alphaCode1 = (uint32_t)bits[2] | ((uint32_t)bits[3] << 8) | ((uint32_t)bits[4] << 16) | ((uint32_t)bits[5] << 24);
	alphaCode2 = bits[0] | (bits[1] << 8);

	color0 = *(const uint16_t*)(blockStorage + 8);
	color1 = *(const uint16_t*)(blockStorage + 10);	

	temp = (color0 >> 11) * 255 + 16;
	r0 = (uint8_t)((temp/32 + temp)/32);
	temp = ((color0 & 0x07E0) >> 5) * 255 + 32;
	g0 = (uint8_t)((temp/64 + temp)/64);
	temp = (color0 & 0x001F) * 255 + 16;
	b0 = (uint8_t)((temp/32 + temp)/32);

	temp = (color1 >> 11) * 255 + 16;
	r1 = (uint8_t)((temp/32 + temp)/32);
	temp = ((color1 & 0x07E0) >> 5) * 255 + 32;
	g1 = (uint8_t)((temp/64 + temp)/64);
	temp = (color1 & 0x001F) * 255 + 16;
	b1 = (uint8_t)((temp/32 + temp)/32);

	code = *(const uint32_t*)(blockStorage + 12);

	for (j = 0; j < 4; j++) {
		for (i = 0; i < 4; i++) {
			uint8_t finalAlpha;
			int alphaCode, alphaCodeIndex;
			uint8_t colorCode;
			uint32_t finalColor;

			alphaCodeIndex = 3*(4*j+i);
			if (alphaCodeIndex <= 12) {
				alphaCode = (alphaCode2 >> alphaCodeIndex) & 0x07;
			} else if (alphaCodeIndex == 15) {
				alphaCode = (alphaCode2 >> 15) | ((alphaCode1 << 1) & 0x06);
			} else /* alphaCodeIndex >= 18 && alphaCodeIndex <= 45 */ {
				alphaCode = (alphaCode1 >> (alphaCodeIndex - 16)) & 0x07;
			}

			if (alphaCode == 0) {
				finalAlpha = alpha0;
			} else if (alphaCode == 1) {
				finalAlpha = alpha1;
			} else {
				if (alpha0 > alpha1) {
					finalAlpha = (uint8_t)(((8-alphaCode)*alpha0 + (alphaCode-1)*alpha1)/7);
				} else {
					if (alphaCode == 6) {
						finalAlpha = 0;
					} else if (alphaCode == 7) {
						finalAlpha = 255;
					} else {
						finalAlpha = (uint8_t)(((6-alphaCode)*alpha0 + (alphaCode-1)*alpha1)/5);
					}
				}
			}

			colorCode = (code >> 2*(4*j+i)) & 0x03; 
			finalColor = 0;

			switch (colorCode) {
			case 0:
				finalColor = PackRGBA(r0, g0, b0, finalAlpha);
				break;
			case 1:
				finalColor = PackRGBA(r1, g1, b1, finalAlpha);
				break;
			case 2:
				finalColor = PackRGBA((2*r0+r1)/3, (2*g0+g1)/3, (2*b0+b1)/3, finalAlpha);
				break;
			case 3:
				finalColor = PackRGBA((r0+2*r1)/3, (g0+2*g1)/3, (b0+2*b1)/3, finalAlpha);
				break;
			}

			if(finalAlpha==0) *simpleAlpha = 1;
			else if(finalAlpha<0xff) *complexAlpha = 1;

			image [i + x + (width* (y+j))] = finalColor; 
		}
	}
}

/*
void DecompressBlockDXT3(): Decompresses one block of a DXT3 texture and stores the resulting pixels at the appropriate offset in 'image'.

uint32_t x:						x-coordinate of the first pixel in the block.
uint32_t y:						y-coordinate of the first pixel in the block.
uint32_t height:				height of the texture being decompressed.
const uint8_t *blockStorage:	pointer to the block to decompress.
uint32_t *image:				pointer to image where the decompressed pixel data should be stored.
*/ 
DXT_SCALAR_FN void DecompressBlockDXT3_Scalar(uint32_t x, uint32_t y, uint32_t width,
	const uint8_t* blockStorage,
	int transparent0, int* simpleAlpha, int *complexAlpha,
	uint32_t* image)
{
	int i;

	uint8_t alphaValues [16] = { 0 };

	for (i = 0; i < 4; ++i) {
		const uint16_t* alphaData = (const uint16_t*) (blockStorage);

		alphaValues [i*4 + 0] = (((*alphaData) >> 0) & 0xF ) * 17;
		alphaValues [i*4 + 1] = (((*alphaData) >> 4) & 0xF ) * 17;
		alphaValues [i*4 + 2] = (((*alphaData) >> 8) & 0xF ) * 17;
		alphaValues [i*4 + 3] = (((*alphaData) >> 12) & 0xF) * 17;

		blockStorage += 2;
	}

	DecompressBlockDXT1Internal (blockStorage,
		image + x + (y * width), width, transparent0, simpleAlpha, complexAlpha, alphaValues);
}

/* ========================================================================== */
/*  NEON block decoders (AArch64)                                              */
/*                                                                            */
/*  Same output as the scalar reference above, byte for byte:                 */
/*   - endpoints are expanded with tables built from the scalar formula        */
/*   - palette interpolation uses the same integer divisions (/3, /2, /5, /7)  */
/*   - the 2-bit / 3-bit selector extraction and the row write order match     */
/*   - simpleAlpha / complexAlpha are set under exactly the same conditions   */
/*  A 4x4 block is decoded with a handful of table lookups instead of 16      */
/*  branchy per-pixel iterations.                                             */
/* ========================================================================== */

#if ARMOPT_NEON64

/* r8 = (t/32 + t)/32 with t = c5*255+16 ;  g8 = (t/64 + t)/64 with t = c6*255+32  (uint32 arithmetic) */
static const uint8_t kDxtExp5[32] =
{
	  0,   8,  16,  25,  33,  41,  49,  58,  66,  74,  82,  90,  99, 107, 115, 123,
	132, 140, 148, 156, 164, 173, 181, 189, 197, 205, 214, 222, 230, 238, 247, 255
};

static const uint8_t kDxtExp6[64] =
{
	  0,   4,   8,  12,  16,  20,  24,  28,  32,  36,  40,  45,  49,  53,  57,  61,
	 65,  69,  73,  77,  81,  85,  89,  93,  97, 101, 105, 109, 113, 117, 121, 125,
	130, 134, 138, 142, 146, 150, 154, 158, 162, 166, 170, 174, 178, 182, 186, 190,
	194, 198, 202, 206, 210, 214, 219, 223, 227, 231, 235, 239, 243, 247, 251, 255
};

/* pixel p = 4*row + col of the block; each pixel owns 4 bytes (R,G,B,A) of its row */
static const uint8_t kDxtRepRow[16] = { 0,0,0,0, 1,1,1,1, 2,2,2,2, 3,3,3,3 };
static const int8_t  kDxtShr2[16]   = { 0,-2,-4,-6, 0,-2,-4,-6, 0,-2,-4,-6, 0,-2,-4,-6 };

/* byte-index builders: lane (4*col + c) <- 4*code(4*row + col) + c */
static const uint8_t kDxtPick[4][16] =
{
	{  0, 0, 0, 0,  1, 1, 1, 1,  2, 2, 2, 2,  3, 3, 3, 3 },
	{  4, 4, 4, 4,  5, 5, 5, 5,  6, 6, 6, 6,  7, 7, 7, 7 },
	{  8, 8, 8, 8,  9, 9, 9, 9, 10,10,10,10, 11,11,11,11 },
	{ 12,12,12,12, 13,13,13,13, 14,14,14,14, 15,15,15,15 }
};
static const uint8_t kDxtAdd4[16] = { 0,1,2,3, 0,1,2,3, 0,1,2,3, 0,1,2,3 };

/* same, but the alpha lane (c == 3) is fetched from a second table (per-pixel alpha, table bytes 16..31) */
static const uint8_t kDxtPick3[4][16] =
{
	{  0, 0, 0,255,  1, 1, 1,255,  2, 2, 2,255,  3, 3, 3,255 },
	{  4, 4, 4,255,  5, 5, 5,255,  6, 6, 6,255,  7, 7, 7,255 },
	{  8, 8, 8,255,  9, 9, 9,255, 10,10,10,255, 11,11,11,255 },
	{ 12,12,12,255, 13,13,13,255, 14,14,14,255, 15,15,15,255 }
};
static const uint8_t kDxtAdd3[4][16] =
{
	{ 0,1,2,16,  0,1,2,17,  0,1,2,18,  0,1,2,19 },
	{ 0,1,2,20,  0,1,2,21,  0,1,2,22,  0,1,2,23 },
	{ 0,1,2,24,  0,1,2,25,  0,1,2,26,  0,1,2,27 },
	{ 0,1,2,28,  0,1,2,29,  0,1,2,30,  0,1,2,31 }
};

/* DXT5 3-bit selectors: 16-bit window (byte b, b+1) per pixel, then a per-pixel right shift */
static const uint8_t kDxt5WinLo[16] = { 0,1, 0,1, 0,1, 1,2, 1,2, 1,2, 2,3, 2,3 };	/* pixels 0..7  */
static const uint8_t kDxt5WinHi[16] = { 3,4, 3,4, 3,4, 4,5, 4,5, 4,5, 5,6, 5,6 };	/* pixels 8..15 */
static const int16_t kDxt5Shr[8]    = { 0, -3, -6, -1, -4, -7, -2, -5 };

#define DXT_PACK( r, g, b, a ) ( (uint32_t)(r) | ( (uint32_t)(g) << 8 ) | ( (uint32_t)(b) << 16 ) | ( (uint32_t)(a) << 24 ) )

/* Palette of a DXT1-style colour block as 4 packed RGBA entries. alpha3 is the alpha of entry 3 when the block
   is in 3-colour mode (color0 <= color1). Returns 1 for 3-colour mode. Same colour maths as the scalar code. */
ARMOPT_FN int Dxt1Palette( uint16_t color0, uint16_t color1, uint32_t alpha3, uint32_t* pal )
{
	uint32_t r0 = kDxtExp5[ color0 >> 11 ];
	uint32_t g0 = kDxtExp6[ ( color0 >> 5 ) & 63 ];
	uint32_t b0 = kDxtExp5[ color0 & 31 ];
	uint32_t r1 = kDxtExp5[ color1 >> 11 ];
	uint32_t g1 = kDxtExp6[ ( color1 >> 5 ) & 63 ];
	uint32_t b1 = kDxtExp5[ color1 & 31 ];

	pal[0] = DXT_PACK( r0, g0, b0, 255 );
	pal[1] = DXT_PACK( r1, g1, b1, 255 );

	if ( color0 > color1 )
	{
		pal[2] = DXT_PACK( ( 2 * r0 + r1 ) / 3, ( 2 * g0 + g1 ) / 3, ( 2 * b0 + b1 ) / 3, 255 );
		pal[3] = DXT_PACK( ( r0 + 2 * r1 ) / 3, ( g0 + 2 * g1 ) / 3, ( b0 + 2 * b1 ) / 3, 255 );
		return 0;
	}

	pal[2] = DXT_PACK( ( r0 + r1 ) / 2, ( g0 + g1 ) / 2, ( b0 + b1 ) / 2, 255 );
	pal[3] = DXT_PACK( 0, 0, 0, alpha3 );
	return 1;
}

/* DXT5 always uses the 4-colour interpolation (it never looks at color0 > color1) */
ARMOPT_FN void Dxt5Palette( uint16_t color0, uint16_t color1, uint32_t* pal )
{
	uint32_t r0 = kDxtExp5[ color0 >> 11 ];
	uint32_t g0 = kDxtExp6[ ( color0 >> 5 ) & 63 ];
	uint32_t b0 = kDxtExp5[ color0 & 31 ];
	uint32_t r1 = kDxtExp5[ color1 >> 11 ];
	uint32_t g1 = kDxtExp6[ ( color1 >> 5 ) & 63 ];
	uint32_t b1 = kDxtExp5[ color1 & 31 ];

	pal[0] = DXT_PACK( r0, g0, b0, 255 );
	pal[1] = DXT_PACK( r1, g1, b1, 255 );
	pal[2] = DXT_PACK( ( 2 * r0 + r1 ) / 3, ( 2 * g0 + g1 ) / 3, ( 2 * b0 + b1 ) / 3, 255 );
	pal[3] = DXT_PACK( ( r0 + 2 * r1 ) / 3, ( g0 + 2 * g1 ) / 3, ( b0 + 2 * b1 ) / 3, 255 );
}

/* 16 two-bit colour selectors, one per byte lane (lane = 4*row + col) */
ARMOPT_FN uint8x16_t Dxt2BitCodes( uint32_t code )
{
	uint8x16_t t = vqtbl1q_u8( vreinterpretq_u8_u32( vdupq_n_u32( code ) ), vld1q_u8( kDxtRepRow ) );
	t = vshlq_u8( t, vld1q_s8( kDxtShr2 ) );
	return vandq_u8( t, vdupq_n_u8( 3 ) );
}

/* one row (4 RGBA pixels) from a 4-entry palette; idx4 = 4 * selector per lane */
ARMOPT_FN uint8x16_t DxtRow4( uint8x16_t pal, uint8x16_t idx4, int j )
{
	uint8x16_t sel = vqtbl1q_u8( idx4, vld1q_u8( kDxtPick[ j ] ) );
	sel = vaddq_u8( sel, vld1q_u8( kDxtAdd4 ) );
	return vqtbl1q_u8( pal, sel );
}

/* one row from the palette (table bytes 0..15) plus a per-pixel alpha vector (table bytes 16..31) */
ARMOPT_FN uint8x16_t DxtRow3( uint8x16x2_t tab, uint8x16_t idx4, int j )
{
	uint8x16_t sel = vqtbl1q_u8( idx4, vld1q_u8( kDxtPick3[ j ] ) );
	sel = vaddq_u8( sel, vld1q_u8( kDxtAdd3[ j ] ) );
	return vqtbl2q_u8( tab, sel );
}

ARMOPT_FN void DxtAlphaFlags( uint8x16_t a, int* simpleAlpha, int* complexAlpha )
{
	if ( vminvq_u8( a ) == 0 )
		*simpleAlpha = 1;												/* some pixel has alpha == 0 */
	if ( vmaxvq_u8( vbicq_u8( vtstq_u8( a, a ), vceqq_u8( a, vdupq_n_u8( 255 ) ) ) ) != 0 )
		*complexAlpha = 1;												/* some pixel has 0 < alpha < 255 */
}

/* DXT1 block with 1-bit alpha ("transparent0"): RGBA, 4 bytes per pixel */
static void Dxt1_Neon_RGBA( const uint8_t* block, uint32_t* output, uint32_t outputStride, int* simpleAlpha )
{
	uint16_t c0, c1;
	uint32_t code, pal[4];
	uint8x16_t palv, idx, idx4;
	int mode3;

	memcpy( &c0, block, 2 );
	memcpy( &c1, block + 2, 2 );
	memcpy( &code, block + 4, 4 );

	mode3 = Dxt1Palette( c0, c1, 0, pal );			/* entry 3 of a 3-colour block is transparent black */
	palv  = vreinterpretq_u8_u32( vld1q_u32( pal ) );
	idx   = Dxt2BitCodes( code );
	idx4  = vshlq_n_u8( idx, 2 );

	vst1q_u8( (uint8_t*)( output ),                                DxtRow4( palv, idx4, 0 ) );
	vst1q_u8( (uint8_t*)( output + (size_t)outputStride ),         DxtRow4( palv, idx4, 1 ) );
	vst1q_u8( (uint8_t*)( output + 2 * (size_t)outputStride ),     DxtRow4( palv, idx4, 2 ) );
	vst1q_u8( (uint8_t*)( output + 3 * (size_t)outputStride ),     DxtRow4( palv, idx4, 3 ) );

	if ( mode3 && vmaxvq_u8( vceqq_u8( idx, vdupq_n_u8( 3 ) ) ) != 0 )
		*simpleAlpha = 1;
}

/* 4 RGBA pixels -> 12 bytes of tightly packed RGB; never writes past the 12 bytes */
ARMOPT_FN void DxtStoreRGB12( uint8_t* dst, uint8x16_t rgba )
{
	static const uint8_t kPackRGB[16] = { 0,1,2, 4,5,6, 8,9,10, 12,13,14, 255,255,255,255 };
	uint8x16_t t = vqtbl1q_u8( rgba, vld1q_u8( kPackRGB ) );
	uint32_t tail;

	vst1_u8( dst, vget_low_u8( t ) );
	tail = vgetq_lane_u32( vreinterpretq_u32_u8( t ), 2 );
	memcpy( dst + 8, &tail, 4 );
}

/* DXT1 block without alpha: tightly packed RGB (3 bytes per pixel); outputStride is the image width in pixels */
static void Dxt1_Neon_RGB( const uint8_t* block, uint8_t* output, uint32_t outputStride )
{
	uint16_t c0, c1;
	uint32_t code, pal[4];
	uint8x16_t palv, idx4;
	const size_t rowBytes = (size_t)outputStride * 3;

	memcpy( &c0, block, 2 );
	memcpy( &c1, block + 2, 2 );
	memcpy( &code, block + 4, 4 );

	(void)Dxt1Palette( c0, c1, 255, pal );			/* 3-colour mode: entry 3 is black; alpha is not stored */
	palv = vreinterpretq_u8_u32( vld1q_u32( pal ) );
	idx4 = vshlq_n_u8( Dxt2BitCodes( code ), 2 );

	DxtStoreRGB12( output,                DxtRow4( palv, idx4, 0 ) );
	DxtStoreRGB12( output + rowBytes,     DxtRow4( palv, idx4, 1 ) );
	DxtStoreRGB12( output + 2 * rowBytes, DxtRow4( palv, idx4, 2 ) );
	DxtStoreRGB12( output + 3 * rowBytes, DxtRow4( palv, idx4, 3 ) );
}

/* DXT3 block: 8 bytes of explicit 4-bit alpha, then a DXT1 colour block */
static void Dxt3_Neon( const uint8_t* block, uint32_t* output, uint32_t outputStride,
					   int transparent0, int* simpleAlpha, int* complexAlpha )
{
	uint16_t c0, c1;
	uint32_t code, pal[4];
	uint8x8_t a8, lo, hi;
	uint8x8x2_t z;
	uint8x16_t nib, alpha, palv, idx, idx4;
	uint8x16x2_t tab;
	int mode3;

	a8  = vld1_u8( block );
	lo  = vand_u8( a8, vdup_n_u8( 0x0F ) );
	hi  = vshr_n_u8( a8, 4 );
	z   = vzip_u8( lo, hi );							/* nibble 0 = low nibble of byte 0, nibble 1 = high nibble, ... */
	nib = vcombine_u8( z.val[0], z.val[1] );
	alpha = vorrq_u8( nib, vshlq_n_u8( nib, 4 ) );		/* nibble * 17 */

	memcpy( &c0, block + 8, 2 );
	memcpy( &c1, block + 10, 2 );
	memcpy( &code, block + 12, 4 );

	mode3 = Dxt1Palette( c0, c1, 255, pal );
	palv  = vreinterpretq_u8_u32( vld1q_u32( pal ) );
	idx   = Dxt2BitCodes( code );
	idx4  = vshlq_n_u8( idx, 2 );

	if ( mode3 && transparent0 )						/* selector 3 of a 3-colour block is fully transparent */
		alpha = vbicq_u8( alpha, vceqq_u8( idx, vdupq_n_u8( 3 ) ) );

	tab.val[0] = palv;
	tab.val[1] = alpha;

	vst1q_u8( (uint8_t*)( output ),                                DxtRow3( tab, idx4, 0 ) );
	vst1q_u8( (uint8_t*)( output + (size_t)outputStride ),         DxtRow3( tab, idx4, 1 ) );
	vst1q_u8( (uint8_t*)( output + 2 * (size_t)outputStride ),     DxtRow3( tab, idx4, 2 ) );
	vst1q_u8( (uint8_t*)( output + 3 * (size_t)outputStride ),     DxtRow3( tab, idx4, 3 ) );

	DxtAlphaFlags( alpha, simpleAlpha, complexAlpha );
}

/* DXT5 block: two alpha endpoints + 48 bits of 3-bit selectors, then a colour block (always 4-colour) */
static void Dxt5_Neon( const uint8_t* block, uint32_t* output, uint32_t outputStride,
					   int* simpleAlpha, int* complexAlpha )
{
	const uint32_t a0 = block[0];
	const uint32_t a1 = block[1];
	uint64_t tbl, bits;
	uint32_t c, code, pal[4];
	uint16_t c0, c1;
	uint8x16_t bv, idx3, alpha, palv, idx4;
	uint16x8_t wlo, whi;
	int16x8_t sh;
	uint8x16x2_t tab;

	/* 8-entry alpha palette, same integer maths as the scalar decoder */
	tbl = (uint64_t)a0 | ( (uint64_t)a1 << 8 );
	if ( a0 > a1 )
	{
		for ( c = 2; c < 8; ++c )
			tbl |= (uint64_t)( ( ( 8 - c ) * a0 + ( c - 1 ) * a1 ) / 7 ) << ( 8 * c );
	}
	else
	{
		for ( c = 2; c < 6; ++c )
			tbl |= (uint64_t)( ( ( 6 - c ) * a0 + ( c - 1 ) * a1 ) / 5 ) << ( 8 * c );
		/* selector 6 -> 0 (already zero), selector 7 -> 255 */
		tbl |= (uint64_t)255 << 56;
	}

	/* 48 selector bits: bytes 2..7. Bytes 8,9 are the first colour, so mask them off. */
	memcpy( &bits, block + 2, 8 );
	bits &= 0x0000FFFFFFFFFFFFULL;

	bv  = vcombine_u8( vcreate_u8( bits ), vdup_n_u8( 0 ) );
	wlo = vreinterpretq_u16_u8( vqtbl1q_u8( bv, vld1q_u8( kDxt5WinLo ) ) );
	whi = vreinterpretq_u16_u8( vqtbl1q_u8( bv, vld1q_u8( kDxt5WinHi ) ) );
	sh  = vld1q_s16( kDxt5Shr );
	wlo = vshlq_u16( wlo, sh );
	whi = vshlq_u16( whi, sh );
	idx3 = vandq_u8( vcombine_u8( vmovn_u16( wlo ), vmovn_u16( whi ) ), vdupq_n_u8( 7 ) );
	alpha = vqtbl1q_u8( vcombine_u8( vcreate_u8( tbl ), vdup_n_u8( 0 ) ), idx3 );

	memcpy( &c0, block + 8, 2 );
	memcpy( &c1, block + 10, 2 );
	memcpy( &code, block + 12, 4 );

	Dxt5Palette( c0, c1, pal );
	palv = vreinterpretq_u8_u32( vld1q_u32( pal ) );
	idx4 = vshlq_n_u8( Dxt2BitCodes( code ), 2 );

	tab.val[0] = palv;
	tab.val[1] = alpha;

	vst1q_u8( (uint8_t*)( output ),                                DxtRow3( tab, idx4, 0 ) );
	vst1q_u8( (uint8_t*)( output + (size_t)outputStride ),         DxtRow3( tab, idx4, 1 ) );
	vst1q_u8( (uint8_t*)( output + 2 * (size_t)outputStride ),     DxtRow3( tab, idx4, 2 ) );
	vst1q_u8( (uint8_t*)( output + 3 * (size_t)outputStride ),     DxtRow3( tab, idx4, 3 ) );

	DxtAlphaFlags( alpha, simpleAlpha, complexAlpha );
}

/* ========================================================================== */
/*  NEON whole-image kernels                                                  */
/*                                                                            */
/*  Used by DecompressImageDXT1/3/5() below. Compared with the per-block      */
/*  decoders above:                                                           */
/*   - the 4-entry palette is built in vector registers (no stores to the     */
/*     stack followed by a vector load, which stalls on in-order cores)       */
/*   - the DXT5 alpha palette is built with vector maths, not a scalar loop   */
/*   - the four row index vectors come from a 4 KB lookup table indexed by    */
/*     the selector byte of that row                                          */
/*   - the output is always RGBA (alpha 255 for opaque DXT1), so the caller   */
/*     needs neither the 3-byte RGB buffer nor a second RGB -> RGBA pass      */
/*   - alpha flags are only computed when the caller asks for them            */
/*   - runs of four complete blocks ( 16 pixels ) are decoded stage by stage  */
/*     ( four palettes, then four alpha tables, then the rows ), so the       */
/*     dependency chains of the four blocks overlap on in-order cores.        */
/*     llvm-mca ( Cortex-A55 / A76 models, estimate only ): about 1.4x - 2x   */
/*     fewer cycles per block than the per-block decoders above               */
/*  Every arithmetic step is the same integer maths as the scalar reference.  */
/* ========================================================================== */

/* Row index vector for a selector byte b (four 2-bit selectors s0..s3):
   lane 4*c + k = 4 * s_c + k, i.e. the byte indices of RGBA palette entry s_c. */
#define DXT_ROW_IDX( b ) { \
	4*(((b)>>0)&3)+0, 4*(((b)>>0)&3)+1, 4*(((b)>>0)&3)+2, 4*(((b)>>0)&3)+3, \
	4*(((b)>>2)&3)+0, 4*(((b)>>2)&3)+1, 4*(((b)>>2)&3)+2, 4*(((b)>>2)&3)+3, \
	4*(((b)>>4)&3)+0, 4*(((b)>>4)&3)+1, 4*(((b)>>4)&3)+2, 4*(((b)>>4)&3)+3, \
	4*(((b)>>6)&3)+0, 4*(((b)>>6)&3)+1, 4*(((b)>>6)&3)+2, 4*(((b)>>6)&3)+3 }
#define DXT_ROW_IDX4( n )  DXT_ROW_IDX( n ), DXT_ROW_IDX( (n) + 1 ), DXT_ROW_IDX( (n) + 2 ), DXT_ROW_IDX( (n) + 3 )
#define DXT_ROW_IDX16( n ) DXT_ROW_IDX4( n ), DXT_ROW_IDX4( (n) + 4 ), DXT_ROW_IDX4( (n) + 8 ), DXT_ROW_IDX4( (n) + 12 )
#define DXT_ROW_IDX64( n ) DXT_ROW_IDX16( n ), DXT_ROW_IDX16( (n) + 16 ), DXT_ROW_IDX16( (n) + 32 ), DXT_ROW_IDX16( (n) + 48 )

static const uint8_t kDxtRowIdx[256][16] =
{
	DXT_ROW_IDX64( 0 ), DXT_ROW_IDX64( 64 ), DXT_ROW_IDX64( 128 ), DXT_ROW_IDX64( 192 )
};

/* DXT3/DXT5: the colour table is palette bytes 0..15, the alpha table is bytes 16..31 of a 32 byte tbl source.
   Alpha lanes (3, 7, 11, 15) take the index of the pixel's alpha byte instead of the palette lookup. */
static const uint8_t kDxtAlphaMask[16] = { 0,0,0,255, 0,0,0,255, 0,0,0,255, 0,0,0,255 };
static const uint8_t kDxtAlphaIdx[4][16] =
{
	{ 0,0,0,16,  0,0,0,17,  0,0,0,18,  0,0,0,19 },
	{ 0,0,0,20,  0,0,0,21,  0,0,0,22,  0,0,0,23 },
	{ 0,0,0,24,  0,0,0,25,  0,0,0,26,  0,0,0,27 },
	{ 0,0,0,28,  0,0,0,29,  0,0,0,30,  0,0,0,31 }
};

/* DXT5 alpha palette: entry c = ( WA[c] * a0 + WB[c] * a1 ) / D, D = 7 when a0 > a1, 5 otherwise.
   Division by a reciprocal multiply (vqdmulh: ( 2 * x * R ) >> 16), exact for every x <= 7 * 255 (checked by the test). */
static const uint16_t kDxt5WA[2][8]  = { { 7, 0, 6, 5, 4, 3, 2, 1 }, { 5, 0, 4, 3, 2, 1, 0, 0 } };
static const uint16_t kDxt5WB[2][8]  = { { 0, 7, 1, 2, 3, 4, 5, 6 }, { 0, 5, 1, 2, 3, 4, 0, 0 } };
static const int16_t  kDxt5Rcp[2][8] = { { 4682, 4682, 4682, 4682, 4682, 4682, 4682, 4682 },
										 { 6554, 6554, 6554, 6554, 6554, 6554, 6554, 6554 } };
static const uint8_t  kDxt5Or[2][8]  = { { 0, 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0, 255 } };	/* 6-entry mode: entry 7 = 255 */

/* DXT3 explicit alpha: 4 bits per pixel, two pixels per byte */
static const uint8_t kDxtNibIdx[16] = { 0,0,1,1,2,2,3,3, 4,4,5,5,6,6,7,7 };
static const int8_t  kDxtNibShl[16] = { 4,0,4,0,4,0,4,0, 4,0,4,0,4,0,4,0 };

/* Colour endpoints. A colour word w = color0 | color1 << 16 becomes [e0 | e1]: eight u16 lanes r g b a of the expanded 8-bit
   endpoint 0, then of endpoint 1 (the alpha lanes come out as 255).
   The 5 / 6 bit channel value c is moved to bit 10 (r, b) or bit 9 (g) of its lane and multiplied with vqrdmulh by 263 / 259, which is
       r, b:  ( c * 263 + 16 ) >> 5          g:  ( c * 259 + 32 ) >> 6
   and equals the scalar (t / 32 + t) / 32 with t = c * 255 + 16 ( (t / 64 + t) / 64 with t = c * 255 + 32 for g ) for every value of c
   (the tests cover all 65536 colour words). One shift, one select and one multiply expand both endpoints, with no separate
   mask / bias / shift steps. */
static const uint8_t  kDxtDupIdx[16] = { 0,1,0,1,0,1,0,1, 2,3,2,3,2,3,2,3 };	/* [c0 c1 c0 c1 ...] -> [c0 x4 | c1 x4] */
static const int16_t  kDxtEShr[8]    = { -1, 4, 10, 0, -1, 4, 10, 0 };
static const uint16_t kDxtEKeep[8]   = { 0x7C00, 0x7E00, 0x7C00, 0, 0x7C00, 0x7E00, 0x7C00, 0 };
static const uint16_t kDxtEPad[8]    = { 0, 0, 0, 0x7C00, 0, 0, 0, 0x7C00 };	/* 31 << 10 in the alpha lane: expands to 255 */
static const int16_t  kDxtEMul[8]    = { 263, 259, 263, 263, 263, 259, 263, 263 };
static const uint16_t kDxtELo[8]     = { 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0, 0, 0, 0 };

typedef struct DxtK
{
	uint8x16_t	dupIdx;
	int16x8_t	eShr;
	uint16x8_t	eKeep, ePad, eLo;
	int16x8_t	eMul;
	int16x8_t	third;			/* 10923: vqdmulh by this is an exact division by 3 for the values used here ( <= 3 * 255 ) */
	uint8x16_t	alphaMask;		/* 0,0,0,255 per pixel */
} DxtK;

ARMOPT_TINY void DxtKInit( DxtK* k )
{
	k->dupIdx    = vld1q_u8( kDxtDupIdx );
	k->eShr      = vld1q_s16( kDxtEShr );
	k->eKeep     = vld1q_u16( kDxtEKeep );
	k->ePad      = vld1q_u16( kDxtEPad );
	k->eLo       = vld1q_u16( kDxtELo );
	k->eMul      = vld1q_s16( kDxtEMul );
	k->third     = vdupq_n_s16( 10923 );
	k->alphaMask = vld1q_u8( kDxtAlphaMask );
}

/* lane 7 = alpha3, everything else 0: entry 3 of a 3-colour palette ( 0, 0, 0, alpha3 ) */
ARMOPT_TINY uint16x8_t DxtAlpha3Vec( uint32_t alpha3 )
{
	return vcombine_u16( vdup_n_u16( 0 ), vcreate_u16( (uint64_t)( alpha3 & 0xFFu ) << 48 ) );
}

/* [e0 | e1] of one colour word */
ARMOPT_TINY uint16x8_t DxtEnds( const DxtK* k, uint32_t w )
{
	uint16x8_t a = vreinterpretq_u16_u8( vqtbl1q_u8( vreinterpretq_u8_u32( vdupq_n_u32( w ) ), k->dupIdx ) );
	a = vshlq_u16( a, k->eShr );
	a = vbslq_u16( k->eKeep, a, k->ePad );
	return vreinterpretq_u16_s16( vqrdmulhq_s16( vreinterpretq_s16_u16( a ), k->eMul ) );
}

/* Palette (4 RGBA entries, 16 bytes) for a 4-colour block: entries 2, 3 = ( 2 * e0 + e1 ) / 3 and ( e0 + 2 * e1 ) / 3. All alpha 255. */
ARMOPT_TINY uint8x16_t DxtPal4( const DxtK* k, uint32_t w )
{
	uint16x8_t e = DxtEnds( k, w );
	uint16x8_t y = vextq_u16( e, e, 4 );											/* [e1 | e0] */
	uint16x8_t s = vaddq_u16( vaddq_u16( e, e ), y );								/* [2 e0 + e1 | 2 e1 + e0] */
	uint16x8_t q = vreinterpretq_u16_s16( vqdmulhq_s16( vreinterpretq_s16_u16( s ), k->third ) );
	return vuzp1q_u8( vreinterpretq_u8_u16( e ), vreinterpretq_u8_u16( q ) );
}

/* Same, and a 3-colour block ( mode3 != 0, color0 <= color1 ) gets entry 2 = ( e0 + e1 ) / 2 and entry 3 = ( 0, 0, 0, alpha3 ) ( p3k = DxtAlpha3Vec() ). */
ARMOPT_TINY uint8x16_t DxtPalG( const DxtK* k, uint32_t w, int mode3, uint16x8_t p3k )
{
	uint16x8_t e = DxtEnds( k, w );
	uint16x8_t y = vextq_u16( e, e, 4 );
	uint16x8_t s = vaddq_u16( vaddq_u16( e, e ), y );
	uint16x8_t q = vreinterpretq_u16_s16( vqdmulhq_s16( vreinterpretq_s16_u16( s ), k->third ) );
	uint16x8_t h = vbslq_u16( k->eLo, vhaddq_u16( e, y ), p3k );					/* [( e0 + e1 ) / 2 | ( 0, 0, 0, alpha3 )] */
	q = vbslq_u16( vdupq_n_u16( (uint16_t)( 0u - (uint32_t)( mode3 != 0 ) ) ), h, q );
	return vuzp1q_u8( vreinterpretq_u8_u16( e ), vreinterpretq_u8_u16( q ) );
}

/* four rows of an RGBA palette lookup */
ARMOPT_TINY void DxtStoreRows4( uint8x16_t pal, uint32_t code, uint8_t* out, size_t strideBytes )
{
	vst1q_u8( out,                      vqtbl1q_u8( pal, vld1q_u8( kDxtRowIdx[ code & 255u ] ) ) );
	vst1q_u8( out + strideBytes,        vqtbl1q_u8( pal, vld1q_u8( kDxtRowIdx[ ( code >> 8 ) & 255u ] ) ) );
	vst1q_u8( out + 2 * strideBytes,    vqtbl1q_u8( pal, vld1q_u8( kDxtRowIdx[ ( code >> 16 ) & 255u ] ) ) );
	vst1q_u8( out + 3 * strideBytes,    vqtbl1q_u8( pal, vld1q_u8( kDxtRowIdx[ code >> 24 ] ) ) );
}

/* one row with per-pixel alpha: colour lookup and alpha lookup in one two-register tbl ( table bytes 0..15 = palette, 16..31 = alpha[16] in pixel order ) */
ARMOPT_TINY uint8x16_t DxtRowAlpha( const DxtK* k, uint8x16x2_t tab, uint32_t sel, int r )
{
	return vqtbl2q_u8( tab, vbslq_u8( k->alphaMask, vld1q_u8( kDxtAlphaIdx[ r ] ), vld1q_u8( kDxtRowIdx[ sel ] ) ) );
}

/* four rows of colour lookup + per-pixel alpha (alpha[16] in pixel order) */
ARMOPT_TINY void DxtStoreRows3( const DxtK* k, uint8x16_t pal, uint8x16_t alpha, uint32_t code, uint8_t* out, size_t strideBytes )
{
	uint8x16x2_t tab;

	tab.val[0] = pal;
	tab.val[1] = alpha;

	vst1q_u8( out,                      DxtRowAlpha( k, tab, code & 255u, 0 ) );
	vst1q_u8( out + strideBytes,        DxtRowAlpha( k, tab, ( code >> 8 ) & 255u, 1 ) );
	vst1q_u8( out + 2 * strideBytes,    DxtRowAlpha( k, tab, ( code >> 16 ) & 255u, 2 ) );
	vst1q_u8( out + 3 * strideBytes,    DxtRowAlpha( k, tab, code >> 24, 3 ) );
}

/* DXT3: the 8 alpha bytes at p -> 16 alpha bytes in pixel order ( nibble * 17 ) */
ARMOPT_TINY uint8x16_t DxtAlpha3( const uint8_t* p )
{
	uint8x16_t t = vqtbl1q_u8( vcombine_u8( vld1_u8( p ), vdup_n_u8( 0 ) ), vld1q_u8( kDxtNibIdx ) );	/* every byte twice */
	t = vshlq_u8( t, vld1q_s8( kDxtNibShl ) );															/* even pixel: low nibble up, odd pixel: high nibble stays */
	t = vandq_u8( t, vdupq_n_u8( 0xF0 ) );
	return vsraq_n_u8( t, t, 4 );																		/* n << 4 + n = n * 17 */
}

/* DXT5: the 8 alpha bytes at p ( two endpoints + 48 bits of 3-bit selectors ) -> 16 alpha bytes in pixel order */
ARMOPT_TINY uint8x16_t DxtAlpha5( const uint8_t* p )
{
	const uint32_t a0 = p[0];
	const uint32_t a1 = p[1];
	const int m5 = ( a0 > a1 ) ? 0 : 1;					/* 0: 8-entry palette ( / 7 ), 1: 6-entry palette ( / 5 ) */
	uint16x8_t x, wlo, whi;
	uint8x8_t pal8;
	uint8x16_t bv, idx3;
	int16x8_t sh;

	/* 8-entry alpha palette */
	x = vmlaq_u16( vmulq_u16( vdupq_n_u16( (uint16_t)a0 ), vld1q_u16( kDxt5WA[ m5 ] ) ),
				   vdupq_n_u16( (uint16_t)a1 ), vld1q_u16( kDxt5WB[ m5 ] ) );
	x = vreinterpretq_u16_s16( vqdmulhq_s16( vreinterpretq_s16_u16( x ), vld1q_s16( kDxt5Rcp[ m5 ] ) ) );
	pal8 = vorr_u8( vmovn_u16( x ), vld1_u8( kDxt5Or[ m5 ] ) );

	/* selectors: bytes 2..7 of the block. The 8 bytes read here also include the first two colour bytes, but every selector's
	   16-bit window ends at byte 7 at the latest and the & 7 below drops everything above the selector's three bits. */
	bv  = vcombine_u8( vld1_u8( p + 2 ), vdup_n_u8( 0 ) );
	wlo = vreinterpretq_u16_u8( vqtbl1q_u8( bv, vld1q_u8( kDxt5WinLo ) ) );
	whi = vreinterpretq_u16_u8( vqtbl1q_u8( bv, vld1q_u8( kDxt5WinHi ) ) );
	sh  = vld1q_s16( kDxt5Shr );
	wlo = vshlq_u16( wlo, sh );
	whi = vshlq_u16( whi, sh );
	idx3 = vandq_u8( vuzp1q_u8( vreinterpretq_u8_u16( wlo ), vreinterpretq_u8_u16( whi ) ), vdupq_n_u8( 7 ) );
	return vqtbl1q_u8( vcombine_u8( pal8, vdup_n_u8( 0 ) ), idx3 );
}

/* ---- one block ---- */

/* DXT1 block -> 4 rows of RGBA. alpha3 = 0 gives the 1-bit-alpha behaviour (transparent0), 255 keeps every texel opaque. */
ARMOPT_TINY void DxtBlock1( const DxtK* k, const uint8_t* b, uint8_t* out, size_t strideBytes, uint32_t alpha3, int* simpleAlpha )
{
	uint32_t w, code;
	int mode3;

	memcpy( &w, b, 4 );
	memcpy( &code, b + 4, 4 );
	mode3 = ( w & 0xFFFFu ) <= ( w >> 16 );

	DxtStoreRows4( DxtPalG( k, w, mode3, DxtAlpha3Vec( alpha3 ) ), code, out, strideBytes );

	/* a texel is transparent when its selector is 3 in a 3-colour block ( both selector bits set ) */
	if ( simpleAlpha && mode3 && alpha3 == 0 && ( code & ( code >> 1 ) & 0x55555555u ) != 0 )
		*simpleAlpha = 1;
}

/* DXT3 block: 8 bytes of 4-bit alpha, then a DXT1 colour block (3-colour mode allowed, like the scalar reference) */
ARMOPT_TINY void DxtBlock3( const DxtK* k, const uint8_t* b, uint8_t* out, size_t strideBytes, int transparent0, int* simpleAlpha, int* complexAlpha )
{
	uint32_t w, code;
	uint8x16_t alpha;
	int mode3;

	alpha = DxtAlpha3( b );

	memcpy( &w, b + 8, 4 );
	memcpy( &code, b + 12, 4 );
	mode3 = ( w & 0xFFFFu ) <= ( w >> 16 );

	if ( mode3 && transparent0 )						/* selector 3 of a 3-colour block is fully transparent */
		alpha = vbicq_u8( alpha, vceqq_u8( Dxt2BitCodes( code ), vdupq_n_u8( 3 ) ) );

	DxtStoreRows3( k, DxtPalG( k, w, mode3, DxtAlpha3Vec( 255 ) ), alpha, code, out, strideBytes );

	if ( simpleAlpha && complexAlpha )
		DxtAlphaFlags( alpha, simpleAlpha, complexAlpha );
}

/* DXT5 block: two alpha endpoints + 48 bits of 3-bit selectors, then a colour block (always 4-colour) */
ARMOPT_TINY void DxtBlock5( const DxtK* k, const uint8_t* b, uint8_t* out, size_t strideBytes, int* simpleAlpha, int* complexAlpha )
{
	uint32_t w, code;
	uint8x16_t alpha;

	alpha = DxtAlpha5( b );

	memcpy( &w, b + 8, 4 );
	memcpy( &code, b + 12, 4 );

	DxtStoreRows3( k, DxtPal4( k, w ), alpha, code, out, strideBytes );

	if ( simpleAlpha && complexAlpha )
		DxtAlphaFlags( alpha, simpleAlpha, complexAlpha );
}

/* ---- four blocks side by side ( 16 pixels wide ) ----
   Every stage runs for all four blocks before the next stage starts. The blocks are independent, so the long dependency
   chains of the palette maths overlap instead of each stalling an in-order core, and the constants stay in registers.
   Only for complete blocks, and when the caller does not want the alpha flags. */
#define DXT_X4( ... ) \
	{ const int j = 0; __VA_ARGS__ } \
	{ const int j = 1; __VA_ARGS__ } \
	{ const int j = 2; __VA_ARGS__ } \
	{ const int j = 3; __VA_ARGS__ }

/* rows 0..3 of four palettes ( pal[j] ) with the selector words code[j]; rowStore( r, j ) writes row r of block j */
#define DXT_ROWS4( ... ) \
	{ const int r = 0; DXT_X4( __VA_ARGS__ ) } \
	{ const int r = 1; DXT_X4( __VA_ARGS__ ) } \
	{ const int r = 2; DXT_X4( __VA_ARGS__ ) } \
	{ const int r = 3; DXT_X4( __VA_ARGS__ ) }

ARMOPT_TINY void DxtGroup1( const DxtK* k, const uint8_t* b, uint8_t* out, size_t st, uint32_t alpha3 )
{
	uint32_t w[4], code[4];
	uint8x16_t pal[4];
	int any3 = 0;

	DXT_X4( memcpy( &w[j], b + 8 * j, 4 ); memcpy( &code[j], b + 8 * j + 4, 4 ); any3 |= ( ( w[j] & 0xFFFFu ) <= ( w[j] >> 16 ) ); )

	if ( any3 )
	{
		const uint16x8_t p3k = DxtAlpha3Vec( alpha3 );
		DXT_X4( pal[j] = DxtPalG( k, w[j], ( w[j] & 0xFFFFu ) <= ( w[j] >> 16 ), p3k ); )
	}
	else
	{
		DXT_X4( pal[j] = DxtPal4( k, w[j] ); )
	}

	DXT_ROWS4( vst1q_u8( out + 16 * j + r * st, vqtbl1q_u8( pal[j], vld1q_u8( kDxtRowIdx[ ( code[j] >> ( 8 * r ) ) & 255u ] ) ) ); )
}

/* returns 0 when it cannot take the group ( transparent0 with a 3-colour block ): the caller decodes the four blocks one by one */
ARMOPT_TINY int DxtGroup3( const DxtK* k, const uint8_t* b, uint8_t* out, size_t st, int transparent0 )
{
	uint32_t w[4], code[4];
	uint8x16_t pal[4], al[4];
	int any3 = 0;

	DXT_X4( memcpy( &w[j], b + 16 * j + 8, 4 ); memcpy( &code[j], b + 16 * j + 12, 4 ); any3 |= ( ( w[j] & 0xFFFFu ) <= ( w[j] >> 16 ) ); )

	if ( any3 && transparent0 )
		return 0;

	DXT_X4( al[j] = DxtAlpha3( b + 16 * j ); )

	if ( any3 )
	{
		const uint16x8_t p3k = DxtAlpha3Vec( 255 );
		DXT_X4( pal[j] = DxtPalG( k, w[j], ( w[j] & 0xFFFFu ) <= ( w[j] >> 16 ), p3k ); )
	}
	else
	{
		DXT_X4( pal[j] = DxtPal4( k, w[j] ); )
	}

	DXT_ROWS4( { uint8x16x2_t tab; tab.val[0] = pal[j]; tab.val[1] = al[j];
				 vst1q_u8( out + 16 * j + r * st, DxtRowAlpha( k, tab, ( code[j] >> ( 8 * r ) ) & 255u, r ) ); } )
	return 1;
}

ARMOPT_TINY void DxtGroup5( const DxtK* k, const uint8_t* b, uint8_t* out, size_t st )
{
	uint32_t w[4], code[4];
	uint8x16_t pal[4], al[4];

	DXT_X4( memcpy( &w[j], b + 16 * j + 8, 4 ); memcpy( &code[j], b + 16 * j + 12, 4 ); )
	DXT_X4( al[j] = DxtAlpha5( b + 16 * j ); )
	DXT_X4( pal[j] = DxtPal4( k, w[j] ); )

	DXT_ROWS4( { uint8x16x2_t tab; tab.val[0] = pal[j]; tab.val[1] = al[j];
				 vst1q_u8( out + 16 * j + r * st, DxtRowAlpha( k, tab, ( code[j] >> ( 8 * r ) ) & 255u, r ) ); } )
}

#endif /* ARMOPT_NEON64 */

/* ========================================================================== */
/*  Public entry points                                                       */
/* ========================================================================== */

void DecompressBlockDXT1(uint32_t x, uint32_t y, uint32_t width,
	const uint8_t* blockStorage,
	int transparent0, int* simpleAlpha, int *complexAlpha,
	uint32_t* image)
{
#if ARMOPT_NEON64
	(void)complexAlpha;		/* a DXT1 block can only be fully opaque or fully transparent per pixel */
	if( transparent0 )
		Dxt1_Neon_RGBA( blockStorage, image + x + (y * width), width, simpleAlpha );
	else
		Dxt1_Neon_RGB( blockStorage, ((uint8_t*)image) + x*3 + (y*3 * width), width );
#else
	DecompressBlockDXT1_Scalar( x, y, width, blockStorage, transparent0, simpleAlpha, complexAlpha, image );
#endif
}

void DecompressBlockDXT5(uint32_t x, uint32_t y, uint32_t width,
	const uint8_t* blockStorage,
	int transparent0, int* simpleAlpha, int *complexAlpha,
	uint32_t* image)
{
#if ARMOPT_NEON64
	(void)transparent0;		/* DXT5 ignores it */
	Dxt5_Neon( blockStorage, image + x + (y * width), width, simpleAlpha, complexAlpha );
#else
	DecompressBlockDXT5_Scalar( x, y, width, blockStorage, transparent0, simpleAlpha, complexAlpha, image );
#endif
}

void DecompressBlockDXT3(uint32_t x, uint32_t y, uint32_t width,
	const uint8_t* blockStorage,
	int transparent0, int* simpleAlpha, int *complexAlpha,
	uint32_t* image)
{
#if ARMOPT_NEON64
	Dxt3_Neon( blockStorage, image + x + (y * width), width, transparent0, simpleAlpha, complexAlpha );
#else
	DecompressBlockDXT3_Scalar( x, y, width, blockStorage, transparent0, simpleAlpha, complexAlpha, image );
#endif
}

/* ========================================================================== */
/*  Whole-image decoders                                                      */
/*                                                                            */
/*  DecompressImageDXT1/3/5() decode a complete mip level ( ceil(w/4) * ceil(h/4) blocks, row major )  */
/*  into tightly packed RGBA8: row pitch = width pixels, exactly width * height * 4 bytes are written, */
/*  also when width / height are not multiples of 4 ( edge blocks are clipped ).                       */
/*                                                                            */
/*  Compared with calling DecompressBlockDXT*() once per block:               */
/*   - DXT1 without alpha ( transparent0 == 0 ) comes out as RGBA with alpha 255, so the caller no     */
/*     longer needs a 3-byte RGB buffer plus a second RGB -> RGBA pass                                */
/*   - edge blocks never write outside the image ( the per-block scalar code wraps rows when the      */
/*     width is not a multiple of 4 )                                                                 */
/*   - simpleAlpha / complexAlpha are only ever set to 1, as before, and only computed when asked for  */
/*  Visible pixels and the alpha flags are bit-identical to the per-block decoders.                   */
/* ========================================================================== */

typedef void ( *DxtScalarBlockFn )( uint32_t x, uint32_t y, uint32_t width, const uint8_t* blockStorage,
									int transparent0, int* simpleAlpha, int* complexAlpha, uint32_t* image );

/* DecompressBlockDXT1_Scalar() writes packed RGB when transparent0 is 0; the image decoder wants RGBA with alpha 255 */
DXT_SCALAR_FN void DxtBlock1Rgba_Scalar( uint32_t x, uint32_t y, uint32_t width, const uint8_t* blockStorage,
										 int transparent0, int* simpleAlpha, int* complexAlpha, uint32_t* image )
{
	static const uint8_t const_alpha [] = {
		255, 255, 255, 255,
		255, 255, 255, 255,
		255, 255, 255, 255,
		255, 255, 255, 255
	};

	DecompressBlockDXT1Internal( blockStorage, image + x + ( (size_t)y * width ), width,
								 transparent0, simpleAlpha, complexAlpha, const_alpha );
}

/* Portable reference: block by block through the scalar decoders, edge blocks via a 4x4 scratch block */
DXT_SCALAR_FN void DxtImage_Scalar( DxtScalarBlockFn fn, uint32_t blockBytes, uint32_t width, uint32_t height,
									const uint8_t* blocks, int transparent0, int* simpleAlpha, int* complexAlpha,
									uint32_t* rgba )
{
	const uint32_t bw = ( width + 3 ) >> 2;
	const uint32_t bh = ( height + 3 ) >> 2;
	uint32_t bx, by, j;
	uint32_t tmp[16];
	int sa = 0, ca = 0;

	for ( by = 0; by < bh; ++by )
	{
		for ( bx = 0; bx < bw; ++bx )
		{
			const uint8_t* blk = blocks + ( (size_t)by * bw + bx ) * blockBytes;
			const uint32_t x = bx * 4;
			const uint32_t y = by * 4;

			if ( x + 4 <= width && y + 4 <= height )
			{
				fn( x, y, width, blk, transparent0, &sa, &ca, rgba );
			}
			else
			{
				const uint32_t vw = ( width - x < 4 ) ? ( width - x ) : 4;
				const uint32_t vh = ( height - y < 4 ) ? ( height - y ) : 4;

				fn( 0, 0, 4, blk, transparent0, &sa, &ca, tmp );
				for ( j = 0; j < vh; ++j )
					memcpy( rgba + (size_t)( y + j ) * width + x, tmp + j * 4, (size_t)vw * 4 );
			}
		}
	}

	if ( simpleAlpha && sa )
		*simpleAlpha = 1;
	if ( complexAlpha && ca )
		*complexAlpha = 1;
}

#if ARMOPT_NEON64

/* one block, selected at compile time ( kind is a constant after inlining ) */
ARMOPT_TINY void DxtBlockAny( const DxtK* k, int kind, const uint8_t* blk, uint8_t* out, size_t strideBytes, int transparent0,
							  uint32_t alpha3, int* sa, int* ca )
{
	if ( kind == 1 )
		DxtBlock1( k, blk, out, strideBytes, alpha3, sa );
	else if ( kind == 3 )
		DxtBlock3( k, blk, out, strideBytes, transparent0, sa, ca );
	else
		DxtBlock5( k, blk, out, strideBytes, sa, ca );
}

/* four complete blocks side by side; 0 = the group kernel cannot take them ( the caller decodes them one by one ) */
ARMOPT_TINY int DxtGroupAny( const DxtK* k, int kind, const uint8_t* blk, uint8_t* out, size_t strideBytes, int transparent0,
							 uint32_t alpha3 )
{
	if ( kind == 1 )
	{
		DxtGroup1( k, blk, out, strideBytes, alpha3 );
		return 1;
	}
	if ( kind == 3 )
		return DxtGroup3( k, blk, out, strideBytes, transparent0 );

	DxtGroup5( k, blk, out, strideBytes );
	return 1;
}

ARMOPT_TINY void DxtImage_Neon( int kind, uint32_t width, uint32_t height, const uint8_t* blocks, int transparent0,
								int* simpleAlpha, int* complexAlpha, uint32_t* rgba )
{
	const uint32_t bw = ( width + 3 ) >> 2;
	const uint32_t bh = ( height + 3 ) >> 2;
	const uint32_t fullBw = width >> 2;							/* complete blocks per row */
	const size_t blockBytes = ( kind == 1 ) ? 8 : 16;
	const size_t strideBytes = (size_t)width * 4;
	const uint32_t alpha3 = transparent0 ? 0u : 255u;			/* DXT1 only: alpha of selector 3 in a 3-colour block */
	const int wantFlags = ( simpleAlpha != NULL ) || ( complexAlpha != NULL );
	int sa = 0, ca = 0;
	int* psa = wantFlags ? &sa : NULL;
	int* pca = wantFlags ? &ca : NULL;
	uint8_t* dst = (uint8_t*)rgba;
	uint8_t tmp[64];
	uint32_t bx, by, j;
	DxtK k;

	DxtKInit( &k );

	for ( by = 0; by < bh; ++by )
	{
		const uint32_t y = by * 4;
		const uint8_t* blk = blocks + (size_t)by * bw * blockBytes;
		uint8_t* row = dst + (size_t)y * strideBytes;

		bx = 0;

		/* runs of four complete blocks ( 16 pixels ) go through the group kernels; they do not compute the alpha flags */
		if ( !wantFlags && y + 4 <= height )
		{
			for ( ; bx + 4 <= fullBw; bx += 4, blk += 4 * blockBytes )
			{
				uint8_t* o = row + (size_t)bx * 16;

				if ( !DxtGroupAny( &k, kind, blk, o, strideBytes, transparent0, alpha3 ) )
				{
					for ( j = 0; j < 4; ++j )
						DxtBlockAny( &k, kind, blk + j * blockBytes, o + (size_t)j * 16, strideBytes, transparent0, alpha3, NULL, NULL );
				}
			}
		}

		for ( ; bx < bw; ++bx, blk += blockBytes )
		{
			const uint32_t x = bx * 4;

			if ( x + 4 <= width && y + 4 <= height )
			{
				DxtBlockAny( &k, kind, blk, row + (size_t)x * 4, strideBytes, transparent0, alpha3, psa, pca );
			}
			else
			{
				/* edge block: decode the whole 4x4 into scratch, copy only the visible part */
				const uint32_t vw = ( width - x < 4 ) ? ( width - x ) : 4;
				const uint32_t vh = ( height - y < 4 ) ? ( height - y ) : 4;

				DxtBlockAny( &k, kind, blk, tmp, 16, transparent0, alpha3, psa, pca );
				for ( j = 0; j < vh; ++j )
					memcpy( row + (size_t)j * strideBytes + (size_t)x * 4, tmp + (size_t)j * 16, (size_t)vw * 4 );
			}
		}
	}

	if ( simpleAlpha && sa )
		*simpleAlpha = 1;
	if ( complexAlpha && ca )
		*complexAlpha = 1;
}

#endif /* ARMOPT_NEON64 */

void DecompressImageDXT1( uint32_t width, uint32_t height, const uint8_t* blocks,
	int transparent0, int* simpleAlpha, int* complexAlpha,
	uint32_t* rgba )
{
	if ( !blocks || !rgba || !width || !height )
		return;
#if ARMOPT_NEON64
	DxtImage_Neon( 1, width, height, blocks, transparent0, simpleAlpha, complexAlpha, rgba );
#else
	DxtImage_Scalar( DxtBlock1Rgba_Scalar, 8, width, height, blocks, transparent0, simpleAlpha, complexAlpha, rgba );
#endif
}

void DecompressImageDXT3( uint32_t width, uint32_t height, const uint8_t* blocks,
	int transparent0, int* simpleAlpha, int* complexAlpha,
	uint32_t* rgba )
{
	if ( !blocks || !rgba || !width || !height )
		return;
#if ARMOPT_NEON64
	DxtImage_Neon( 3, width, height, blocks, transparent0, simpleAlpha, complexAlpha, rgba );
#else
	DxtImage_Scalar( DecompressBlockDXT3_Scalar, 16, width, height, blocks, transparent0, simpleAlpha, complexAlpha, rgba );
#endif
}

void DecompressImageDXT5( uint32_t width, uint32_t height, const uint8_t* blocks,
	int transparent0, int* simpleAlpha, int* complexAlpha,
	uint32_t* rgba )
{
	if ( !blocks || !rgba || !width || !height )
		return;
	(void)transparent0;		/* DXT5 ignores it */
#if ARMOPT_NEON64
	DxtImage_Neon( 5, width, height, blocks, 0, simpleAlpha, complexAlpha, rgba );
#else
	DxtImage_Scalar( DecompressBlockDXT5_Scalar, 16, width, height, blocks, 0, simpleAlpha, complexAlpha, rgba );
#endif
}
