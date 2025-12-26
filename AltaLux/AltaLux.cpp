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
#include <vector>

#include "Filter/CBaseAltaLuxFilter.h"
#include "Filter/CAltaLuxFilterFactory.h"
#include "UIDraw/UIDraw.h"
#include "ScopedBitmapHeader.h"
#include <iostream>

#include <dwmapi.h>
#pragma comment(lib, "Dwmapi.lib")

#include <Uxtheme.h>
#include <vssym32.h>

#pragma comment(lib, "uxtheme.lib")

using WeakImagePtr = std::weak_ptr<std::vector<unsigned char>>;
using SharedImagePtr = std::shared_ptr<std::vector<unsigned char>>;

const int RGB24_PIXEL_SIZE = 3;
const int RGB32_PIXEL_SIZE = 4;

HINSTANCE hDll;
BITMAPINFOHEADER BmHdrCopy;
int ImageWidth;
int ImageHeight;
int ImageBitDepth;
int FullImageWidth;
int FullImageHeight;
bool CroppedImage;
bool SkipProcessing;
int ScaledImageWidth;
int ScaledImageHeight;
int ScalingFactor = 1;
WeakImagePtr SrcImagePtr;				// source image
WeakImagePtr ProcImagePtr;				// processed image
WeakImagePtr ScaledSrcImagePtr;			// down-sampled source image
WeakImagePtr ScaledProcImagePtr;		// processed image
WeakImagePtr ScaledProcImageGridMPtr;	// processed image with lesser intensity
WeakImagePtr ScaledProcImageGridPPtr;	// processed image with higher intensity
WeakImagePtr ScaledProcImageIntensityMPtr;	// processed image with coarser grid
WeakImagePtr ScaledProcImageIntensityPPtr;	// processed image with finer grid
/// GUI
int FilterIntensity = AL_DEFAULT_STRENGTH;
int FilterScale = DEFAULT_HOR_REGIONS;
bool CompleteVisualization = true;
bool NoZoom = false;

/// <summary>
/// DLL entry point called by Windows when the DLL is loaded or unloaded.
/// Stores the module handle for later use by the plugin.
/// </summary>
/// <param name="hModule">Handle to the DLL module</param>
/// <param name="ul_reason_for_call">Reason for calling function (attach/detach)</param>
/// <param name="lpReserved">Reserved parameter</param>
/// <returns>TRUE if successful, FALSE otherwise</returns>
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
			break;
		}
	case DLL_THREAD_ATTACH:
	case DLL_THREAD_DETACH:
	case DLL_PROCESS_DETACH:
		break;
	}
	return TRUE;
}

/// <summary>
/// Copies the scaled source image data to the target buffer.
/// Used to initialize preview buffers before processing.
/// </summary>
/// <param name="TargetImage">Destination buffer to receive the scaled source image data</param>
void CopyScaledSrcImage(unsigned char* TargetImage)
{
	if (TargetImage == nullptr)
		return;
	auto ScaledSrcImage = ScaledSrcImagePtr.lock();
	if (ScaledSrcImage == nullptr)
		return;
	memcpy(TargetImage, ScaledSrcImage.get()->data(), ScaledImageWidth * ScaledImageHeight * ImageBitDepth);
}

/// <summary>
/// Creates and returns an instance of CBaseAltaLuxFilter based on image dimensions.
/// The function determines whether to use a scaled or full-size image based on the availability of the scaled source image. 
/// </summary>
/// <param name="IsRescalingEnabled">A reference to a boolean that will be set to true if scaling is enabled (i.e., the scaled image was used to create the filter), or false if the full-size image was used.</param>
/// <returns>A pointer to an instance of the AltaLux filter if successful, nullptr otherwise.</returns>
std::unique_ptr<CBaseAltaLuxFilter> InstantiateFilter(bool& IsRescalingEnabled)
{
	try
	{		
		IsRescalingEnabled = ScaledSrcImagePtr.lock() != nullptr;
		CBaseAltaLuxFilter* rawFilter = nullptr;
		if (IsRescalingEnabled)		
		{
			rawFilter = CAltaLuxFilterFactory::CreateAltaLuxFilter(ScaledImageWidth, ScaledImageHeight, FilterScale, FilterScale);			
		}
		else
		{
			rawFilter = CAltaLuxFilterFactory::CreateAltaLuxFilter(ImageWidth, ImageHeight, FilterScale, FilterScale);			
		}
		return std::unique_ptr<CBaseAltaLuxFilter>(rawFilter);
	}
	catch (const std::exception& e)
	{
	#ifdef ENABLE_LOGGING
		std::cerr << "Exception: " << e.what() << std::endl;
	#else
		(void)e;  // Suppress unused variable warning
	#endif
		return nullptr;
	}
}

