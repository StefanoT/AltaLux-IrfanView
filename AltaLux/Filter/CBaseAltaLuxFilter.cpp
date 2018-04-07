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

#include "CBaseAltaLuxFilter.h"

#include <windows.h>
#include <cstdio>
#include <cmath>
#include <malloc.h>
#include <memory>
#include <ppl.h>

#ifdef ENABLE_LOGGING
	#include "..\Log\easylogging++.h"
#endif // ENABLE_LOGGING

CBaseAltaLuxFilter::CBaseAltaLuxFilter(int Width, int Height, int HorSlices, int VerSlices)
{
	OriginalImageWidth = Width;
	OriginalImageHeight = Height;

	/// delay allocation of ImageBuffer into SetStrength
	ImageBuffer = nullptr;

	NumHorRegions = HorSlices;
	NumVertRegions = VerSlices;

	RegionWidth = OriginalImageWidth / NumHorRegions;
	RegionHeight = OriginalImageHeight / NumVertRegions;

	ImageWidth = RegionWidth * NumHorRegions;
	ImageHeight = RegionHeight * NumVertRegions;

	SetStrength();
}

CBaseAltaLuxFilter::~CBaseAltaLuxFilter()
{
	if (ImageBuffer)
	{
		try
		{
			delete[] ImageBuffer;
		}
		catch (...)
		{
			ImageBuffer = nullptr;
		}
	}
}

void CBaseAltaLuxFilter::SetSlices(int HorSlices, int VerSlices)
{
	// clamp number of regions
	if (HorSlices < MIN_HOR_REGIONS)
		HorSlices = MIN_HOR_REGIONS;
	if (HorSlices > MAX_HOR_REGIONS)
		HorSlices = MAX_HOR_REGIONS;
	if (VerSlices < MIN_VERT_REGIONS)
		VerSlices = MIN_VERT_REGIONS;
	if (VerSlices > MAX_VERT_REGIONS)
		VerSlices = MAX_VERT_REGIONS;

	NumHorRegions = HorSlices;
	NumVertRegions = VerSlices;

	RegionWidth = OriginalImageWidth / NumHorRegions;
	RegionHeight = OriginalImageHeight / NumVertRegions;

	ImageWidth = RegionWidth * NumHorRegions;
	ImageHeight = RegionHeight * NumVertRegions;
}

void CBaseAltaLuxFilter::SetStrength(int _Strength)
{
	Strength = _Strength + 4;
	if (Strength < AL_MIN_STRENGTH)
		Strength = AL_MIN_STRENGTH;
	if (Strength > AL_MAX_STRENGTH)
		Strength = AL_MAX_STRENGTH;

	if (Strength == AL_MIN_STRENGTH)
	{
		/// free ImageBuffer, if allocated
		if (ImageBuffer)
		{
			try
			{
				delete[] ImageBuffer;
			}
			catch (...)
			{
			}
			ImageBuffer = nullptr;
		}
	}
	else
	{
		if (ImageBuffer == nullptr)
		{
			/// allocate ImageBuffer
			try
			{
				ImageBuffer = new unsigned char[IMAGE_BUFFER_SIZE];
			}
			catch (...)
			{
				ImageBuffer = nullptr;
			}
		}
	}

	ClipLimit = MIN_CLIP_LIMIT + (MAX_CLIP_LIMIT - MIN_CLIP_LIMIT) * ((float)(Strength - AL_MIN_STRENGTH)) / (
		AL_MAX_STRENGTH - AL_MIN_STRENGTH);
	if (ClipLimit < MIN_CLIP_LIMIT)
		ClipLimit = MIN_CLIP_LIMIT;
	if (ClipLimit > MAX_CLIP_LIMIT)
		ClipLimit = MAX_CLIP_LIMIT;
}

bool CBaseAltaLuxFilter::IsEnabled() const
{
	return Strength != AL_MIN_STRENGTH;
}

