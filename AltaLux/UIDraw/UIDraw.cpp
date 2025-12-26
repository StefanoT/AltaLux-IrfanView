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

#include "UIDraw.h"
#include <cstdio>
#include <tchar.h>

int RectWidth(RECT& RectToMeasure)
{
	return RectToMeasure.right - RectToMeasure.left;
}

int RectHeight(RECT& RectToMeasure)
{
	return RectToMeasure.bottom - RectToMeasure.top;
}

BITMAPINFOHEADER MakeBitmapInfo(const BITMAPINFOHEADER* src, int width, int height)
{
	BITMAPINFOHEADER info;
	memcpy(&info, src, sizeof(BITMAPINFOHEADER)); // copy all original header fields
	info.biWidth = ((width + 7) / 8) * 8;         // round up to nearest multiple of 8
	info.biHeight = height;
	return info;
}

/// <summary>
/// Draw a quad
/// </summary>
/// <param name="hdc"></param>
/// <param name="QuadRect"></param>
void DrawScaleQuad(HDC hdc, RECT QuadRect)
{
	const int QuadWidth = (QuadRect.right - QuadRect.left) / 8;
	const int QuadHeight = (QuadRect.bottom - QuadRect.top) / 8;
	HPEN hPen = CreatePen(PS_SOLID, 1, RGB(255, 0, 0));
	HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
	HBRUSH hOldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));

	MoveToEx(hdc, QuadRect.left, QuadRect.top, nullptr);
	LineTo(hdc, QuadRect.left + QuadWidth, QuadRect.top);

	MoveToEx(hdc, QuadRect.left, QuadRect.top, nullptr);
	LineTo(hdc, QuadRect.left, QuadRect.top + QuadHeight);

	MoveToEx(hdc, QuadRect.right, QuadRect.bottom, nullptr);
	LineTo(hdc, QuadRect.right - QuadWidth, QuadRect.bottom);

	MoveToEx(hdc, QuadRect.right, QuadRect.bottom, nullptr);
	LineTo(hdc, QuadRect.right, QuadRect.bottom - QuadHeight);

	SelectObject(hdc, hOldPen);
	SelectObject(hdc, hOldBrush);
	DeleteObject(hPen);
}

/// <summary>
/// Draw grid of quads showing scaling ratio
/// </summary>
/// <param name="hdc"></param>
/// <param name="rectTo"></param>
/// <param name="FilterScale"></param>
void DrawScaleGrid(HDC hdc, RECT rectTo, int FilterScale)
{
	const int BlitHeight = rectTo.bottom - rectTo.top;
	const int BlitWidth = rectTo.right - rectTo.left;
	for (auto y = 0; y < FilterScale; y++)
		for (auto x = 0; x < FilterScale; x++)
		{
			RECT QuadRect;
			QuadRect.left = rectTo.left + ((x * BlitWidth) / FilterScale);
			QuadRect.top = rectTo.top + ((y * BlitHeight) / FilterScale);
			QuadRect.right = rectTo.left + (((x + 1) * BlitWidth) / FilterScale);
			QuadRect.bottom = rectTo.top + (((y + 1) * BlitHeight) / FilterScale);
			DrawScaleQuad(hdc, QuadRect);
		}
}

