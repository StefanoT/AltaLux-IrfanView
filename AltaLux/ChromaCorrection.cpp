#include "ChromaCorrection.h"

#include <algorithm>
#include <cmath>
#include <memory>

#include <ppl.h>

#include "AltaLuxCore.h"

namespace
{
	// BT.709 luma factors in Q15, in BGR byte order. These mirror the CLAHE
	// filter's extraction (CBaseAltaLuxFilter.cpp); a shared home is not worth
	// the churn because the risk map is insensitive to a ±1 quantization here.
	const int kLumaScalingLog = 15;
	const int kLumaBlueScale = static_cast<int>(0.0722 * (1 << kLumaScalingLog));
	const int kLumaGreenScale = static_cast<int>(0.7152 * (1 << kLumaScalingLog));
	const int kLumaRedScale = static_cast<int>(0.2126 * (1 << kLumaScalingLog));

	// Luma-gain risk: zero below ~+0.5 stop of lifting, saturated above ~+2
	// stops, expressed in photographic stops so the thresholds keep their
	// meaning if the enhancement curve changes.
	const float kGainLowStops = 0.5f;
	const float kGainHighStops = 2.0f;

	// Original-darkness risk: full at luma 8 and below, gone at luma 64 and
	// above; midtones carry reliable chroma even when lifted hard.
	const int kDarkLumaLow = 8;
	const int kDarkLumaHigh = 64;

	// Local-activity risk: flat neighborhoods (mean absolute 3x3 deviation at
	// or below 1) carry the full risk, textured ones (deviation 6 and above)
	// keep most of their chroma because edges and detail read as structure,
	// not noise.
	const int kActivityLow = 1;
	const int kActivityHigh = 6;

	// Fraction of the risk that survives on textured areas, in Q8 (77 ~ 0.3).
	const int kTextureFloorQ8 = 77;

	const int kParallelThreshold = 200000;  // mirrors RunPixelBlocks in AltaLuxCore.cpp
	const int kParallelBlocks = 16;

	float SmoothStep(float edge0, float edge1, float x)
	{
		const float t = (x - edge0) / (edge1 - edge0);
		const float clamped = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
		return clamped * clamped * (3.0f - 2.0f * clamped);
	}

	// Gain x darkness risk for every (original, enhanced) luma pair, Q8. Built
	// once at load like the reciprocal table; the log2 stops ratio makes the
	// table the natural place to pay for it.
	struct GainRiskLutTable
	{
		unsigned int table[256 * 256];
	};

	const GainRiskLutTable g_GainRiskLut = []() {
		GainRiskLutTable lut = {};
		for (int y = 0; y < 256; ++y)
		{
			const float darkRisk = 1.0f - SmoothStep(static_cast<float>(kDarkLumaLow),
				static_cast<float>(kDarkLumaHigh), static_cast<float>(y));
			for (int yp = 0; yp < 256; ++yp)
			{
				const float gainStops = std::log2(
					(static_cast<float>(yp) + 1.0f) / (static_cast<float>(y) + 1.0f));
				const float gainRisk = SmoothStep(kGainLowStops, kGainHighStops, gainStops);
				lut.table[(y << 8) | yp] = static_cast<unsigned int>(gainRisk * darkRisk * 255.0f + 0.5f);
			}
		}
		return lut;
	}();

	struct ActivityRiskLutTable
	{
		unsigned int table[256];
	};

	const ActivityRiskLutTable g_ActivityRiskLut = []() {
		ActivityRiskLutTable lut = {};
		for (int t = 0; t < 256; ++t)
		{
			const float risk = 1.0f - SmoothStep(static_cast<float>(kActivityLow),
				static_cast<float>(kActivityHigh), static_cast<float>(t));
			lut.table[t] = static_cast<unsigned int>(risk * 255.0f + 0.5f);
		}
		return lut;
	}();

	template<typename BlockFn>
	void RunBlocks(int pixelCount, BlockFn blockFn)
	{
		if (pixelCount >= kParallelThreshold)
		{
			const int blockSize = (pixelCount + kParallelBlocks - 1) / kParallelBlocks;
			concurrency::parallel_for(0, kParallelBlocks, [&](int blockIdx)
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

void ExtractBgrLuma(const unsigned char* image, unsigned char* luma, int pixelCount,
	int bitDepth, AltaLuxKernels::KernelImplementation implementation)
{
	AltaLuxKernels::ExtractRGBLuma(image, luma, pixelCount, bitDepth,
		kLumaBlueScale, kLumaGreenScale, kLumaRedScale, kLumaScalingLog, implementation);
}

int ComputeGainRiskQ8(int originalLuma, int enhancedLuma)
{
	const int y = ClampInt(originalLuma, 0, 255);
	const int yp = ClampInt(enhancedLuma, 0, 255);
	return static_cast<int>(g_GainRiskLut.table[(y << 8) | yp]);
}

int ComputeActivityRiskQ8(int activity)
{
	const int t = ClampInt(activity, 0, 255);
	return static_cast<int>(g_ActivityRiskLut.table[t]);
}

bool ApplyChromaCorrection(unsigned char* targetImage, const unsigned char* originalLuma,
	int width, int height, int bitDepth, int chromaProtection,
	AltaLuxKernels::KernelImplementation implementation)
{
	if (targetImage == nullptr || originalLuma == nullptr)
	{
		return false;
	}

	if (width <= 0 || height <= 0)
	{
		return false;
	}

	if (bitDepth != Constants::RGB24PixelSize && bitDepth != Constants::RGB32PixelSize)
	{
		return false;
	}

	const int clampedProtection = ClampInt(chromaProtection, 0, 100);
	const int maxStrengthQ8 = (Constants::MaxChromaAttenuationQ8 * clampedProtection + 50) / 100;
	if (maxStrengthQ8 <= 0)
	{
		return true;
	}

	const int pixelCount = width * height;
	// The activity plane doubles as the blur scratch once the risk map exists.
	std::unique_ptr<unsigned char[]> enhancedLuma(new unsigned char[static_cast<size_t>(pixelCount)]);
	std::unique_ptr<unsigned char[]> activity(new unsigned char[static_cast<size_t>(pixelCount)]);
	std::unique_ptr<unsigned char[]> risk(new unsigned char[static_cast<size_t>(pixelCount)]);

	ExtractBgrLuma(targetImage, enhancedLuma.get(), pixelCount, bitDepth, implementation);
	AltaLuxKernels::ComputeLocalActivity3x3(originalLuma, activity.get(), width, height,
		implementation);
	AltaLuxKernels::ComputeChromaRisk(originalLuma, enhancedLuma.get(), activity.get(), risk.get(),
		pixelCount, g_GainRiskLut.table, g_ActivityRiskLut.table, kTextureFloorQ8);
	AltaLuxKernels::BlurRiskMap(risk.get(), activity.get(), width, height, implementation);

	RunBlocks(pixelCount, [&](int pStart, int pEnd)
	{
		AltaLuxKernels::ApplyChromaAttenuation(targetImage, enhancedLuma.get(), risk.get(),
			pStart, pEnd, bitDepth, maxStrengthQ8, implementation);
	});

	return true;
}
