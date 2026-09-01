#include "SelectionCore.h"

#include <algorithm>
#include <cmath>
#include <cstring>

SelectionHistory::SelectionHistory(std::size_t maxEdits, std::size_t maxBytes)
	: maxEdits_(maxEdits), maxBytes_(maxBytes)
{
}

bool SelectionHistory::Apply(std::vector<std::uint8_t>& selection, const std::uint8_t* operationMask,
	std::size_t pixelCount, SelectionCombineMode mode)
{
	if (operationMask == nullptr || selection.size() != pixelCount)
	{
		return false;
	}

	SelectionEdit edit;
	const std::uint8_t newValue = mode == SelectionCombineMode::Add ? 255 : 0;
	std::size_t index = 0;
	while (index < pixelCount)
	{
		if (operationMask[index] == 0 || selection[index] == newValue)
		{
			++index;
			continue;
		}

		const std::size_t start = index;
		const std::uint8_t previousValue = selection[index];
		while (index < pixelCount && operationMask[index] != 0 && selection[index] == previousValue)
		{
			selection[index] = newValue;
			++index;
		}
		edit.spans.push_back({ start, index - start, previousValue });
	}

	if (edit.spans.empty())
	{
		return false;
	}
	edit.storageBytes = edit.spans.size() * sizeof(SelectionChangedSpan);
	Push(std::move(edit));
	return true;
}

bool SelectionHistory::Fill(std::vector<std::uint8_t>& selection, std::uint8_t value)
{
	SelectionEdit edit;
	std::size_t index = 0;
	while (index < selection.size())
	{
		if (selection[index] == value)
		{
			++index;
			continue;
		}
		const std::size_t start = index;
		const std::uint8_t previousValue = selection[index];
		while (index < selection.size() && selection[index] == previousValue && selection[index] != value)
		{
			selection[index] = value;
			++index;
		}
		edit.spans.push_back({ start, index - start, previousValue });
	}
	if (edit.spans.empty())
	{
		return false;
	}
	edit.storageBytes = edit.spans.size() * sizeof(SelectionChangedSpan);
	Push(std::move(edit));
	return true;
}

bool SelectionHistory::Undo(std::vector<std::uint8_t>& selection)
{
	if (edits_.empty())
	{
		return false;
	}
	SelectionEdit edit = std::move(edits_.back());
	edits_.pop_back();
	storageBytes_ -= edit.storageBytes;
	for (const SelectionChangedSpan& span : edit.spans)
	{
		if (span.start + span.length <= selection.size())
		{
			std::fill(selection.begin() + span.start, selection.begin() + span.start + span.length,
				span.previousValue);
		}
	}
	return true;
}

void SelectionHistory::Reset()
{
	edits_.clear();
	storageBytes_ = 0;
}

bool SelectionHistory::CanUndo() const
{
	return !edits_.empty();
}

void SelectionHistory::Push(SelectionEdit&& edit)
{
	storageBytes_ += edit.storageBytes;
	edits_.push_back(std::move(edit));
	while (edits_.size() > maxEdits_ || storageBytes_ > maxBytes_)
	{
		storageBytes_ -= edits_.front().storageBytes;
		edits_.pop_front();
	}
}

bool HasSelectedPixels(const std::vector<std::uint8_t>& selection)
{
	return std::find_if(selection.begin(), selection.end(), [](std::uint8_t value) { return value != 0; }) != selection.end();
}

bool MapPreviewPointToImage(const RECT& imageRect, int x, int y, int imageWidth, int imageHeight,
	float& imageX, float& imageY)
{
	const int rectWidth = imageRect.right - imageRect.left;
	const int rectHeight = imageRect.bottom - imageRect.top;
	if (rectWidth <= 0 || rectHeight <= 0 || imageWidth <= 0 || imageHeight <= 0 ||
		x < imageRect.left || x >= imageRect.right || y < imageRect.top || y >= imageRect.bottom)
	{
		return false;
	}
	imageX = (static_cast<float>(x - imageRect.left) + 0.5f) * imageWidth / rectWidth - 0.5f;
	imageY = (static_cast<float>(y - imageRect.top) + 0.5f) * imageHeight / rectHeight - 0.5f;
	imageX = (std::max)(0.0f, (std::min)(imageX, static_cast<float>(imageWidth - 1)));
	imageY = (std::max)(0.0f, (std::min)(imageY, static_cast<float>(imageHeight - 1)));
	return true;
}

