/*
Project: AltaLux plugin for IrfanView
Author: Stefano Tommesani
Website: http://www.tommesani.com

Microsoft Public License (MS-PL) [OSI Approved License]
*/

#include "stdafx.h"
#include "AltaLux.h"
#include "resource.h"

#include <Commctrl.h>
#include <windowsx.h>
#include <Uxtheme.h>
#include <dwmapi.h>
#include <vssym32.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <memory>
#include <vector>

#include "Filter/CAltaLuxFilterFactory.h"
#include "Filter/CBaseAltaLuxFilter.h"
#include "Kernels/AltaLuxKernels.h"
#include "ScopedBitmapHeader.h"
#include "AltaLuxCore.h"
#include "UIDraw/UIDraw.h"

#pragma comment(lib, "Dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

using WeakImagePtr = std::weak_ptr<std::vector<unsigned char>>;
using SharedImagePtr = std::shared_ptr<std::vector<unsigned char>>;

namespace
{
	const int SECURITY_PADDING = 4096;
	const int PREVIEW_MARGIN = 16;
	const int PANEL_GAP = 18;
	const int PANEL_WIDTH = 240;
	const int SPLIT_HIT_RADIUS = 8;
	const int PRESET_TOLERANCE = 3;
	const UINT_PTR PREVIEW_REFRESH_TIMER_ID = 1;
	const UINT PREVIEW_REFRESH_DELAY_MS = 40;

	const std::array<Preset, 3> Presets = { {
		{ IDC_PRESET_NATURAL, L"Natural", 35, 10, 55 },
		{ IDC_PRESET_BALANCED, L"Balanced", 45, 25, 25 },
		{ IDC_PRESET_DETAIL, L"Detail", 55, 60, 10 }
	} };

	HINSTANCE hDll = nullptr;
	BITMAPINFOHEADER BmHdrCopy = {};
	int ImageWidth = 0;
	int ImageHeight = 0;
	int ImageBitDepth = 0;
	int FullImageWidth = 0;
	int FullImageHeight = 0;
	int ScaledImageWidth = 0;
	int ScaledImageHeight = 0;
	int ScalingFactor = 1;
	bool CroppedImage = false;
	bool SkipProcessing = false;

	WeakImagePtr ScaledSrcImagePtr;
	WeakImagePtr ScaledProcImagePtr;

	UiState gUiState = {
		Constants::DefaultStrength,
		Constants::DefaultDetail,
		Constants::DefaultNatural,
		false,
		false,
		false,
		0
	};

	char SetupIniFile[1024] = {};

	class ScopedBrush
	{
	public:
		explicit ScopedBrush(COLORREF color) : brush_(CreateSolidBrush(color)) {}
		~ScopedBrush()
		{
			if (brush_ != nullptr)
			{
				DeleteObject(brush_);
			}
		}

		HBRUSH get() const
		{
			return brush_;
		}

	private:
		HBRUSH brush_;
	};

	enum class ThemeMode { Light, Dark };

	ThemeMode gCurrentTheme = ThemeMode::Light;
	std::unique_ptr<ScopedBrush> gBackgroundBrush = std::make_unique<ScopedBrush>(RGB(255, 255, 255));

	HHOOK gKeyboardHook = nullptr;
	HWND gHookDlg = nullptr;

	int GetImageByteCount(int width, int height)
	{
		return width * height * ImageBitDepth;
	}

	int GetRGBImageSize(int width, int height)
	{
		return GetImageByteCount(width, height) + SECURITY_PADDING;
	}

	RECT GetPreviewRect(HWND hwnd)
	{
		RECT clientRect = {};
		GetClientRect(hwnd, &clientRect);

		RECT previewRect = {};
		previewRect.left = PREVIEW_MARGIN;
		previewRect.top = PREVIEW_MARGIN;
		const int minimumPreviewRight = previewRect.left + 320;
		const int preferredPreviewRight = clientRect.right - PANEL_WIDTH - PANEL_GAP - PREVIEW_MARGIN;
		previewRect.right = preferredPreviewRight > minimumPreviewRight ? preferredPreviewRight : minimumPreviewRight;
		previewRect.bottom = clientRect.bottom - PREVIEW_MARGIN;
		return previewRect;
	}

	RECT GetActiveImageRect(HWND hwnd)
	{
		return GetPreviewImageRect(ScaledImageWidth, ScaledImageHeight, GetPreviewRect(hwnd), gUiState.zoomToSelection);
	}

