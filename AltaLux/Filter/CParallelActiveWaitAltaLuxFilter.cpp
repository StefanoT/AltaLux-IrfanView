/*
Project: AltaLux plugin for IrfanView
Author: Stefano Tommesani
Website: http://www.tommesani.com

Microsoft Public License (MS-PL) [OSI Approved License]

This license governs use of the accompanying software. If you use the software, you accept this license. If you do not accept the license, do not use the software.

1. Definitions
The terms "reproduce," "reproduction," "derivative works," and "distribution" have the same meaning here as under U.S. copyright law.
A "contribution" is the original software, or any additions or changes to the software.
A "contributor" is any person that distributes its contribution under this license.
"Licensed patents" are a contributor's patent claims that read directly on its contribution.

2. Grant of Rights
(A) Copyright Grant- Subject to the terms of this license, including the license conditions and limitations in section 3, each contributor grants you a non-exclusive, worldwide, royalty-free copyright license to reproduce its contribution, prepare derivative works of its contribution, and distribute its contribution or any derivative works that you create.
(B) Patent Grant- Subject to the terms of this license, including the license conditions and limitations in section 3, each contributor grants you a non-exclusive, worldwide, royalty-free license under its licensed patents to make, have made, use, sell, offer for sale, import, and/or otherwise dispose of its contribution in the software or derivative works of the contribution in the software.

3. Conditions and Limitations
(A) No Trademark License- This license does not grant you rights to use any contributors' name, logo, or trademarks.
(B) If you bring a patent claim against any contributor over patents that you claim are infringed by the software, your patent license from such contributor to the software ends automatically.
(C) If you distribute any portion of the software, you must retain all copyright, patent, trademark, and attribution notices that are present in the software.
(D) If you distribute any portion of the software in source code form, you may do so only under this license by including a complete copy of this license with your distribution. If you distribute any portion of the software in compiled or object code form, you may only do so under a license that complies with this license.
(E) The software is licensed "as-is." You bear the risk of using it. The contributors give no express warranties, guarantees or conditions. You may have additional consumer rights under your local laws which this license cannot change. To the extent permitted under your local laws, the contributors exclude the implied warranties of merchantability, fitness for a particular purpose and non-infringement.
*/

#include "CParallelActiveWaitAltaLuxFilter.h"

#include <windows.h>
#include <memory>
#include <ppl.h>