/// <summary>
/// Processes the current image with the AltaLux filter using current settings.
/// Generates multiple preview variants with different intensity and grid scale values.
/// If rescaling is enabled, processes the downsampled image; otherwise processes the full-size image.
/// </summary>
void DoProcessing()
{
	bool IsRescalingEnabled = false;
	auto AltaLuxFilterPtr = InstantiateFilter(IsRescalingEnabled);
	if (!AltaLuxFilterPtr)
		return;

	try
	{
		auto processImage = [&](const SharedImagePtr image) 
		{
			switch (ImageBitDepth) 
			{
				case RGB24_PIXEL_SIZE:
					AltaLuxFilterPtr->ProcessRGB24(static_cast<void*>(image.get()->data()));
					break;
				case RGB32_PIXEL_SIZE:
					AltaLuxFilterPtr->ProcessRGB32(static_cast<void*>(image.get()->data()));
					break;
				default:
					break;
			}
		};

		if (IsRescalingEnabled)
		{
			// rescaling is enabled, so preview is computed on the smaller resampled image
			auto ScaledProcImage = ScaledProcImagePtr.lock();
			if (ScaledProcImage != nullptr)
			{
				CopyScaledSrcImage(ScaledProcImage.get()->data());
				AltaLuxFilterPtr->SetStrength(FilterIntensity);
				processImage(ScaledProcImage);
			}
			// preview intensity changes
			const int STRENGTH_DELTA = 15;

			// preview with lesser intensity
			auto ScaledProcImageIntensityM = ScaledProcImageIntensityMPtr.lock();
			if (ScaledProcImageIntensityM != nullptr)
			{
				CopyScaledSrcImage(ScaledProcImageIntensityM.get()->data());
				AltaLuxFilterPtr->SetStrength(max(FilterIntensity - STRENGTH_DELTA, AL_MIN_STRENGTH));
				processImage(ScaledProcImageIntensityM);
			}

			// preview with higher intensity
			auto ScaledProcImageIntensityP = ScaledProcImageIntensityPPtr.lock();
			if (ScaledProcImageIntensityP != nullptr)
			{
				CopyScaledSrcImage(ScaledProcImageIntensityP.get()->data());
				AltaLuxFilterPtr->SetStrength(min(FilterIntensity + STRENGTH_DELTA, AL_MAX_STRENGTH));
				processImage(ScaledProcImageIntensityP);
			}

			// preview with grid changes
			const int SLICE_DELTA = 2;

			// preview with coarser grid
			auto ScaledProcImageGridM = ScaledProcImageGridMPtr.lock();
			if (ScaledProcImageGridM != nullptr)
			{
				CopyScaledSrcImage(ScaledProcImageGridM.get()->data());
				AltaLuxFilterPtr->SetSlices(
					max(FilterScale - SLICE_DELTA, MIN_HOR_REGIONS), max(FilterScale - SLICE_DELTA, MIN_VERT_REGIONS));
				processImage(ScaledProcImageGridM);
			}

			// preview with finer grid
			auto ScaledProcImageGridP = ScaledProcImageGridPPtr.lock();
			if (ScaledProcImageGridP != nullptr)
			{
				CopyScaledSrcImage(ScaledProcImageGridP.get()->data());
				AltaLuxFilterPtr->SetSlices(
					min(FilterScale + SLICE_DELTA, MAX_HOR_REGIONS), min(FilterScale + SLICE_DELTA, MAX_VERT_REGIONS));
				processImage(ScaledProcImageGridP);
			}
		}
		else
		{
			// full-scale view only
			auto SrcImage = SrcImagePtr.lock();
			auto ProcImage = ProcImagePtr.lock();
			if ((SrcImage != nullptr) && (ProcImage != nullptr))
			{
				memcpy(ProcImage.get()->data(), SrcImage.get()->data(), ImageWidth * ImageHeight * ImageBitDepth);
				AltaLuxFilterPtr->SetStrength(FilterIntensity);
				processImage(ProcImage);
			}
		}
	}
	catch (const std::exception& e) {
	#ifdef ENABLE_LOGGING
		std::cerr << "Exception: " << e.what() << std::endl;
	#else
		(void)e;  // Suppress unused variable warning
	#endif
	}
}

/// <summary>
/// Modifies the provided rectangle, scaling its width and height by the provided scaling factor, expressed as a percentage. 
/// The scaled rectangle is repositioned to start at the origin(0, 0).
/// </summary>
/// <param name="RectToScale">The RECT structure representing the rectangle to be scaled</param>
/// <param name=" ScalingFactor">The scaling factor, expressed as a percentage. A factor of 100 means no scaling (100%), 50 means the rectangle is reduced to half its original size, and 200 doubles the size.</param>
void ScaleRect(RECT& RectToScale, int ScalingFactor)
{
	int originalWidth = RectToScale.right - RectToScale.left;
	int originalHeight = RectToScale.bottom - RectToScale.top;
	RectToScale.left = 0;
	RectToScale.top = 0;
	RectToScale.right = (originalWidth * ScalingFactor) / 100;
	RectToScale.bottom = (originalHeight * ScalingFactor) / 100;
}

/// <summary>
/// Down-samples the source image into the destination image using simple averaging.
/// Supports only 24-bit or 32-bit RGB images.
/// </summary>
/// <param name="src">Pointer to source image data</param>
/// <param name="srcWidth">Width of the source image</param>
/// <param name="srcHeight">Height of the source image</param>
/// <param name="dest">Pointer to destination image data</param>
/// <param name="scalingFactor">Factor by which to scale down (integer >= 1)</param>
/// <param name="bitDepth">Bytes per pixel (3 or 4)</param>
/// <remarks>Downsampling is computed with simple averaging as it is used only for previews</remarks>
void ScaleDownImage(unsigned char* SrcImage, const int SrcImageWidth, const int SrcImageHeight, unsigned char* DestImage, const int ScalingFactor, const int BitDepth)
{
	if (!SrcImage || !DestImage || ScalingFactor <= 0 || (BitDepth != 3 && BitDepth != 4))
		return;

	if (ScalingFactor == 1)
	{
		/// no rescaling
		memcpy(DestImage, SrcImage, SrcImageWidth * SrcImageHeight * BitDepth);
		return;
	}

	auto DestImagePtr = static_cast<unsigned char *>(DestImage);
	auto SrcImagePtr = static_cast<unsigned char *>(SrcImage);
	const int SrcImageStride = SrcImageWidth * BitDepth;
	const int DestImageWidth = SrcImageWidth / ScalingFactor;
	const int DestImageHeight = SrcImageHeight / ScalingFactor;
	unsigned char* DestPixelPtr = DestImagePtr;
	const int ScalingArea = ScalingFactor * ScalingFactor;

	for (int y = 0; y < DestImageHeight; y++)
	{
		unsigned char* SrcPixelPtr = &SrcImagePtr[((y * ScalingFactor) * SrcImageWidth) * BitDepth];
		for (int x = 0; x < DestImageWidth; x++)
		{
			unsigned int RAcc = 0;
			unsigned int GAcc = 0;
			unsigned int BAcc = 0;
			for (int iy = 0; iy < ScalingFactor; iy++)
			{
				int SrcPixelIndex = iy * SrcImageStride;
				for (int ix = 0; ix < ScalingFactor; ix++)
				{
					RAcc += static_cast<unsigned int>(SrcPixelPtr[SrcPixelIndex]);
					GAcc += static_cast<unsigned int>(SrcPixelPtr[SrcPixelIndex + 1]);
					BAcc += static_cast<unsigned int>(SrcPixelPtr[SrcPixelIndex + 2]);
					SrcPixelIndex += BitDepth;
				}
			}
			if ((ScalingFactor == 2) || (ScalingFactor == 4))
			{
				DestPixelPtr[0] = static_cast<unsigned char>(RAcc >> ScalingFactor);
				DestPixelPtr[1] = static_cast<unsigned char>(GAcc >> ScalingFactor);
				DestPixelPtr[2] = static_cast<unsigned char>(BAcc >> ScalingFactor);
			}
			else
			{
				DestPixelPtr[0] = static_cast<unsigned char>(RAcc / ScalingArea);
				DestPixelPtr[1] = static_cast<unsigned char>(GAcc / ScalingArea);
				DestPixelPtr[2] = static_cast<unsigned char>(BAcc / ScalingArea);
			}
			SrcPixelPtr += ScalingFactor * BitDepth;
			DestPixelPtr += BitDepth;
		}
	}
}

