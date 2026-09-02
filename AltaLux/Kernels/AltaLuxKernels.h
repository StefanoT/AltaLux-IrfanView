#pragma once

// Kernel entry points dispatch to scalar, SSSE3, or AVX2 implementations.
// SIMD paths may still delegate specific operations to scalar code when the
// instruction set lacks safe scatter/gather support or when handling tails.
//
// MakeHistogram, ClipHistogram, MapHistogram, and Interpolate are scalar-only
// and take no implementation argument: their algorithms (conflicting histogram
// increments, loop-carried prefix sums, stateful redistribution, and
// grey-indexed row-map interpolation) do not vectorize safely or profitably
// with SSSE3/AVX2, so every tier runs the same scalar code.
namespace AltaLuxKernels
{
	enum class KernelImplementation
	{
		Scalar,
		SSSE3,
		AVX2
	};

	enum class PackedYUVLumaPosition
	{
		LowByte,
		HighByte
	};

	const char* GetImplementationName(KernelImplementation implementation);
	bool IsImplementationSupported(KernelImplementation implementation);
	KernelImplementation GetBestSupportedImplementation();

	void ExtractPackedYUVLuma(const unsigned char* source, unsigned char* luma,
		int pixelCount, PackedYUVLumaPosition lumaPosition, KernelImplementation implementation);
	void InjectPackedYUVLuma(unsigned char* target, const unsigned char* luma,
		int pixelCount, PackedYUVLumaPosition lumaPosition, KernelImplementation implementation);

	void ExtractRGBLuma(const unsigned char* source, unsigned char* luma, int pixelCount,
		int pixelStride, int firstFactor, int secondFactor, int thirdFactor, int scalingLog,
		KernelImplementation implementation);
	void InjectRGBLuma(unsigned char* image, const unsigned char* luma, int pixelCount,
		int pixelStride, int firstFactor, int secondFactor, int thirdFactor, int scalingLog,
		const int* reciprocalLut, KernelImplementation implementation);
	void InjectRGBLumaWithOriginalLuma(unsigned char* image, const unsigned char* luma,
		const unsigned char* originalLuma, int pixelCount, int pixelStride,
		const int* reciprocalLut, KernelImplementation implementation);
	void ScaleDownBox(const unsigned char* source, int sourceWidth, int sourceHeight,
		unsigned char* target, int scaleFactor, int pixelStride, KernelImplementation implementation);

	void MakeHistogram(const unsigned char* image, int imageStride, int regionWidth,
		int regionHeight, unsigned int* histogram);
	void ClipHistogram(unsigned int* histogram, unsigned int clipLimit);
	void MapHistogram(unsigned int* histogram, unsigned int pixelCount);

	void AccumulateLayer(unsigned int* accum, const unsigned char* layer, int pixelStart,
		int pixelEnd, int pixelStride, int weight, bool firstLayer, KernelImplementation implementation);
	void WriteAccumulatedImage(unsigned char* target, const unsigned int* accum, int pixelStart,
		int pixelEnd, int pixelStride, int weightScaleLog2, int weightHalf,
		KernelImplementation implementation);
	void Interpolate(unsigned char* image, int imageStride,
		const unsigned int* mapLeftUp, const unsigned int* mapRightUp,
		const unsigned int* mapLeftBottom, const unsigned int* mapRightBottom,
		unsigned int matrixWidth, unsigned int matrixHeight);
}
