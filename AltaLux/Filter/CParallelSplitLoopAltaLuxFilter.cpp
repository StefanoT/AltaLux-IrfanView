/*
Project: AltaLux plugin for IrfanView
Author: Stefano Tommesani
Website: http://www.tommesani.com

Microsoft Public License (MS-PL) [OSI Approved License]
The full license text is in the LICENSE file at the root of the repository.
*/

#include "CParallelSplitLoopAltaLuxFilter.h"

#include <memory>
#include <ppl.h>

int CParallelSplitLoopAltaLuxFilter::Run()
{
	if (ClipLimit == 1.0)
		return AL_OK;

	auto pImage = static_cast<PixelType*>(ImageBuffer);
	auto pulMapArray = std::make_unique<unsigned int[]>(NumHorRegions * NumVertRegions * NUM_GRAY_LEVELS);

	const unsigned int NumPixels = static_cast<unsigned int>(RegionWidth) * static_cast<unsigned int>(RegionHeight);

	unsigned int ulClipLimit;
	if (ClipLimit > 0.0)
	{
		ulClipLimit = static_cast<unsigned int>(ClipLimit * (RegionWidth * RegionHeight) / NUM_GRAY_LEVELS);
		ulClipLimit = (ulClipLimit < 1UL) ? 1UL : ulClipLimit;
	}
	else
	{
		ulClipLimit = 1UL << 14; // Large value: adaptive histogram equalization without clipping.
	}

	// Phase 1 builds independent tile maps. parallel_for completes all maps
	// before Phase 2 begins, which is the required dependency barrier.
	concurrency::parallel_for((int)0, (int)(NumVertRegions + 1), [&](int uiY)
	{
		if (uiY < static_cast<int>(NumVertRegions))
		{
			PixelType* pImPointer = pImage;
			if (uiY > 0)
				pImPointer += ((RegionHeight >> 1) + ((uiY - 1) * RegionHeight)) * OriginalImageWidth;
			for (unsigned int uiX = 0; uiX < NumHorRegions; uiX++, pImPointer += RegionWidth)
			{
				unsigned int* pHistogram = &pulMapArray[NUM_GRAY_LEVELS * (uiY * NumHorRegions + uiX)];
				MakeHistogram(pImPointer, pHistogram);
				ClipHistogram(pHistogram, ulClipLimit);
				MapHistogram(pHistogram, NumPixels);
			}
		}
	});

	// Phase 2 reads tile maps only and writes non-overlapping output regions.
	concurrency::parallel_for((int)0, (int)(NumVertRegions + 1), [&](int uiY)
	{
		unsigned int uiSubX, uiSubY;
		unsigned int uiXL, uiXR, uiYU, uiYB;

		PixelType* pImPointer = pImage;
		if (uiY > 0)
			pImPointer += ((RegionHeight >> 1) + ((uiY - 1) * RegionHeight)) * OriginalImageWidth;

		// Edges use half regions and duplicate the nearest map where a neighbor is missing.
		if (uiY == 0)
		{
			uiSubY = RegionHeight >> 1;
			uiYU = 0;
			uiYB = 0;
		}
		else
		{
			if (uiY == NumVertRegions)
			{
				uiSubY = (RegionHeight >> 1) + (OriginalImageHeight - ImageHeight);
				uiYU = NumVertRegions - 1;
				uiYB = uiYU;
			}
			else
			{
				uiSubY = RegionHeight;
				uiYU = uiY - 1;
				uiYB = uiY;
			}
		}

		for (unsigned int uiX = 0; uiX <= NumHorRegions; uiX++)
		{
			if (uiX == 0)
			{
				uiSubX = RegionWidth >> 1;
				uiXL = 0;
				uiXR = 0;
			}
			else
			{
				if (uiX == NumHorRegions)
				{
					uiSubX = (RegionWidth >> 1) + (OriginalImageWidth - ImageWidth);
					uiXL = NumHorRegions - 1;
					uiXR = uiXL;
				}
				else
				{
					uiSubX = RegionWidth;
					uiXL = uiX - 1;
					uiXR = uiX;
				}
			}

			auto pulLU = &pulMapArray[NUM_GRAY_LEVELS * (uiYU * NumHorRegions + uiXL)];
			auto pulRU = &pulMapArray[NUM_GRAY_LEVELS * (uiYU * NumHorRegions + uiXR)];
			auto pulLB = &pulMapArray[NUM_GRAY_LEVELS * (uiYB * NumHorRegions + uiXL)];
			auto pulRB = &pulMapArray[NUM_GRAY_LEVELS * (uiYB * NumHorRegions + uiXR)];

			Interpolate(pImPointer, pulLU, pulRU, pulLB, pulRB, uiSubX, uiSubY);

			pImPointer += uiSubX;
		}
	});

	return AL_OK;
}
