#include "AltaLuxCore.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <memory>
#include <vector>

#include <immintrin.h>
#include <ppl.h>

#include "Filter/CAltaLuxFilterFactory.h"
#include "Filter/CBaseAltaLuxFilter.h"

namespace
{
	constexpr int kMultiscaleParallelThreshold = 200000;  // ~450x450; skip parallel_for below
	constexpr int kParallelLayerThreshold = 1000000;       // ~1 MP; below this, extra buffers/tasks rarely pay off
	constexpr int kMultiscaleNumBlocks = 16;              // outer partition count for PPL
	constexpr int kWeightScale = 1024;                    // weights sum to 1024 (2^10)
	constexpr int kWeightScaleLog2 = 10;
	constexpr int kWeightHalf = kWeightScale / 2;         // rounding offset for >>10

	int GetImageByteCount(int width, int height, int bitDepth)
	{
		return width * height * bitDepth;
	}

	std::unique_ptr<CBaseAltaLuxFilter> CreateFilter(int width, int height, int regions)
	{
		return std::unique_ptr<CBaseAltaLuxFilter>(CAltaLuxFilterFactory::CreateAltaLuxFilter(width, height, regions, regions));
	}

	bool ProcessSingleLayer(unsigned char* image, int width, int height, int bitDepth, int regions, int strength)
	{
		auto filter = CreateFilter(width, height, GetSafeLayerRegions(width, height, regions, MIN_HOR_REGIONS, MAX_HOR_REGIONS));
		if (!filter)
		{
			return false;
		}

		filter->SetStrength(strength);
		// Windows DIBs are BGR-ordered, so route to the BGR variants to keep
		// the BT.709 luma coefficients aligned with the actual channel bytes.
		if (bitDepth == Constants::Rgb32PixelSize)
		{
			return filter->ProcessBGR32(image) == AL_OK;
		}

		if (bitDepth == Constants::Rgb24PixelSize)
		{
			return filter->ProcessBGR24(image) == AL_OK;
		}

		return false;
	}

	// -------- SIMD helpers for multiscale accumulate/blend --------
	// accum is laid out as 3 × uint32 per pixel (interleaved BGR). Each helper processes
	// 4 pixels = 12 int32 = three __m128i registers. Uses SSE4.1 (_mm_mullo_epi32,
	// _mm_packus_epi32, _mm_extract_epi32) and SSSE3 (_mm_shuffle_epi8); the filter code
	// already assumes this baseline.

