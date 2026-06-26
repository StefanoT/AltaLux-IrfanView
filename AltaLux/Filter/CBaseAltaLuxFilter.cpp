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

#include "..\Kernels\AltaLuxKernels.h"

namespace
{
	/// Pre-computed reciprocal table for multiplicative color scaling in InjectYComponent.
	/// table[oldY] = (1 << 16) / oldY in Q16 fixed-point; index 0 is unused
	/// (InjectYComponent branches on OldYValue == 0 to avoid division-by-zero).
	/// This 1 KiB 1D table replaces a 64 KiB 2D scale table: the per-pixel scale factor
	/// (newY << 8) / oldY is reconstructed on the fly with one multiply plus a shift,
	/// fits easily in L1, and is friendlier to vectorization (gather index has half the bits).
	/// Dynamic initialization of a namespace-scope const runs exactly once at DLL
	/// load, before any filter constructor can execute: no guards, no per-instance cost.
	struct RecipLUTTable
	{
		int table[256];
	};

	const RecipLUTTable g_RecipLUT = []() {
		RecipLUTTable lut{};
		for (int oldY = 1; oldY < 256; ++oldY)
		{
			lut.table[oldY] = (1 << 16) / oldY;
		}
		return lut;
	}();
}

#ifdef ENABLE_LOGGING
	#include "..\Log\easylogging++.h"
#endif // ENABLE_LOGGING

