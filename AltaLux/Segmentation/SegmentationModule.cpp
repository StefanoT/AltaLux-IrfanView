#include "SegmentationModule.h"

#include <cwchar>

namespace
{
	std::wstring FormatWindowsError(DWORD error)
	{
		wchar_t* message = nullptr;
		const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
			FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0, reinterpret_cast<wchar_t*>(&message), 0, nullptr);
		std::wstring result = length != 0 && message != nullptr ? std::wstring(message, length) : L"Unknown Windows error";
		if (message != nullptr) LocalFree(message);
		return result;
	}
}

SegmentationModule::~SegmentationModule()
{
	Unload();
}

bool SegmentationModule::Load(HINSTANCE hostModule) noexcept
{
	Unload();
#if !defined(_WIN64)
	(void)hostModule;
	lastError_ = L"Object selection is available only in the x64 AltaLux plug-in.";
	return false;
#else
	wchar_t hostPath[32768] = {};
	const DWORD pathLength = GetModuleFileNameW(hostModule, hostPath, static_cast<DWORD>(_countof(hostPath)));
	if (pathLength == 0 || pathLength >= _countof(hostPath))
	{
		lastError_ = L"Unable to locate AltaLux.dll.";
		return false;
	}
	wchar_t* separator = wcsrchr(hostPath, L'\\');
	if (separator == nullptr)
	{
		lastError_ = L"Unable to locate the IrfanView plug-in directory.";
		return false;
	}
	*(separator + 1) = L'\0';
	if (wcscat_s(hostPath, L"AltaLuxSegmentation.dll") != 0)
	{
		lastError_ = L"The segmentation module path is too long.";
		return false;
	}

	module_ = LoadLibraryExW(hostPath, nullptr,
		LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
	if (module_ == nullptr)
	{
		lastError_ = std::wstring(L"AltaLuxSegmentation.dll could not be loaded from the AltaLux plug-in directory.\n\n") +
			FormatWindowsError(::GetLastError());
		return false;
	}

	auto factory = reinterpret_cast<AltaLuxCreateSegmentationEngineFn>(
		GetProcAddress(module_, ALTALUX_SEGMENTATION_FACTORY_NAME));
	if (factory == nullptr)
	{
		lastError_ = L"AltaLuxSegmentation.dll does not export the required versioned factory.";
		Unload();
		return false;
	}

	const SegmentationStatus status = factory(ALTALUX_SEGMENTATION_ABI_VERSION, &engine_);
	if (status != SegmentationStatus::Ok || engine_ == nullptr)
	{
		lastError_ = status == SegmentationStatus::UnsupportedAbi ?
			L"AltaLuxSegmentation.dll is incompatible with this version of AltaLux." :
			L"AltaLuxSegmentation.dll could not initialize. Verify that ONNX Runtime, both AltaLux MobileSAM models, and the model manifest are installed beside AltaLux.dll.";
		Unload();
		return false;
	}
	lastError_.clear();
	return true;
#endif
}

void SegmentationModule::Unload() noexcept
{
	if (engine_ != nullptr)
	{
		engine_->Release();
		engine_ = nullptr;
	}
	if (module_ != nullptr)
	{
		FreeLibrary(module_);
		module_ = nullptr;
	}
}

bool SegmentationModule::IsLoaded() const noexcept
{
	return engine_ != nullptr;
}

ISegmentationEngine* SegmentationModule::GetEngine() const noexcept
{
	return engine_;
}

const std::wstring& SegmentationModule::GetLastError() const noexcept
{
	return lastError_;
}
