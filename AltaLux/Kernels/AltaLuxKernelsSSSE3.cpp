#include "AltaLuxKernelsInternal.h"

#include <tmmintrin.h>

namespace
{
	// Multiplies four unsigned 32-bit lanes using the even-lane 32x32->64
	// instruction. Shifted copies produce lanes 1 and 3, then the low
	// 32-bit products are interleaved back into normal lane order.
	inline __m128i MulloEpi32Compat(__m128i a, __m128i b)
	{
		const __m128i prod02 = _mm_mul_epu32(a, b);
		const __m128i prod13 = _mm_mul_epu32(_mm_srli_si128(a, 4), _mm_srli_si128(b, 4));
		const __m128i prod02Low = _mm_shuffle_epi32(prod02, _MM_SHUFFLE(0, 0, 2, 0));
		const __m128i prod13Low = _mm_shuffle_epi32(prod13, _MM_SHUFFLE(0, 0, 2, 0));
		return _mm_unpacklo_epi32(prod02Low, prod13Low);
	}

	// Computes luma for four RGB/RGBX pixels already arranged as four dwords:
	// C0 C1 C2 X. Bytes are widened to 16-bit channels, _mm_madd_epi16 performs
	// pairwise channel*factor sums, and adjacent pair sums are combined into one
	// 32-bit luma value per pixel.
	inline __m128i CalculateRGBLuma4FromPacked(__m128i pixels, __m128i factors,
		__m128i roundingOffset, __m128i shiftCount)
	{
		const __m128i zero = _mm_setzero_si128();
		const __m128i loWords = _mm_unpacklo_epi8(pixels, zero);
		const __m128i hiWords = _mm_unpackhi_epi8(pixels, zero);
		const __m128i loPairs = _mm_madd_epi16(loWords, factors);
		const __m128i hiPairs = _mm_madd_epi16(hiWords, factors);
		const __m128i loSums = _mm_add_epi32(loPairs, _mm_srli_si128(loPairs, 4));
		const __m128i hiSums = _mm_add_epi32(hiPairs, _mm_srli_si128(hiPairs, 4));
		const __m128i y01 = _mm_shuffle_epi32(loSums, _MM_SHUFFLE(2, 0, 2, 0));
		const __m128i y23 = _mm_shuffle_epi32(hiSums, _MM_SHUFFLE(2, 0, 2, 0));
		const __m128i y = _mm_unpacklo_epi64(y01, y23);
		return _mm_sra_epi32(_mm_add_epi32(y, roundingOffset), shiftCount);
	}

	inline __m128i CalculateRGB32Luma4(const unsigned char* src, __m128i factors,
		__m128i roundingOffset, __m128i shiftCount)
	{
		return CalculateRGBLuma4FromPacked(_mm_loadu_si128(reinterpret_cast<const __m128i*>(src)),
			factors, roundingOffset, shiftCount);
	}