int CBaseAltaLuxFilter::ProcessUYVY(void* Image)
{
#ifdef _WIN64

#else
	if (Image == nullptr)
		return AL_NULL_IMAGE;

	if (!IsEnabled())
		return AL_OK;

	if (ImageBuffer == nullptr)
	{
		/// if ImageBuffer allocation failed in the constructor, try again
		/// if it still fails, return AL_OUT_OF_MEMORY
		try
		{
			ImageBuffer = new unsigned char[IMAGE_BUFFER_SIZE];
		}
		catch (...)
		{
			ImageBuffer = nullptr;
		}
		if (ImageBuffer == nullptr)
			return AL_OUT_OF_MEMORY;
	}

	/// copy luma from UYVY Image into ImageBuffer
	auto ImagePtr = static_cast<unsigned char *>(Image);
	auto ImageBufferPtr = static_cast<unsigned char *>(ImageBuffer);

	//	ImagePtr++;
	//	for (int i = (ImageWidth * ImageHeight); i > 0; i--)
	//	{
	//		*ImageBufferPtr = *ImagePtr;
	//		ImagePtr += 2;
	//		ImageBufferPtr++;
	//	}

	int ImageSize = ImageWidth * ImageHeight;

	__asm
	{
		mov EAX,ImagePtr
		mov EDX,ImageBufferPtr
		mov ECX,ImageSize
		shr ECX,3

		ALIGN 16
		CopyIntoImageBufferLoop:
		movq mm0,[EAX]
		movq mm1,[EAX+8]
		add EAX,16
		psrlw mm0,8
		psrlw mm1,8
		packuswb mm0,mm1
		movq [EDX],mm0
		add EDX,8
		sub ECX,1
		jnz CopyIntoImageBufferLoop

		emms
	}
	/// perform processing on ImageBuffer
	auto RunReturn = Run();
	if (RunReturn != AL_OK)
		return RunReturn;

	/// copy processed luma back into UYVY Image
	ImagePtr = static_cast<unsigned char *>(Image);
	ImageBufferPtr = static_cast<unsigned char *>(ImageBuffer);

	// ImagePtr++;
	// for (int j = (ImageWidth * ImageHeight); j > 0; j--)
	// {
	// 	*ImagePtr = *ImageBufferPtr;
	// 	ImagePtr += 2;
	// 	ImageBufferPtr++;
	// }

#define CHROMA_MASK	mm6

	__asm
	{
		mov EAX,ImagePtr
		mov EDX,ImageBufferPtr
		mov ECX,ImageSize
		shr ECX,3
		pcmpeqb CHROMA_MASK,CHROMA_MASK
		psrlw CHROMA_MASK,8

		ALIGN 16
		CopyIntoImageLoop:
		movq mm0,[EDX]
		add EDX,8
		movq mm3,[EAX]
		movq mm4,[EAX+8]
		pxor mm1,mm1
		pxor mm2,mm2
		punpcklbw mm1,mm0
		punpckhbw mm2,mm0
		pand mm3,CHROMA_MASK
		pand mm4,CHROMA_MASK
		por mm1,mm3
		por mm2,mm4
		movq [EAX],mm1
		movq [EAX+8],mm2
		add EAX,16
		sub ECX,1
		jnz CopyIntoImageLoop

		emms
	}

#undef CHROMA_MASK

#endif // _WIN64

	return AL_OK;
}

int CBaseAltaLuxFilter::ProcessVYUY(void* Image)
{
	return ProcessUYVY(Image); //< no operations are performed on chroma
}

