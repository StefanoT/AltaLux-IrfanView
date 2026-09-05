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
#include <vector>

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

	// Resamples the source image to targetWidth x targetHeight by averaging the
	// source rectangle each destination pixel covers (nearest sampling on axes
	// the destination magnifies), the same box-filter approach as the kernel
	// ScaleDownBox but with an arbitrary destination size. Row order is
	// preserved, so the result draws with the same header orientation as the
	// source. targetStride is the byte pitch of each target row.
	void DownscaleAreaAverage(const unsigned char* source, int sourceWidth, int sourceHeight, int pixelStride,
	                          int targetWidth, int targetHeight, int targetStride, std::vector<unsigned char>& target)
	{
		target.resize(static_cast<std::size_t>(targetStride) * targetHeight);

		const auto mapAxis = [](int targetCount, int sourceCount, std::vector<int>& starts, std::vector<int>& ends)
		{
			starts.resize(static_cast<std::size_t>(targetCount));
			ends.resize(static_cast<std::size_t>(targetCount));
			for (int i = 0; i < targetCount; ++i)
			{
				const int start = static_cast<int>((static_cast<long long>(i) * sourceCount) / targetCount);
				int end = static_cast<int>((static_cast<long long>(i + 1) * sourceCount + targetCount - 1) / targetCount);
				if (end <= start)
				{
					end = start + 1;
				}
				if (end > sourceCount)
				{
					end = sourceCount;
				}
				starts[static_cast<std::size_t>(i)] = start;
				ends[static_cast<std::size_t>(i)] = end;
			}
		};

		std::vector<int> columnStart(targetWidth);
		std::vector<int> columnEnd(targetWidth);
		std::vector<int> rowStart(targetHeight);
		std::vector<int> rowEnd(targetHeight);
		mapAxis(targetWidth, sourceWidth, columnStart, columnEnd);
		mapAxis(targetHeight, sourceHeight, rowStart, rowEnd);

		for (int y = 0; y < targetHeight; ++y)
		{
			unsigned char* targetRow = target.data() + static_cast<std::size_t>(y) * targetStride;
			for (int x = 0; x < targetWidth; ++x)
			{
				unsigned int channelSum[4] = {};
				for (int sy = rowStart[static_cast<std::size_t>(y)]; sy < rowEnd[static_cast<std::size_t>(y)]; ++sy)
				{
					const unsigned char* sourceRow = source + static_cast<std::size_t>(sy) * sourceWidth * pixelStride;
					for (int sx = columnStart[static_cast<std::size_t>(x)]; sx < columnEnd[static_cast<std::size_t>(x)]; ++sx)
					{
						const unsigned char* pixel = sourceRow + static_cast<std::size_t>(sx) * pixelStride;
						channelSum[0] += pixel[0];
						channelSum[1] += pixel[1];
						channelSum[2] += pixel[2];
						if (pixelStride == 4)
						{
							channelSum[3] += pixel[3];
						}
					}
				}
				const unsigned int sampleCount = static_cast<unsigned int>(
					(rowEnd[static_cast<std::size_t>(y)] - rowStart[static_cast<std::size_t>(y)]) *
					(columnEnd[static_cast<std::size_t>(x)] - columnStart[static_cast<std::size_t>(x)]));
				unsigned char* targetPixel = targetRow + static_cast<std::size_t>(x) * pixelStride;
				targetPixel[0] = static_cast<unsigned char>((channelSum[0] + sampleCount / 2) / sampleCount);
				targetPixel[1] = static_cast<unsigned char>((channelSum[1] + sampleCount / 2) / sampleCount);
				targetPixel[2] = static_cast<unsigned char>((channelSum[2] + sampleCount / 2) / sampleCount);
				if (pixelStride == 4)
				{
					targetPixel[3] = static_cast<unsigned char>((channelSum[3] + sampleCount / 2) / sampleCount);
				}
			}
		}
	}

	void DrawBitmapRegion(HDC hdc, LPBITMAPINFOHEADER pBmHdr, void* imageToDraw, int imageWidth, int imageHeight,
	                      const RECT& destinationRect, PreviewDisplayCache& displayCache)
	{
		const int destinationWidth = RectWidth(destinationRect);
		const int destinationHeight = RectHeight(destinationRect);
		if (imageToDraw == nullptr || destinationWidth <= 0 || destinationHeight <= 0)
		{
			return;
		}

		// Magnification and 1:1 keep the direct GDI path: nearest-neighbour
		// sampling is the expected behaviour when zooming in.
		if (destinationWidth >= imageWidth && destinationHeight >= imageHeight)
		{
			BITMAPINFOHEADER imageInfo = MakeBitmapInfo(pBmHdr, imageWidth, imageHeight);
			SetStretchBltMode(hdc, COLORONCOLOR);
			StretchDIBits(hdc, destinationRect.left, destinationRect.top, destinationWidth, destinationHeight,
			              0, 0, imageWidth, imageHeight, imageToDraw, reinterpret_cast<BITMAPINFO*>(&imageInfo),
			              DIB_RGB_COLORS, SRCCOPY);
			return;
		}

		// Minification through GDI's COLORONCOLOR drops pixels and aliases, so
		// resample the image to the exact destination size once and blit 1:1.
		// The cache is keyed by destination size; callers clear it when the
		// source content changes.
		if (displayCache.width != destinationWidth || displayCache.height != destinationHeight ||
			displayCache.pixels.empty())
		{
			const int pixelStride = pBmHdr->biBitCount == 32 ? 4 : 3;
			const int displayStride = ((destinationWidth * pixelStride) + 3) / 4 * 4;
			DownscaleAreaAverage(static_cast<const unsigned char*>(imageToDraw), imageWidth, imageHeight, pixelStride,
			                     destinationWidth, destinationHeight, displayStride, displayCache.pixels);
			displayCache.width = destinationWidth;
			displayCache.height = destinationHeight;
		}

		BITMAPINFOHEADER displayInfo = {};
		displayInfo.biSize = sizeof(BITMAPINFOHEADER);
		displayInfo.biWidth = destinationWidth;
		displayInfo.biHeight = destinationHeight;
		displayInfo.biPlanes = 1;
		displayInfo.biBitCount = pBmHdr->biBitCount;
		displayInfo.biCompression = BI_RGB;
		StretchDIBits(hdc, destinationRect.left, destinationRect.top, destinationWidth, destinationHeight,
		              0, 0, destinationWidth, destinationHeight, displayCache.pixels.data(),
		              reinterpret_cast<BITMAPINFO*>(&displayInfo), DIB_RGB_COLORS, SRCCOPY);
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
                      const RECT& ImageRect, PreviewDisplayCache& DisplayCache)
{
	// The image rect may overflow the preview rect when zoomed/panned; clipping
	// to the preview frame is owned by the caller via SaveDC/IntersectClipRect.
	DrawBitmapRegion(hdc, pBmHdr, ImageToDraw, ImageWidth, ImageHeight, ImageRect, DisplayCache);
}

void ClearPreviewDisplayCache(PreviewDisplayCache& cache)
{
	cache.pixels.clear();
	cache.width = 0;
	cache.height = 0;
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
                               int SplitX, bool CompareHoldOriginal, bool DarkMode,
                               PreviewDisplayCache& OriginalCache, PreviewDisplayCache& ProcessedCache)
{
	const RECT visibleRect = VisibleRectOf(PreviewRect, ImageRect);
	if (RectWidth(visibleRect) <= 0 || RectHeight(visibleRect) <= 0)
	{
		return;
	}

	const int savedDc = SaveDC(hdc);
	IntersectClipRect(hdc, visibleRect.left, visibleRect.top, visibleRect.right, visibleRect.bottom);
	DrawPreviewImage(hdc, pBmHdr, OriginalImage, ImageWidth, ImageHeight, ImageRect, OriginalCache);

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
		DrawPreviewImage(hdc, pBmHdr, ProcessedImage, ImageWidth, ImageHeight, ImageRect, ProcessedCache);
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
