#include "AltaLuxCore.h"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <memory>
#include <vector>

#include <ppl.h>

#include "ChromaCorrection.h"
#include "Filter/CAltaLuxFilterFactory.h"
#include "Filter/CBaseAltaLuxFilter.h"

namespace
{
	constexpr int kMultiscaleParallelThreshold = 200000;  // ~450x450; skip parallel_for below
	constexpr int kParallelLayerThreshold = 1000000;       // ~1 MP; below this, three concurrent CLAHE tasks rarely pay off
	constexpr int kMultiscaleNumBlocks = 16;              // outer partition count for PPL
	constexpr int kWeightScale = 1024;                    // weights sum to 1024 (2^10)

	size_t GetImageByteCount(int width, int height, int bitDepth)
	{
		return static_cast<size_t>(width) * static_cast<size_t>(height) * static_cast<size_t>(bitDepth);
	}

	int RectWidthOf(const RECT& rect)
	{
		return rect.right - rect.left;
	}

	int RectHeightOf(const RECT& rect)
	{
		return rect.bottom - rect.top;
	}

	std::unique_ptr<CBaseAltaLuxFilter> CreateFilter(int width, int height, int regions)
	{
		return std::unique_ptr<CBaseAltaLuxFilter>(CAltaLuxFilterFactory::CreateAltaLuxFilter(width, height, regions, regions));
	}

	// Q16 reciprocal-of-luma table for the fused multiscale blend, matching the
	// filter layer's table: index 0 is a zero sentinel so Y=0 pixels stay black.
	// Dynamic initialization of a namespace-scope const runs exactly once at DLL
	// load, before any processing call can execute.
	struct ReciprocalLutTable
	{
		int table[256];
	};

	const ReciprocalLutTable g_RecipLut = []() {
		ReciprocalLutTable lut = {};
		for (int value = 1; value < 256; ++value)
		{
			lut.table[value] = (1 << 16) / value;
		}
		return lut;
	}();