void ResizeMaskNearest(const std::uint8_t* source, int sourceWidth, int sourceHeight,
	std::uint8_t* target, int targetWidth, int targetHeight)
{
	if (source == nullptr || target == nullptr || sourceWidth <= 0 || sourceHeight <= 0 ||
		targetWidth <= 0 || targetHeight <= 0)
	{
		return;
	}
	for (int y = 0; y < targetHeight; ++y)
	{
		const int sourceY = (std::min)(sourceHeight - 1, y * sourceHeight / targetHeight);
		for (int x = 0; x < targetWidth; ++x)
		{
			const int sourceX = (std::min)(sourceWidth - 1, x * sourceWidth / targetWidth);
			target[y * targetWidth + x] = source[sourceY * sourceWidth + sourceX];
		}
	}
}

bool BuildFeatheredMask(const std::uint8_t* binaryMask, int width, int height, int softness,
	std::vector<std::uint8_t>& alphaMask)
{
	if (binaryMask == nullptr || width <= 0 || height <= 0 || softness < 0 || softness > 20)
	{
		return false;
	}
	const std::size_t pixelCount = static_cast<std::size_t>(width) * height;
	std::vector<std::uint8_t> dilated(pixelCount, 0);
	for (int y = 0; y < height; ++y)
	{
		for (int x = 0; x < width; ++x)
		{
			std::uint8_t selected = 0;
			for (int dy = -1; dy <= 1 && selected == 0; ++dy)
			{
				const int sy = y + dy;
				if (sy < 0 || sy >= height) continue;
				for (int dx = -1; dx <= 1; ++dx)
				{
					const int sx = x + dx;
					if (sx >= 0 && sx < width && binaryMask[sy * width + sx] != 0)
					{
						selected = 255;
						break;
					}
				}
			}
			dilated[y * width + x] = selected;
		}
	}
	if (softness == 0)
	{
		alphaMask = std::move(dilated);
		return true;
	}

	const float sigma = (std::max)(0.5f, softness * 0.5f);
	std::vector<float> kernel(static_cast<std::size_t>(softness * 2 + 1));
	float kernelSum = 0.0f;
	for (int i = -softness; i <= softness; ++i)
	{
		const float value = std::exp(-(i * i) / (2.0f * sigma * sigma));
		kernel[static_cast<std::size_t>(i + softness)] = value;
		kernelSum += value;
	}
	for (float& value : kernel) value /= kernelSum;

	std::vector<float> horizontal(pixelCount, 0.0f);
	for (int y = 0; y < height; ++y)
	{
		for (int x = 0; x < width; ++x)
		{
			float value = 0.0f;
			for (int k = -softness; k <= softness; ++k)
			{
				const int sx = (std::max)(0, (std::min)(width - 1, x + k));
				value += dilated[y * width + sx] * kernel[static_cast<std::size_t>(k + softness)];
			}
			horizontal[y * width + x] = value;
		}
	}
	alphaMask.assign(pixelCount, 0);
	for (int y = 0; y < height; ++y)
	{
		for (int x = 0; x < width; ++x)
		{
			float value = 0.0f;
			for (int k = -softness; k <= softness; ++k)
			{
				const int sy = (std::max)(0, (std::min)(height - 1, y + k));
				value += horizontal[sy * width + x] * kernel[static_cast<std::size_t>(k + softness)];
			}
			alphaMask[y * width + x] = static_cast<std::uint8_t>((std::max)(0.0f, (std::min)(255.0f, value)) + 0.5f);
		}
	}
	return true;
}

bool CompositeWithMask(const std::uint8_t* original, const std::uint8_t* processed, std::uint8_t* target,
	const std::uint8_t* alphaMask, int width, int height, int pixelStride)
{
	if (original == nullptr || processed == nullptr || target == nullptr || alphaMask == nullptr ||
		width <= 0 || height <= 0 || (pixelStride != 3 && pixelStride != 4))
	{
		return false;
	}
	const int pixelCount = width * height;
	for (int pixel = 0; pixel < pixelCount; ++pixel)
	{
		const unsigned int alpha = alphaMask[pixel];
		const unsigned int inverse = 255U - alpha;
		const int base = pixel * pixelStride;
		for (int channel = 0; channel < 3; ++channel)
		{
			target[base + channel] = static_cast<std::uint8_t>(
				(original[base + channel] * inverse + processed[base + channel] * alpha + 127U) / 255U);
		}
		if (pixelStride == 4)
		{
			target[base + 3] = original[base + 3];
		}
	}
	return true;
}
