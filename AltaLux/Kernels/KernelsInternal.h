#pragma once

#include "Kernels.h"

namespace AltaLuxKernels
{
	inline unsigned char ClampToByte(int value)
	{
		return static_cast<unsigned char>(value < 0 ? 0 : (value > 255 ? 255 : value));
	}

	// Scale-cap lookup for the RGB inject kernels: table[maxChannel] holds
	// (255 << 8) / maxChannel so the largest channel maps to at most 255 without
	// drifting in hue, with a huge sentinel at index 0 that leaves black pixels
	// uncapped. The 1 KiB table replaces a per-pixel integer division in the
	// scalar and SSSE3 inject paths; the AVX2 tier gathers the same table.
	struct ScaleCapLutTable
	{
		int table[256];
	};

	extern const ScaleCapLutTable g_ScaleCapLut;

	inline int ComputeRGBScale(int newY, int oldY, int c0, int c1, int c2, const int* reciprocalLut)
	{
		int scale = (newY * reciprocalLut[oldY] + (1 << 7)) >> 8;
		int maxChannel = c0 > c1 ? c0 : c1;
		maxChannel = maxChannel > c2 ? maxChannel : c2;
		const int scaleCap = g_ScaleCapLut.table[maxChannel];
		return scale > scaleCap ? scaleCap : scale;
	}

	// Blend-domain variant of ComputeRGBScale for the fused multiscale kernel.
	// blendQ8 is the weighted layer sum >> 2: a Q8 fixed-point blended luma with
	// two fractional bits (up to 65280), so the Q16 reciprocal multiply must run
	// in unsigned arithmetic -- 65280 * 65536 exceeds INT_MAX but fits in 32
	// bits. reciprocalLut[0] is a zero sentinel, so Y=0 pixels scale to black.
	inline int ComputeBlendedRGBScale(int blendQ8, int originalY, int c0, int c1, int c2,
		const int* reciprocalLut)
	{
		const unsigned int scale = (static_cast<unsigned int>(blendQ8)
			* static_cast<unsigned int>(reciprocalLut[originalY]) + (1u << 15)) >> 16;
		int maxChannel = c0 > c1 ? c0 : c1;
		maxChannel = maxChannel > c2 ? maxChannel : c2;
		const int scaleCap = g_ScaleCapLut.table[maxChannel];
		return static_cast<int>(scale) > scaleCap ? scaleCap : static_cast<int>(scale);
	}

	// Per-lane scale for the SIMD tiers without gather support: the blend, the
	// reciprocal lookup and the cap all run scalar on one pixel.
	inline int ComputeBlendedScaleAt(const unsigned char* originalLuma, const unsigned char* fineLuma,
		const unsigned char* balancedLuma, const unsigned char* smoothLuma, int index,
		int fineWeight, int balancedWeight, int smoothWeight, const unsigned char* pixel,
		const int* reciprocalLut)
	{
		const int blendQ8 = (fineWeight * fineLuma[index] + balancedWeight * balancedLuma[index]
			+ smoothWeight * smoothLuma[index]) >> 2;
		return ComputeBlendedRGBScale(blendQ8, originalLuma[index], pixel[0], pixel[1], pixel[2],
			reciprocalLut);
	}

	inline void ApplyRGBScale(unsigned char* pixel, int scale)
	{
		pixel[0] = ClampToByte((pixel[0] * scale + (1 << 7)) >> 8);
		pixel[1] = ClampToByte((pixel[1] * scale + (1 << 7)) >> 8);
		pixel[2] = ClampToByte((pixel[2] * scale + (1 << 7)) >> 8);
	}

