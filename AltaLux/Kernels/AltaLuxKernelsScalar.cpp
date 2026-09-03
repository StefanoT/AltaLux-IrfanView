#include "AltaLuxKernelsInternal.h"

#include <cstring>

namespace AltaLuxKernels
{
	namespace
	{
		const int LumaMapSize = 256;

		struct RowMapPair
		{
			unsigned int left;
			unsigned int right;
		};

		inline int AbsDiff(int a, int b)
		{
			const int diff = a - b;
			return diff < 0 ? -diff : diff;
		}
	}

	void ExtractPackedYUVLumaScalar(const unsigned char* source, unsigned char* luma,
		int pixelCount, PackedYUVLumaPosition lumaPosition)
	{
		const int offset = (lumaPosition == PackedYUVLumaPosition::HighByte) ? 1 : 0;
		for (int i = 0; i < pixelCount; ++i)
		{
			luma[i] = source[(i * 2) + offset];
		}
	}

	void InjectPackedYUVLumaScalar(unsigned char* target, const unsigned char* luma,
		int pixelCount, PackedYUVLumaPosition lumaPosition)
	{
		const int offset = (lumaPosition == PackedYUVLumaPosition::HighByte) ? 1 : 0;
		for (int i = 0; i < pixelCount; ++i)
		{
			target[(i * 2) + offset] = luma[i];
		}
	}

	void ExtractRGBLumaScalar(const unsigned char* source, unsigned char* luma, int pixelCount,
		int pixelStride, int firstFactor, int secondFactor, int thirdFactor, int scalingLog)
	{
		const int roundingOffset = 1 << (scalingLog - 1);
		for (int i = 0; i < pixelCount; ++i)
		{
			const unsigned char* pixel = source + (i * pixelStride);
			int y = pixel[0] * firstFactor;
			y += pixel[1] * secondFactor;
			y += pixel[2] * thirdFactor;
			y = (y + roundingOffset) >> scalingLog;
			luma[i] = ClampToByte(y);
		}
	}

	void InjectRGBLumaScalar(unsigned char* image, const unsigned char* luma, int pixelCount,
		int pixelStride, int firstFactor, int secondFactor, int thirdFactor, int scalingLog,
		const int* reciprocalLut)
	{
		const int roundingOffset = 1 << (scalingLog - 1);
		for (int i = 0; i < pixelCount; ++i)
		{
			unsigned char* pixel = image + (i * pixelStride);
			int oldY = pixel[0] * firstFactor;
			oldY += pixel[1] * secondFactor;
			oldY += pixel[2] * thirdFactor;
			oldY = (oldY + roundingOffset) >> scalingLog;
			if (oldY > 255)
			{
				oldY = 255;
			}

			const int scale = ComputeRGBScale(luma[i], oldY, pixel[0], pixel[1], pixel[2], reciprocalLut);
			ApplyRGBScale(pixel, scale);
		}
	}

	void InjectRGBLumaWithOriginalLumaScalar(unsigned char* image, const unsigned char* luma,
		const unsigned char* originalLuma, int pixelCount, int pixelStride,
		const int* reciprocalLut)
	{
		int imageBase = 0;
		for (int i = 0; i < pixelCount; ++i)
		{
			unsigned char* pixel = image + imageBase;
			const int scale = ComputeRGBScale(luma[i], originalLuma[i],
				pixel[0], pixel[1], pixel[2], reciprocalLut);
			ApplyRGBScale(pixel, scale);
			imageBase += pixelStride;
		}
	}

	void ScaleDownBoxScalar(const unsigned char* source, int sourceWidth, int sourceHeight,
		unsigned char* target, int scaleFactor, int pixelStride)
	{
		if (source == nullptr || target == nullptr || sourceWidth <= 0 || sourceHeight <= 0 ||
			scaleFactor <= 0 || (pixelStride != 3 && pixelStride != 4))
		{
			return;
		}

		if (scaleFactor == 1)
		{
			memcpy(target, source, static_cast<size_t>(sourceWidth) * sourceHeight * pixelStride);
			return;
		}

		const int sourceStride = sourceWidth * pixelStride;
		const int targetWidth = sourceWidth / scaleFactor;
		const int targetHeight = sourceHeight / scaleFactor;
		const int targetStride = targetWidth * pixelStride;
		const int scaleArea = scaleFactor * scaleFactor;

		for (int y = 0; y < targetHeight; ++y)
		{
			const unsigned char* srcRow = source + ((y * scaleFactor) * sourceStride);
			unsigned char* dst = target + (y * targetStride);
			for (int x = 0; x < targetWidth; ++x)
			{
				unsigned int channelSum[4] = {};
				const unsigned char* srcBlock = srcRow + (x * scaleFactor * pixelStride);
				for (int iy = 0; iy < scaleFactor; ++iy)
				{
					const unsigned char* pixel = srcBlock + (iy * sourceStride);
					for (int ix = 0; ix < scaleFactor; ++ix)
					{
						channelSum[0] += pixel[0];
						channelSum[1] += pixel[1];
						channelSum[2] += pixel[2];
						if (pixelStride == 4)
						{
							channelSum[3] += pixel[3];
						}
						pixel += pixelStride;
					}
				}

				dst[0] = static_cast<unsigned char>(channelSum[0] / scaleArea);
				dst[1] = static_cast<unsigned char>(channelSum[1] / scaleArea);
				dst[2] = static_cast<unsigned char>(channelSum[2] / scaleArea);
				if (pixelStride == 4)
				{
					dst[3] = static_cast<unsigned char>(channelSum[3] / scaleArea);
				}
				dst += pixelStride;
			}
		}
	}