CBaseAltaLuxFilter::CBaseAltaLuxFilter(int Width, int Height, int HorSlices, int VerSlices)
{
	OriginalImageWidth = Width;
	OriginalImageHeight = Height;

	/// delay allocation of ImageBuffer into SetStrength
	ImageBuffer = nullptr;
	OriginalLumaBuffer = nullptr;
	KernelImplementationMode = AltaLuxKernels::GetBestSupportedImplementation();

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
		_aligned_free(ImageBuffer);
		ImageBuffer = nullptr;
	}
	if (OriginalLumaBuffer)
	{
		_aligned_free(OriginalLumaBuffer);
		OriginalLumaBuffer = nullptr;
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
	Strength = _Strength;
	if (Strength < AL_MIN_STRENGTH)
		Strength = AL_MIN_STRENGTH;
	if (Strength > AL_MAX_STRENGTH)
		Strength = AL_MAX_STRENGTH;

	if (Strength == AL_MIN_STRENGTH)
	{
		/// free ImageBuffer, if allocated
		if (ImageBuffer)
		{
			_aligned_free(ImageBuffer);
			ImageBuffer = nullptr;
		}
		if (OriginalLumaBuffer)
		{
			_aligned_free(OriginalLumaBuffer);
			OriginalLumaBuffer = nullptr;
		}
	}
	else
	{
		if (ImageBuffer == nullptr)
		{
			/// allocate ImageBuffer with 16-byte alignment for SIMD optimization
			ImageBuffer = (unsigned char*)_aligned_malloc(IMAGE_BUFFER_SIZE, 16);
			// _aligned_malloc returns nullptr on failure (doesn't throw exceptions)
			// Caller should check ImageBuffer != nullptr before use
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

void CBaseAltaLuxFilter::SetKernelImplementation(AltaLuxKernels::KernelImplementation implementation)
{
	KernelImplementationMode = implementation;
}

AltaLuxKernels::KernelImplementation CBaseAltaLuxFilter::GetKernelImplementation() const
{
	return KernelImplementationMode;
}

int CBaseAltaLuxFilter::ProcessUYVY(void* Image)
{
	if (Image == nullptr)
		return AL_NULL_IMAGE;

	if (!IsEnabled())
		return AL_OK;

	if (ImageBuffer == nullptr)
	{
		/// if ImageBuffer allocation failed in SetStrength, try again
		/// _aligned_malloc returns nullptr on failure (doesn't throw exceptions)
		ImageBuffer = (unsigned char*)_aligned_malloc(IMAGE_BUFFER_SIZE, 16);
		if (ImageBuffer == nullptr)
			return AL_OUT_OF_MEMORY;
	}

	auto ImagePtr = static_cast<unsigned char *>(Image);
	auto ImageBufferPtr = static_cast<unsigned char *>(ImageBuffer);
	const int ImageSize = ImageWidth * ImageHeight;
	const auto implementation = KernelImplementationMode;

	/// copy luma from UYVY into ImageBuffer
	/// UYVY: luma is the high byte of each 16-bit word
	AltaLuxKernels::ExtractPackedYUVLuma(ImagePtr, ImageBufferPtr, ImageSize,
		AltaLuxKernels::PackedYUVLumaPosition::HighByte, implementation);

	/// perform processing on ImageBuffer
	const int RunReturn = Run();
	if (RunReturn != AL_OK)
		return RunReturn;

	/// copy processed luma back into UYVY, preserving chroma
	/// chroma (U/V) lives in the low byte of each 16-bit word
	AltaLuxKernels::InjectPackedYUVLuma(ImagePtr, ImageBufferPtr, ImageSize,
		AltaLuxKernels::PackedYUVLumaPosition::HighByte, implementation);

	return AL_OK;
}

int CBaseAltaLuxFilter::ProcessVYUY(void* Image)
{
	return ProcessUYVY(Image); //< no operations are performed on chroma
}

int CBaseAltaLuxFilter::ProcessYUYV(void* Image)
{
	if (Image == nullptr)
		return AL_NULL_IMAGE;

	if (!IsEnabled())
		return AL_OK;

	if (ImageBuffer == nullptr)
	{
		/// if ImageBuffer allocation failed in SetStrength, try again
		/// _aligned_malloc returns nullptr on failure (doesn't throw exceptions)
		ImageBuffer = (unsigned char*)_aligned_malloc(IMAGE_BUFFER_SIZE, 16);
		if (ImageBuffer == nullptr)
			return AL_OUT_OF_MEMORY;
	}

	auto ImagePtr = static_cast<unsigned char *>(Image);
	auto ImageBufferPtr = static_cast<unsigned char *>(ImageBuffer);
	const int ImageSize = ImageWidth * ImageHeight;
	const auto implementation = KernelImplementationMode;

	/// copy luma from YUYV into ImageBuffer
	/// YUYV: luma is the low byte of each 16-bit word
	AltaLuxKernels::ExtractPackedYUVLuma(ImagePtr, ImageBufferPtr, ImageSize,
		AltaLuxKernels::PackedYUVLumaPosition::LowByte, implementation);

	/// perform processing on ImageBuffer
	const int RunReturn = Run();
	if (RunReturn != AL_OK)
		return RunReturn;

	/// copy processed luma back into YUYV, preserving chroma
	/// chroma (U/V) lives in the high byte of each 16-bit word
	AltaLuxKernels::InjectPackedYUVLuma(ImagePtr, ImageBufferPtr, ImageSize,
		AltaLuxKernels::PackedYUVLumaPosition::LowByte, implementation);

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

	// The input buffer is already 8bpp grayscale; point ImageBuffer at the caller's
	// buffer for the duration of Run(). The RAII guard restores ImageBuffer on every
	// exit path, so the destructor never accidentally _aligned_free()s caller memory.
	unsigned char* const SavedImageBuffer = ImageBuffer;
	ImageBuffer = static_cast<unsigned char *>(Image);
	struct RestoreGuard
	{
		unsigned char** slot;
		unsigned char* restore;
		~RestoreGuard() { *slot = restore; }
	} guard{ &ImageBuffer, SavedImageBuffer };

	return Run();
}

/// ITU-R BT.709 luma (also matches sRGB / Display P3 primaries):
///   Ey = 0.2126*Er + 0.7152*Eg + 0.0722*Eb
/// Modern JPEG/HEIF stills and HD/4K video from phones and cameras are authored
/// in this space. BT.601 (0.299 / 0.587 / 0.114) is retained only for legacy SD video.
const int SCALING_LOG = 15;
const int SCALING_FACTOR = (1 << SCALING_LOG);
const int Y_RED_SCALE = static_cast<int>(0.2126 * SCALING_FACTOR);
const int Y_GREEN_SCALE = static_cast<int>(0.7152 * SCALING_FACTOR);
const int Y_BLUE_SCALE = static_cast<int>(0.0722 * SCALING_FACTOR);

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

	// Early return if filter is disabled (Strength = 0)
	// Avoids unnecessary processing and memory allocation
	if (!IsEnabled())
		return AL_OK;

	if (ImageBuffer == nullptr)
	{
		/// if ImageBuffer allocation failed in SetStrength, try again
		/// _aligned_malloc returns nullptr on failure (doesn't throw exceptions)
		ImageBuffer = (unsigned char*)_aligned_malloc(IMAGE_BUFFER_SIZE, 16);
		if (ImageBuffer == nullptr)
			return AL_OUT_OF_MEMORY;
	}
	if (OriginalLumaBuffer == nullptr)
	{
		OriginalLumaBuffer = (unsigned char*)_aligned_malloc(IMAGE_BUFFER_SIZE, 16);
		if (OriginalLumaBuffer == nullptr)
			return AL_OUT_OF_MEMORY;
	}

	ExtractYComponent(Image, FirstFactor, SecondFactor, ThirdFactor, PixelOffset);
	const int numPixels = OriginalImageWidth * OriginalImageHeight;
	memcpy(OriginalLumaBuffer, ImageBuffer, numPixels);

	/// perform processing on ImageBuffer
	int RunReturn = Run();
	if (RunReturn != AL_OK)
		return RunReturn;

	InjectYComponent(Image, FirstFactor, SecondFactor, ThirdFactor, PixelOffset, OriginalLumaBuffer);

	return AL_OK;
}

/// <summary>
/// Extraction of Y (luminance) component from RGB image
/// </summary>
/// <remarks>
/// Performance improvements:
/// - Optional SIMD for 2-4x speedup
/// </remarks>
void CBaseAltaLuxFilter::ExtractYComponent(void* Image, int FirstFactor,
	int SecondFactor, int ThirdFactor,
	int PixelOffset)
{
	unsigned char* ImagePtr = static_cast<unsigned char*>(Image);
	unsigned char* ImageBufferPtr = static_cast<unsigned char*>(ImageBuffer);
	const int numPixels = OriginalImageWidth * OriginalImageHeight;
	AltaLuxKernels::ExtractRGBLuma(ImagePtr, ImageBufferPtr, numPixels, PixelOffset,
		FirstFactor, SecondFactor, ThirdFactor, SCALING_LOG,
		KernelImplementationMode);
}

/// <summary>
/// Injects processed Y component back into RGB image using multiplicative scaling
/// </summary>
/// <remarks>
/// Uses multiplicative scaling to preserve color ratios (hue and saturation).
/// R' = R × (Y_new / Y_old) preserves color perfectly.
/// Lookup table eliminates per-pixel division.
/// </remarks>
void CBaseAltaLuxFilter::InjectYComponent(void* Image,
                                          int FirstFactor, int SecondFactor,
                                          int ThirdFactor, int PixelOffset,
                                          const unsigned char* OriginalLuma)
{
	unsigned char* ImagePtr = static_cast<unsigned char*>(Image);
	unsigned char* ImageBufferPtr = static_cast<unsigned char*>(ImageBuffer);
	const int numPixels = OriginalImageWidth * OriginalImageHeight;
	AltaLuxKernels::InjectRGBLumaWithOriginalLuma(ImagePtr, ImageBufferPtr, OriginalLuma,
		numPixels, PixelOffset, g_RecipLUT.table, KernelImplementationMode);
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
/// <summary>
/// Performs clipping of the histogram and redistribution of bins
/// </summary>
/// <param name="pHistogram">Pointer to histogram array to be clipped (NUM_GRAY_LEVELS elements)</param>
/// <param name="ClipLimit">Maximum allowed bin count</param>
/// <remarks>
/// The histogram is clipped and the number of excess pixels is counted. Afterwards
/// the excess pixels are equally redistributed across the whole histogram (providing
/// the bin count is smaller than the cliplimit). This prevents over-amplification
/// of noise in uniform regions.
/// </remarks>
void CBaseAltaLuxFilter::ClipHistogram(unsigned int* pHistogram, unsigned int ClipLimit)
{
	AltaLuxKernels::ClipHistogram(pHistogram, ClipLimit, KernelImplementationMode);
}

/// <summary>
/// Classifies the greylevels present in the image array into a greylevel histogram
/// </summary>
/// <param name="pImage">Pointer to top-left corner of image tile</param>
/// <param name="pHistogram">Output histogram array (NUM_GRAY_LEVELS elements)</param>
/// <remarks>
/// Only processes RegionWidth × RegionHeight area starting from pImage pointer
/// </remarks>
void CBaseAltaLuxFilter::MakeHistogram(PixelType* pImage, unsigned int* pHistogram)
{
	AltaLuxKernels::MakeHistogram(pImage, OriginalImageWidth, RegionWidth, RegionHeight,
		pHistogram, KernelImplementationMode);
}

/// <summary>
/// Calculates the equalized lookup table (mapping) by cumulating the input histogram
/// </summary>
/// <param name="pHistogram">Input/output histogram array (modified in-place, NUM_GRAY_LEVELS elements)</param>
/// <param name="NumOfPixels">Total number of pixels in the tile</param>
/// <remarks>
/// The lookup table is rescaled to range [0..255]. Each bin becomes the cumulative
/// sum up to that gray level, normalized to the output range.
/// </remarks>
void CBaseAltaLuxFilter::MapHistogram(unsigned int* pHistogram, unsigned int NumOfPixels)
{
	AltaLuxKernels::MapHistogram(pHistogram, NumOfPixels, KernelImplementationMode);
}

/// <summary>
/// Calculates new greylevel assignments for pixels within a submatrix using bilinear interpolation
/// </summary>
/// <param name="pImage">Pointer to input/output image region</param>
/// <param name="pMapLeftUp">Mapping of greylevels from upper-left tile histogram</param>
/// <param name="pMapRightUp">Mapping of greylevels from upper-right tile histogram</param>
/// <param name="pMapLeftBottom">Mapping of greylevels from lower-left tile histogram</param>
/// <param name="pMapRightBottom">Mapping of greylevels from lower-right tile histogram</param>
/// <param name="MatrixWidth">Width of image submatrix to interpolate</param>
/// <param name="MatrixHeight">Height of image submatrix to interpolate</param>
/// <remarks>
/// This function calculates the new greylevel assignments of pixels within a submatrix
/// of the image with size MatrixWidth and MatrixHeight. This is done by a bilinear interpolation
/// between four different mappings in order to eliminate boundary artifacts.
/// Each pixel value is weighted by its distance to neighboring tiles.
/// </remarks>
void CBaseAltaLuxFilter::Interpolate(PixelType* pImage,
                                     unsigned int* pMapLeftUp, unsigned int* pMapRightUp,
                                     unsigned int* pMapLeftBottom, unsigned int* pMapRightBottom,
                                     unsigned int MatrixWidth, unsigned int MatrixHeight)
{
	AltaLuxKernels::Interpolate(pImage, OriginalImageWidth,
		pMapLeftUp, pMapRightUp, pMapLeftBottom, pMapRightBottom,
		MatrixWidth, MatrixHeight, KernelImplementationMode);
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

	if (static_cast<unsigned int>(uiY) < NumVertRegions)
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
