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
// ComputeLocalActivity3x3, BlurRiskMap, and ComputeChromaRisk are scalar-only
// as well: the 2D passes spend more time on cross-lane shuffles than on their
// arithmetic, and the risk combination is dominated by a randomly indexed
// 64K-entry table whose AVX2 gather measured 2.3x slower than scalar loads.
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

	// Shadow chroma correction kernels. The per-pixel risk map estimates how
	// unreliable the chroma of a strongly lifted, originally dark, flat pixel
	// is, so the caller can attenuate it toward the enhanced luma. gainRiskLut
	// is a 65536-entry table indexed by (originalLuma << 8) | enhancedLuma and
	// activityRiskLut is a 256-entry table indexed by local activity; both hold
	// Q8 values and are prebuilt by the caller. textureFloorQ8 is the fraction
	// of the risk that survives on textured areas, in Q8.
	void ComputeLocalActivity3x3(const unsigned char* luma, unsigned char* activity,
		int width, int height);
	void BlurRiskMap(unsigned char* risk, unsigned char* temp, int width, int height);
	void ComputeChromaRisk(const unsigned char* originalLuma, const unsigned char* enhancedLuma,
		const unsigned char* activity, unsigned char* risk, int pixelCount,
		const unsigned int* gainRiskLut, const unsigned int* activityRiskLut, int textureFloorQ8);
	void ApplyChromaAttenuation(unsigned char* target, const unsigned char* enhancedLuma,
		const unsigned char* risk, int pixelStart, int pixelEnd, int pixelStride,
		int maxStrengthQ8, KernelImplementation implementation);
}