	void MakeHistogramScalar(const unsigned char* image, int imageStride, int regionWidth,
		int regionHeight, unsigned int* histogram)
	{
		memset(histogram, 0, sizeof(unsigned int) * LumaMapSize);

		for (int y = 0; y < regionHeight; ++y)
		{
			const unsigned char* rowEnd = image + regionWidth;
			while (image < rowEnd)
			{
				++histogram[*image++];
			}
			image += imageStride - regionWidth;
		}
	}

	void ClipHistogramScalar(unsigned int* histogram, unsigned int clipLimit)
	{
		unsigned int totalPixels = 0;
		for (int i = 0; i < LumaMapSize; ++i)
		{
			totalPixels += histogram[i];
		}

		const unsigned int minimumFeasibleClipLimit =
			(totalPixels + LumaMapSize - 1) / LumaMapSize;
		if (clipLimit < minimumFeasibleClipLimit)
		{
			clipLimit = minimumFeasibleClipLimit;
		}

		unsigned int excess = 0;
		for (int i = 0; i < LumaMapSize; ++i)
		{
			if (histogram[i] > clipLimit)
			{
				excess += histogram[i] - clipLimit;
				histogram[i] = clipLimit;
			}
		}

		while (excess)
		{
			bool madeProgress = false;
			const unsigned int binIncrement = excess / LumaMapSize;
			if (binIncrement > 0)
			{
				for (int i = 0; i < LumaMapSize && excess; ++i)
				{
					const unsigned int room = clipLimit - histogram[i];
					if (room > 0)
					{
						unsigned int add = room < binIncrement ? room : binIncrement;
						if (add > excess)
						{
							add = excess;
						}
						histogram[i] += add;
						excess -= add;
						madeProgress = true;
					}
				}
			}
			else
			{
				for (int i = 0; i < LumaMapSize && excess; ++i)
				{
					if (histogram[i] < clipLimit)
					{
						++histogram[i];
						--excess;
						madeProgress = true;
					}
				}
			}

			if (!madeProgress)
			{
				++clipLimit;
			}
		}
	}

	void MapHistogramScalar(unsigned int* histogram, unsigned int pixelCount)
	{
		unsigned int histogramSum = 0;
		const float scale = 255.0f / pixelCount;

		for (int i = 0; i < LumaMapSize; ++i)
		{
			histogramSum += histogram[i];
			const unsigned int targetValue = static_cast<unsigned int>(histogramSum * scale);
			histogram[i] = targetValue > 255 ? 255 : targetValue;
		}
	}

	void AccumulateLayerScalar(unsigned int* accum, const unsigned char* layer, int pixelStart,
		int pixelEnd, int pixelStride, int weight, bool firstLayer)
	{
		int sourceBase = pixelStart * pixelStride;
		int accumBase = pixelStart * 3;
		if (firstLayer)
		{
			for (int p = (pixelEnd - pixelStart); p > 0; --p)
			{
				const unsigned int c0 = static_cast<unsigned int>(layer[sourceBase] * weight);
				const unsigned int c1 = static_cast<unsigned int>(layer[sourceBase + 1] * weight);
				const unsigned int c2 = static_cast<unsigned int>(layer[sourceBase + 2] * weight);
				sourceBase += pixelStride;

				accum[accumBase] = c0;
				accum[accumBase + 1] = c1;
				accum[accumBase + 2] = c2;
				accumBase += 3;
			}
		}
		else {
			for (int p = (pixelEnd - pixelStart); p > 0; --p)
			{
				const unsigned int c0 = static_cast<unsigned int>(layer[sourceBase] * weight);
				const unsigned int c1 = static_cast<unsigned int>(layer[sourceBase + 1] * weight);
				const unsigned int c2 = static_cast<unsigned int>(layer[sourceBase + 2] * weight);
				sourceBase += pixelStride;

				accum[accumBase] += c0;
				accum[accumBase + 1] += c1;
				accum[accumBase + 2] += c2;
				accumBase += 3;
			}
		}
	}

