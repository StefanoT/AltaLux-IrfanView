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

// AltaLux.cpp : Defines the entry point for the DLL application.
//

#include "stdafx.h"
#include "AltaLux.h"
#include "resource.h"

#include <cstdio>
#include <cmath>
#include <malloc.h>
#include <Commctrl.h>

#include <memory>

#include "Filter/CBaseAltaLuxFilter.h"
#include "Filter/CAltaLuxFilterFactory.h"
#include "UIDraw/UIDraw.h"
#include "ScopedBitmapHeader.h"

#ifdef ENABLE_LOGGING
#define ELPP_NO_DEFAULT_LOG_FILE
#include "Log\easylogging++.h"
#include "Log\SetupLog.h"

#define ELPP_AS_DLL // Tells Easylogging++ that it's used for DLL
#define ELPP_EXPORT_SYMBOLS // Tells Easylogging++ to export symbols

INITIALIZE_EASYLOGGINGPP
#endif // ENABLE_LOGGING

const int RGB_PIXEL_SIZE = 3;

HINSTANCE hDll;
BITMAPINFOHEADER BmHdrCopy;
int ImageWidth;
int ImageHeight;
int FullImageWidth;
int FullImageHeight;
bool CroppedImage;
bool SkipProcessing;
int ScaledImageWidth;
int ScaledImageHeight;
int ScalingFactor = 1;
std::weak_ptr<unsigned char[]> SrcImagePtr;				// source image
std::weak_ptr<unsigned char[]> ProcImagePtr;			// processed image
std::weak_ptr<unsigned char[]> ScaledSrcImagePtr;		// down-sampled source image
std::weak_ptr<unsigned char[]> ScaledProcImagePtr;		// processed image
std::weak_ptr<unsigned char[]> ScaledProcImageGridMPtr;	// processed image with lesser intensity
std::weak_ptr<unsigned char[]> ScaledProcImageGridPPtr;	// processed image with higher intensity
std::weak_ptr<unsigned char[]> ScaledProcImageIntensityMPtr;	// processed image with coarser grid
std::weak_ptr<unsigned char[]> ScaledProcImageIntensityPPtr;	// processed image with finer grid
/// GUI
int FilterIntensity = AL_DEFAULT_STRENGTH;
int FilterScale = DEFAULT_HOR_REGIONS;
bool CompleteVisualization = true;

BOOL APIENTRY DllMain(HANDLE hModule,
                      DWORD ul_reason_for_call,
                      LPVOID lpReserved
)
{
	switch (ul_reason_for_call)
	{
	case DLL_PROCESS_ATTACH:
		{
			hDll = (HINSTANCE)hModule;
#ifdef ENABLE_LOGGING
		SetupLogging();
#endif
		}
	case DLL_THREAD_ATTACH:
	case DLL_THREAD_DETACH:
	case DLL_PROCESS_DETACH:
		break;
	}
	return TRUE;
}

void CopyScaledSrcImage(unsigned char* TargetImage)
{
	if (TargetImage == nullptr)
		return;
	std::shared_ptr<unsigned char[]> ScaledSrcImage = ScaledSrcImagePtr.lock();
	if (ScaledSrcImage == nullptr)
		return;
	memcpy(TargetImage, ScaledSrcImage.get(), ScaledImageWidth * ScaledImageHeight * RGB_PIXEL_SIZE);
}

