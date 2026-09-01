#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

enum class SelectionCombineMode
{
	Add,
	Remove
};

struct SelectionChangedSpan
{
	std::size_t start;
	std::size_t length;
	std::uint8_t previousValue;
};

struct SelectionEdit
{
	std::vector<SelectionChangedSpan> spans;
	std::size_t storageBytes = 0;
};

class SelectionHistory
{
public:
	SelectionHistory(std::size_t maxEdits = 64, std::size_t maxBytes = 128U * 1024U * 1024U);

	bool Apply(std::vector<std::uint8_t>& selection, const std::uint8_t* operationMask,
		std::size_t pixelCount, SelectionCombineMode mode);
	bool Fill(std::vector<std::uint8_t>& selection, std::uint8_t value);
	bool Undo(std::vector<std::uint8_t>& selection);
	void Reset();
	bool CanUndo() const;

private:
	void Push(SelectionEdit&& edit);

	std::deque<SelectionEdit> edits_;
	std::size_t storageBytes_ = 0;
	std::size_t maxEdits_;
	std::size_t maxBytes_;
};

bool HasSelectedPixels(const std::vector<std::uint8_t>& selection);
bool MapPreviewPointToImage(const RECT& imageRect, int x, int y, int imageWidth, int imageHeight,
	float& imageX, float& imageY);
void ResizeMaskNearest(const std::uint8_t* source, int sourceWidth, int sourceHeight,
	std::uint8_t* target, int targetWidth, int targetHeight);
bool BuildFeatheredMask(const std::uint8_t* binaryMask, int width, int height, int softness,
	std::vector<std::uint8_t>& alphaMask);
bool CompositeWithMask(const std::uint8_t* original, const std::uint8_t* processed, std::uint8_t* target,
	const std::uint8_t* alphaMask, int width, int height, int pixelStride);