int CBaseAltaLuxFilter::ProcessYUYV(void* Image)
{
#ifdef _WIN64

#else
	if (Image == nullptr)
		return AL_NULL_IMAGE;

	if (ImageBuffer == nullptr)
	{
		/// if ImageBuffer allocation failed in the constructor, try again
		/// if it still fails, return AL_OUT_OF_MEMORY
		try
		{
			ImageBuffer = new unsigned char[IMAGE_BUFFER_SIZE];
		}
		catch (...)
		{
			ImageBuffer = nullptr;
		}
		if (ImageBuffer == nullptr)
			return AL_OUT_OF_MEMORY;
	}

	/// copy luma from YUYV Image into ImageBuffer
	auto ImagePtr = static_cast<unsigned char *>(Image);
	auto ImageBufferPtr = static_cast<unsigned char *>(ImageBuffer);

	//	for (int i = (ImageWidth * ImageHeight); i > 0; i--)
	//	{
	//		*ImageBufferPtr = *ImagePtr;
	//		ImagePtr += 2;
	//		ImageBufferPtr++;
	//	}

	int ImageSize = ImageWidth * ImageHeight;

#define LUMA_MASK	mm6
	__asm
	{
		mov EAX,ImagePtr
		mov EDX,ImageBufferPtr
		mov ECX,ImageSize
		shr ECX,3
		pcmpeqb LUMA_MASK,LUMA_MASK
		psrlw LUMA_MASK,8

		ALIGN 16
		CopyIntoImageBufferLoop:
		movq mm0,[EAX]
		movq mm1,[EAX+8]
		add EAX,16
		pand mm0,LUMA_MASK
		pand mm1,LUMA_MASK
		packuswb mm0,mm1
		movq [EDX],mm0
		add EDX,8
		sub ECX,1
		jnz CopyIntoImageBufferLoop

		emms
	}
#undef LUMA_MASK

	/// perform processing on ImageBuffer
	int RunReturn = Run();
	if (RunReturn != AL_OK)
		return RunReturn;

	/// copy processed luma back into YUYV Image
	ImagePtr = (unsigned char *)Image;
	ImageBufferPtr = (unsigned char *)ImageBuffer;

	//	for (int j = (ImageWidth * ImageHeight); j > 0; j--)
	//	{
	//		*ImagePtr = *ImageBufferPtr;
	//		ImagePtr += 2;
	//		ImageBufferPtr++;
	//	}

#define CHROMA_MASK	mm6

	__asm
	{
		mov EAX,ImagePtr
		mov EDX,ImageBufferPtr
		mov ECX,ImageSize
		shr ECX,3
		pcmpeqb CHROMA_MASK,CHROMA_MASK
		psllw CHROMA_MASK,8

		ALIGN 16
		CopyIntoImageLoop:
		movq mm1,[EDX]
		pxor mm0,mm0
		movq mm2,mm1
		add EDX,8
		movq mm3,[EAX]
		movq mm4,[EAX+8]
		punpcklbw mm1,mm0
		punpckhbw mm2,mm0
		pand mm3,CHROMA_MASK
		pand mm4,CHROMA_MASK
		por mm1,mm3
		por mm2,mm4
		movq [EAX],mm1
		movq [EAX+8],mm2
		add EAX,16
		sub ECX,1
		jnz CopyIntoImageLoop

		emms
	}

#undef CHROMA_MASK

#endif // _WIN64
	return AL_OK;
}

int CBaseAltaLuxFilter::ProcessYVYU(void* Image)
{
	return ProcessYUYV(Image); //< no operations are performed on chroma
}

/// <summary>
/// process a 8-bpp, luma-only input image
/// </summary>
/// <param name="Image">image to be processed</param>
/// <returns></returns>
int CBaseAltaLuxFilter::ProcessGray(void* Image)
{
	if (Image == nullptr)
		return AL_NULL_IMAGE;

	// as the input buffer is already in the gray 8bpp pixel format, do not copy data to ImageBuffer and use the provided buffer directly
	unsigned char* SavedImageBuffer = ImageBuffer;
	ImageBuffer = static_cast<unsigned char *>(Image);

	const int RunReturn = Run();
	// restore ImageBuffer
	ImageBuffer = SavedImageBuffer;
	if (RunReturn != AL_OK)
		return RunReturn;
	return AL_OK;
}

#include <mmintrin.h>

/// Ey = 0.299*Er + 0.587*Eg + 0.114*Eb
const int SCALING_LOG = 15;
const int SCALING_FACTOR = (1 << SCALING_LOG);
const int Y_RED_SCALE = static_cast<int>(0.299 * SCALING_FACTOR);
const int Y_GREEN_SCALE = static_cast<int>(0.587 * SCALING_FACTOR);
const int Y_BLUE_SCALE = static_cast<int>(0.114 * SCALING_FACTOR);