	void WriteAccumulatedImageScalar(unsigned char* target, const unsigned int* accum, int pixelStart,
		int pixelEnd, int pixelStride, int weightScaleLog2, int weightHalf)
	{
		int targetBase = pixelStart * pixelStride;
		int accumBase = pixelStart * 3;
		for (int p = (pixelEnd - pixelStart); p > 0; --p)
		{
			target[targetBase] = ClampToByte(static_cast<int>((accum[accumBase] + weightHalf) >> weightScaleLog2));
			target[targetBase + 1] = ClampToByte(static_cast<int>((accum[accumBase + 1] + weightHalf) >> weightScaleLog2));
			target[targetBase + 2] = ClampToByte(static_cast<int>((accum[accumBase + 2] + weightHalf) >> weightScaleLog2));
			targetBase += pixelStride;
			accumBase += 3;
		}
	}

	void InterpolateScalar(unsigned char* image, int imageStride,
		const unsigned int* mapLeftUp, const unsigned int* mapRightUp,
		const unsigned int* mapLeftBottom, const unsigned int* mapRightBottom,
		unsigned int matrixWidth, unsigned int matrixHeight)
	{
		const unsigned int matrixArea = matrixWidth * matrixHeight;
		const bool useShift = (matrixArea & (matrixArea - 1)) == 0;
		unsigned int shiftIndex = 0;
		if (useShift)
		{
			unsigned int area = matrixArea;
			while (area >>= 1)
			{
				++shiftIndex;
			}
		}

		RowMapPair rowMap[LumaMapSize];
		for (unsigned int yCoef = 0, yInvCoef = matrixHeight;
			yCoef < matrixHeight;
			++yCoef, --yInvCoef, image += imageStride)
		{
			for (int grey = 0; grey < LumaMapSize; ++grey)
			{
				rowMap[grey].left = (yInvCoef * mapLeftUp[grey]) + (yCoef * mapLeftBottom[grey]);
				rowMap[grey].right = (yInvCoef * mapRightUp[grey]) + (yCoef * mapRightBottom[grey]);
			}

			unsigned char* pixel = image;
			if (useShift)
			{
				for (unsigned int xCoef = 0, xInvCoef = matrixWidth;
					xCoef < matrixWidth;
					++xCoef, --xInvCoef, ++pixel)
				{
					const RowMapPair& map = rowMap[*pixel];
					*pixel = static_cast<unsigned char>(((xInvCoef * map.left) + (xCoef * map.right)) >> shiftIndex);
				}
			}
			else
			{
				const unsigned int rounding = matrixArea >> 1;
				for (unsigned int xCoef = 0, xInvCoef = matrixWidth;
					xCoef < matrixWidth;
					++xCoef, --xInvCoef, ++pixel)
				{
					const RowMapPair& map = rowMap[*pixel];
					*pixel = static_cast<unsigned char>(((xInvCoef * map.left) + (xCoef * map.right) + rounding) / matrixArea);
				}
			}
		}
	}

	// Mean absolute deviation of the center pixel from its 3x3 neighborhood on a
	// packed luma plane, scaled to 0..255. Edges replicate so border pixels use
	// their own row/column twice. Flat luma yields 0, texture and noise yield
	// higher values; the chroma risk stage treats only the low end as flat.
	// Interior columns run without per-pixel edge clamps; the two border
	// columns keep the replicated-edge rule with their clamped duplicates.
	void ComputeLocalActivity3x3Scalar(const unsigned char* luma, unsigned char* activity,
		int width, int height)
	{
		if (luma == nullptr || activity == nullptr || width <= 0 || height <= 0)
		{
			return;
		}

		const int last = width - 1;
		for (int y = 0; y < height; ++y)
		{
			const unsigned char* rowUp = luma + ((y > 0 ? y - 1 : 0) * width);
			const unsigned char* row = luma + (y * width);
			const unsigned char* rowDown = luma + ((y < height - 1 ? y + 1 : height - 1) * width);
			unsigned char* out = activity + (y * width);
			for (int x = 1; x < last; ++x)
			{
				const int center = row[x];
				int sum = AbsDiff(rowUp[x - 1], center) + AbsDiff(rowUp[x], center) + AbsDiff(rowUp[x + 1], center);
				sum += AbsDiff(row[x - 1], center) + AbsDiff(row[x + 1], center);
				sum += AbsDiff(rowDown[x - 1], center) + AbsDiff(rowDown[x], center) + AbsDiff(rowDown[x + 1], center);
				out[x] = static_cast<unsigned char>((sum + 4) >> 3);
			}

			const int centerFirst = row[0];
			const int sumFirst = AbsDiff(rowUp[0], centerFirst) + AbsDiff(rowUp[1], centerFirst)
				+ AbsDiff(row[0], centerFirst) + AbsDiff(row[1], centerFirst)
				+ AbsDiff(rowDown[0], centerFirst) + AbsDiff(rowDown[1], centerFirst);
			out[0] = static_cast<unsigned char>((sumFirst + 4) >> 3);
			const int centerLast = row[last];
			const int sumLast = AbsDiff(rowUp[last - 1], centerLast) + AbsDiff(rowUp[last], centerLast)
				+ AbsDiff(row[last - 1], centerLast) + AbsDiff(row[last], centerLast)
				+ AbsDiff(rowDown[last - 1], centerLast) + AbsDiff(rowDown[last], centerLast);
			out[last] = static_cast<unsigned char>((sumLast + 4) >> 3);
		}
	}