	inline void AccumulateChunk4(unsigned int* accumChunk, const unsigned char* layerChunk,
		__m128i weightVec, int bitDepth, bool firstLayer)
	{
		const __m128i zero = _mm_setzero_si128();

		__m128i bgrBytes;
		if (bitDepth == 3)
		{
			// 12 valid BGR bytes + 4 junk bytes (caller guarantees the overread is safe)
			bgrBytes = _mm_loadu_si128(reinterpret_cast<const __m128i*>(layerChunk));
		}
		else
		{
			// 4 pixels × 4 bytes BGRA; shuffle away the alpha bytes
			const __m128i bgrExtract = _mm_setr_epi8(0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14, -1, -1, -1, -1);
			const __m128i bgraBytes = _mm_loadu_si128(reinterpret_cast<const __m128i*>(layerChunk));
			bgrBytes = _mm_shuffle_epi8(bgraBytes, bgrExtract);
		}

		const __m128i lo16 = _mm_unpacklo_epi8(bgrBytes, zero);
		const __m128i hi16 = _mm_unpackhi_epi8(bgrBytes, zero);
		const __m128i v0 = _mm_unpacklo_epi16(lo16, zero);
		const __m128i v1 = _mm_unpackhi_epi16(lo16, zero);
		const __m128i v2 = _mm_unpacklo_epi16(hi16, zero);

		const __m128i w0 = _mm_mullo_epi32(v0, weightVec);
		const __m128i w1 = _mm_mullo_epi32(v1, weightVec);
		const __m128i w2 = _mm_mullo_epi32(v2, weightVec);

		if (firstLayer)
		{
			// Assign-store: skips the need to zero-initialize accum before the first layer
			_mm_storeu_si128(reinterpret_cast<__m128i*>(accumChunk + 0), w0);
			_mm_storeu_si128(reinterpret_cast<__m128i*>(accumChunk + 4), w1);
			_mm_storeu_si128(reinterpret_cast<__m128i*>(accumChunk + 8), w2);
		}
		else
		{
			const __m128i a0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(accumChunk + 0));
			const __m128i a1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(accumChunk + 4));
			const __m128i a2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(accumChunk + 8));
			_mm_storeu_si128(reinterpret_cast<__m128i*>(accumChunk + 0), _mm_add_epi32(a0, w0));
			_mm_storeu_si128(reinterpret_cast<__m128i*>(accumChunk + 4), _mm_add_epi32(a1, w1));
			_mm_storeu_si128(reinterpret_cast<__m128i*>(accumChunk + 8), _mm_add_epi32(a2, w2));
		}
	}

	inline void BlendChunk4(unsigned char* targetChunk, const unsigned char* sourceChunk,
		const unsigned int* accumChunk, __m128 strengthVec, int bitDepth)
	{
		const __m128i zero = _mm_setzero_si128();
		const __m128i roundingVec = _mm_set1_epi32(kWeightHalf);
		const __m128 halfVec = _mm_set1_ps(0.5f);

		// Unpack source BGR into 3 × __m128i of 32-bit ints (mirror of AccumulateChunk4)
		__m128i srcBgr;
		if (bitDepth == 3)
		{
			srcBgr = _mm_loadu_si128(reinterpret_cast<const __m128i*>(sourceChunk));
		}
		else
		{
			const __m128i bgrExtract = _mm_setr_epi8(0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14, -1, -1, -1, -1);
			const __m128i bgraBytes = _mm_loadu_si128(reinterpret_cast<const __m128i*>(sourceChunk));
			srcBgr = _mm_shuffle_epi8(bgraBytes, bgrExtract);
		}
		const __m128i sLo16 = _mm_unpacklo_epi8(srcBgr, zero);
		const __m128i sHi16 = _mm_unpackhi_epi8(srcBgr, zero);
		const __m128i sV0 = _mm_unpacklo_epi16(sLo16, zero);
		const __m128i sV1 = _mm_unpackhi_epi16(sLo16, zero);
		const __m128i sV2 = _mm_unpacklo_epi16(sHi16, zero);

		// enhanced = (accum + weightHalf) >> 10
		const __m128i a0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(accumChunk + 0));
		const __m128i a1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(accumChunk + 4));
		const __m128i a2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(accumChunk + 8));
		const __m128i e0 = _mm_srli_epi32(_mm_add_epi32(a0, roundingVec), kWeightScaleLog2);
		const __m128i e1 = _mm_srli_epi32(_mm_add_epi32(a1, roundingVec), kWeightScaleLog2);
		const __m128i e2 = _mm_srli_epi32(_mm_add_epi32(a2, roundingVec), kWeightScaleLog2);

		// result = src + strength01 * (enhanced - src) + 0.5, truncated → round-to-nearest
		const __m128 sF0 = _mm_cvtepi32_ps(sV0);
		const __m128 sF1 = _mm_cvtepi32_ps(sV1);
		const __m128 sF2 = _mm_cvtepi32_ps(sV2);
		const __m128 eF0 = _mm_cvtepi32_ps(e0);
		const __m128 eF1 = _mm_cvtepi32_ps(e1);
		const __m128 eF2 = _mm_cvtepi32_ps(e2);
		const __m128 r0 = _mm_add_ps(_mm_add_ps(sF0, _mm_mul_ps(strengthVec, _mm_sub_ps(eF0, sF0))), halfVec);
		const __m128 r1 = _mm_add_ps(_mm_add_ps(sF1, _mm_mul_ps(strengthVec, _mm_sub_ps(eF1, sF1))), halfVec);
		const __m128 r2 = _mm_add_ps(_mm_add_ps(sF2, _mm_mul_ps(strengthVec, _mm_sub_ps(eF2, sF2))), halfVec);
		const __m128i i0 = _mm_cvttps_epi32(r0);
		const __m128i i1 = _mm_cvttps_epi32(r1);
		const __m128i i2 = _mm_cvttps_epi32(r2);

		// Pack 32→16 (saturating) and 16→8 (saturating) — this handles clamp to [0,255]
		const __m128i p01 = _mm_packus_epi32(i0, i1);
		const __m128i p2z = _mm_packus_epi32(i2, zero);
		const __m128i packed = _mm_packus_epi16(p01, p2z);

		if (bitDepth == 3)
		{
			// 12 bytes of BGR laid out contiguously; store 8 + 4
			_mm_storel_epi64(reinterpret_cast<__m128i*>(targetChunk), packed);
			const int upperQuad = _mm_extract_epi32(packed, 2);
			memcpy(targetChunk + 8, &upperQuad, 4);
		}
		else
		{
			// Re-interleave BGR with the alpha bytes already in target (copied from source earlier)
			const __m128i bgraExpand = _mm_setr_epi8(0, 1, 2, -1, 3, 4, 5, -1, 6, 7, 8, -1, 9, 10, 11, -1);
			const __m128i bgraBody = _mm_shuffle_epi8(packed, bgraExpand);
			const __m128i alphaMask = _mm_setr_epi8(0, 0, 0, -1, 0, 0, 0, -1, 0, 0, 0, -1, 0, 0, 0, -1);
			const __m128i targetOrig = _mm_loadu_si128(reinterpret_cast<const __m128i*>(targetChunk));
			const __m128i keepAlpha = _mm_and_si128(targetOrig, alphaMask);
			_mm_storeu_si128(reinterpret_cast<__m128i*>(targetChunk), _mm_or_si128(bgraBody, keepAlpha));
		}
	}

	// Scalar fallbacks for 0–7 trailing pixels that can't safely run through SIMD.

	inline void AccumulatePixelScalar(unsigned int* accum, const unsigned char* layer,
		int pixelIndex, int bitDepth, int weight, bool firstLayer)
	{
		const int channelBase = pixelIndex * bitDepth;
		const int accumIdx = pixelIndex * 3;
		const unsigned int c0 = static_cast<unsigned int>(weight * layer[channelBase]);
		const unsigned int c1 = static_cast<unsigned int>(weight * layer[channelBase + 1]);
		const unsigned int c2 = static_cast<unsigned int>(weight * layer[channelBase + 2]);
		if (firstLayer)
		{
			accum[accumIdx]     = c0;
			accum[accumIdx + 1] = c1;
			accum[accumIdx + 2] = c2;
		}
		else
		{
			accum[accumIdx]     += c0;
			accum[accumIdx + 1] += c1;
			accum[accumIdx + 2] += c2;
		}
	}

	inline void BlendPixelScalar(unsigned char* target, const unsigned char* source,
		const unsigned int* accum, int pixelIndex, int bitDepth, float strength01)
	{
		const int channelBase = pixelIndex * bitDepth;
		const int accumIdx = pixelIndex * 3;
		const int e0 = static_cast<int>((accum[accumIdx]     + kWeightHalf) >> kWeightScaleLog2);
		const int e1 = static_cast<int>((accum[accumIdx + 1] + kWeightHalf) >> kWeightScaleLog2);
		const int e2 = static_cast<int>((accum[accumIdx + 2] + kWeightHalf) >> kWeightScaleLog2);
		const int s0 = source[channelBase];
		const int s1 = source[channelBase + 1];
		const int s2 = source[channelBase + 2];
		const int r0 = static_cast<int>(s0 + strength01 * (e0 - s0) + 0.5f);
		const int r1 = static_cast<int>(s1 + strength01 * (e1 - s1) + 0.5f);
		const int r2 = static_cast<int>(s2 + strength01 * (e2 - s2) + 0.5f);
		target[channelBase]     = static_cast<unsigned char>(ClampInt(r0, 0, 255));
		target[channelBase + 1] = static_cast<unsigned char>(ClampInt(r1, 0, 255));
		target[channelBase + 2] = static_cast<unsigned char>(ClampInt(r2, 0, 255));
		// bitDepth == 4: target[channelBase + 3] is already correct from the initial memcpy
	}

	void AccumulateRange(unsigned int* accum, const unsigned char* layer,
		int pStart, int pEnd, int bitDepth, int weight, bool firstLayer)
	{
		const __m128i weightVec = _mm_set1_epi32(weight);
		int p = pStart;
		// 24bpp loads 16 bytes per 4-pixel chunk but only 12 are valid → guarantee at least
		// 4 more pixels remain so the 4-byte overread stays inside the buffer.
		// 32bpp loads exactly 16 bytes per 4-pixel chunk → no overread.
		const int simdStep = 4;
		const int simdGuard = (bitDepth == 3) ? 8 : 4;
		while (p + simdGuard <= pEnd)
		{
			AccumulateChunk4(accum + p * 3, layer + p * bitDepth, weightVec, bitDepth, firstLayer);
			p += simdStep;
		}
		for (; p < pEnd; ++p)
		{
			AccumulatePixelScalar(accum, layer, p, bitDepth, weight, firstLayer);
		}
	}

	void BlendRange(unsigned char* target, const unsigned char* source, const unsigned int* accum,
		int pStart, int pEnd, int bitDepth, float strength01)
	{
		const __m128 strengthVec = _mm_set1_ps(strength01);
		int p = pStart;
		const int simdStep = 4;
		const int simdGuard = (bitDepth == 3) ? 8 : 4;
		while (p + simdGuard <= pEnd)
		{
			BlendChunk4(target + p * bitDepth, source + p * bitDepth, accum + p * 3, strengthVec, bitDepth);
			p += simdStep;
		}
		for (; p < pEnd; ++p)
		{
			BlendPixelScalar(target, source, accum, p, bitDepth, strength01);
		}
	}

	template<typename BlockFn>
	void RunPixelBlocks(int pixelCount, BlockFn blockFn)
	{
		if (pixelCount >= kMultiscaleParallelThreshold)
		{
			const int blockSize = (pixelCount + kMultiscaleNumBlocks - 1) / kMultiscaleNumBlocks;
			concurrency::parallel_for(0, kMultiscaleNumBlocks, [&](int blockIdx)
			{
				const int pStart = blockIdx * blockSize;
				const int pEnd = (std::min)(pStart + blockSize, pixelCount);
				if (pStart < pEnd)
				{
					blockFn(pStart, pEnd);
				}
			});
		}
		else
		{
			blockFn(0, pixelCount);
		}
	}
}