/// <summary>
/// Fills a rectangular area with a solid color.
/// </summary>
/// <param name="hdc">Device context handle</param>
/// <param name="rectClient">Rectangle area to fill</param>
/// <param name="R">Red component (0-255)</param>
/// <param name="G">Green component (0-255)</param>
/// <param name="B">Blue component (0-255)</param>
void FillImageArea(HDC hdc, const RECT& rectClient, BYTE R, BYTE G, BYTE B)
{
	HRGN bgRgn = CreateRectRgnIndirect(&rectClient);
	HBRUSH hBrush = CreateSolidBrush(RGB(R, G, B));
	FillRgn(hdc, bgRgn, hBrush);
	DeleteObject(hBrush);
	DeleteObject(bgRgn);
}

/// <summary>
/// Clears a rectangular area by filling it with black (RGB 0,0,0).
/// </summary>
/// <param name="hdc">Device context handle</param>
/// <param name="rectClient">Rectangle area to clear</param>
void ClearImageArea(HDC hdc, const RECT& rectClient)
{
	FillImageArea(hdc, rectClient, 0, 0, 0);
}

/// <summary>
/// Updates the slider controls to reflect current filter intensity and scale values.
/// Forces immediate repaint of both sliders.
/// </summary>
/// <param name="hwnd">Handle to the parent dialog window</param>
void UpdateSliders(HWND hwnd)
{
	HWND hSlider1 = GetDlgItem(hwnd, IDC_SLIDER1);
	HWND hSlider2 = GetDlgItem(hwnd, IDC_SLIDER2);

	// Set the slider positions
	SendMessage(hSlider1, TBM_SETPOS, (WPARAM)TRUE, (LPARAM)FilterIntensity);
	SendMessage(hSlider2, TBM_SETPOS, (WPARAM)TRUE, (LPARAM)FilterScale);

	// Force immediate repaint of sliders
	// Note: RDW_ERASE is NOT used here to preserve tick marks on the trackbar controls
	RedrawWindow(hSlider1, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
	RedrawWindow(hSlider2, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
}

/// <summary>
/// Draws a gray line from point (x1, y1) to point (x2, y2).
/// Used for visual separation in the preview grid.
/// </summary>
/// <param name="hdc">Device context handle</param>
/// <param name="x1">X coordinate of start point</param>
/// <param name="y1">Y coordinate of start point</param>
/// <param name="x2">X coordinate of end point</param>
/// <param name="y2">Y coordinate of end point</param>
void DrawGrayLine(HDC hdc, int x1, int y1, int x2, int y2)
{
	HPEN hPen = CreatePen(PS_SOLID, 1, RGB(128, 128, 128)); // Create a gray pen
	HPEN oldPen = (HPEN)SelectObject(hdc, hPen);

	MoveToEx(hdc, x1, y1, NULL); // Move to the start point
	LineTo(hdc, x2, y2);         // Draw to the end point

	SelectObject(hdc, oldPen);
	DeleteObject(hPen);
}

/// <summary>
/// Prepares the drawing area for visualization by clearing it and optionally drawing guide lines.
/// Guide lines divide the preview area into thirds horizontally and vertically.
/// </summary>
/// <param name="hdc">Device context handle</param>
/// <param name="rectClient">Rectangle area to prepare</param>
/// <param name="drawLines">If true, draws guide lines; if false, only clears the area</param>
void PrepareVisualization(HDC hdc, RECT rectClient, bool drawLines)
{
	ClearImageArea(hdc, rectClient);

	if (drawLines)
	{
		auto rectWidth = RectWidth(rectClient);
		auto rectHeight = RectHeight(rectClient);
		DrawGrayLine(hdc, rectClient.left, rectClient.top + rectHeight / 3, rectClient.right, rectClient.top + rectHeight / 3);
		DrawGrayLine(hdc, rectClient.left + rectWidth / 3, rectClient.top, rectClient.left + rectWidth / 3, rectClient.top + rectHeight / 3);
		DrawGrayLine(hdc, rectClient.right - rectWidth / 3, rectClient.top + rectHeight / 3, rectClient.right - rectWidth / 3, rectClient.bottom);
	}
}

/// <summary>
/// Handles WM_PAINT messages to render the dialog GUI.
/// In complete visualization mode, displays the original image and multiple processed variants
/// with different intensity and grid scale settings arranged in a grid layout.
/// In simple mode, displays only the processed image.
/// </summary>
/// <param name="hwnd">Handle to the dialog window</param>
void HandlePaintMessage(HWND hwnd)
{
	RECT rectClient;
	GetClientRect(hwnd, &rectClient);
	rectClient.right -= 100;

	PAINTSTRUCT ps;
	HDC hdc = BeginPaint(hwnd, &ps);

	try
	{
		PrepareVisualization(hdc, rectClient, CompleteVisualization);

		if (CompleteVisualization)
		{
			const BYTE LESS_INTENSE = 5;
			const BYTE MORE_INTENSE = 15;
			const BYTE CURR_INTENSE = 10;

			const int SMALL_PICTURE = 32;

			// draw original image
			auto ScaledSrcImage = ScaledSrcImagePtr.lock();
			if (ScaledSrcImage != nullptr)
			{
				RECT OriginalImageRect = rectClient;
				ScaleRect(OriginalImageRect, SMALL_PICTURE);
				// draw in top-left corner
				DrawSingleImage(hdc, &BmHdrCopy, ScaledSrcImage.get()->data(), ScaledImageWidth, ScaledImageHeight, OriginalImageRect, false,
					FilterScale, NoZoom, L"Original image");
			}

			// draw processed image with lesser intensity
			auto ScaledProcImageIntensityM = ScaledProcImageIntensityMPtr.lock();
			if (ScaledProcImageIntensityM != nullptr)
			{
				RECT IntensityMImageRect = rectClient;
				ScaleRect(IntensityMImageRect, SMALL_PICTURE);
				// draw in top-mid position
				OffsetRect(&IntensityMImageRect, (RectWidth(rectClient) - RectWidth(IntensityMImageRect)) / 2, 0);
				FillImageArea(hdc, IntensityMImageRect, LESS_INTENSE, LESS_INTENSE, LESS_INTENSE);
				DrawSingleImage(hdc, &BmHdrCopy, ScaledProcImageIntensityM.get()->data(), ScaledImageWidth, ScaledImageHeight, IntensityMImageRect,
					false, FilterScale, NoZoom, L"Weaker filter (- Intensity)");
			}

			// draw processed image with higher intensity
			auto ScaledProcImageIntensityP = ScaledProcImageIntensityPPtr.lock();
			if (ScaledProcImageIntensityP != nullptr)
			{
				RECT IntensityPImageRect = rectClient;
				ScaleRect(IntensityPImageRect, SMALL_PICTURE);
				// draw in top-right corner
				OffsetRect(&IntensityPImageRect, (RectWidth(rectClient) - RectWidth(IntensityPImageRect)), 0);
				FillImageArea(hdc, IntensityPImageRect, MORE_INTENSE, MORE_INTENSE, MORE_INTENSE);
				DrawSingleImage(hdc, &BmHdrCopy, ScaledProcImageIntensityP.get()->data(), ScaledImageWidth, ScaledImageHeight, IntensityPImageRect,
					false, FilterScale, NoZoom, L"Stronger filter (+ Intensity)");
			}

			// draw processed image with coarser grid
			auto ScaledProcImageGridM = ScaledProcImageGridMPtr.lock();
			if (ScaledProcImageGridM != nullptr)
			{
				RECT GridMImageRect = rectClient;
				ScaleRect(GridMImageRect, SMALL_PICTURE);
				// draw in right-mid position
				OffsetRect(&GridMImageRect, RectWidth(rectClient) - RectWidth(GridMImageRect), (RectHeight(rectClient) - RectHeight(GridMImageRect)) / 2);
				FillImageArea(hdc, GridMImageRect, LESS_INTENSE, LESS_INTENSE, LESS_INTENSE);
				DrawSingleImage(hdc, &BmHdrCopy, ScaledProcImageGridM.get()->data(), ScaledImageWidth, ScaledImageHeight, GridMImageRect, true,
					max(FilterScale - 2, MIN_HOR_REGIONS), NoZoom, L"Coarser grid (- Scale)");
			}

			// draw processed image with finer grid
			auto ScaledProcImageGridP = ScaledProcImageGridPPtr.lock();
			if (ScaledProcImageGridP != nullptr)
			{
				RECT GridPImageRect = rectClient;
				ScaleRect(GridPImageRect, SMALL_PICTURE);
				// draw in bottom-right corner
				OffsetRect(&GridPImageRect, (RectWidth(rectClient) - RectWidth(GridPImageRect)), RectHeight(rectClient) - RectHeight(GridPImageRect));
				FillImageArea(hdc, GridPImageRect, MORE_INTENSE, MORE_INTENSE, MORE_INTENSE);
				DrawSingleImage(hdc, &BmHdrCopy, ScaledProcImageGridP.get()->data(), ScaledImageWidth, ScaledImageHeight, GridPImageRect, true,
					min(FilterScale + 2, MAX_HOR_REGIONS), NoZoom, L"Finer grid (+ Scale)");
			}

			// draw processed image
			auto ScaledProcImage = ScaledProcImagePtr.lock();
			if (ScaledProcImage != nullptr)
			{
				RECT CentralImageRect = rectClient;
				RECT SmallImageRect = rectClient;
				ScaleRect(SmallImageRect, SMALL_PICTURE);
				// draw in bottom-left corner
				CentralImageRect.left = 0;
				CentralImageRect.top = (RectHeight(rectClient) - RectHeight(SmallImageRect)) / 2;
				CentralImageRect.right = ((RectWidth(rectClient) - RectWidth(SmallImageRect)) / 2) + RectWidth(SmallImageRect);
				FillImageArea(hdc, CentralImageRect, CURR_INTENSE, CURR_INTENSE, CURR_INTENSE);
				DrawSingleImage(hdc, &BmHdrCopy, ScaledProcImage.get()->data(), ScaledImageWidth, ScaledImageHeight, CentralImageRect, false,
					FilterScale, NoZoom, L"Processed image");
			}
		}
		else
		{
			auto ScaledProcImage = ScaledProcImagePtr.lock();
			if (ScaledProcImage != nullptr)
			{
				// draw processed image only
				DrawSingleImage(hdc, &BmHdrCopy, ScaledProcImage.get()->data(), ScaledImageWidth, ScaledImageHeight, rectClient, false,
					FilterScale, NoZoom, L"Processed image");
			}
		}
	}
	catch (const std::exception& e)
	{
	#ifdef ENABLE_LOGGING
		std::cerr << "Exception: " << e.what() << std::endl;
	#else
		(void)e;  // Suppress unused variable warning
	#endif
	}
	EndPaint(hwnd, &ps);

	// Ensure child controls repaint (optional but useful)
	// Note: RDW_ERASE is NOT used here to preserve control details like slider tick marks
	EnumChildWindows(hwnd, [](HWND child, LPARAM) -> BOOL {
		RedrawWindow(child, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
		return TRUE;
		}, 0);
}

/// <summary>
/// Repositions a control relative to the right edge of the window.
/// Used during window resize to maintain control positions.
/// </summary>
/// <param name="hwnd">Handle to the parent window</param>
/// <param name="controlId">ID of the control to reposition</param>
/// <param name="offset">Distance from the right edge of the window</param>
/// <param name="windowWidth">Current width of the window</param>
void RepositionControl(HWND hwnd, int controlId, int offset, int windowWidth)
{
	HWND OkButtonCtrl = GetDlgItem(hwnd, controlId);
	RECT rect;
	GetWindowRect(OkButtonCtrl, &rect);
	MapWindowPoints(HWND_DESKTOP, hwnd, (LPPOINT)&rect, 2);
	SetWindowPos(OkButtonCtrl, NULL, windowWidth - offset, rect.top, 0, 0, SWP_NOZORDER | SWP_NOSIZE);
}

/// <summary>
/// Checks whether Windows dark mode is enabled by reading the system registry.
/// Queries the AppsUseLightTheme registry value to determine the current theme.
/// </summary>
/// <returns>true if dark mode is enabled, false if light mode or if registry read fails</returns>
bool IsDarkModeEnabled() {
	DWORD dwValue = 0;
	DWORD dwSize = sizeof(dwValue);

	// Open the registry key where the theme setting is stored
	LSTATUS status = RegGetValueW(
		HKEY_CURRENT_USER,
		L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
		L"AppsUseLightTheme",
		RRF_RT_DWORD,
		NULL,
		&dwValue,
		&dwSize
	);

	if (status == ERROR_SUCCESS) {
		return dwValue == 0;  // 0 means dark mode is enabled, 1 means light mode is enabled
	}

	return false;  // Default to false (light mode) if the registry value can't be read
}

// Helper RAII wrapper for HBRUSH
class ScopedBrush {
public:
	explicit ScopedBrush(COLORREF color) : brush_(CreateSolidBrush(color)) {}
	~ScopedBrush() { if (brush_) DeleteObject(brush_); }
	HBRUSH get() const { return brush_; }
private:
	HBRUSH brush_;
};

// Global RAII-managed brush`
static ScopedBrush* gBackgroundBrush = new ScopedBrush(RGB(255, 255, 255));  // default light color

// Enum to track current mode
enum class ThemeMode { Light, Dark };
static ThemeMode gCurrentTheme = ThemeMode::Light;

// Updates window and controls for dark mode/light mode
void AdjustForDarkMode(HWND hwnd) {
	bool darkMode = IsDarkModeEnabled();
	ThemeMode desiredTheme = darkMode ? ThemeMode::Dark : ThemeMode::Light;

	if (desiredTheme == gCurrentTheme) return;
	gCurrentTheme = desiredTheme;

	COLORREF bgColor = darkMode ? RGB(45, 45, 48) : RGB(255, 255, 255);

	delete gBackgroundBrush;
	gBackgroundBrush = new ScopedBrush(bgColor);

	// Note: RDW_ERASE is NOT used to preserve child control details like slider tick marks
	RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
}

char SetupIniFile[1024];

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

/// <summary>
/// Dialog procedure that handles all messages for the AltaLux plugin dialog.
/// Manages user interactions with sliders, buttons, mouse clicks for parameter adjustment,
/// theme changes, and window resizing. Supports both light and dark themes.
/// </summary>
/// <param name="hwnd">Handle to the dialog window</param>
/// <param name="msg">Message identifier</param>
/// <param name="wparam">First message parameter</param>
/// <param name="lparam">Second message parameter</param>
/// <returns>TRUE if message was processed, FALSE otherwise; for some messages returns a handle</returns>
INT_PTR CALLBACK DlgProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
	switch (msg)
	{
	case WM_INITDIALOG:
		{
			// Initialize theme based on current system setting
			AdjustForDarkMode(hwnd);

			// Set title bar to match system theme
			BOOL useDarkMode = IsDarkModeEnabled();
			DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDarkMode, sizeof(useDarkMode));

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
					// Recompute filtered images
					DoProcessing();
					// Repaint sliders fully
					UpdateSliders(hwnd);
					// Full repaint of the main dialog
					// Note: Use FALSE to avoid erasing child control backgrounds (slider tick marks)
					InvalidateRect(hwnd, nullptr, FALSE);
					UpdateWindow(hwnd);
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
					sprintf_s(FilterString, sizeof(FilterString), "%d", FilterIntensity);
					WritePrivateProfileStringA("AltaLux", "Intensity", FilterString, SetupIniFile);
					sprintf_s(FilterString, sizeof(FilterString), "%d", FilterScale);
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
					DoProcessing();
					UpdateSliders(hwnd);        // redraw sliders after processing
					// Note: Use FALSE to avoid erasing child control backgrounds (slider tick marks)
					InvalidateRect(hwnd, nullptr, FALSE);  // redraw dialog/image
					UpdateWindow(hwnd);
					return TRUE;
				}
			case IDC_TOGGLEVISUALIZATION:
				{
					CompleteVisualization = !CompleteVisualization;
					RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
					return TRUE;
				}
			case IDC_TOGGLEZOOM:
				{
					NoZoom = !NoZoom;
					RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
					return TRUE;
				}
			}
		}

	case WM_VSCROLL:
	{
		HWND hwndTrack = (HWND)lparam;
		int dwPos = static_cast<int>(SendMessage(hwndTrack, TBM_GETPOS, 0, 0));

		if (hwndTrack == GetDlgItem(hwnd, IDC_SLIDER1))
			FilterIntensity = dwPos;
		if (hwndTrack == GetDlgItem(hwnd, IDC_SLIDER2))
			FilterScale = dwPos;

		switch (LOWORD(wparam))
		{
		case TB_THUMBTRACK:
		case TB_THUMBPOSITION:
			// Optional live preview
			// DoProcessing();
			// InvalidateRect(hwnd, nullptr, TRUE);
			break;

		case TB_ENDTRACK:
			DoProcessing();
			UpdateSliders(hwnd); // redraw sliders fully
			// Note: RDW_ERASE is NOT used because HandlePaintMessage already clears the background
			// explicitly via PrepareVisualization, and RDW_ERASE with RDW_ALLCHILDREN would
			// erase child controls' backgrounds, removing slider tick marks
			RedrawWindow(hwnd, nullptr, nullptr,
				RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
			break;
		}
		return TRUE;
	}

	case WM_PAINT:
		{
			HandlePaintMessage(hwnd);
			return TRUE;
		}

	case WM_CTLCOLORDLG:
	case WM_CTLCOLORSTATIC:
	case WM_CTLCOLORBTN:
	{
		HDC hdc = reinterpret_cast<HDC>(wparam);
		SetBkMode(hdc, TRANSPARENT);

		SetTextColor(hdc, gCurrentTheme == ThemeMode::Dark
			? RGB(255, 255, 255)
			: RGB(0, 0, 0));

		return reinterpret_cast<INT_PTR>(gBackgroundBrush->get());
	}

	case WM_THEMECHANGED:
	case WM_SETTINGCHANGE:
		AdjustForDarkMode(hwnd);

		// Update title bar to match new theme
		{
			BOOL useDarkMode = IsDarkModeEnabled();
			DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDarkMode, sizeof(useDarkMode));
		}

		// Note: Use FALSE to avoid erasing child control backgrounds (slider tick marks)
		InvalidateRect(hwnd, nullptr, FALSE);
		// Note: RDW_ERASE is NOT used to preserve control details like slider tick marks
		EnumChildWindows(hwnd, [](HWND child, LPARAM) -> BOOL {
			RedrawWindow(child, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
			return TRUE;
			}, 0);
		break;

	case WM_SIZE:
		{
			int width = LOWORD(lparam);  // New width of the window
			int height = HIWORD(lparam); // New height of the window
			// Reposition or resize controls based on the new width and height
			// x=572 in the dialog definition in the RC file maps to an offset of 88
			const int DEF_OFFSET = 88;
			RepositionControl(hwnd, IDOK, DEF_OFFSET, width);
			RepositionControl(hwnd, IDCANCEL, DEF_OFFSET, width);
			RepositionControl(hwnd, IDC_INTENSITY_SLIDER, DEF_OFFSET - 32, width);
			RepositionControl(hwnd, IDC_SCALE_SLIDER, DEF_OFFSET - 32, width);		
			RepositionControl(hwnd, IDC_INTENSITY_STATIC, DEF_OFFSET, width);
			RepositionControl(hwnd, IDC_SCALE_STATIC, DEF_OFFSET, width);
			RepositionControl(hwnd, ID_DEFAULT, DEF_OFFSET, width);
			RepositionControl(hwnd, IDC_TOGGLEVISUALIZATION, DEF_OFFSET, width);
			RepositionControl(hwnd, IDC_TOGGLEZOOM, DEF_OFFSET, width);
			RepositionControl(hwnd, IDC_BITMAP_GRID_LARGE_STATIC, DEF_OFFSET, width);
			RepositionControl(hwnd, IDC_BITMAP_GRID_SMALL_STATIC, DEF_OFFSET, width);
			RepositionControl(hwnd, IDC_BITMAP_INTENSITY_LOW_STATIC, DEF_OFFSET, width);
			RepositionControl(hwnd, IDC_BITMAP_INTENSITY_HIGH_STATIC, DEF_OFFSET, width);
			InvalidateRgn(hwnd, nullptr, true);
			return TRUE;
		}

	case WM_GETMINMAXINFO:
		{
			MINMAXINFO* pMMI = (MINMAXINFO*)lparam;
			pMMI->ptMinTrackSize.x = 800;  // Minimum width of the window
			pMMI->ptMinTrackSize.y = 650;  // Minimum height of the window
			return TRUE;
		}

	default: return FALSE;
	}
	return TRUE;
}

