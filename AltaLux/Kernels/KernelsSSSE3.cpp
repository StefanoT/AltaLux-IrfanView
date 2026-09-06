#include "KernelsInternal.h"

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

	// Loads four RGB32/BGR32 pixels, extracts the three color bytes from each
	// dword, applies the layer weight, and builds three channel-pure vectors
	// that the planar accumulator stores without reordering.
	inline void LoadWeightedRGB32Channels4(const unsigned char* src, __m128i weightVec,
		__m128i& c0, __m128i& c1, __m128i& c2)
	{
		const __m128i channelMask = _mm_set1_epi32(0xFF);
		const __m128i pixels = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src));
		c0 = MulloEpi32Compat(_mm_and_si128(pixels, channelMask), weightVec);
		c1 = MulloEpi32Compat(_mm_and_si128(_mm_srli_epi32(pixels, 8), channelMask), weightVec);
		c2 = MulloEpi32Compat(_mm_and_si128(_mm_srli_epi32(pixels, 16), channelMask), weightVec);
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

	inline int AbsDiffByte(int a, int b)
	{
		const int diff = a - b;
		return diff < 0 ? -diff : diff;
	}

	// One [1 2 1] / 4 tap along x over sixteen bytes. For the horizontal pass
	// a, b and c are the same row shifted by one byte; for the vertical pass
	// they are three neighboring rows at the same column.
	inline __m128i BlurTaps16SSSE3(const unsigned char* a, const unsigned char* b,
		const unsigned char* c)
	{
		const __m128i zero = _mm_setzero_si128();
		const __m128i two = _mm_set1_epi16(2);
		const __m128i aW = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a));
		const __m128i bW = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b));
		const __m128i cW = _mm_loadu_si128(reinterpret_cast<const __m128i*>(c));
		const __m128i sumLo = _mm_add_epi16(
			_mm_add_epi16(_mm_unpacklo_epi8(aW, zero), _mm_unpacklo_epi8(cW, zero)),
			_mm_add_epi16(_mm_unpacklo_epi8(bW, zero), _mm_unpacklo_epi8(bW, zero)));
		const __m128i sumHi = _mm_add_epi16(
			_mm_add_epi16(_mm_unpackhi_epi8(aW, zero), _mm_unpackhi_epi8(cW, zero)),
			_mm_add_epi16(_mm_unpackhi_epi8(bW, zero), _mm_unpackhi_epi8(bW, zero)));
		return _mm_packus_epi16(
			_mm_srli_epi16(_mm_add_epi16(sumLo, two), 2),
			_mm_srli_epi16(_mm_add_epi16(sumHi, two), 2));
	}

	// Accumulates |n - c| for sixteen bytes into 16-bit lane sums; eight of
	// them can reach 2040, so the sums must live in 16 bits.
	inline void AccumulateAbsDiff16SSSE3(__m128i& sumLo, __m128i& sumHi,
		const unsigned char* neighbors, __m128i center)
	{
		const __m128i zero = _mm_setzero_si128();
		const __m128i n = _mm_loadu_si128(reinterpret_cast<const __m128i*>(neighbors));
		const __m128i diff = _mm_sub_epi8(_mm_max_epu8(n, center), _mm_min_epu8(n, center));
		sumLo = _mm_add_epi16(sumLo, _mm_unpacklo_epi8(diff, zero));
		sumHi = _mm_add_epi16(sumHi, _mm_unpackhi_epi8(diff, zero));
	}

	// Mean absolute deviation from the 3x3 neighborhood for sixteen interior
	// pixels. up, mid and down point at the block's left column; the center
	// row's vector starts one byte later.
	inline __m128i Activity16SSSE3(const unsigned char* up, const unsigned char* mid,
		const unsigned char* down, const unsigned char* center)
	{
		const __m128i zero = _mm_setzero_si128();
		const __m128i rounding = _mm_set1_epi16(4);
		const __m128i c = _mm_loadu_si128(reinterpret_cast<const __m128i*>(center));
		__m128i sumLo = zero;
		__m128i sumHi = zero;
		AccumulateAbsDiff16SSSE3(sumLo, sumHi, up, c);
		AccumulateAbsDiff16SSSE3(sumLo, sumHi, up + 1, c);
		AccumulateAbsDiff16SSSE3(sumLo, sumHi, up + 2, c);
		AccumulateAbsDiff16SSSE3(sumLo, sumHi, mid, c);
		AccumulateAbsDiff16SSSE3(sumLo, sumHi, mid + 2, c);
		AccumulateAbsDiff16SSSE3(sumLo, sumHi, down, c);
		AccumulateAbsDiff16SSSE3(sumLo, sumHi, down + 1, c);
		AccumulateAbsDiff16SSSE3(sumLo, sumHi, down + 2, c);
		return _mm_packus_epi16(
			_mm_srli_epi16(_mm_add_epi16(sumLo, rounding), 3),
			_mm_srli_epi16(_mm_add_epi16(sumHi, rounding), 3));
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
	void AccumulateLayerSSSE3(unsigned int* accum, int planeStride, const unsigned char* layer, int pixelStart,
		int pixelEnd, int pixelStride, int weight, bool firstLayer)
	{
		const __m128i weightVec = _mm_set1_epi32(weight);
		int p = pixelStart;
		const unsigned char* src = layer + (pixelStart * pixelStride);
		unsigned int* dst = accum + (pixelStart * 3);

		if (pixelStride == 4)
		{
			// Planar accumulator: channel-pure vectors store straight into the
			// R/G/B planes with no triplet reordering.
			unsigned int* dstR = accum + pixelStart;
			unsigned int* dstG = dstR + planeStride;
			unsigned int* dstB = dstG + planeStride;
			if (firstLayer)
			{
				for (; p <= pixelEnd - 4; p += 4, src += 16, dstR += 4, dstG += 4, dstB += 4)
				{
					__m128i c0, c1, c2;
					LoadWeightedRGB32Channels4(src, weightVec, c0, c1, c2);
					_mm_storeu_si128(reinterpret_cast<__m128i*>(dstR), c0);
					_mm_storeu_si128(reinterpret_cast<__m128i*>(dstG), c1);
					_mm_storeu_si128(reinterpret_cast<__m128i*>(dstB), c2);
				}
			}
			else
			{
				for (; p <= pixelEnd - 4; p += 4, src += 16, dstR += 4, dstG += 4, dstB += 4)
				{
					__m128i c0, c1, c2;
					LoadWeightedRGB32Channels4(src, weightVec, c0, c1, c2);
					_mm_storeu_si128(reinterpret_cast<__m128i*>(dstR),
						_mm_add_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(dstR)), c0));
					_mm_storeu_si128(reinterpret_cast<__m128i*>(dstG),
						_mm_add_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(dstG)), c1));
					_mm_storeu_si128(reinterpret_cast<__m128i*>(dstB),
						_mm_add_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(dstB)), c2));
				}
			}
			AccumulateLayerScalar(accum, planeStride, layer, p, pixelEnd, pixelStride, weight, firstLayer);
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
			AccumulateLayerScalar(accum, planeStride, layer, p, pixelEnd, pixelStride, weight, firstLayer);
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
		AccumulateLayerScalar(accum, planeStride, layer, p, pixelEnd, pixelStride, weight, firstLayer);
	}

	// Converts accumulated weighted sums back to RGB bytes four pixels at a time.
	// Three vectors hold the twelve channel sums; each lane is rounded, shifted,
	// saturated to bytes, and stored through RGB24 or RGB32 packing helpers.
	void WriteAccumulatedImageSSSE3(unsigned char* target, const unsigned int* accum, int planeStride,
		int pixelStart, int pixelEnd, int pixelStride, int weightScaleLog2, int weightHalf)
	{
		const __m128i roundingVec = _mm_set1_epi32(weightHalf);
		const __m128i shiftVec = _mm_cvtsi32_si128(weightScaleLog2);
		int p = pixelStart;
		if (pixelStride == 4)
		{
			// Planar accumulator: three channel-pure loads replace the
			// interleaved ones; the interleave happens once here at write-out.
			const unsigned int* srcR = accum + pixelStart;
			const unsigned int* srcG = srcR + planeStride;
			const unsigned int* srcB = srcG + planeStride;
			for (; p <= pixelEnd - 4; p += 4, srcR += 4, srcG += 4, srcB += 4)
			{
				const __m128i e0 = _mm_srl_epi32(
					_mm_add_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(srcR)), roundingVec),
					shiftVec);
				const __m128i e1 = _mm_srl_epi32(
					_mm_add_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(srcG)), roundingVec),
					shiftVec);
				const __m128i e2 = _mm_srl_epi32(
					_mm_add_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(srcB)), roundingVec),
					shiftVec);
				StoreRGB32Pixels4(target + (p * 4), InterleaveChannels3x4(e0, e1, e2));
			}
			WriteAccumulatedImageScalar(target, accum, planeStride, p, pixelEnd, pixelStride, weightScaleLog2, weightHalf);
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
			WriteAccumulatedImageScalar(target, accum, planeStride, p, pixelEnd, pixelStride, weightScaleLog2, weightHalf);
			return;
		}

		WriteAccumulatedImageScalar(target, accum, planeStride, p, pixelEnd, pixelStride, weightScaleLog2, weightHalf);
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

	// Row-vectorized [1 2 1] / 4 blur. The horizontal pass keeps the two
	// replicated-edge columns scalar and feeds the interior to the tap helper;
	// the vertical pass covers full-width chunks because its rows are already
	// clamped. Both tails are scalar.
	void BlurRiskMapSSSE3(unsigned char* risk, unsigned char* temp, int width, int height)
	{
		if (risk == nullptr || temp == nullptr || width <= 0 || height <= 0)
		{
			return;
		}

		const int last = width - 1;
		for (int y = 0; y < height; ++y)
		{
			const unsigned char* row = risk + (y * width);
			unsigned char* outRow = temp + (y * width);
			int x = 1;
			for (; x <= last - 16; x += 16)
			{
				_mm_storeu_si128(reinterpret_cast<__m128i*>(outRow + x),
					BlurTaps16SSSE3(row + x - 1, row + x, row + x + 1));
			}
			for (; x < last; ++x)
			{
				outRow[x] = static_cast<unsigned char>((row[x - 1] + (row[x] << 1) + row[x + 1] + 2) >> 2);
			}
			outRow[0] = static_cast<unsigned char>(((row[0] * 3) + row[1] + 2) >> 2);
			outRow[last] = static_cast<unsigned char>((row[last - 1] + (row[last] * 3) + 2) >> 2);
		}

		for (int y = 0; y < height; ++y)
		{
			const unsigned char* rowUp = temp + ((y > 0 ? y - 1 : 0) * width);
			const unsigned char* row = temp + (y * width);
			const unsigned char* rowDown = temp + ((y < height - 1 ? y + 1 : height - 1) * width);
			unsigned char* outRow = risk + (y * width);
			int x = 0;
			for (; x <= width - 16; x += 16)
			{
				_mm_storeu_si128(reinterpret_cast<__m128i*>(outRow + x),
					BlurTaps16SSSE3(rowUp + x, row + x, rowDown + x));
			}
			for (; x < width; ++x)
			{
				outRow[x] = static_cast<unsigned char>((rowUp[x] + (row[x] << 1) + rowDown[x] + 2) >> 2);
			}
		}
	}

	// Row-vectorized 3x3 mean absolute deviation; the border columns use the
	// same replicated-edge formulas as the scalar kernel.
	void ComputeLocalActivity3x3SSSE3(const unsigned char* luma, unsigned char* activity,
		int width, int height)
	{
		if (luma == nullptr || activity == nullptr || width <= 0 || height <= 0)
		{
			return;
		}

		const int last = width - 1;
		for (int y = 0; y < height; ++y)
		{
			const unsigned char* rowUp = luma + ((y > 0 ? y - 1 : 0) * width);
			const unsigned char* row = luma + (y * width);
			const unsigned char* rowDown = luma + ((y < height - 1 ? y + 1 : height - 1) * width);
			unsigned char* out = activity + (y * width);
			int x = 1;
			for (; x <= width - 17; x += 16)
			{
				_mm_storeu_si128(reinterpret_cast<__m128i*>(out + x),
					Activity16SSSE3(rowUp + x - 1, row + x - 1, rowDown + x - 1, row + x));
			}
			for (; x < last; ++x)
			{
				const int center = row[x];
				int sum = AbsDiffByte(rowUp[x - 1], center) + AbsDiffByte(rowUp[x], center) + AbsDiffByte(rowUp[x + 1], center);
				sum += AbsDiffByte(row[x - 1], center) + AbsDiffByte(row[x + 1], center);
				sum += AbsDiffByte(rowDown[x - 1], center) + AbsDiffByte(rowDown[x], center) + AbsDiffByte(rowDown[x + 1], center);
				out[x] = static_cast<unsigned char>((sum + 4) >> 3);
			}

			const int centerFirst = row[0];
			const int sumFirst = AbsDiffByte(rowUp[0], centerFirst) + AbsDiffByte(rowUp[1], centerFirst)
				+ AbsDiffByte(row[0], centerFirst) + AbsDiffByte(row[1], centerFirst)
				+ AbsDiffByte(rowDown[0], centerFirst) + AbsDiffByte(rowDown[1], centerFirst);
			out[0] = static_cast<unsigned char>((sumFirst + 4) >> 3);
			const int centerLast = row[last];
			const int sumLast = AbsDiffByte(rowUp[last - 1], centerLast) + AbsDiffByte(rowUp[last], centerLast)
				+ AbsDiffByte(row[last - 1], centerLast) + AbsDiffByte(row[last], centerLast)
				+ AbsDiffByte(rowDown[last - 1], centerLast) + AbsDiffByte(rowDown[last], centerLast);
			out[last] = static_cast<unsigned char>((sumLast + 4) >> 3);
		}
	}
}