/// <summary>
/// process an input image with a generic format
/// </summary>
/// <param name="Image">image to be processed</param>
/// <param name="FirstFactor">scaling factor for first byte of each pixel</param>
/// <param name="SecondFactor">scaling factor for second byte of each pixel</param>
/// <param name="ThirdFactor">scaling factor for third byte of each pixel</param>
/// <param name="PixelOffset">distance in bytes between pixels (3 for RGB24, 4 for RGB32)</param>
/// <returns></returns>
int CBaseAltaLuxFilter::ProcessGeneric(void* Image, int FirstFactor, int SecondFactor,
                                       int ThirdFactor, int PixelOffset)
{
	if (Image == nullptr)
		return AL_NULL_IMAGE;

	if (ImageBuffer == nullptr)
	{
		/// if ImageBuffer allocation failed in the constructor, try again
		/// if it still fails, return AL_OUT_OF_MEMORY
		try
		{
			ImageBuffer = new unsigned char[IMAGE_BUFFER_SIZE];
		}
		catch (...)
		{
			ImageBuffer = nullptr;
		}
		if (ImageBuffer == nullptr)
			return AL_OUT_OF_MEMORY;
	}

	/// extract Y component from generic RGB image
	unsigned char* ImagePtr = (unsigned char *)Image;
	unsigned char* ImageBufferPtr = (unsigned char *)ImageBuffer;

	/// C code
	for (int i = (OriginalImageWidth * OriginalImageHeight); i > 0; i--)
	{
		int YValue = (ImagePtr[0] * FirstFactor) +
			(ImagePtr[1] * SecondFactor) +
			(ImagePtr[2] * ThirdFactor);
		ImagePtr += PixelOffset;
		YValue += 1 << (SCALING_LOG - 1);
		YValue >>= SCALING_LOG;
		if (YValue > 255)
			YValue = 255;
		*ImageBufferPtr = (unsigned char)YValue;
		ImageBufferPtr++;
	}

	/// perform processing on ImageBuffer
	int RunReturn = Run();
	if (RunReturn != AL_OK)
		return RunReturn;

	/// inject Y component back into generic RGB image
	ImagePtr = (unsigned char *)Image;
	ImageBufferPtr = (unsigned char *)ImageBuffer;

	/// C code
	for (int j = (OriginalImageWidth * OriginalImageHeight); j > 0; j--)
	{
		int OldYValue = (ImagePtr[0] * FirstFactor) +
			(ImagePtr[1] * SecondFactor) +
			(ImagePtr[2] * ThirdFactor);
		OldYValue += 1 << (SCALING_LOG - 1);
		OldYValue >>= SCALING_LOG;
		if (OldYValue > 255)
			OldYValue = 255;
		int DiffYValue = (int)(*ImageBufferPtr) - OldYValue;
		if (DiffYValue < 0)
		{
			int NewVal0 = DiffYValue;
			NewVal0 += ImagePtr[0];
			if (NewVal0 < 0)
				NewVal0 = 0;
			ImagePtr[0] = (unsigned char)NewVal0;

			int NewVal1 = DiffYValue;
			NewVal1 += ImagePtr[1];
			if (NewVal1 < 0)
				NewVal1 = 0;
			ImagePtr[1] = (unsigned char)NewVal1;

			int NewVal2 = DiffYValue;
			NewVal2 += ImagePtr[2];
			if (NewVal2 < 0)
				NewVal2 = 0;
			ImagePtr[2] = (unsigned char)NewVal2;
		}
		else
		{
			int NewVal0 = DiffYValue;
			NewVal0 += ImagePtr[0];
			if (NewVal0 > 255)
				NewVal0 = 255;
			ImagePtr[0] = (unsigned char)NewVal0;

			int NewVal1 = DiffYValue;
			NewVal1 += ImagePtr[1];
			if (NewVal1 > 255)
				NewVal1 = 255;
			ImagePtr[1] = (unsigned char)NewVal1;

			int NewVal2 = DiffYValue;
			NewVal2 += ImagePtr[2];
			if (NewVal2 > 255)
				NewVal2 = 255;
			ImagePtr[2] = (unsigned char)NewVal2;
		}

		ImagePtr += PixelOffset;
		ImageBufferPtr++;
	}

	return AL_OK;
}