void DoProcessing()
{
#ifdef ENABLE_LOGGING
	TIMED_FUNC(DoProcessingTimerObj);
#endif

	CBaseAltaLuxFilter* AltaLuxFilter = nullptr;
	bool IsRescalingEnabled = false;
	try
	{
		auto ScaledSrcImage = ScaledSrcImagePtr.lock();
		if (ScaledSrcImage != nullptr)
		{
			AltaLuxFilter = CAltaLuxFilterFactory::CreateAltaLuxFilter(ScaledImageWidth, ScaledImageHeight, FilterScale,
				FilterScale);
			IsRescalingEnabled = true;
		}
		else
			AltaLuxFilter = CAltaLuxFilterFactory::CreateAltaLuxFilter(ImageWidth, ImageHeight, FilterScale, FilterScale);
	}
	catch (std::exception& e)
	{
#ifdef ENABLE_LOGGING
		LOG(ERROR) << "Cannot create filter instance (77): " << e.what();
#endif
		AltaLuxFilter = nullptr;
	}

	if (AltaLuxFilter == nullptr)
		return;

	try
	{
#ifdef ENABLE_LOGGING
		TIMED_SCOPE(timerBlkObj, "Filter processing");
#endif
		std::unique_ptr<CBaseAltaLuxFilter> AltaLuxFilterPtr(AltaLuxFilter);
		if (IsRescalingEnabled)
		{
			// rescaling is enabled, so preview is computed on the smaller resampled image
			auto ScaledProcImage = ScaledProcImagePtr.lock();
			if (ScaledProcImage != nullptr)
			{
				CopyScaledSrcImage(ScaledProcImage.get());
				AltaLuxFilterPtr->SetStrength(FilterIntensity);
				AltaLuxFilterPtr->ProcessRGB24(static_cast<void *>(ScaledProcImage.get()));
			}
			// preview intensity changes
			const int STRENGTH_DELTA = 15;

			// preview with lesser intensity
			auto ScaledProcImageIntensityM = ScaledProcImageIntensityMPtr.lock();
			if (ScaledProcImageIntensityM != nullptr)
			{
				CopyScaledSrcImage(ScaledProcImageIntensityM.get());
				AltaLuxFilterPtr->SetStrength(max(FilterIntensity - STRENGTH_DELTA, AL_MIN_STRENGTH));
				AltaLuxFilterPtr->ProcessRGB24(static_cast<void *>(ScaledProcImageIntensityM.get()));
			}

			// preview with higher intensity
			auto ScaledProcImageIntensityP = ScaledProcImageIntensityPPtr.lock();
			if (ScaledProcImageIntensityP != nullptr)
			{
				CopyScaledSrcImage(ScaledProcImageIntensityP.get());
				AltaLuxFilterPtr->SetStrength(min(FilterIntensity + STRENGTH_DELTA, AL_MAX_STRENGTH));
				AltaLuxFilterPtr->ProcessRGB24(static_cast<void *>(ScaledProcImageIntensityP.get()));
			}

			// preview with grid changes
			const int SLICE_DELTA = 2;

			// preview with coarser grid
			auto ScaledProcImageGridM = ScaledProcImageGridMPtr.lock();
			if (ScaledProcImageGridM != nullptr)
			{
				CopyScaledSrcImage(ScaledProcImageGridM.get());
				AltaLuxFilterPtr->SetSlices(
					max(FilterScale - SLICE_DELTA, MIN_HOR_REGIONS), max(FilterScale - SLICE_DELTA, MIN_VERT_REGIONS));
				AltaLuxFilterPtr->ProcessRGB24(static_cast<void *>(ScaledProcImageGridM.get()));
			}

			// preview with finer grid
			auto ScaledProcImageGridP = ScaledProcImageGridPPtr.lock();
			if (ScaledProcImageGridP != nullptr)
			{
				CopyScaledSrcImage(ScaledProcImageGridP.get());
				AltaLuxFilterPtr->SetSlices(
					min(FilterScale + SLICE_DELTA, MAX_HOR_REGIONS), min(FilterScale + SLICE_DELTA, MAX_VERT_REGIONS));
				AltaLuxFilterPtr->ProcessRGB24(static_cast<void *>(ScaledProcImageGridP.get()));
			}
		}
		else
		{
			// full-scale view only
			auto SrcImage = SrcImagePtr.lock();
			auto ProcImage = ProcImagePtr.lock();
			if ((SrcImage != nullptr) && (ProcImage != nullptr))
			{
				memcpy(ProcImage.get(), SrcImage.get(), ImageWidth * ImageHeight * RGB_PIXEL_SIZE);
				AltaLuxFilterPtr->SetStrength(FilterIntensity);
				AltaLuxFilterPtr->ProcessRGB24(static_cast<void *>(ProcImage.get()));
			}
		}
	}
	catch (std::exception& e)
	{
#ifdef ENABLE_LOGGING
		LOG(ERROR) << "Exception running filter instance (114): " << e.what();
#endif
	}
}

void ScaleRect(RECT& RectToScale, int ScalingFactor)
{
	int RectWidth = RectToScale.right - RectToScale.left;
	int RectHeight = RectToScale.bottom - RectToScale.top;
	RectWidth = (RectWidth * ScalingFactor) / 100;
	RectHeight = (RectHeight * ScalingFactor) / 100;
	RectToScale.left = 0;
	RectToScale.top = 0;
	RectToScale.right = RectWidth;
	RectToScale.bottom = RectHeight;
}

