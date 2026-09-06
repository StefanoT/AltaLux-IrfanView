#pragma once

// =============================================================================
// Kernel design contract
// =============================================================================
// Every kernel is implemented in Scalar; the SSSE3 and AVX2 tiers extend the
// hot ones. Four rules hold across the layer:
//
// 1. Bit-exact equivalence. A SIMD kernel returns exactly the bytes the scalar
//    kernel returns, on every input including tails. No approximations: this
//    rules out rcp-based reciprocal estimation and float rewrites of the
//    fixed-point math. AltaLuxUnitTest (TestStrategies.cpp) pins it per tier
//    with SSSE3/AVX2FilterOutputsMatchScalar (end-to-end filter runs),
//    SSSE3/AVX2ScaleDownBoxMatchesScalar, and SSSE3/AVX2ChromaKernelsMatchScalar.
//
// 2. Tail layering. A SIMD kernel vectorizes whole blocks, then delegates the
//    remaining pixels to the next lower tier: AVX2 tail -> SSSE3 tail ->
//    scalar. Dispatch (Kernels.cpp) normalizes an unsupported tier to Scalar
//    before calling, and AVX2 implies SSSE3, so a delegated tail can never run
//    instructions the CPU lacks.
//
// 3. No overreads. Fixed-width loads must never cross the end of a caller
//    buffer. RGB24 loops load 16 bytes = 5 1/3 pixels, so they stop 6 pixels
//    short of the end (the "6-px stop rule") and hand the remainder to the
//    next tier.
//
// 4. Measured, not assumed. Tier assignments and "SIMD lost" claims are
//    reproducible with AltaLuxBench: 3840x2160 random buffers, calibrated
//    batch size, median of 15 samples interleaved round-robin across tiers to
//    cancel clock drift. Re-run it before moving a kernel between tiers.
//
// Build layout: one translation unit per tier so /arch stays per file --
// KernelsAVX2.cpp with /arch:AVX2, KernelsSSSE3.cpp with /arch:SSE2 on Win32,
// KernelsScalar.cpp at the platform baseline. The SSE2 setting is safe: MSVC
// emits the SSSE3 instructions its intrinsics name regardless of /arch, and
// the hot loops are all intrinsics; only compiler-generated code in that file
// stays at SSE2. Do not merge tiers into one file: per-file /arch is what
// keeps SIMD code from leaking into the baseline or into a tier a CPU may
// not support.
//
// Kernel x tier matrix (vectorized kernels tail-layer per rule 2; SIMD paths
// also fall back to their scalar kernel for unsupported shapes such as odd
// pixel strides):
//
//   Vectorized above scalar:
//     ExtractPackedYUVLuma / InjectPackedYUVLuma     packed 16-bit YUV words
//     ExtractRGBLuma                                 RGB/BGR, 24 and 32 bpp
//     InjectRGBLuma / InjectRGBLumaWithOriginalLuma  RGB/BGR 32 bpp only; SSSE3
//                                                    has no gather, so the
//                                                    reciprocal lookup and the
//                                                    scale cap stay scalar there
//     ScaleDownBox                                   scaleFactor == 2 only
//     AccumulateLayer / WriteAccumulatedImage        multiscale accumulation
//     ApplyChromaAttenuation                         RGB/BGR, 24 and 32 bpp
//
//   Scalar-only (no implementation argument; every tier runs the same code):
//     MakeHistogram / ClipHistogram / MapHistogram   conflicting histogram
//                                                    increments, loop-carried
//                                                    prefix sums, stateful
//                                                    redistribution
//     Interpolate                                    grey-indexed row-map lookup;
//                                                    SIMD designs measured slower
//     ComputeChromaRisk                              dominated by a randomly
//                                                    indexed 64K-entry table;
//                                                    AVX2 gather measured 2.3x
//                                                    slower than scalar loads
//     ComputeLocalActivity3x3 / BlurRiskMap          vectorizable along x with
//                                                    no cross-lane work, not yet
//                                                    rewritten; see the notes in
//                                                    Kernels.cpp
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
