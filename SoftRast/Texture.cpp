#include <string.h>
#include <intrin.h>
#include <immintrin.h>

#include <kt/Memory.h>
#include <kt/Logging.h>

#include "Texture.h"
#include "stb_image.h"
#include "stb_image_resize.h"

#define SR_TILE_TEXTURES (1)

namespace kt
{

template <>
void Serialize(ISerializer* _s, sr::Tex::TextureData& _tex)
{
	Serialize(_s, _tex.m_texels);
	Serialize(_s, _tex.m_widthLog2);
	Serialize(_s, _tex.m_heightLog2);
	Serialize(_s, _tex.m_bytesPerPixel);
	Serialize(_s, _tex.m_mipOffsets);
	Serialize(_s, _tex.m_numMips);
}

}

namespace sr
{

namespace Tex
{

static uint32_t MortonEncode(uint32_t _x, uint32_t _y)
{
	constexpr uint32_t pdep_x_mask = 0x55555555; // 0b010101 ...
	constexpr uint32_t pdep_y_mask = 0xAAAAAAAA; // 0b101010 ...
	return _pdep_u32(_x, pdep_x_mask) | _pdep_u32(_y, pdep_y_mask);
}

// https://lemire.me/blog/2018/01/09/how-fast-can-you-bit-interleave-32-bit-integers-simd-edition/

__m256i interleave_uint8_with_zeros_avx_lut(__m256i word)
{
	const __m256i m = _mm256_set_epi8(85, 84, 81, 80, 69, 68,
									  65, 64, 21, 20, 17, 16, 5, 4, 1, 0, 85, 84,
									  81, 80, 69, 68, 65, 64, 21, 20, 17, 16, 5,
									  4, 1, 0);
	__m256i lownibbles =
		_mm256_shuffle_epi8(m, _mm256_and_si256(word,
												_mm256_set1_epi8(0xf)));
	__m256i highnibbles = _mm256_and_si256(word,
										   _mm256_set1_epi8(0xf0));
	highnibbles = _mm256_srli_epi16(highnibbles, 4);
	highnibbles = _mm256_shuffle_epi8(m, highnibbles);
	highnibbles = _mm256_slli_epi16(highnibbles, 8);
	return _mm256_or_si256(lownibbles, highnibbles);
}

static __m256i MortonEncode_AVX(__m256i _x, __m256i _y)
{
 	__m256i const interleaved_x = interleave_uint8_with_zeros_avx_lut(_x);
	__m256i const interleaved_y = interleave_uint8_with_zeros_avx_lut(_y);
	return _mm256_or_si256(interleaved_x, _mm256_slli_epi32(interleaved_y, 1));
}

constexpr uint32_t c_texTileSizeLog2 = 5;
constexpr uint32_t c_texTileSize = 1 << c_texTileSizeLog2;
constexpr uint32_t c_texTileMask = c_texTileSize - 1;

static void TileTexture(uint8_t const* _src, uint8_t* _dest, uint32_t const dimX_noPad, uint32_t const dimY_noPad)
{
#if SR_TILE_TEXTURES
	uint32_t const mipTileWidth = uint32_t(kt::AlignUp(dimX_noPad, c_texTileSize)) >> c_texTileSizeLog2;

	// Naive tile+swizzling of texture. 
	// Go over the texture in linear order, find the tiled offset and morton code the inner tile coordinates.
	for (uint32_t yy = 0; yy < dimY_noPad; ++yy)
	{
		for (uint32_t xx = 0; xx < dimX_noPad; ++xx)
		{
			uint32_t const linearOffs = yy * dimX_noPad + xx;

			uint32_t const tileX = xx >> c_texTileSizeLog2;
			uint32_t const tileY = yy >> c_texTileSizeLog2;

			uint32_t const inTileAddressX = xx & c_texTileMask;
			uint32_t const inTileAddressY = yy & c_texTileMask;

			uint32_t const morton = MortonEncode(inTileAddressX, inTileAddressY);

			uint32_t const tiledOffs = (tileY * mipTileWidth + tileX) * (c_texTileSize * c_texTileSize) + morton;
			memcpy(_dest + tiledOffs * 4, _src + linearOffs * 4, 4);
		}
	}
#else
	memcpy(_dest, _src, dimX_noPad * dimY_noPad * sizeof(uint32_t));
#endif
}

void TextureData::CreateFromFile(char const* _file)
{
	Clear();
	static uint32_t const req_comp = 4;
	int x, y, comp;
	uint8_t* srcImageData = stbi_load(_file, &x, &y, &comp, req_comp);
	if (!srcImageData)
	{
		KT_LOG_ERROR("Failed to load texture: %s", _file);
		return;
	}

	KT_SCOPE_EXIT(stbi_image_free(srcImageData));
	CreateFromRGBA8(srcImageData, uint32_t(x), uint32_t(y), true);
}

void TextureData::CreateFromRGBA8(uint8_t const* _texels, uint32_t _width, uint32_t _height, bool _calcMips /*= false*/)
{

	KT_ASSERT(kt::IsPow2(_width) && kt::IsPow2(_height));

	m_widthLog2 = kt::FloorLog2(uint32_t(_width));
	m_heightLog2 = kt::FloorLog2(uint32_t(_height));

	KT_ASSERT(m_heightLog2 < Config::c_maxTexDimLog2);
	KT_ASSERT(m_widthLog2 < Config::c_maxTexDimLog2);
	KT_ASSERT((_width % c_texTileSize) == 0);
	KT_ASSERT((_height % c_texTileSize) == 0); // TODO: Pad if this isn't true (but will be with 2^x x>=5)

	m_bytesPerPixel = 4;

	if (!_calcMips)
	{
		m_numMips = 1;
		m_mipOffsets[0] = 0;

		m_texels.Resize(_width * _height * m_bytesPerPixel);
		TileTexture(_texels, m_texels.Data(), _width, _height);
		return;
	}

	// Calculate mips.

	uint32_t const fullMipChainLen = kt::FloorLog2(kt::Max(uint32_t(_width), uint32_t(_height))) + 1; // +1 for base tex.
	m_numMips = fullMipChainLen;

	struct MipInfo
	{
		uint32_t m_offs;
		uint32_t m_dims[2];
	};

	uint32_t curMipDataOffset = 0;

	MipInfo* mipInfos = (MipInfo*)KT_ALLOCA(sizeof(MipInfo) * fullMipChainLen);

	for (uint32_t mipIdx = 0; mipIdx < fullMipChainLen; ++mipIdx)
	{
		CalcMipDims2D(uint32_t(_width), uint32_t(_height), mipIdx, mipInfos[mipIdx].m_dims);
		mipInfos[mipIdx].m_offs = curMipDataOffset;
		m_mipOffsets[mipIdx] = curMipDataOffset;

		// Align the offset to account for tiling
#if SR_TILE_TEXTURES
		uint32_t const mipDimX_tilePad = uint32_t(kt::AlignUp(mipInfos[mipIdx].m_dims[0], c_texTileSize));
		uint32_t const mipDimY_tilePad = uint32_t(kt::AlignUp(mipInfos[mipIdx].m_dims[1], c_texTileSize));
#else
		uint32_t const mipDimX_tilePad = mipInfos[mipIdx].m_dims[0];
		uint32_t const mipDimY_tilePad = mipInfos[mipIdx].m_dims[1];
#endif

		curMipDataOffset += mipDimX_tilePad * mipDimY_tilePad * m_bytesPerPixel;
	}


	m_texels.Resize(curMipDataOffset);
	uint8_t* texWritePointer = m_texels.Data();

	// tile mip 0
	TileTexture(_texels, texWritePointer, mipInfos[0].m_dims[0], mipInfos[0].m_dims[1]);

	uint32_t const largestMipSize = _width * _height * m_bytesPerPixel;
	uint8_t* tempResizeBuff = (uint8_t*)kt::Malloc(largestMipSize);
	KT_SCOPE_EXIT(kt::Free(tempResizeBuff));

	for (uint32_t mipIdx = 1; mipIdx < fullMipChainLen; ++mipIdx)
	{
		MipInfo const& mipInfo = mipInfos[mipIdx];
		uint8_t* mipPtr = texWritePointer + mipInfo.m_offs;

		uint32_t const mipDimX = mipInfo.m_dims[0];
		uint32_t const mipDimY = mipInfo.m_dims[1];

		stbir_resize_uint8(_texels, _width, _height, 0, tempResizeBuff, mipDimX, mipDimY, 0, m_bytesPerPixel);
		TileTexture(tempResizeBuff, mipPtr, mipDimX, mipDimY);
	}
}

void TextureData::Clear()
{
	m_texels.ClearAndFree();
}

void CalcMipDims2D(uint32_t _x, uint32_t _y, uint32_t _level, uint32_t o_dims[2])
{
	o_dims[0] = kt::Max<uint32_t>(1u, _x >> _level);
	o_dims[1] = kt::Max<uint32_t>(1u, _y >> _level);
}

__m256i CalcMipLevels(TextureData const& _tex, __m256 _dudx, __m256 _dudy, __m256 _dvdx, __m256 _dvdy)
{
	__m256 const height = _mm256_set1_ps(float(1u << _tex.m_heightLog2));
	__m256 const width = _mm256_set1_ps(float(1u << _tex.m_widthLog2));
	
	__m256 const dudx_tex = _mm256_mul_ps(_dudx, width);
	__m256 const dudy_tex = _mm256_mul_ps(_dudy, height);

	__m256 const dvdx_tex = _mm256_mul_ps(_dvdx, width);
	__m256 const dvdy_tex = _mm256_mul_ps(_dvdy, height);

	// Work out the texture coordinate with the largest derivative.
	// -> max(dot(dudx, dudy), dot(dvdx, dvdy))
	__m256 const du_dot2 = _mm256_fmadd_ps(dudx_tex, dudx_tex, _mm256_mul_ps(dudy_tex, dudy_tex));
	__m256 const dv_dot2 = _mm256_fmadd_ps(dvdx_tex, dvdx_tex, _mm256_mul_ps(dvdy_tex, dvdy_tex));

	// Todo: with proper log2 we can use identity log2(x^(1/2)) == 0.5 * log2(x) and remove sqrt.
	__m256 const maxCoord = _mm256_sqrt_ps(_mm256_max_ps(du_dot2, dv_dot2));

	// Approximate (floor) log2 by extracting exponent.
	return _mm256_min_epi32(_mm256_set1_epi32(_tex.m_numMips - 1), _mm256_max_epi32(_mm256_setzero_si256(), simdutil::ExtractExponent(maxCoord)));
}


static __m256i BoundCoordsWrap(__m256i _coord, __m256i _bound)
{
	// Assuming width and height are powers of two.
	__m256i const one = _mm256_set1_epi32(1);
	return _mm256_and_si256(_coord, _mm256_sub_epi32(_bound, one));
}

static void GatherQuadsAndInterpolate
(
	TextureData const& _tex, 
	uint32_t _mips[8], 
	__m256i _mipWidth, 
	__m256i _x0, 
	__m256i _y0, 
	__m256i _x1,
	__m256i _y1,
	__m256& o_r,
	__m256& o_g,
	__m256& o_b,
	__m256& o_a,
	float _interpU[8],
	float _interpV[8],
	uint32_t _execMask
)
{
	// Compute the tile offsets.
	KT_ALIGNAS(32) uint32_t morton_x0y0[8];
	KT_ALIGNAS(32) uint32_t morton_x1y0[8];
	KT_ALIGNAS(32) uint32_t morton_x1y1[8];
	KT_ALIGNAS(32) uint32_t morton_x0y1[8];

#if SR_TILE_TEXTURES
	{
		__m256i const tileX0 = _mm256_srli_epi32(_x0, c_texTileSizeLog2);
		__m256i const tileY0 = _mm256_srli_epi32(_y0, c_texTileSizeLog2);
		__m256i const tileX1 = _mm256_srli_epi32(_x1, c_texTileSizeLog2);
		__m256i const tileY1 = _mm256_srli_epi32(_y1, c_texTileSizeLog2);

		__m256i const c_texTileMaskAvx = _mm256_set1_epi32(c_texTileMask);

		// Compute the inner-tile coordinates.
		__m256i const inTileAddressX0 = _mm256_and_si256(_x0, c_texTileMaskAvx);
		__m256i const inTileAddressY0 = _mm256_and_si256(_y0, c_texTileMaskAvx);
		__m256i const inTileAddressX1 = _mm256_and_si256(_x1, c_texTileMaskAvx);
		__m256i const inTileAddressY1 = _mm256_and_si256(_y1, c_texTileMaskAvx);

		// TODO: Broken for non pow2 textures (we assert not supporting those though!)
		__m256i const texTileSize = _mm256_set1_epi32(c_texTileSize);
		__m256i const mipTileWidth = _mm256_srli_epi32(_mm256_max_epi32(_mipWidth, texTileSize), c_texTileSizeLog2);

		// Compute the linear offset to the start of each tile (not including bytes per pixel).
		__m256i const offs_x0y0 = _mm256_mullo_epi32(_mm256_set1_epi32(c_texTileSize * c_texTileSize), _mm256_add_epi32(_mm256_mullo_epi32(tileY0, mipTileWidth), tileX0));
		__m256i const offs_x1y0 = _mm256_mullo_epi32(_mm256_set1_epi32(c_texTileSize * c_texTileSize), _mm256_add_epi32(_mm256_mullo_epi32(tileY0, mipTileWidth), tileX1));
		__m256i const offs_x0y1 = _mm256_mullo_epi32(_mm256_set1_epi32(c_texTileSize * c_texTileSize), _mm256_add_epi32(_mm256_mullo_epi32(tileY1, mipTileWidth), tileX0));
		__m256i const offs_x1y1 = _mm256_mullo_epi32(_mm256_set1_epi32(c_texTileSize * c_texTileSize), _mm256_add_epi32(_mm256_mullo_epi32(tileY1, mipTileWidth), tileX1));

		// Add offset to morton encoded inner tile coordinates, multiply by 4 for bytes per pixel.
		// This is the final pixel offset.
		_mm256_store_si256((__m256i*)morton_x0y0, _mm256_slli_epi32(_mm256_add_epi32(offs_x0y0, MortonEncode_AVX(inTileAddressX0, inTileAddressY0)), 2));
		_mm256_store_si256((__m256i*)morton_x1y0, _mm256_slli_epi32(_mm256_add_epi32(offs_x1y0, MortonEncode_AVX(inTileAddressX1, inTileAddressY0)), 2));
		_mm256_store_si256((__m256i*)morton_x1y1, _mm256_slli_epi32(_mm256_add_epi32(offs_x1y1, MortonEncode_AVX(inTileAddressX1, inTileAddressY1)), 2));
		_mm256_store_si256((__m256i*)morton_x0y1, _mm256_slli_epi32(_mm256_add_epi32(offs_x0y1, MortonEncode_AVX(inTileAddressX0, inTileAddressY1)), 2));
	}
#else
	{
		_mm256_store_si256((__m256i*)morton_x0y0, _mm256_slli_epi32(_mm256_add_epi32(_x0, _mm256_mullo_epi32(_y0, _mipWidth)), 2));
		_mm256_store_si256((__m256i*)morton_x0y1, _mm256_slli_epi32(_mm256_add_epi32(_x0, _mm256_mullo_epi32(_y1, _mipWidth)), 2));
		_mm256_store_si256((__m256i*)morton_x1y0, _mm256_slli_epi32(_mm256_add_epi32(_x1, _mm256_mullo_epi32(_y0, _mipWidth)), 2));
		_mm256_store_si256((__m256i*)morton_x1y1, _mm256_slli_epi32(_mm256_add_epi32(_x1, _mm256_mullo_epi32(_y1, _mipWidth)), 2));
	}
#endif

	// | denotes ymm reg split. Each row is a ymm reg. 
	// If we load AoS with stride like this than we can tranpose independent 4x4 sub matrix in registers without cross lane permutes.
	//      AoS					SoA
	// [rgba0|rgba4]		[r0123|r4567]
	// [rgba1|rgba5]		[g0123|g4567]
	// [rgba2|rgba6]	->	[b0123|b4567]
	// [rgba3|rgba7]		[b0123|b4567]

	// f(x) = float offest
	// f(0) = 0
	// f(1) = 8
	// f(2) = 16
	// f(3) = 24
	// f(4) = 4
	// f(5) = 12
	// f(6) = 20
	// f(7) = 28

	// f = x >= 4 ? (x%4)*8+4 : x*8

	KT_ALIGNAS(32) float interpolatedTexels[8 * 4];

	// Todo: this assumes that we are shading in lanes and that the execution mask never has any holes, only some bits from msb stripped.
	uint32_t const numQuads = kt::Popcnt(_execMask);
	for (uint32_t i = 0; i < numQuads; ++i)
	{
		uint8_t const* mipPtr = _tex.m_texels.Data() + _tex.m_mipOffsets[_mips[i]];
		__m128 x0y0_tex, x1y1_tex, x1y0_tex, x0y1_tex;
		// Convert each pixel in the quad and store.
		{
			uint8_t const* pix_x0y0 = mipPtr + morton_x0y0[i];
			__m128i const x0y0 = _mm_cvtsi32_si128(*(uint32_t*)pix_x0y0);
			x0y0_tex = _mm_mul_ps(_mm_set1_ps(1.0f / 255.0f), _mm_cvtepi32_ps(_mm_cvtepu8_epi32(x0y0)));
		}

		{
			uint8_t const* pix_x1y0 = mipPtr + morton_x1y0[i];
			__m128i const x1y0 = _mm_cvtsi32_si128(*(uint32_t*)pix_x1y0);
			x1y0_tex = _mm_mul_ps(_mm_set1_ps(1.0f / 255.0f), _mm_cvtepi32_ps(_mm_cvtepu8_epi32(x1y0)));
		}

		{
			uint8_t const* pix_x1y1 = mipPtr + morton_x1y1[i];
			__m128i const x1y1 = _mm_cvtsi32_si128(*(uint32_t*)pix_x1y1);
			x1y1_tex = _mm_mul_ps(_mm_set1_ps(1.0f / 255.0f), _mm_cvtepi32_ps(_mm_cvtepu8_epi32(x1y1)));
		}

		{
			uint8_t const* pix_x0y1 = mipPtr + morton_x0y1[i];
			__m128i const x0y1 = _mm_cvtsi32_si128(*(uint32_t*)pix_x0y1);
			x0y1_tex = _mm_mul_ps(_mm_set1_ps(1.0f / 255.0f), _mm_cvtepi32_ps(_mm_cvtepu8_epi32(x0y1)));
		}

		__m128 const interpU_broadcast = _mm_broadcast_ss(_interpU + i);
		__m128 const interpV_broadcast = _mm_broadcast_ss(_interpV + i);
		__m128 const left = simdutil::Lerp(x0y0_tex, x0y1_tex, interpV_broadcast);
		__m128 const right = simdutil::Lerp(x1y0_tex, x1y1_tex, interpV_broadcast);
		__m128 const finalInterp = simdutil::Lerp(left, right, interpU_broadcast);

		// See above comment.
		uint32_t const writeIdx = i >= 4 ? (i & 3) * 8 + 4 : i * 8;
		_mm_store_ps(interpolatedTexels + writeIdx, finalInterp);
	}

	o_r = _mm256_load_ps(interpolatedTexels);
	o_g = _mm256_load_ps(interpolatedTexels + 8);
	o_b = _mm256_load_ps(interpolatedTexels + 16);
	o_a = _mm256_load_ps(interpolatedTexels + 24);

	simdutil::Transpose4x4SubMatricies(o_r, o_g, o_b, o_a);

}

void SampleWrap
(
	TextureData const& _tex,
	__m256 _u,
	__m256 _v,
	__m256 _dudx,
	__m256 _dudy,
	__m256 _dvdx,
	__m256 _dvdy,
	__m256& o_r,
	__m256& o_g,
	__m256& o_b,
	__m256& o_a,
	uint32_t _execMask
)
{
	__m256i const mipFloor = CalcMipLevels(_tex, _dudx, _dudy, _dvdx, _dvdy);

	__m256i const one = _mm256_set1_epi32(1);

	__m256i const widthLog2 = _mm256_set1_epi32(_tex.m_widthLog2);
	__m256i const heightLog2 = _mm256_set1_epi32(_tex.m_heightLog2);

	// Calculate mip widths.
	__m256i const width = _mm256_sllv_epi32(one, _mm256_sub_epi32(widthLog2, _mm256_min_epi32(widthLog2, mipFloor)));
	__m256i const height = _mm256_sllv_epi32(one, _mm256_sub_epi32(heightLog2, _mm256_min_epi32(heightLog2, mipFloor)));

	__m256 const signBit = SR_AVX_LOAD_CONST_FLOAT(simdutil::c_avxSignBit);

	// Perform wrapping of uv coordinates and account for sign.

	__m256 const uSign = _mm256_and_ps(signBit, _u);
	__m256 const vSign = _mm256_and_ps(signBit, _v);

	__m256 const absU = _mm256_xor_ps(uSign, _u);
	__m256 const absV = _mm256_xor_ps(vSign, _v);

	__m256 const fracU = _mm256_sub_ps(absU, _mm256_floor_ps(absU));
	__m256 const fracV = _mm256_sub_ps(absV, _mm256_floor_ps(absV));

	__m256 const fracU_wrap = _mm256_blendv_ps(fracU, _mm256_sub_ps(_mm256_set1_ps(1.0f), fracU), uSign);
	__m256 const fracV_wrap = _mm256_blendv_ps(fracV, _mm256_sub_ps(_mm256_set1_ps(1.0f), fracV), vSign);

	__m256 const widthF = _mm256_cvtepi32_ps(width);
	__m256 const heightF = _mm256_cvtepi32_ps(height);

	__m256 const u_texSpace = _mm256_mul_ps(widthF, fracU_wrap);
	__m256 const v_texSpace = _mm256_mul_ps(heightF, fracV_wrap);

	__m256 const u_texSpace_floor = _mm256_floor_ps(u_texSpace);
	__m256 const v_texSpace_floor = _mm256_floor_ps(v_texSpace);

	__m256 const u_interp = _mm256_sub_ps(u_texSpace, u_texSpace_floor);
	__m256 const v_interp = _mm256_sub_ps(v_texSpace, v_texSpace_floor);

	__m256i const x0 = BoundCoordsWrap(_mm256_cvtps_epi32(u_texSpace_floor), width);
	__m256i const y0 = BoundCoordsWrap(_mm256_cvtps_epi32(v_texSpace_floor), height);

	__m256i const x1 = BoundCoordsWrap(_mm256_add_epi32(x0, one), width);
	__m256i const y1 = BoundCoordsWrap(_mm256_add_epi32(y0, one), height);

	KT_ALIGNAS(32) uint32_t mips[8];
	_mm256_store_si256((__m256i*)mips, mipFloor);

	KT_ALIGNAS(32) float interpU[8];
	KT_ALIGNAS(32) float interpV[8];

	_mm256_store_ps(interpU, u_interp);
	_mm256_store_ps(interpV, v_interp);

	GatherQuadsAndInterpolate(_tex, mips, width, x0, y0, x1, y1, o_r, o_g, o_b, o_a, interpU, interpV, _execMask);
}

}
}