/// <summary>
/// Calculates the buffer size required to store an RGB image with security padding.
/// </summary>
/// <param name="ImageWidth">Width of the image in pixels</param>
/// <param name="ImageHeight">Height of the image in pixels</param>
/// <returns>Total buffer size in bytes including security padding</returns>
int GetRGBImageSize(int ImageWidth, int ImageHeight)
{
	const int SECURITY_PADDING = 4096;
	return (ImageWidth * ImageHeight * ImageBitDepth) + SECURITY_PADDING;
}

/// <summary>
/// Computes the optimal scaling factor for downsampling images in preview mode.
/// Ensures the scaled image width is a multiple of 8 for proper rendering.
/// Target preview size is approximately 1000x800 pixels.
/// </summary>
/// <remarks>Scaled image width must be a multiple of 8; if not, a smaller scaling factor is chosen</remarks>
void ComputeScalingFactor()
{
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
		{
			// fix for non-standard, multiple of 4 rescaled images that may not be drawn correctly
			ScalingFactor--;
		}
		else
			break;
	}
	if (ScalingFactor > 1)
	{
		ScaledImageWidth = ImageWidth / ScalingFactor;
		ScaledImageHeight = ImageHeight / ScalingFactor;
	}
	else
	{
		ScaledImageWidth = ImageWidth;
		ScaledImageHeight = ImageHeight;
	}
}