int CBaseAltaLuxFilter::ProcessRGB24(void* Image)
{
	return ProcessGeneric(Image, Y_RED_SCALE, Y_GREEN_SCALE, Y_BLUE_SCALE, 3);
}

int CBaseAltaLuxFilter::ProcessRGB32(void* Image)
{
	return ProcessGeneric(Image, Y_RED_SCALE, Y_GREEN_SCALE, Y_BLUE_SCALE, 4);
}

int CBaseAltaLuxFilter::ProcessBGR24(void* Image)
{
	return ProcessGeneric(Image, Y_BLUE_SCALE, Y_GREEN_SCALE, Y_RED_SCALE, 3);
}

int CBaseAltaLuxFilter::ProcessBGR32(void* Image)
{
	return ProcessGeneric(Image, Y_BLUE_SCALE, Y_GREEN_SCALE, Y_RED_SCALE, 4);
}

/// private methods


#ifdef _WIN64
	void FloatToInt(unsigned int *int_pointer, float f)
	{
		*int_pointer = (unsigned int)f;
	}
#else
__forceinline void FloatToInt(unsigned int* int_pointer, float f)
{
	__asm
	{
		fld f
		mov edx,int_pointer
		frndint
		fistp dword ptr[edx];
	}
}
#endif  // _WIN64

void CBaseAltaLuxFilter::ClipHistogram(unsigned int* pHistogram, unsigned int ClipLimit)
/* This function performs clipping of the histogram and redistribution of bins.
 * The histogram is clipped and the number of excess pixels is counted. Afterwards
 * the excess pixels are equally redistributed across the whole histogram (providing
 * the bin count is smaller than the cliplimit).
 */
{
	unsigned int *pulBinPointer, *pulEndPointer, *pulHisto;
	unsigned int ulNrExcess, ulUpper, ulBinIncr, ulStepSize, i;
	int lBinExcess;

	ulNrExcess = 0;
	pulBinPointer = pHistogram;
	for (i = 0; i < NUM_GRAY_LEVELS; i++)
	{
		/// calculate total number of excess pixels
		lBinExcess = (int)pulBinPointer[i] - ClipLimit;
		if (lBinExcess > 0)
			ulNrExcess += lBinExcess; //< excess in current bin
	}

	/// Second part: clip histogram and redistribute excess pixels in each bin
	ulBinIncr = ulNrExcess / NUM_GRAY_LEVELS; //< average binincrement
	ulUpper = ClipLimit - ulBinIncr; //< Bins larger than ulUpper set to cliplimit

	for (i = 0; i < NUM_GRAY_LEVELS; i++)
	{
		if (pHistogram[i] > ClipLimit)
			pHistogram[i] = ClipLimit; //< clip bin
		else
		{
			if (pHistogram[i] > ulUpper)
			{
				/// high bin count
				ulNrExcess -= pHistogram[i] - ulUpper;
				pHistogram[i] = ClipLimit;
			}
			else
			{
				/// low bin count
				ulNrExcess -= ulBinIncr;
				pHistogram[i] += ulBinIncr;
			}
		}
	}

	while (ulNrExcess)
	{
		/// Redistribute remaining excess
		pulEndPointer = &pHistogram[NUM_GRAY_LEVELS];
		pulHisto = pHistogram;

		while (ulNrExcess && pulHisto < pulEndPointer)
		{
			ulStepSize = NUM_GRAY_LEVELS / ulNrExcess;
			if (ulStepSize < 1)
				ulStepSize = 1; //< stepsize at least 1
			for (pulBinPointer = pulHisto; pulBinPointer < pulEndPointer && ulNrExcess; pulBinPointer += ulStepSize)
			{
				if (*pulBinPointer < ClipLimit)
				{
					(*pulBinPointer)++;
					ulNrExcess--; //< reduce excess
				}
			}
			pulHisto++; //< restart redistributing on other bin location
		}
	}
}

