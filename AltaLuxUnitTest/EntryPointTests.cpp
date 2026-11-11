// First coverage of the plugin layer itself: builds real HGLOBAL DIBs the way
// IrfanView hands them to StartEffects2 and drives the export in direct mode
// (param1/param2 >= 0, no dialog). Interactive mode needs a message loop and
// the dialog resource, so it stays a manual test.

#include "stdafx.h"

#include <windows.h>

#include "..\AltaLux\AltaLux.h"
#include "AllocationHook.h"

#include <climits>
#include <cstring>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace AltaLuxUnitTest
{
	namespace
	{
		// IrfanView's selection rectangle convention (see AltaLux.h):
		// left/top are the offset, right/bottom are the WIDTH/HEIGHT.
		RECT MakeSelectionRect(int left, int top, int width, int height)
		{
			RECT rect = { left, top, width, height };
			return rect;
		}

		size_t DibStrideBytes(int width, int bitCount)
		{
			return (static_cast<size_t>(width) * bitCount + 31) / 32 * 4;
		}

		// Smooth dark bands over a soft gradient with per-channel bias: every
		// region of the image has shadow content for the filter to lift, so
		// processed pixels visibly change. Expressed in visual coordinates
		// (y = 0 is the top row as displayed).
		unsigned char PatternByte(int x, int visualY, int channel)
		{
			const int band = ((x / 5 + visualY / 3) % 16) * 15;
			const int channelBias = channel * 20;
			return static_cast<unsigned char>((band + channelBias + ((x + visualY) % 7)) & 0xFF);
		}

		// Allocates a movable HGLOBAL holding a BITMAPINFOHEADER followed by
		// 4-byte-padded rows, the layout ScopedBitmapHeader expects.
		HGLOBAL BuildTestDib(int width, int height, int bitCount, bool topDown)
		{
			const size_t stride = DibStrideBytes(width, bitCount);
			const size_t size = sizeof(BITMAPINFOHEADER) + stride * height;
			HGLOBAL hDib = GlobalAlloc(GMEM_MOVEABLE, size);
			if (hDib == nullptr)
			{
				return nullptr;
			}
			auto* header = static_cast<BITMAPINFOHEADER*>(GlobalLock(hDib));
			if (header == nullptr)
			{
				GlobalFree(hDib);
				return nullptr;
			}
			memset(header, 0, sizeof(BITMAPINFOHEADER));
			header->biSize = sizeof(BITMAPINFOHEADER);
			header->biWidth = width;
			header->biHeight = topDown ? -height : height;
			header->biPlanes = 1;
			header->biBitCount = static_cast<WORD>(bitCount);

			auto* bits = reinterpret_cast<unsigned char*>(header + 1);
			const int bytesPerPixel = bitCount / 8;
			for (int memoryRow = 0; memoryRow < height; ++memoryRow)
			{
				const int visualRow = topDown ? memoryRow : height - 1 - memoryRow;
				unsigned char* row = bits + memoryRow * stride;
				for (int x = 0; x < width; ++x)
				{
					for (int channel = 0; channel < bytesPerPixel; ++channel)
					{
						row[x * bytesPerPixel + channel] = PatternByte(x, visualRow, channel);
					}
				}
			}
			GlobalUnlock(hDib);
			return hDib;
		}

		HGLOBAL BuildOversizedDib()
		{
			// 60000x60000x24 claims 3.6 gigapixels, past the kernels' int
			// pixel index range; only a tiny buffer actually sits behind the
			// header, so any copy attempt would read far out of bounds.
			HGLOBAL hDib = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + 16);
			if (hDib == nullptr)
			{
				return nullptr;
			}
			auto* header = static_cast<BITMAPINFOHEADER*>(GlobalLock(hDib));
			if (header == nullptr)
			{
				GlobalFree(hDib);
				return nullptr;
			}
			memset(header, 0, sizeof(BITMAPINFOHEADER));
			header->biSize = sizeof(BITMAPINFOHEADER);
			header->biWidth = 60000;
			header->biHeight = 60000;
			header->biPlanes = 1;
			header->biBitCount = 24;
			GlobalUnlock(hDib);
			return hDib;
		}

		std::vector<unsigned char> SnapshotDib(HGLOBAL hDib)
		{
			const size_t size = GlobalSize(hDib);
			std::vector<unsigned char> snapshot(size);
			void* bits = GlobalLock(hDib);
			memcpy(snapshot.data(), bits, size);
			GlobalUnlock(hDib);
			return snapshot;
		}

		void RestoreDib(HGLOBAL hDib, const std::vector<unsigned char>& snapshot)
		{
			void* bits = GlobalLock(hDib);
			memcpy(bits, snapshot.data(), snapshot.size());
			GlobalUnlock(hDib);
		}

		struct DibGeometry
		{
			int width = 0;
			int height = 0;
			int bytesPerPixel = 0;
			bool topDown = false;
		};

		DibGeometry GetDibGeometry(HGLOBAL hDib)
		{
			auto* header = static_cast<BITMAPINFOHEADER*>(GlobalLock(hDib));
			DibGeometry geometry;
			geometry.width = header->biWidth;
			geometry.height = header->biHeight < 0 ? -header->biHeight : header->biHeight;
			geometry.bytesPerPixel = header->biBitCount / 8;
			geometry.topDown = header->biHeight < 0;
			GlobalUnlock(hDib);
			return geometry;
		}

		// Compares a snapshot against the DIB: every pixel outside the visual
		// selection rect (padding bytes included) must be byte-identical.
		void AssertOutsideRectUnchanged(HGLOBAL hDib, const std::vector<unsigned char>& before,
			const RECT& selection)
		{
			const DibGeometry geometry = GetDibGeometry(hDib);
			const std::vector<unsigned char> after = SnapshotDib(hDib);
			const size_t stride = DibStrideBytes(geometry.width, geometry.bytesPerPixel * 8);
			const unsigned char* oldBits = before.data() + sizeof(BITMAPINFOHEADER);
			const unsigned char* newBits = after.data() + sizeof(BITMAPINFOHEADER);
			for (int memoryRow = 0; memoryRow < geometry.height; ++memoryRow)
			{
				const int visualRow = geometry.topDown ? memoryRow : geometry.height - 1 - memoryRow;
				for (int x = 0; x < geometry.width; ++x)
				{
					if (x >= selection.left && x < selection.right &&
						visualRow >= selection.top && visualRow < selection.bottom)
					{
						continue;
					}
					const size_t offset = memoryRow * stride + x * geometry.bytesPerPixel;
					Assert::IsTrue(memcmp(oldBits + offset, newBits + offset, geometry.bytesPerPixel) == 0,
						(L"a pixel outside the selection changed: x=" + std::to_wstring(x) +
							L" visualRow=" + std::to_wstring(visualRow)).c_str());
				}
			}
		}

		// Allocates a quadrant image with clearly different dark textures per
		// visual quadrant, so a wrong selection region is unmistakable.
		HGLOBAL BuildQuadrantDib(int width, int height, int bitCount, bool topDown)
		{
			HGLOBAL hDib = BuildTestDib(width, height, bitCount, topDown);
			if (hDib == nullptr)
			{
				return nullptr;
			}
			auto* header = static_cast<BITMAPINFOHEADER*>(GlobalLock(hDib));
			const size_t stride = DibStrideBytes(width, bitCount);
			auto* bits = reinterpret_cast<unsigned char*>(header + 1);
			const int bytesPerPixel = bitCount / 8;
			for (int memoryRow = 0; memoryRow < height; ++memoryRow)
			{
				const int visualRow = topDown ? memoryRow : height - 1 - memoryRow;
				unsigned char* row = bits + memoryRow * stride;
				for (int x = 0; x < width; ++x)
				{
					const int quadrant = (x >= width / 2 ? 1 : 0) + (visualRow >= height / 2 ? 2 : 0);
					const unsigned char base = static_cast<unsigned char>(40 + quadrant * 45);
					for (int channel = 0; channel < bytesPerPixel; ++channel)
					{
						row[x * bytesPerPixel + channel] =
							static_cast<unsigned char>(base + ((x * 7 + visualRow * 13 + channel * 29) % 40));
					}
				}
			}
			GlobalUnlock(hDib);
			return hDib;
		}

		bool PixelChanged(HGLOBAL hDib, const std::vector<unsigned char>& before, int x, int visualRow)
		{
			const DibGeometry geometry = GetDibGeometry(hDib);
			const std::vector<unsigned char> after = SnapshotDib(hDib);
			const size_t stride = DibStrideBytes(geometry.width, geometry.bytesPerPixel * 8);
			const int memoryRow = geometry.topDown ? visualRow : geometry.height - 1 - visualRow;
			const size_t offset = sizeof(BITMAPINFOHEADER) + memoryRow * stride + x * geometry.bytesPerPixel;
			return memcmp(before.data() + offset, after.data() + offset, geometry.bytesPerPixel) != 0;
		}

		bool RunDirect(HGLOBAL hDib, RECT rect)
		{
			return StartEffects2(hDib, nullptr, 0, rect, 50, 50, nullptr, nullptr, 0);
		}
	}

	TEST_CLASS(EntryPointTests)
	{
	public:
		TEST_METHOD_CLEANUP(DisarmAllocationHook)
		{
			AltaLuxTest::DisarmAllocationFailure();
		}

		TEST_METHOD(DirectModeProcessesFullImageBothDepthsAndOrientations)
		{
			const int sizes[][2] = { { 64, 48 }, { 65, 49 } };
			for (int bitCount = 24; bitCount <= 32; bitCount += 8)
			{
				for (bool topDown : { false, true })
				{
					for (auto& size : sizes)
					{
						HGLOBAL hDib = BuildTestDib(size[0], size[1], bitCount, topDown);
						Assert::IsNotNull(hDib);
						const auto before = SnapshotDib(hDib);
						Assert::IsTrue(RunDirect(hDib, MakeSelectionRect(0, 0, size[0], size[1])));
						const auto after = SnapshotDib(hDib);
						Assert::IsTrue(memcmp(before.data(), after.data(), before.size()) != 0,
							L"processing must change the image");
						GlobalFree(hDib);
					}
				}
			}
		}

		TEST_METHOD(DirectModeFailsCleanlyWhenEveryAllocationFails)
		{
			HGLOBAL hDib = BuildTestDib(64, 48, 24, false);
			Assert::IsNotNull(hDib);
			const auto pristine = SnapshotDib(hDib);

			// Reference pass: count the allocations of a clean run.
			AltaLuxTest::ArmAllocationFailureAt(LONG_MAX);
			Assert::IsTrue(RunDirect(hDib, MakeSelectionRect(0, 0, 64, 48)));
			const long probedCount = AltaLuxTest::AllocationCount();
			AltaLuxTest::DisarmAllocationFailure();
			Assert::IsTrue(probedCount > 0, L"the apply path performed no probed allocations?");

			for (long n = 1; n <= probedCount; ++n)
			{
				RestoreDib(hDib, pristine);
				AltaLuxTest::ArmAllocationFailureAt(n);
				const bool result = RunDirect(hDib, MakeSelectionRect(0, 0, 64, 48));
				AltaLuxTest::DisarmAllocationFailure();
				Assert::IsFalse(result, (std::to_wstring(n) + L": expected a clean failure").c_str());
				const auto after = SnapshotDib(hDib);
				Assert::IsTrue(memcmp(pristine.data(), after.data(), pristine.size()) == 0,
					(std::to_wstring(n) + L": the DIB must stay untouched on failure").c_str());
			}
			GlobalFree(hDib);
		}

		TEST_METHOD(DirectModeProcessesExactlyTheSelectedRect)
		{
			// An odd selection at odd offsets must be applied pixel-exactly:
			// everything outside stays untouched (IrfanView's rect is in
			// visual coordinates) and the border pixels of the selection are
			// processed, not snapped away to a multiple of 8.
			const RECT selection = MakeSelectionRect(13, 7, 21, 17);
			// The checker wants exclusive edges; IrfanView rects carry
			// width/height in right/bottom.
			const RECT edges = { selection.left, selection.top,
				selection.left + selection.right, selection.top + selection.bottom };
			for (int bitCount = 24; bitCount <= 32; bitCount += 8)
			{
				HGLOBAL hDib = BuildTestDib(65, 49, bitCount, false);
				Assert::IsNotNull(hDib);
				const auto before = SnapshotDib(hDib);
				Assert::IsTrue(RunDirect(hDib, selection));
				AssertOutsideRectUnchanged(hDib, before, edges);
				Assert::IsTrue(PixelChanged(hDib, before, selection.left + selection.right - 1,
					selection.top + selection.bottom - 1),
					L"the last column/row of the selection must be processed");
				Assert::IsTrue(PixelChanged(hDib, before, selection.left, selection.top),
					L"the first pixel of the selection must be processed");
				GlobalFree(hDib);
			}
		}

		TEST_METHOD(DirectModeSelectsTheVisualTopLeftQuadrantOnBottomUpDibs)
		{
			// Bottom-up DIBs store memory row 0 at the visual bottom, so the
			// selection rect must map onto the mirrored memory rows. A
			// quadrant image with a top-left selection proves the mapping.
			const int width = 64;
			const int height = 48;
			HGLOBAL hDib = BuildQuadrantDib(width, height, 24, false);
			Assert::IsNotNull(hDib);
			const RECT selection = MakeSelectionRect(0, 0, width / 2, height / 2);
			const RECT edges = { 0, 0, width / 2, height / 2 };
			const auto before = SnapshotDib(hDib);
			Assert::IsTrue(RunDirect(hDib, selection));
			AssertOutsideRectUnchanged(hDib, before, edges);
			Assert::IsTrue(PixelChanged(hDib, before, 3, 3),
				L"the visual top-left quadrant must be processed");
			GlobalFree(hDib);
		}

		TEST_METHOD(DirectModeLeavesTinyAndEmptySelectionsUntouched)
		{
			// Selections that are empty after clamping, or too small for a
			// sensible tile, succeed as no-ops instead of failing silently.
			HGLOBAL hDib = BuildTestDib(64, 48, 24, false);
			Assert::IsNotNull(hDib);
			const auto pristine = SnapshotDib(hDib);
			const RECT cases[] =
			{
				MakeSelectionRect(10, 10, 0, 10),    // empty
				MakeSelectionRect(10, 10, 3, 3),     // below the minimum tile size
				MakeSelectionRect(70, 10, 20, 10),   // fully outside; clamps to empty
			};
			for (const RECT& selection : cases)
			{
				Assert::IsTrue(RunDirect(hDib, selection));
				const auto after = SnapshotDib(hDib);
				Assert::IsTrue(memcmp(pristine.data(), after.data(), pristine.size()) == 0,
					L"a no-op selection must leave the DIB untouched");
			}
			GlobalFree(hDib);
		}

		TEST_METHOD(OversizedImageIsRejectedBeforeAnyAllocation)
		{
			HGLOBAL hDib = BuildOversizedDib();
			Assert::IsNotNull(hDib);
			const auto pristine = SnapshotDib(hDib);

			AltaLuxTest::ArmAllocationFailureAt(LONG_MAX);
			Assert::IsFalse(RunDirect(hDib, MakeSelectionRect(0, 0, 60000, 60000)));
			Assert::AreEqual(0L, AltaLuxTest::AllocationCount(),
				L"oversized images must be rejected before allocating");
			AltaLuxTest::DisarmAllocationFailure();

			const auto after = SnapshotDib(hDib);
			Assert::IsTrue(memcmp(pristine.data(), after.data(), pristine.size()) == 0);
			GlobalFree(hDib);
		}
	};
}
