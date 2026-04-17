#include "stdafx.h"
#include "CppUnitTest.h"

#include "..\AltaLux\AltaLuxCore.h"
#include "..\AltaLux\Filter\CBaseAltaLuxFilter.h"
#include "..\AltaLux\Filter\CAltaLuxFilterFactory.h"

#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace AltaLuxUnitTest
{
	TEST_CLASS(TestStrategies)
	{
	public:
		static const int IMAGE_WIDTH = 1024;
		static const int IMAGE_HEIGHT = 768;
		static const int RGBA_PIXEL_SIZE = 4;
		static const int IMAGE_SIZE = IMAGE_WIDTH * IMAGE_HEIGHT * RGBA_PIXEL_SIZE;

		unsigned char* InputImage;
		unsigned char* SerialImage;
		unsigned char* ParallelImage;

		TEST_METHOD_INITIALIZE(SetupBitmaps)
		{
			InputImage = new unsigned char[IMAGE_SIZE];
			srand(0x5555);
			for (int j = 0; j < IMAGE_SIZE; ++j)
			{
				InputImage[j] = static_cast<unsigned char>(rand() % 256);
			}

			SerialImage = new unsigned char[IMAGE_SIZE];
			memcpy(SerialImage, InputImage, IMAGE_SIZE);
			CBaseAltaLuxFilter* serialCode = CAltaLuxFilterFactory::CreateSpecificAltaLuxFilter(
				ALTALUX_FILTER_SERIAL, IMAGE_WIDTH, IMAGE_HEIGHT);
			serialCode->ProcessRGB32(SerialImage);
			delete serialCode;

			ParallelImage = new unsigned char[IMAGE_SIZE];
			memcpy(ParallelImage, InputImage, IMAGE_SIZE);
		}

		TEST_METHOD_CLEANUP(FreeBitmaps)
		{
			delete[] SerialImage;
			delete[] ParallelImage;
			delete[] InputImage;
		}

		TEST_METHOD(ParallelSplitLoopTest)
		{
			CBaseAltaLuxFilter* parallelCode = CAltaLuxFilterFactory::CreateSpecificAltaLuxFilter(
				ALTALUX_FILTER_PARALLEL_SPLIT_LOOP, IMAGE_WIDTH, IMAGE_HEIGHT);
			parallelCode->ProcessRGB32(ParallelImage);
			Assert::IsTrue(memcmp(SerialImage, ParallelImage, IMAGE_SIZE) == 0);
			Assert::IsFalse(memcmp(InputImage, ParallelImage, IMAGE_SIZE) == 0);
			delete parallelCode;
		}

		TEST_METHOD(ParallelEventTest)
		{
			CBaseAltaLuxFilter* parallelCode = CAltaLuxFilterFactory::CreateSpecificAltaLuxFilter(
				ALTALUX_FILTER_PARALLEL_EVENT, IMAGE_WIDTH, IMAGE_HEIGHT);
			parallelCode->ProcessRGB32(ParallelImage);
			Assert::IsTrue(memcmp(SerialImage, ParallelImage, IMAGE_SIZE) == 0);
			Assert::IsFalse(memcmp(InputImage, ParallelImage, IMAGE_SIZE) == 0);
			delete parallelCode;
		}

		TEST_METHOD(ParallelActiveWaitTest)
		{
			CBaseAltaLuxFilter* parallelCode = CAltaLuxFilterFactory::CreateSpecificAltaLuxFilter(
				ALTALUX_FILTER_ACTIVE_WAIT, IMAGE_WIDTH, IMAGE_HEIGHT);
			parallelCode->ProcessRGB32(ParallelImage);
			Assert::IsTrue(memcmp(SerialImage, ParallelImage, IMAGE_SIZE) == 0);
			Assert::IsFalse(memcmp(InputImage, ParallelImage, IMAGE_SIZE) == 0);
			delete parallelCode;
		}
	};

	TEST_CLASS(CoreTests)
	{
	public:
		static UiState MakeProcessingState(int strength, int detail, int naturalLook)
		{
			UiState state = { strength, detail, naturalLook, false, false, false, 0 };
			return state;
		}

		static std::vector<unsigned char> MakeCheckerboardRgb24(int width, int height, unsigned char low, unsigned char high)
		{
			std::vector<unsigned char> image(static_cast<size_t>(width * height * Constants::Rgb24PixelSize), 0);
			for (int y = 0; y < height; ++y)
			{
				for (int x = 0; x < width; ++x)
				{
					const unsigned char value = (((x + y) & 1) == 0) ? low : high;
					const int base = ((y * width) + x) * Constants::Rgb24PixelSize;
					image[base] = value;
					image[base + 1] = value;
					image[base + 2] = value;
				}
			}
			return image;
		}

		static std::vector<unsigned char> MakeHorizontalGradientRgb24(int width, int height)
		{
			std::vector<unsigned char> image(static_cast<size_t>(width * height * Constants::Rgb24PixelSize), 0);
			for (int y = 0; y < height; ++y)
			{
				for (int x = 0; x < width; ++x)
				{
					const unsigned char value = static_cast<unsigned char>((x * 255) / (width - 1));
					const int base = ((y * width) + x) * Constants::Rgb24PixelSize;
					image[base] = value;
					image[base + 1] = value;
					image[base + 2] = value;
				}
			}
			return image;
		}

		static std::vector<unsigned char> ConvertRgb24ToRgb32(const std::vector<unsigned char>& rgb24, unsigned char alpha)
		{
			const size_t pixelCount = rgb24.size() / Constants::Rgb24PixelSize;
			std::vector<unsigned char> rgb32(pixelCount * Constants::Rgb32PixelSize, alpha);
			for (size_t pixelIndex = 0; pixelIndex < pixelCount; ++pixelIndex)
			{
				const size_t srcBase = pixelIndex * Constants::Rgb24PixelSize;
				const size_t dstBase = pixelIndex * Constants::Rgb32PixelSize;
				rgb32[dstBase] = rgb24[srcBase];
				rgb32[dstBase + 1] = rgb24[srcBase + 1];
				rgb32[dstBase + 2] = rgb24[srcBase + 2];
			}
			return rgb32;
		}

		static int SumAbsoluteRgbDifference(const std::vector<unsigned char>& a, const std::vector<unsigned char>& b, int bitDepth)
		{
			int sum = 0;
			const size_t pixelCount = a.size() / bitDepth;
			for (size_t pixelIndex = 0; pixelIndex < pixelCount; ++pixelIndex)
			{
				const size_t base = pixelIndex * bitDepth;
				sum += abs(static_cast<int>(a[base]) - static_cast<int>(b[base]));
				sum += abs(static_cast<int>(a[base + 1]) - static_cast<int>(b[base + 1]));
				sum += abs(static_cast<int>(a[base + 2]) - static_cast<int>(b[base + 2]));
			}
			return sum;
		}

		TEST_METHOD(ApplyPresetSetsExpectedValues)
		{
			UiState state = { 0, 0, 0, false, false, false, 0 };
			Preset preset = { 0, L"Detail", 55, 60, 10 };

			ApplyPreset(state, preset);

			Assert::AreEqual(55, state.strength);
			Assert::AreEqual(60, state.detail);
			Assert::AreEqual(10, state.naturalLook);
		}

		TEST_METHOD(PresetToleranceDetectionWorks)
		{
			UiState state = { 46, 24, 27, false, false, false, 0 };
			Preset preset = { 0, L"Balanced", 45, 25, 25 };

			Assert::IsTrue(IsPresetActive(state, preset, 3));
			Assert::IsFalse(IsPresetActive(state, preset, 1));
		}

		TEST_METHOD(BlendWeightsNormalizeAndRespectBalancedFloor)
		{
			UiState state = { 45, 100, 100, false, false, false, 0 };
			BlendWeights weights = ComputeBlendWeights(state);
			const float sum = weights.fine + weights.balanced + weights.smooth;

			Assert::IsTrue(weights.balanced >= Constants::MinBalancedWeight - 0.0001f);
			Assert::IsTrue(sum > 0.999f && sum < 1.001f);
		}

		TEST_METHOD(LayerStrengthUsesConservativeNonLinearCurve)
		{
			Assert::AreEqual(0, ComputeLayerStrength(0));
			Assert::AreEqual(11, ComputeLayerStrength(10));
			Assert::AreEqual(15, ComputeLayerStrength(25));
			Assert::AreEqual(20, ComputeLayerStrength(45));
			Assert::AreEqual(26, ComputeLayerStrength(60));
			Assert::AreEqual(31, ComputeLayerStrength(75));
			Assert::AreEqual(38, ComputeLayerStrength(90));
			Assert::AreEqual(42, ComputeLayerStrength(100));
		}

		TEST_METHOD(LayerStrengthCurveIsMonotonicAndClamped)
		{
			int previous = ComputeLayerStrength(0);
			for (int strength = 1; strength <= 100; ++strength)
			{
				const int current = ComputeLayerStrength(strength);
				Assert::IsTrue(current >= previous);
				Assert::IsTrue(current >= Constants::MinLayerStrength);
				Assert::IsTrue(current <= Constants::MaxLayerStrength);
				previous = current;
			}

			Assert::AreEqual(0, ComputeLayerStrength(-20));
			Assert::AreEqual(Constants::MaxLayerStrength, ComputeLayerStrength(140));
		}

		TEST_METHOD(DetailAndNaturalAreIndependent)
		{
			UiState baselineState = { 45, 0, 0, false, false, false, 0 };
			UiState detailState = { 45, 100, 0, false, false, false, 0 };
			UiState naturalState = { 45, 0, 100, false, false, false, 0 };
			UiState combinedState = { 45, 100, 100, false, false, false, 0 };

			BlendWeights baselineWeights = ComputeBlendWeights(baselineState);
			BlendWeights detailWeights = ComputeBlendWeights(detailState);
			BlendWeights naturalWeights = ComputeBlendWeights(naturalState);
			BlendWeights combinedWeights = ComputeBlendWeights(combinedState);

			Assert::IsTrue(detailWeights.fine > baselineWeights.fine);
			Assert::IsTrue(naturalWeights.smooth > baselineWeights.smooth);
			Assert::IsTrue(combinedWeights.fine > baselineWeights.fine);
			Assert::IsTrue(combinedWeights.smooth > baselineWeights.smooth);
		}

		TEST_METHOD(SafeLayerRegionsClampToImageAndLimits)
		{
			Assert::AreEqual(16, GetSafeLayerRegions(200, 150, 32, 2, 16));
			Assert::AreEqual(1, GetSafeLayerRegions(1, 1, 16, 2, 16));
			Assert::AreEqual(4, GetSafeLayerRegions(10, 4, 4, 2, 16));
			Assert::AreEqual(1, GetSafeLayerRegions(0, 0, 8, 2, 16));
		}

		TEST_METHOD(LayerRegionCountsMatchFineToSmoothSemantics)
		{
			Assert::IsTrue(Constants::FineRegions > Constants::BalancedRegions);
			Assert::IsTrue(Constants::BalancedRegions > Constants::SmoothRegions);
		}

		TEST_METHOD(PreviewImageRectFitsAndCentersImage)
		{
			RECT container = { 0, 0, 400, 200 };
			RECT fitted = GetPreviewImageRect(800, 400, container, false);

			Assert::AreEqual(0L, fitted.left);
			Assert::AreEqual(0L, fitted.top);
			Assert::AreEqual(400L, fitted.right);
			Assert::AreEqual(200L, fitted.bottom);
		}

		TEST_METHOD(PreviewImageRectHonorsNoRescalingCropMode)
		{
			RECT container = { 10, 20, 210, 120 };
			RECT cropped = GetPreviewImageRect(400, 300, container, true);

			Assert::AreEqual(container.left, cropped.left);
			Assert::AreEqual(container.top, cropped.top);
			Assert::AreEqual(container.right, cropped.right);
			Assert::AreEqual(container.bottom, cropped.bottom);
		}

		TEST_METHOD(MultiscaleProcessingIsNoOpAtZeroStrength)
		{
			const int width = 16;
			const int height = 16;
			const int bitDepth = Constants::Rgb24PixelSize;
			std::vector<unsigned char> input(static_cast<size_t>(width * height * bitDepth));
			for (size_t i = 0; i < input.size(); ++i)
			{
				input[i] = static_cast<unsigned char>((i * 37U) % 251U);
			}

			std::vector<unsigned char> output(input.size(), 0);
			const bool processed = ProcessMultiscaleImage(input.data(), output.data(), width, height, bitDepth,
				MakeProcessingState(0, 60, 40));

			Assert::IsTrue(processed);
			Assert::IsTrue(memcmp(input.data(), output.data(), input.size()) == 0);
		}

		TEST_METHOD(MultiscaleProcessingPreservesAlphaForRgb32)
		{
			const int width = 16;
			const int height = 16;
			const int bitDepth = Constants::Rgb32PixelSize;
			std::vector<unsigned char> input(static_cast<size_t>(width * height * bitDepth));
			for (int pixelIndex = 0; pixelIndex < width * height; ++pixelIndex)
			{
				const int base = pixelIndex * bitDepth;
				input[base] = static_cast<unsigned char>((pixelIndex * 11) % 256);
				input[base + 1] = static_cast<unsigned char>((pixelIndex * 17) % 256);
				input[base + 2] = static_cast<unsigned char>((pixelIndex * 23) % 256);
				input[base + 3] = static_cast<unsigned char>((pixelIndex * 29) % 256);
			}

			std::vector<unsigned char> output(input.size(), 0);
			const bool processed = ProcessMultiscaleImage(input.data(), output.data(), width, height, bitDepth,
				MakeProcessingState(45, 25, 25));

			Assert::IsTrue(processed);
			for (int pixelIndex = 0; pixelIndex < width * height; ++pixelIndex)
			{
				const int alphaOffset = (pixelIndex * bitDepth) + 3;
				Assert::AreEqual(static_cast<int>(input[alphaOffset]), static_cast<int>(output[alphaOffset]));
			}
		}

		TEST_METHOD(MultiscaleProcessingKeepsFlatImageGrayscale)
		{
			const int width = 16;
			const int height = 16;
			const int bitDepth = Constants::Rgb24PixelSize;
			std::vector<unsigned char> input(static_cast<size_t>(width * height * bitDepth), 96);
			std::vector<unsigned char> output(input.size(), 0);

			const bool processed = ProcessMultiscaleImage(input.data(), output.data(), width, height, bitDepth,
				MakeProcessingState(45, 25, 25));

			Assert::IsTrue(processed);
			for (int pixelIndex = 0; pixelIndex < width * height; ++pixelIndex)
			{
				const int base = pixelIndex * bitDepth;
				Assert::AreEqual(static_cast<int>(output[base]), static_cast<int>(output[base + 1]));
				Assert::AreEqual(static_cast<int>(output[base + 1]), static_cast<int>(output[base + 2]));
			}
		}

		TEST_METHOD(MultiscaleProcessingRgb24AndRgb32MatchForSameRgbContent)
		{
			const int width = 16;
			const int height = 16;
			const std::vector<unsigned char> rgb24 = MakeHorizontalGradientRgb24(width, height);
			const std::vector<unsigned char> rgb32 = ConvertRgb24ToRgb32(rgb24, 197);

			std::vector<unsigned char> out24(rgb24.size(), 0);
			std::vector<unsigned char> out32(rgb32.size(), 0);

			const UiState state = MakeProcessingState(45, 25, 25);
			Assert::IsTrue(ProcessMultiscaleImage(rgb24.data(), out24.data(), width, height, Constants::Rgb24PixelSize, state));
			Assert::IsTrue(ProcessMultiscaleImage(rgb32.data(), out32.data(), width, height, Constants::Rgb32PixelSize, state));

			for (int pixelIndex = 0; pixelIndex < width * height; ++pixelIndex)
			{
				const int base24 = pixelIndex * Constants::Rgb24PixelSize;
				const int base32 = pixelIndex * Constants::Rgb32PixelSize;
				Assert::AreEqual(static_cast<int>(out24[base24]), static_cast<int>(out32[base32]));
				Assert::AreEqual(static_cast<int>(out24[base24 + 1]), static_cast<int>(out32[base32 + 1]));
				Assert::AreEqual(static_cast<int>(out24[base24 + 2]), static_cast<int>(out32[base32 + 2]));
				Assert::AreEqual(197, static_cast<int>(out32[base32 + 3]));
			}
		}

		TEST_METHOD(DetailChangesCheckerboardOutput)
		{
			const int width = 16;
			const int height = 16;
			const std::vector<unsigned char> input = MakeCheckerboardRgb24(width, height, 32, 192);
			std::vector<unsigned char> lowDetail(input.size(), 0);
			std::vector<unsigned char> highDetail(input.size(), 0);

			Assert::IsTrue(ProcessMultiscaleImage(input.data(), lowDetail.data(), width, height, Constants::Rgb24PixelSize,
				MakeProcessingState(45, 0, 25)));
			Assert::IsTrue(ProcessMultiscaleImage(input.data(), highDetail.data(), width, height, Constants::Rgb24PixelSize,
				MakeProcessingState(45, 100, 25)));

			Assert::IsTrue(SumAbsoluteRgbDifference(lowDetail, highDetail, Constants::Rgb24PixelSize) > 0);
		}

		TEST_METHOD(NaturalLookChangesGradientOutput)
		{
			const int width = 16;
			const int height = 16;
			const std::vector<unsigned char> input = MakeHorizontalGradientRgb24(width, height);
			std::vector<unsigned char> lowNatural(input.size(), 0);
			std::vector<unsigned char> highNatural(input.size(), 0);

			Assert::IsTrue(ProcessMultiscaleImage(input.data(), lowNatural.data(), width, height, Constants::Rgb24PixelSize,
				MakeProcessingState(45, 25, 0)));
			Assert::IsTrue(ProcessMultiscaleImage(input.data(), highNatural.data(), width, height, Constants::Rgb24PixelSize,
				MakeProcessingState(45, 25, 100)));

			Assert::IsTrue(SumAbsoluteRgbDifference(lowNatural, highNatural, Constants::Rgb24PixelSize) > 0);
		}

		TEST_METHOD(MultiscaleProcessingHandlesImageAboveParallelLayerThreshold)
		{
			const int width = 1024;
			const int height = 1024;
			const int bitDepth = Constants::Rgb24PixelSize;
			std::vector<unsigned char> input(static_cast<size_t>(width * height * bitDepth), 0);
			for (int y = 0; y < height; ++y)
			{
				for (int x = 0; x < width; ++x)
				{
					const int base = ((y * width) + x) * bitDepth;
					input[base] = static_cast<unsigned char>((x + y) & 0xFF);
					input[base + 1] = static_cast<unsigned char>(((2 * x) + y) & 0xFF);
					input[base + 2] = static_cast<unsigned char>((x + (2 * y)) & 0xFF);
				}
			}

			std::vector<unsigned char> output(input.size(), 0);
			const bool processed = ProcessMultiscaleImage(input.data(), output.data(), width, height, bitDepth,
				MakeProcessingState(45, 25, 25));

			Assert::IsTrue(processed);
			Assert::IsTrue(SumAbsoluteRgbDifference(input, output, bitDepth) > 0);
		}
	};
}
