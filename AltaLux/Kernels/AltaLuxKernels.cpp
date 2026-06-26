#include "AltaLuxKernelsInternal.h"

#include <intrin.h>

namespace
{
	struct CpuFeatures
	{
		bool sse2;
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
			features.sse2 = (cpuInfo[3] & (1 << 26)) != 0;

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

#if defined(_M_X64)
		features.sse2 = true;
#endif
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
		case KernelImplementation::SSE2:
			return "SSE2";
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
		case KernelImplementation::SSE2:
			return features.sse2;
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
		if (IsImplementationSupported(KernelImplementation::SSE2))
		{
			return KernelImplementation::SSE2;
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
		case KernelImplementation::SSE2:
			ExtractPackedYUVLumaSSE2(source, luma, pixelCount, lumaPosition);
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
		case KernelImplementation::SSE2:
			InjectPackedYUVLumaSSE2(target, luma, pixelCount, lumaPosition);
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
		case KernelImplementation::SSE2:
			ExtractRGBLumaSSE2(source, luma, pixelCount, pixelStride,
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
		case KernelImplementation::SSE2:
			InjectRGBLumaSSE2(image, luma, pixelCount, pixelStride,
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
		case KernelImplementation::SSE2:
			InjectRGBLumaWithOriginalLumaSSE2(image, luma, originalLuma, pixelCount,
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
		case KernelImplementation::SSE2:
			ScaleDownBoxSSE2(source, sourceWidth, sourceHeight, target, scaleFactor, pixelStride);
			break;
		default:
			ScaleDownBoxScalar(source, sourceWidth, sourceHeight, target, scaleFactor, pixelStride);
			break;
		}
	}

	void MakeHistogram(const unsigned char* image, int imageStride, int regionWidth,
		int regionHeight, unsigned int* histogram, KernelImplementation implementation)
	{
		switch (NormalizeImplementation(implementation))
		{
		case KernelImplementation::AVX2:
			MakeHistogramAVX2(image, imageStride, regionWidth, regionHeight, histogram);
			break;
		case KernelImplementation::SSE2:
			MakeHistogramSSE2(image, imageStride, regionWidth, regionHeight, histogram);
			break;
		default:
			MakeHistogramScalar(image, imageStride, regionWidth, regionHeight, histogram);
			break;
		}
	}

	void ClipHistogram(unsigned int* histogram, unsigned int clipLimit, KernelImplementation implementation)
	{
		switch (NormalizeImplementation(implementation))
		{
		case KernelImplementation::AVX2:
			ClipHistogramAVX2(histogram, clipLimit);
			break;
		case KernelImplementation::SSE2:
			ClipHistogramSSE2(histogram, clipLimit);
			break;
		default:
			ClipHistogramScalar(histogram, clipLimit);
			break;
		}
	}

	void MapHistogram(unsigned int* histogram, unsigned int pixelCount, KernelImplementation implementation)
	{
		switch (NormalizeImplementation(implementation))
		{
		case KernelImplementation::AVX2:
			MapHistogramAVX2(histogram, pixelCount);
			break;
		case KernelImplementation::SSE2:
			MapHistogramSSE2(histogram, pixelCount);
			break;
		default:
			MapHistogramScalar(histogram, pixelCount);
			break;
		}
	}

	void AccumulateLayer(unsigned int* accum, const unsigned char* layer, int pixelStart,
		int pixelEnd, int pixelStride, int weight, bool firstLayer, KernelImplementation implementation)
	{
		switch (NormalizeImplementation(implementation))
		{
		case KernelImplementation::AVX2:
			AccumulateLayerAVX2(accum, layer, pixelStart, pixelEnd, pixelStride, weight, firstLayer);
			break;
		case KernelImplementation::SSE2:
			AccumulateLayerSSE2(accum, layer, pixelStart, pixelEnd, pixelStride, weight, firstLayer);
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
		case KernelImplementation::SSE2:
			WriteAccumulatedImageSSE2(target, accum, pixelStart, pixelEnd, pixelStride,
				weightScaleLog2, weightHalf);
			break;
		default:
			WriteAccumulatedImageScalar(target, accum, pixelStart, pixelEnd, pixelStride,
				weightScaleLog2, weightHalf);
			break;
		}
	}

	void Interpolate(unsigned char* image, int imageStride,
		const unsigned int* mapLeftUp, const unsigned int* mapRightUp,
		const unsigned int* mapLeftBottom, const unsigned int* mapRightBottom,
		unsigned int matrixWidth, unsigned int matrixHeight, KernelImplementation implementation)
	{
		switch (NormalizeImplementation(implementation))
		{
		case KernelImplementation::AVX2:
			InterpolateAVX2(image, imageStride, mapLeftUp, mapRightUp,
				mapLeftBottom, mapRightBottom, matrixWidth, matrixHeight);
			break;
		case KernelImplementation::SSE2:
			InterpolateSSE2(image, imageStride, mapLeftUp, mapRightUp,
				mapLeftBottom, mapRightBottom, matrixWidth, matrixHeight);
			break;
		default:
			InterpolateScalar(image, imageStride, mapLeftUp, mapRightUp,
				mapLeftBottom, mapRightBottom, matrixWidth, matrixHeight);
			break;
		}
	}
}
