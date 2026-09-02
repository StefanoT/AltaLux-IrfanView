#include "AltaLuxKernelsInternal.h"

#include <intrin.h>

namespace
{
	struct CpuFeatures
	{
		bool ssse3;
		bool avx2;
	};

	CpuFeatures DetectCpuFeatures()
	{
		CpuFeatures features = { false, false };

		int cpuInfo[4] = {};
		__cpuid(cpuInfo, 0);
		const int maxFunction = cpuInfo[0];

		if (maxFunction >= 1)
		{
			__cpuidex(cpuInfo, 1, 0);
			features.ssse3 = (cpuInfo[2] & (1 << 9)) != 0;

			const bool osXsave = (cpuInfo[2] & (1 << 27)) != 0;
			const bool avx = (cpuInfo[2] & (1 << 28)) != 0;
			bool osAvxState = false;
			if (osXsave && avx)
			{
				const unsigned __int64 xcr0 = _xgetbv(0);
				osAvxState = (xcr0 & 0x6) == 0x6;
			}

			if (osAvxState && maxFunction >= 7)
			{
				__cpuidex(cpuInfo, 7, 0);
				features.avx2 = (cpuInfo[1] & (1 << 5)) != 0;
			}
		}

		return features;
	}

	const CpuFeatures& GetCpuFeatures()
	{
		static const CpuFeatures features = DetectCpuFeatures();
		return features;
	}

	AltaLuxKernels::KernelImplementation NormalizeImplementation(
		AltaLuxKernels::KernelImplementation implementation)
	{
		if (AltaLuxKernels::IsImplementationSupported(implementation))
		{
			return implementation;
		}
		return AltaLuxKernels::KernelImplementation::Scalar;
	}
}

namespace AltaLuxKernels
{
	const char* GetImplementationName(KernelImplementation implementation)
	{
		switch (implementation)
		{
		case KernelImplementation::Scalar:
			return "Scalar";
		case KernelImplementation::SSSE3:
			return "SSSE3";
		case KernelImplementation::AVX2:
			return "AVX2";
		default:
			return "Unknown";
		}
	}

	bool IsImplementationSupported(KernelImplementation implementation)
	{
		const CpuFeatures& features = GetCpuFeatures();
		switch (implementation)
		{
		case KernelImplementation::Scalar:
			return true;
		case KernelImplementation::SSSE3:
			return features.ssse3;
		case KernelImplementation::AVX2:
			return features.avx2;
		default:
			return false;
		}
	}

	KernelImplementation GetBestSupportedImplementation()
	{
		if (IsImplementationSupported(KernelImplementation::AVX2))
		{
			return KernelImplementation::AVX2;
		}
		if (IsImplementationSupported(KernelImplementation::SSSE3))
		{
			return KernelImplementation::SSSE3;
		}
		return KernelImplementation::Scalar;
	}

	void ExtractPackedYUVLuma(const unsigned char* source, unsigned char* luma,
		int pixelCount, PackedYUVLumaPosition lumaPosition, KernelImplementation implementation)
	{
		switch (NormalizeImplementation(implementation))
		{
		case KernelImplementation::AVX2:
			ExtractPackedYUVLumaAVX2(source, luma, pixelCount, lumaPosition);
			break;
		case KernelImplementation::SSSE3:
			ExtractPackedYUVLumaSSSE3(source, luma, pixelCount, lumaPosition);
			break;
		default:
			ExtractPackedYUVLumaScalar(source, luma, pixelCount, lumaPosition);
			break;
		}
	}

	void InjectPackedYUVLuma(unsigned char* target, const unsigned char* luma,
		int pixelCount, PackedYUVLumaPosition lumaPosition, KernelImplementation implementation)
	{
		switch (NormalizeImplementation(implementation))
		{
		case KernelImplementation::AVX2:
			InjectPackedYUVLumaAVX2(target, luma, pixelCount, lumaPosition);
			break;
		case KernelImplementation::SSSE3:
			InjectPackedYUVLumaSSSE3(target, luma, pixelCount, lumaPosition);
			break;
		default:
			InjectPackedYUVLumaScalar(target, luma, pixelCount, lumaPosition);
			break;
		}
	}

	void ExtractRGBLuma(const unsigned char* source, unsigned char* luma, int pixelCount,
		int pixelStride, int firstFactor, int secondFactor, int thirdFactor, int scalingLog,
		KernelImplementation implementation)
	{
		switch (NormalizeImplementation(implementation))
		{
		case KernelImplementation::AVX2:
			ExtractRGBLumaAVX2(source, luma, pixelCount, pixelStride,
				firstFactor, secondFactor, thirdFactor, scalingLog);
			break;
		case KernelImplementation::SSSE3:
			ExtractRGBLumaSSSE3(source, luma, pixelCount, pixelStride,
				firstFactor, secondFactor, thirdFactor, scalingLog);
			break;
		default:
			ExtractRGBLumaScalar(source, luma, pixelCount, pixelStride,
				firstFactor, secondFactor, thirdFactor, scalingLog);
			break;
		}
	}

