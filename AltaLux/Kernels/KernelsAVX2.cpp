#include "KernelsInternal.h"

#include <immintrin.h>

namespace
{
	struct ScaleCapLutTable
	{
		int table[256];
	};

	// Lookup table used by RGB32 luma injection to cap the scale factor so no
	// color channel exceeds 255. AVX2 gathers this by max input channel value.
	const ScaleCapLutTable g_ScaleCapLut = []() {
		ScaleCapLutTable lut = {};
		lut.table[0] = 0x7FFFFFFF;
		for (int value = 1; value < 256; ++value)
		{
			lut.table[value] = (255 << 8) / value;
		}
		return lut;
	}();

	inline __m128i CalculateRGBLuma4FromPacked(__m128i pixels, __m128i factors,
		__m128i roundingOffset, __m128i shiftCount);

	// Computes luma for four RGB32/BGR32 pixels. This is the same 128-bit data
	// shape as the SSSE3 helper: widen bytes, pairwise multiply-add channel factors,
	// merge pair sums, then round and shift to one 32-bit luma per pixel.
	inline __m128i CalculateRGB32Luma4(const unsigned char* src, __m128i factors,
		__m128i roundingOffset, __m128i shiftCount)
	{
		const __m128i pixels = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src));
		return CalculateRGBLuma4FromPacked(pixels, factors, roundingOffset, shiftCount);
	}

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

	inline __m128i LoadRGB24AsRGBX4(const unsigned char* src)
	{
		const __m128i rgb24ToRGBX = _mm_setr_epi8(
			0, 1, 2, -1,
			3, 4, 5, -1,
			6, 7, 8, -1,
			9, 10, 11, -1);
		return _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(src)), rgb24ToRGBX);
	}

	// Stores four 32-bit luma values as four saturated bytes. Used for RGB32 paths
	// that reuse the 128-bit luma helper under AVX2 code generation.
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
	// PSHUFB expands contiguous RGB triplets into RGBX dwords, then the original
	// alpha/unused byte is ORed back into each pixel.
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

	// Reorders eight vectors of per-channel RGB32 data from planar lanes
	// RRRRRRRR/GGGGGGGG/BBBBBBBB into the accumulator's RGBRGB... uint32 triplet layout.
	inline void BuildAccumTriplets8(__m256i c0, __m256i c1, __m256i c2,
		__m256i& out0, __m256i& out1, __m256i& out2)
	{
		const __m256i out0C0 = _mm256_permutevar8x32_epi32(c0, _mm256_setr_epi32(0, 0, 0, 1, 0, 0, 2, 0));
		const __m256i out0C1 = _mm256_permutevar8x32_epi32(c1, _mm256_setr_epi32(0, 0, 0, 0, 1, 0, 0, 2));
		const __m256i out0C2 = _mm256_permutevar8x32_epi32(c2, _mm256_setr_epi32(0, 0, 0, 0, 0, 1, 0, 0));
		out0 = _mm256_blend_epi32(_mm256_blend_epi32(out0C0, out0C1, 0x92), out0C2, 0x24);

		const __m256i out1C0 = _mm256_permutevar8x32_epi32(c0, _mm256_setr_epi32(0, 3, 0, 0, 4, 0, 0, 5));
		const __m256i out1C1 = _mm256_permutevar8x32_epi32(c1, _mm256_setr_epi32(0, 0, 3, 0, 0, 4, 0, 0));
		const __m256i out1C2 = _mm256_permutevar8x32_epi32(c2, _mm256_setr_epi32(2, 0, 0, 3, 0, 0, 4, 0));
		out1 = _mm256_blend_epi32(_mm256_blend_epi32(out1C0, out1C1, 0x24), out1C2, 0x49);

		const __m256i out2C0 = _mm256_permutevar8x32_epi32(c0, _mm256_setr_epi32(0, 0, 6, 0, 0, 7, 0, 0));
		const __m256i out2C1 = _mm256_permutevar8x32_epi32(c1, _mm256_setr_epi32(5, 0, 0, 6, 0, 0, 7, 0));
		const __m256i out2C2 = _mm256_permutevar8x32_epi32(c2, _mm256_setr_epi32(0, 5, 0, 0, 6, 0, 0, 7));
		out2 = _mm256_blend_epi32(_mm256_blend_epi32(out2C0, out2C1, 0x49), out2C2, 0x92);
	}

	// Loads eight RGB32/BGR32 pixels, extracts the three color bytes from each
	// dword, applies the layer weight, and builds three accumulator vectors.
	inline void LoadWeightedRGB32Accum8(const unsigned char* src, __m256i weightVec,
		__m256i& w0, __m256i& w1, __m256i& w2)
	{
		const __m256i channelMask = _mm256_set1_epi32(0xFF);
		const __m256i pixels = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src));
		const __m256i c0 = _mm256_mullo_epi32(_mm256_and_si256(pixels, channelMask), weightVec);
		const __m256i c1 = _mm256_mullo_epi32(_mm256_and_si256(_mm256_srli_epi32(pixels, 8), channelMask), weightVec);
		const __m256i c2 = _mm256_mullo_epi32(_mm256_and_si256(_mm256_srli_epi32(pixels, 16), channelMask), weightVec);
		BuildAccumTriplets8(c0, c1, c2, w0, w1, w2);
	}

	inline __m256i LoadTwoRGB24ScaleDownChunksAVX2(const unsigned char* row)
	{
		const __m128i firstTwoDestPixels = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row));
		const __m128i secondTwoDestPixels = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row + 12));
		return _mm256_inserti128_si256(_mm256_castsi128_si256(firstTwoDestPixels), secondTwoDestPixels, 1);
	}

	inline __m256i AverageBox2x2ShuffleAVX2(__m256i topPixels, __m256i bottomPixels,
		__m256i evenMask, __m256i oddMask)
	{
		const __m256i zero = _mm256_setzero_si256();
		const __m256i topEven = _mm256_shuffle_epi8(topPixels, evenMask);
		const __m256i topOdd = _mm256_shuffle_epi8(topPixels, oddMask);
		const __m256i bottomEven = _mm256_shuffle_epi8(bottomPixels, evenMask);
		const __m256i bottomOdd = _mm256_shuffle_epi8(bottomPixels, oddMask);
		__m256i sum = _mm256_add_epi16(_mm256_unpacklo_epi8(topEven, zero), _mm256_unpacklo_epi8(topOdd, zero));
		sum = _mm256_add_epi16(sum, _mm256_unpacklo_epi8(bottomEven, zero));
		sum = _mm256_add_epi16(sum, _mm256_unpacklo_epi8(bottomOdd, zero));
		return _mm256_packus_epi16(_mm256_srli_epi16(sum, 2), zero);
	}

	inline void StoreFourRGB32DownscaledPixelsAVX2(unsigned char* target, __m256i pixels)
	{
		_mm_storel_epi64(reinterpret_cast<__m128i*>(target), _mm256_castsi256_si128(pixels));
		_mm_storel_epi64(reinterpret_cast<__m128i*>(target + 8), _mm256_extracti128_si256(pixels, 1));
	}

	inline void StoreLow6BytesAVX2(unsigned char* target, __m128i value)
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

	// Moves one 256-bit channel vector toward the enhanced luma:
	// y + ((c - y) * A) >> 8. The shift is arithmetic because (c - y) * A can
	// be negative; the result always lies between y and c, so it stays in
	// byte range without saturation.
	inline __m256i AttenuateChannel256(__m256i channel, __m256i luma, __m256i attenuation,
		__m256i rounding, __m128i shiftCount)
	{
		const __m256i diff = _mm256_sub_epi32(channel, luma);
		const __m256i scaled = _mm256_mullo_epi32(diff, attenuation);
		return _mm256_add_epi32(luma, _mm256_sra_epi32(_mm256_add_epi32(scaled, rounding), shiftCount));
	}

	// 128-bit variant of AttenuateChannel256, used by the RGB24 path where
	// pixels are processed in groups of four through PSHUFB loads.
	inline __m128i AttenuateChannel128(__m128i channel, __m128i luma, __m128i attenuation,
		__m128i rounding, __m128i shiftCount)
	{
		const __m128i diff = _mm_sub_epi32(channel, luma);
		const __m128i scaled = _mm_mullo_epi32(diff, attenuation);
		return _mm_add_epi32(luma, _mm_sra_epi32(_mm_add_epi32(scaled, rounding), shiftCount));
	}

	inline void StoreFourRGB24DownscaledPixelsAVX2(unsigned char* target, __m256i pixels)
	{
		StoreLow6BytesAVX2(target, _mm256_castsi256_si128(pixels));
		StoreLow6BytesAVX2(target + 6, _mm256_extracti128_si256(pixels, 1));
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
}

