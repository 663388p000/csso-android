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

ARM / Mali port: on AArch64 the three public entry points below run NEON block decoders (see the
"NEON block decoders" section at the end of this file). They produce exactly the same bytes and the
same simpleAlpha / complexAlpha flags as the scalar code in this file, which is kept as the reference
(DecompressBlockDXT*_Scalar) and is what runs on every other CPU or when TOGL_DISABLE_ARM_NEON is defined.

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