/// <summary>
/// Down-samples incoming image for computing preview on smaller images
/// </summary>
/// <param name="SrcImage"></param>
/// <param name="SrcImageWidth"></param>
/// <param name="SrcImageHeight"></param>
/// <param name="DestImage"></param>
/// <param name="ScalingFactor">dest image width = source image width / ScalingFactor, same for height</param>
/// <remarks>Downsampling is computed with simple averaging as it is used only for previews</remarks>
void ScaleDownImage(void* SrcImage, int SrcImageWidth, int SrcImageHeight, void* DestImage, int ScalingFactor)
{
	if (SrcImage == nullptr)
		return;
	if (DestImage == nullptr)
		return;
	if (ScalingFactor == 1)
	{
		/// no rescaling
		memcpy(DestImage, SrcImage, SrcImageWidth * SrcImageHeight * 3);
		return;
	}

	auto DestImagePtr = static_cast<unsigned char *>(DestImage);
	auto SrcImagePtr = static_cast<unsigned char *>(SrcImage);
	const int SrcImageStride = SrcImageWidth * RGB_PIXEL_SIZE;
	const int DestImageWidth = SrcImageWidth / ScalingFactor;
	const int DestImageHeight = SrcImageHeight / ScalingFactor;
	unsigned char* DestPixelPtr = DestImagePtr;

	for (int y = 0; y < DestImageHeight; y++)
	{
		unsigned char* SrcPixelPtr = &SrcImagePtr[((y * ScalingFactor) * SrcImageWidth) * RGB_PIXEL_SIZE];
		for (int x = 0; x < DestImageWidth; x++)
		{
			unsigned int RAcc = 0;
			unsigned int GAcc = 0;
			unsigned int BAcc = 0;
			for (int iy = 0; iy < ScalingFactor; iy++)
				for (int ix = 0; ix < ScalingFactor; ix++)
				{
					RAcc += static_cast<unsigned int>(SrcPixelPtr[(iy * SrcImageStride) + (ix * RGB_PIXEL_SIZE)]);
					GAcc += static_cast<unsigned int>(SrcPixelPtr[(iy * SrcImageStride) + (ix * RGB_PIXEL_SIZE) + 1]);
					BAcc += static_cast<unsigned int>(SrcPixelPtr[(iy * SrcImageStride) + (ix * RGB_PIXEL_SIZE) + 2]);
				}
			if (ScalingFactor == 2)
			{
				DestPixelPtr[0] = static_cast<unsigned char>(RAcc >> 2);
				DestPixelPtr[1] = static_cast<unsigned char>(GAcc >> 2);
				DestPixelPtr[2] = static_cast<unsigned char>(BAcc >> 2);
			}
			else
			{
				DestPixelPtr[0] = static_cast<unsigned char>(RAcc / (ScalingFactor * ScalingFactor));
				DestPixelPtr[1] = static_cast<unsigned char>(GAcc / (ScalingFactor * ScalingFactor));
				DestPixelPtr[2] = static_cast<unsigned char>(BAcc / (ScalingFactor * ScalingFactor));
			}
			SrcPixelPtr += ScalingFactor * RGB_PIXEL_SIZE;
			DestPixelPtr += RGB_PIXEL_SIZE;
		}
	}
}

void ClearImageArea(HDC hdc, RECT& rectClient)
{
	HRGN bgRgn = CreateRectRgnIndirect(&rectClient);
	HBRUSH hBrush = CreateSolidBrush(RGB(0, 0, 0));
	FillRgn(hdc, bgRgn, hBrush);
}

void UpdateSliders(HWND hwnd)
{
	HWND hwndTrack = GetDlgItem(hwnd, IDC_SLIDER1);
	SendMessage(hwndTrack, TBM_SETPOS,
	            (WPARAM)TRUE, // redraw flag 
	            (LPARAM)FilterIntensity);
	hwndTrack = GetDlgItem(hwnd, IDC_SLIDER2);
	SendMessage(hwndTrack, TBM_SETPOS,
	            (WPARAM)TRUE, // redraw flag 
	            (LPARAM)FilterScale);
}

