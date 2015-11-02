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

#include <stdio.h>
#include <math.h>
#include <malloc.h>
#include <Commctrl.h>

#include <memory>

#include "Filter\CBaseAltaLuxFilter.h"
#include "Filter\CAltaLuxFilterFactory.h"
#include "UIDraw\UIDraw.h"

#ifdef ENABLE_LOGGING
	#define ELPP_NO_DEFAULT_LOG_FILE
	#include "Log\easylogging++.h"
	#include "Log\SetupLog.h"

	#define ELPP_AS_DLL // Tells Easylogging++ that it's used for DLL
	#define ELPP_EXPORT_SYMBOLS // Tells Easylogging++ to export symbols

	INITIALIZE_EASYLOGGINGPP
#endif // ENABLE_LOGGING

HINSTANCE hDll;
LPBITMAPINFOHEADER pbBmHdr;
BITMAPINFOHEADER BmHdrCopy;
unsigned char *SrcImage = NULL;
unsigned char *ProcImage = NULL;
int ImageWidth;
int ImageHeight;
int FullImageWidth;
int FullImageHeight;
bool CroppedImage;
bool SkipProcessing;
int ScaledImageWidth;
int ScaledImageHeight;
int ScalingFactor = 1;
unsigned char *ScaledSrcImage = NULL;		// down-sampled source image
unsigned char *ScaledProcImage = NULL;		// processed image
unsigned char *ScaledProcImageGridM = NULL;	// processed image with lesser intensity
unsigned char *ScaledProcImageGridP = NULL;	// processed image with higher intensity
unsigned char *ScaledProcImageIntensityM = NULL;	// processed image with coarser grid
unsigned char *ScaledProcImageIntensityP = NULL;	// processed image with finer grid
/// GUI
int FilterIntensity = AL_DEFAULT_STRENGTH;
int FilterScale = DEFAULT_HOR_REGIONS;
bool CompleteVisualization = true;

