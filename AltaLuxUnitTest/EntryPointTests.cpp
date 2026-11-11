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