/// <summary>
/// Copies the processed image data back into the IrfanView bitmap buffer.
/// Handles both cropped and full image cases, adjusting for bitmap stride.
/// </summary>
/// <param name="ImageBits">Pointer to the IrfanView bitmap buffer</param>
/// <param name="ImageBitsStride">Byte width of each bitmap row including padding</param>
/// <param name="SrcImage">Pointer to the processed image data</param>
/// <param name="ClipRect">Clipping rectangle for cropped images</param>
void CopyToSourceImage(BYTE* ImageBits, DWORD ImageBitsStride, unsigned char* SrcImage, RECT ClipRect)
{
	if (!ImageBits || !SrcImage)
		return;

	unsigned char* SrcImagePtr = SrcImage;
	unsigned char* ImageBitsPtr = ImageBits;
	if (CroppedImage)
	{
		ImageBitsPtr += ClipRect.left * ImageBitDepth;
		ImageBitsPtr += ImageBitsStride * ClipRect.top;
		for (int y = ClipRect.top; y < ClipRect.bottom; y++)
		{
			memcpy(ImageBitsPtr, SrcImagePtr, ImageWidth * ImageBitDepth);
			SrcImagePtr += ImageWidth * ImageBitDepth;
			ImageBitsPtr += ImageBitsStride;
		}
	}
	else
	{
		for (int y = 0; y < FullImageHeight; y++)
		{
			memcpy(ImageBitsPtr, SrcImagePtr, ImageWidth * ImageBitDepth);
			SrcImagePtr += ImageWidth * ImageBitDepth;
			ImageBitsPtr += ImageBitsStride;
		}
	}
}