	void ExtractPackedYUVLumaScalar(const unsigned char* source, unsigned char* luma,
		int pixelCount, PackedYUVLumaPosition lumaPosition);
	void InjectPackedYUVLumaScalar(unsigned char* target, const unsigned char* luma,
		int pixelCount, PackedYUVLumaPosition lumaPosition);
	void ExtractRGBLumaScalar(const unsigned char* source, unsigned char* luma, int pixelCount,
		int pixelStride, int firstFactor, int secondFactor, int thirdFactor, int scalingLog);
	void InjectRGBLumaScalar(unsigned char* image, const unsigned char* luma, int pixelCount,
		int pixelStride, int firstFactor, int secondFactor, int thirdFactor, int scalingLog,
		const int* reciprocalLut);
	void InjectRGBLumaWithOriginalLumaScalar(unsigned char* image, const unsigned char* luma,
		const unsigned char* originalLuma, int pixelCount, int pixelStride,
		const int* reciprocalLut);
	void ScaleDownBoxScalar(const unsigned char* source, int sourceWidth, int sourceHeight,
		unsigned char* target, int scaleFactor, int pixelStride);
	void MakeHistogramScalar(const unsigned char* image, int imageStride, int regionWidth,
		int regionHeight, unsigned int* histogram);
	void ClipHistogramScalar(unsigned int* histogram, unsigned int clipLimit);
	void MapHistogramScalar(unsigned int* histogram, unsigned int pixelCount);
	void AccumulateLayerScalar(unsigned int* accum, int planeStride, const unsigned char* layer,
		int pixelStart, int pixelEnd, int pixelStride, int weight, bool firstLayer);
	void WriteAccumulatedImageScalar(unsigned char* target, const unsigned int* accum, int planeStride,
		int pixelStart, int pixelEnd, int pixelStride, int weightScaleLog2, int weightHalf);
	void BlendLumaAndInjectRGBScalar(unsigned char* image, const unsigned char* originalLuma,
		const unsigned char* fineLuma, const unsigned char* balancedLuma,
		const unsigned char* smoothLuma, int fineWeight, int balancedWeight, int smoothWeight,
		int pixelStart, int pixelEnd, int pixelStride, const int* reciprocalLut);
	void InterpolateScalar(unsigned char* image, int imageStride,
		const unsigned int* mapLeftUp, const unsigned int* mapRightUp,
		const unsigned int* mapLeftBottom, const unsigned int* mapRightBottom,
		unsigned int matrixWidth, unsigned int matrixHeight);
	void ComputeLocalActivity3x3Scalar(const unsigned char* luma, unsigned char* activity,
		int width, int height);
	void BlurRiskMapScalar(unsigned char* risk, unsigned char* temp, int width, int height);	void ComputeChromaRiskScalar(const unsigned char* originalLuma, const unsigned char* enhancedLuma,
		const unsigned char* activity, unsigned char* risk, int pixelCount,
		const unsigned char* gainRiskLut, const unsigned int* activityRiskLut, int textureFloorQ8);
	void ApplyChromaAttenuationScalar(unsigned char* target, const unsigned char* enhancedLuma,
		const unsigned char* risk, int pixelStart, int pixelEnd, int pixelStride, int maxStrengthQ8);