void CBaseAltaLuxFilter::MakeHistogram(PixelType* pImage, unsigned int* pHistogram)
/* This function classifies the greylevels present in the array image into
 * a greylevel histogram.
 */
{
	/// clear histogram
	memset(pHistogram, 0, sizeof(unsigned int) * NUM_GRAY_LEVELS);

	for (int i = 0; i < RegionHeight; i++)
	{
		PixelType* pImagePointer = &pImage[RegionWidth];
		while (pImage < pImagePointer)
			pHistogram[*pImage++]++;
		pImagePointer += OriginalImageWidth;
		pImage = &pImagePointer[-RegionWidth];
	}
}

void CBaseAltaLuxFilter::MapHistogram(unsigned int* pHistogram, unsigned int NumOfPixels)
/* This function calculates the equalized lookup table (mapping) by
 * cumulating the input histogram. Lookup table is rescaled in range [0..255].
 */
{
	unsigned int HistoSum = 0;
	const float Scale = ((float)MAX_GRAY_VALUE) / NumOfPixels;

	for (unsigned int i = 0; i < NUM_GRAY_LEVELS; i++)
	{
		HistoSum += pHistogram[i];
		unsigned int TargetValue;
		FloatToInt(&TargetValue, HistoSum * Scale);
		pHistogram[i] = min(MAX_GRAY_VALUE, TargetValue);
	}
}

void CBaseAltaLuxFilter::Interpolate(PixelType* pImage,
                                     unsigned int* pMapLeftUp, unsigned int* pMapRightUp,
                                     unsigned int* pMapLeftBottom, unsigned int* pMapRightBottom,
                                     unsigned int MatrixWidth, unsigned int MatrixHeight)
/* pImage		- pointer to input/output image
 * pMap*		- mappings of greylevels from histograms
 * MatrixWidth  - MatrixWidth of image submatrix
 * MatrixHeight - MatrixHeight of image submatrix
 * This function calculates the new greylevel assignments of pixels within a submatrix
 * of the image with size MatrixWidth and MatrixHeight. This is done by a bilinear interpolation
 * between four different mappings in order to eliminate boundary artifacts.
 */
{
	const unsigned int PtrIncr = OriginalImageWidth - MatrixWidth; //< pointer increment after processing row
	unsigned int MatrixArea = MatrixWidth * MatrixHeight; //< normalization factor

	if (MatrixArea & (MatrixArea - 1))
	{
		/// if MatrixArea is not a power of two, use division
		float FInvArea = 1.0f / MatrixArea;
		FInvArea *= 65536.0f;
		FInvArea *= 65536.0f;
		int InvMatrixArea = (int)FInvArea;
		// huge images
		for (unsigned int YCoef = 0, YInvCoef = MatrixHeight;
		     YCoef < MatrixHeight;
		     YCoef++, YInvCoef--, pImage += PtrIncr)
		{
			for (unsigned int XCoef = 0, XInvCoef = MatrixWidth;
			     XCoef < MatrixWidth;
			     XCoef++, XInvCoef--)
			{
				PixelType GreyValue = *pImage; //< get histogram bin value

				*pImage++ = (PixelType)((YInvCoef * (XInvCoef * pMapLeftUp[GreyValue]
						+ XCoef * pMapRightUp[GreyValue])
					+ YCoef * (XInvCoef * pMapLeftBottom[GreyValue]
						+ XCoef * pMapRightBottom[GreyValue])
					+ (MatrixArea >> 1)) / MatrixArea);
			}
		}
	}
	else
	{
		/// avoid the division and use a right shift instead
		unsigned int ShiftIndex = 0;
		while (MatrixArea >>= 1)
			ShiftIndex++; //< Calculate log2 of MatrixArea
		for (unsigned int YCoef = 0, YInvCoef = MatrixHeight;
		     YCoef < MatrixHeight;
		     YCoef++, YInvCoef--, pImage += PtrIncr)
		{
			for (unsigned int XCoef = 0, XInvCoef = MatrixWidth;
			     XCoef < MatrixWidth;
			     XCoef++, XInvCoef--)
			{
				PixelType GreyValue = *pImage; //< get histogram bin value
				*pImage++ = (PixelType)((YInvCoef * (XInvCoef * pMapLeftUp[GreyValue]
						+ XCoef * pMapRightUp[GreyValue])
					+ YCoef * (XInvCoef * pMapLeftBottom[GreyValue]
						+ XCoef * pMapRightBottom[GreyValue])) >> ShiftIndex);
			}
		}
	}
}