/// <summary>
/// repaint the GUI
/// </summary>
/// <param name="hwnd"></param>
void HandlePaintMessage(HWND hwnd)
{
	RECT rectClient;
	GetClientRect(hwnd, &rectClient);
	rectClient.right -= 100;

	PAINTSTRUCT ps;
	HDC hdc = BeginPaint(hwnd, &ps);
	try
	{
		ClearImageArea(hdc, rectClient);
		if (CompleteVisualization)
		{
			int SavedFilterScale = FilterScale;

			// draw original image
			auto ScaledSrcImage = SrcImagePtr.lock();
			if (ScaledSrcImage != nullptr)
			{
				RECT OriginalImageRect = rectClient;
				ScaleRect(OriginalImageRect, 31);
				DrawSingleImage(hdc, &BmHdrCopy, ScaledSrcImage.get(), ScaledImageWidth, ScaledImageHeight, OriginalImageRect, false,
					FilterScale);
			}

			// draw processed image with lesser intensity
			auto ScaledProcImageIntensityM = ScaledProcImageIntensityMPtr.lock();
			if (ScaledProcImageIntensityM != nullptr)
			{
				RECT IntensityMImageRect = rectClient;
				ScaleRect(IntensityMImageRect, 31);
				OffsetRect(&IntensityMImageRect, (RectWidth(rectClient) - RectWidth(IntensityMImageRect)) / 2, 0);
				DrawSingleImage(hdc, &BmHdrCopy, ScaledProcImageIntensityM.get(), ScaledImageWidth, ScaledImageHeight, IntensityMImageRect,
					false, FilterScale);
			}

			// draw processed image with higher intensity
			auto ScaledProcImageIntensityP = ScaledProcImageIntensityPPtr.lock();
			if (ScaledProcImageIntensityP != nullptr)
			{
				RECT IntensityPImageRect = rectClient;
				ScaleRect(IntensityPImageRect, 31);
				OffsetRect(&IntensityPImageRect, (RectWidth(rectClient) - RectWidth(IntensityPImageRect)) / 2,
					(RectHeight(rectClient) - RectHeight(IntensityPImageRect)));
				DrawSingleImage(hdc, &BmHdrCopy, ScaledProcImageIntensityP.get(), ScaledImageWidth, ScaledImageHeight, IntensityPImageRect,
					false, FilterScale);
			}

			// draw processed image with coarser grid
			auto ScaledProcImageGridM = ScaledProcImageGridMPtr.lock();
			if (ScaledProcImageGridM != nullptr)
			{
				RECT GridMImageRect = rectClient;
				ScaleRect(GridMImageRect, 31);
				FilterScale = max(SavedFilterScale - 2, MIN_HOR_REGIONS);
				OffsetRect(&GridMImageRect, 0, (RectHeight(rectClient) - RectHeight(GridMImageRect)) / 2);
				DrawSingleImage(hdc, &BmHdrCopy, ScaledProcImageGridM.get(), ScaledImageWidth, ScaledImageHeight, GridMImageRect, true,
					FilterScale);
			}

			// draw processed image with finer grid
			auto ScaledProcImageGridP = ScaledProcImageGridPPtr.lock();
			if (ScaledProcImageGridP != nullptr)
			{
				RECT GridPImageRect = rectClient;
				ScaleRect(GridPImageRect, 31);
				FilterScale = min(SavedFilterScale + 2, MAX_HOR_REGIONS);
				OffsetRect(&GridPImageRect, (RectWidth(rectClient) - RectWidth(GridPImageRect)),
					(RectHeight(rectClient) - RectHeight(GridPImageRect)) / 2);
				DrawSingleImage(hdc, &BmHdrCopy, ScaledProcImageGridP.get(), ScaledImageWidth, ScaledImageHeight, GridPImageRect, true,
					FilterScale);
			}

			FilterScale = SavedFilterScale;

			// draw processed image
			auto ScaledProcImage = ScaledProcImagePtr.lock();
			if (ScaledProcImage != nullptr)
			{
				RECT CentralImageRect = rectClient;
				ScaleRect(CentralImageRect, 55);
				OffsetRect(&CentralImageRect, (RectWidth(rectClient) - RectWidth(CentralImageRect)) / 2,
					(RectHeight(rectClient) - RectHeight(CentralImageRect)) / 2);
				DrawSingleImage(hdc, &BmHdrCopy, ScaledProcImage.get(), ScaledImageWidth, ScaledImageHeight, CentralImageRect, true,
					FilterScale);
			}
		}
		else
		{
			auto ScaledProcImage = ScaledProcImagePtr.lock();
			if (ScaledProcImage != nullptr)
			{
				// draw processed image only
				DrawSingleImage(hdc, &BmHdrCopy, ScaledProcImage.get(), ScaledImageWidth, ScaledImageHeight, rectClient, true,
					FilterScale);
			}
		}
	}
	catch (std::exception& e)
	{
#ifdef ENABLE_LOGGING
		LOG(ERROR) << "Exception handling paint message (263): " << e.what();
#endif
	}
	EndPaint(hwnd, &ps);
}

char SetupIniFile[1024];