int ClampInt(int value, int minimum, int maximum)
{
	return (value < minimum) ? minimum : ((value > maximum) ? maximum : value);
}

int ComputeLayerStrength(int userStrength)
{
	const int clampedStrength = ClampInt(userStrength, 0, 100);
	if (clampedStrength <= 0)
	{
		return 0;
	}

	const float user01 = static_cast<float>(clampedStrength) / 100.0f;
	const float curved = powf(user01, 1.4f);
	const int range = Constants::MaxLayerStrength - Constants::MinLayerStrength;
	return ClampInt(
		Constants::MinLayerStrength + static_cast<int>((static_cast<float>(range) * curved) + 0.5f),
		AL_MIN_STRENGTH,
		AL_MAX_STRENGTH);
}

BlendWeights ComputeBlendWeights(const UiState& state)
{
	const float detail01 = static_cast<float>(state.detail) / 100.0f;
	const float natural01 = static_cast<float>(state.naturalLook) / 100.0f;

	float fine = Constants::BaseFine + (Constants::MaxDetailShift * detail01);
	float smooth = Constants::BaseSmooth + (Constants::MaxNaturalShift * natural01);
	float balanced = Constants::BaseBalanced
		- (Constants::MaxDetailShift * detail01)
		- (Constants::MaxNaturalShift * natural01);

	if (balanced < Constants::MinBalancedWeight)
	{
		const float deficit = Constants::MinBalancedWeight - balanced;
		const float adjustable = fine + smooth;
		if (adjustable > 0.0f)
		{
			const float reductionFactor = (std::max)(0.0f, (adjustable - deficit) / adjustable);
			fine *= reductionFactor;
			smooth *= reductionFactor;
		}
		balanced = Constants::MinBalancedWeight;
	}

	const float sum = fine + balanced + smooth;
	BlendWeights weights = { 0.0f, 0.0f, 0.0f };
	if (sum > 0.0f)
	{
		weights.fine = fine / sum;
		weights.balanced = balanced / sum;
		weights.smooth = smooth / sum;
	}

	return weights;
}

