/*
Project: AltaLux plugin for IrfanView
Author: Stefano Tommesani
Website: http://www.tommesani.com

Microsoft Public License (MS-PL) [OSI Approved License]
The full license text is in the LICENSE file at the root of the repository.
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
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Kernels/AltaLuxKernels.h"
#include "ScopedBitmapHeader.h"
#include "AltaLuxCore.h"
#include "AltaLuxVersion.h"
#include "UIDraw/UIDraw.h"
#include "Segmentation/SegmentationModule.h"
#include "Segmentation/SelectionCore.h"

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
	const UINT WM_ALTALUX_SEGMENTATION_PREPARED = WM_APP + 42;
	const UINT WM_ALTALUX_SEGMENTATION_COMPLETED = WM_APP + 43;

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
	bool SourceTopDown = false;

	WeakImagePtr ScaledSrcImagePtr;
	WeakImagePtr ScaledProcImagePtr;
	WeakImagePtr FullSrcImagePtr;

	struct SelectionPromptMarker
	{
		float x;
		float y;
		SelectionCombineMode mode;
	};

	struct SegmentationWorkerResult
	{
		std::uint32_t generation = 0;
		SegmentationStatus status = SegmentationStatus::RuntimeError;
		SelectionCombineMode mode = SelectionCombineMode::Add;
		SegmentationPoint point = {};
		std::vector<unsigned char> operationMask;
		std::wstring error;
	};

	struct SelectionDialogSession
	{
		bool selectiveMode = false;
		bool ready = false;
		bool busy = false;
		bool showMask = true;
		int softness = 3;
		SelectionCombineMode combineMode = SelectionCombineMode::Add;
		std::vector<unsigned char> selectionMask;
		std::vector<unsigned char> previewMask;
		std::vector<unsigned char> previewOriginalDisplay;
		std::vector<unsigned char> previewProcessedDisplay;
		std::vector<SelectionPromptMarker> markers;
		SelectionHistory history;
		SegmentationModule module;
		std::thread worker;
		std::atomic<std::uint32_t> generation{ 0 };
		std::mutex moduleMutex;
	};

	std::unique_ptr<SelectionDialogSession> gSelectionSession;

	UiState gUiState = {
		Constants::DefaultStrength,
		Constants::DefaultDetail,
		Constants::DefaultNatural,
		Constants::DefaultChromaProtection,
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
	HFONT gSectionFont = nullptr;

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

		if (gSelectionSession == nullptr || !gSelectionSession->selectiveMode ||
			gSelectionSession->selectionMask.empty())
		{
			if (gSelectionSession != nullptr)
			{
				gSelectionSession->previewOriginalDisplay.clear();
				gSelectionSession->previewProcessedDisplay.clear();
			}
			return;
		}

		const std::size_t previewPixelCount = static_cast<std::size_t>(ScaledImageWidth) * ScaledImageHeight;
		gSelectionSession->previewMask.resize(previewPixelCount);
		ResizeMaskNearest(gSelectionSession->selectionMask.data(), ImageWidth, ImageHeight,
			gSelectionSession->previewMask.data(), ScaledImageWidth, ScaledImageHeight);

		std::vector<unsigned char> previewAlpha;
		const int previewSoftness = gSelectionSession->softness == 0 ? 0 :
			(std::max)(1, gSelectionSession->softness / ScalingFactor);
		if (!BuildFeatheredMask(gSelectionSession->previewMask.data(), ScaledImageWidth, ScaledImageHeight,
			previewSoftness, previewAlpha))
		{
			return;
		}
		CompositeWithMask(scaledSrc->data(), scaledProc->data(), scaledProc->data(), previewAlpha.data(),
			ScaledImageWidth, ScaledImageHeight, ImageBitDepth);

		if (!gSelectionSession->showMask)
		{
			gSelectionSession->previewOriginalDisplay.clear();
			gSelectionSession->previewProcessedDisplay.clear();
			return;
		}

		const std::size_t byteCount = static_cast<std::size_t>(GetImageByteCount(ScaledImageWidth, ScaledImageHeight));
		gSelectionSession->previewOriginalDisplay.assign(scaledSrc->begin(), scaledSrc->begin() + byteCount);
		gSelectionSession->previewProcessedDisplay.assign(scaledProc->begin(), scaledProc->begin() + byteCount);
		auto applyOverlay = [&](std::vector<unsigned char>& image)
		{
			for (std::size_t pixel = 0; pixel < previewPixelCount; ++pixel)
			{
				const unsigned int overlayAlpha = static_cast<unsigned int>(previewAlpha[pixel]) * 89U / 255U;
				if (overlayAlpha == 0) continue;
				const unsigned int inverse = 255U - overlayAlpha;
				const std::size_t base = pixel * ImageBitDepth;
				image[base] = static_cast<unsigned char>((image[base] * inverse + 255U * overlayAlpha + 127U) / 255U);
				image[base + 1] = static_cast<unsigned char>((image[base + 1] * inverse + 220U * overlayAlpha + 127U) / 255U);
				image[base + 2] = static_cast<unsigned char>((image[base + 2] * inverse + 127U) / 255U);
			}
		};
		applyOverlay(gSelectionSession->previewOriginalDisplay);
		applyOverlay(gSelectionSession->previewProcessedDisplay);
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
			SetButtonText(hwnd, preset.buttonId, preset.name);
			SendMessage(GetDlgItem(hwnd, preset.buttonId), BM_SETCHECK,
				IsPresetActive(preset) ? BST_CHECKED : BST_UNCHECKED, 0);
		}
	}

	void UpdateCommandLabels(HWND hwnd)
	{
		SendMessage(GetDlgItem(hwnd, IDC_VIEW_FIT), BM_SETCHECK,
			gUiState.zoomToSelection ? BST_UNCHECKED : BST_CHECKED, 0);
		SendMessage(GetDlgItem(hwnd, IDC_VIEW_ACTUAL), BM_SETCHECK,
			gUiState.zoomToSelection ? BST_CHECKED : BST_UNCHECKED, 0);
		const bool selective = gSelectionSession != nullptr && gSelectionSession->selectiveMode;
		SetButtonText(hwnd, IDC_MODE_ENTIRE_IMAGE, L"Entire image");
		SetButtonText(hwnd, IDC_MODE_SELECT_OBJECTS, L"Select objects");
		SendMessage(GetDlgItem(hwnd, IDC_MODE_ENTIRE_IMAGE), BM_SETCHECK,
			selective ? BST_UNCHECKED : BST_CHECKED, 0);
		SendMessage(GetDlgItem(hwnd, IDC_MODE_SELECT_OBJECTS), BM_SETCHECK,
			selective ? BST_CHECKED : BST_UNCHECKED, 0);
	}

	void ApplyPanelTypography(HWND hwnd)
	{
		if (gSectionFont != nullptr)
		{
			DeleteObject(gSectionFont);
			gSectionFont = nullptr;
		}
		HFONT dialogFont = reinterpret_cast<HFONT>(SendMessage(hwnd, WM_GETFONT, 0, 0));
		LOGFONTW sectionLogFont = {};
		if (dialogFont != nullptr && GetObjectW(dialogFont, sizeof(sectionLogFont), &sectionLogFont) != 0)
		{
			sectionLogFont.lfWeight = FW_SEMIBOLD;
			gSectionFont = CreateFontIndirectW(&sectionLogFont);
		}
		const int sectionIds[] = {
			IDC_ENHANCEMENT_SECTION, IDC_VIEW_SECTION, IDC_APPLY_TO_SECTION, IDC_SELECTION_SECTION
		};
		for (const int controlId : sectionIds)
		{
			SendMessage(GetDlgItem(hwnd, controlId), WM_SETFONT,
				reinterpret_cast<WPARAM>(gSectionFont != nullptr ? gSectionFont : dialogFont), TRUE);
		}
	}

	void SetSelectionStatus(HWND hwnd, const wchar_t* text)
	{
		SetWindowTextW(GetDlgItem(hwnd, IDC_SELECTION_STATUS), text != nullptr ? text : L"");
	}

	void UpdateSelectionControls(HWND hwnd)
	{
		const int selectiveControls[] = {
			IDC_SELECTION_ADD, IDC_SELECTION_REMOVE, IDC_SELECTION_UNDO, IDC_SELECTION_CLEAR,
			IDC_SELECTION_SELECT_ALL, IDC_SELECTION_SHOW_MASK, IDC_EDGE_SOFTNESS_STATIC,
			IDC_EDGE_SOFTNESS_SLIDER, IDC_SELECTION_STATUS, IDC_SELECTION_SECTION
		};
#if !defined(_WIN64)
		ShowWindow(GetDlgItem(hwnd, IDC_APPLY_TO_SECTION), SW_HIDE);
		ShowWindow(GetDlgItem(hwnd, IDC_MODE_ENTIRE_IMAGE), SW_HIDE);
		ShowWindow(GetDlgItem(hwnd, IDC_MODE_SELECT_OBJECTS), SW_HIDE);
		for (const int controlId : selectiveControls) ShowWindow(GetDlgItem(hwnd, controlId), SW_HIDE);
		EnableWindow(GetDlgItem(hwnd, IDOK), TRUE);
		UpdateCommandLabels(hwnd);
		return;
#else
		ShowWindow(GetDlgItem(hwnd, IDC_APPLY_TO_SECTION), SW_SHOW);
		ShowWindow(GetDlgItem(hwnd, IDC_MODE_ENTIRE_IMAGE), SW_SHOW);
		ShowWindow(GetDlgItem(hwnd, IDC_MODE_SELECT_OBJECTS), SW_SHOW);
		const bool selective = gSelectionSession != nullptr && gSelectionSession->selectiveMode;
		const bool ready = selective && gSelectionSession->ready && !gSelectionSession->busy;
		for (const int controlId : selectiveControls)
		{
			ShowWindow(GetDlgItem(hwnd, controlId), selective ? SW_SHOW : SW_HIDE);
		}
		EnableWindow(GetDlgItem(hwnd, IDC_SELECTION_ADD), ready);
		EnableWindow(GetDlgItem(hwnd, IDC_SELECTION_REMOVE), ready);
		EnableWindow(GetDlgItem(hwnd, IDC_SELECTION_UNDO), ready && gSelectionSession->history.CanUndo());
		EnableWindow(GetDlgItem(hwnd, IDC_SELECTION_CLEAR), ready && HasSelectedPixels(gSelectionSession->selectionMask));
		EnableWindow(GetDlgItem(hwnd, IDC_SELECTION_SELECT_ALL), ready);
		EnableWindow(GetDlgItem(hwnd, IDC_SELECTION_SHOW_MASK), ready);
		EnableWindow(GetDlgItem(hwnd, IDC_EDGE_SOFTNESS_SLIDER), ready);
		EnableWindow(GetDlgItem(hwnd, IDOK), !selective || (ready && HasSelectedPixels(gSelectionSession->selectionMask)));
		if (selective)
		{
			SendMessage(GetDlgItem(hwnd, IDC_SELECTION_ADD), BM_SETCHECK,
				gSelectionSession->combineMode == SelectionCombineMode::Add ? BST_CHECKED : BST_UNCHECKED, 0);
			SendMessage(GetDlgItem(hwnd, IDC_SELECTION_REMOVE), BM_SETCHECK,
				gSelectionSession->combineMode == SelectionCombineMode::Remove ? BST_CHECKED : BST_UNCHECKED, 0);
			SendMessage(GetDlgItem(hwnd, IDC_SELECTION_SHOW_MASK), BM_SETCHECK,
				gSelectionSession->showMask ? BST_CHECKED : BST_UNCHECKED, 0);
			SendMessage(GetDlgItem(hwnd, IDC_EDGE_SOFTNESS_SLIDER), TBM_SETPOS, TRUE,
				gSelectionSession->softness);
		}
		UpdateCommandLabels(hwnd);
#endif
	}

	void UpdateSliders(HWND hwnd)
	{
		SendMessage(GetDlgItem(hwnd, IDC_STRENGTH_SLIDER), TBM_SETPOS, TRUE, gUiState.strength);
		SendMessage(GetDlgItem(hwnd, IDC_DETAIL_SLIDER), TBM_SETPOS, TRUE, gUiState.detail);
		SendMessage(GetDlgItem(hwnd, IDC_NATURALLOOK_SLIDER), TBM_SETPOS, TRUE, gUiState.naturalLook);
		SendMessage(GetDlgItem(hwnd, IDC_CHROMA_SLIDER), TBM_SETPOS, TRUE, gUiState.chromaProtection);
		UpdatePresetButtons(hwnd);
		UpdateCommandLabels(hwnd);
	}

	void SetSliderRanges(HWND hwnd)
	{
		const int sliderIds[] = { IDC_STRENGTH_SLIDER, IDC_DETAIL_SLIDER, IDC_NATURALLOOK_SLIDER,
			IDC_CHROMA_SLIDER };
		for (const int sliderId : sliderIds)
		{
			HWND slider = GetDlgItem(hwnd, sliderId);
			SendMessage(slider, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
			SendMessage(slider, TBM_SETTICFREQ, 10, 0);
			SendMessage(slider, TBM_SETPAGESIZE, 0, 10);
			SendMessage(slider, TBM_SETLINESIZE, 0, 1);
		}
		HWND softness = GetDlgItem(hwnd, IDC_EDGE_SOFTNESS_SLIDER);
		SendMessage(softness, TBM_SETRANGE, TRUE, MAKELONG(0, 20));
		SendMessage(softness, TBM_SETTICFREQ, 2, 0);
		SendMessage(softness, TBM_SETPAGESIZE, 0, 5);
		SendMessage(softness, TBM_SETLINESIZE, 0, 1);
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
		const int rowGap = 10;
		const int top = PREVIEW_MARGIN + 2;
		const int sectionHeight = 14;
		const int strengthTop = top + 24;
		const int detailTop = top + 78;
		const int naturalTop = top + 148;
		const int chromaTop = top + 218;

		MoveWindow(GetDlgItem(hwnd, IDC_ENHANCEMENT_SECTION), panelLeft, top, panelWidth, sectionHeight, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_STRENGTH_STATIC), panelLeft, strengthTop, panelWidth, labelHeight, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_STRENGTH_SLIDER), panelLeft, strengthTop + 14, panelWidth, sliderHeight, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_DETAIL_STATIC), panelLeft, detailTop, panelWidth, labelHeight, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_DETAIL_SLIDER), panelLeft, detailTop + 14, panelWidth, sliderHeight, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_DETAIL_HELP_STATIC), panelLeft, detailTop + 46, panelWidth, helperHeight, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_NATURALLOOK_STATIC), panelLeft, naturalTop, panelWidth, labelHeight, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_NATURALLOOK_SLIDER), panelLeft, naturalTop + 14, panelWidth, sliderHeight, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_NATURALLOOK_HELP_STATIC), panelLeft, naturalTop + 46, panelWidth, helperHeight, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_CHROMA_STATIC), panelLeft, chromaTop, panelWidth, labelHeight, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_CHROMA_SLIDER), panelLeft, chromaTop + 14, panelWidth, sliderHeight, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_CHROMA_HELP_STATIC), panelLeft, chromaTop + 46, panelWidth, helperHeight, TRUE);

		const int presetWidth = (panelWidth - (2 * rowGap)) / 3;
		MoveWindow(GetDlgItem(hwnd, IDC_PRESET_NATURAL), panelLeft, top + 286, presetWidth, buttonHeight, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_PRESET_BALANCED), panelLeft + presetWidth + rowGap, top + 286, presetWidth, buttonHeight, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_PRESET_DETAIL), panelLeft + (2 * (presetWidth + rowGap)), top + 286, presetWidth, buttonHeight, TRUE);

		MoveWindow(GetDlgItem(hwnd, IDC_VIEW_SECTION), panelLeft, top + 320, panelWidth, sectionHeight, TRUE);
		const int viewWidth = (panelWidth - rowGap) / 2;
		MoveWindow(GetDlgItem(hwnd, IDC_VIEW_FIT), panelLeft, top + 340, viewWidth, buttonHeight + 2, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_VIEW_ACTUAL), panelLeft + viewWidth + rowGap, top + 340,
			viewWidth, buttonHeight + 2, TRUE);

		MoveWindow(GetDlgItem(hwnd, IDC_APPLY_TO_SECTION), panelLeft, top + 372, panelWidth, sectionHeight, TRUE);
		const int modeWidth = (panelWidth - rowGap) / 2;
		MoveWindow(GetDlgItem(hwnd, IDC_MODE_ENTIRE_IMAGE), panelLeft, top + 392, modeWidth, buttonHeight + 2, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_MODE_SELECT_OBJECTS), panelLeft + modeWidth + rowGap, top + 392, modeWidth, buttonHeight + 2, TRUE);

		MoveWindow(GetDlgItem(hwnd, IDC_SELECTION_SECTION), panelLeft, top + 430, panelWidth, sectionHeight, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_SELECTION_ADD), panelLeft, top + 450, modeWidth, buttonHeight + 2, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_SELECTION_REMOVE), panelLeft + modeWidth + rowGap, top + 450, modeWidth, buttonHeight + 2, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_SELECTION_UNDO), panelLeft, top + 482, presetWidth, buttonHeight, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_SELECTION_CLEAR), panelLeft + presetWidth + rowGap, top + 482, presetWidth, buttonHeight, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_SELECTION_SELECT_ALL), panelLeft + (2 * (presetWidth + rowGap)), top + 482, presetWidth, buttonHeight, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_SELECTION_SHOW_MASK), panelLeft, top + 512, panelWidth, buttonHeight + 2, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_EDGE_SOFTNESS_STATIC), panelLeft, top + 546, panelWidth, labelHeight, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_EDGE_SOFTNESS_SLIDER), panelLeft, top + 560, panelWidth, sliderHeight, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_SELECTION_STATUS), panelLeft, top + 594, panelWidth, 28, TRUE);

		const int actionWidth = modeWidth;
		const int actionTop = clientRect.bottom - PREVIEW_MARGIN - buttonHeight;
		MoveWindow(GetDlgItem(hwnd, IDCANCEL), panelLeft + panelWidth - (2 * actionWidth) - rowGap, actionTop, actionWidth, buttonHeight, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDOK), panelLeft + panelWidth - actionWidth, actionTop, actionWidth, buttonHeight, TRUE);

		ClampSplitToPreview(hwnd);
	}

	void StopSelectionWorker()
	{
		if (gSelectionSession == nullptr) return;
		++gSelectionSession->generation;
		{
			std::lock_guard<std::mutex> lock(gSelectionSession->moduleMutex);
			if (gSelectionSession->module.GetEngine() != nullptr)
			{
				gSelectionSession->module.GetEngine()->Cancel();
			}
		}
		if (gSelectionSession->worker.joinable())
		{
			gSelectionSession->worker.join();
		}
		gSelectionSession->busy = false;
	}

	void FinishSelectionWorker()
	{
		if (gSelectionSession != nullptr && gSelectionSession->worker.joinable())
		{
			gSelectionSession->worker.join();
		}
	}

	void DiscardPendingSegmentationResults(HWND hwnd)
	{
		MSG message = {};
		while (PeekMessage(&message, hwnd, WM_ALTALUX_SEGMENTATION_PREPARED,
			WM_ALTALUX_SEGMENTATION_COMPLETED, PM_REMOVE))
		{
			delete reinterpret_cast<SegmentationWorkerResult*>(message.lParam);
		}
	}

	bool PostSegmentationResult(HWND hwnd, UINT message, SegmentationWorkerResult* result)
	{
		if (PostMessage(hwnd, message, 0, reinterpret_cast<LPARAM>(result)) == FALSE)
		{
			delete result;
			return false;
		}
		return true;
	}

	void StartSegmentationPreparation(HWND hwnd)
	{
		if (gSelectionSession == nullptr) return;
		StopSelectionWorker();
		auto source = FullSrcImagePtr.lock();
		if (source == nullptr)
		{
			SetSelectionStatus(hwnd, L"Source image is unavailable.");
			return;
		}

		gSelectionSession->selectiveMode = true;
		gSelectionSession->ready = false;
		gSelectionSession->busy = true;
		gSelectionSession->selectionMask.assign(static_cast<std::size_t>(ImageWidth) * ImageHeight, 0);
		gSelectionSession->history.Reset();
		gSelectionSession->markers.clear();
		const std::uint32_t generation = ++gSelectionSession->generation;
		SetSelectionStatus(hwnd, L"Analyzing image...");
		UpdateSelectionControls(hwnd);
		InvalidatePreview(hwnd);

		gSelectionSession->worker = std::thread([hwnd, generation, source]()
		{
			auto* result = new SegmentationWorkerResult();
			result->generation = generation;
			ISegmentationEngine* engine = nullptr;
			{
				std::lock_guard<std::mutex> lock(gSelectionSession->moduleMutex);
				if (!gSelectionSession->module.Load(hDll))
				{
					result->status = SegmentationStatus::RuntimeError;
					result->error = gSelectionSession->module.GetLastError();
					PostSegmentationResult(hwnd, WM_ALTALUX_SEGMENTATION_PREPARED, result);
					return;
				}
				engine = gSelectionSession->module.GetEngine();
			}

			const int rowStride = ImageWidth * ImageBitDepth;
			SegmentationImageView image = {};
			image.pixels = source->data();
			image.width = static_cast<std::uint32_t>(ImageWidth);
			image.height = static_cast<std::uint32_t>(ImageHeight);
			image.stride = rowStride;
			image.format = ImageBitDepth == Constants::RGB32PixelSize ?
				SegmentationPixelFormat::Bgra32 : SegmentationPixelFormat::Bgr24;
			if (!SourceTopDown)
			{
				image.pixels += static_cast<std::size_t>(ImageHeight - 1) * rowStride;
				image.stride = -rowStride;
			}
			result->status = engine->PrepareImage(image);
			if (result->status != SegmentationStatus::Ok && engine->GetLastError() != nullptr)
			{
				result->error = engine->GetLastError();
			}
			PostSegmentationResult(hwnd, WM_ALTALUX_SEGMENTATION_PREPARED, result);
		});
	}

	bool MapClientPointToSource(HWND hwnd, int x, int y, float& sourceX, float& sourceY)
	{
		const RECT imageRect = GetActiveImageRect(hwnd);
		float scaledX = 0.0f;
		float scaledY = 0.0f;
		if (!MapPreviewPointToImage(imageRect, x, y, ScaledImageWidth, ScaledImageHeight, scaledX, scaledY))
		{
			return false;
		}
		if (gUiState.zoomToSelection && ScaledImageWidth > RectWidth(imageRect) && ScaledImageHeight > RectHeight(imageRect))
		{
			scaledX = static_cast<float>((ScaledImageWidth - RectWidth(imageRect)) / 2 + (x - imageRect.left));
			scaledY = static_cast<float>((ScaledImageHeight - RectHeight(imageRect)) / 2 + (y - imageRect.top));
		}
		sourceX = (scaledX + 0.5f) * ImageWidth / ScaledImageWidth - 0.5f;
		sourceY = (scaledY + 0.5f) * ImageHeight / ScaledImageHeight - 0.5f;
		sourceX = (std::max)(0.0f, (std::min)(sourceX, static_cast<float>(ImageWidth - 1)));
		sourceY = (std::max)(0.0f, (std::min)(sourceY, static_cast<float>(ImageHeight - 1)));
		return true;
	}

	void StartPointSegmentation(HWND hwnd, float sourceX, float sourceY)
	{
		if (gSelectionSession == nullptr || !gSelectionSession->ready || gSelectionSession->busy) return;
		FinishSelectionWorker();
		gSelectionSession->busy = true;
		const std::uint32_t generation = ++gSelectionSession->generation;
		const SelectionCombineMode mode = gSelectionSession->combineMode;
		SetSelectionStatus(hwnd, L"Selecting...");
		UpdateSelectionControls(hwnd);

		gSelectionSession->worker = std::thread([hwnd, generation, sourceX, sourceY, mode]()
		{
			auto* result = new SegmentationWorkerResult();
			result->generation = generation;
			result->mode = mode;
			result->point = { sourceX, sourceY };
			result->operationMask.assign(static_cast<std::size_t>(ImageWidth) * ImageHeight, 0);
			ISegmentationEngine* engine = nullptr;
			{
				std::lock_guard<std::mutex> lock(gSelectionSession->moduleMutex);
				engine = gSelectionSession->module.GetEngine();
			}
			if (engine == nullptr)
			{
				result->status = SegmentationStatus::NotReady;
				result->error = L"The segmentation engine is not ready.";
				PostSegmentationResult(hwnd, WM_ALTALUX_SEGMENTATION_COMPLETED, result);
				return;
			}

			const int maskStride = ImageWidth;
			SegmentationMaskView mask = {};
			mask.pixels = result->operationMask.data();
			mask.width = static_cast<std::uint32_t>(ImageWidth);
			mask.height = static_cast<std::uint32_t>(ImageHeight);
			mask.stride = maskStride;
			if (!SourceTopDown)
			{
				mask.pixels += static_cast<std::size_t>(ImageHeight - 1) * maskStride;
				mask.stride = -maskStride;
			}
			result->status = engine->SegmentPoint(result->point, mask);
			if (result->status != SegmentationStatus::Ok && engine->GetLastError() != nullptr)
			{
				result->error = engine->GetLastError();
			}
			PostSegmentationResult(hwnd, WM_ALTALUX_SEGMENTATION_COMPLETED, result);
		});
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

	void DrawSelectionMarkers(HDC hdc, HWND hwnd)
	{
		if (gSelectionSession == nullptr || !gSelectionSession->selectiveMode) return;
		const RECT imageRect = GetActiveImageRect(hwnd);
		for (const SelectionPromptMarker& marker : gSelectionSession->markers)
		{
			const float scaledX = (marker.x + 0.5f) * ScaledImageWidth / ImageWidth - 0.5f;
			const float scaledY = (marker.y + 0.5f) * ScaledImageHeight / ImageHeight - 0.5f;
			int clientX = 0;
			int clientY = 0;
			if (gUiState.zoomToSelection && ScaledImageWidth > RectWidth(imageRect) && ScaledImageHeight > RectHeight(imageRect))
			{
				clientX = imageRect.left + static_cast<int>(scaledX) - (ScaledImageWidth - RectWidth(imageRect)) / 2;
				clientY = imageRect.top + static_cast<int>(scaledY) - (ScaledImageHeight - RectHeight(imageRect)) / 2;
			}
			else
			{
				clientX = imageRect.left + static_cast<int>((scaledX + 0.5f) * RectWidth(imageRect) / ScaledImageWidth);
				clientY = imageRect.top + static_cast<int>((scaledY + 0.5f) * RectHeight(imageRect) / ScaledImageHeight);
			}
			if (clientX < imageRect.left || clientX >= imageRect.right || clientY < imageRect.top || clientY >= imageRect.bottom)
			{
				continue;
			}
			const COLORREF color = marker.mode == SelectionCombineMode::Add ? RGB(20, 220, 80) : RGB(240, 70, 70);
			HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
			HBRUSH brush = CreateSolidBrush(color);
			HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, pen));
			HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(hdc, brush));
			Ellipse(hdc, clientX - 5, clientY - 5, clientX + 6, clientY + 6);
			SelectObject(hdc, oldBrush);
			SelectObject(hdc, oldPen);
			DeleteObject(brush);
			DeleteObject(pen);
		}
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
			unsigned char* originalToDraw = scaledSrc->data();
			unsigned char* processedToDraw = scaledProc->data();
			if (gSelectionSession != nullptr && !gSelectionSession->previewOriginalDisplay.empty() &&
				!gSelectionSession->previewProcessedDisplay.empty())
			{
				originalToDraw = gSelectionSession->previewOriginalDisplay.data();
				processedToDraw = gSelectionSession->previewProcessedDisplay.data();
			}
			DrawMainPreviewComparison(paintDc, &BmHdrCopy, originalToDraw, processedToDraw,
			                          ScaledImageWidth, ScaledImageHeight, previewRect, gUiState.splitX,
			                          gUiState.compareHoldOriginal, gUiState.zoomToSelection, darkMode);
			DrawSelectionMarkers(paintDc, hwnd);
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
		gUiState.chromaProtection = GetPrivateProfileIntA("AltaLux", "ChromaProtection",
			Constants::DefaultChromaProtection, SetupIniFile);
		gUiState.zoomToSelection = GetPrivateProfileIntA("AltaLux", "Zoom", 0, SetupIniFile) != 0;
		gUiState.compareHoldOriginal = false;
		gUiState.draggingSplit = false;
		gUiState.splitX = 0;

		gUiState.strength = ClampInt(gUiState.strength, 0, 100);
		gUiState.detail = ClampInt(gUiState.detail, 0, 100);
		gUiState.naturalLook = ClampInt(gUiState.naturalLook, 0, 100);
		gUiState.chromaProtection = ClampInt(gUiState.chromaProtection, 0, 100);
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
		sprintf_s(valueBuffer, "%d", gUiState.chromaProtection);
		WritePrivateProfileStringA("AltaLux", "ChromaProtection", valueBuffer, SetupIniFile);
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
		ApplyPanelTypography(hwnd);
		SetSliderRanges(hwnd);
		LayoutControlsV2(hwnd);
		CenterSplitInPreview(hwnd);
		UpdateSliders(hwnd);
		DoPreviewProcessingV2();
		UpdateSelectionControls(hwnd);
		gHookDlg = hwnd;
		gKeyboardHook = SetWindowsHookExW(WH_GETMESSAGE, CompareHoldHookProc, nullptr, GetCurrentThreadId());
		return TRUE;
	}

	case WM_DESTROY:
		KillTimer(hwnd, PREVIEW_REFRESH_TIMER_ID);
		StopSelectionWorker();
		DiscardPendingSegmentationResults(hwnd);
		if (gSelectionSession != nullptr)
		{
			std::lock_guard<std::mutex> lock(gSelectionSession->moduleMutex);
			gSelectionSession->module.Unload();
		}
		if (gKeyboardHook != nullptr)
		{
			UnhookWindowsHookEx(gKeyboardHook);
			gKeyboardHook = nullptr;
		}
		gHookDlg = nullptr;
		if (gSectionFont != nullptr)
		{
			DeleteObject(gSectionFont);
			gSectionFont = nullptr;
		}
		BufferedPaintUnInit();
		return TRUE;

	case WM_COMMAND:
		switch (LOWORD(wparam))
		{
		case IDOK:
			if (gSelectionSession != nullptr && gSelectionSession->selectiveMode &&
				(!gSelectionSession->ready || !HasSelectedPixels(gSelectionSession->selectionMask)))
			{
				return TRUE;
			}
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

		case IDC_VIEW_FIT:
			gUiState.zoomToSelection = false;
			UpdateCommandLabels(hwnd);
			InvalidatePreview(hwnd);
			return TRUE;

		case IDC_VIEW_ACTUAL:
			gUiState.zoomToSelection = true;
			UpdateCommandLabels(hwnd);
			InvalidatePreview(hwnd);
			return TRUE;

		case IDC_MODE_ENTIRE_IMAGE:
			if (gSelectionSession != nullptr)
			{
				if (gSelectionSession->busy) StopSelectionWorker();
				gSelectionSession->selectiveMode = false;
				SetSelectionStatus(hwnd, L"");
				UpdateSelectionControls(hwnd);
				RefreshPreview(hwnd);
			}
			return TRUE;

		case IDC_MODE_SELECT_OBJECTS:
			if (gSelectionSession != nullptr)
			{
				if (gSelectionSession->ready)
				{
					gSelectionSession->selectiveMode = true;
					SetSelectionStatus(hwnd, L"Click an object to add it.");
					UpdateSelectionControls(hwnd);
					RefreshPreview(hwnd);
				}
				else if (!gSelectionSession->busy)
				{
					StartSegmentationPreparation(hwnd);
				}
			}
			return TRUE;

		case IDC_SELECTION_ADD:
			gSelectionSession->combineMode = SelectionCombineMode::Add;
			SetSelectionStatus(hwnd, L"Click an object to add it.");
			UpdateSelectionControls(hwnd);
			return TRUE;

		case IDC_SELECTION_REMOVE:
			gSelectionSession->combineMode = SelectionCombineMode::Remove;
			SetSelectionStatus(hwnd, L"Click an object to remove it.");
			UpdateSelectionControls(hwnd);
			return TRUE;

		case IDC_SELECTION_UNDO:
			if (gSelectionSession->history.Undo(gSelectionSession->selectionMask))
			{
				if (!gSelectionSession->markers.empty()) gSelectionSession->markers.pop_back();
				RefreshPreview(hwnd);
			}
			UpdateSelectionControls(hwnd);
			return TRUE;

		case IDC_SELECTION_CLEAR:
			if (gSelectionSession->history.Fill(gSelectionSession->selectionMask, 0))
			{
				gSelectionSession->markers.clear();
				RefreshPreview(hwnd);
			}
			UpdateSelectionControls(hwnd);
			return TRUE;

		case IDC_SELECTION_SELECT_ALL:
			if (gSelectionSession->history.Fill(gSelectionSession->selectionMask, 255))
			{
				RefreshPreview(hwnd);
			}
			UpdateSelectionControls(hwnd);
			return TRUE;

		case IDC_SELECTION_SHOW_MASK:
			gSelectionSession->showMask = SendMessage(GetDlgItem(hwnd, IDC_SELECTION_SHOW_MASK),
				BM_GETCHECK, 0, 0) == BST_CHECKED;
			RefreshPreview(hwnd);
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
		else if (slider == GetDlgItem(hwnd, IDC_CHROMA_SLIDER))
		{
			gUiState.chromaProtection = value;
		}
		else if (slider == GetDlgItem(hwnd, IDC_EDGE_SOFTNESS_SLIDER) && gSelectionSession != nullptr)
		{
			gSelectionSession->softness = value;
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
		if (gSelectionSession != nullptr && gSelectionSession->selectiveMode &&
			gSelectionSession->ready && !gSelectionSession->busy)
		{
			float sourceX = 0.0f;
			float sourceY = 0.0f;
			if (MapClientPointToSource(hwnd, x, y, sourceX, sourceY))
			{
				StartPointSegmentation(hwnd, sourceX, sourceY);
				return TRUE;
			}
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
		const RECT activeImageRect = GetActiveImageRect(hwnd);
		if (gSelectionSession != nullptr && gSelectionSession->selectiveMode &&
			gSelectionSession->ready && PtInRect(&activeImageRect, cursor))
		{
			SetCursor(LoadCursor(nullptr, IDC_CROSS));
			return TRUE;
		}
		break;
	}

	case WM_ALTALUX_SEGMENTATION_PREPARED:
	{
		std::unique_ptr<SegmentationWorkerResult> result(reinterpret_cast<SegmentationWorkerResult*>(lparam));
		FinishSelectionWorker();
		if (gSelectionSession == nullptr || result == nullptr || result->generation != gSelectionSession->generation)
		{
			return TRUE;
		}
		gSelectionSession->busy = false;
		if (result->status == SegmentationStatus::Ok)
		{
			gSelectionSession->ready = true;
			SetSelectionStatus(hwnd, L"Click an object to add it.");
		}
		else
		{
			gSelectionSession->ready = false;
			gSelectionSession->selectiveMode = false;
			const std::wstring message = result->error.empty() ?
				L"The optional AltaLux AI selection package is unavailable." : result->error;
			MessageBoxW(hwnd, message.c_str(), L"AltaLux object selection", MB_OK | MB_ICONINFORMATION);
		}
		UpdateSelectionControls(hwnd);
		RefreshPreview(hwnd);
		return TRUE;
	}

	case WM_ALTALUX_SEGMENTATION_COMPLETED:
	{
		std::unique_ptr<SegmentationWorkerResult> result(reinterpret_cast<SegmentationWorkerResult*>(lparam));
		FinishSelectionWorker();
		if (gSelectionSession == nullptr || result == nullptr || result->generation != gSelectionSession->generation)
		{
			return TRUE;
		}
		gSelectionSession->busy = false;
		if (result->status == SegmentationStatus::Ok &&
			gSelectionSession->history.Apply(gSelectionSession->selectionMask, result->operationMask.data(),
				result->operationMask.size(), result->mode))
		{
			gSelectionSession->markers.push_back({ result->point.x, result->point.y, result->mode });
			SetSelectionStatus(hwnd, result->mode == SelectionCombineMode::Add ?
				L"Object added. Click another object." : L"Object removed. Click another object.");
		}
		else if (result->status != SegmentationStatus::Ok)
		{
			const std::wstring message = result->error.empty() ? L"Object selection failed." : result->error;
			MessageBoxW(hwnd, message.c_str(), L"AltaLux object selection", MB_OK | MB_ICONWARNING);
			SetSelectionStatus(hwnd, L"Selection failed; try another point.");
		}
		UpdateSelectionControls(hwnd);
		RefreshPreview(hwnd);
		return TRUE;
	}

	case WM_PAINT:
		HandlePaintMessage(hwnd);
		return TRUE;

	case WM_CTLCOLORDLG:
	case WM_CTLCOLORSTATIC:
	case WM_CTLCOLORBTN:
	{
		HDC hdc = reinterpret_cast<HDC>(wparam);
		const HWND control = reinterpret_cast<HWND>(lparam);
		const int controlId = control != nullptr ? GetDlgCtrlID(control) : 0;
		const bool sectionHeader = controlId == IDC_ENHANCEMENT_SECTION ||
			controlId == IDC_VIEW_SECTION || controlId == IDC_APPLY_TO_SECTION ||
			controlId == IDC_SELECTION_SECTION;
		const bool secondaryText = controlId == IDC_DETAIL_HELP_STATIC ||
			controlId == IDC_NATURALLOOK_HELP_STATIC || controlId == IDC_CHROMA_HELP_STATIC ||
			controlId == IDC_SELECTION_STATUS;
		SetBkMode(hdc, TRANSPARENT);
		if (sectionHeader)
		{
			SetTextColor(hdc, gCurrentTheme == ThemeMode::Dark ? RGB(142, 190, 255) : RGB(0, 92, 184));
		}
		else if (secondaryText)
		{
			SetTextColor(hdc, gCurrentTheme == ThemeMode::Dark ? RGB(184, 184, 184) : RGB(88, 88, 88));
		}
		else
		{
			SetTextColor(hdc, gCurrentTheme == ThemeMode::Dark ? RGB(245, 245, 245) : RGB(24, 24, 24));
		}
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
		SourceTopDown = bitmapHeader->biHeight < 0;

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
		FullSrcImagePtr = srcImage;
		gSelectionSession = std::make_unique<SelectionDialogSession>();

		ScaleDownImage(srcImage->data(), ImageWidth, ImageHeight, scaledSrcImage->data(), ScalingFactor, ImageBitDepth);
		CopyScaledSrcImage(scaledProcImage->data());

		const INT_PTR dialogResult = DialogBox(hDll, MAKEINTRESOURCE(IDD_ALTALUX_DIALOG), hwnd, DlgProc);
		if (dialogResult == -1)
		{
			gSelectionSession.reset();
			FullSrcImagePtr.reset();
			return false;
		}

		if (SkipProcessing)
		{
			gSelectionSession.reset();
			FullSrcImagePtr.reset();
			return true;
		}
	}
	else
	{
		gUiState.strength = ClampInt(param1, 0, 100);
		gUiState.detail = Constants::DefaultDetail;
		gUiState.naturalLook = Constants::DefaultNatural;
		gUiState.chromaProtection = Constants::DefaultChromaProtection;
		gUiState.zoomToSelection = false;
		gUiState.compareHoldOriginal = false;
		gUiState.draggingSplit = false;
		gUiState.splitX = 0;
	}

	const bool applySelection = gSelectionSession != nullptr && gSelectionSession->selectiveMode &&
		gSelectionSession->ready && HasSelectedPixels(gSelectionSession->selectionMask);
	if (applySelection)
	{
		std::vector<unsigned char> processedImage(srcImage->begin(),
			srcImage->begin() + GetImageByteCount(ImageWidth, ImageHeight));
		if (!ProcessMultiscaleImage(srcImage->data(), processedImage.data(), ImageWidth, ImageHeight, ImageBitDepth, gUiState))
		{
			gSelectionSession.reset();
			FullSrcImagePtr.reset();
			return false;
		}
		std::vector<unsigned char> alphaMask;
		if (!BuildFeatheredMask(gSelectionSession->selectionMask.data(), ImageWidth, ImageHeight,
			gSelectionSession->softness, alphaMask) ||
			!CompositeWithMask(srcImage->data(), processedImage.data(), srcImage->data(), alphaMask.data(),
				ImageWidth, ImageHeight, ImageBitDepth))
		{
			gSelectionSession.reset();
			FullSrcImagePtr.reset();
			return false;
		}
	}
	else if (!ProcessMultiscaleImage(srcImage->data(), srcImage->data(), ImageWidth, ImageHeight, ImageBitDepth, gUiState))
	{
		gSelectionSession.reset();
		FullSrcImagePtr.reset();
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
	gSelectionSession.reset();
	FullSrcImagePtr.reset();

	return true;
}

int __cdecl GetPlugInInfo(char* versionString, char* fileFormats)
{
	sprintf_s(versionString, 64, ALTALUX_VERSION_DISPLAY_STRING);
	sprintf_s(fileFormats, 256, "AltaLux image enhancement filter");
	return 0;
}

bool __cdecl AltaLux_Effects(HANDLE hDib, HWND hwnd, int filter, RECT rect, int param1, int param2, char* iniFile,
                             char* szAppName, int regID)
{
	return StartEffects2(hDib, hwnd, filter, rect, param1, param2, iniFile, szAppName, regID);
}
