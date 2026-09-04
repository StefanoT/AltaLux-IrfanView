/*
Project: AltaLux plugin for IrfanView
Author: Stefano Tommesani
Website: http://www.tommesani.com

Microsoft Public License (MS-PL) [OSI Approved License]
The full license text is in the LICENSE file at the root of the repository.
*/

#pragma once

#include <Windows.h>
#include "..\AltaLuxCore.h"

// Sets the DPI used to scale overlay chrome (split handle, preview labels);
// defaults to 96 and must be updated on WM_DPICHANGED by the dialog owner.
void SetUIDrawDpi(UINT dpi);

void DrawPreviewImage(HDC hdc, LPBITMAPINFOHEADER pBmHdr, void* ImageToDraw, int ImageWidth, int ImageHeight,
                      const RECT& ImageRect);
void DrawSplitHandle(HDC hdc, const RECT& PreviewRect, int SplitX, bool DarkMode);
void DrawMainPreviewComparison(HDC hdc, LPBITMAPINFOHEADER pBmHdr, void* OriginalImage, void* ProcessedImage,
                               int ImageWidth, int ImageHeight, const RECT& PreviewRect, const RECT& ImageRect,
                               int SplitX, bool CompareHoldOriginal, bool DarkMode);

int RectWidth(const RECT& RectToMeasure);
int RectHeight(const RECT& RectToMeasure);