int GetSafeLayerRegions(int width, int height, int preferredRegions, int minRegions, int maxRegions)
{
	const int minImageDimension = width < height ? width : height;
	if (minImageDimension <= 0)
	{
		return 1;
	}

	int safeMinimum = minRegions;
	if (safeMinimum > minImageDimension)
	{
		safeMinimum = 1;
	}

	int safeMaximum = minImageDimension;
	if (safeMaximum > maxRegions)
	{
		safeMaximum = maxRegions;
	}
	if (safeMaximum < safeMinimum)
	{
		safeMaximum = safeMinimum;
	}

	return ClampInt(preferredRegions, safeMinimum, safeMaximum);
}

void ApplyPreset(UiState& state, const Preset& preset)
{
	state.strength = preset.strength;
	state.detail = preset.detail;
	state.naturalLook = preset.naturalLook;
}

bool IsPresetActive(const UiState& state, const Preset& preset, int tolerance)
{
	return abs(state.strength - preset.strength) <= tolerance &&
		abs(state.detail - preset.detail) <= tolerance &&
		abs(state.naturalLook - preset.naturalLook) <= tolerance;
}

RECT FitImageRect(const RECT& container, int imageWidth, int imageHeight)
{
	RECT fitted = container;
	if (imageWidth <= 0 || imageHeight <= 0)
	{
		return fitted;
	}

	const int containerWidth = container.right - container.left;
	const int containerHeight = container.bottom - container.top;
	const float scaleX = static_cast<float>(imageWidth) / static_cast<float>(containerWidth);
	const float scaleY = static_cast<float>(imageHeight) / static_cast<float>(containerHeight);
	const float maxScale = (std::max)(scaleX, scaleY);

	fitted.left = container.left;
	fitted.top = container.top;
	fitted.right = fitted.left + static_cast<int>(imageWidth / maxScale);
	fitted.bottom = fitted.top + static_cast<int>(imageHeight / maxScale);

	const int fittedWidth = fitted.right - fitted.left;
	const int fittedHeight = fitted.bottom - fitted.top;
	const int xOffset = ((containerWidth - fittedWidth) > 0) ? ((containerWidth - fittedWidth) / 2) : 0;
	const int yOffset = ((containerHeight - fittedHeight) > 0) ? ((containerHeight - fittedHeight) / 2) : 0;
	OffsetRect(&fitted, xOffset, yOffset);
	return fitted;
}

