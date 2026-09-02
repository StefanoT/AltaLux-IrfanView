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

void DrawPreviewImage(HDC hdc, LPBITMAPINFOHEADER pBmHdr, void* ImageToDraw, int ImageWidth, int ImageHeight,
                      const RECT& RectPosition, bool NoRescaling);
void DrawSplitHandle(HDC hdc, const RECT& PreviewRect, int SplitX, bool DarkMode);
void DrawMainPreviewComparison(HDC hdc, LPBITMAPINFOHEADER pBmHdr, void* OriginalImage, void* ProcessedImage,
                               int ImageWidth, int ImageHeight, const RECT& PreviewRect, int SplitX,
                               bool CompareHoldOriginal, bool NoRescaling, bool DarkMode);

int RectWidth(const RECT& RectToMeasure);
int RectHeight(const RECT& RectToMeasure);