	void CenterSplitInPreview(HWND hwnd)
	{
		const RECT imageRect = GetActiveImageRect(hwnd);
		gUiState.splitX = imageRect.left + (RectWidth(imageRect) / 2);
	}

	void ClampSplitToPreview(HWND hwnd)
	{
		const RECT imageRect = GetActiveImageRect(hwnd);
		if (imageRect.right <= imageRect.left)
		{
			return;
		}

		// Initialize the split to the middle only when it has never been placed;
		// otherwise clamp to the edges so a drag beyond the image sticks there
		// instead of snapping back to center on every WM_MOUSEMOVE.
		if (gUiState.splitX <= 0)
		{
			CenterSplitInPreview(hwnd);
			return;
		}

		gUiState.splitX = ClampInt(gUiState.splitX, imageRect.left, imageRect.right);
	}

	void InvalidatePreview(HWND hwnd)
	{
		const RECT previewRect = GetPreviewRect(hwnd);
		InvalidateRect(hwnd, &previewRect, FALSE);
	}

	bool IsDarkModeEnabled()
	{
		DWORD value = 0;
		DWORD valueSize = sizeof(value);
		const LSTATUS status = RegGetValueW(
			HKEY_CURRENT_USER,
			L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
			L"AppsUseLightTheme",
			RRF_RT_DWORD,
			nullptr,
			&value,
			&valueSize);
		return status == ERROR_SUCCESS && value == 0;
	}

	void AdjustForDarkMode(HWND hwnd)
	{
		const bool darkMode = IsDarkModeEnabled();
		const ThemeMode targetTheme = darkMode ? ThemeMode::Dark : ThemeMode::Light;
		if (targetTheme == gCurrentTheme)
		{
			return;
		}

		gCurrentTheme = targetTheme;
		gBackgroundBrush = std::make_unique<ScopedBrush>(darkMode ? RGB(32, 32, 32) : RGB(255, 255, 255));
		RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
	}

	void CopyScaledSrcImage(unsigned char* targetImage)
	{
		auto scaledSrc = ScaledSrcImagePtr.lock();
		if (targetImage == nullptr || scaledSrc == nullptr)
		{
			return;
		}

		memcpy(targetImage, scaledSrc->data(), GetImageByteCount(ScaledImageWidth, ScaledImageHeight));
	}

	void DoPreviewProcessingV2()
	{
		auto scaledSrc = ScaledSrcImagePtr.lock();
		auto scaledProc = ScaledProcImagePtr.lock();
		if (scaledSrc == nullptr || scaledProc == nullptr)
		{
			return;
		}

		ProcessMultiscaleImage(scaledSrc->data(), scaledProc->data(), ScaledImageWidth, ScaledImageHeight, ImageBitDepth, gUiState);
	}

	void ApplyPreset(const Preset& preset)
	{
		ApplyPreset(gUiState, preset);
	}

	bool IsPresetActive(const Preset& preset)
	{
		return IsPresetActive(gUiState, preset, PRESET_TOLERANCE);
	}

	void SetButtonText(HWND hwnd, int controlId, const wchar_t* text)
	{
		SetWindowTextW(GetDlgItem(hwnd, controlId), text);
	}

	void UpdatePresetButtons(HWND hwnd)
	{
		for (const auto& preset : Presets)
		{
			if (IsPresetActive(preset))
			{
				wchar_t activeLabel[32] = {};
				swprintf_s(activeLabel, L"[%s]", preset.name);
				SetButtonText(hwnd, preset.buttonId, activeLabel);
			}
			else
			{
				SetButtonText(hwnd, preset.buttonId, preset.name);
			}
		}
	}

	void UpdateCommandLabels(HWND hwnd)
	{
		SetButtonText(hwnd, IDC_TOGGLEZOOM, gUiState.zoomToSelection ? L"Zoom: 1:1" : L"Zoom: Fit");
	}

	void UpdateSliders(HWND hwnd)
	{
		SendMessage(GetDlgItem(hwnd, IDC_STRENGTH_SLIDER), TBM_SETPOS, TRUE, gUiState.strength);
		SendMessage(GetDlgItem(hwnd, IDC_DETAIL_SLIDER), TBM_SETPOS, TRUE, gUiState.detail);
		SendMessage(GetDlgItem(hwnd, IDC_NATURALLOOK_SLIDER), TBM_SETPOS, TRUE, gUiState.naturalLook);
		UpdatePresetButtons(hwnd);
		UpdateCommandLabels(hwnd);
	}