BOOL CALLBACK DlgProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
	HBITMAP hBitmap;

	switch (msg)
	{
	case WM_INITDIALOG:
		{
			// Do stuff you need to init your dialogbox here	    
			HWND hwndBitmap = GetDlgItem(hwnd, IDC_SFONDO);
			SetWindowPos(hwndBitmap, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);

			HWND hwndTrack = GetDlgItem(hwnd, IDC_SLIDER1);
			SendMessage(hwndTrack, TBM_SETRANGE,
			            (WPARAM)TRUE, // redraw flag 
			            (LPARAM)MAKELONG(AL_MIN_STRENGTH, AL_MAX_STRENGTH)); // min. & max. positions 
			SendMessage(hwndTrack, TBM_SETPOS,
			            (WPARAM)TRUE, // redraw flag 
			            (LPARAM)FilterIntensity);

			hwndTrack = GetDlgItem(hwnd, IDC_SLIDER2);
			SendMessage(hwndTrack, TBM_SETRANGE,
			            (WPARAM)TRUE, // redraw flag 
			            (LPARAM)MAKELONG(MIN_HOR_REGIONS, MAX_HOR_REGIONS)); // min. & max. positions 
			SendMessage(hwndTrack, TBM_SETPOS,
			            (WPARAM)TRUE, // redraw flag 
			            (LPARAM)FilterScale);

			DoProcessing();
			InvalidateRgn(hwnd, nullptr, true);
			return TRUE;
		}
	case WM_LBUTTONDOWN:
		{
			int MouseXPos = LOWORD(lparam);
			int MouseYPos = HIWORD(lparam);

			RECT rectClient;
			GetClientRect(hwnd, &rectClient);
			rectClient.right -= 100;

			if ((CompleteVisualization) && (MouseXPos < rectClient.right))
			{
				const int ThirdWidth = RectWidth(rectClient) / 3;
				const int ThirdHeight = RectHeight(rectClient) / 3;
				auto ChangedSettings = false;
				if ((MouseXPos < ThirdWidth) && (MouseYPos > ThirdHeight) && (MouseYPos < (2 * ThirdHeight)))
				{
					FilterScale = max(FilterScale - 2, MIN_HOR_REGIONS);
					ChangedSettings = true;
				}
				if ((MouseXPos > (rectClient.right - ThirdWidth)) && (MouseYPos > ThirdHeight) && (MouseYPos < (2 * ThirdHeight)))
				{
					FilterScale = min(FilterScale + 2, MAX_HOR_REGIONS);
					ChangedSettings = true;
				}
				if ((MouseYPos < ThirdHeight) && (MouseXPos > ThirdWidth) && (MouseXPos < (2 * ThirdWidth)))
				{
					FilterIntensity = max(FilterIntensity - 15, AL_MIN_STRENGTH);
					ChangedSettings = true;
				}
				if ((MouseYPos > (rectClient.bottom - ThirdHeight)) && (MouseXPos > ThirdWidth) && (MouseXPos < (2 * ThirdWidth)))
				{
					FilterIntensity = min(FilterIntensity + 15, AL_MAX_STRENGTH);
					ChangedSettings = true;
				}
				if (ChangedSettings)
				{
					UpdateSliders(hwnd);
					DoProcessing();
					InvalidateRgn(hwnd, nullptr, true);
				}
			}
			return TRUE;
		}
	case WM_COMMAND:
		{
			switch (LOWORD(wparam))
			{
			case IDOK:
				{
					char FilterString[256];
					SkipProcessing = false;
					sprintf(FilterString, "%d", FilterIntensity);
					WritePrivateProfileStringA("AltaLux", "Intensity", FilterString, SetupIniFile);
					sprintf(FilterString, "%d", FilterScale);
					WritePrivateProfileStringA("AltaLux", "Scale", FilterString, SetupIniFile);
					EndDialog(hwnd, wparam);
					return TRUE;
				} // Check the rest of your dialog box controls here            
			case IDCANCEL:
				{
					SkipProcessing = true;
					EndDialog(hwnd, wparam);
					return TRUE;
				}
			case ID_DEFAULT:
				{
					FilterIntensity = AL_DEFAULT_STRENGTH;
					FilterScale = DEFAULT_HOR_REGIONS;
					UpdateSliders(hwnd);
					DoProcessing();
					InvalidateRgn(hwnd, nullptr, true);
					return TRUE;
				}
			case IDC_TOGGLEVISUALIZATION:
				{
					CompleteVisualization = !CompleteVisualization;
					InvalidateRgn(hwnd, nullptr, true);
					return TRUE;
				}
			}
		}
	case WM_VSCROLL:
		{
			HWND hwndTrack = (HWND)lparam;
			switch (LOWORD(wparam))
			{
			case TB_THUMBTRACK:
				{
					int dwPos = SendMessage(hwndTrack, TBM_GETPOS, 0, 0);
					if (hwndTrack == GetDlgItem(hwnd, IDC_SLIDER1))
						FilterIntensity = dwPos;
					if (hwndTrack == GetDlgItem(hwnd, IDC_SLIDER2))
						FilterScale = dwPos;
					break;
				}
			case TB_ENDTRACK:
				{
					int dwPos = SendMessage(hwndTrack, TBM_GETPOS, 0, 0);
					if (hwndTrack == GetDlgItem(hwnd, IDC_SLIDER1))
						FilterIntensity = dwPos;
					if (hwndTrack == GetDlgItem(hwnd, IDC_SLIDER2))
						FilterScale = dwPos;
					DoProcessing();
					InvalidateRgn(hwnd, nullptr, true);
				}
			}
			return TRUE;
		}
	case WM_PAINT:
		{
			HandlePaintMessage(hwnd);
			return FALSE;
		}
	default: return FALSE;
	}
	return TRUE;
}

unsigned char* AllocateRGBImage(int ImageWidth, int ImageHeight)
{
	const int SECURITY_PADDING = 4096;

	if ((ImageWidth <= 0) || (ImageHeight <= 0))
		return nullptr;

	unsigned char* AllocatedImage = nullptr;
	try
	{
		AllocatedImage = (unsigned char *)malloc((ImageWidth * ImageHeight * RGB_PIXEL_SIZE) + SECURITY_PADDING);
	}
	catch (std::exception& e)
	{
#ifdef ENABLE_LOGGING
		LOG(ERROR) << "Cannot create image buffer (427): " << e.what();
#endif
		AllocatedImage = nullptr;
	}
	return AllocatedImage;
}