void CBaseAltaLuxFilter::CalcGraylevelMappings(int uiY, unsigned int ulClipLimit, unsigned int* pulMapArray)
{
	PixelType* pImage = (PixelType *)ImageBuffer;
	PixelType* pImPointer = pImage; //< pointer to image

	/// region pixel count
	unsigned int NumPixels = (unsigned int)RegionWidth * (unsigned int)RegionHeight; //< region pixel count

	/// Interpolate greylevel mappings to get CLAHE image
	for (int k = 0; k < uiY; k++)
	{
		if (k == 0)
			pImPointer += (RegionHeight >> 1) * OriginalImageWidth;
		else
			pImPointer += RegionHeight * OriginalImageWidth;
	}

	PixelType* SavedImPointer = pImPointer;

	if (uiY < NumVertRegions)
	{
		/// calculate greylevel mappings for each contextual region
		for (unsigned int uiX = 0; uiX < NumHorRegions; uiX++, pImPointer += RegionWidth)
		{
			unsigned int* pHistogram = &pulMapArray[NUM_GRAY_LEVELS * (uiY * NumHorRegions + uiX)];
			MakeHistogram(pImPointer, pHistogram);
			ClipHistogram(pHistogram, ulClipLimit);
			MapHistogram(pHistogram, NumPixels);
		}
	}
}

void CBaseAltaLuxFilter::ProcessRow(int uiY, unsigned int ulClipLimit, unsigned int* pulMapArray)
{
	PixelType* pImage = (PixelType *)ImageBuffer;

	unsigned int uiX; //< counters
	unsigned int uiSubX, uiSubY; //< size of subimages
	unsigned int uiXL, uiXR, uiYU, uiYB; //< auxiliary variables interpolation routine
	PixelType* pImPointer; //< pointer to image
	unsigned int *pulLU, *pulLB, *pulRU, *pulRB; //< auxiliary pointers interpolation

	/// region pixel count
	unsigned int NumPixels = (unsigned int)RegionWidth * (unsigned int)RegionHeight; //< region pixel count

	/// Interpolate greylevel mappings to get CLAHE image

	pImPointer = pImage;
	for (int k = 0; k < uiY; k++)
	{
		if (k == 0)
			pImPointer += (RegionHeight >> 1) * OriginalImageWidth;
		else
			pImPointer += RegionHeight * OriginalImageWidth;
	}

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

	for (uiX = 0; uiX <= NumHorRegions; uiX++)
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
		pulLU = &pulMapArray[NUM_GRAY_LEVELS * (uiYU * NumHorRegions + uiXL)];
		pulRU = &pulMapArray[NUM_GRAY_LEVELS * (uiYU * NumHorRegions + uiXR)];
		pulLB = &pulMapArray[NUM_GRAY_LEVELS * (uiYB * NumHorRegions + uiXL)];
		pulRB = &pulMapArray[NUM_GRAY_LEVELS * (uiYB * NumHorRegions + uiXR)];

		Interpolate(pImPointer, pulLU, pulRU, pulLB, pulRB, uiSubX, uiSubY);

		pImPointer += uiSubX; //< set pointer on next matrix
	}
}