	void ExtractPackedYUVLumaSSSE3(const unsigned char* source, unsigned char* luma,
		int pixelCount, PackedYUVLumaPosition lumaPosition);
	void InjectPackedYUVLumaSSSE3(unsigned char* target, const unsigned char* luma,
		int pixelCount, PackedYUVLumaPosition lumaPosition);
	void ExtractRGBLumaSSSE3(const unsigned char* source, unsigned char* luma, int pixelCount,
		int pixelStride, int firstFactor, int secondFactor, int thirdFactor, int scalingLog);
	void InjectRGBLumaSSSE3(unsigned char* image, const unsigned char* luma, int pixelCount,
		int pixelStride, int firstFactor, int secondFactor, int thirdFactor, int scalingLog,
		const int* reciprocalLut);
	void InjectRGBLumaWithOriginalLumaSSSE3(unsigned char* image, const unsigned char* luma,
		const unsigned char* originalLuma, int pixelCount, int pixelStride,
		const int* reciprocalLut);
	void ScaleDownBoxSSSE3(const unsigned char* source, int sourceWidth, int sourceHeight,
		unsigned char* target, int scaleFactor, int pixelStride);
	void AccumulateLayerSSSE3(unsigned int* accum, int planeStride, const unsigned char* layer,
		int pixelStart, int pixelEnd, int pixelStride, int weight, bool firstLayer);
	void WriteAccumulatedImageSSSE3(unsigned char* target, const unsigned int* accum, int planeStride,
		int pixelStart, int pixelEnd, int pixelStride, int weightScaleLog2, int weightHalf);
	void BlendLumaAndInjectRGBSSSE3(unsigned char* image, const unsigned char* originalLuma,
		const unsigned char* fineLuma, const unsigned char* balancedLuma,
		const unsigned char* smoothLuma, int fineWeight, int balancedWeight, int smoothWeight,
		int pixelStart, int pixelEnd, int pixelStride, const int* reciprocalLut);
	void ApplyChromaAttenuationSSSE3(unsigned char* target, const unsigned char* enhancedLuma,
		const unsigned char* risk, int pixelStart, int pixelEnd, int pixelStride, int maxStrengthQ8);
	void ComputeLocalActivity3x3SSSE3(const unsigned char* luma, unsigned char* activity,
		int width, int height);
	void BlurRiskMapSSSE3(unsigned char* risk, unsigned char* temp, int width, int height);

	void ExtractPackedYUVLumaAVX2(const unsigned char* source, unsigned char* luma,
		int pixelCount, PackedYUVLumaPosition lumaPosition);
	void InjectPackedYUVLumaAVX2(unsigned char* target, const unsigned char* luma,
		int pixelCount, PackedYUVLumaPosition lumaPosition);
	void ExtractRGBLumaAVX2(const unsigned char* source, unsigned char* luma, int pixelCount,
		int pixelStride, int firstFactor, int secondFactor, int thirdFactor, int scalingLog);
	void InjectRGBLumaAVX2(unsigned char* image, const unsigned char* luma, int pixelCount,
		int pixelStride, int firstFactor, int secondFactor, int thirdFactor, int scalingLog,
		const int* reciprocalLut);
	void InjectRGBLumaWithOriginalLumaAVX2(unsigned char* image, const unsigned char* luma,
		const unsigned char* originalLuma, int pixelCount, int pixelStride,
		const int* reciprocalLut);
	void ScaleDownBoxAVX2(const unsigned char* source, int sourceWidth, int sourceHeight,
		unsigned char* target, int scaleFactor, int pixelStride);
	void AccumulateLayerAVX2(unsigned int* accum, int planeStride, const unsigned char* layer,
		int pixelStart, int pixelEnd, int pixelStride, int weight, bool firstLayer);
	void WriteAccumulatedImageAVX2(unsigned char* target, const unsigned int* accum, int planeStride,
		int pixelStart, int pixelEnd, int pixelStride, int weightScaleLog2, int weightHalf);
	void BlendLumaAndInjectRGBAVX2(unsigned char* image, const unsigned char* originalLuma,
		const unsigned char* fineLuma, const unsigned char* balancedLuma,
		const unsigned char* smoothLuma, int fineWeight, int balancedWeight, int smoothWeight,
		int pixelStart, int pixelEnd, int pixelStride, const int* reciprocalLut);
	void ApplyChromaAttenuationAVX2(unsigned char* target, const unsigned char* enhancedLuma,
		const unsigned char* risk, int pixelStart, int pixelEnd, int pixelStride, int maxStrengthQ8);
	void ComputeLocalActivity3x3AVX2(const unsigned char* luma, unsigned char* activity,
		int width, int height);
	void BlurRiskMapAVX2(unsigned char* risk, unsigned char* temp, int width, int height);
}