/// <summary>
/// Computes the optimal scaling factor for images in preview
/// </summary>
/// <remarks>scaled image width must be a multiple of 8, if not a minor scaling factor is chosen</remarks>
void ComputeScalingFactor()
{
#ifdef ENABLE_LOGGING
	LOG(INFO) << "Calculating scaling factor (474)";
#endif
	int HorScaling = ImageWidth / 1000;
	int VerScaling = ImageHeight / 800;
	ScalingFactor = min(HorScaling, VerScaling);
	if (ScalingFactor < 1)
		ScalingFactor = 1;
	while (ScalingFactor > 1)
	{
		ScaledImageWidth = ImageWidth / ScalingFactor;
		ScaledImageHeight = ImageHeight / ScalingFactor;
		if ((ScaledImageWidth & 0x07) != 0)
			// fix for non-standard, multiple of 4 rescaled images that may not be drawn correctly
		{
#ifdef ENABLE_LOGGING
			LOG(INFO) << "Scaled image width " << ScaledImageWidth << " is not a multiple of 4";
#endif
			ScalingFactor--;
		}
		else
			break;
	}
#ifdef ENABLE_LOGGING
	LOG(INFO) << "Scaling factor is " << ScalingFactor;
#endif
	if (ScalingFactor > 1)
	{
		ScaledImageWidth = ImageWidth / ScalingFactor;
		ScaledImageHeight = ImageHeight / ScalingFactor;
#ifdef ENABLE_LOGGING
		LOG(INFO) << "Scaled image: width " << ScaledImageWidth << " height " << ScaledImageHeight;
#endif
	}
	else
	{
		ScaledImageWidth = ImageWidth;
		ScaledImageHeight = ImageHeight;
	}
}

void CopyToSourceImage(BYTE* ImageBits, DWORD ImageBitsStride, unsigned char* SrcImage, RECT ClipRect)
{
	// copy the image back into ImageBits
	if (CroppedImage)
	{
		unsigned char* SrcImagePtr = SrcImage;
		unsigned char* ImageBitsPtr = ImageBits;
		ImageBitsPtr += ClipRect.left * RGB_PIXEL_SIZE;
		ImageBitsPtr += ImageBitsStride * ClipRect.top;
		for (int y = ClipRect.top; y < ClipRect.bottom; y++)
		{
			memcpy(ImageBitsPtr, SrcImagePtr, ImageWidth * RGB_PIXEL_SIZE);
			SrcImagePtr += ImageWidth * RGB_PIXEL_SIZE;
			ImageBitsPtr += ImageBitsStride;
		}
	}
	else
	{
		unsigned char* SrcImagePtr = SrcImage;
		unsigned char* ImageBitsPtr = ImageBits;
		for (int y = 0; y < FullImageHeight; y++)
		{
			memcpy(ImageBitsPtr, SrcImagePtr, ImageWidth * RGB_PIXEL_SIZE);
			SrcImagePtr += ImageWidth * RGB_PIXEL_SIZE;
			ImageBitsPtr += ImageBitsStride;
		}
	}
}

void NormalizeClipRect(RECT& ClipRect)
{
	ClipRect.bottom += ClipRect.top;
	ClipRect.right += ClipRect.left;
	ClipRect.right = ClipRect.right & ~7;
	ClipRect.left = ClipRect.left & ~7;
	ClipRect.bottom = ClipRect.bottom & ~7;
	ClipRect.top = ClipRect.top & ~7;
	ImageWidth = ClipRect.right - ClipRect.left;
	ImageHeight = ClipRect.bottom - ClipRect.top;
}

void CopyFromSourceImage(unsigned char* SrcImage, RECT ClipRect, BYTE* ImageBits, DWORD ImageBitsStride)
{
	if (CroppedImage)
	{
		// copy only part of source image
		unsigned char* SrcImagePtr = SrcImage;
		unsigned char* ImageBitsPtr = ImageBits;
		ImageBitsPtr += ClipRect.left * RGB_PIXEL_SIZE;
		ImageBitsPtr += ImageBitsStride * ClipRect.top;
		for (int y = ClipRect.top; y < ClipRect.bottom; y++)
		{
			memcpy(SrcImagePtr, ImageBitsPtr, ImageWidth * RGB_PIXEL_SIZE);
			SrcImagePtr += ImageWidth * RGB_PIXEL_SIZE;
			ImageBitsPtr += ImageBitsStride;
		}
	}
	else
	{
		// copy whole source image
		unsigned char* SrcImagePtr = SrcImage;
		unsigned char* ImageBitsPtr = ImageBits;
		for (int y = 0; y < FullImageHeight; y++)
		{
			memcpy(SrcImagePtr, ImageBitsPtr, ImageWidth * RGB_PIXEL_SIZE);
			SrcImagePtr += ImageWidth * RGB_PIXEL_SIZE;
			ImageBitsPtr += ImageBitsStride;
		}
	}
}

bool IsCroppedImage()
{
	if ((FullImageWidth > ImageWidth) || (FullImageHeight > ImageHeight))
		return true;
	/// crop on non-standard size images
	if ((FullImageWidth & 7) != 0)
		return true;
	if ((FullImageHeight & 7) != 0)
		return true;
	return false;
}