	void SetSliderRanges(HWND hwnd)
	{
		const int sliderIds[] = { IDC_STRENGTH_SLIDER, IDC_DETAIL_SLIDER, IDC_NATURALLOOK_SLIDER };
		for (const int sliderId : sliderIds)
		{
			HWND slider = GetDlgItem(hwnd, sliderId);
			SendMessage(slider, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
			SendMessage(slider, TBM_SETTICFREQ, 10, 0);
			SendMessage(slider, TBM_SETPAGESIZE, 0, 10);
			SendMessage(slider, TBM_SETLINESIZE, 0, 1);
		}
	}

	void LayoutControlsV2(HWND hwnd)
	{
		const RECT previewRect = GetPreviewRect(hwnd);
		RECT clientRect = {};
		GetClientRect(hwnd, &clientRect);

		const int panelLeft = previewRect.right + PANEL_GAP;
		const int panelWidth = clientRect.right - panelLeft - PREVIEW_MARGIN;
		const int labelHeight = 12;
		const int helperHeight = 12;
		const int sliderHeight = 28;
		const int buttonHeight = 20;
		const int rowGap = 12;
		const int top = PREVIEW_MARGIN + 2;
		const int strengthTop = top + 34;
		const int detailTop = top + 112;
		const int naturalTop = top + 204;

		MoveWindow(GetDlgItem(hwnd, IDC_STRENGTH_STATIC), panelLeft, strengthTop, panelWidth, labelHeight, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_STRENGTH_SLIDER), panelLeft, strengthTop + 14, panelWidth, sliderHeight, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_DETAIL_STATIC), panelLeft, detailTop, panelWidth, labelHeight, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_DETAIL_SLIDER), panelLeft, detailTop + 14, panelWidth, sliderHeight, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_DETAIL_HELP_STATIC), panelLeft, detailTop + 46, panelWidth, helperHeight, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_NATURALLOOK_STATIC), panelLeft, naturalTop, panelWidth, labelHeight, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_NATURALLOOK_SLIDER), panelLeft, naturalTop + 14, panelWidth, sliderHeight, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_NATURALLOOK_HELP_STATIC), panelLeft, naturalTop + 46, panelWidth, helperHeight, TRUE);

		const int presetWidth = (panelWidth - (2 * rowGap)) / 3;
		MoveWindow(GetDlgItem(hwnd, IDC_PRESET_NATURAL), panelLeft, top + 304, presetWidth, buttonHeight, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_PRESET_BALANCED), panelLeft + presetWidth + rowGap, top + 304, presetWidth, buttonHeight, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_PRESET_DETAIL), panelLeft + (2 * (presetWidth + rowGap)), top + 304, presetWidth, buttonHeight, TRUE);

		MoveWindow(GetDlgItem(hwnd, IDC_TOGGLEZOOM), panelLeft + presetWidth + rowGap, top + 344, presetWidth, buttonHeight, TRUE);

		const int actionWidth = presetWidth;
		const int actionTop = clientRect.bottom - PREVIEW_MARGIN - buttonHeight;
		MoveWindow(GetDlgItem(hwnd, IDOK), panelLeft + panelWidth - (2 * actionWidth) - rowGap, actionTop, actionWidth, buttonHeight, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDCANCEL), panelLeft + panelWidth - actionWidth, actionTop, actionWidth, buttonHeight, TRUE);

		ClampSplitToPreview(hwnd);
	}

	void RefreshPreview(HWND hwnd)
	{
		DoPreviewProcessingV2();
		UpdatePresetButtons(hwnd);
		UpdateCommandLabels(hwnd);
		InvalidatePreview(hwnd);
		UpdateWindow(hwnd);
	}

	void SchedulePreviewRefresh(HWND hwnd)
	{
		SetTimer(hwnd, PREVIEW_REFRESH_TIMER_ID, PREVIEW_REFRESH_DELAY_MS, nullptr);
	}

	void FillImageArea(HDC hdc, const RECT& rect, BYTE r, BYTE g, BYTE b)
	{
		const HBRUSH brush = CreateSolidBrush(RGB(r, g, b));
		FillRect(hdc, &rect, brush);
		DeleteObject(brush);
	}

	void HandlePaintMessage(HWND hwnd)
	{
		PAINTSTRUCT ps = {};
		HDC hdc = BeginPaint(hwnd, &ps);

		RECT clientRect = {};
		GetClientRect(hwnd, &clientRect);
		const RECT previewRect = GetPreviewRect(hwnd);
		const bool darkMode = gCurrentTheme == ThemeMode::Dark;

		// Render into an off-screen buffer so the user never sees intermediate
		// state (background fill → image blit → split handle). Without this, the
		// split-drag paint flickers badly because each step updates the screen.
		HDC paintDc = nullptr;
		HPAINTBUFFER paintBuffer = BeginBufferedPaint(hdc, &ps.rcPaint, BPBF_COMPATIBLEBITMAP, nullptr, &paintDc);
		if (paintBuffer == nullptr)
		{
			paintDc = hdc;
		}

		FillRect(paintDc, &clientRect, gBackgroundBrush->get());
		FillImageArea(paintDc, previewRect, darkMode ? 18 : 24, darkMode ? 18 : 24, darkMode ? 18 : 28);
		FrameRect(paintDc, &previewRect, static_cast<HBRUSH>(GetStockObject(darkMode ? DKGRAY_BRUSH : GRAY_BRUSH)));

		auto scaledSrc = ScaledSrcImagePtr.lock();
		auto scaledProc = ScaledProcImagePtr.lock();
		if (scaledSrc != nullptr && scaledProc != nullptr)
		{
			DrawMainPreviewComparison(paintDc, &BmHdrCopy, scaledSrc->data(), scaledProc->data(),
			                          ScaledImageWidth, ScaledImageHeight, previewRect, gUiState.splitX,
			                          gUiState.compareHoldOriginal, gUiState.zoomToSelection, darkMode);
		}

		if (paintBuffer != nullptr)
		{
			EndBufferedPaint(paintBuffer, TRUE);
		}

		EndPaint(hwnd, &ps);
	}

	void LoadUiStateFromSettings()
	{
		gUiState.strength = GetPrivateProfileIntA("AltaLux", "Strength", -1, SetupIniFile);
		if (gUiState.strength < 0)
		{
			gUiState.strength = GetPrivateProfileIntA("AltaLux", "Intensity", Constants::DefaultStrength, SetupIniFile);
		}

		gUiState.detail = GetPrivateProfileIntA("AltaLux", "Detail", Constants::DefaultDetail, SetupIniFile);
		gUiState.naturalLook = GetPrivateProfileIntA("AltaLux", "NaturalLook", Constants::DefaultNatural, SetupIniFile);
		gUiState.zoomToSelection = GetPrivateProfileIntA("AltaLux", "Zoom", 0, SetupIniFile) != 0;
		gUiState.compareHoldOriginal = false;
		gUiState.draggingSplit = false;
		gUiState.splitX = 0;

		gUiState.strength = ClampInt(gUiState.strength, 0, 100);
		gUiState.detail = ClampInt(gUiState.detail, 0, 100);
		gUiState.naturalLook = ClampInt(gUiState.naturalLook, 0, 100);
	}

	void SaveUiStateToSettings()
	{
		char valueBuffer[32] = {};
		sprintf_s(valueBuffer, "%d", gUiState.strength);
		WritePrivateProfileStringA("AltaLux", "Strength", valueBuffer, SetupIniFile);
		sprintf_s(valueBuffer, "%d", gUiState.detail);
		WritePrivateProfileStringA("AltaLux", "Detail", valueBuffer, SetupIniFile);
		sprintf_s(valueBuffer, "%d", gUiState.naturalLook);
		WritePrivateProfileStringA("AltaLux", "NaturalLook", valueBuffer, SetupIniFile);
		sprintf_s(valueBuffer, "%d", gUiState.zoomToSelection ? 1 : 0);
		WritePrivateProfileStringA("AltaLux", "Zoom", valueBuffer, SetupIniFile);
	}

	void ScaleDownImage(unsigned char* sourceImage, int sourceWidth, int sourceHeight, unsigned char* targetImage,
	                    int scaleFactor, int bitDepth)
	{
		AltaLuxKernels::ScaleDownBox(sourceImage, sourceWidth, sourceHeight, targetImage,
			scaleFactor, bitDepth, AltaLuxKernels::GetBestSupportedImplementation());
	}

	void ComputeScalingFactor()
	{
		int horizontalScale = ImageWidth / 1000;
		int verticalScale = ImageHeight / 800;
		ScalingFactor = (std::min)(horizontalScale, verticalScale);
		if (ScalingFactor < 1)
		{
			ScalingFactor = 1;
		}

		while (ScalingFactor > 1)
		{
			ScaledImageWidth = ImageWidth / ScalingFactor;
			ScaledImageHeight = ImageHeight / ScalingFactor;
			if ((ScaledImageWidth & 0x07) != 0)
			{
				--ScalingFactor;
			}
			else
			{
				break;
			}
		}

		if (ScalingFactor <= 1)
		{
			ScalingFactor = 1;
			ScaledImageWidth = ImageWidth;
			ScaledImageHeight = ImageHeight;
		}
	}

	void NormalizeClipRect(RECT& clipRect)
	{
		clipRect.bottom += clipRect.top;
		clipRect.right += clipRect.left;
		clipRect.right &= ~7;
		clipRect.left &= ~7;
		clipRect.bottom &= ~7;
		clipRect.top &= ~7;
		ImageWidth = clipRect.right - clipRect.left;
		ImageHeight = clipRect.bottom - clipRect.top;
	}

	bool IsCroppedImage()
	{
		if ((FullImageWidth > ImageWidth) || (FullImageHeight > ImageHeight))
		{
			return true;
		}

		return ((FullImageWidth & 7) != 0) || ((FullImageHeight & 7) != 0);
	}

	void CopyFromSourceImage(unsigned char* targetImage, RECT clipRect, BYTE* imageBits, DWORD imageBitsStride)
	{
		if (targetImage == nullptr || imageBits == nullptr)
		{
			return;
		}

		unsigned char* dst = targetImage;
		unsigned char* src = imageBits;
		if (CroppedImage)
		{
			src += clipRect.left * ImageBitDepth;
			src += imageBitsStride * clipRect.top;
			for (int y = clipRect.top; y < clipRect.bottom; ++y)
			{
				memcpy(dst, src, ImageWidth * ImageBitDepth);
				dst += ImageWidth * ImageBitDepth;
				src += imageBitsStride;
			}
			return;
		}

		for (int y = 0; y < FullImageHeight; ++y)
		{
			memcpy(dst, src, ImageWidth * ImageBitDepth);
			dst += ImageWidth * ImageBitDepth;
			src += imageBitsStride;
		}
	}

	void CopyToSourceImage(BYTE* imageBits, DWORD imageBitsStride, unsigned char* sourceImage, RECT clipRect)
	{
		if (imageBits == nullptr || sourceImage == nullptr)
		{
			return;
		}

		unsigned char* src = sourceImage;
		unsigned char* dst = imageBits;
		if (CroppedImage)
		{
			dst += clipRect.left * ImageBitDepth;
			dst += imageBitsStride * clipRect.top;
			for (int y = clipRect.top; y < clipRect.bottom; ++y)
			{
				memcpy(dst, src, ImageWidth * ImageBitDepth);
				src += ImageWidth * ImageBitDepth;
				dst += imageBitsStride;
			}
			return;
		}

		for (int y = 0; y < FullImageHeight; ++y)
		{
			memcpy(dst, src, ImageWidth * ImageBitDepth);
			src += ImageWidth * ImageBitDepth;
			dst += imageBitsStride;
		}
	}

	bool IsSupportedBitDepth(ScopedBitmapHeader& bitmapHeader)
	{
		switch (bitmapHeader->biBitCount)
		{
		case 24:
			ImageBitDepth = Constants::RGB24PixelSize;
			return true;
		case 32:
			ImageBitDepth = Constants::RGB32PixelSize;
			return true;
		default:
			return false;
		}
	}

	bool IsPointNearSplit(HWND hwnd, int x, int y)
	{
		const RECT imageRect = GetActiveImageRect(hwnd);
		if (!PtInRect(&imageRect, POINT{ x, y }))
		{
			return false;
		}

		return abs(x - gUiState.splitX) <= SPLIT_HIT_RADIUS;
	}

	LRESULT CALLBACK CompareHoldHookProc(int code, WPARAM wparam, LPARAM lparam)
	{
		if (code == HC_ACTION && wparam == PM_REMOVE && gHookDlg != nullptr)
		{
			MSG* msg = reinterpret_cast<MSG*>(lparam);
			if (msg->wParam == VK_SPACE &&
				(msg->message == WM_KEYDOWN || msg->message == WM_KEYUP) &&
				(msg->hwnd == gHookDlg || IsChild(gHookDlg, msg->hwnd)))
			{
				if (msg->message == WM_KEYDOWN && !gUiState.compareHoldOriginal)
				{
					gUiState.compareHoldOriginal = true;
					InvalidatePreview(gHookDlg);
				}
				else if (msg->message == WM_KEYUP && gUiState.compareHoldOriginal)
				{
					gUiState.compareHoldOriginal = false;
					InvalidatePreview(gHookDlg);
				}
				msg->message = WM_NULL;
			}
		}
		return CallNextHookEx(nullptr, code, wparam, lparam);
	}

}

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

