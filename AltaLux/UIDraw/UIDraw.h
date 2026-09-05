/*
Project: AltaLux plugin for IrfanView
Author: Stefano Tommesani
Website: http://www.tommesani.com

Microsoft Public License (MS-PL) [OSI Approved License]
The full license text is in the LICENSE file at the root of the repository.
*/

#pragma once

#include <Windows.h>
#include <vector>
#include "..\AltaLuxCore.h"

// Sets the DPI used to scale overlay chrome (split handle, preview labels);
// defaults to 96 and must be updated on WM_DPICHANGED by the dialog owner.
void SetUIDrawDpi(UINT dpi);

// Scratch memory for the display-resolution preview, owned by the caller.
// Pass one instance per drawn image and clear it whenever the source image
// content changes; the next draw rebuilds it at the current destination size.
struct PreviewDisplayCache
{
	std::vector<unsigned char> pixels;
	int width = 0;
	int height = 0;
};

void ClearPreviewDisplayCache(PreviewDisplayCache& cache);

void DrawPreviewImage(HDC hdc, LPBITMAPINFOHEADER pBmHdr, void* ImageToDraw, int ImageWidth, int ImageHeight,
                      const RECT& ImageRect, PreviewDisplayCache& DisplayCache);
void DrawSplitHandle(HDC hdc, const RECT& PreviewRect, int SplitX, bool DarkMode);
void DrawMainPreviewComparison(HDC hdc, LPBITMAPINFOHEADER pBmHdr, void* OriginalImage, void* ProcessedImage,
                               int ImageWidth, int ImageHeight, const RECT& PreviewRect, const RECT& ImageRect,
                               int SplitX, bool CompareHoldOriginal, bool DarkMode,
                               PreviewDisplayCache& OriginalCache, PreviewDisplayCache& ProcessedCache);

int RectWidth(const RECT& RectToMeasure);
int RectHeight(const RECT& RectToMeasure);