/// <summary>
/// Normalizes and aligns a clipping rectangle to 8-pixel boundaries.
/// Converts from (left, top, width, height) format to (left, top, right, bottom) format.
/// Updates global ImageWidth and ImageHeight to match the normalized rectangle.
/// </summary>
/// <param name="ClipRect">Rectangle to normalize (modified in place)</param>
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

/// <summary>
/// Copies image data from the IrfanView bitmap buffer to the internal processing buffer.
/// Handles both cropped and full image cases, adjusting for bitmap stride.
/// </summary>
/// <param name="SrcImage">Destination buffer for image data</param>
/// <param name="ClipRect">Clipping rectangle for cropped images</param>
/// <param name="ImageBits">Pointer to the IrfanView bitmap buffer</param>
/// <param name="ImageBitsStride">Byte width of each bitmap row including padding</param>
void CopyFromSourceImage(unsigned char* SrcImage, RECT ClipRect, BYTE* ImageBits, DWORD ImageBitsStride)
{
	if (!ImageBits || !SrcImage)
		return;

	unsigned char* SrcImagePtr = SrcImage;
	unsigned char* ImageBitsPtr = ImageBits;
	if (CroppedImage)
	{
		// copy only part of source image
		ImageBitsPtr += ClipRect.left * ImageBitDepth;
		ImageBitsPtr += ImageBitsStride * ClipRect.top;
		for (int y = ClipRect.top; y < ClipRect.bottom; y++)
		{
			memcpy(SrcImagePtr, ImageBitsPtr, ImageWidth * ImageBitDepth);
			SrcImagePtr += ImageWidth * ImageBitDepth;
			ImageBitsPtr += ImageBitsStride;
		}
	}
	else
	{
		// copy whole source image
		for (int y = 0; y < FullImageHeight; y++)
		{
			memcpy(SrcImagePtr, ImageBitsPtr, ImageWidth * ImageBitDepth);
			SrcImagePtr += ImageWidth * ImageBitDepth;
			ImageBitsPtr += ImageBitsStride;
		}
	}
}