RECT GetPreviewImageRect(int imageWidth, int imageHeight, const RECT& rectPosition, bool noRescaling)
{
	if (noRescaling &&
		imageWidth > (rectPosition.right - rectPosition.left) &&
		imageHeight > (rectPosition.bottom - rectPosition.top))
	{
		return rectPosition;
	}

	return FitImageRect(rectPosition, imageWidth, imageHeight);
}

bool ProcessMultiscaleImage(const unsigned char* sourceImage, unsigned char* targetImage, int width, int height,
	int bitDepth, const UiState& state)
{
	if (sourceImage == nullptr || targetImage == nullptr)
	{
		return false;
	}

	if (width <= 0 || height <= 0)
	{
		return false;
	}

	if (bitDepth != Constants::Rgb24PixelSize && bitDepth != Constants::Rgb32PixelSize)
	{
		return false;
	}

	const int byteCount = GetImageByteCount(width, height, bitDepth);
	// memcpy requires non-overlapping regions; StartEffects2 calls us in-place
	// (src == dst) for the final write-back, so skip the self-copy.
	if (targetImage != sourceImage)
	{
		memcpy(targetImage, sourceImage, byteCount);
	}

	if (state.strength <= 0)
	{
		return true;
	}

	const BlendWeights weights = ComputeBlendWeights(state);
	const int pixelCount = width * height;
	const int fineWeight = static_cast<int>(weights.fine * static_cast<float>(kWeightScale) + 0.5f);
	const int balancedWeight = static_cast<int>(weights.balanced * static_cast<float>(kWeightScale) + 0.5f);
	const int smoothWeight = (std::max)(0, kWeightScale - fineWeight - balancedWeight);
	const int layerStrength = ComputeLayerStrength(state.strength);

	// accum holds per-channel weighted sums as uint32. Allocated via plain new[] rather
	// than std::vector — the size constructor of std::vector value-initializes (zeroes),
	// which on large images is a 100+ MB memset we don't need because the fine-layer
	// pass assigns every element (see firstLayer branch in AccumulateChunk4).
	// layerBuffer likewise gets fully overwritten by the memcpy at the top of
	// accumulateLayer, so its initial contents don't matter.
	std::unique_ptr<unsigned int[]> accum(new unsigned int[static_cast<size_t>(pixelCount) * 3U]);
	std::unique_ptr<unsigned char[]> layerBuffer(new unsigned char[static_cast<size_t>(byteCount)]);

	auto processLayerToBuffer = [&](unsigned char* buffer, int regions) -> bool
	{
		memcpy(buffer, sourceImage, byteCount);
		return ProcessSingleLayer(buffer, width, height, bitDepth, regions, layerStrength);
	};

	auto accumulateProcessedLayer = [&](const unsigned char* buffer, int weight, bool firstLayer)
	{
		RunPixelBlocks(pixelCount, [&](int pStart, int pEnd)
		{
			AccumulateRange(accum.get(), buffer, pStart, pEnd, bitDepth, weight, firstLayer);
		});
	};

	auto accumulateLayer = [&](int regions, int weight, bool firstLayer) -> bool
	{
		if (!processLayerToBuffer(layerBuffer.get(), regions))
		{
			return false;
		}
		accumulateProcessedLayer(layerBuffer.get(), weight, firstLayer);
		return true;
	};

	bool layersProcessed = false;
	if (pixelCount >= kParallelLayerThreshold)
	{
		std::unique_ptr<unsigned char[]> fineLayer(new unsigned char[static_cast<size_t>(byteCount)]);
		std::unique_ptr<unsigned char[]> balancedLayer(new unsigned char[static_cast<size_t>(byteCount)]);
		std::unique_ptr<unsigned char[]> smoothLayer(new unsigned char[static_cast<size_t>(byteCount)]);

		bool fineOk = false;
		bool balancedOk = false;
		bool smoothOk = false;
		concurrency::parallel_invoke(
			[&] { fineOk = processLayerToBuffer(fineLayer.get(), Constants::FineRegions); },
			[&] { balancedOk = processLayerToBuffer(balancedLayer.get(), Constants::BalancedRegions); },
			[&] { smoothOk = processLayerToBuffer(smoothLayer.get(), Constants::SmoothRegions); });

		layersProcessed = fineOk && balancedOk && smoothOk;
		if (layersProcessed)
		{
			accumulateProcessedLayer(fineLayer.get(), fineWeight, true);
			accumulateProcessedLayer(balancedLayer.get(), balancedWeight, false);
			accumulateProcessedLayer(smoothLayer.get(), smoothWeight, false);
		}
	}
	else
	{
		layersProcessed =
			accumulateLayer(Constants::FineRegions, fineWeight, true) &&
			accumulateLayer(Constants::BalancedRegions, balancedWeight, false) &&
			accumulateLayer(Constants::SmoothRegions, smoothWeight, false);
	}

	if (!layersProcessed)
	{
		if (targetImage != sourceImage)
		{
			memcpy(targetImage, sourceImage, byteCount);
		}
		return false;
	}

	const float strength01 = static_cast<float>(state.strength) / 100.0f;
	RunPixelBlocks(pixelCount, [&](int pStart, int pEnd)
	{
		BlendRange(targetImage, sourceImage, accum.get(), pStart, pEnd, bitDepth, strength01);
	});

	return true;
}