namespace AltaLuxKernels
{
	// Extracts the luma byte from packed 16-bit YUV words. Two 256-bit loads cover
	// thirty-two pixels; the wanted byte is shifted or masked into 16-bit lanes,
	// packed to bytes, and permuted because AVX2 packing works per 128-bit lane.
	void ExtractPackedYUVLumaAVX2(const unsigned char* source, unsigned char* luma,
		int pixelCount, PackedYUVLumaPosition lumaPosition)
	{
		int i = 0;
		const int vectorizedPixels = pixelCount & ~31;
		if (lumaPosition == PackedYUVLumaPosition::HighByte)
		{
			for (; i < vectorizedPixels; i += 32)
			{
				__m256i chunk0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(source + (i * 2)));
				__m256i chunk1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(source + (i * 2) + 32));
				chunk0 = _mm256_srli_epi16(chunk0, 8);
				chunk1 = _mm256_srli_epi16(chunk1, 8);
				__m256i packed = _mm256_packus_epi16(chunk0, chunk1);
				packed = _mm256_permute4x64_epi64(packed, 0xD8);
				_mm256_storeu_si256(reinterpret_cast<__m256i*>(luma + i), packed);
			}
		}
		else
		{
			const __m256i lumaMask = _mm256_set1_epi16(0x00FF);
			for (; i < vectorizedPixels; i += 32)
			{
				__m256i chunk0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(source + (i * 2)));
				__m256i chunk1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(source + (i * 2) + 32));
				chunk0 = _mm256_and_si256(chunk0, lumaMask);
				chunk1 = _mm256_and_si256(chunk1, lumaMask);
				__m256i packed = _mm256_packus_epi16(chunk0, chunk1);
				packed = _mm256_permute4x64_epi64(packed, 0xD8);
				_mm256_storeu_si256(reinterpret_cast<__m256i*>(luma + i), packed);
			}
		}
		ExtractPackedYUVLumaSSSE3(source + (i * 2), luma + i, pixelCount - i, lumaPosition);
	}

	// Replaces the luma byte inside packed 16-bit YUV words without disturbing
	// chroma. Sixteen luma bytes are widened per half, placed in either byte of
	// each word, masked with preserved chroma, and stored back in two chunks.
	void InjectPackedYUVLumaAVX2(unsigned char* target, const unsigned char* luma,
		int pixelCount, PackedYUVLumaPosition lumaPosition)
	{
		int i = 0;
		const int vectorizedPixels = pixelCount & ~31;
		if (lumaPosition == PackedYUVLumaPosition::HighByte)
		{
			const __m256i chromaMask = _mm256_set1_epi16(0x00FF);
			for (; i < vectorizedPixels; i += 32)
			{
				__m256i y0 = _mm256_cvtepu8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(luma + i)));
				__m256i y1 = _mm256_cvtepu8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(luma + i + 16)));
				y0 = _mm256_slli_epi16(y0, 8);
				y1 = _mm256_slli_epi16(y1, 8);
				__m256i img0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(target + (i * 2)));
				__m256i img1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(target + (i * 2) + 32));
				img0 = _mm256_or_si256(_mm256_and_si256(img0, chromaMask), y0);
				img1 = _mm256_or_si256(_mm256_and_si256(img1, chromaMask), y1);
				_mm256_storeu_si256(reinterpret_cast<__m256i*>(target + (i * 2)), img0);
				_mm256_storeu_si256(reinterpret_cast<__m256i*>(target + (i * 2) + 32), img1);
			}
		}
		else
		{
			const __m256i chromaMask = _mm256_set1_epi16(static_cast<short>(0xFF00));
			for (; i < vectorizedPixels; i += 32)
			{
				const __m256i y0 = _mm256_cvtepu8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(luma + i)));
				const __m256i y1 = _mm256_cvtepu8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(luma + i + 16)));
				__m256i img0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(target + (i * 2)));
				__m256i img1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(target + (i * 2) + 32));
				img0 = _mm256_or_si256(_mm256_and_si256(img0, chromaMask), y0);
				img1 = _mm256_or_si256(_mm256_and_si256(img1, chromaMask), y1);
				_mm256_storeu_si256(reinterpret_cast<__m256i*>(target + (i * 2)), img0);
				_mm256_storeu_si256(reinterpret_cast<__m256i*>(target + (i * 2) + 32), img1);
			}
		}
		InjectPackedYUVLumaSSSE3(target + (i * 2), luma + i, pixelCount - i, lumaPosition);
	}

	// Extracts luma from RGB/BGR input. RGB24 uses contiguous loads plus PSHUFB to
	// expand two 4-pixel groups from 3-byte pixels into RGBX dwords; this avoids
	// the high latency of AVX2 gathers at 3-byte offsets. RGB32 uses the same
	// 128-bit four-pixel helper twice per loop.
	void ExtractRGBLumaAVX2(const unsigned char* source, unsigned char* luma, int pixelCount,
		int pixelStride, int firstFactor, int secondFactor, int thirdFactor, int scalingLog)
	{
		int i = 0;
		if (pixelStride == 3)
		{
			const __m128i factors = _mm_setr_epi16(
				static_cast<short>(firstFactor), static_cast<short>(secondFactor), static_cast<short>(thirdFactor), 0,
				static_cast<short>(firstFactor), static_cast<short>(secondFactor), static_cast<short>(thirdFactor), 0);
			const __m128i roundingOffset = _mm_set1_epi32(1 << (scalingLog - 1));
			const __m128i shiftCount = _mm_cvtsi32_si128(scalingLog);
			for (; i <= pixelCount - 10; i += 8)
			{
				StoreLuma4(luma + i, CalculateRGBLuma4FromPacked(
					LoadRGB24AsRGBX4(source + (i * 3)), factors, roundingOffset, shiftCount));
				StoreLuma4(luma + i + 4, CalculateRGBLuma4FromPacked(
					LoadRGB24AsRGBX4(source + ((i + 4) * 3)), factors, roundingOffset, shiftCount));
			}
			ExtractRGBLumaScalar(source + (i * 3), luma + i, pixelCount - i,
				3, firstFactor, secondFactor, thirdFactor, scalingLog);
			return;
		}

		if (pixelStride == 4)
		{
			const __m128i factors = _mm_setr_epi16(
				static_cast<short>(firstFactor), static_cast<short>(secondFactor), static_cast<short>(thirdFactor), 0,
				static_cast<short>(firstFactor), static_cast<short>(secondFactor), static_cast<short>(thirdFactor), 0);
			const __m128i roundingOffset = _mm_set1_epi32(1 << (scalingLog - 1));
			const __m128i shiftCount = _mm_cvtsi32_si128(scalingLog);
			for (; i <= pixelCount - 8; i += 8)
			{
				StoreLuma4(luma + i, CalculateRGB32Luma4(source + (i * 4), factors, roundingOffset, shiftCount));
				StoreLuma4(luma + i + 4, CalculateRGB32Luma4(source + (i * 4) + 16, factors, roundingOffset, shiftCount));
			}
			ExtractRGBLumaSSSE3(source + (i * 4), luma + i, pixelCount - i,
				4, firstFactor, secondFactor, thirdFactor, scalingLog);
			return;
		}

		ExtractRGBLumaScalar(source, luma, pixelCount, pixelStride,
			firstFactor, secondFactor, thirdFactor, scalingLog);
	}

	// Injects processed luma into RGB32/BGR32 pixels eight at a time. Channels are
	// extracted with masks/shifts, old luma and scale factors are computed in
	// 32-bit lanes, reciprocal/cap tables are gathered, and RGB bytes are rebuilt.
	void InjectRGBLumaAVX2(unsigned char* image, const unsigned char* luma, int pixelCount,
		int pixelStride, int firstFactor, int secondFactor, int thirdFactor, int scalingLog,
		const int* reciprocalLut)
	{
		if (pixelStride != 4)
		{
			InjectRGBLumaScalar(image, luma, pixelCount, pixelStride,
				firstFactor, secondFactor, thirdFactor, scalingLog, reciprocalLut);
			return;
		}

		const __m256i channelMask = _mm256_set1_epi32(0xFF);
		const __m256i alphaMask = _mm256_set1_epi32(static_cast<int>(0xFF000000u));
		const __m256i firstFactorVec = _mm256_set1_epi32(firstFactor);
		const __m256i secondFactorVec = _mm256_set1_epi32(secondFactor);
		const __m256i thirdFactorVec = _mm256_set1_epi32(thirdFactor);
		const __m256i roundingOffset = _mm256_set1_epi32(1 << (scalingLog - 1));
		const __m256i scaleRounding = _mm256_set1_epi32(1 << 7);
		const __m256i maxByte = _mm256_set1_epi32(255);
		const __m128i shiftCount = _mm_cvtsi32_si128(scalingLog);
		int i = 0;
		for (; i <= pixelCount - 8; i += 8)
		{
			const __m256i pixels = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(image + (i * 4)));
			const __m256i c0 = _mm256_and_si256(pixels, channelMask);
			const __m256i c1 = _mm256_and_si256(_mm256_srli_epi32(pixels, 8), channelMask);
			const __m256i c2 = _mm256_and_si256(_mm256_srli_epi32(pixels, 16), channelMask);
			__m256i oldY = _mm256_add_epi32(_mm256_mullo_epi32(c0, firstFactorVec),
				_mm256_mullo_epi32(c1, secondFactorVec));
			oldY = _mm256_add_epi32(oldY, _mm256_mullo_epi32(c2, thirdFactorVec));
			oldY = _mm256_sra_epi32(_mm256_add_epi32(oldY, roundingOffset), shiftCount);
			oldY = _mm256_min_epi32(oldY, maxByte);

			const __m256i newY = _mm256_cvtepu8_epi32(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(luma + i)));
			const __m256i reciprocal = _mm256_i32gather_epi32(reciprocalLut, oldY, 4);
			__m256i scale = _mm256_srli_epi32(_mm256_add_epi32(
				_mm256_mullo_epi32(newY, reciprocal), scaleRounding), 8);

			__m256i maxChannel = _mm256_max_epi32(c0, c1);
			maxChannel = _mm256_max_epi32(maxChannel, c2);
			const __m256i scaleCap = _mm256_i32gather_epi32(g_ScaleCapLut.table, maxChannel, 4);
			scale = _mm256_min_epi32(scale, scaleCap);

			const __m256i out0 = _mm256_srli_epi32(_mm256_add_epi32(_mm256_mullo_epi32(c0, scale), scaleRounding), 8);
			const __m256i out1 = _mm256_srli_epi32(_mm256_add_epi32(_mm256_mullo_epi32(c1, scale), scaleRounding), 8);
			const __m256i out2 = _mm256_srli_epi32(_mm256_add_epi32(_mm256_mullo_epi32(c2, scale), scaleRounding), 8);
			__m256i output = _mm256_or_si256(_mm256_and_si256(pixels, alphaMask), out0);
			output = _mm256_or_si256(output, _mm256_slli_epi32(out1, 8));
			output = _mm256_or_si256(output, _mm256_slli_epi32(out2, 16));
			_mm256_storeu_si256(reinterpret_cast<__m256i*>(image + (i * 4)), output);
		}
		InjectRGBLumaSSSE3(image + (i * 4), luma + i, pixelCount - i,
				4, firstFactor, secondFactor, thirdFactor, scalingLog, reciprocalLut);
	}

	// Injects processed luma into RGB32/BGR32 pixels when extraction has cached
	// the original luma. Eight RGB32 pixels are loaded, original/new luma bytes
	// are widened, reciprocal and scale-cap tables are gathered, and the scaled
	// RGB channels are rebuilt while preserving alpha/padding bytes.
	void InjectRGBLumaWithOriginalLumaAVX2(unsigned char* image, const unsigned char* luma,
		const unsigned char* originalLuma, int pixelCount, int pixelStride,
		const int* reciprocalLut)
	{
		if (pixelStride != 4)
		{
			InjectRGBLumaWithOriginalLumaScalar(image, luma, originalLuma, pixelCount,
				pixelStride, reciprocalLut);
			return;
		}

		const __m256i channelMask = _mm256_set1_epi32(0xFF);
		const __m256i alphaMask = _mm256_set1_epi32(static_cast<int>(0xFF000000u));
		const __m256i scaleRounding = _mm256_set1_epi32(1 << 7);
		int i = 0;
		for (; i <= pixelCount - 8; i += 8)
		{
			const __m256i pixels = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(image + (i * 4)));
			const __m256i c0 = _mm256_and_si256(pixels, channelMask);
			const __m256i c1 = _mm256_and_si256(_mm256_srli_epi32(pixels, 8), channelMask);
			const __m256i c2 = _mm256_and_si256(_mm256_srli_epi32(pixels, 16), channelMask);
			const __m256i oldY = _mm256_cvtepu8_epi32(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(originalLuma + i)));
			const __m256i newY = _mm256_cvtepu8_epi32(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(luma + i)));
			const __m256i reciprocal = _mm256_i32gather_epi32(reciprocalLut, oldY, 4);
			__m256i scale = _mm256_srli_epi32(_mm256_add_epi32(
				_mm256_mullo_epi32(newY, reciprocal), scaleRounding), 8);

			__m256i maxChannel = _mm256_max_epi32(c0, c1);
			maxChannel = _mm256_max_epi32(maxChannel, c2);
			const __m256i scaleCap = _mm256_i32gather_epi32(g_ScaleCapLut.table, maxChannel, 4);
			scale = _mm256_min_epi32(scale, scaleCap);

			const __m256i out0 = _mm256_srli_epi32(_mm256_add_epi32(_mm256_mullo_epi32(c0, scale), scaleRounding), 8);
			const __m256i out1 = _mm256_srli_epi32(_mm256_add_epi32(_mm256_mullo_epi32(c1, scale), scaleRounding), 8);
			const __m256i out2 = _mm256_srli_epi32(_mm256_add_epi32(_mm256_mullo_epi32(c2, scale), scaleRounding), 8);
			__m256i output = _mm256_or_si256(_mm256_and_si256(pixels, alphaMask), out0);
			output = _mm256_or_si256(output, _mm256_slli_epi32(out1, 8));
			output = _mm256_or_si256(output, _mm256_slli_epi32(out2, 16));
			_mm256_storeu_si256(reinterpret_cast<__m256i*>(image + (i * 4)), output);
		}
		InjectRGBLumaWithOriginalLumaSSSE3(image + (i * 4), luma + i, originalLuma + i,
			pixelCount - i, 4, reciprocalLut);
	}

	// Downscales an RGB24/RGB32 image with a box filter. AVX2 handles the common
	// 2x preview reduction by loading contiguous source bytes, using VPSHUFB to
	// split even and odd horizontal samples in each 128-bit lane, then averaging
	// the four contributing pixels. Other scale factors use the scalar exact kernel.
	void ScaleDownBoxAVX2(const unsigned char* source, int sourceWidth, int sourceHeight,
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
		const __m256i rgb32EvenMask = _mm256_setr_epi8(
			0, 1, 2, 3, 8, 9, 10, 11, -1, -1, -1, -1, -1, -1, -1, -1,
			0, 1, 2, 3, 8, 9, 10, 11, -1, -1, -1, -1, -1, -1, -1, -1);
		const __m256i rgb32OddMask = _mm256_setr_epi8(
			4, 5, 6, 7, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1,
			4, 5, 6, 7, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1);
		const __m256i rgb24EvenMask = _mm256_setr_epi8(
			0, 1, 2, 6, 7, 8, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
			0, 1, 2, 6, 7, 8, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);
		const __m256i rgb24OddMask = _mm256_setr_epi8(
			3, 4, 5, 9, 10, 11, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
			3, 4, 5, 9, 10, 11, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);

		for (int y = 0; y < targetHeight; ++y)
		{
			const unsigned char* topRow = source + ((y * 2) * sourceStride);
			const unsigned char* bottomRow = topRow + sourceStride;
			unsigned char* dst = target + (y * targetStride);
			int x = 0;
			if (pixelStride == 4)
			{
				for (; x <= targetWidth - 4; x += 4)
				{
					const __m256i topPixels = _mm256_loadu_si256(
						reinterpret_cast<const __m256i*>(topRow + (x * 2 * 4)));
					const __m256i bottomPixels = _mm256_loadu_si256(
						reinterpret_cast<const __m256i*>(bottomRow + (x * 2 * 4)));
					const __m256i out = AverageBox2x2ShuffleAVX2(topPixels, bottomPixels,
						rgb32EvenMask, rgb32OddMask);
					StoreFourRGB32DownscaledPixelsAVX2(dst + (x * 4), out);
				}
			}
			else
			{
				for (; x <= targetWidth - 5; x += 4)
				{
					const __m256i topPixels = LoadTwoRGB24ScaleDownChunksAVX2(topRow + (x * 2 * 3));
					const __m256i bottomPixels = LoadTwoRGB24ScaleDownChunksAVX2(bottomRow + (x * 2 * 3));
					const __m256i out = AverageBox2x2ShuffleAVX2(topPixels, bottomPixels,
						rgb24EvenMask, rgb24OddMask);
					StoreFourRGB24DownscaledPixelsAVX2(dst + (x * 3), out);
				}
			}
			ScaleDownBox2x2Tail(topRow, bottomRow, dst + (x * pixelStride), x, targetWidth, pixelStride);
		}
	}

	// Accumulates eight RGB/RGBX pixels into the uint32 weighted-sum buffer. Source
	// bytes are expanded into three AVX2 vectors holding twenty-four channels,
	// multiplied by the weight, then assigned or added to the accumulator.
	void AccumulateLayerAVX2(unsigned int* accum, const unsigned char* layer, int pixelStart,
		int pixelEnd, int pixelStride, int weight, bool firstLayer)
	{
		const __m256i weightVec = _mm256_set1_epi32(weight);
		int p = pixelStart;
		const unsigned char* src = layer + (pixelStart * pixelStride);
		unsigned int* dst = accum + (pixelStart * 3);

		if (pixelStride == 4)
		{
			if (firstLayer)
			{
				for (; p <= pixelEnd - 8; p += 8, src += 32, dst += 24)
				{
					__m256i w0, w1, w2;
					LoadWeightedRGB32Accum8(src, weightVec, w0, w1, w2);
					_mm256_storeu_si256(reinterpret_cast<__m256i*>(dst), w0);
					_mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + 8), w1);
					_mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + 16), w2);
				}
			}
			else
			{
				for (; p <= pixelEnd - 8; p += 8, src += 32, dst += 24)
				{
					__m256i w0, w1, w2;
					LoadWeightedRGB32Accum8(src, weightVec, w0, w1, w2);
					_mm256_storeu_si256(reinterpret_cast<__m256i*>(dst),
						_mm256_add_epi32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(dst)), w0));
					_mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + 8),
						_mm256_add_epi32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(dst + 8)), w1));
					_mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + 16),
						_mm256_add_epi32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(dst + 16)), w2));
				}
			}
			AccumulateLayerSSSE3(accum, layer, p, pixelEnd, pixelStride, weight, firstLayer);
			return;
		}

		const int sourceStep = pixelStride * 8;
		if (firstLayer)
		{
			for (; p <= pixelEnd - 8; p += 8, src += sourceStep, dst += 24)
			{
				const __m256i v0 = _mm256_setr_epi32(src[0], src[1], src[2],
					src[pixelStride], src[pixelStride + 1], src[pixelStride + 2],
					src[pixelStride * 2], src[(pixelStride * 2) + 1]);
				const __m256i v1 = _mm256_setr_epi32(src[(pixelStride * 2) + 2],
					src[pixelStride * 3], src[(pixelStride * 3) + 1], src[(pixelStride * 3) + 2],
					src[pixelStride * 4], src[(pixelStride * 4) + 1], src[(pixelStride * 4) + 2],
					src[pixelStride * 5]);
				const __m256i v2 = _mm256_setr_epi32(src[(pixelStride * 5) + 1], src[(pixelStride * 5) + 2],
					src[pixelStride * 6], src[(pixelStride * 6) + 1], src[(pixelStride * 6) + 2],
					src[pixelStride * 7], src[(pixelStride * 7) + 1], src[(pixelStride * 7) + 2]);
				const __m256i w0 = _mm256_mullo_epi32(v0, weightVec);
				const __m256i w1 = _mm256_mullo_epi32(v1, weightVec);
				const __m256i w2 = _mm256_mullo_epi32(v2, weightVec);
				_mm256_storeu_si256(reinterpret_cast<__m256i*>(dst), w0);
				_mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + 8), w1);
				_mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + 16), w2);
			}
		}
		else
		{
			for (; p <= pixelEnd - 8; p += 8, src += sourceStep, dst += 24)
			{
				const __m256i v0 = _mm256_setr_epi32(src[0], src[1], src[2],
					src[pixelStride], src[pixelStride + 1], src[pixelStride + 2],
					src[pixelStride * 2], src[(pixelStride * 2) + 1]);
				const __m256i v1 = _mm256_setr_epi32(src[(pixelStride * 2) + 2],
					src[pixelStride * 3], src[(pixelStride * 3) + 1], src[(pixelStride * 3) + 2],
					src[pixelStride * 4], src[(pixelStride * 4) + 1], src[(pixelStride * 4) + 2],
					src[pixelStride * 5]);
				const __m256i v2 = _mm256_setr_epi32(src[(pixelStride * 5) + 1], src[(pixelStride * 5) + 2],
					src[pixelStride * 6], src[(pixelStride * 6) + 1], src[(pixelStride * 6) + 2],
					src[pixelStride * 7], src[(pixelStride * 7) + 1], src[(pixelStride * 7) + 2]);
				const __m256i w0 = _mm256_mullo_epi32(v0, weightVec);
				const __m256i w1 = _mm256_mullo_epi32(v1, weightVec);
				const __m256i w2 = _mm256_mullo_epi32(v2, weightVec);
				_mm256_storeu_si256(reinterpret_cast<__m256i*>(dst),
					_mm256_add_epi32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(dst)), w0));
				_mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + 8),
					_mm256_add_epi32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(dst + 8)), w1));
				_mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + 16),
					_mm256_add_epi32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(dst + 16)), w2));
			}
		}
		AccumulateLayerSSSE3(accum, layer, p, pixelEnd, pixelStride, weight, firstLayer);
	}

	// Converts accumulated weighted sums back to RGB bytes eight pixels at a time.
	// Three AVX2 vectors hold the twenty-four channel sums; 128-bit halves are
	// packed into RGB24/RGB32 groups because the final byte layout is 12 bytes per
	// four pixels before optional alpha preservation.
	void WriteAccumulatedImageAVX2(unsigned char* target, const unsigned int* accum, int pixelStart,
		int pixelEnd, int pixelStride, int weightScaleLog2, int weightHalf)
	{
		const __m256i roundingVec = _mm256_set1_epi32(weightHalf);
		const __m128i shiftVec = _mm_cvtsi32_si128(weightScaleLog2);
		int p = pixelStart;
		if (pixelStride == 4)
		{
			for (; p <= pixelEnd - 8; p += 8)
			{
				const unsigned int* src = accum + (p * 3);
				const __m256i e0 = _mm256_srl_epi32(
					_mm256_add_epi32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(src)), roundingVec),
					shiftVec);
				const __m256i e1 = _mm256_srl_epi32(
					_mm256_add_epi32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + 8)), roundingVec),
					shiftVec);
				const __m256i e2 = _mm256_srl_epi32(
					_mm256_add_epi32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + 16)), roundingVec),
					shiftVec);
				StoreRGB32Pixels4(target + (p * 4),
					PackRGBTriplets4(_mm256_castsi256_si128(e0),
						_mm256_extracti128_si256(e0, 1),
						_mm256_castsi256_si128(e1)));
				StoreRGB32Pixels4(target + ((p + 4) * 4),
					PackRGBTriplets4(_mm256_extracti128_si256(e1, 1),
						_mm256_castsi256_si128(e2),
						_mm256_extracti128_si256(e2, 1)));
			}
			WriteAccumulatedImageSSSE3(target, accum, p, pixelEnd, pixelStride, weightScaleLog2, weightHalf);
			return;
		}

		if (pixelStride == 3)
		{
			for (; p <= pixelEnd - 8; p += 8)
			{
				const unsigned int* src = accum + (p * 3);
				const __m256i e0 = _mm256_srl_epi32(
					_mm256_add_epi32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(src)), roundingVec),
					shiftVec);
				const __m256i e1 = _mm256_srl_epi32(
					_mm256_add_epi32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + 8)), roundingVec),
					shiftVec);
				const __m256i e2 = _mm256_srl_epi32(
					_mm256_add_epi32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + 16)), roundingVec),
					shiftVec);
				StoreRGB24Pixels4(target + (p * 3),
					PackRGBTriplets4(_mm256_castsi256_si128(e0),
						_mm256_extracti128_si256(e0, 1),
						_mm256_castsi256_si128(e1)));
				StoreRGB24Pixels4(target + ((p + 4) * 3),
					PackRGBTriplets4(_mm256_extracti128_si256(e1, 1),
						_mm256_castsi256_si128(e2),
						_mm256_extracti128_si256(e2, 1)));
			}
			WriteAccumulatedImageSSSE3(target, accum, p, pixelEnd, pixelStride, weightScaleLog2, weightHalf);
			return;
		}

		WriteAccumulatedImageSSSE3(target, accum, p, pixelEnd, pixelStride, weightScaleLog2, weightHalf);
	}

	// Attenuates chroma toward the enhanced luma. RGB32 processes eight pixels
	// per iteration with channel lanes rebuilt directly into dwords (alpha
	// preserved by mask); RGB24 reuses the 128-bit four-pixel groups of the
	// luma kernels, stopping six pixels short of the end so the 16-byte RGBX
	// loads never read past the buffer.
	void ApplyChromaAttenuationAVX2(unsigned char* target, const unsigned char* enhancedLuma,
		const unsigned char* risk, int pixelStart, int pixelEnd, int pixelStride, int maxStrengthQ8)
	{
		if (pixelStride != 3 && pixelStride != 4)
		{
			ApplyChromaAttenuationScalar(target, enhancedLuma, risk, pixelStart, pixelEnd,
				pixelStride, maxStrengthQ8);
			return;
		}

		const __m256i channelMask = _mm256_set1_epi32(0xFF);
		const __m256i alphaMask = _mm256_set1_epi32(static_cast<int>(0xFF000000u));
		const __m256i maxStrengthVec = _mm256_set1_epi32(maxStrengthQ8);
		const __m256i fullAttenuation = _mm256_set1_epi32(256);
		const __m256i strengthRounding = _mm256_set1_epi32(127);
		const __m256i channelRounding256 = _mm256_set1_epi32(128);
		const __m128i channelRounding128 = _mm_set1_epi32(128);
		const __m128i shiftCount = _mm_cvtsi32_si128(8);
		int i = pixelStart;
		if (pixelStride == 4)
		{
			for (; i <= pixelEnd - 8; i += 8)
			{
				const __m256i pixels = _mm256_loadu_si256(
					reinterpret_cast<const __m256i*>(target + (i * 4)));
				const __m256i riskVec = _mm256_cvtepu8_epi32(
					_mm_loadl_epi64(reinterpret_cast<const __m128i*>(risk + i)));
				const __m256i lumaVec = _mm256_cvtepu8_epi32(
					_mm_loadl_epi64(reinterpret_cast<const __m128i*>(enhancedLuma + i)));
				const __m256i strength = _mm256_srli_epi32(_mm256_add_epi32(
					_mm256_mullo_epi32(riskVec, maxStrengthVec), strengthRounding), 8);
				const __m256i attenuation = _mm256_sub_epi32(fullAttenuation, strength);

				const __m256i c0 = _mm256_and_si256(pixels, channelMask);
				const __m256i c1 = _mm256_and_si256(_mm256_srli_epi32(pixels, 8), channelMask);
				const __m256i c2 = _mm256_and_si256(_mm256_srli_epi32(pixels, 16), channelMask);
				const __m256i out0 = AttenuateChannel256(c0, lumaVec, attenuation, channelRounding256, shiftCount);
				const __m256i out1 = AttenuateChannel256(c1, lumaVec, attenuation, channelRounding256, shiftCount);
				const __m256i out2 = AttenuateChannel256(c2, lumaVec, attenuation, channelRounding256, shiftCount);
				__m256i output = _mm256_or_si256(_mm256_and_si256(pixels, alphaMask), out0);
				output = _mm256_or_si256(output, _mm256_slli_epi32(out1, 8));
				output = _mm256_or_si256(output, _mm256_slli_epi32(out2, 16));
				_mm256_storeu_si256(reinterpret_cast<__m256i*>(target + (i * 4)), output);
			}
		}
		else
		{
			const __m128i channelMask128 = _mm_set1_epi32(0xFF);
			const __m128i maxStrengthVec128 = _mm_set1_epi32(maxStrengthQ8);
			const __m128i fullAttenuation128 = _mm_set1_epi32(256);
			const __m128i strengthRounding128 = _mm_set1_epi32(127);
			for (; i <= pixelEnd - 6; i += 4)
			{
				const __m128i pixels = LoadRGB24AsRGBX4(target + (i * 3));
				const __m128i riskVec = _mm_cvtepu8_epi32(
					_mm_cvtsi32_si128(*reinterpret_cast<const int*>(risk + i)));
				const __m128i lumaVec = _mm_cvtepu8_epi32(
					_mm_cvtsi32_si128(*reinterpret_cast<const int*>(enhancedLuma + i)));
				const __m128i strength = _mm_srli_epi32(_mm_add_epi32(
					_mm_mullo_epi32(riskVec, maxStrengthVec128), strengthRounding128), 8);
				const __m128i attenuation = _mm_sub_epi32(fullAttenuation128, strength);

				const __m128i c0 = _mm_and_si128(pixels, channelMask128);
				const __m128i c1 = _mm_and_si128(_mm_srli_epi32(pixels, 8), channelMask128);
				const __m128i c2 = _mm_and_si128(_mm_srli_epi32(pixels, 16), channelMask128);
				const __m128i out0 = AttenuateChannel128(c0, lumaVec, attenuation, channelRounding128, shiftCount);
				const __m128i out1 = AttenuateChannel128(c1, lumaVec, attenuation, channelRounding128, shiftCount);
				const __m128i out2 = AttenuateChannel128(c2, lumaVec, attenuation, channelRounding128, shiftCount);
				StoreRGB24Pixels4(target + (i * 3), InterleaveChannels3x4(out0, out1, out2));
			}
		}
		ApplyChromaAttenuationSSSE3(target, enhancedLuma, risk, i, pixelEnd, pixelStride, maxStrengthQ8);
	}
}