/// <summary>
/// Determines whether the image is cropped or has non-standard dimensions.
/// An image is considered cropped if the full size differs from the working size,
/// or if dimensions are not multiples of 8.
/// </summary>
/// <returns>true if image is cropped or non-standard, false otherwise</returns>
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
/// Verifies if the image bit depth provided in the bitmap header is supported by the plugin.
/// Sets the global ImageBitDepth variable based on the bitmap's bit count.
/// </summary>
/// <param name="pbBmHdr">Reference to bitmap header wrapper containing image information</param>
/// <returns>true if the bit depth is supported (24 or 32 bits per pixel), false otherwise</returns>
bool isSupportedBitDepth(ScopedBitmapHeader& pbBmHdr)
{
	switch (pbBmHdr->biBitCount) {
	case 24:
		ImageBitDepth = RGB24_PIXEL_SIZE;  // Typically 3 bytes
		break;
	case 32:
		ImageBitDepth = RGB32_PIXEL_SIZE;  // Typically 4 bytes
		break;
	default:
		return false;  // Unsupported bit depth
	}
	return true;
}

/// <summary>
/// Processes the incoming bitmap with the AltaLux filter.
/// Can directly process the image with specified parameters or open the GUI for user interaction.
/// Handles both 24-bit and 32-bit RGB images, including cropped regions.
/// </summary>
/// <param name="hDib">Handle to the DIB (Device Independent Bitmap)</param>
/// <param name="hwnd">Handle to parent window</param>
/// <param name="filter">Filter type identifier (unused)</param>
/// <param name="rect">Clipping rectangle (left, top, width, height format)</param>
/// <param name="param1">Filter intensity [0-100]; if -1, opens GUI for user selection</param>
/// <param name="param2">Filter scale [2-16]; if -1, opens GUI for user selection</param>
/// <param name="iniFile">Path to INI file for saving/loading parameters</param>
/// <param name="szAppName">Application name (unused)</param>
/// <param name="regID">Registration ID (unused)</param>
/// <returns>true if processing succeeded, false on error or unsupported format</returns>
bool __cdecl StartEffects2(HANDLE hDib, HWND hwnd, int filter, RECT rect, int param1, int param2, char* iniFile,
                           char* szAppName, int regID)
{
#define WIDTHBYTES(bits) (((bits) + 31) / 32 * 4)

	const int SECURITY_PADDING = 4096;
	SharedImagePtr SrcImage;
	RECT ClipRect = rect;
	{
		ScopedBitmapHeader pbBmHdr(hDib);
		memcpy(&BmHdrCopy, &(*pbBmHdr), sizeof(BITMAPINFOHEADER));

		if (pbBmHdr->biPlanes != 1 || !isSupportedBitDepth(pbBmHdr))
			return false;

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
		}

		BYTE* ImageBits = pbBmHdr.GetImageBits();
		DWORD ImageBitsStride = WIDTHBYTES(static_cast<DWORD>(FullImageWidth * pbBmHdr->biBitCount));
		/// SrcImage
		SrcImage = std::make_shared<std::vector<unsigned char>>(GetRGBImageSize(ImageWidth, ImageHeight));		
		SrcImagePtr = SrcImage;

		// copy from ImageBits into SrcImage
		CopyFromSourceImage(SrcImage.get()->data(), ClipRect, ImageBits, ImageBitsStride);
	}

	/// ProcImage
	auto ProcImage = std::make_shared<std::vector<unsigned char>>(*SrcImage.get());
	ProcImagePtr = ProcImage;

	/// param1 : [0..100], default 25
	/// param2 : [2..16], default 8
	if ((param1 == -1) || (param2 == -1))
	{
		// show GUI
		strcpy_s(SetupIniFile, sizeof(SetupIniFile), iniFile);
		FilterIntensity = GetPrivateProfileIntA("AltaLux", "Intensity", AL_DEFAULT_STRENGTH, SetupIniFile);
		FilterScale = GetPrivateProfileIntA("AltaLux", "Scale", DEFAULT_HOR_REGIONS, SetupIniFile);

		ComputeScalingFactor();

		auto ScaledSrcImage = std::make_shared<std::vector<unsigned char>>(GetRGBImageSize(ScaledImageWidth, ScaledImageHeight));
		ScaledSrcImagePtr = ScaledSrcImage;

		ScaleDownImage(SrcImage->data(), ImageWidth, ImageHeight, ScaledSrcImage->data(), ScalingFactor, ImageBitDepth);

		auto ScaledProcImage = std::make_shared<std::vector<unsigned char>>(*ScaledSrcImage.get());
		ScaledProcImagePtr = ScaledProcImage;

		// allocate buffer for processed image with coarser grid
		auto ScaledProcImageGridM = std::make_shared<std::vector<unsigned char>>(*ScaledSrcImage.get());
		ScaledProcImageGridMPtr = ScaledProcImageGridM;

		// allocate buffer for processed image with finer grid
		auto ScaledProcImageGridP = std::make_shared<std::vector<unsigned char>>(*ScaledSrcImage.get());
		ScaledProcImageGridPPtr = ScaledProcImageGridP;

		// allocate buffer for processed image with lesser intensity
		auto ScaledProcImageIntensityM = std::make_shared<std::vector<unsigned char>>(*ScaledSrcImage.get());
		ScaledProcImageIntensityMPtr = ScaledProcImageIntensityM;

		// allocate buffer for processed image with higher intensity
		auto ScaledProcImageIntensityP = std::make_shared<std::vector<unsigned char>>(*ScaledSrcImage.get());
		ScaledProcImageIntensityPPtr = ScaledProcImageIntensityP;

		INT_PTR ret = DialogBox(hDll, MAKEINTRESOURCE(IDD_DIALOG1), hwnd, (DLGPROC)DlgProc);

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
		if (ImageBitDepth == RGB32_PIXEL_SIZE)
			AltaLuxFilter->ProcessRGB32(static_cast<void*>(SrcImage->data()));
		else
			AltaLuxFilter->ProcessRGB24(static_cast<void *>(SrcImage->data()));
		
		ScopedBitmapHeader pbBmHdr(hDib);
		BYTE* ImageBits = pbBmHdr.GetImageBits();
		DWORD ImageBitsStride = WIDTHBYTES(static_cast<DWORD>(FullImageWidth * pbBmHdr->biBitCount));
		CopyToSourceImage(ImageBits, ImageBitsStride, SrcImage->data(), ClipRect);
	}
	catch (const std::exception& e)
	{
	#ifdef ENABLE_LOGGING
		std::cerr << "Exception: " << e.what() << std::endl;
	#else
		(void)e;  // Suppress unused variable warning
	#endif
		return false;
	}
	return true;
}