BOOL APIENTRY DllMain(HANDLE hModule, DWORD ul_reason_for_call, LPVOID)
{
	if (ul_reason_for_call == DLL_PROCESS_ATTACH)
	{
		hDll = static_cast<HINSTANCE>(hModule);
	}
	return TRUE;
}

INT_PTR CALLBACK DlgProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
	switch (msg)
	{
	case WM_INITDIALOG:
	{
		BufferedPaintInit();
		SetClassLongPtr(hwnd, GCL_STYLE, GetClassLongPtr(hwnd, GCL_STYLE) | CS_DBLCLKS);
		AdjustForDarkMode(hwnd);
		BOOL useDarkMode = IsDarkModeEnabled();
		DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDarkMode, sizeof(useDarkMode));
		SetSliderRanges(hwnd);
		LayoutControlsV2(hwnd);
		CenterSplitInPreview(hwnd);
		UpdateSliders(hwnd);
		DoPreviewProcessingV2();
		gHookDlg = hwnd;
		gKeyboardHook = SetWindowsHookExW(WH_GETMESSAGE, CompareHoldHookProc, nullptr, GetCurrentThreadId());
		return TRUE;
	}

	case WM_DESTROY:
		KillTimer(hwnd, PREVIEW_REFRESH_TIMER_ID);
		if (gKeyboardHook != nullptr)
		{
			UnhookWindowsHookEx(gKeyboardHook);
			gKeyboardHook = nullptr;
		}
		gHookDlg = nullptr;
		BufferedPaintUnInit();
		return TRUE;

	case WM_COMMAND:
		switch (LOWORD(wparam))
		{
		case IDOK:
			SkipProcessing = false;
			SaveUiStateToSettings();
			EndDialog(hwnd, IDOK);
			return TRUE;

		case IDCANCEL:
			SkipProcessing = true;
			EndDialog(hwnd, IDCANCEL);
			return TRUE;

		case IDC_PRESET_NATURAL:
			ApplyPreset(Presets[0]);
			UpdateSliders(hwnd);
			RefreshPreview(hwnd);
			return TRUE;

		case IDC_PRESET_BALANCED:
			ApplyPreset(Presets[1]);
			UpdateSliders(hwnd);
			RefreshPreview(hwnd);
			return TRUE;

		case IDC_PRESET_DETAIL:
			ApplyPreset(Presets[2]);
			UpdateSliders(hwnd);
			RefreshPreview(hwnd);
			return TRUE;

		case IDC_TOGGLEZOOM:
			gUiState.zoomToSelection = !gUiState.zoomToSelection;
			UpdateCommandLabels(hwnd);
			InvalidatePreview(hwnd);
			return TRUE;
		}
		break;

	case WM_HSCROLL:
	{
		HWND slider = reinterpret_cast<HWND>(lparam);
		if (slider == nullptr)
		{
			return FALSE;
		}

		const int value = static_cast<int>(SendMessage(slider, TBM_GETPOS, 0, 0));
		if (slider == GetDlgItem(hwnd, IDC_STRENGTH_SLIDER))
		{
			gUiState.strength = value;
		}
		else if (slider == GetDlgItem(hwnd, IDC_DETAIL_SLIDER))
		{
			gUiState.detail = value;
		}
		else if (slider == GetDlgItem(hwnd, IDC_NATURALLOOK_SLIDER))
		{
			gUiState.naturalLook = value;
		}

		switch (LOWORD(wparam))
		{
		case TB_ENDTRACK:
		case TB_THUMBPOSITION:
			KillTimer(hwnd, PREVIEW_REFRESH_TIMER_ID);
			RefreshPreview(hwnd);
			break;
		default:
			UpdatePresetButtons(hwnd);
			SchedulePreviewRefresh(hwnd);
			break;
		}
		return TRUE;
	}

	case WM_TIMER:
		if (wparam == PREVIEW_REFRESH_TIMER_ID)
		{
			KillTimer(hwnd, PREVIEW_REFRESH_TIMER_ID);
			RefreshPreview(hwnd);
			return TRUE;
		}
		break;

	case WM_LBUTTONDOWN:
	{
		const int x = GET_X_LPARAM(lparam);
		const int y = GET_Y_LPARAM(lparam);
		SetFocus(hwnd);

		if (IsPointNearSplit(hwnd, x, y))
		{
			gUiState.draggingSplit = true;
			SetCapture(hwnd);
			gUiState.splitX = x;
			InvalidatePreview(hwnd);
			return TRUE;
		}
		break;
	}

	case WM_MOUSEMOVE:
		if (gUiState.draggingSplit)
		{
			gUiState.splitX = GET_X_LPARAM(lparam);
			ClampSplitToPreview(hwnd);
			InvalidatePreview(hwnd);
			return TRUE;
		}
		break;

	case WM_LBUTTONUP:
		if (gUiState.draggingSplit)
		{
			gUiState.draggingSplit = false;
			ReleaseCapture();
			InvalidatePreview(hwnd);
			return TRUE;
		}
		break;

	case WM_LBUTTONDBLCLK:
		if (IsPointNearSplit(hwnd, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)))
		{
			CenterSplitInPreview(hwnd);
			InvalidatePreview(hwnd);
			return TRUE;
		}
		break;

	case WM_CAPTURECHANGED:
		gUiState.draggingSplit = false;
		break;

	case WM_SETCURSOR:
	{
		POINT cursor = {};
		GetCursorPos(&cursor);
		ScreenToClient(hwnd, &cursor);
		if (IsPointNearSplit(hwnd, cursor.x, cursor.y))
		{
			SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
			return TRUE;
		}
		break;
	}

	case WM_PAINT:
		HandlePaintMessage(hwnd);
		return TRUE;

	case WM_CTLCOLORDLG:
	case WM_CTLCOLORSTATIC:
	case WM_CTLCOLORBTN:
	{
		HDC hdc = reinterpret_cast<HDC>(wparam);
		SetBkMode(hdc, TRANSPARENT);
		SetTextColor(hdc, gCurrentTheme == ThemeMode::Dark ? RGB(245, 245, 245) : RGB(24, 24, 24));
		return reinterpret_cast<INT_PTR>(gBackgroundBrush->get());
	}

	case WM_THEMECHANGED:
	{
		AdjustForDarkMode(hwnd);
		BOOL useDarkMode = IsDarkModeEnabled();
		DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDarkMode, sizeof(useDarkMode));
		InvalidateRect(hwnd, nullptr, FALSE);
		return TRUE;
	}

	case WM_SETTINGCHANGE:
	{
		if (lparam == 0)
			break;
		const bool isImmersive = IsWindowUnicode(hwnd)
			? (wcscmp(reinterpret_cast<const wchar_t*>(lparam), L"ImmersiveColorSet") == 0)
			: (strcmp(reinterpret_cast<const char*>(lparam), "ImmersiveColorSet") == 0);
		if (!isImmersive)
			break;
		AdjustForDarkMode(hwnd);
		BOOL useDarkMode = IsDarkModeEnabled();
		DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDarkMode, sizeof(useDarkMode));
		InvalidateRect(hwnd, nullptr, FALSE);
		return TRUE;
	}

	case WM_SIZE:
		if (wparam == SIZE_MINIMIZED)
			return TRUE;
		LayoutControlsV2(hwnd);
		InvalidateRect(hwnd, nullptr, FALSE);
		return TRUE;

	case WM_GETMINMAXINFO:
	{
		MINMAXINFO* minMaxInfo = reinterpret_cast<MINMAXINFO*>(lparam);
		minMaxInfo->ptMinTrackSize.x = 960;
		minMaxInfo->ptMinTrackSize.y = 680;
		return TRUE;
	}
	}

	return FALSE;
}

