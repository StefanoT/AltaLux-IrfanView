#pragma once

#include <Windows.h>

#include <string>

#include "SegmentationApi.h"

class SegmentationModule
{
public:
	SegmentationModule() = default;
	~SegmentationModule();

	SegmentationModule(const SegmentationModule&) = delete;
	SegmentationModule& operator=(const SegmentationModule&) = delete;

	bool Load(HINSTANCE hostModule) noexcept;
	void Unload() noexcept;
	bool IsLoaded() const noexcept;
	ISegmentationEngine* GetEngine() const noexcept;
	const std::wstring& GetLastError() const noexcept;

private:
	HMODULE module_ = nullptr;
	ISegmentationEngine* engine_ = nullptr;
	std::wstring lastError_;
};