	inline __m128i LoadRGB24AsRGBX4(const unsigned char* src)
	{
		const __m128i rgb24ToRGBX = _mm_setr_epi8(
			0, 1, 2, -1,
			3, 4, 5, -1,
			6, 7, 8, -1,
			9, 10, 11, -1);
		return _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(src)), rgb24ToRGBX);
	}

	// Stores four 32-bit luma values as four saturated bytes. The two packing
	// steps narrow 32->16 and 16->8 before the low dword is written.
	inline void StoreLuma4(unsigned char* target, __m128i y)
	{
		const __m128i packed16 = _mm_packs_epi32(y, _mm_setzero_si128());
		const __m128i packed8 = _mm_packus_epi16(packed16, _mm_setzero_si128());
		*reinterpret_cast<int*>(target) = _mm_cvtsi128_si32(packed8);
	}

	// Packs twelve 32-bit RGB channel values for four pixels into twelve bytes:
	// R0 G0 B0 R1 G1 B1 R2 G2 B2 R3 G3 B3. Values are saturated while narrowing.
	inline __m128i PackRGBTriplets4(__m128i e0, __m128i e1, __m128i e2)
	{
		const __m128i packed01 = _mm_packs_epi32(e0, e1);
		const __m128i packed2 = _mm_packs_epi32(e2, _mm_setzero_si128());
		return _mm_packus_epi16(packed01, packed2);
	}

	// Writes four packed RGB24 pixels. The packed triplets occupy the low twelve
	// bytes, so the store is split into one 8-byte write and one 4-byte write.
	inline void StoreRGB24Pixels4(unsigned char* target, __m128i packedRGB)
	{
		_mm_storel_epi64(reinterpret_cast<__m128i*>(target), packedRGB);
		*reinterpret_cast<int*>(target + 8) = _mm_cvtsi128_si32(_mm_srli_si128(packedRGB, 8));
	}

	// Writes four packed RGB32 pixels while preserving the existing fourth byte.
	// SSSE3 expands the contiguous RGB triplets into RGBX dwords with PSHUFB, then
	// the previous alpha/unused byte is ORed back into each pixel.
	inline void StoreRGB32Pixels4(unsigned char* target, __m128i packedRGB)
	{
		const __m128i rgbToRGBX = _mm_setr_epi8(
			0, 1, 2, -1,
			3, 4, 5, -1,
			6, 7, 8, -1,
			9, 10, 11, -1);
		const __m128i alphaMask = _mm_set1_epi32(static_cast<int>(0xFF000000u));
		const __m128i colors = _mm_shuffle_epi8(packedRGB, rgbToRGBX);
		const __m128i original = _mm_loadu_si128(reinterpret_cast<const __m128i*>(target));
		_mm_storeu_si128(reinterpret_cast<__m128i*>(target),
			_mm_or_si128(_mm_and_si128(original, alphaMask), colors));
	}

	// Reorders four vectors of per-channel RGB32 data from planar lanes
	// RRRR/GGGG/BBBB into the accumulator's RGBRGB... uint32 triplet layout.
	inline void BuildAccumTriplets4(__m128i c0, __m128i c1, __m128i c2,
		__m128i& out0, __m128i& out1, __m128i& out2)
	{
		const __m128i lane0 = _mm_setr_epi32(-1, 0, 0, 0);
		const __m128i lane1 = _mm_setr_epi32(0, -1, 0, 0);
		const __m128i lane2 = _mm_setr_epi32(0, 0, -1, 0);
		const __m128i lane3 = _mm_setr_epi32(0, 0, 0, -1);
		const __m128i lanes03 = _mm_or_si128(lane0, lane3);

		out0 = _mm_or_si128(
			_mm_and_si128(_mm_shuffle_epi32(c0, _MM_SHUFFLE(1, 0, 0, 0)), lanes03),
			_mm_or_si128(
				_mm_and_si128(_mm_shuffle_epi32(c1, _MM_SHUFFLE(0, 0, 0, 0)), lane1),
				_mm_and_si128(_mm_shuffle_epi32(c2, _MM_SHUFFLE(0, 0, 0, 0)), lane2)));
		out1 = _mm_or_si128(
			_mm_and_si128(_mm_shuffle_epi32(c1, _MM_SHUFFLE(2, 0, 0, 1)), lanes03),
			_mm_or_si128(
				_mm_and_si128(_mm_shuffle_epi32(c2, _MM_SHUFFLE(1, 1, 1, 1)), lane1),
				_mm_and_si128(_mm_shuffle_epi32(c0, _MM_SHUFFLE(2, 2, 2, 2)), lane2)));
		out2 = _mm_or_si128(
			_mm_and_si128(_mm_shuffle_epi32(c2, _MM_SHUFFLE(3, 0, 0, 2)), lanes03),
			_mm_or_si128(
				_mm_and_si128(_mm_shuffle_epi32(c0, _MM_SHUFFLE(3, 3, 3, 3)), lane1),
				_mm_and_si128(_mm_shuffle_epi32(c1, _MM_SHUFFLE(3, 3, 3, 3)), lane2)));
	}

	// Loads four RGB32/BGR32 pixels, extracts the three color bytes from each
	// dword, applies the layer weight, and builds three accumulator vectors.
	inline void LoadWeightedRGB32Accum4(const unsigned char* src, __m128i weightVec,
		__m128i& w0, __m128i& w1, __m128i& w2)
	{
		const __m128i channelMask = _mm_set1_epi32(0xFF);
		const __m128i pixels = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src));
		const __m128i c0 = MulloEpi32Compat(_mm_and_si128(pixels, channelMask), weightVec);
		const __m128i c1 = MulloEpi32Compat(_mm_and_si128(_mm_srli_epi32(pixels, 8), channelMask), weightVec);
		const __m128i c2 = MulloEpi32Compat(_mm_and_si128(_mm_srli_epi32(pixels, 16), channelMask), weightVec);
		BuildAccumTriplets4(c0, c1, c2, w0, w1, w2);
	}

	inline void LoadWeightedRGB24Accum4(const unsigned char* src, __m128i weightVec,
		__m128i& w0, __m128i& w1, __m128i& w2)
	{
		const __m128i rgbx = LoadRGB24AsRGBX4(src);
		const __m128i v0Mask = _mm_setr_epi8(0, -1, -1, -1, 1, -1, -1, -1, 2, -1, -1, -1, 4, -1, -1, -1);
		const __m128i v1Mask = _mm_setr_epi8(5, -1, -1, -1, 6, -1, -1, -1, 8, -1, -1, -1, 9, -1, -1, -1);
		const __m128i v2Mask = _mm_setr_epi8(10, -1, -1, -1, 12, -1, -1, -1, 13, -1, -1, -1, 14, -1, -1, -1);
		w0 = MulloEpi32Compat(_mm_shuffle_epi8(rgbx, v0Mask), weightVec);
		w1 = MulloEpi32Compat(_mm_shuffle_epi8(rgbx, v1Mask), weightVec);
		w2 = MulloEpi32Compat(_mm_shuffle_epi8(rgbx, v2Mask), weightVec);
	}

	inline unsigned int LoadRGB24Pixel(const unsigned char* pixel)
	{
		return static_cast<unsigned int>(pixel[0]) |
			(static_cast<unsigned int>(pixel[1]) << 8) |
			(static_cast<unsigned int>(pixel[2]) << 16);
	}

	inline void StoreRGB24Pixel(unsigned char* pixel, unsigned int value)
	{
		pixel[0] = static_cast<unsigned char>(value);
		pixel[1] = static_cast<unsigned char>(value >> 8);
		pixel[2] = static_cast<unsigned char>(value >> 16);
	}

	inline unsigned int LoadPackedPixel(const unsigned char* pixel, int pixelStride)
	{
		if (pixelStride == 4)
		{
			return *reinterpret_cast<const unsigned int*>(pixel);
		}

		return LoadRGB24Pixel(pixel);
	}

	inline __m128i LoadFourBoxPixelsSSSE3(const unsigned char* row, int startX, int sourceOffset,
		int pixelStride)
	{
		return _mm_setr_epi32(
			static_cast<int>(LoadPackedPixel(row + (((startX + 0) * 2 + sourceOffset) * pixelStride), pixelStride)),
			static_cast<int>(LoadPackedPixel(row + (((startX + 1) * 2 + sourceOffset) * pixelStride), pixelStride)),
			static_cast<int>(LoadPackedPixel(row + (((startX + 2) * 2 + sourceOffset) * pixelStride), pixelStride)),
			static_cast<int>(LoadPackedPixel(row + (((startX + 3) * 2 + sourceOffset) * pixelStride), pixelStride)));
	}

	inline __m128i AverageBox2x2PixelsSSSE3(__m128i topLeft, __m128i topRight,
		__m128i bottomLeft, __m128i bottomRight, bool includeFourthChannel)
	{
		const __m128i byteMask = _mm_set1_epi32(0xFF);
		__m128i c0 = _mm_add_epi32(_mm_and_si128(topLeft, byteMask), _mm_and_si128(topRight, byteMask));
		c0 = _mm_add_epi32(c0, _mm_and_si128(bottomLeft, byteMask));
		c0 = _mm_add_epi32(c0, _mm_and_si128(bottomRight, byteMask));
		c0 = _mm_srli_epi32(c0, 2);

		__m128i c1 = _mm_add_epi32(_mm_and_si128(_mm_srli_epi32(topLeft, 8), byteMask),
			_mm_and_si128(_mm_srli_epi32(topRight, 8), byteMask));
		c1 = _mm_add_epi32(c1, _mm_and_si128(_mm_srli_epi32(bottomLeft, 8), byteMask));
		c1 = _mm_add_epi32(c1, _mm_and_si128(_mm_srli_epi32(bottomRight, 8), byteMask));
		c1 = _mm_srli_epi32(c1, 2);

		__m128i c2 = _mm_add_epi32(_mm_and_si128(_mm_srli_epi32(topLeft, 16), byteMask),
			_mm_and_si128(_mm_srli_epi32(topRight, 16), byteMask));
		c2 = _mm_add_epi32(c2, _mm_and_si128(_mm_srli_epi32(bottomLeft, 16), byteMask));
		c2 = _mm_add_epi32(c2, _mm_and_si128(_mm_srli_epi32(bottomRight, 16), byteMask));
		c2 = _mm_srli_epi32(c2, 2);

		__m128i packed = _mm_or_si128(c0, _mm_slli_epi32(c1, 8));
		packed = _mm_or_si128(packed, _mm_slli_epi32(c2, 16));
		if (includeFourthChannel)
		{
			__m128i c3 = _mm_add_epi32(_mm_and_si128(_mm_srli_epi32(topLeft, 24), byteMask),
				_mm_and_si128(_mm_srli_epi32(topRight, 24), byteMask));
			c3 = _mm_add_epi32(c3, _mm_and_si128(_mm_srli_epi32(bottomLeft, 24), byteMask));
			c3 = _mm_add_epi32(c3, _mm_and_si128(_mm_srli_epi32(bottomRight, 24), byteMask));
			packed = _mm_or_si128(packed, _mm_slli_epi32(_mm_srli_epi32(c3, 2), 24));
		}
		return packed;
	}

	inline void StoreFourDownscaledPixelsSSSE3(unsigned char* target, __m128i pixels, int pixelStride)
	{
		if (pixelStride == 4)
		{
			_mm_storeu_si128(reinterpret_cast<__m128i*>(target), pixels);
			return;
		}

		StoreRGB24Pixel(target, static_cast<unsigned int>(_mm_cvtsi128_si32(pixels)));
		StoreRGB24Pixel(target + 3, static_cast<unsigned int>(_mm_cvtsi128_si32(_mm_srli_si128(pixels, 4))));
		StoreRGB24Pixel(target + 6, static_cast<unsigned int>(_mm_cvtsi128_si32(_mm_srli_si128(pixels, 8))));
		StoreRGB24Pixel(target + 9, static_cast<unsigned int>(_mm_cvtsi128_si32(_mm_srli_si128(pixels, 12))));
	}

	inline void ScaleDownBox2x2Tail(const unsigned char* topRow, const unsigned char* bottomRow,
		unsigned char* target, int startX, int endX, int pixelStride)
	{
		for (int x = startX; x < endX; ++x)
		{
			const unsigned char* top = topRow + (x * 2 * pixelStride);
			const unsigned char* bottom = bottomRow + (x * 2 * pixelStride);
			target[0] = static_cast<unsigned char>((top[0] + top[pixelStride] + bottom[0] + bottom[pixelStride]) >> 2);
			target[1] = static_cast<unsigned char>((top[1] + top[pixelStride + 1] + bottom[1] + bottom[pixelStride + 1]) >> 2);
			target[2] = static_cast<unsigned char>((top[2] + top[pixelStride + 2] + bottom[2] + bottom[pixelStride + 2]) >> 2);
			if (pixelStride == 4)
			{
				target[3] = static_cast<unsigned char>((top[3] + top[7] + bottom[3] + bottom[7]) >> 2);
			}
			target += pixelStride;
		}
	}

	inline __m128i AverageBox2x2ShuffleSSSE3(const unsigned char* top, const unsigned char* bottom,
		__m128i evenMask, __m128i oddMask)
	{
		const __m128i zero = _mm_setzero_si128();
		const __m128i topPixels = _mm_loadu_si128(reinterpret_cast<const __m128i*>(top));
		const __m128i bottomPixels = _mm_loadu_si128(reinterpret_cast<const __m128i*>(bottom));
		const __m128i topEven = _mm_shuffle_epi8(topPixels, evenMask);
		const __m128i topOdd = _mm_shuffle_epi8(topPixels, oddMask);
		const __m128i bottomEven = _mm_shuffle_epi8(bottomPixels, evenMask);
		const __m128i bottomOdd = _mm_shuffle_epi8(bottomPixels, oddMask);
		__m128i sum = _mm_add_epi16(_mm_unpacklo_epi8(topEven, zero), _mm_unpacklo_epi8(topOdd, zero));
		sum = _mm_add_epi16(sum, _mm_unpacklo_epi8(bottomEven, zero));
		sum = _mm_add_epi16(sum, _mm_unpacklo_epi8(bottomOdd, zero));
		return _mm_packus_epi16(_mm_srli_epi16(sum, 2), zero);
	}

	inline void StoreLow6Bytes(unsigned char* target, __m128i value)
	{
		*reinterpret_cast<unsigned int*>(target) = static_cast<unsigned int>(_mm_cvtsi128_si32(value));
		*reinterpret_cast<unsigned short*>(target + 4) =
			static_cast<unsigned short>(_mm_extract_epi16(value, 2));
	}

	// Interleaves three channel-pure 32-bit vectors (four pixels each) into
	// twelve contiguous pixel bytes: c0 c1 c2 c0 c1 c2 ... Packing is lane-major,
	// so a PSHUFB pass reorders the three 4-byte blocks into pixel triplets.
	inline __m128i InterleaveChannels3x4(__m128i e0, __m128i e1, __m128i e2)
	{
		const __m128i interleaveMask = _mm_setr_epi8(
			0, 4, 8, 1, 5, 9, 2, 6, 10, 3, 7, 11, -1, -1, -1, -1);
		const __m128i packed16 = _mm_packs_epi32(e0, e1);
		const __m128i packed2 = _mm_packs_epi32(e2, _mm_setzero_si128());
		return _mm_shuffle_epi8(_mm_packus_epi16(packed16, packed2), interleaveMask);
	}

	// Moves one channel vector toward the enhanced luma: y + ((c - y) * A) >> 8.
	// The shift is arithmetic because (c - y) * A can be negative; the result
	// always lies between y and c, so it is already a valid byte.
	inline __m128i AttenuateChannelSSSE3(__m128i channel, __m128i luma, __m128i attenuation,
		__m128i rounding, __m128i shiftCount)
	{
		const __m128i diff = _mm_sub_epi32(channel, luma);
		const __m128i scaled = MulloEpi32Compat(diff, attenuation);
		return _mm_add_epi32(luma, _mm_sra_epi32(_mm_add_epi32(scaled, rounding), shiftCount));
	}
}

