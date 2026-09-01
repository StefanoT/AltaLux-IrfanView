#pragma once

#include <cstdint>

constexpr std::uint32_t ALTALUX_SEGMENTATION_ABI_VERSION = 1;

enum class SegmentationStatus : std::int32_t
{
	Ok = 0,
	InvalidArgument = 1,
	UnsupportedAbi = 2,
	NotReady = 3,
	Cancelled = 4,
	ModelError = 5,
	RuntimeError = 6
};

enum class SegmentationPixelFormat : std::uint32_t
{
	Bgr24 = 1,
	Bgra32 = 2
};

struct SegmentationImageView
{
	const std::uint8_t* pixels;
	std::uint32_t width;
	std::uint32_t height;
	std::int32_t stride;
	SegmentationPixelFormat format;
};

struct SegmentationPoint
{
	float x;
	float y;
};

struct SegmentationMaskView
{
	std::uint8_t* pixels;
	std::uint32_t width;
	std::uint32_t height;
	std::int32_t stride;
};

class ISegmentationEngine
{
public:
	virtual SegmentationStatus PrepareImage(const SegmentationImageView& image) noexcept = 0;
	virtual SegmentationStatus SegmentPoint(const SegmentationPoint& point, SegmentationMaskView& mask) noexcept = 0;
	virtual void Cancel() noexcept = 0;
	virtual const wchar_t* GetLastError() const noexcept = 0;
	virtual void Release() noexcept = 0;

protected:
	virtual ~ISegmentationEngine() = default;
};

using AltaLuxCreateSegmentationEngineFn = SegmentationStatus (*)(
	std::uint32_t abiVersion, ISegmentationEngine** engine);

#define ALTALUX_SEGMENTATION_FACTORY_NAME "AltaLuxCreateSegmentationEngine"