/// <summary>
/// Draw an image using GDI
/// </summary>
/// <param name="hdc"></param>
/// <param name="pBmHdr"></param>
/// <param name="ImageToDraw"></param>
/// <param name="ImageWidth"></param>
/// <param name="ImageHeight"></param>
/// <param name="RectPosition"></param>
/// <param name="ShowGrid">if true, overlay a grid showing the FilterScale</param>
/// <param name="FilterScale"></param>
/// <param name="NoRescaling">if true, draw the central part of the image without any rescaling</param>
/// <remarks>NoRescaling works only if input image is wider and taller than drawing area</remarks>
void DrawSingleImage(HDC hdc, LPBITMAPINFOHEADER pBmHdr, void* ImageToDraw, int ImageWidth, int ImageHeight,
                     RECT RectPosition, bool ShowGrid, int FilterScale, bool NoRescaling, LPCWSTR Title)
{
	RECT rectTo;
	rectTo.left = rectTo.top = 0;

	if ((NoRescaling) && (ImageWidth > RectWidth(RectPosition)) && (ImageHeight > RectHeight(RectPosition)))
	{
		// draw central part of image with no rescaling
		SetStretchBltMode(hdc, COLORONCOLOR);
		BITMAPINFOHEADER ImageInfo = MakeBitmapInfo(pBmHdr, ImageWidth, ImageHeight);

		const int HorOffset = (ImageWidth - RectWidth(RectPosition)) >> 1;
		const int VerOffset = (ImageHeight - RectHeight(RectPosition)) >> 1;

		StretchDIBits(hdc, RectPosition.left, RectPosition.top, RectWidth(RectPosition), RectHeight(RectPosition), HorOffset, VerOffset,
			RectWidth(RectPosition), RectHeight(RectPosition), ImageToDraw, reinterpret_cast<BITMAPINFO *>(&ImageInfo), DIB_RGB_COLORS, SRCCOPY);

		SetTextColor(hdc, RGB(255, 255, 255));  // white text
		SetBkColor(hdc, RGB(0, 0, 0));          // black background
		TextOut(hdc, RectPosition.left, RectPosition.top, Title, static_cast<int>(wcslen(Title)));
	} else {
		// draw full image with rescaling
		float ScalingX = static_cast<float>(ImageWidth) / RectWidth(RectPosition);
		float ScalingY = static_cast<float>(ImageHeight) / RectHeight(RectPosition);
		const float MaxScaling = (ScalingX > ScalingY ? ScalingX : ScalingY);
		rectTo.right = static_cast<int>(ImageWidth / MaxScaling);
		rectTo.bottom = static_cast<int>(ImageHeight / MaxScaling);
		const int xOffset = max(0, (RectWidth(RectPosition) - rectTo.right)) >> 1;
		const int yOffset = max(0, (RectHeight(RectPosition) - rectTo.bottom)) >> 1;
		OffsetRect(&rectTo, xOffset, yOffset);
		OffsetRect(&rectTo, RectPosition.left, RectPosition.top);

		SetStretchBltMode(hdc, COLORONCOLOR);
		BITMAPINFOHEADER ImageInfo = MakeBitmapInfo(pBmHdr, ImageWidth, ImageHeight);
		StretchDIBits(hdc, rectTo.left, rectTo.top, RectWidth(rectTo), RectHeight(rectTo), 0, 0, ImageWidth,
			ImageHeight, ImageToDraw, reinterpret_cast<BITMAPINFO *>(&ImageInfo), DIB_RGB_COLORS, SRCCOPY);

		if (ShowGrid)
		{
			/// draw scale
			DrawScaleGrid(hdc, rectTo, FilterScale);
		}

		SetTextColor(hdc, RGB(255, 255, 255));  // white text
		SetBkColor(hdc, RGB(0, 0, 0));          // black background
		TextOut(hdc, rectTo.left, rectTo.top, Title, static_cast<int>(wcslen(Title)));
	}
}

void DrawImage(HWND hwnd, LPBITMAPINFOHEADER pBmHdr, void* ImageToDraw, int ImageWidth, int ImageHeight,
               int FilterScale)
{
	RECT rectClient;
	GetClientRect(hwnd, &rectClient);
	rectClient.right -= 100;
	RECT rectTo;
	rectTo.left = rectTo.top = 0;
	float ScalingX = static_cast<float>(ImageWidth) / (rectClient.right - rectClient.left);
	float ScalingY = static_cast<float>(ImageHeight) / (rectClient.bottom - rectClient.top);
	const float MaxScaling = (ScalingX > ScalingY ? ScalingX : ScalingY);
	rectTo.right = static_cast<int>(ImageWidth / MaxScaling);
	rectTo.bottom = static_cast<int>(ImageHeight / MaxScaling);
	const int xOffset = max(0, ((rectClient.right - rectClient.left) - rectTo.right)) >> 1;
	const int yOffset = max(0, ((rectClient.bottom - rectClient.top) - rectTo.bottom)) >> 1;
	OffsetRect(&rectTo, xOffset, yOffset);

	PAINTSTRUCT ps;
	HDC hdc = BeginPaint(hwnd, &ps);

	HRGN bgRgn = CreateRectRgnIndirect(&rectClient);
	HBRUSH hBrush = CreateSolidBrush(RGB(0,0,0));
	FillRgn(hdc, bgRgn, hBrush);
	DeleteObject(bgRgn);
	DeleteObject(hBrush);

	if (ScalingX > ScalingY)
	{
		char CopyMsg[256];
		sprintf_s(CopyMsg, sizeof(CopyMsg), "AltaLux technology by Stefano Tommesani (www.tommesani.com)");
		//set text background
		SetTextColor(ps.hdc, RGB(128, 128, 128));
		SetBkMode(ps.hdc, TRANSPARENT);
		//paint text
		DrawTextA(ps.hdc, CopyMsg, static_cast<int>(strlen(CopyMsg)), &rectClient, DT_BOTTOM | DT_SINGLELINE | DT_CENTER);
	}
	else
	{
		// do not draw any text as it would overlap image
	}

	SetStretchBltMode(hdc, COLORONCOLOR);
	StretchDIBits(hdc, rectTo.left, rectTo.top, rectTo.right - rectTo.left, rectTo.bottom - rectTo.top, 0, 0, ImageWidth,
	              ImageHeight, ImageToDraw, reinterpret_cast<BITMAPINFO *>(pBmHdr), DIB_RGB_COLORS, SRCCOPY);

	/// draw scale
	DrawScaleGrid(hdc, rectTo, FilterScale);

	EndPaint(hwnd, &ps);
}