/// <summary>
/// processes incoming image
/// </summary>
/// <returns>error code, refer to AL_XXX codes</returns>
/// <remarks>
/// working parallel code with single loop, using active waits to protect data dependencies
/// [Filter processing of full resolution image] in [1.131 seconds]
/// </remarks>
int CParallelActiveWaitAltaLuxFilter::Run()
{
	if (ClipLimit == 1.0)
		return AL_OK; //< is OK, immediately returns original image

	auto pImage = static_cast<PixelType *>(ImageBuffer);

	/// pulMapArray is pointer to mappings
	auto pulMapArray = std::make_unique<unsigned int[]>(NumHorRegions * NumVertRegions * NUM_GRAY_LEVELS);
	if (pulMapArray == nullptr)
		return AL_OUT_OF_MEMORY; //< not enough memory

	/// region pixel count
	auto NumPixels = static_cast<unsigned int>(RegionWidth) * static_cast<unsigned int>(RegionHeight);
	//< region pixel count

	unsigned int ulClipLimit; //< clip limit
	if (ClipLimit > 0.0)
	{
		/// calculate actual cliplimit
		ulClipLimit = static_cast<unsigned int>(ClipLimit * (RegionWidth * RegionHeight) / NUM_GRAY_LEVELS);
		ulClipLimit = (ulClipLimit < 1UL) ? 1UL : ulClipLimit;
	}
	else
		ulClipLimit = 1UL << 14; //< large value, do not clip (AHE)

	/// Interpolate greylevel mappings to get CLAHE image
	// create events for signaling that the first phase is completed
	__declspec(align(32)) volatile LONG FirstPhaseCompleted[MAX_VERT_REGIONS + 1];
	for (auto i = 0; i <= NumVertRegions; i++)
		InterlockedExchange((volatile LONG*)&FirstPhaseCompleted[i], -1);

	concurrency::parallel_for((LONG)0, (LONG)(NumVertRegions + 1), [&](LONG uiY)
	{
		// first half
		PixelType* pImPointer = pImage;
		if (uiY > 0)
			pImPointer += ((RegionHeight >> 1) + ((uiY - 1) * RegionHeight)) * OriginalImageWidth;

		if (uiY < NumVertRegions)
		{
			/// calculate greylevel mappings for each contextual region
			for (LONG uiX = 0; uiX < NumHorRegions; uiX++, pImPointer += RegionWidth)
			{
				unsigned int* pHistogram = &pulMapArray[NUM_GRAY_LEVELS * (uiY * NumHorRegions + uiX)];
				MakeHistogram(pImPointer, pHistogram);
				ClipHistogram(pHistogram, ulClipLimit);
				MapHistogram(pHistogram, NumPixels);
				InterlockedExchange((volatile LONG*)&FirstPhaseCompleted[uiY], uiX);
			}
		}

		// second half
		unsigned int uiSubX, uiSubY; //< size of subimages
		unsigned int uiXL, uiXR, uiYU, uiYB; //< auxiliary variables interpolation routine

		pImPointer = pImage;
		if (uiY > 0)
			pImPointer += ((RegionHeight >> 1) + ((uiY - 1) * RegionHeight)) * OriginalImageWidth;

		if (uiY == 0)
		{
			/// special case: top row
			uiSubY = RegionHeight >> 1;
			uiYU = 0;
			uiYB = 0;
		}
		else
		{
			if (uiY == NumVertRegions)
			{
				/// special case: bottom row
				uiSubY = (RegionHeight >> 1) + (OriginalImageHeight - ImageHeight);
				uiYU = NumVertRegions - 1;
				uiYB = uiYU;
			}
			else
			{
				/// default values
				uiSubY = RegionHeight;
				uiYU = uiY - 1;
				uiYB = uiY;
			}
		}

		for (LONG uiX = 0; uiX <= NumHorRegions; uiX++)
		{
			if (uiX == 0)
			{
				/// special case: left column
				uiSubX = RegionWidth >> 1;
				uiXL = 0;
				uiXR = 0;
			}
			else
			{
				if (uiX == NumHorRegions)
				{
					/// special case: right column
					uiSubX = (RegionWidth >> 1) + (OriginalImageWidth - ImageWidth);
					uiXL = NumHorRegions - 1;
					uiXR = uiXL;
				}
				else
				{
					/// default values
					uiSubX = RegionWidth;
					uiXL = uiX - 1;
					uiXR = uiX;
				}
			}
			if (uiY > 0)
			{
				while (true)
				{
					// active waiting until processing of needed blocks on previous line has completed
					auto statePrevLine = InterlockedExchangeAdd((volatile LONG*)&FirstPhaseCompleted[uiY - 1], 0);
					if ((statePrevLine > -1) && ((statePrevLine >= uiXR) || (statePrevLine == (NumHorRegions - 1))))
						break;
					Sleep(1);
				}
			}

			auto pulLU = &pulMapArray[NUM_GRAY_LEVELS * (uiYU * NumHorRegions + uiXL)];
			auto pulRU = &pulMapArray[NUM_GRAY_LEVELS * (uiYU * NumHorRegions + uiXR)];
			auto pulLB = &pulMapArray[NUM_GRAY_LEVELS * (uiYB * NumHorRegions + uiXL)];
			auto pulRB = &pulMapArray[NUM_GRAY_LEVELS * (uiYB * NumHorRegions + uiXR)];

			Interpolate(pImPointer, pulLU, pulRU, pulLB, pulRB, uiSubX, uiSubY);

			pImPointer += uiSubX; //< set pointer on next matrix
		}
	});

	return AL_OK; //< return status OK
}
