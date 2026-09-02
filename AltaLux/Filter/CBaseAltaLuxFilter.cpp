/*
Project: AltaLux plugin for IrfanView
Author: Stefano Tommesani
Website: http://www.tommesani.com

Microsoft Public License (MS-PL) [OSI Approved License]
The full license text is in the LICENSE file at the root of the repository.
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
		// Retry allocation here because SetStrength may have failed earlier.
		ImageBuffer = (unsigned char*)_aligned_malloc(IMAGE_BUFFER_SIZE, 16);
		if (ImageBuffer == nullptr)
			return AL_OUT_OF_MEMORY;
	}

	auto ImagePtr = static_cast<unsigned char*>(Image);
	auto ImageBufferPtr = static_cast<unsigned char*>(ImageBuffer);
	const int ImageSize = ImageWidth * ImageHeight;
	const auto implementation = KernelImplementationMode;

	// UYVY stores luma in the high byte of each 16-bit word.
	AltaLuxKernels::ExtractPackedYUVLuma(ImagePtr, ImageBufferPtr, ImageSize,
		AltaLuxKernels::PackedYUVLumaPosition::HighByte, implementation);

	// Process ImageBuffer in-place.
	const int RunReturn = Run();
	if (RunReturn != AL_OK)
		return RunReturn;

	// Copy processed luma back while preserving chroma in the low bytes.
	AltaLuxKernels::InjectPackedYUVLuma(ImagePtr, ImageBufferPtr, ImageSize,
		AltaLuxKernels::PackedYUVLumaPosition::HighByte, implementation);

	return AL_OK;
}

int CBaseAltaLuxFilter::ProcessVYUY(void* Image)
{
	return ProcessUYVY(Image); // Chroma is preserved by the UYVY path.
}

int CBaseAltaLuxFilter::ProcessYUYV(void* Image)
{
	if (Image == nullptr)
		return AL_NULL_IMAGE;

	if (!IsEnabled())
		return AL_OK;

	if (ImageBuffer == nullptr)
	{
		// Retry allocation here because SetStrength may have failed earlier.
		ImageBuffer = (unsigned char*)_aligned_malloc(IMAGE_BUFFER_SIZE, 16);
		if (ImageBuffer == nullptr)
			return AL_OUT_OF_MEMORY;
	}

	auto ImagePtr = static_cast<unsigned char*>(Image);
	auto ImageBufferPtr = static_cast<unsigned char*>(ImageBuffer);
	const int ImageSize = ImageWidth * ImageHeight;
	const auto implementation = KernelImplementationMode;

	// YUYV stores luma in the low byte of each 16-bit word.
	AltaLuxKernels::ExtractPackedYUVLuma(ImagePtr, ImageBufferPtr, ImageSize,
		AltaLuxKernels::PackedYUVLumaPosition::LowByte, implementation);

	// Process ImageBuffer in-place.
	const int RunReturn = Run();
	if (RunReturn != AL_OK)
		return RunReturn;

	// Copy processed luma back into YUYV, preserving chroma in the high bytes.
	AltaLuxKernels::InjectPackedYUVLuma(ImagePtr, ImageBufferPtr, ImageSize,
		AltaLuxKernels::PackedYUVLumaPosition::LowByte, implementation);

	return AL_OK;
}

int CBaseAltaLuxFilter::ProcessYVYU(void* Image)
{
	return ProcessYUYV(Image); // Chroma is preserved by the YUYV path.
}

int CBaseAltaLuxFilter::ProcessGray(void* Image)
{
	if (Image == nullptr)
		return AL_NULL_IMAGE;

	// The input buffer is already 8bpp grayscale; point ImageBuffer at the caller's
	// buffer for the duration of Run(). The RAII guard restores ImageBuffer on every
	// exit path, so the destructor never accidentally _aligned_free()s caller memory.
	unsigned char* const SavedImageBuffer = ImageBuffer;
	ImageBuffer = static_cast<unsigned char*>(Image);
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

int CBaseAltaLuxFilter::ProcessGeneric(void* Image, int FirstFactor, int SecondFactor,
                                       int ThirdFactor, int PixelOffset)
{
	if (Image == nullptr)
		return AL_NULL_IMAGE;

	// Avoid work and allocation when strength is zero.
	if (!IsEnabled())
		return AL_OK;

	if (ImageBuffer == nullptr)
	{
		// Retry allocation here because SetStrength may have failed earlier.
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

	// Process ImageBuffer in-place.
	int RunReturn = Run();
	if (RunReturn != AL_OK)
		return RunReturn;

	InjectYComponent(Image, PixelOffset, OriginalLumaBuffer);

	return AL_OK;
}

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

// Reapply processed luma by scaling RGB channels from the original luma.
// The reciprocal table avoids a per-pixel divide in scalar and SIMD kernels.
void CBaseAltaLuxFilter::InjectYComponent(void* Image, int PixelOffset,
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

void CBaseAltaLuxFilter::ClipHistogram(unsigned int* pHistogram, unsigned int ClipLimit)
{
	AltaLuxKernels::ClipHistogram(pHistogram, ClipLimit, KernelImplementationMode);
}

void CBaseAltaLuxFilter::MakeHistogram(PixelType* pImage, unsigned int* pHistogram)
{
	AltaLuxKernels::MakeHistogram(pImage, OriginalImageWidth, RegionWidth, RegionHeight,
		pHistogram, KernelImplementationMode);
}

void CBaseAltaLuxFilter::MapHistogram(unsigned int* pHistogram, unsigned int NumOfPixels)
{
	AltaLuxKernels::MapHistogram(pHistogram, NumOfPixels, KernelImplementationMode);
}

void CBaseAltaLuxFilter::Interpolate(PixelType* pImage,
                                     unsigned int* pMapLeftUp, unsigned int* pMapRightUp,
                                     unsigned int* pMapLeftBottom, unsigned int* pMapRightBottom,
                                     unsigned int MatrixWidth, unsigned int MatrixHeight)
{
	AltaLuxKernels::Interpolate(pImage, OriginalImageWidth,
		pMapLeftUp, pMapRightUp, pMapLeftBottom, pMapRightBottom,
		MatrixWidth, MatrixHeight, KernelImplementationMode);
}