/// <summary>
/// Processes the incoming bitmap with the AltaLux filter. Can directly process the image or open the GUI for
/// letting the user choose the parameters.
/// </summary>
/// <param name="hDib"></param>
/// <param name="hwnd"></param>
/// <param name="filter"></param>
/// <param name="rect"></param>
/// <param name="param1">if -1, open GUI</param>
/// <param name="param2">if -1, open GUI</param>
/// <param name="iniFile">file containing saved parameters</param>
/// <param name="szAppName"></param>
/// <param name="regID"></param>
/// <returns></returns>
bool __cdecl StartEffects2(HANDLE hDib, HWND hwnd, int filter, RECT rect, int param1, int param2, char* iniFile,
                           char* szAppName, int regID)
{
#define WIDTHBYTES(bits) (((bits) + 31) / 32 * 4)

	const int SECURITY_PADDING = 4096;
	std::shared_ptr<unsigned char[]> SrcImage;
	RECT ClipRect = rect;
	{
		ScopedBitmapHeader pbBmHdr(hDib);
		// TODO BmHdrCopy = pbBmHdr->;
		if (pbBmHdr->biPlanes != 1)
		{
#ifdef ENABLE_LOGGING
			LOG(ERROR) << "Only planar images are supported (478)";
#endif
			return false;
		}
		if (pbBmHdr->biBitCount != 24)
		{
#ifdef ENABLE_LOGGING
			LOG(ERROR) << "Only 24-bit images are supported (484)";
#endif
			return false;
		}
		FullImageWidth = abs(pbBmHdr->biWidth);
		FullImageHeight = abs(pbBmHdr->biHeight);
		/// ClipRect is actually (left, top, width, height) !
		ImageWidth = ClipRect.right;
		ImageHeight = ClipRect.bottom;

		CroppedImage = IsCroppedImage();
		if (CroppedImage)
		{
			NormalizeClipRect(ClipRect);
			BmHdrCopy.biWidth = ImageWidth;
			BmHdrCopy.biHeight = ImageHeight;
#ifdef ENABLE_LOGGING
			LOG(INFO) << "Cropped image: width " << ImageWidth << " height " << ImageHeight;
#endif
		}
		else
		{
#ifdef ENABLE_LOGGING
			LOG(INFO) << "Full image: width " << ImageWidth << " height " << ImageHeight;
#endif
		}

		BYTE* ImageBits = (BYTE *)&(*pbBmHdr) + (WORD)pbBmHdr->biSize;
		DWORD ImageBitsStride = WIDTHBYTES((DWORD)FullImageWidth * pbBmHdr->biBitCount);
		/// SrcImage
#ifdef ENABLE_LOGGING
		LOG(INFO) << "Allocating source image (553)";
#endif

		SrcImage = std::make_shared<unsigned char[]>((ImageWidth * ImageHeight * RGB_PIXEL_SIZE) + SECURITY_PADDING);
		if (SrcImage == nullptr)
		{
#ifdef ENABLE_LOGGING
			LOG(ERROR) << "Cannot create Source image (490): " << e.what();
#endif
			return false;
		}
		SrcImagePtr = SrcImage;

		// copy from ImageBits into SrcImage
#ifdef ENABLE_LOGGING
		LOG(INFO) << "Copying source image (568)";
#endif
		CopyFromSourceImage(SrcImage.get(), ClipRect, ImageBits, ImageBitsStride);
	}

	/// ProcImage
#ifdef ENABLE_LOGGING
	LOG(INFO) << "Allocating processed image (594)";
#endif
	std::shared_ptr<unsigned char[]> ProcImage(AllocateRGBImage(ImageWidth, ImageHeight));
	if (ProcImage == nullptr)
	{
#ifdef ENABLE_LOGGING
		LOG(ERROR) << "Cannot create processed image (598): " << e.what();
#endif
		return false;
	}
	ProcImagePtr = ProcImage;
#ifdef ENABLE_LOGGING
	LOG(INFO) << "Copying processed image (606)";
#endif
	memcpy(ProcImage.get(), SrcImage.get(), ImageWidth * ImageHeight * RGB_PIXEL_SIZE);

	/// param1 : [0..100], default 25
	/// param2 : [2..16], default 8
	if ((param1 == -1) || (param2 == -1))
	{
		// show GUI
		GlobalUnlock(hDib);

#ifdef ENABLE_LOGGING
		LOG(INFO) << "Loading saved settings (544)";
#endif
		strcpy(SetupIniFile, iniFile);
		FilterIntensity = GetPrivateProfileIntA("AltaLux", "Intensity", AL_DEFAULT_STRENGTH, SetupIniFile);
		FilterScale = GetPrivateProfileIntA("AltaLux", "Scale", DEFAULT_HOR_REGIONS, SetupIniFile);

		ComputeScalingFactor();

		auto ScaledSrcImage = std::make_shared<unsigned char[]>(AllocateRGBImage(ScaledImageWidth, ScaledImageHeight));
		if (ScaledSrcImage == nullptr)
			return false;
		ScaledSrcImagePtr = ScaledSrcImage;

		auto ScaledProcImage = std::make_shared<unsigned char[]>(AllocateRGBImage(ScaledImageWidth, ScaledImageHeight));
		if (ScaledProcImage == nullptr)
			return false;
		ScaledProcImagePtr = ScaledProcImage;

		// allocate buffer for processed image with coarser grid
		auto ScaledProcImageGridM = std::make_shared<unsigned char[]>(AllocateRGBImage(ScaledImageWidth, ScaledImageHeight));
		if (ScaledProcImageGridM == nullptr)
			return false;
		ScaledProcImageGridMPtr = ScaledProcImageGridM;

		// allocate buffer for processed image with finer grid
		auto ScaledProcImageGridP = std::make_shared<unsigned char[]>(AllocateRGBImage(ScaledImageWidth, ScaledImageHeight));
		if (ScaledProcImageGridP == nullptr)
			return false;
		ScaledProcImageGridPPtr = ScaledProcImageGridP;

		// allocate buffer for processed image with lesser intensity
		auto ScaledProcImageIntensityM = std::make_shared<unsigned char[]>(AllocateRGBImage(ScaledImageWidth, ScaledImageHeight));
		if (ScaledProcImageIntensityM == nullptr)
			return false;
		ScaledProcImageIntensityMPtr = ScaledProcImageIntensityM;

		// allocate buffer for processed image with higher intensity
		auto ScaledProcImageIntensityP = std::make_shared<unsigned char[]>(AllocateRGBImage(ScaledImageWidth, ScaledImageHeight));
		if (ScaledProcImageIntensityP == nullptr)
			return false;
		ScaledProcImageIntensityPPtr = ScaledProcImageIntensityP;

		ScaleDownImage(SrcImage.get(), ImageWidth, ImageHeight, ScaledSrcImage.get(), ScalingFactor);

		// copy scaled source image
		int ScaledImageSize = ScaledImageWidth * ScaledImageHeight * RGB_PIXEL_SIZE;
		memcpy(ScaledProcImage.get(), ScaledSrcImage.get(), ScaledImageSize);
		memcpy(ScaledProcImageGridM.get(), ScaledSrcImage.get(), ScaledImageSize);
		memcpy(ScaledProcImageGridP.get(), ScaledSrcImage.get(), ScaledImageSize);
		memcpy(ScaledProcImageIntensityM.get(), ScaledSrcImage.get(), ScaledImageSize);
		memcpy(ScaledProcImageIntensityP.get(), ScaledSrcImage.get(), ScaledImageSize);

		int ret = DialogBox(hDll, MAKEINTRESOURCE(IDD_DIALOG1), hwnd, (DLGPROC)DlgProc);

		if (ret == -1)
			return false;

		if (SkipProcessing)
			return true;

		param1 = FilterIntensity;
		param2 = FilterScale;
	}

	// if the filter parameters are unspecified, revert to default ones
	if (param1 < 0)
		param1 = AL_DEFAULT_STRENGTH;
	if (param2 < 0)
		param2 = DEFAULT_HOR_REGIONS;

	try
	{
		std::unique_ptr<CBaseAltaLuxFilter> AltaLuxFilter(
			CAltaLuxFilterFactory::CreateAltaLuxFilter(ImageWidth, ImageHeight, param2, param2));
		AltaLuxFilter->SetStrength(param1);
		{
#ifdef ENABLE_LOGGING
			TIMED_SCOPE(timerBlkObj, "Filter processing of full resolution image");
#endif
			AltaLuxFilter->ProcessRGB24(static_cast<void *>(SrcImage.get()));
		}

		ScopedBitmapHeader pbBmHdr(hDib);
		BYTE* ImageBits = (BYTE *)&(*pbBmHdr) + (WORD)pbBmHdr->biSize;
		DWORD ImageBitsStride = WIDTHBYTES((DWORD)FullImageWidth * pbBmHdr->biBitCount);
		CopyToSourceImage(ImageBits, ImageBitsStride, SrcImage.get(), ClipRect);
	}
	catch (std::exception& e)
	{
#ifdef ENABLE_LOGGING
		LOG(ERROR) << "Exception running filter instance (651): " << e.what();
#endif
	}
	return true;
}

/// <summary>
/// General information about the plug-in
/// </summary>
/// <param name="versionString">output buffer for version number</param>
/// <param name="fileFormats">output buffer for plug-in description</param>
/// <returns></returns>
int __cdecl GetPlugInInfo(char* versionString, char* fileFormats)
{
	sprintf(versionString, "1.08"); // your version-nr
	sprintf(fileFormats, "AltaLux image enhancement filter"); // some infos
	return 0;
}

bool __cdecl AltaLux_Effects(HANDLE hDib, HWND hwnd, int filter, RECT rect, int param1, int param2, char* iniFile,
                             char* szAppName, int regID)
{
	return StartEffects2(hDib, hwnd, filter, rect, param1, param2, iniFile, szAppName, regID);
}
