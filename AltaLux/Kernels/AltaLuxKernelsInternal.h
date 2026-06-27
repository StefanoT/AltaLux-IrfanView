#pragma once

#include "AltaLuxKernels.h"

namespace AltaLuxKernels
{
	inline unsigned char ClampToByte(int value)
	{
		return static_cast<unsigned char>(value < 0 ? 0 : (value > 255 ? 255 : value));
	}

	inline int ComputeRGBScale(int newY, int oldY, int c0, int c1, int c2, const int* reciprocalLut)
	{
		int scale = (newY * reciprocalLut[oldY] + (1 << 7)) >> 8;
		int maxChannel = c0 > c1 ? c0 : c1;
		maxChannel = maxChannel > c2 ? maxChannel : c2;
		if (maxChannel > 0)
		{
			const int scaleCap = (255 << 8) / maxChannel;
			if (scale > scaleCap)
			{
				scale = scaleCap;
			}
		}
		return scale;
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
	void MakeHistogramSSSE3(const unsigned char* image, int imageStride, int regionWidth,
		int regionHeight, unsigned int* histogram);
	void ClipHistogramSSSE3(unsigned int* histogram, unsigned int clipLimit);
	void MapHistogramSSSE3(unsigned int* histogram, unsigned int pixelCount);
	void AccumulateLayerSSSE3(unsigned int* accum, const unsigned char* layer, int pixelStart,
		int pixelEnd, int pixelStride, int weight, bool firstLayer);
	void WriteAccumulatedImageSSSE3(unsigned char* target, const unsigned int* accum, int pixelStart,
		int pixelEnd, int pixelStride, int weightScaleLog2, int weightHalf);
	void InterpolateSSSE3(unsigned char* image, int imageStride,
		const unsigned int* mapLeftUp, const unsigned int* mapRightUp,
		const unsigned int* mapLeftBottom, const unsigned int* mapRightBottom,
		unsigned int matrixWidth, unsigned int matrixHeight);

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
	void MakeHistogramAVX2(const unsigned char* image, int imageStride, int regionWidth,
		int regionHeight, unsigned int* histogram);
	void ClipHistogramAVX2(unsigned int* histogram, unsigned int clipLimit);
	void MapHistogramAVX2(unsigned int* histogram, unsigned int pixelCount);
	void AccumulateLayerAVX2(unsigned int* accum, const unsigned char* layer, int pixelStart,
		int pixelEnd, int pixelStride, int weight, bool firstLayer);
	void WriteAccumulatedImageAVX2(unsigned char* target, const unsigned int* accum, int pixelStart,
		int pixelEnd, int pixelStride, int weightScaleLog2, int weightHalf);
	void InterpolateAVX2(unsigned char* image, int imageStride,
		const unsigned int* mapLeftUp, const unsigned int* mapRightUp,
		const unsigned int* mapLeftBottom, const unsigned int* mapRightBottom,
		unsigned int matrixWidth, unsigned int matrixHeight);
}