bool __cdecl StartEffects2(HANDLE hDib, HWND hwnd, int, RECT rect, int param1, int param2, char* iniFile, char*, int)
{
#define WIDTHBYTES(bits) (((bits) + 31) / 32 * 4)

	SharedImagePtr srcImage;
	RECT clipRect = rect;

	{
		ScopedBitmapHeader bitmapHeader(hDib);
		if (!bitmapHeader.IsValid())
		{
			return false;
		}
		memcpy(&BmHdrCopy, &(*bitmapHeader), sizeof(BITMAPINFOHEADER));

		if (bitmapHeader->biPlanes != 1 || !IsSupportedBitDepth(bitmapHeader))
		{
			return false;
		}

		FullImageWidth = abs(bitmapHeader->biWidth);
		FullImageHeight = abs(bitmapHeader->biHeight);
		ImageWidth = clipRect.right;
		ImageHeight = clipRect.bottom;

		CroppedImage = IsCroppedImage();
		if (CroppedImage)
		{
			NormalizeClipRect(clipRect);
			BmHdrCopy.biWidth = ImageWidth;
			BmHdrCopy.biHeight = ImageHeight;
		}

		BYTE* imageBits = bitmapHeader.GetImageBits();
		DWORD imageBitsStride = WIDTHBYTES(static_cast<DWORD>(FullImageWidth * bitmapHeader->biBitCount));
		srcImage = std::make_shared<std::vector<unsigned char>>(GetRGBImageSize(ImageWidth, ImageHeight));
		CopyFromSourceImage(srcImage->data(), clipRect, imageBits, imageBitsStride);
	}

	if ((param1 == -1) || (param2 == -1))
	{
		strcpy_s(SetupIniFile, sizeof(SetupIniFile), iniFile);
		LoadUiStateFromSettings();

		ComputeScalingFactor();
		auto scaledSrcImage = std::make_shared<std::vector<unsigned char>>(GetRGBImageSize(ScaledImageWidth, ScaledImageHeight));
		auto scaledProcImage = std::make_shared<std::vector<unsigned char>>(GetRGBImageSize(ScaledImageWidth, ScaledImageHeight));
		ScaledSrcImagePtr = scaledSrcImage;
		ScaledProcImagePtr = scaledProcImage;

		ScaleDownImage(srcImage->data(), ImageWidth, ImageHeight, scaledSrcImage->data(), ScalingFactor, ImageBitDepth);
		CopyScaledSrcImage(scaledProcImage->data());

		const INT_PTR dialogResult = DialogBox(hDll, MAKEINTRESOURCE(IDD_ALTALUX_DIALOG), hwnd, DlgProc);
		if (dialogResult == -1)
		{
			return false;
		}

		if (SkipProcessing)
		{
			return true;
		}
	}
	else
	{
		gUiState.strength = ClampInt(param1, 0, 100);
		gUiState.detail = Constants::DefaultDetail;
		gUiState.naturalLook = Constants::DefaultNatural;
		gUiState.zoomToSelection = false;
		gUiState.compareHoldOriginal = false;
		gUiState.draggingSplit = false;
		gUiState.splitX = 0;
	}

	if (!ProcessMultiscaleImage(srcImage->data(), srcImage->data(), ImageWidth, ImageHeight, ImageBitDepth, gUiState))
	{
		return false;
	}

	{
		ScopedBitmapHeader bitmapHeader(hDib);
		if (!bitmapHeader.IsValid())
		{
			return false;
		}
		BYTE* imageBits = bitmapHeader.GetImageBits();
		DWORD imageBitsStride = WIDTHBYTES(static_cast<DWORD>(FullImageWidth * bitmapHeader->biBitCount));
		CopyToSourceImage(imageBits, imageBitsStride, srcImage->data(), clipRect);
	}

	return true;
}

int __cdecl GetPlugInInfo(char* versionString, char* fileFormats)
{
	sprintf_s(versionString, 64, "2.00");
	sprintf_s(fileFormats, 256, "AltaLux image enhancement filter");
	return 0;
}

bool __cdecl AltaLux_Effects(HANDLE hDib, HWND hwnd, int filter, RECT rect, int param1, int param2, char* iniFile,
                             char* szAppName, int regID)
{
	return StartEffects2(hDib, hwnd, filter, rect, param1, param2, iniFile, szAppName, regID);
}