BOOL APIENTRY DllMain( HANDLE hModule, 
					   DWORD  ul_reason_for_call, 
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

void DoProcessing()
{
#ifdef ENABLE_LOGGING
	TIMED_FUNC(DoProcessingTimerObj);
#endif

	CBaseAltaLuxFilter *AltaLuxFilter = NULL;
	try
	{
		if (ScaledSrcImage != NULL)
			AltaLuxFilter = CAltaLuxFilterFactory::CreateAltaLuxFilter(ScaledImageWidth, ScaledImageHeight, FilterScale, FilterScale);
		else
			AltaLuxFilter = CAltaLuxFilterFactory::CreateAltaLuxFilter(ImageWidth, ImageHeight, FilterScale, FilterScale);
	} catch (std::exception& e) {
	#ifdef ENABLE_LOGGING
		LOG(ERROR) << "Cannot create filter instance (77): " << e.what();
	#endif
		AltaLuxFilter = NULL;
	}
	if (AltaLuxFilter != NULL)
	{
		try
		{
		#ifdef ENABLE_LOGGING
			TIMED_SCOPE(timerBlkObj, "Filter processing");
		#endif
			std::unique_ptr<CBaseAltaLuxFilter> AltaLuxFilterPtr(AltaLuxFilter);
			if (ScaledSrcImage != NULL)
			{
				// rescaling is enabled, so preview is computed on the smaller resampled image
				memcpy(ScaledProcImage, ScaledSrcImage, ScaledImageWidth * ScaledImageHeight * 3);
				AltaLuxFilterPtr->SetStrength(FilterIntensity);
				AltaLuxFilterPtr->ProcessRGB24((void *)ScaledProcImage);	
			#ifdef ENABLE_LOGGING
				PERFORMANCE_CHECKPOINT(timerBlkObj);
			#endif

				// preview with lesser intensity
				memcpy(ScaledProcImageIntensityM, ScaledSrcImage, ScaledImageWidth * ScaledImageHeight * 3);
				AltaLuxFilterPtr->SetStrength(max(FilterIntensity - 15, AL_MIN_STRENGTH));
				AltaLuxFilterPtr->ProcessRGB24((void *)ScaledProcImageIntensityM);
			#ifdef ENABLE_LOGGING
				PERFORMANCE_CHECKPOINT(timerBlkObj);
			#endif

				// preview with higher intensity
				memcpy(ScaledProcImageIntensityP, ScaledSrcImage, ScaledImageWidth * ScaledImageHeight * 3);
				AltaLuxFilterPtr->SetStrength(min(FilterIntensity + 15, AL_MAX_STRENGTH));
				AltaLuxFilterPtr->ProcessRGB24((void *)ScaledProcImageIntensityP);
			#ifdef ENABLE_LOGGING
				PERFORMANCE_CHECKPOINT(timerBlkObj);
			#endif

				// preview with coarser grid
				memcpy(ScaledProcImageGridM, ScaledSrcImage, ScaledImageWidth * ScaledImageHeight * 3);
				AltaLuxFilterPtr->SetSlices(max(FilterScale - 2, MIN_HOR_REGIONS), max(FilterScale - 2, MIN_VERT_REGIONS));
				AltaLuxFilterPtr->ProcessRGB24((void *)ScaledProcImageGridM);
			#ifdef ENABLE_LOGGING
				PERFORMANCE_CHECKPOINT(timerBlkObj);
			#endif

				// preview with finer grid
				memcpy(ScaledProcImageGridP, ScaledSrcImage, ScaledImageWidth * ScaledImageHeight * 3);
				AltaLuxFilterPtr->SetSlices(min(FilterScale + 2, MAX_HOR_REGIONS), min(FilterScale + 2, MAX_VERT_REGIONS));
				AltaLuxFilterPtr->ProcessRGB24((void *)ScaledProcImageGridP);
			#ifdef ENABLE_LOGGING
				PERFORMANCE_CHECKPOINT(timerBlkObj);
			#endif
			} else {
				// full-scale view only
				memcpy(ProcImage, SrcImage, ImageWidth * ImageHeight * 3);
				AltaLuxFilterPtr->SetStrength(FilterIntensity);
				AltaLuxFilterPtr->ProcessRGB24((void *)ProcImage);
			}
		}
		catch (std::exception& e)
		{
		#ifdef ENABLE_LOGGING
			LOG(ERROR) << "Exception running filter instance (114): " << e.what();
		#endif
		}
	}
}

void ScaleRect(RECT &RectToScale, int ScalingFactor)
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
void ScaleDownImage(void *SrcImage, int SrcImageWidth, int SrcImageHeight, void *DestImage, int ScalingFactor)
{
	if (SrcImage == NULL)
		return;
	if (DestImage == NULL)
		return;
	if (ScalingFactor == 1)
	{
		/// no rescaling
		memcpy(DestImage, SrcImage, SrcImageWidth * SrcImageHeight * 3);
		return;
	}

	unsigned char *DestImagePtr = (unsigned char *)DestImage;
	unsigned char *SrcImagePtr = (unsigned char *)SrcImage;
	int SrcImageStride = SrcImageWidth * 3;
	int DestImageWidth = SrcImageWidth / ScalingFactor;
	int DestImageHeight = SrcImageHeight / ScalingFactor;
	unsigned char *DestPixelPtr = DestImagePtr;

	for (int y = 0; y < DestImageHeight; y++)
	{
		unsigned char *SrcPixelPtr = &SrcImagePtr[((y * ScalingFactor) * SrcImageWidth) * 3];
		for (int x = 0; x < DestImageWidth; x++)
		{
			unsigned int RAcc = 0;
			unsigned int GAcc = 0;
			unsigned int BAcc = 0;
			for (int iy = 0; iy < ScalingFactor; iy++)
				for (int ix = 0; ix < ScalingFactor; ix++)
				{
					RAcc += (unsigned int)SrcPixelPtr[(iy * SrcImageStride) + (ix * 3)    ];
					GAcc += (unsigned int)SrcPixelPtr[(iy * SrcImageStride) + (ix * 3) + 1];
					BAcc += (unsigned int)SrcPixelPtr[(iy * SrcImageStride) + (ix * 3) + 2];
				}
			if (ScalingFactor == 2)
			{
				DestPixelPtr[0] = (unsigned char)(RAcc >> 2);
				DestPixelPtr[1] = (unsigned char)(GAcc >> 2);
				DestPixelPtr[2] = (unsigned char)(BAcc >> 2);
			} else {
				DestPixelPtr[0] = (unsigned char)(RAcc / (ScalingFactor * ScalingFactor));
				DestPixelPtr[1] = (unsigned char)(GAcc / (ScalingFactor * ScalingFactor));
				DestPixelPtr[2] = (unsigned char)(BAcc / (ScalingFactor * ScalingFactor));
			}
			SrcPixelPtr += ScalingFactor * 3;
			DestPixelPtr += 3;
		}
	}
}

void UpdateFilterDisplay(HWND hwnd)
{
	/*
	char FilterString[16];
	sprintf(FilterString, "%d", FilterIntensity);
	SendMessage (GetDlgItem(hwnd, IDC_STATIC_INTENSITY), WM_SETTEXT, 0, (LPARAM)FilterString);
	sprintf(FilterString, "%d", FilterScale);
	SendMessage (GetDlgItem(hwnd, IDC_STATIC_SCALE), WM_SETTEXT, 0, (LPARAM)FilterString);
	*/
}

void ClearImageArea(HDC hdc, RECT &rectClient)
{
	HRGN bgRgn = CreateRectRgnIndirect(&rectClient);
	HBRUSH hBrush = CreateSolidBrush(RGB(0,0,0));
	FillRgn(hdc, bgRgn, hBrush);
}

void UpdateSliders(HWND hwnd)
{
	HWND hwndTrack = GetDlgItem(hwnd, IDC_SLIDER1);
	SendMessage(hwndTrack, TBM_SETPOS, 
				(WPARAM) TRUE,                   // redraw flag 
				(LPARAM) FilterIntensity); 
	hwndTrack = GetDlgItem(hwnd, IDC_SLIDER2);
	SendMessage(hwndTrack, TBM_SETPOS, 
				(WPARAM) TRUE,                   // redraw flag 
				(LPARAM) FilterScale);
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
	HDC hdc = BeginPaint(hwnd,&ps);
	try
	{
		ClearImageArea(hdc, rectClient);
		if (CompleteVisualization)
		{
			int SavedFilterScale = FilterScale;

			// draw original image
			RECT OriginalImageRect = rectClient;
			ScaleRect(OriginalImageRect, 31);
			DrawSingleImage(hdc, &BmHdrCopy, ScaledSrcImage, ScaledImageWidth, ScaledImageHeight, OriginalImageRect, false, FilterScale);

			// draw processed image with lesser intensity
			RECT IntensityMImageRect = rectClient;
			ScaleRect(IntensityMImageRect, 31);
			OffsetRect(&IntensityMImageRect, (RectWidth(rectClient) - RectWidth(IntensityMImageRect)) / 2, 0);
			DrawSingleImage(hdc, &BmHdrCopy, ScaledProcImageIntensityM, ScaledImageWidth, ScaledImageHeight, IntensityMImageRect, false, FilterScale);

			// draw processed image with higher intensity
			RECT IntensityPImageRect = rectClient;
			ScaleRect(IntensityPImageRect, 31);
			OffsetRect(&IntensityPImageRect, (RectWidth(rectClient) - RectWidth(IntensityPImageRect)) / 2, (RectHeight(rectClient) - RectHeight(IntensityPImageRect)));
			DrawSingleImage(hdc, &BmHdrCopy, ScaledProcImageIntensityP, ScaledImageWidth, ScaledImageHeight, IntensityPImageRect, false, FilterScale);

			// draw processed image with coarser grid
			RECT GridMImageRect = rectClient;
			ScaleRect(GridMImageRect, 31);
			FilterScale = max(SavedFilterScale - 2, MIN_HOR_REGIONS);
			OffsetRect(&GridMImageRect, 0, (RectHeight(rectClient) - RectHeight(GridMImageRect)) / 2);
			DrawSingleImage(hdc, &BmHdrCopy, ScaledProcImageGridM, ScaledImageWidth, ScaledImageHeight, GridMImageRect, true, FilterScale);

			// draw processed image with finer grid
			RECT GridPImageRect = rectClient;
			ScaleRect(GridPImageRect, 31);
			FilterScale = min(SavedFilterScale + 2, MAX_HOR_REGIONS);
			OffsetRect(&GridPImageRect, (RectWidth(rectClient) - RectWidth(GridPImageRect)), (RectHeight(rectClient) - RectHeight(GridPImageRect)) / 2);
			DrawSingleImage(hdc, &BmHdrCopy, ScaledProcImageGridP, ScaledImageWidth, ScaledImageHeight, GridPImageRect, true, FilterScale);

			FilterScale = SavedFilterScale;

			// draw processed image
			RECT CentralImageRect = rectClient;
			ScaleRect(CentralImageRect, 55);
			OffsetRect(&CentralImageRect, (RectWidth(rectClient) - RectWidth(CentralImageRect)) / 2, (RectHeight(rectClient) - RectHeight(CentralImageRect)) / 2);
			DrawSingleImage(hdc, &BmHdrCopy, ScaledProcImage, ScaledImageWidth, ScaledImageHeight, CentralImageRect, true, FilterScale);
		} else {
			// draw processed image only
			DrawSingleImage(hdc, &BmHdrCopy, ScaledProcImage, ScaledImageWidth, ScaledImageHeight, rectClient, true, FilterScale);
		}
	} catch (std::exception& e) {
	#ifdef ENABLE_LOGGING
		LOG(ERROR) << "Exception handling paint message (263): " << e.what();
	#endif
	}
	EndPaint(hwnd,&ps);
}

char SetupIniFile[1024];

BOOL CALLBACK DlgProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{    
	HBITMAP hBitmap;

	switch(msg)    
	{
	case WM_INITDIALOG:	{            // Do stuff you need to init your dialogbox here	    
		HWND hwndBitmap = GetDlgItem(hwnd, IDC_SFONDO);
		SetWindowPos(hwndBitmap, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
		
		HWND hwndTrack = GetDlgItem(hwnd, IDC_SLIDER1);
		SendMessage(hwndTrack, TBM_SETRANGE, 
			(WPARAM) TRUE,                   // redraw flag 
			(LPARAM) MAKELONG(AL_MIN_STRENGTH, AL_MAX_STRENGTH));  // min. & max. positions 
		SendMessage(hwndTrack, TBM_SETPOS, 
			(WPARAM) TRUE,                   // redraw flag 
			(LPARAM) FilterIntensity); 

		hwndTrack = GetDlgItem(hwnd, IDC_SLIDER2);
		SendMessage(hwndTrack, TBM_SETRANGE, 
			(WPARAM) TRUE,                   // redraw flag 
			(LPARAM) MAKELONG(MIN_HOR_REGIONS, MAX_HOR_REGIONS));  // min. & max. positions 
		SendMessage(hwndTrack, TBM_SETPOS, 
			(WPARAM) TRUE,                   // redraw flag 
			(LPARAM) FilterScale);

		UpdateFilterDisplay(hwnd);
		DoProcessing();
		InvalidateRgn(hwnd, NULL, true);
		return TRUE;
		}    	
	case WM_LBUTTONDOWN:	{
		int MouseXPos = LOWORD(lparam); 
		int MouseYPos = HIWORD(lparam); 

		RECT rectClient;
		GetClientRect(hwnd, &rectClient);
		rectClient.right -= 100;

		if ((CompleteVisualization) && (MouseXPos < rectClient.right))
		{
			int ThirdWidth = RectWidth(rectClient) / 3;
			int ThirdHeight = RectHeight(rectClient) / 3;
			bool ChangedSettings = false;
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
				UpdateFilterDisplay(hwnd);
				DoProcessing();
				InvalidateRgn(hwnd, NULL, true);
			}
		}
		return TRUE;
		}
	case WM_COMMAND:        {	    
		switch(LOWORD(wparam))            
		{            
			case IDOK:		{		
				char FilterString[256];
				SkipProcessing = false;
				sprintf(FilterString, "%d", FilterIntensity);
				WritePrivateProfileStringA("AltaLux","Intensity",FilterString, SetupIniFile);
				sprintf(FilterString, "%d", FilterScale);
				WritePrivateProfileStringA("AltaLux","Scale",FilterString, SetupIniFile);
				EndDialog(hwnd,wparam);
				return TRUE;		
			}			// Check the rest of your dialog box controls here            
			case IDCANCEL : {		
				SkipProcessing = true;
				EndDialog(hwnd,wparam);
				return TRUE;		
			}
			case ID_DEFAULT : {
				FilterIntensity = AL_DEFAULT_STRENGTH;
				FilterScale = DEFAULT_HOR_REGIONS;
				UpdateSliders(hwnd);
				UpdateFilterDisplay(hwnd);
				DoProcessing();
				InvalidateRgn(hwnd, NULL, true);
				return TRUE;
			}
			case IDC_TOGGLEVISUALIZATION : {
				CompleteVisualization = !CompleteVisualization;
				InvalidateRgn(hwnd, NULL, true);
				return TRUE;
			}
		}
		}
	case WM_VSCROLL: {
		HWND hwndTrack = (HWND)lparam;
		switch (LOWORD(wparam)) { 
			case TB_THUMBTRACK:
				{
					int dwPos = SendMessage(hwndTrack, TBM_GETPOS, 0, 0); 
					if (hwndTrack == GetDlgItem(hwnd, IDC_SLIDER1))
						FilterIntensity = dwPos;
					if (hwndTrack == GetDlgItem(hwnd, IDC_SLIDER2))
						FilterScale = dwPos;
					UpdateFilterDisplay(hwnd);
					break;
				}
			case TB_ENDTRACK: 
				{
					int dwPos = SendMessage(hwndTrack, TBM_GETPOS, 0, 0); 
					if (hwndTrack == GetDlgItem(hwnd, IDC_SLIDER1))
						FilterIntensity = dwPos;
					if (hwndTrack == GetDlgItem(hwnd, IDC_SLIDER2))
						FilterScale = dwPos;

					UpdateFilterDisplay(hwnd);
					DoProcessing();
					InvalidateRgn(hwnd, NULL, true);
				}
			}
		return TRUE;
		}
	case WM_PAINT : {
		HandlePaintMessage(hwnd);
		return FALSE;
		}
	default: return FALSE;
	}
	return TRUE;
}

unsigned char *AllocateRGBImage(int ImageWidth, int ImageHeight)
{
	const int SECURITY_PADDING = 4096;

	if ((ImageWidth <= 0) || (ImageHeight <= 0))
		return NULL;

	unsigned char *AllocatedImage = NULL;
	try
	{
		AllocatedImage = (unsigned char *)malloc((ImageWidth * ImageHeight * 3) + SECURITY_PADDING);
	} catch (std::exception& e) {
	#ifdef ENABLE_LOGGING
		LOG(ERROR) << "Cannot create image buffer (427): " << e.what();
	#endif
		AllocatedImage = NULL;
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
		if ((ScaledImageWidth & 0x07) != 0) // fix for non-standard, multiple of 4 rescaled images that may not be drawn correctly
		{
		#ifdef ENABLE_LOGGING
			LOG(INFO) << "Scaled image width " << ScaledImageWidth << " is not a multiple of 4";
		#endif
			ScalingFactor--;
		} else
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
	} else {
		ScaledImageWidth = ImageWidth;
		ScaledImageHeight = ImageHeight;
	}
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
bool __cdecl StartEffects2(HANDLE hDib,HWND hwnd,int filter,RECT rect,int param1,int param2,char *iniFile,char *szAppName,int regID)
{
	#define WIDTHBYTES(bits) (((bits) + 31) / 32 * 4)

	const int SECURITY_PADDING = 4096;
	const int IMAGE_BPP = 3; // 24 bits per pixel only
	pbBmHdr= (LPBITMAPINFOHEADER)GlobalLock(hDib);
	BmHdrCopy = *pbBmHdr;
	if (pbBmHdr->biPlanes != 1)
	{
	#ifdef ENABLE_LOGGING
		LOG(ERROR) << "Only planar images are supported (478)";
	#endif
		GlobalUnlock(hDib);
		return false;
	}
	if (pbBmHdr->biBitCount != 24)
	{
	#ifdef ENABLE_LOGGING
		LOG(ERROR) << "Only 24-bit images are supported (484)";
	#endif
		GlobalUnlock(hDib);
		return false;
	}
	RECT ClipRect = rect;
	FullImageWidth = abs(pbBmHdr->biWidth);
	FullImageHeight = abs(pbBmHdr->biHeight);
	/// ClipRect is actually (left, top, width, height) !
	ImageWidth = ClipRect.right;
	ImageHeight = ClipRect.bottom;

	CroppedImage = false;
	if ((FullImageWidth > ImageWidth) || (FullImageHeight > ImageHeight))
		CroppedImage = true;
	else
	{
		/// crop on non-standard size images
		if ((FullImageWidth & 7) != 0)
			CroppedImage = true;
		if ((FullImageHeight & 7) != 0)
			CroppedImage = true;
	}
	if (CroppedImage)
	{		
		/// normalize clip rect
		ClipRect.bottom += ClipRect.top;
		ClipRect.right += ClipRect.left;
		ClipRect.right = ClipRect.right & ~7;
		ClipRect.left = ClipRect.left & ~7;
		ClipRect.bottom = ClipRect.bottom & ~7;
		ClipRect.top = ClipRect.top & ~7;
		ImageWidth = ClipRect.right - ClipRect.left;
		ImageHeight = ClipRect.bottom - ClipRect.top;
		BmHdrCopy.biWidth = ImageWidth;
		BmHdrCopy.biHeight = ImageHeight;
	#ifdef ENABLE_LOGGING
		LOG(INFO) << "Cropped image: width " << ImageWidth << " height " << ImageHeight;
	#endif
	}
	else {
	#ifdef ENABLE_LOGGING
		LOG(INFO) << "Full image: width " << ImageWidth << " height " << ImageHeight;
	#endif
	}

	BYTE *ImageBits = (BYTE *)pbBmHdr + (WORD)pbBmHdr->biSize;
	DWORD ImageBitsStride = WIDTHBYTES((DWORD)FullImageWidth * pbBmHdr->biBitCount);
	/// SrcImage
#ifdef ENABLE_LOGGING
	LOG(INFO) << "Allocating source image (553)";
#endif
	try
	{
		SrcImage = (unsigned char *)malloc((ImageWidth * ImageHeight * IMAGE_BPP) + SECURITY_PADDING);
	} catch (std::exception& e) {
		SrcImage = NULL;
	#ifdef ENABLE_LOGGING
		LOG(ERROR) << "Cannot create Source image (490): " << e.what();
	#endif
	}
	if (SrcImage == NULL)
	{
		GlobalUnlock(hDib);
		return false;
	}
	std::unique_ptr<unsigned char[]> SrcImageUniquePtr(SrcImage);

	// copy from ImageBits into SrcImage
#ifdef ENABLE_LOGGING
	LOG(INFO) << "Copying source image (568)";
#endif
	if (CroppedImage)
	{
		// copy only part of source image
		unsigned char *SrcImagePtr = SrcImageUniquePtr.get();
		unsigned char *ImageBitsPtr = ImageBits;
		ImageBitsPtr += ClipRect.left * IMAGE_BPP;
		ImageBitsPtr += ImageBitsStride * ClipRect.top;
		for (int y = ClipRect.top; y < ClipRect.bottom; y++)
		{
			memcpy(SrcImagePtr, ImageBitsPtr, ImageWidth * IMAGE_BPP);
			SrcImagePtr += ImageWidth * IMAGE_BPP;
			ImageBitsPtr += ImageBitsStride;
		}
	} else {
		// copy whole source image
		unsigned char *SrcImagePtr = SrcImageUniquePtr.get();
		unsigned char *ImageBitsPtr = ImageBits;
		for (int y = 0; y < FullImageHeight; y++)
		{
			memcpy(SrcImagePtr, ImageBitsPtr, ImageWidth * IMAGE_BPP);
			SrcImagePtr += ImageWidth * IMAGE_BPP;
			ImageBitsPtr += ImageBitsStride;
		}
	}

	/// ProcImage
#ifdef ENABLE_LOGGING
	LOG(INFO) << "Allocating processed image (594)";
#endif
	try
	{
		ProcImage = AllocateRGBImage(ImageWidth, ImageHeight);
	} catch (std::exception& e) {
		ProcImage = NULL;
	#ifdef ENABLE_LOGGING
		LOG(ERROR) << "Cannot create processed image (598): " << e.what();
	#endif
	}
	if (ProcImage == NULL)
	{
		GlobalUnlock(hDib);
		return false;
	}
	std::unique_ptr<unsigned char[]> ProcImageUniquePtr(ProcImage);
#ifdef ENABLE_LOGGING
	LOG(INFO) << "Copying processed image (606)";
#endif
	memcpy(ProcImageUniquePtr.get(), SrcImageUniquePtr.get(), ImageWidth * ImageHeight * IMAGE_BPP);

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
		FilterIntensity = GetPrivateProfileIntA("AltaLux","Intensity",AL_DEFAULT_STRENGTH, SetupIniFile);
		FilterScale = GetPrivateProfileIntA("AltaLux","Scale",DEFAULT_HOR_REGIONS, SetupIniFile);

		ComputeScalingFactor();

		ScaledSrcImage = AllocateRGBImage(ScaledImageWidth, ScaledImageHeight);
		if (ScaledSrcImage == NULL)
			return false;
		std::unique_ptr<unsigned char[]> ScaledSrcImageUniquePtr(ScaledSrcImage);

		ScaledProcImage = AllocateRGBImage(ScaledImageWidth, ScaledImageHeight);
		if (ScaledProcImage == NULL)
			return false;
		std::unique_ptr<unsigned char[]> ScaledProcImageUniquePtr(ScaledProcImage);

		// allocate buffer for processed image with coarser grid
		ScaledProcImageGridM = AllocateRGBImage(ScaledImageWidth, ScaledImageHeight);
		if (ScaledProcImageGridM == NULL)
			return false;
		std::unique_ptr<unsigned char[]> ScaledProcImageGridMUniquePtr(ScaledProcImageGridM);

		// allocate buffer for processed image with finer grid
		ScaledProcImageGridP = AllocateRGBImage(ScaledImageWidth, ScaledImageHeight);
		if (ScaledProcImageGridP == NULL)
			return false;
		std::unique_ptr<unsigned char[]> ScaledProcImageGridPUniquePtr(ScaledProcImageGridP);

		// allocate buffer for processed image with lesser intensity
		ScaledProcImageIntensityM = AllocateRGBImage(ScaledImageWidth, ScaledImageHeight);
		if (ScaledProcImageIntensityM == NULL)
			return false;
		std::unique_ptr<unsigned char[]> ScaledProcImageIntensityMUniquePtr(ScaledProcImageIntensityM);

		// allocate buffer for processed image with higher intensity
		ScaledProcImageIntensityP = AllocateRGBImage(ScaledImageWidth, ScaledImageHeight);
		if (ScaledProcImageIntensityP == NULL)
			return false;
		std::unique_ptr<unsigned char[]> ScaledProcImageIntensityPUniquePtr(ScaledProcImageIntensityP);

		ScaleDownImage(SrcImageUniquePtr.get(), ImageWidth, ImageHeight, ScaledSrcImageUniquePtr.get(), ScalingFactor);

		// copy scaled source image
		int ScaledImageSize = ScaledImageWidth * ScaledImageHeight * IMAGE_BPP;
		memcpy(ScaledProcImageUniquePtr.get(), ScaledSrcImageUniquePtr.get(), ScaledImageSize);
		memcpy(ScaledProcImageGridMUniquePtr.get(), ScaledSrcImageUniquePtr.get(), ScaledImageSize);
		memcpy(ScaledProcImageGridPUniquePtr.get(), ScaledSrcImageUniquePtr.get(), ScaledImageSize);
		memcpy(ScaledProcImageIntensityMUniquePtr.get(), ScaledSrcImageUniquePtr.get(), ScaledImageSize);
		memcpy(ScaledProcImageIntensityPUniquePtr.get(), ScaledSrcImageUniquePtr.get(), ScaledImageSize);

		int ret = DialogBox(hDll, MAKEINTRESOURCE(IDD_DIALOG1), hwnd, (DLGPROC)DlgProc);

		if (ret == -1)
			return false;

		if (SkipProcessing)
			return true;
		
		param1 = FilterIntensity;
		param2 = FilterScale;

		pbBmHdr= (LPBITMAPINFOHEADER)GlobalLock(hDib);  // lock again the DIB
		ImageBits = (BYTE *)pbBmHdr + (WORD)pbBmHdr->biSize;
	}

	// if the filter parameters are unspecified, revert to default ones
	if (param1 < 0)
		param1 = AL_DEFAULT_STRENGTH;
	if (param2 < 0)
		param2 = DEFAULT_HOR_REGIONS;

	try
	{
		std::unique_ptr<CBaseAltaLuxFilter> AltaLuxFilter(CAltaLuxFilterFactory::CreateAltaLuxFilter(ImageWidth, ImageHeight, param2, param2));
		AltaLuxFilter->SetStrength(param1);
		{
		#ifdef ENABLE_LOGGING
			TIMED_SCOPE(timerBlkObj, "Filter processing of full resolution image");
		#endif
			AltaLuxFilter->ProcessRGB24((void *)SrcImage);
		}
		// copy the image back into ImageBits
		if (CroppedImage)
		{
			unsigned char *SrcImagePtr = SrcImageUniquePtr.get();
			unsigned char *ImageBitsPtr = ImageBits;
			ImageBitsPtr += ClipRect.left * 3;
			ImageBitsPtr += ImageBitsStride * ClipRect.top;
			for (int y = ClipRect.top; y < ClipRect.bottom; y++)
			{
				memcpy(ImageBitsPtr, SrcImagePtr, ImageWidth * 3);
				SrcImagePtr += ImageWidth * 3;
				ImageBitsPtr += ImageBitsStride;
			}
		} else {
			unsigned char *SrcImagePtr = SrcImageUniquePtr.get();
			unsigned char *ImageBitsPtr = ImageBits;
			for (int y = 0; y < FullImageHeight; y++)
			{
				memcpy(ImageBitsPtr, SrcImagePtr, ImageWidth * 3);
				SrcImagePtr += ImageWidth * 3;
				ImageBitsPtr += ImageBitsStride;
			}
		}
	}
	catch (std::exception& e)
	{
	#ifdef ENABLE_LOGGING
		LOG(ERROR) << "Exception running filter instance (651): " << e.what();
	#endif
	}

	GlobalUnlock(hDib);
	return true;
}

/// <summary>
/// General information about the plug-in
/// </summary>
/// <param name="versionString">output buffer for version number</param>
/// <param name="fileFormats">output buffer for plug-in description</param>
/// <returns></returns>
int __cdecl GetPlugInInfo(char *versionString,char *fileFormats)
{
	sprintf(versionString,"1.08"); // your version-nr
	sprintf(fileFormats,"AltaLux image enhancement filter"); // some infos
	return 0;
}

bool __cdecl AltaLux_Effects(HANDLE hDib,HWND hwnd,int filter,RECT rect,int param1,int param2,char *iniFile,char *szAppName,int regID)
{
	return StartEffects2(hDib, hwnd, filter, rect, param1, param2, iniFile, szAppName, regID);
}