namespace AltaLuxKernels
{
	// Extracts the luma byte from packed 16-bit YUV words. Two source vectors
	// contain sixteen pixels; the wanted byte is shifted or masked into 16-bit
	// lanes, then packed down to sixteen contiguous luma bytes.
	void ExtractPackedYUVLumaSSSE3(const unsigned char* source, unsigned char* luma,
		int pixelCount, PackedYUVLumaPosition lumaPosition)
	{
		int i = 0;
		const int vectorizedPixels = pixelCount & ~15;
		if (lumaPosition == PackedYUVLumaPosition::HighByte)
		{
			for (; i < vectorizedPixels; i += 16)
			{
				__m128i chunk0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(source + (i * 2)));
				__m128i chunk1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(source + (i * 2) + 16));
				chunk0 = _mm_srli_epi16(chunk0, 8);
				chunk1 = _mm_srli_epi16(chunk1, 8);
				_mm_storeu_si128(reinterpret_cast<__m128i*>(luma + i), _mm_packus_epi16(chunk0, chunk1));
			}
		}
		else
		{
			const __m128i lumaMask = _mm_set1_epi16(0x00FF);
			for (; i < vectorizedPixels; i += 16)
			{
				__m128i chunk0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(source + (i * 2)));
				__m128i chunk1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(source + (i * 2) + 16));
				chunk0 = _mm_and_si128(chunk0, lumaMask);
				chunk1 = _mm_and_si128(chunk1, lumaMask);
				_mm_storeu_si128(reinterpret_cast<__m128i*>(luma + i), _mm_packus_epi16(chunk0, chunk1));
			}
		}
		ExtractPackedYUVLumaScalar(source + (i * 2), luma + i, pixelCount - i, lumaPosition);
	}

	// Replaces the luma byte inside packed 16-bit YUV words without disturbing
	// chroma. The luma bytes are widened into either the low or high byte of each
	// word, masked with the preserved chroma bytes, and stored back in place.
	void InjectPackedYUVLumaSSSE3(unsigned char* target, const unsigned char* luma,
		int pixelCount, PackedYUVLumaPosition lumaPosition)
	{
		int i = 0;
		const int vectorizedPixels = pixelCount & ~15;
		const __m128i zero = _mm_setzero_si128();
		if (lumaPosition == PackedYUVLumaPosition::HighByte)
		{
			const __m128i chromaMask = _mm_set1_epi16(0x00FF);
			for (; i < vectorizedPixels; i += 16)
			{
				const __m128i y = _mm_loadu_si128(reinterpret_cast<const __m128i*>(luma + i));
				__m128i img0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(target + (i * 2)));
				__m128i img1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(target + (i * 2) + 16));
				img0 = _mm_or_si128(_mm_and_si128(img0, chromaMask), _mm_unpacklo_epi8(zero, y));
				img1 = _mm_or_si128(_mm_and_si128(img1, chromaMask), _mm_unpackhi_epi8(zero, y));
				_mm_storeu_si128(reinterpret_cast<__m128i*>(target + (i * 2)), img0);
				_mm_storeu_si128(reinterpret_cast<__m128i*>(target + (i * 2) + 16), img1);
			}
		}
		else
		{
			const __m128i chromaMask = _mm_set1_epi16(static_cast<short>(0xFF00));
			for (; i < vectorizedPixels; i += 16)
			{
				const __m128i y = _mm_loadu_si128(reinterpret_cast<const __m128i*>(luma + i));
				__m128i img0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(target + (i * 2)));
				__m128i img1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(target + (i * 2) + 16));
				img0 = _mm_or_si128(_mm_and_si128(img0, chromaMask), _mm_unpacklo_epi8(y, zero));
				img1 = _mm_or_si128(_mm_and_si128(img1, chromaMask), _mm_unpackhi_epi8(y, zero));
				_mm_storeu_si128(reinterpret_cast<__m128i*>(target + (i * 2)), img0);
				_mm_storeu_si128(reinterpret_cast<__m128i*>(target + (i * 2) + 16), img1);
			}
		}
		InjectPackedYUVLumaScalar(target + (i * 2), luma + i, pixelCount - i, lumaPosition);
	}

	// Extracts luma from RGB/BGR pixels four at a time. RGB24 is loaded
	// contiguously and expanded to RGBX dwords with PSHUFB, avoiding the scalar
	// byte gathers that made the previous middle-tier path slow.
	void ExtractRGBLumaSSSE3(const unsigned char* source, unsigned char* luma, int pixelCount,
		int pixelStride, int firstFactor, int secondFactor, int thirdFactor, int scalingLog)
	{
		const __m128i factors = _mm_setr_epi16(
			static_cast<short>(firstFactor), static_cast<short>(secondFactor), static_cast<short>(thirdFactor), 0,
			static_cast<short>(firstFactor), static_cast<short>(secondFactor), static_cast<short>(thirdFactor), 0);
		const __m128i roundingOffset = _mm_set1_epi32(1 << (scalingLog - 1));
		const __m128i shiftCount = _mm_cvtsi32_si128(scalingLog);
		int i = 0;
		if (pixelStride == 3)
		{
			for (; i <= pixelCount - 6; i += 4)
			{
				const __m128i y = CalculateRGBLuma4FromPacked(LoadRGB24AsRGBX4(source + (i * 3)),
					factors, roundingOffset, shiftCount);
				StoreLuma4(luma + i, y);
			}
			ExtractRGBLumaScalar(source + (i * 3), luma + i, pixelCount - i,
				3, firstFactor, secondFactor, thirdFactor, scalingLog);
			return;
		}

		if (pixelStride == 4)
		{
			for (; i <= pixelCount - 4; i += 4)
			{
				const __m128i y = CalculateRGB32Luma4(source + (i * 4), factors, roundingOffset, shiftCount);
				StoreLuma4(luma + i, y);
			}
			ExtractRGBLumaScalar(source + (i * 4), luma + i, pixelCount - i,
				4, firstFactor, secondFactor, thirdFactor, scalingLog);
			return;
		}

		ExtractRGBLumaScalar(source, luma, pixelCount, pixelStride,
			firstFactor, secondFactor, thirdFactor, scalingLog);
	}

	// Injects processed luma into RGB32/BGR32 pixels. SSSE3 is used to compute the
	// old luma values in groups of four; the per-pixel reciprocal lookup and scale
	// cap remain scalar because SSSE3 has no integer gather instruction.
	void InjectRGBLumaSSSE3(unsigned char* image, const unsigned char* luma, int pixelCount,
		int pixelStride, int firstFactor, int secondFactor, int thirdFactor, int scalingLog,
		const int* reciprocalLut)
	{
		if (pixelStride != 4)
		{
			InjectRGBLumaScalar(image, luma, pixelCount, pixelStride,
				firstFactor, secondFactor, thirdFactor, scalingLog, reciprocalLut);
			return;
		}

		const __m128i factors = _mm_setr_epi16(
			static_cast<short>(firstFactor), static_cast<short>(secondFactor), static_cast<short>(thirdFactor), 0,
			static_cast<short>(firstFactor), static_cast<short>(secondFactor), static_cast<short>(thirdFactor), 0);
		const __m128i roundingOffset = _mm_set1_epi32(1 << (scalingLog - 1));
		const __m128i shiftCount = _mm_cvtsi32_si128(scalingLog);
		__declspec(align(16)) int oldY[4];
		int i = 0;
		for (; i <= pixelCount - 4; i += 4)
		{
			const __m128i y = CalculateRGB32Luma4(image + (i * 4), factors, roundingOffset, shiftCount);
			_mm_store_si128(reinterpret_cast<__m128i*>(oldY), y);
			for (int lane = 0; lane < 4; ++lane)
			{
				unsigned char* pixel = image + ((i + lane) * 4);
				const int clampedOldY = oldY[lane] > 255 ? 255 : oldY[lane];
				const int scale = ComputeRGBScale(luma[i + lane], clampedOldY,
					pixel[0], pixel[1], pixel[2], reciprocalLut);
				ApplyRGBScale(pixel, scale);
			}
		}
		InjectRGBLumaScalar(image + (i * 4), luma + i, pixelCount - i,
			4, firstFactor, secondFactor, thirdFactor, scalingLog, reciprocalLut);
	}

	// Injects processed luma into RGB32/BGR32 pixels when the original luma has
	// already been cached during extraction. This removes the second RGB->luma
	// pass; SSSE3 still applies four per-pixel scale factors to RGB channels at a
	// time, while reciprocal lookup stays scalar because SSSE3 has no gather.
	void InjectRGBLumaWithOriginalLumaSSSE3(unsigned char* image, const unsigned char* luma,
		const unsigned char* originalLuma, int pixelCount, int pixelStride,
		const int* reciprocalLut)
	{
		if (pixelStride != 4)
		{
			InjectRGBLumaWithOriginalLumaScalar(image, luma, originalLuma, pixelCount,
				pixelStride, reciprocalLut);
			return;
		}

		const __m128i channelMask = _mm_set1_epi32(0xFF);
		const __m128i alphaMask = _mm_set1_epi32(static_cast<int>(0xFF000000u));
		const __m128i scaleRounding = _mm_set1_epi32(1 << 7);
		int i = 0;
		for (; i <= pixelCount - 4; i += 4)
		{
			const unsigned char* pixel0 = image + (i * 4);
			const int scale0 = ComputeRGBScale(luma[i], originalLuma[i],
				pixel0[0], pixel0[1], pixel0[2], reciprocalLut);
			const int scale1 = ComputeRGBScale(luma[i + 1], originalLuma[i + 1],
				pixel0[4], pixel0[5], pixel0[6], reciprocalLut);
			const int scale2 = ComputeRGBScale(luma[i + 2], originalLuma[i + 2],
				pixel0[8], pixel0[9], pixel0[10], reciprocalLut);
			const int scale3 = ComputeRGBScale(luma[i + 3], originalLuma[i + 3],
				pixel0[12], pixel0[13], pixel0[14], reciprocalLut);

			const __m128i pixels = _mm_loadu_si128(reinterpret_cast<const __m128i*>(pixel0));
			const __m128i scale = _mm_setr_epi32(scale0, scale1, scale2, scale3);
			const __m128i c0 = _mm_and_si128(pixels, channelMask);
			const __m128i c1 = _mm_and_si128(_mm_srli_epi32(pixels, 8), channelMask);
			const __m128i c2 = _mm_and_si128(_mm_srli_epi32(pixels, 16), channelMask);
			const __m128i out0 = _mm_srli_epi32(_mm_add_epi32(MulloEpi32Compat(c0, scale), scaleRounding), 8);
			const __m128i out1 = _mm_srli_epi32(_mm_add_epi32(MulloEpi32Compat(c1, scale), scaleRounding), 8);
			const __m128i out2 = _mm_srli_epi32(_mm_add_epi32(MulloEpi32Compat(c2, scale), scaleRounding), 8);
			__m128i output = _mm_or_si128(_mm_and_si128(pixels, alphaMask), out0);
			output = _mm_or_si128(output, _mm_slli_epi32(out1, 8));
			output = _mm_or_si128(output, _mm_slli_epi32(out2, 16));
			_mm_storeu_si128(reinterpret_cast<__m128i*>(image + (i * 4)), output);
		}
		InjectRGBLumaWithOriginalLumaScalar(image + (i * 4), luma + i, originalLuma + i,
			pixelCount - i, 4, reciprocalLut);
	}

	// Downscales an RGB24/RGB32 image with a box filter. SSSE3 handles the common
	// 2x preview reduction by loading contiguous source pixels and using PSHUFB to
	// select the even/odd horizontal samples that feed each destination pixel.
	void ScaleDownBoxSSSE3(const unsigned char* source, int sourceWidth, int sourceHeight,
		unsigned char* target, int scaleFactor, int pixelStride)
	{
		if (scaleFactor != 2 || source == nullptr || target == nullptr ||
			sourceWidth <= 0 || sourceHeight <= 0 || (pixelStride != 3 && pixelStride != 4))
		{
			ScaleDownBoxScalar(source, sourceWidth, sourceHeight, target, scaleFactor, pixelStride);
			return;
		}

		const int sourceStride = sourceWidth * pixelStride;
		const int targetWidth = sourceWidth / 2;
		const int targetHeight = sourceHeight / 2;
		const int targetStride = targetWidth * pixelStride;
		const __m128i rgb32EvenMask = _mm_setr_epi8(0, 1, 2, 3, 8, 9, 10, 11, -1, -1, -1, -1, -1, -1, -1, -1);
		const __m128i rgb32OddMask = _mm_setr_epi8(4, 5, 6, 7, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1);
		const __m128i rgb24EvenMask = _mm_setr_epi8(0, 1, 2, 6, 7, 8, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);
		const __m128i rgb24OddMask = _mm_setr_epi8(3, 4, 5, 9, 10, 11, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);

		for (int y = 0; y < targetHeight; ++y)
		{
			const unsigned char* topRow = source + ((y * 2) * sourceStride);
			const unsigned char* bottomRow = topRow + sourceStride;
			unsigned char* dst = target + (y * targetStride);
			int x = 0;
			if (pixelStride == 4)
			{
				for (; x <= targetWidth - 2; x += 2)
				{
					const __m128i out = AverageBox2x2ShuffleSSSE3(
						topRow + (x * 2 * 4), bottomRow + (x * 2 * 4),
						rgb32EvenMask, rgb32OddMask);
					_mm_storel_epi64(reinterpret_cast<__m128i*>(dst + (x * 4)), out);
				}
			}
			else
			{
				for (; x <= targetWidth - 3; x += 2)
				{
					const __m128i out = AverageBox2x2ShuffleSSSE3(
						topRow + (x * 2 * 3), bottomRow + (x * 2 * 3),
						rgb24EvenMask, rgb24OddMask);
					StoreLow6Bytes(dst + (x * 3), out);
				}
			}
			ScaleDownBox2x2Tail(topRow, bottomRow, dst + (x * pixelStride), x, targetWidth, pixelStride);
		}
	}

	// Accumulates four RGB/RGBX pixels into the uint32 weighted-sum buffer. Source
	// bytes are expanded into three vectors holding twelve channels, multiplied by
	// the layer weight, then either assigned or added to the accumulator.
	void AccumulateLayerSSSE3(unsigned int* accum, const unsigned char* layer, int pixelStart,
		int pixelEnd, int pixelStride, int weight, bool firstLayer)
	{
		const __m128i weightVec = _mm_set1_epi32(weight);
		int p = pixelStart;
		const unsigned char* src = layer + (pixelStart * pixelStride);
		unsigned int* dst = accum + (pixelStart * 3);

		if (pixelStride == 4)
		{
			if (firstLayer)
			{
				for (; p <= pixelEnd - 4; p += 4, src += 16, dst += 12)
				{
					__m128i w0, w1, w2;
					LoadWeightedRGB32Accum4(src, weightVec, w0, w1, w2);
					_mm_storeu_si128(reinterpret_cast<__m128i*>(dst), w0);
					_mm_storeu_si128(reinterpret_cast<__m128i*>(dst + 4), w1);
					_mm_storeu_si128(reinterpret_cast<__m128i*>(dst + 8), w2);
				}
			}
			else
			{
				for (; p <= pixelEnd - 4; p += 4, src += 16, dst += 12)
				{
					__m128i w0, w1, w2;
					LoadWeightedRGB32Accum4(src, weightVec, w0, w1, w2);
					_mm_storeu_si128(reinterpret_cast<__m128i*>(dst),
						_mm_add_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(dst)), w0));
					_mm_storeu_si128(reinterpret_cast<__m128i*>(dst + 4),
						_mm_add_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(dst + 4)), w1));
					_mm_storeu_si128(reinterpret_cast<__m128i*>(dst + 8),
						_mm_add_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(dst + 8)), w2));
				}
			}
			AccumulateLayerScalar(accum, layer, p, pixelEnd, pixelStride, weight, firstLayer);
			return;
		}

		if (pixelStride == 3)
		{
			if (firstLayer)
			{
				for (; p <= pixelEnd - 6; p += 4, src += 12, dst += 12)
				{
					__m128i w0, w1, w2;
					LoadWeightedRGB24Accum4(src, weightVec, w0, w1, w2);
					_mm_storeu_si128(reinterpret_cast<__m128i*>(dst), w0);
					_mm_storeu_si128(reinterpret_cast<__m128i*>(dst + 4), w1);
					_mm_storeu_si128(reinterpret_cast<__m128i*>(dst + 8), w2);
				}
			}
			else
			{
				for (; p <= pixelEnd - 6; p += 4, src += 12, dst += 12)
				{
					__m128i w0, w1, w2;
					LoadWeightedRGB24Accum4(src, weightVec, w0, w1, w2);
					_mm_storeu_si128(reinterpret_cast<__m128i*>(dst),
						_mm_add_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(dst)), w0));
					_mm_storeu_si128(reinterpret_cast<__m128i*>(dst + 4),
						_mm_add_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(dst + 4)), w1));
					_mm_storeu_si128(reinterpret_cast<__m128i*>(dst + 8),
						_mm_add_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(dst + 8)), w2));
				}
			}
			AccumulateLayerScalar(accum, layer, p, pixelEnd, pixelStride, weight, firstLayer);
			return;
		}

		const int sourceStep = pixelStride * 4;
		if (firstLayer)
		{
			for (; p <= pixelEnd - 4; p += 4, src += sourceStep, dst += 12)
			{
				const __m128i v0 = _mm_setr_epi32(src[0], src[1], src[2], src[pixelStride]);
				const __m128i v1 = _mm_setr_epi32(src[pixelStride + 1], src[pixelStride + 2],
					src[pixelStride * 2], src[(pixelStride * 2) + 1]);
				const __m128i v2 = _mm_setr_epi32(src[(pixelStride * 2) + 2],
					src[pixelStride * 3], src[(pixelStride * 3) + 1], src[(pixelStride * 3) + 2]);
				const __m128i w0 = MulloEpi32Compat(v0, weightVec);
				const __m128i w1 = MulloEpi32Compat(v1, weightVec);
				const __m128i w2 = MulloEpi32Compat(v2, weightVec);
				_mm_storeu_si128(reinterpret_cast<__m128i*>(dst), w0);
				_mm_storeu_si128(reinterpret_cast<__m128i*>(dst + 4), w1);
				_mm_storeu_si128(reinterpret_cast<__m128i*>(dst + 8), w2);
			}
		}
		else
		{
			for (; p <= pixelEnd - 4; p += 4, src += sourceStep, dst += 12)
			{
				const __m128i v0 = _mm_setr_epi32(src[0], src[1], src[2], src[pixelStride]);
				const __m128i v1 = _mm_setr_epi32(src[pixelStride + 1], src[pixelStride + 2],
					src[pixelStride * 2], src[(pixelStride * 2) + 1]);
				const __m128i v2 = _mm_setr_epi32(src[(pixelStride * 2) + 2],
					src[pixelStride * 3], src[(pixelStride * 3) + 1], src[(pixelStride * 3) + 2]);
				const __m128i w0 = MulloEpi32Compat(v0, weightVec);
				const __m128i w1 = MulloEpi32Compat(v1, weightVec);
				const __m128i w2 = MulloEpi32Compat(v2, weightVec);
				_mm_storeu_si128(reinterpret_cast<__m128i*>(dst),
					_mm_add_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(dst)), w0));
				_mm_storeu_si128(reinterpret_cast<__m128i*>(dst + 4),
					_mm_add_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(dst + 4)), w1));
				_mm_storeu_si128(reinterpret_cast<__m128i*>(dst + 8),
					_mm_add_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(dst + 8)), w2));
			}
		}
		AccumulateLayerScalar(accum, layer, p, pixelEnd, pixelStride, weight, firstLayer);
	}

	// Converts accumulated weighted sums back to RGB bytes four pixels at a time.
	// Three vectors hold the twelve channel sums; each lane is rounded, shifted,
	// saturated to bytes, and stored through RGB24 or RGB32 packing helpers.
	void WriteAccumulatedImageSSSE3(unsigned char* target, const unsigned int* accum, int pixelStart,
		int pixelEnd, int pixelStride, int weightScaleLog2, int weightHalf)
	{
		const __m128i roundingVec = _mm_set1_epi32(weightHalf);
		const __m128i shiftVec = _mm_cvtsi32_si128(weightScaleLog2);
		int p = pixelStart;
		if (pixelStride == 4)
		{
			for (; p <= pixelEnd - 4; p += 4)
			{
				const unsigned int* src = accum + (p * 3);
				const __m128i e0 = _mm_srl_epi32(
					_mm_add_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(src)), roundingVec),
					shiftVec);
				const __m128i e1 = _mm_srl_epi32(
					_mm_add_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(src + 4)), roundingVec),
					shiftVec);
				const __m128i e2 = _mm_srl_epi32(
					_mm_add_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(src + 8)), roundingVec),
					shiftVec);
				StoreRGB32Pixels4(target + (p * 4), PackRGBTriplets4(e0, e1, e2));
			}
			WriteAccumulatedImageScalar(target, accum, p, pixelEnd, pixelStride, weightScaleLog2, weightHalf);
			return;
		}

		if (pixelStride == 3)
		{
			for (; p <= pixelEnd - 4; p += 4)
			{
				const unsigned int* src = accum + (p * 3);
				const __m128i e0 = _mm_srl_epi32(
					_mm_add_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(src)), roundingVec),
					shiftVec);
				const __m128i e1 = _mm_srl_epi32(
					_mm_add_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(src + 4)), roundingVec),
					shiftVec);
				const __m128i e2 = _mm_srl_epi32(
					_mm_add_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(src + 8)), roundingVec),
					shiftVec);
				StoreRGB24Pixels4(target + (p * 3), PackRGBTriplets4(e0, e1, e2));
			}
			WriteAccumulatedImageScalar(target, accum, p, pixelEnd, pixelStride, weightScaleLog2, weightHalf);
			return;
		}

		WriteAccumulatedImageScalar(target, accum, p, pixelEnd, pixelStride, weightScaleLog2, weightHalf);
	}

	// Attenuates chroma toward the enhanced luma four pixels at a time. The
	// per-channel math runs in 32-bit lanes with the SSSE3-compatible mullo
	// emulation; RGB24 is expanded to RGBX dwords with PSHUFB and, like the
	// luma extraction kernel, stops six pixels short of the end so the 16-byte
	// loads never read past the buffer.
	void ApplyChromaAttenuationSSSE3(unsigned char* target, const unsigned char* enhancedLuma,
		const unsigned char* risk, int pixelStart, int pixelEnd, int pixelStride, int maxStrengthQ8)
	{
		if (pixelStride != 3 && pixelStride != 4)
		{
			ApplyChromaAttenuationScalar(target, enhancedLuma, risk, pixelStart, pixelEnd,
				pixelStride, maxStrengthQ8);
			return;
		}

		const __m128i channelMask = _mm_set1_epi32(0xFF);
		const __m128i maxStrengthVec = _mm_set1_epi32(maxStrengthQ8);
		const __m128i fullAttenuation = _mm_set1_epi32(256);
		const __m128i strengthRounding = _mm_set1_epi32(127);
		const __m128i channelRounding = _mm_set1_epi32(128);
		const __m128i shiftCount = _mm_cvtsi32_si128(8);
		int i = pixelStart;
		if (pixelStride == 4)
		{
			for (; i <= pixelEnd - 4; i += 4)
			{
				const __m128i pixels = _mm_loadu_si128(reinterpret_cast<const __m128i*>(target + (i * 4)));
				const __m128i riskVec = _mm_setr_epi32(risk[i], risk[i + 1], risk[i + 2], risk[i + 3]);
				const __m128i lumaVec = _mm_setr_epi32(enhancedLuma[i], enhancedLuma[i + 1],
					enhancedLuma[i + 2], enhancedLuma[i + 3]);
				const __m128i strength = _mm_srli_epi32(_mm_add_epi32(
					MulloEpi32Compat(riskVec, maxStrengthVec), strengthRounding), 8);
				const __m128i attenuation = _mm_sub_epi32(fullAttenuation, strength);

				const __m128i c0 = _mm_and_si128(pixels, channelMask);
				const __m128i c1 = _mm_and_si128(_mm_srli_epi32(pixels, 8), channelMask);
				const __m128i c2 = _mm_and_si128(_mm_srli_epi32(pixels, 16), channelMask);
				const __m128i out0 = AttenuateChannelSSSE3(c0, lumaVec, attenuation, channelRounding, shiftCount);
				const __m128i out1 = AttenuateChannelSSSE3(c1, lumaVec, attenuation, channelRounding, shiftCount);
				const __m128i out2 = AttenuateChannelSSSE3(c2, lumaVec, attenuation, channelRounding, shiftCount);
				StoreRGB32Pixels4(target + (i * 4), InterleaveChannels3x4(out0, out1, out2));
			}
		}
		else
		{
			for (; i <= pixelEnd - 6; i += 4)
			{
				const __m128i pixels = LoadRGB24AsRGBX4(target + (i * 3));
				const __m128i riskVec = _mm_setr_epi32(risk[i], risk[i + 1], risk[i + 2], risk[i + 3]);
				const __m128i lumaVec = _mm_setr_epi32(enhancedLuma[i], enhancedLuma[i + 1],
					enhancedLuma[i + 2], enhancedLuma[i + 3]);
				const __m128i strength = _mm_srli_epi32(_mm_add_epi32(
					MulloEpi32Compat(riskVec, maxStrengthVec), strengthRounding), 8);
				const __m128i attenuation = _mm_sub_epi32(fullAttenuation, strength);

				const __m128i c0 = _mm_and_si128(pixels, channelMask);
				const __m128i c1 = _mm_and_si128(_mm_srli_epi32(pixels, 8), channelMask);
				const __m128i c2 = _mm_and_si128(_mm_srli_epi32(pixels, 16), channelMask);
				const __m128i out0 = AttenuateChannelSSSE3(c0, lumaVec, attenuation, channelRounding, shiftCount);
				const __m128i out1 = AttenuateChannelSSSE3(c1, lumaVec, attenuation, channelRounding, shiftCount);
				const __m128i out2 = AttenuateChannelSSSE3(c2, lumaVec, attenuation, channelRounding, shiftCount);
				StoreRGB24Pixels4(target + (i * 3), InterleaveChannels3x4(out0, out1, out2));
			}
		}
		ApplyChromaAttenuationScalar(target, enhancedLuma, risk, i, pixelEnd, pixelStride, maxStrengthQ8);
	}
}
