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
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "Kernels/Kernels.h"
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
	const int MIN_DIALOG_WIDTH = 960;
	// Tall enough for the full selection panel at 96 DPI (about 790 px of
	// client area)
	const int MIN_DIALOG_HEIGHT = 830;
	const int SPLIT_HIT_RADIUS = 8;
	// Client-pixel movement beyond which a selective-mode press counts as a
	// pan instead of an object pick
	const int PICK_DRAG_SLOP = 4;
	const int PRESET_TOLERANCE = 3;
	const UINT_PTR PREVIEW_REFRESH_TIMER_ID = 1;
	const UINT_PTR PREVIEW_BUSY_TIMER_ID = 2;
	const UINT PREVIEW_REFRESH_DELAY_MS = 40;
	const UINT PREVIEW_BUSY_ANIMATION_MS = 30;
	const UINT WM_ALTALUX_SEGMENTATION_PREPARED = WM_APP + 42;
	const UINT WM_ALTALUX_SEGMENTATION_COMPLETED = WM_APP + 43;
	const UINT WM_ALTALUX_PREVIEW_READY = WM_APP + 44;
	const UINT WM_ALTALUX_APPLY_DONE = WM_APP + 45;
	const UINT_PTR APPLY_BUSY_TIMER_ID = 1;

	// The plugin DLL is loaded into the IrfanView process, so a DLL manifest
	// cannot claim DPI awareness for us (the host's manifest wins). Instead we
	// try to upgrade the process at runtime before the dialog is shown; the
	// call simply fails if the host already declared its own awareness.
	void TryEnablePerMonitorDpiAwareness()
	{
		using SetProcessDpiAwarenessContextProc = BOOL(WINAPI*)(HANDLE);
		const HANDLE perMonitorV2 = reinterpret_cast<HANDLE>(-4);
		HMODULE user32 = GetModuleHandleW(L"user32.dll");
		if (user32 == nullptr)
		{
			return;
		}
		auto proc = reinterpret_cast<SetProcessDpiAwarenessContextProc>(
			GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
		if (proc != nullptr)
		{
			proc(perMonitorV2);
		}
	}

	UINT gDialogDpi = USER_DEFAULT_SCREEN_DPI;

	UINT GetWindowDpiSafe(HWND hwnd)
	{
		using GetDpiForWindowProc = UINT(WINAPI*)(HWND);
		HMODULE user32 = GetModuleHandleW(L"user32.dll");
		if (user32 != nullptr)
		{
			auto proc = reinterpret_cast<GetDpiForWindowProc>(GetProcAddress(user32, "GetDpiForWindow"));
			if (proc != nullptr)
			{
				const UINT dpi = proc(hwnd);
				if (dpi != 0)
				{
					return dpi;
				}
			}
		}
		HDC dc = GetDC(hwnd);
		if (dc == nullptr)
		{
			return USER_DEFAULT_SCREEN_DPI;
		}
		const UINT dpi = static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSX));
		ReleaseDC(hwnd, dc);
		return dpi;
	}

	int ScaleForDpi(int value)
	{
		return MulDiv(value, gDialogDpi, USER_DEFAULT_SCREEN_DPI);
	}

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
		0,
		1.0,
		0,
		0,
		false,
		0,
		0,
		0,
		0,
		false,
		0.0f,
		0.0f
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
	HFONT gHelperFont = nullptr;
	HFONT gDialogFont = nullptr;

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
		previewRect.left = ScaleForDpi(PREVIEW_MARGIN);
		previewRect.top = ScaleForDpi(PREVIEW_MARGIN);
		const int minimumPreviewRight = previewRect.left + ScaleForDpi(320);
		const int preferredPreviewRight = clientRect.right - ScaleForDpi(PANEL_WIDTH + PANEL_GAP + PREVIEW_MARGIN);
		previewRect.right = preferredPreviewRight > minimumPreviewRight ? preferredPreviewRight : minimumPreviewRight;
		previewRect.bottom = clientRect.bottom - ScaleForDpi(PREVIEW_MARGIN);
		return previewRect;
	}

	void ClampSplitToPreview(HWND hwnd);
	void InvalidatePreview(HWND hwnd);
	RECT GetActiveImageRect(HWND hwnd);

	RECT GetZoomedDestRect(HWND hwnd)
	{
		return GetZoomedImageRect(ScaledImageWidth, ScaledImageHeight, GetPreviewRect(hwnd),
		                          gUiState.zoomFactor, gUiState.panX, gUiState.panY);
	}

	bool IsImagePannable(HWND hwnd)
	{
		const RECT previewRect = GetPreviewRect(hwnd);
		const RECT destRect = GetZoomedDestRect(hwnd);
		return RectWidth(destRect) > RectWidth(previewRect) || RectHeight(destRect) > RectHeight(previewRect);
	}

	void SyncZoomRadioButtons(HWND hwnd)
	{
		const bool isFit = gUiState.zoomFactor <= 1.0;
		const double actualScale = GetActualPixelScale(ScaledImageWidth, ScaledImageHeight,
			GetPreviewRect(hwnd), gUiState.zoomFactor);
		const bool isActual = !isFit && (actualScale > 0.99) && (actualScale < 1.01);
		SendMessage(GetDlgItem(hwnd, IDC_VIEW_FIT), BM_SETCHECK,
			isFit ? BST_CHECKED : BST_UNCHECKED, 0);
		SendMessage(GetDlgItem(hwnd, IDC_VIEW_ACTUAL), BM_SETCHECK,
			isActual ? BST_CHECKED : BST_UNCHECKED, 0);
	}

	void ResetZoomTo(HWND hwnd, double zoomFactor)
	{
		gUiState.zoomFactor = zoomFactor;
		gUiState.panX = 0;
		gUiState.panY = 0;
		gUiState.zoomToSelection = zoomFactor > 1.0;
		ClampSplitToPreview(hwnd);
		SyncZoomRadioButtons(hwnd);
		InvalidatePreview(hwnd);
	}

	// Zoom about the given client point so the image pixel under the cursor
	// stays under the cursor; falls back to a centered zoom when the point is
	// outside the current image rect.
	void ZoomAtClientPoint(HWND hwnd, int x, int y, double newZoom)
	{
		const RECT container = GetPreviewRect(hwnd);
		newZoom = (std::max)(1.0, (std::min)(32.0, newZoom));
		if (newZoom <= 1.0)
		{
			ResetZoomTo(hwnd, 1.0);
			return;
		}

		const RECT oldRect = GetZoomedDestRect(hwnd);
		const double imageFractionX = static_cast<double>(x - oldRect.left) / RectWidth(oldRect);
		const double imageFractionY = static_cast<double>(y - oldRect.top) / RectHeight(oldRect);

		const RECT newRect = GetZoomedImageRect(ScaledImageWidth, ScaledImageHeight, container, newZoom, 0, 0);
		const int centerX = (container.left + container.right) / 2;
		const int centerY = (container.top + container.bottom) / 2;

		int panX = 0;
		int panY = 0;
		if (imageFractionX >= 0.0 && imageFractionX <= 1.0 && imageFractionY >= 0.0 && imageFractionY <= 1.0)
		{
			const int anchorLeft = x - static_cast<int>(imageFractionX * RectWidth(newRect));
			const int anchorTop = y - static_cast<int>(imageFractionY * RectHeight(newRect));
			panX = anchorLeft - (centerX - RectWidth(newRect) / 2);
			panY = anchorTop - (centerY - RectHeight(newRect) / 2);
		}

		ClampPanOffsets(ScaledImageWidth, ScaledImageHeight, container, newZoom, panX, panY);
		gUiState.zoomFactor = newZoom;
		gUiState.panX = panX;
		gUiState.panY = panY;
		gUiState.zoomToSelection = true;
		ClampSplitToPreview(hwnd);
		SyncZoomRadioButtons(hwnd);
		InvalidatePreview(hwnd);
	}

	void SetActualSizeZoom(HWND hwnd)
	{
		const RECT container = GetPreviewRect(hwnd);
		const RECT fitRect = FitImageRect(container, ScaledImageWidth, ScaledImageHeight);
		if (RectWidth(fitRect) <= 0)
		{
			ResetZoomTo(hwnd, 1.0);
			return;
		}
		const double actualZoom = static_cast<double>(ScaledImageWidth) / RectWidth(fitRect);
		ResetZoomTo(hwnd, (std::max)(1.0, actualZoom));
		gUiState.zoomToSelection = true;
		SyncZoomRadioButtons(hwnd);
	}

	RECT GetActiveImageRect(HWND hwnd)
	{
		RECT visible = GetPreviewRect(hwnd);
		IntersectRect(&visible, &visible, &GetZoomedDestRect(hwnd));
		return visible;
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

	// Preview jobs are snapshots of everything the processing kernel needs, so
	// the worker never touches dialog state that the UI thread mutates while a
	// job is in flight.
	struct PreviewJob
	{
		std::uint32_t generation = 0;
		UiState ui = {};
		bool selective = false;
		bool showMask = false;
		int softness = 3;
		std::vector<unsigned char> selectionMask;
	};

	struct PreviewResult
	{
		std::uint32_t generation = 0;
		std::vector<unsigned char> processed;
		std::vector<unsigned char> originalDisplay;
		std::vector<unsigned char> processedDisplay;
	};

	struct PreviewWorkerState
	{
		HWND hwnd = nullptr;
		std::thread thread;
		std::mutex mutex;
		std::condition_variable wake;
		std::optional<PreviewJob> pending;
		bool stop = false;
		std::atomic<std::uint32_t> generation{ 0 };
	};

	std::unique_ptr<PreviewWorkerState> gPreviewWorker;
	bool gPreviewBusy = false;
	float gPreviewBusyPosition = 0.0f;
	// Display-resolution copies of the preview images for the paint pass;
	// cleared whenever a preview result replaces the image content.
	PreviewDisplayCache gOriginalDisplayCache;
	PreviewDisplayCache gProcessedDisplayCache;

	void ProcessPreviewJob(const PreviewJob& job, const SharedImagePtr& scaledSrc, PreviewResult& result)
	{
		if (scaledSrc == nullptr)
		{
			return;
		}

		const std::size_t byteCount = static_cast<std::size_t>(GetImageByteCount(ScaledImageWidth, ScaledImageHeight));
		result.processed.assign(scaledSrc->begin(), scaledSrc->begin() + byteCount);
		ProcessMultiscaleImage(scaledSrc->data(), result.processed.data(), ScaledImageWidth,
			ScaledImageHeight, ImageBitDepth, job.ui);

		if (!job.selective || job.selectionMask.empty())
		{
			return;
		}

		const std::size_t previewPixelCount = static_cast<std::size_t>(ScaledImageWidth) * ScaledImageHeight;
		std::vector<unsigned char> previewMask(previewPixelCount);
		ResizeMaskNearest(job.selectionMask.data(), ImageWidth, ImageHeight,
			previewMask.data(), ScaledImageWidth, ScaledImageHeight);

		std::vector<unsigned char> previewAlpha;
		const int previewSoftness = job.softness == 0 ? 0 : (std::max)(1, job.softness / ScalingFactor);
		if (!BuildFeatheredMask(previewMask.data(), ScaledImageWidth, ScaledImageHeight,
			previewSoftness, previewAlpha))
		{
			return;
		}
		CompositeWithMask(scaledSrc->data(), result.processed.data(), result.processed.data(),
			previewAlpha.data(), ScaledImageWidth, ScaledImageHeight, ImageBitDepth);

		if (!job.showMask)
		{
			return;
		}

		result.originalDisplay.assign(scaledSrc->begin(), scaledSrc->begin() + byteCount);
		result.processedDisplay.assign(result.processed.begin(), result.processed.begin() + byteCount);
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
		applyOverlay(result.originalDisplay);
		applyOverlay(result.processedDisplay);
	}

	void PreviewWorkerLoop(PreviewWorkerState& worker, SharedImagePtr scaledSrc)
	{
		for (;;)
		{
			PreviewJob job;
			{
				std::unique_lock<std::mutex> lock(worker.mutex);
				worker.wake.wait(lock, [&worker]()
				{
					return worker.stop || worker.pending.has_value();
				});
				if (worker.stop)
				{
					return;
				}
				job = std::move(*worker.pending);
				worker.pending.reset();
			}

			auto* result = new PreviewResult();
			result->generation = job.generation;
			ProcessPreviewJob(job, scaledSrc, *result);
			if (PostMessage(worker.hwnd, WM_ALTALUX_PREVIEW_READY, 0,
				reinterpret_cast<LPARAM>(result)) == FALSE)
			{
				delete result;
			}
		}
	}

	void StartPreviewWorker(HWND hwnd)
	{
		if (gPreviewWorker != nullptr || ScaledSrcImagePtr.lock() == nullptr)
		{
			return;
		}
		gPreviewWorker = std::make_unique<PreviewWorkerState>();
		gPreviewWorker->hwnd = hwnd;
		gPreviewWorker->thread = std::thread(PreviewWorkerLoop, std::ref(*gPreviewWorker),
			ScaledSrcImagePtr.lock());
	}

	void StopPreviewWorker()
	{
		if (gPreviewWorker == nullptr)
		{
			return;
		}
		{
			std::lock_guard<std::mutex> lock(gPreviewWorker->mutex);
			gPreviewWorker->stop = true;
		}
		gPreviewWorker->wake.notify_all();
		if (gPreviewWorker->thread.joinable())
		{
			gPreviewWorker->thread.join();
		}
		gPreviewWorker.reset();
		gPreviewBusy = false;
	}

	void DiscardPendingPreviewResults(HWND hwnd)
	{
		MSG message = {};
		while (PeekMessage(&message, hwnd, WM_ALTALUX_PREVIEW_READY, WM_ALTALUX_PREVIEW_READY, PM_REMOVE))
		{
			delete reinterpret_cast<PreviewResult*>(message.lParam);
		}
	}

	void GetPreviewBusyStripRect(HWND hwnd, RECT& stripRect)
	{
		const RECT previewRect = GetPreviewRect(hwnd);
		stripRect.left = previewRect.left;
		stripRect.top = previewRect.top;
		stripRect.right = previewRect.right;
		stripRect.bottom = previewRect.top + 4;
	}

	void SetPreviewBusy(HWND hwnd, bool busy)
	{
		if (gPreviewBusy == busy)
		{
			return;
		}
		gPreviewBusy = busy;
		if (busy)
		{
			SetTimer(hwnd, PREVIEW_BUSY_TIMER_ID, PREVIEW_BUSY_ANIMATION_MS, nullptr);
		}
		else
		{
			KillTimer(hwnd, PREVIEW_BUSY_TIMER_ID);
			RECT stripRect = {};
			GetPreviewBusyStripRect(hwnd, stripRect);
			InvalidateRect(hwnd, &stripRect, FALSE);
		}
	}

	void DrawPreviewBusyLine(HDC hdc, const RECT& previewRect)
	{
		const int width = previewRect.right - previewRect.left;
		const int band = (std::max)(24, width / 4);
		const int travel = width + band;
		int x = previewRect.left + static_cast<int>(gPreviewBusyPosition * travel) - band;
		x = (std::max)(static_cast<int>(previewRect.left), (std::min)(x, static_cast<int>(previewRect.right) - 1));
		RECT lineRect = { x, previewRect.top, (std::min)(static_cast<int>(previewRect.right), x + band), previewRect.top + 3 };
		const HBRUSH brush = CreateSolidBrush(RGB(0, 120, 215));
		FillRect(hdc, &lineRect, brush);
		DeleteObject(brush);
	}

	// Borderless popup shown while the full-resolution image is processed on a
	// worker thread after the dialog closes; the calling thread pumps messages
	// so the indeterminate band keeps animating without freezing the host.
	class ApplyProgressPopup
	{
	public:
		ApplyProgressPopup(HWND parent)
		{
			RegisterWindowClass();
			BufferedPaintInit();
			RECT parentRect = {};
			GetWindowRect(parent, &parentRect);
			const int width = ScaleForDpi(340);
			const int height = ScaleForDpi(110);
			const int x = parentRect.left + ((std::max)(0, static_cast<int>(parentRect.right - parentRect.left)) - width) / 2;
			const int y = parentRect.top + ((std::max)(0, static_cast<int>(parentRect.bottom - parentRect.top)) - height) / 2;
			hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, ClassName, L"AltaLux",
				WS_POPUP | WS_BORDER, (std::max)(0, x), (std::max)(0, y), width, height,
				parent, nullptr, hDll, this);
			if (hwnd_ != nullptr)
			{
				ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
				UpdateWindow(hwnd_);
				SetTimer(hwnd_, APPLY_BUSY_TIMER_ID, PREVIEW_BUSY_ANIMATION_MS, nullptr);
			}
		}

		~ApplyProgressPopup()
		{
			if (hwnd_ != nullptr)
			{
				DestroyWindow(hwnd_);
			}
			BufferedPaintUnInit();
		}

		HWND hwnd() const { return hwnd_; }
		bool succeeded() const { return succeeded_; }

		void NotifyDone(bool success)
		{
			PostMessage(hwnd_, WM_ALTALUX_APPLY_DONE, success ? 1 : 0, 0);
		}

		void PumpUntilDone()
		{
			MSG message = {};
			while (!done_ && GetMessage(&message, nullptr, 0, 0) > 0)
			{
				TranslateMessage(&message);
				DispatchMessage(&message);
			}
		}

	private:
		static constexpr const wchar_t* ClassName = L"AltaLuxApplyProgress";

		static void RegisterWindowClass()
		{
			WNDCLASSW windowClass = {};
			windowClass.lpfnWndProc = &ApplyProgressPopup::WindowProc;
			windowClass.hInstance = hDll;
			windowClass.hCursor = LoadCursor(nullptr, IDC_WAIT);
			windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
			windowClass.lpszClassName = ClassName;
			RegisterClassW(&windowClass);
		}

		static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
		{
			ApplyProgressPopup* self = nullptr;
			if (msg == WM_NCCREATE)
			{
				self = reinterpret_cast<ApplyProgressPopup*>(
					reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
				SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
			}
			else
			{
				self = reinterpret_cast<ApplyProgressPopup*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
			}
			if (self != nullptr && self->hwnd_ == hwnd)
			{
				return self->HandleMessage(hwnd, msg, wparam, lparam);
			}
			return DefWindowProcW(hwnd, msg, wparam, lparam);
		}

		LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
		{
			switch (msg)
			{
			case WM_ALTALUX_APPLY_DONE:
				succeeded_ = wparam != 0;
				done_ = true;
				DestroyWindow(hwnd);
				return 0;

			case WM_TIMER:
				if (wparam == APPLY_BUSY_TIMER_ID)
				{
					position_ += 0.02f;
					if (position_ > 1.0f) position_ = 0.0f;
					RECT bandRect = {};
					GetBandRect(bandRect);
					InvalidateRect(hwnd, &bandRect, FALSE);
					return 0;
				}
				break;

			case WM_PAINT:
			{
				PAINTSTRUCT ps = {};
				HDC hdc = BeginPaint(hwnd, &ps);
				if (hdc != nullptr)
				{
					RECT clientRect = {};
					GetClientRect(hwnd, &clientRect);
					HDC paintDc = nullptr;
					HPAINTBUFFER paintBuffer = BeginBufferedPaint(hdc, &ps.rcPaint,
						BPBF_COMPATIBLEBITMAP, nullptr, &paintDc);
					if (paintBuffer == nullptr) paintDc = hdc;

					const bool darkMode = gCurrentTheme == ThemeMode::Dark;
					FillRect(paintDc, &clientRect, darkMode ?
	 static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)) : reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
					SetBkMode(paintDc, TRANSPARENT);
					SetTextColor(paintDc, darkMode ? RGB(245, 245, 245) : RGB(24, 24, 24));
					HFONT guiFont = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
					HFONT oldFont = static_cast<HFONT>(SelectObject(paintDc, guiFont));
					RECT textRect = clientRect;
					textRect.bottom -= ScaleForDpi(24);
					DrawTextW(paintDc, L"Enhancing full-resolution image...", -1, &textRect,
						DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
					SelectObject(paintDc, oldFont);

					RECT bandRect = {};
					GetBandRect(bandRect);
					RECT trackRect = bandRect;
					const HBRUSH trackBrush = CreateSolidBrush(darkMode ? RGB(64, 64, 64) : RGB(228, 228, 228));
					FillRect(paintDc, &trackRect, trackBrush);
					DeleteObject(trackBrush);
					const int trackWidth = trackRect.right - trackRect.left;
					const int band = (std::max)(24, trackWidth / 4);
					const int travel = trackWidth + band;
					int x = trackRect.left + static_cast<int>(position_ * travel) - band;
					x = (std::max)(static_cast<int>(trackRect.left), (std::min)(x, static_cast<int>(trackRect.right) - 1));
					RECT lineRect = { x, trackRect.top, x + (std::min)(trackWidth, band), trackRect.bottom };
					const HBRUSH brush = CreateSolidBrush(RGB(0, 120, 215));
					FillRect(paintDc, &lineRect, brush);
					DeleteObject(brush);

					if (paintBuffer != nullptr) EndBufferedPaint(paintBuffer, TRUE);
					EndPaint(hwnd, &ps);
				}
				return 0;
			}

			case WM_DESTROY:
				KillTimer(hwnd, APPLY_BUSY_TIMER_ID);
				hwnd_ = nullptr;
				return 0;
			}
			return DefWindowProcW(hwnd, msg, wparam, lparam);
		}

		void GetBandRect(RECT& bandRect)
		{
			RECT clientRect = {};
			GetClientRect(hwnd_, &clientRect);
			bandRect.left = clientRect.left + ScaleForDpi(24);
			bandRect.right = clientRect.right - ScaleForDpi(24);
			bandRect.top = clientRect.bottom - ScaleForDpi(22);
			bandRect.bottom = clientRect.bottom - ScaleForDpi(16);
		}

		HWND hwnd_ = nullptr;
		bool done_ = false;
		bool succeeded_ = false;
		float position_ = 0.0f;
	};

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
		SyncZoomRadioButtons(hwnd);
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
		HFONT dialogFont = reinterpret_cast<HFONT>(SendMessage(hwnd, WM_GETFONT, 0, 0));
		if (gSectionFont != nullptr)
		{
			DeleteObject(gSectionFont);
			gSectionFont = nullptr;
		}
		if (gHelperFont != nullptr)
		{
			DeleteObject(gHelperFont);
			gHelperFont = nullptr;
		}
		LOGFONTW sectionLogFont = {};
		if (dialogFont != nullptr && GetObjectW(dialogFont, sizeof(sectionLogFont), &sectionLogFont) != 0)
		{
			sectionLogFont.lfWeight = FW_SEMIBOLD;
			gSectionFont = CreateFontIndirectW(&sectionLogFont);
		}
		LOGFONTW helperLogFont = {};
		if (dialogFont != nullptr && GetObjectW(dialogFont, sizeof(helperLogFont), &helperLogFont) != 0)
		{
			helperLogFont.lfWeight = FW_NORMAL;
			// Helper text sits one size below the 9pt body text
			helperLogFont.lfHeight = -MulDiv(8, gDialogDpi, 72);
			gHelperFont = CreateFontIndirectW(&helperLogFont);
		}
		const int sectionIds[] = {
			IDC_ENHANCEMENT_SECTION, IDC_VIEW_SECTION, IDC_APPLY_TO_SECTION, IDC_SELECTION_SECTION
		};
		for (const int controlId : sectionIds)
		{
			SendMessage(GetDlgItem(hwnd, controlId), WM_SETFONT,
				reinterpret_cast<WPARAM>(gSectionFont != nullptr ? gSectionFont : dialogFont), TRUE);
		}
		const int helperIds[] = {
			IDC_DETAIL_HELP_STATIC, IDC_NATURALLOOK_HELP_STATIC, IDC_CHROMA_HELP_STATIC, IDC_SELECTION_STATUS
		};
		for (const int controlId : helperIds)
		{
			SendMessage(GetDlgItem(hwnd, controlId), WM_SETFONT,
				reinterpret_cast<WPARAM>(gHelperFont != nullptr ? gHelperFont : dialogFont), TRUE);
		}
	}

	BOOL CALLBACK ApplyFontToChild(HWND child, LPARAM font)
	{
		SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(font), TRUE);
		return TRUE;
	}

	void ApplyDialogFont(HWND hwnd)
	{
		if (gDialogFont != nullptr)
		{
			DeleteObject(gDialogFont);
			gDialogFont = nullptr;
		}
		LOGFONTW logFont = {};
		// 9pt body text; lfHeight is in pixels, so convert points via the dialog DPI
		logFont.lfHeight = -MulDiv(9, gDialogDpi, 72);
		logFont.lfWeight = FW_NORMAL;
		logFont.lfCharSet = DEFAULT_CHARSET;
		logFont.lfQuality = CLEARTYPE_QUALITY;
		wcscpy_s(logFont.lfFaceName, L"Segoe UI");
		gDialogFont = CreateFontIndirectW(&logFont);
		const HFONT font = gDialogFont != nullptr ? gDialogFont :
			reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
		SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
		EnumChildWindows(hwnd, ApplyFontToChild, reinterpret_cast<LPARAM>(font));
	}

	void SetSelectionStatus(HWND hwnd, const wchar_t* text)
	{
		SetWindowTextW(GetDlgItem(hwnd, IDC_SELECTION_STATUS), text != nullptr ? text : L"");
	}

	void SetSliderValueText(HWND hwnd, int controlId, int value)
	{
		wchar_t text[16];
		swprintf_s(text, L"%d", value);
		SetWindowTextW(GetDlgItem(hwnd, controlId), text);
	}

	void UpdateSliderValues(HWND hwnd)
	{
		SetSliderValueText(hwnd, IDC_STRENGTH_VALUE, gUiState.strength);
		SetSliderValueText(hwnd, IDC_DETAIL_VALUE, gUiState.detail);
		SetSliderValueText(hwnd, IDC_NATURALLOOK_VALUE, gUiState.naturalLook);
		SetSliderValueText(hwnd, IDC_CHROMA_VALUE, gUiState.chromaProtection);
		if (gSelectionSession != nullptr)
		{
			SetSliderValueText(hwnd, IDC_EDGE_SOFTNESS_VALUE, gSelectionSession->softness);
		}
	}

	void UpdateSelectionControls(HWND hwnd)
	{
		const int selectiveControls[] = {
			IDC_SELECTION_ADD, IDC_SELECTION_REMOVE, IDC_SELECTION_UNDO, IDC_SELECTION_CLEAR,
			IDC_SELECTION_SELECT_ALL, IDC_SELECTION_SHOW_MASK, IDC_EDGE_SOFTNESS_STATIC,
			IDC_EDGE_SOFTNESS_SLIDER, IDC_EDGE_SOFTNESS_VALUE, IDC_SELECTION_STATUS, IDC_SELECTION_SECTION
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
			SetSliderValueText(hwnd, IDC_EDGE_SOFTNESS_VALUE, gSelectionSession->softness);
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
		UpdateSliderValues(hwnd);
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

		// Vertical rhythm: rows inside a section sit 10-14 design px apart while
		// every section header gets 22 px of clearance above it, so each section
		// reads as one group; DrawPanelSectionSeparators adds a hairline under
		// each header to reinforce the grouping. Text rows are 16 design px tall
		// so the 9pt body font renders without clipping descenders.
		const int panelLeft = previewRect.right + ScaleForDpi(PANEL_GAP);
		const int panelWidth = clientRect.right - panelLeft - ScaleForDpi(PREVIEW_MARGIN);
		const int textRowHeight = ScaleForDpi(16);
		const int valueWidth = ScaleForDpi(34);
		const int sliderHeight = ScaleForDpi(28);
		// Every button shares one height so rows line up across sections
		const int buttonHeight = ScaleForDpi(24);
		const int rowGap = ScaleForDpi(10);
		const int sectionClearance = ScaleForDpi(22);
		const int headerToRowGap = ScaleForDpi(12);
		const int captionToSliderGap = ScaleForDpi(2);
		const int sliderToHelperGap = ScaleForDpi(4);
		const int top = ScaleForDpi(PREVIEW_MARGIN + 2);

		const int labelWidth = panelWidth - valueWidth - ScaleForDpi(4);
		const int twoButtonWidth = (panelWidth - rowGap) / 2;
		const int threeButtonWidth = (panelWidth - (2 * rowGap)) / 3;

		// Rows stack downward from the top margin; y sits at the top of the
		// row being placed and advances by that row's height plus its gap.
		int y = top;
		MoveWindow(GetDlgItem(hwnd, IDC_ENHANCEMENT_SECTION), panelLeft, y, panelWidth, textRowHeight, TRUE);
		y += textRowHeight + headerToRowGap;

		MoveWindow(GetDlgItem(hwnd, IDC_STRENGTH_STATIC), panelLeft, y, labelWidth, textRowHeight, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_STRENGTH_VALUE), panelLeft + panelWidth - valueWidth, y, valueWidth, textRowHeight, TRUE);
		y += textRowHeight + captionToSliderGap;
		MoveWindow(GetDlgItem(hwnd, IDC_STRENGTH_SLIDER), panelLeft, y, panelWidth, sliderHeight, TRUE);
		y += sliderHeight + rowGap;

		MoveWindow(GetDlgItem(hwnd, IDC_DETAIL_STATIC), panelLeft, y, labelWidth, textRowHeight, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_DETAIL_VALUE), panelLeft + panelWidth - valueWidth, y, valueWidth, textRowHeight, TRUE);
		y += textRowHeight + captionToSliderGap;
		MoveWindow(GetDlgItem(hwnd, IDC_DETAIL_SLIDER), panelLeft, y, panelWidth, sliderHeight, TRUE);
		y += sliderHeight + sliderToHelperGap;
		MoveWindow(GetDlgItem(hwnd, IDC_DETAIL_HELP_STATIC), panelLeft, y, panelWidth, textRowHeight, TRUE);
		y += textRowHeight + rowGap;

		MoveWindow(GetDlgItem(hwnd, IDC_NATURALLOOK_STATIC), panelLeft, y, labelWidth, textRowHeight, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_NATURALLOOK_VALUE), panelLeft + panelWidth - valueWidth, y, valueWidth, textRowHeight, TRUE);
		y += textRowHeight + captionToSliderGap;
		MoveWindow(GetDlgItem(hwnd, IDC_NATURALLOOK_SLIDER), panelLeft, y, panelWidth, sliderHeight, TRUE);
		y += sliderHeight + sliderToHelperGap;
		MoveWindow(GetDlgItem(hwnd, IDC_NATURALLOOK_HELP_STATIC), panelLeft, y, panelWidth, textRowHeight, TRUE);
		y += textRowHeight + rowGap;

		MoveWindow(GetDlgItem(hwnd, IDC_CHROMA_STATIC), panelLeft, y, labelWidth, textRowHeight, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_CHROMA_VALUE), panelLeft + panelWidth - valueWidth, y, valueWidth, textRowHeight, TRUE);
		y += textRowHeight + captionToSliderGap;
		MoveWindow(GetDlgItem(hwnd, IDC_CHROMA_SLIDER), panelLeft, y, panelWidth, sliderHeight, TRUE);
		y += sliderHeight + sliderToHelperGap;
		MoveWindow(GetDlgItem(hwnd, IDC_CHROMA_HELP_STATIC), panelLeft, y, panelWidth, textRowHeight, TRUE);
		y += textRowHeight + ScaleForDpi(12);

		MoveWindow(GetDlgItem(hwnd, IDC_PRESET_NATURAL), panelLeft, y, threeButtonWidth, buttonHeight, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_PRESET_BALANCED), panelLeft + threeButtonWidth + rowGap, y, threeButtonWidth, buttonHeight, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_PRESET_DETAIL), panelLeft + (2 * (threeButtonWidth + rowGap)), y, threeButtonWidth, buttonHeight, TRUE);
		y += buttonHeight;

		y += sectionClearance;
		MoveWindow(GetDlgItem(hwnd, IDC_VIEW_SECTION), panelLeft, y, panelWidth, textRowHeight, TRUE);
		y += textRowHeight + headerToRowGap;
		MoveWindow(GetDlgItem(hwnd, IDC_VIEW_FIT), panelLeft, y, twoButtonWidth, buttonHeight, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_VIEW_ACTUAL), panelLeft + twoButtonWidth + rowGap, y, twoButtonWidth, buttonHeight, TRUE);
		y += buttonHeight;

		y += sectionClearance;
		MoveWindow(GetDlgItem(hwnd, IDC_APPLY_TO_SECTION), panelLeft, y, panelWidth, textRowHeight, TRUE);
		y += textRowHeight + headerToRowGap;
		MoveWindow(GetDlgItem(hwnd, IDC_MODE_ENTIRE_IMAGE), panelLeft, y, twoButtonWidth, buttonHeight, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_MODE_SELECT_OBJECTS), panelLeft + twoButtonWidth + rowGap, y, twoButtonWidth, buttonHeight, TRUE);
		y += buttonHeight;

		y += sectionClearance;
		MoveWindow(GetDlgItem(hwnd, IDC_SELECTION_SECTION), panelLeft, y, panelWidth, textRowHeight, TRUE);
		y += textRowHeight + headerToRowGap;
		MoveWindow(GetDlgItem(hwnd, IDC_SELECTION_ADD), panelLeft, y, twoButtonWidth, buttonHeight, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_SELECTION_REMOVE), panelLeft + twoButtonWidth + rowGap, y, twoButtonWidth, buttonHeight, TRUE);
		y += buttonHeight + rowGap;
		MoveWindow(GetDlgItem(hwnd, IDC_SELECTION_UNDO), panelLeft, y, threeButtonWidth, buttonHeight, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_SELECTION_CLEAR), panelLeft + threeButtonWidth + rowGap, y, threeButtonWidth, buttonHeight, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_SELECTION_SELECT_ALL), panelLeft + (2 * (threeButtonWidth + rowGap)), y, threeButtonWidth, buttonHeight, TRUE);
		y += buttonHeight + rowGap;
		MoveWindow(GetDlgItem(hwnd, IDC_SELECTION_SHOW_MASK), panelLeft, y, panelWidth, buttonHeight, TRUE);
		y += buttonHeight + headerToRowGap;
		MoveWindow(GetDlgItem(hwnd, IDC_EDGE_SOFTNESS_STATIC), panelLeft, y, labelWidth, textRowHeight, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDC_EDGE_SOFTNESS_VALUE), panelLeft + panelWidth - valueWidth, y, valueWidth, textRowHeight, TRUE);
		y += textRowHeight + captionToSliderGap;
		MoveWindow(GetDlgItem(hwnd, IDC_EDGE_SOFTNESS_SLIDER), panelLeft, y, panelWidth, sliderHeight, TRUE);
		y += sliderHeight + rowGap;
		MoveWindow(GetDlgItem(hwnd, IDC_SELECTION_STATUS), panelLeft, y, panelWidth, ScaleForDpi(30), TRUE);

		const int actionTop = clientRect.bottom - ScaleForDpi(PREVIEW_MARGIN) - buttonHeight;
		MoveWindow(GetDlgItem(hwnd, IDCANCEL), panelLeft + panelWidth - (2 * twoButtonWidth) - rowGap, actionTop, twoButtonWidth, buttonHeight, TRUE);
		MoveWindow(GetDlgItem(hwnd, IDOK), panelLeft + panelWidth - twoButtonWidth, actionTop, twoButtonWidth, buttonHeight, TRUE);

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
		if (!PtInRect(&imageRect, POINT{ x, y }))
		{
			return false;
		}
		const RECT destRect = GetZoomedDestRect(hwnd);
		const float scaledX = static_cast<float>((x - destRect.left) * ScaledImageWidth) / RectWidth(destRect);
		const float scaledY = static_cast<float>((y - destRect.top) * ScaledImageHeight) / RectHeight(destRect);
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
		UpdatePresetButtons(hwnd);
		UpdateCommandLabels(hwnd);
		if (gPreviewWorker == nullptr)
		{
			InvalidatePreview(hwnd);
			return;
		}

		PreviewJob job;
		job.generation = ++gPreviewWorker->generation;
		job.ui = gUiState;
		if (gSelectionSession != nullptr && gSelectionSession->selectiveMode &&
			!gSelectionSession->selectionMask.empty())
		{
			job.selective = true;
			job.showMask = gSelectionSession->showMask;
			job.softness = gSelectionSession->softness;
			job.selectionMask = gSelectionSession->selectionMask;
		}
		{
			std::lock_guard<std::mutex> lock(gPreviewWorker->mutex);
			gPreviewWorker->pending = std::move(job);
		}
		gPreviewWorker->wake.notify_one();
		SetPreviewBusy(hwnd, true);
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
			const RECT destRect = GetZoomedDestRect(hwnd);
			const int clientX = destRect.left + static_cast<int>((scaledX + 0.5f) * RectWidth(destRect) / ScaledImageWidth);
			const int clientY = destRect.top + static_cast<int>((scaledY + 0.5f) * RectHeight(destRect) / ScaledImageHeight);
			if (clientX < imageRect.left || clientX >= imageRect.right || clientY < imageRect.top || clientY >= imageRect.bottom)
			{
				continue;
			}
			const COLORREF color = marker.mode == SelectionCombineMode::Add ? RGB(20, 220, 80) : RGB(240, 70, 70);
			const int markerRadius = ScaleForDpi(5);
			HPEN pen = CreatePen(PS_SOLID, ScaleForDpi(2), RGB(255, 255, 255));
			HBRUSH brush = CreateSolidBrush(color);
			HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, pen));
			HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(hdc, brush));
			Ellipse(hdc, clientX - markerRadius, clientY - markerRadius,
				clientX + markerRadius + 1, clientY + markerRadius + 1);
			SelectObject(hdc, oldBrush);
			SelectObject(hdc, oldPen);
			DeleteObject(brush);
			DeleteObject(pen);
		}
	}

	// Draws a hairline under each panel section header so the panel reads as
	// distinct groups; hidden sections (e.g. selection on Win32) draw nothing.
	void DrawPanelSectionSeparators(HDC hdc, HWND hwnd)
	{
		const int sectionIds[] = {
			IDC_ENHANCEMENT_SECTION, IDC_VIEW_SECTION, IDC_APPLY_TO_SECTION, IDC_SELECTION_SECTION
		};
		const RECT previewRect = GetPreviewRect(hwnd);
		RECT clientRect = {};
		GetClientRect(hwnd, &clientRect);
		const int panelLeft = previewRect.right + ScaleForDpi(PANEL_GAP);
		const int panelRight = clientRect.right - ScaleForDpi(PREVIEW_MARGIN);
		const COLORREF lineColor = gCurrentTheme == ThemeMode::Dark ? RGB(64, 64, 64) : RGB(228, 228, 228);
		const HBRUSH lineBrush = CreateSolidBrush(lineColor);
		for (const int controlId : sectionIds)
		{
			const HWND header = GetDlgItem(hwnd, controlId);
			if (header == nullptr || !IsWindowVisible(header))
			{
				continue;
			}
			RECT headerRect = {};
			GetWindowRect(header, &headerRect);
			MapWindowPoints(nullptr, hwnd, reinterpret_cast<LPPOINT>(&headerRect), 2);
			const int lineTop = headerRect.bottom + ScaleForDpi(4);
			const RECT lineRect = { panelLeft, lineTop, panelRight, lineTop + (std::max)(1, ScaleForDpi(1)) };
			FillRect(hdc, &lineRect, lineBrush);
		}
		DeleteObject(lineBrush);
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
		DrawPanelSectionSeparators(paintDc, hwnd);

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
			                          ScaledImageWidth, ScaledImageHeight, previewRect, GetZoomedDestRect(hwnd),
			                          gUiState.splitX, gUiState.compareHoldOriginal, darkMode,
			                          gOriginalDisplayCache, gProcessedDisplayCache);
			DrawSelectionMarkers(paintDc, hwnd);
		}

		if (gPreviewBusy)
		{
			DrawPreviewBusyLine(paintDc, previewRect);
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
		gUiState.zoomFactor = 1.0;
		gUiState.panX = 0;
		gUiState.panY = 0;
		gUiState.draggingPan = false;
		gUiState.pendingPick = false;

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

	void SaveWindowRectToSettings(HWND hwnd)
	{
		RECT windowRect = {};
		if (!GetWindowRect(hwnd, &windowRect))
		{
			return;
		}
		char valueBuffer[64] = {};
		sprintf_s(valueBuffer, "%d,%d,%d,%d", static_cast<int>(windowRect.left), static_cast<int>(windowRect.top),
			static_cast<int>(windowRect.right - windowRect.left), static_cast<int>(windowRect.bottom - windowRect.top));
		WritePrivateProfileStringA("AltaLux", "WindowRect", valueBuffer, SetupIniFile);
	}

	// Restores the last dialog size and position; keeps the template's
	// centered placement when nothing was saved yet or the saved spot is
	// unusable (e.g. the monitor it lived on is gone).
	void RestoreWindowRectFromSettings(HWND hwnd)
	{
		char valueBuffer[64] = {};
		GetPrivateProfileStringA("AltaLux", "WindowRect", "", valueBuffer, sizeof(valueBuffer), SetupIniFile);
		int left = 0;
		int top = 0;
		int width = 0;
		int height = 0;
		if (sscanf_s(valueBuffer, "%d,%d,%d,%d", &left, &top, &width, &height) != 4)
		{
			return;
		}

		width = (std::max)(width, ScaleForDpi(MIN_DIALOG_WIDTH));
		height = (std::max)(height, ScaleForDpi(MIN_DIALOG_HEIGHT));
		const RECT savedRect = { left, top, left + width, top + height };
		const HMONITOR monitor = MonitorFromRect(&savedRect, MONITOR_DEFAULTTONEAREST);
		MONITORINFO monitorInfo = {};
		monitorInfo.cbSize = sizeof(monitorInfo);
		if (!GetMonitorInfoA(monitor, &monitorInfo))
		{
			return;
		}
		const int workLeft = monitorInfo.rcWork.left;
		const int workTop = monitorInfo.rcWork.top;
		const int workRight = monitorInfo.rcWork.right;
		const int workBottom = monitorInfo.rcWork.bottom;
		width = (std::min)(width, workRight - workLeft);
		height = (std::min)(height, workBottom - workTop);
		left = (std::min)((std::max)(left, workLeft), workRight - width);
		top = (std::min)((std::max)(top, workTop), workBottom - height);
		SetWindowPos(hwnd, nullptr, left, top, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
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

		return abs(x - gUiState.splitX) <= ScaleForDpi(SPLIT_HIT_RADIUS);
	}

	LRESULT CALLBACK CompareHoldHookProc(int code, WPARAM wparam, LPARAM lparam)
	{
		if (code == HC_ACTION && wparam == PM_REMOVE && gHookDlg != nullptr)
		{
			MSG* msg = reinterpret_cast<MSG*>(lparam);
			// Space is the primary hold-to-compare key; Ctrl is the modifier
			// alternative, useful while dragging (pan/split) or when a control
			// has keyboard focus and would otherwise consume the space bar.
			const bool holdKey = msg->wParam == VK_SPACE || msg->wParam == VK_CONTROL;
			if (holdKey &&
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
		gDialogDpi = GetWindowDpiSafe(hwnd);
		SetUIDrawDpi(gDialogDpi);
		SetClassLongPtr(hwnd, GCL_STYLE, GetClassLongPtr(hwnd, GCL_STYLE) | CS_DBLCLKS);
		AdjustForDarkMode(hwnd);
		BOOL useDarkMode = IsDarkModeEnabled();
		DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDarkMode, sizeof(useDarkMode));
		ApplyDialogFont(hwnd);
		ApplyPanelTypography(hwnd);
		SetSliderRanges(hwnd);
		RestoreWindowRectFromSettings(hwnd);
		LayoutControlsV2(hwnd);
		if (gUiState.zoomToSelection)
		{
			SetActualSizeZoom(hwnd);
		}
		else
		{
			SyncZoomRadioButtons(hwnd);
		}
		CenterSplitInPreview(hwnd);
		UpdateSliders(hwnd);
		StartPreviewWorker(hwnd);
		RefreshPreview(hwnd);
		UpdateSelectionControls(hwnd);
		gHookDlg = hwnd;
		gKeyboardHook = SetWindowsHookExW(WH_GETMESSAGE, CompareHoldHookProc, nullptr, GetCurrentThreadId());
		return TRUE;
	}

	case WM_DESTROY:
		SaveWindowRectToSettings(hwnd);
		KillTimer(hwnd, PREVIEW_REFRESH_TIMER_ID);
		KillTimer(hwnd, PREVIEW_BUSY_TIMER_ID);
		StopPreviewWorker();
		DiscardPendingPreviewResults(hwnd);
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
		if (gHelperFont != nullptr)
		{
			DeleteObject(gHelperFont);
			gHelperFont = nullptr;
		}
		if (gDialogFont != nullptr)
		{
			DeleteObject(gDialogFont);
			gDialogFont = nullptr;
		}
		BufferedPaintUnInit();
		return TRUE;

	case WM_COMMAND:
		if (HIWORD(wparam) == STN_DBLCLK)
		{
			bool reset = true;
			switch (LOWORD(wparam))
			{
			case IDC_STRENGTH_STATIC: gUiState.strength = Constants::DefaultStrength; break;
			case IDC_DETAIL_STATIC: gUiState.detail = Constants::DefaultDetail; break;
			case IDC_NATURALLOOK_STATIC: gUiState.naturalLook = Constants::DefaultNatural; break;
			case IDC_CHROMA_STATIC: gUiState.chromaProtection = Constants::DefaultChromaProtection; break;
			case IDC_EDGE_SOFTNESS_STATIC:
				if (gSelectionSession != nullptr) gSelectionSession->softness = 3;
				else reset = false;
				break;
			default: reset = false; break;
			}
			if (reset)
			{
				UpdateSliders(hwnd);
				if (gSelectionSession != nullptr) UpdateSelectionControls(hwnd);
				RefreshPreview(hwnd);
				return TRUE;
			}
		}
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
			ResetZoomTo(hwnd, 1.0);
			return TRUE;

		case IDC_VIEW_ACTUAL:
			SetActualSizeZoom(hwnd);
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
		UpdateSliderValues(hwnd);

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
		if (wparam == PREVIEW_BUSY_TIMER_ID)
		{
			gPreviewBusyPosition += 0.02f;
			if (gPreviewBusyPosition > 1.0f)
			{
				gPreviewBusyPosition = 0.0f;
			}
			RECT stripRect = {};
			GetPreviewBusyStripRect(hwnd, stripRect);
			InvalidateRect(hwnd, &stripRect, FALSE);
			return TRUE;
		}
		break;

	case WM_ALTALUX_PREVIEW_READY:
	{
		std::unique_ptr<PreviewResult> result(reinterpret_cast<PreviewResult*>(lparam));
		if (gPreviewWorker == nullptr || result == nullptr ||
			result->generation != gPreviewWorker->generation.load())
		{
			return TRUE;
		}

		auto scaledProc = ScaledProcImagePtr.lock();
		if (scaledProc != nullptr && !result->processed.empty())
		{
			memcpy(scaledProc->data(), result->processed.data(),
				GetImageByteCount(ScaledImageWidth, ScaledImageHeight));
		}
		if (gSelectionSession != nullptr)
		{
			gSelectionSession->previewOriginalDisplay = std::move(result->originalDisplay);
			gSelectionSession->previewProcessedDisplay = std::move(result->processedDisplay);
		}
		ClearPreviewDisplayCache(gOriginalDisplayCache);
		ClearPreviewDisplayCache(gProcessedDisplayCache);

		bool moreWorkQueued = false;
		{
			std::lock_guard<std::mutex> lock(gPreviewWorker->mutex);
			moreWorkQueued = gPreviewWorker->pending.has_value();
		}
		SetPreviewBusy(hwnd, moreWorkQueued);
		InvalidatePreview(hwnd);
		UpdateWindow(hwnd);
		return TRUE;
	}

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
		// In selective mode a still press inside the image picks an object on
		// release, while dragging the same button pans; treat every press as a
		// potential pan and decide on release whether the gesture stayed still.
		const bool pickReady = gSelectionSession != nullptr && gSelectionSession->selectiveMode &&
			gSelectionSession->ready && !gSelectionSession->busy;
		float sourceX = 0.0f;
		float sourceY = 0.0f;
		const bool pickPending = pickReady && MapClientPointToSource(hwnd, x, y, sourceX, sourceY);
		if (IsImagePannable(hwnd))
		{
			gUiState.draggingPan = true;
			gUiState.panLastX = x;
			gUiState.panLastY = y;
			gUiState.panDragOriginX = x;
			gUiState.panDragOriginY = y;
			gUiState.pendingPick = pickPending;
			gUiState.pendingPickX = sourceX;
			gUiState.pendingPickY = sourceY;
			SetCapture(hwnd);
			return TRUE;
		}
		if (pickPending)
		{
			StartPointSegmentation(hwnd, sourceX, sourceY);
			return TRUE;
		}
		break;
	}

	case WM_MBUTTONDOWN:
	{
		// Middle button always pans, even in selective mode where a still
		// left-click picks an object.
		const int x = GET_X_LPARAM(lparam);
		const int y = GET_Y_LPARAM(lparam);
		if (IsImagePannable(hwnd))
		{
			gUiState.draggingPan = true;
			gUiState.panLastX = x;
			gUiState.panLastY = y;
			gUiState.pendingPick = false;
			SetCapture(hwnd);
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
		if (gUiState.draggingPan)
		{
			const int x = GET_X_LPARAM(lparam);
			const int y = GET_Y_LPARAM(lparam);
			if (gUiState.pendingPick &&
				(abs(x - gUiState.panDragOriginX) > ScaleForDpi(PICK_DRAG_SLOP) ||
					abs(y - gUiState.panDragOriginY) > ScaleForDpi(PICK_DRAG_SLOP)))
			{
				gUiState.pendingPick = false;
			}
			gUiState.panX += x - gUiState.panLastX;
			gUiState.panY += y - gUiState.panLastY;
			gUiState.panLastX = x;
			gUiState.panLastY = y;
			ClampPanOffsets(ScaledImageWidth, ScaledImageHeight, GetPreviewRect(hwnd),
				gUiState.zoomFactor, gUiState.panX, gUiState.panY);
			ClampSplitToPreview(hwnd);
			InvalidatePreview(hwnd);
			return TRUE;
		}
		break;

	case WM_MOUSEWHEEL:
	{
		const POINT cursor = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
		POINT client = cursor;
		ScreenToClient(hwnd, &client);
		const RECT previewRect = GetPreviewRect(hwnd);
		if (PtInRect(&previewRect, client) && ScaledImageWidth > 0 && ScaledImageHeight > 0)
		{
			const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
			const double step = delta > 0 ? 1.25 : 0.8;
			ZoomAtClientPoint(hwnd, client.x, client.y, gUiState.zoomFactor * step);
			return TRUE;
		}
		break;
	}

	case WM_LBUTTONUP:
	case WM_MBUTTONUP:
		if (gUiState.draggingSplit)
		{
			gUiState.draggingSplit = false;
			ReleaseCapture();
			InvalidatePreview(hwnd);
			return TRUE;
		}
		if (gUiState.draggingPan)
		{
			gUiState.draggingPan = false;
			ReleaseCapture();
			const bool pick = msg == WM_LBUTTONUP && gUiState.pendingPick;
			gUiState.pendingPick = false;
			InvalidatePreview(hwnd);
			if (pick)
			{
				StartPointSegmentation(hwnd, gUiState.pendingPickX, gUiState.pendingPickY);
			}
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
		gUiState.draggingPan = false;
		gUiState.pendingPick = false;
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
		if (gUiState.draggingPan)
		{
			SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
			return TRUE;
		}
		const RECT activeImageRect = GetActiveImageRect(hwnd);
		if (gSelectionSession != nullptr && gSelectionSession->selectiveMode &&
			gSelectionSession->ready && PtInRect(&activeImageRect, cursor))
		{
			SetCursor(LoadCursor(nullptr, IDC_CROSS));
			return TRUE;
		}
		if (IsImagePannable(hwnd) && PtInRect(&activeImageRect, cursor))
		{
			SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
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

	case WM_DPICHANGED:
	{
		gDialogDpi = HIWORD(wparam);
		SetUIDrawDpi(gDialogDpi);
		const RECT* suggestedRect = reinterpret_cast<const RECT*>(lparam);
		SetWindowPos(hwnd, nullptr, suggestedRect->left, suggestedRect->top,
			suggestedRect->right - suggestedRect->left, suggestedRect->bottom - suggestedRect->top,
			SWP_NOZORDER | SWP_NOACTIVATE);
		ApplyDialogFont(hwnd);
		ApplyPanelTypography(hwnd);
		LayoutControlsV2(hwnd);
		InvalidateRect(hwnd, nullptr, FALSE);
		return TRUE;
	}

	case WM_GETMINMAXINFO:
	{
		MINMAXINFO* minMaxInfo = reinterpret_cast<MINMAXINFO*>(lparam);
		minMaxInfo->ptMinTrackSize.x = ScaleForDpi(MIN_DIALOG_WIDTH);
		minMaxInfo->ptMinTrackSize.y = ScaleForDpi(MIN_DIALOG_HEIGHT);
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
		TryEnablePerMonitorDpiAwareness();

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
		gUiState.zoomFactor = 1.0;
		gUiState.panX = 0;
		gUiState.panY = 0;
		gUiState.draggingPan = false;
		gUiState.pendingPick = false;
	}

	auto processFullImage = [&]() -> bool
	{
		const bool applySelection = gSelectionSession != nullptr && gSelectionSession->selectiveMode &&
			gSelectionSession->ready && HasSelectedPixels(gSelectionSession->selectionMask);
		if (applySelection)
		{
			std::vector<unsigned char> processedImage(srcImage->begin(),
				srcImage->begin() + GetImageByteCount(ImageWidth, ImageHeight));
			if (!ProcessMultiscaleImage(srcImage->data(), processedImage.data(), ImageWidth, ImageHeight, ImageBitDepth, gUiState))
			{
				return false;
			}
			std::vector<unsigned char> alphaMask;
			if (!BuildFeatheredMask(gSelectionSession->selectionMask.data(), ImageWidth, ImageHeight,
				gSelectionSession->softness, alphaMask) ||
				!CompositeWithMask(srcImage->data(), processedImage.data(), srcImage->data(), alphaMask.data(),
					ImageWidth, ImageHeight, ImageBitDepth))
			{
				return false;
			}
		}
		else if (!ProcessMultiscaleImage(srcImage->data(), srcImage->data(), ImageWidth, ImageHeight, ImageBitDepth, gUiState))
		{
			return false;
		}
		return true;
	};

	const bool interactiveDialog = (param1 == -1) || (param2 == -1);
	bool processOk = false;
	if (interactiveDialog)
	{
		// The user just confirmed the dialog; give them feedback while the
		// full-resolution pass runs instead of freezing IrfanView silently.
		ApplyProgressPopup progress(hwnd);
		if (progress.hwnd() != nullptr)
		{
			std::thread applyWorker([&]()
			{
				processOk = processFullImage();
				progress.NotifyDone(processOk);
			});
			progress.PumpUntilDone();
			applyWorker.join();
		}
		else
		{
			processOk = processFullImage();
		}
	}
	else
	{
		processOk = processFullImage();
	}

	if (!processOk)
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