	// Blurs the Q8 risk map with a separable [1 2 1]/4 kernel and clamped edges,
	// in place: the horizontal pass writes to temp, the vertical pass writes back
	// to risk. temp must hold width*height bytes and may alias the activity map
	// once that input is no longer needed. Interior columns avoid per-pixel
	// clamps; the border columns fold their replicated neighbor in directly.
	void BlurRiskMapScalar(unsigned char* risk, unsigned char* temp, int width, int height)
	{
		if (risk == nullptr || temp == nullptr || width <= 0 || height <= 0)
		{
			return;
		}

		const int last = width - 1;
		for (int y = 0; y < height; ++y)
		{
			const unsigned char* row = risk + (y * width);
			unsigned char* outRow = temp + (y * width);
			for (int x = 1; x < last; ++x)
			{
				outRow[x] = static_cast<unsigned char>((row[x - 1] + (row[x] << 1) + row[x + 1] + 2) >> 2);
			}
			outRow[0] = static_cast<unsigned char>(((row[0] * 3) + row[1] + 2) >> 2);
			outRow[last] = static_cast<unsigned char>((row[last - 1] + (row[last] * 3) + 2) >> 2);
		}

		for (int y = 0; y < height; ++y)
		{
			const unsigned char* rowUp = temp + ((y > 0 ? y - 1 : 0) * width);
			const unsigned char* row = temp + (y * width);
			const unsigned char* rowDown = temp + ((y < height - 1 ? y + 1 : height - 1) * width);
			unsigned char* outRow = risk + (y * width);
			for (int x = 0; x < width; ++x)
			{
				outRow[x] = static_cast<unsigned char>((rowUp[x] + (row[x] << 1) + rowDown[x] + 2) >> 2);
			}
		}
	}

	// Combines the luma-gain/darkness table with the local-activity table into
	// the final Q8 risk: full risk on flat pixels, textureFloorQ8 of it on
	// textured pixels, zero where the gain or darkness term already vanished.
	void ComputeChromaRiskScalar(const unsigned char* originalLuma, const unsigned char* enhancedLuma,
		const unsigned char* activity, unsigned char* risk, int pixelCount,
		const unsigned int* gainRiskLut, const unsigned int* activityRiskLut, int textureFloorQ8)
	{
		for (int i = 0; i < pixelCount; ++i)
		{
			const unsigned int gainRisk = gainRiskLut[(originalLuma[i] << 8) | enhancedLuma[i]];
			const unsigned int activityRisk = activityRiskLut[activity[i]];
			const unsigned int textureBlend = textureFloorQ8 +
				(((255 - textureFloorQ8) * activityRisk + 127) >> 8);
			risk[i] = static_cast<unsigned char>((gainRisk * textureBlend + 127) >> 8);
		}
	}

	// Attenuates chroma toward the neutral gray of the enhanced luma. The
	// per-pixel strength S = maxStrengthQ8 * risk >> 8 becomes a Q8 attenuation
	// A = 256 - S, and every color channel moves from its current value toward
	// the enhanced luma by that factor: c' = y' + ((c - y') * A) >> 8. A = 256
	// (zero risk) reproduces c exactly, and the result always stays between y'
	// and c, so no saturation can occur. The alpha/padding byte is untouched.
	void ApplyChromaAttenuationScalar(unsigned char* target, const unsigned char* enhancedLuma,
		const unsigned char* risk, int pixelStart, int pixelEnd, int pixelStride, int maxStrengthQ8)
	{
		int targetBase = pixelStart * pixelStride;
		for (int i = pixelStart; i < pixelEnd; ++i)
		{
			const int strength = (maxStrengthQ8 * risk[i] + 127) >> 8;
			const int attenuation = 256 - strength;
			const int newY = enhancedLuma[i];
			for (int c = 0; c < 3; ++c)
			{
				const int diff = target[targetBase + c] - newY;
				target[targetBase + c] = ClampToByte(newY + ((diff * attenuation + 128) >> 8));
			}
			targetBase += pixelStride;
		}
	}
}
