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
	UINT gUIDrawDpi = USER_DEFAULT_SCREEN_DPI;

	int ScaleForDpi(int value)
	{
		return MulDiv(value, gUIDrawDpi, USER_DEFAULT_SCREEN_DPI);
	}

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
		textRect.left += ScaleForDpi(12);
		textRect.right -= ScaleForDpi(12);
		textRect.top += ScaleForDpi(10);
		textRect.bottom = textRect.top + ScaleForDpi(20);

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

	RECT VisibleRectOf(const RECT& previewRect, const RECT& imageRect)
	{
		RECT visible = previewRect;
		IntersectRect(&visible, &previewRect, &imageRect);
		return visible;
	}
}

void DrawPreviewImage(HDC hdc, LPBITMAPINFOHEADER pBmHdr, void* ImageToDraw, int ImageWidth, int ImageHeight,
                      const RECT& ImageRect)
{
	// The image rect may overflow the preview rect when zoomed/panned; clipping
	// to the preview frame is owned by the caller via SaveDC/IntersectClipRect.
	DrawBitmapRegion(hdc, pBmHdr, ImageToDraw, ImageWidth, ImageHeight, ImageRect, 0, 0, ImageWidth, ImageHeight);
}

void SetUIDrawDpi(UINT dpi)
{
	gUIDrawDpi = dpi != 0 ? dpi : USER_DEFAULT_SCREEN_DPI;
}

void DrawSplitHandle(HDC hdc, const RECT& PreviewRect, int SplitX, bool DarkMode)
{
	const COLORREF lineColor = DarkMode ? RGB(230, 230, 230) : RGB(32, 32, 32);
	const COLORREF gripColor = DarkMode ? RGB(60, 60, 60) : RGB(245, 245, 245);
	const int gripHalfWidth = ScaleForDpi(8);
	const int gripHalfHeight = ScaleForDpi(16);
	const int gripInset = ScaleForDpi(8);
	const int gripLineStep = ScaleForDpi(3);

	HPEN pen = CreatePen(PS_SOLID, ScaleForDpi(2), lineColor);
	HBRUSH brush = CreateSolidBrush(gripColor);
	HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, pen));
	HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(hdc, brush));

	MoveToEx(hdc, SplitX, PreviewRect.top, nullptr);
	LineTo(hdc, SplitX, PreviewRect.bottom);

	RECT gripRect = { SplitX - gripHalfWidth, PreviewRect.top + (RectHeight(PreviewRect) / 2) - gripHalfHeight,
	                  SplitX + gripHalfWidth, PreviewRect.top + (RectHeight(PreviewRect) / 2) + gripHalfHeight };
	RoundRect(hdc, gripRect.left, gripRect.top, gripRect.right, gripRect.bottom, ScaleForDpi(6), ScaleForDpi(6));

	for (int lineOffset = -gripLineStep; lineOffset <= gripLineStep; lineOffset += gripLineStep)
	{
		MoveToEx(hdc, SplitX + lineOffset, gripRect.top + gripInset, nullptr);
		LineTo(hdc, SplitX + lineOffset, gripRect.bottom - gripInset);
	}

	SelectObject(hdc, oldBrush);
	SelectObject(hdc, oldPen);
	DeleteObject(brush);
	DeleteObject(pen);
}

void DrawMainPreviewComparison(HDC hdc, LPBITMAPINFOHEADER pBmHdr, void* OriginalImage, void* ProcessedImage,
                               int ImageWidth, int ImageHeight, const RECT& PreviewRect, const RECT& ImageRect,
                               int SplitX, bool CompareHoldOriginal, bool DarkMode)
{
	const RECT visibleRect = VisibleRectOf(PreviewRect, ImageRect);
	if (RectWidth(visibleRect) <= 0 || RectHeight(visibleRect) <= 0)
	{
		return;
	}

	const int savedDc = SaveDC(hdc);
	IntersectClipRect(hdc, visibleRect.left, visibleRect.top, visibleRect.right, visibleRect.bottom);
	DrawPreviewImage(hdc, pBmHdr, OriginalImage, ImageWidth, ImageHeight, ImageRect);

	if (!CompareHoldOriginal)
	{
		int clampedSplit = SplitX;
		if (clampedSplit < visibleRect.left)
		{
			clampedSplit = visibleRect.left;
		}
		if (clampedSplit > visibleRect.right)
		{
			clampedSplit = visibleRect.right;
		}
		const int splitDc = SaveDC(hdc);
		IntersectClipRect(hdc, clampedSplit, visibleRect.top, visibleRect.right, visibleRect.bottom);
		DrawPreviewImage(hdc, pBmHdr, ProcessedImage, ImageWidth, ImageHeight, ImageRect);
		RestoreDC(hdc, splitDc);

		DrawSplitHandle(hdc, visibleRect, clampedSplit, DarkMode);
		DrawPreviewLabel(hdc, visibleRect, L"Before", DarkMode, DT_LEFT);
		DrawPreviewLabel(hdc, visibleRect, L"After", DarkMode, DT_RIGHT);
	}
	else
	{
		DrawPreviewLabel(hdc, visibleRect, L"Before", DarkMode, DT_LEFT);
	}
	RestoreDC(hdc, savedDc);
}