	// Runs one multiscale layer's CLAHE in place on a packed 1 B/px luma plane.
	// ProcessGray points the filter's ImageBuffer straight at the caller's
	// memory, so the layer allocates nothing beyond the filter instance.
	bool ProcessSingleLayer(unsigned char* lumaPlane, int width, int height, int regions, int strength,
		AltaLuxKernels::KernelImplementation implementation)
	{
		auto filter = CreateFilter(width, height, GetSafeLayerRegions(width, height, regions, MIN_HOR_REGIONS, MAX_HOR_REGIONS));
		if (!filter)
		{
			return false;
		}

		filter->SetStrength(strength);
		filter->SetKernelImplementation(implementation);
		return filter->ProcessGray(lumaPlane) == AL_OK;
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

RECT GetZoomedImageRect(int imageWidth, int imageHeight, const RECT& container, double zoomFactor, int panX, int panY)
{
	const RECT fitRect = FitImageRect(container, imageWidth, imageHeight);
	if (zoomFactor <= 1.0)
	{
		return fitRect;
	}

	const int fittedWidth = RectWidthOf(fitRect);
	const int fittedHeight = RectHeightOf(fitRect);
	const double zoom = (std::max)(1.0, zoomFactor);
	const int zoomedWidth = static_cast<int>(fittedWidth * zoom);
	const int zoomedHeight = static_cast<int>(fittedHeight * zoom);
	const int centerX = (container.left + container.right) / 2 + panX;
	const int centerY = (container.top + container.bottom) / 2 + panY;

	RECT zoomed = {};
	zoomed.left = centerX - zoomedWidth / 2;
	zoomed.top = centerY - zoomedHeight / 2;
	zoomed.right = zoomed.left + zoomedWidth;
	zoomed.bottom = zoomed.top + zoomedHeight;
	return zoomed;
}

void ClampPanOffsets(int imageWidth, int imageHeight, const RECT& container, double zoomFactor, int& panX, int& panY)
{
	const RECT zoomedRect = GetZoomedImageRect(imageWidth, imageHeight, container, zoomFactor, 0, 0);
	const int overflowX = RectWidthOf(zoomedRect) - RectWidthOf(container);
	const int overflowY = RectHeightOf(zoomedRect) - RectHeightOf(container);
	panX = (overflowX > 0) ? ClampInt(panX, -(overflowX / 2), overflowX / 2) : 0;
	panY = (overflowY > 0) ? ClampInt(panY, -(overflowY / 2), overflowY / 2) : 0;
}

double GetActualPixelScale(int imageWidth, int imageHeight, const RECT& container, double zoomFactor)
{
	if (imageWidth <= 0 || imageHeight <= 0)
	{
		return 1.0;
	}
	const RECT zoomedRect = GetZoomedImageRect(imageWidth, imageHeight, container, zoomFactor, 0, 0);
	return static_cast<double>(RectWidthOf(zoomedRect)) / imageWidth;
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

	// The kernels and the CLAHE filters index pixels with int; wider images
	// (stitched panoramas in a 64-bit host) must fail cleanly here instead of
	// overflowing the index arithmetic and corrupting memory.
	const unsigned long long pixelCount64 =
		static_cast<unsigned long long>(width) * static_cast<unsigned long long>(height);
	if (pixelCount64 > static_cast<unsigned long long>(INT_MAX))
	{
		return false;
	}

	const size_t byteCount = GetImageByteCount(width, height, bitDepth);
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

	// The layers blend in luma, so the whole layer phase works on packed
	// 1 B/px planes: the untouched original plus one CLAHE plane per layer,
	// each with the one-row padding the filter's own buffers use
	// (IMAGE_BUFFER_SIZE in CBaseAltaLuxFilter.h). At 4 B/px total for the
	// planes, execution no longer changes the memory profile: the threshold
	// below only decides whether the three filter runs are worth concurrent
	// tasks, not how much is allocated.
	const size_t planeBytes = static_cast<size_t>(width) * static_cast<size_t>(height + 1);
	std::unique_ptr<unsigned char[]> originalLuma(new unsigned char[planeBytes]);
	std::unique_ptr<unsigned char[]> fineLayer(new unsigned char[planeBytes]);
	std::unique_ptr<unsigned char[]> balancedLayer(new unsigned char[planeBytes]);
	std::unique_ptr<unsigned char[]> smoothLayer(new unsigned char[planeBytes]);

	ExtractBgrLuma(sourceImage, originalLuma.get(), pixelCount, bitDepth, implementation);
	memcpy(fineLayer.get(), originalLuma.get(), pixelCount);
	memcpy(balancedLayer.get(), originalLuma.get(), pixelCount);
	memcpy(smoothLayer.get(), originalLuma.get(), pixelCount);

	bool layersProcessed;
	if (pixelCount >= kParallelLayerThreshold)
	{
		bool fineOk = false;
		bool balancedOk = false;
		bool smoothOk = false;
		concurrency::parallel_invoke(
			[&] { fineOk = ProcessSingleLayer(fineLayer.get(), width, height, Constants::FineRegions, layerStrength, implementation); },
			[&] { balancedOk = ProcessSingleLayer(balancedLayer.get(), width, height, Constants::BalancedRegions, layerStrength, implementation); },
			[&] { smoothOk = ProcessSingleLayer(smoothLayer.get(), width, height, Constants::SmoothRegions, layerStrength, implementation); });
		layersProcessed = fineOk && balancedOk && smoothOk;
	}
	else
	{
		layersProcessed = ProcessSingleLayer(fineLayer.get(), width, height, Constants::FineRegions, layerStrength, implementation) &&
			ProcessSingleLayer(balancedLayer.get(), width, height, Constants::BalancedRegions, layerStrength, implementation) &&
			ProcessSingleLayer(smoothLayer.get(), width, height, Constants::SmoothRegions, layerStrength, implementation);
	}

	if (!layersProcessed)
	{
		if (targetImage != sourceImage)
		{
			memcpy(targetImage, sourceImage, byteCount);
		}
		return false;
	}

	// One fused pass per pixel: blend the three CLAHE planes in luma, then
	// scale each BGR channel by the capped blended-luma ratio. This replaces
	// the three per-layer BGR round trips, the 12 B/px accumulator and the
	// write-back of the old pipeline; being in-place, it reads the pixel
	// before writing it, so overlapping blocks stay safe.
	RunPixelBlocks(pixelCount, [&](int pStart, int pEnd)
	{
		AltaLuxKernels::BlendLumaAndInjectRGB(targetImage, originalLuma.get(), fineLayer.get(),
			balancedLayer.get(), smoothLayer.get(), fineWeight, balancedWeight, smoothWeight,
			pStart, pEnd, bitDepth, g_RecipLut.table, implementation);
	});

	if (state.chromaProtection > 0)
	{
		// The chroma stage needs the original luma (already cached above) and
		// three scratch planes; the layer planes are dead after the fused
		// pass, so the stage reuses them and adds nothing to peak memory.
		// Y' is extracted from the finished pixels: the attenuation is
		// luma-neutral only against the pixel's actual luma, not the blend.
		if (!ApplyChromaCorrectionWithScratch(targetImage, originalLuma.get(), width, height, bitDepth,
			state.chromaProtection, fineLayer.get(), balancedLayer.get(), smoothLayer.get(),
			implementation))
		{
			return false;
		}
	}

	return true;
}

bool ProcessMultiscaleImage(const unsigned char* sourceImage, unsigned char* targetImage, int width, int height,
	int bitDepth, const UiState& state)
{
	return ProcessMultiscaleImageWithKernels(sourceImage, targetImage, width, height, bitDepth, state,
		AltaLuxKernels::GetBestSupportedImplementation());
}
