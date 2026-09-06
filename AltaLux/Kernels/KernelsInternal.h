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
	void AccumulateLayerScalar(unsigned int* accum, const unsigned char* layer, int pixelStart,
		int pixelEnd, int pixelStride, int weight, bool firstLayer);
	void WriteAccumulatedImageScalar(unsigned char* target, const unsigned int* accum, int pixelStart,
		int pixelEnd, int pixelStride, int weightScaleLog2, int weightHalf);
	void InterpolateScalar(unsigned char* image, int imageStride,
		const unsigned int* mapLeftUp, const unsigned int* mapRightUp,
		const unsigned int* mapLeftBottom, const unsigned int* mapRightBottom,
		unsigned int matrixWidth, unsigned int matrixHeight);
	void ComputeLocalActivity3x3Scalar(const unsigned char* luma, unsigned char* activity,
		int width, int height);
	void BlurRiskMapScalar(unsigned char* risk, unsigned char* temp, int width, int height);
	void ComputeChromaRiskScalar(const unsigned char* originalLuma, const unsigned char* enhancedLuma,
		const unsigned char* activity, unsigned char* risk, int pixelCount,
		const unsigned int* gainRiskLut, const unsigned int* activityRiskLut, int textureFloorQ8);
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
	void AccumulateLayerSSSE3(unsigned int* accum, const unsigned char* layer, int pixelStart,
		int pixelEnd, int pixelStride, int weight, bool firstLayer);
	void WriteAccumulatedImageSSSE3(unsigned char* target, const unsigned int* accum, int pixelStart,
		int pixelEnd, int pixelStride, int weightScaleLog2, int weightHalf);
	void ApplyChromaAttenuationSSSE3(unsigned char* target, const unsigned char* enhancedLuma,
		const unsigned char* risk, int pixelStart, int pixelEnd, int pixelStride, int maxStrengthQ8);

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
	void AccumulateLayerAVX2(unsigned int* accum, const unsigned char* layer, int pixelStart,
		int pixelEnd, int pixelStride, int weight, bool firstLayer);
	void WriteAccumulatedImageAVX2(unsigned char* target, const unsigned int* accum, int pixelStart,
		int pixelEnd, int pixelStride, int weightScaleLog2, int weightHalf);
	void ApplyChromaAttenuationAVX2(unsigned char* target, const unsigned char* enhancedLuma,
		const unsigned char* risk, int pixelStart, int pixelEnd, int pixelStride, int maxStrengthQ8);
}
