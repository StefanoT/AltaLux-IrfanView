/*
Project: AltaLux plugin for IrfanView
Author: Stefano Tommesani
Website: http://www.tommesani.com

Microsoft Public License (MS-PL) [OSI Approved License]
The full license text is in the LICENSE file at the root of the repository.
*/

#include "UIDraw.h"

#include <algorithm>
#include <cstring>
#include <cwchar>

int RectWidth(const RECT& RectToMeasure)
{
	return RectToMeasure.right - RectToMeasure.left;
}

int RectHeight(const RECT& RectToMeasure)
{
	return RectToMeasure.bottom - RectToMeasure.top;
}

namespace
{
	BITMAPINFOHEADER MakeBitmapInfo(const BITMAPINFOHEADER* src, int width, int height)
	{
		BITMAPINFOHEADER info;
		memcpy(&info, src, sizeof(BITMAPINFOHEADER));
		info.biWidth = ((width + 7) / 8) * 8;
		info.biHeight = height;
		return info;
	}

	void DrawBitmapRegion(HDC hdc, LPBITMAPINFOHEADER pBmHdr, void* imageToDraw, int imageWidth, int imageHeight,
	                      const RECT& destinationRect, int srcX, int srcY, int srcWidth, int srcHeight)
	{
		if (imageToDraw == nullptr || srcWidth <= 0 || srcHeight <= 0 || RectWidth(destinationRect) <= 0 || RectHeight(destinationRect) <= 0)
		{
			return;
		}

		BITMAPINFOHEADER imageInfo = MakeBitmapInfo(pBmHdr, imageWidth, imageHeight);
		SetStretchBltMode(hdc, COLORONCOLOR);
		StretchDIBits(hdc, destinationRect.left, destinationRect.top, RectWidth(destinationRect), RectHeight(destinationRect),
		              srcX, srcY, srcWidth, srcHeight, imageToDraw, reinterpret_cast<BITMAPINFO*>(&imageInfo),
		              DIB_RGB_COLORS, SRCCOPY);
	}

	void DrawPreviewLabel(HDC hdc, const RECT& previewRect, LPCWSTR label, bool /*darkMode*/, UINT format)
	{
		RECT textRect = previewRect;
		textRect.left += 12;
		textRect.right -= 12;
		textRect.top += 10;
		textRect.bottom = textRect.top + 20;

		SetBkMode(hdc, TRANSPARENT);

		const int labelLength = static_cast<int>(wcslen(label));
		const UINT drawFlags = format | DT_VCENTER | DT_SINGLELINE;

		// Paint a shadow first so the caption stays legible on top of bright/dark
		// image regions. Two offsets give a softer halo without needing AlphaBlend.
		SetTextColor(hdc, RGB(0, 0, 0));
		for (int offset = 1; offset <= 2; ++offset)
		{
			RECT shadowRect = textRect;
			OffsetRect(&shadowRect, offset, offset);
			DrawTextW(hdc, label, labelLength, &shadowRect, drawFlags);
		}

		SetTextColor(hdc, RGB(245, 245, 245));
		DrawTextW(hdc, label, labelLength, &textRect, drawFlags);
	}
}

void DrawPreviewImage(HDC hdc, LPBITMAPINFOHEADER pBmHdr, void* ImageToDraw, int ImageWidth, int ImageHeight,
                      const RECT& RectPosition, bool NoRescaling)
{
	if (ImageToDraw == nullptr)
	{
		return;
	}

	if (NoRescaling && ImageWidth > RectWidth(RectPosition) && ImageHeight > RectHeight(RectPosition))
	{
		const int srcX = (ImageWidth - RectWidth(RectPosition)) / 2;
		const int srcY = (ImageHeight - RectHeight(RectPosition)) / 2;
		DrawBitmapRegion(hdc, pBmHdr, ImageToDraw, ImageWidth, ImageHeight, RectPosition, srcX, srcY,
		                 RectWidth(RectPosition), RectHeight(RectPosition));
		return;
	}

	const RECT fittedRect = GetPreviewImageRect(ImageWidth, ImageHeight, RectPosition, NoRescaling);
	DrawBitmapRegion(hdc, pBmHdr, ImageToDraw, ImageWidth, ImageHeight, fittedRect, 0, 0, ImageWidth, ImageHeight);
}

void DrawSplitHandle(HDC hdc, const RECT& PreviewRect, int SplitX, bool DarkMode)
{
	const COLORREF lineColor = DarkMode ? RGB(230, 230, 230) : RGB(32, 32, 32);
	const COLORREF gripColor = DarkMode ? RGB(60, 60, 60) : RGB(245, 245, 245);

	HPEN pen = CreatePen(PS_SOLID, 2, lineColor);
	HBRUSH brush = CreateSolidBrush(gripColor);
	HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, pen));
	HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(hdc, brush));

	MoveToEx(hdc, SplitX, PreviewRect.top, nullptr);
	LineTo(hdc, SplitX, PreviewRect.bottom);

	RECT gripRect = { SplitX - 8, PreviewRect.top + (RectHeight(PreviewRect) / 2) - 16, SplitX + 8,
	                  PreviewRect.top + (RectHeight(PreviewRect) / 2) + 16 };
	RoundRect(hdc, gripRect.left, gripRect.top, gripRect.right, gripRect.bottom, 6, 6);

	MoveToEx(hdc, SplitX - 3, gripRect.top + 8, nullptr);
	LineTo(hdc, SplitX - 3, gripRect.bottom - 8);
	MoveToEx(hdc, SplitX, gripRect.top + 8, nullptr);
	LineTo(hdc, SplitX, gripRect.bottom - 8);
	MoveToEx(hdc, SplitX + 3, gripRect.top + 8, nullptr);
	LineTo(hdc, SplitX + 3, gripRect.bottom - 8);

	SelectObject(hdc, oldBrush);
	SelectObject(hdc, oldPen);
	DeleteObject(brush);
	DeleteObject(pen);
}

void DrawMainPreviewComparison(HDC hdc, LPBITMAPINFOHEADER pBmHdr, void* OriginalImage, void* ProcessedImage,
                               int ImageWidth, int ImageHeight, const RECT& PreviewRect, int SplitX,
                               bool CompareHoldOriginal, bool NoRescaling, bool DarkMode)
{
	const RECT imageRect = GetPreviewImageRect(ImageWidth, ImageHeight, PreviewRect, NoRescaling);
	DrawPreviewImage(hdc, pBmHdr, OriginalImage, ImageWidth, ImageHeight, PreviewRect, NoRescaling);

	if (CompareHoldOriginal)
	{
		DrawPreviewLabel(hdc, imageRect, L"Before", DarkMode, DT_LEFT);
		return;
	}

	int clampedSplit = SplitX;
	if (clampedSplit < imageRect.left)
	{
		clampedSplit = imageRect.left;
	}
	if (clampedSplit > imageRect.right)
	{
		clampedSplit = imageRect.right;
	}
	const int savedDc = SaveDC(hdc);
	IntersectClipRect(hdc, clampedSplit, imageRect.top, imageRect.right, imageRect.bottom);
	DrawPreviewImage(hdc, pBmHdr, ProcessedImage, ImageWidth, ImageHeight, PreviewRect, NoRescaling);
	RestoreDC(hdc, savedDc);

	DrawSplitHandle(hdc, imageRect, clampedSplit, DarkMode);
	DrawPreviewLabel(hdc, imageRect, L"Before", DarkMode, DT_LEFT);
	DrawPreviewLabel(hdc, imageRect, L"After", DarkMode, DT_RIGHT);
}
