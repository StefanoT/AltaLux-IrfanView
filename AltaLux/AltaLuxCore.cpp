#include "AltaLuxCore.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <memory>
#include <vector>

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

	bool ProcessSingleLayer(unsigned char* image, int width, int height, int bitDepth, int regions, int strength,
		AltaLuxKernels::KernelImplementation implementation)
	{
		auto filter = CreateFilter(width, height, GetSafeLayerRegions(width, height, regions, MIN_HOR_REGIONS, MAX_HOR_REGIONS));
		if (!filter)
		{
			return false;
		}

		filter->SetStrength(strength);
		filter->SetKernelImplementation(implementation);
		// Windows DIBs are BGR-ordered, so route to the BGR variants to keep
		// the BT.709 luma coefficients aligned with the actual channel bytes.
		if (bitDepth == Constants::RGB32PixelSize)
		{
			return filter->ProcessBGR32(image) == AL_OK;
		}

		if (bitDepth == Constants::RGB24PixelSize)
		{
			return filter->ProcessBGR24(image) == AL_OK;
		}

		return false;
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

bool ProcessMultiscaleImageWithKernels(const unsigned char* sourceImage, unsigned char* targetImage,
	int width, int height, int bitDepth, const UiState& state,
	AltaLuxKernels::KernelImplementation implementation)
{
	if (sourceImage == nullptr || targetImage == nullptr)
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
		// pass assigns every element through the firstLayer branch in the kernel layer.
	// layerBuffer likewise gets fully overwritten by the memcpy at the top of
	// accumulateLayer, so its initial contents don't matter.
	std::unique_ptr<unsigned int[]> accum(new unsigned int[static_cast<size_t>(pixelCount) * 3U]);
	std::unique_ptr<unsigned char[]> layerBuffer(new unsigned char[static_cast<size_t>(byteCount)]);

		auto processLayerToBuffer = [&](unsigned char* buffer, int regions) -> bool
		{
			memcpy(buffer, sourceImage, byteCount);
			return ProcessSingleLayer(buffer, width, height, bitDepth, regions, layerStrength, implementation);
		};

	auto accumulateProcessedLayer = [&](const unsigned char* buffer, int weight, bool firstLayer)
	{
		RunPixelBlocks(pixelCount, [&](int pStart, int pEnd)
		{
			AltaLuxKernels::AccumulateLayer(accum.get(), buffer, pStart, pEnd, bitDepth,
				weight, firstLayer, implementation);
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

	RunPixelBlocks(pixelCount, [&](int pStart, int pEnd)
	{
		AltaLuxKernels::WriteAccumulatedImage(targetImage, accum.get(), pStart, pEnd, bitDepth,
			kWeightScaleLog2, kWeightHalf, implementation);
	});

	return true;
}

bool ProcessMultiscaleImage(const unsigned char* sourceImage, unsigned char* targetImage, int width, int height,
	int bitDepth, const UiState& state)
{
	return ProcessMultiscaleImageWithKernels(sourceImage, targetImage, width, height, bitDepth, state,
		AltaLuxKernels::GetBestSupportedImplementation());
}