	void InjectRGBLuma(unsigned char* image, const unsigned char* luma, int pixelCount,
		int pixelStride, int firstFactor, int secondFactor, int thirdFactor, int scalingLog,
		const int* reciprocalLut, KernelImplementation implementation)
	{
		switch (NormalizeImplementation(implementation))
		{
		case KernelImplementation::AVX2:
			InjectRGBLumaAVX2(image, luma, pixelCount, pixelStride,
				firstFactor, secondFactor, thirdFactor, scalingLog, reciprocalLut);
			break;
		case KernelImplementation::SSSE3:
			InjectRGBLumaSSSE3(image, luma, pixelCount, pixelStride,
				firstFactor, secondFactor, thirdFactor, scalingLog, reciprocalLut);
			break;
		default:
			InjectRGBLumaScalar(image, luma, pixelCount, pixelStride,
				firstFactor, secondFactor, thirdFactor, scalingLog, reciprocalLut);
			break;
		}
	}

	void InjectRGBLumaWithOriginalLuma(unsigned char* image, const unsigned char* luma,
		const unsigned char* originalLuma, int pixelCount, int pixelStride,
		const int* reciprocalLut, KernelImplementation implementation)
	{
		switch (NormalizeImplementation(implementation))
		{
		case KernelImplementation::AVX2:
			InjectRGBLumaWithOriginalLumaAVX2(image, luma, originalLuma, pixelCount,
				pixelStride, reciprocalLut);
			break;
		case KernelImplementation::SSSE3:
			InjectRGBLumaWithOriginalLumaSSSE3(image, luma, originalLuma, pixelCount,
				pixelStride, reciprocalLut);
			break;
		default:
			InjectRGBLumaWithOriginalLumaScalar(image, luma, originalLuma, pixelCount,
				pixelStride, reciprocalLut);
			break;
		}
	}

	void ScaleDownBox(const unsigned char* source, int sourceWidth, int sourceHeight,
		unsigned char* target, int scaleFactor, int pixelStride, KernelImplementation implementation)
	{
		switch (NormalizeImplementation(implementation))
		{
		case KernelImplementation::AVX2:
			ScaleDownBoxAVX2(source, sourceWidth, sourceHeight, target, scaleFactor, pixelStride);
			break;
		case KernelImplementation::SSSE3:
			ScaleDownBoxSSSE3(source, sourceWidth, sourceHeight, target, scaleFactor, pixelStride);
			break;
		default:
			ScaleDownBoxScalar(source, sourceWidth, sourceHeight, target, scaleFactor, pixelStride);
			break;
		}
	}

	// Scalar-only operation: conflicting histogram increments (multiple lanes
	// targeting the same bin) cannot be vectorized without scatter/conflict
	// detection, which neither SSSE3 nor AVX2 provides.
	void MakeHistogram(const unsigned char* image, int imageStride, int regionWidth,
		int regionHeight, unsigned int* histogram)
	{
		MakeHistogramScalar(image, imageStride, regionWidth, regionHeight, histogram);
	}

	// Scalar-only operation: the redistribution loop is stateful, so the whole
	// clip-and-redistribute pass stays scalar.
	void ClipHistogram(unsigned int* histogram, unsigned int clipLimit)
	{
		ClipHistogramScalar(histogram, clipLimit);
	}

	// Scalar-only operation: the equalization map is a 256-bin prefix sum with
	// a loop-carried dependency.
	void MapHistogram(unsigned int* histogram, unsigned int pixelCount)
	{
		MapHistogramScalar(histogram, pixelCount);
	}

	void AccumulateLayer(unsigned int* accum, const unsigned char* layer, int pixelStart,
		int pixelEnd, int pixelStride, int weight, bool firstLayer, KernelImplementation implementation)
	{
		switch (NormalizeImplementation(implementation))
		{
		case KernelImplementation::AVX2:
			AccumulateLayerAVX2(accum, layer, pixelStart, pixelEnd, pixelStride, weight, firstLayer);
			break;
		case KernelImplementation::SSSE3:
			AccumulateLayerSSSE3(accum, layer, pixelStart, pixelEnd, pixelStride, weight, firstLayer);
			break;
		default:
			AccumulateLayerScalar(accum, layer, pixelStart, pixelEnd, pixelStride, weight, firstLayer);
			break;
		}
	}

	void WriteAccumulatedImage(unsigned char* target, const unsigned int* accum, int pixelStart,
		int pixelEnd, int pixelStride, int weightScaleLog2, int weightHalf,
		KernelImplementation implementation)
	{
		switch (NormalizeImplementation(implementation))
		{
		case KernelImplementation::AVX2:
			WriteAccumulatedImageAVX2(target, accum, pixelStart, pixelEnd, pixelStride,
				weightScaleLog2, weightHalf);
			break;
		case KernelImplementation::SSSE3:
			WriteAccumulatedImageSSSE3(target, accum, pixelStart, pixelEnd, pixelStride,
				weightScaleLog2, weightHalf);
			break;
		default:
			WriteAccumulatedImageScalar(target, accum, pixelStart, pixelEnd, pixelStride,
				weightScaleLog2, weightHalf);
			break;
		}
	}

	// Scalar-only operation: row-map construction was benchmarked against both
	// SIMD tiers and lost, because the grey-value-indexed pixel lookup stays
	// scalar and neither tier has an integer gather fast enough to compensate.
	// Revisit only with a gather-based design that beats the scalar loop.
	void Interpolate(unsigned char* image, int imageStride,
		const unsigned int* mapLeftUp, const unsigned int* mapRightUp,
		const unsigned int* mapLeftBottom, const unsigned int* mapRightBottom,
		unsigned int matrixWidth, unsigned int matrixHeight)
	{
		InterpolateScalar(image, imageStride, mapLeftUp, mapRightUp,
			mapLeftBottom, mapRightBottom, matrixWidth, matrixHeight);
	}
}