/// <summary>
/// Returns general information about the plugin.
/// Called by IrfanView to retrieve version and description.
/// </summary>
/// <param name="versionString">Output buffer for version number string</param>
/// <param name="fileFormats">Output buffer for plugin description</param>
/// <returns>Always returns 0</returns>
int __cdecl GetPlugInInfo(char* versionString, char* fileFormats)
{
	sprintf_s(versionString, 64, "1.10"); // your version-nr
	sprintf_s(fileFormats, 256, "AltaLux image enhancement filter"); // some infos
	return 0;
}

/// <summary>
/// Main entry point for the AltaLux plugin effects processing.
/// This is the exported function called by IrfanView to apply the filter.
/// Delegates to StartEffects2 for actual processing.
/// </summary>
/// <param name="hDib">Handle to the DIB (Device Independent Bitmap)</param>
/// <param name="hwnd">Handle to parent window</param>
/// <param name="filter">Filter type identifier</param>
/// <param name="rect">Clipping rectangle</param>
/// <param name="param1">Filter intensity parameter</param>
/// <param name="param2">Filter scale parameter</param>
/// <param name="iniFile">Path to INI file for parameters</param>
/// <param name="szAppName">Application name</param>
/// <param name="regID">Registration ID</param>
/// <returns>true if processing succeeded, false otherwise</returns>
bool __cdecl AltaLux_Effects(HANDLE hDib, HWND hwnd, int filter, RECT rect, int param1, int param2, char* iniFile,
                             char* szAppName, int regID)
{
	return StartEffects2(hDib, hwnd, filter, rect, param1, param2, iniFile, szAppName, regID);
}
