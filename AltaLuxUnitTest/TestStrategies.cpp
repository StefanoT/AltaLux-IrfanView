#include "stdafx.h"
#include "CppUnitTest.h"

#include "..\AltaLux\AltaLuxCore.h"
#include "..\AltaLux\ChromaCorrection.h"
#include "..\AltaLux\Kernels\Kernels.h"
#include "..\AltaLux\Filter\CBaseAltaLuxFilter.h"
#include "..\AltaLux\Filter\CAltaLuxFilterFactory.h"
#include "..\AltaLux\Segmentation\SelectionCore.h"

#include <memory>
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

	};

	TEST_CLASS(CoreTests)
	{
	public:
		static UiState MakeProcessingState(int strength, int detail, int naturalLook, int chromaProtection = 0)
		{
			UiState state = { strength, detail, naturalLook, chromaProtection, false, false, false, 0 };
			return state;
		}

		static std::vector<unsigned char> MakeCheckerboardRGB24(int width, int height, unsigned char low, unsigned char high)
		{
			std::vector<unsigned char> image(static_cast<size_t>(width * height * Constants::RGB24PixelSize), 0);
			for (int y = 0; y < height; ++y)
			{
				for (int x = 0; x < width; ++x)
				{
					const unsigned char value = (((x + y) & 1) == 0) ? low : high;
					const int base = ((y * width) + x) * Constants::RGB24PixelSize;
					image[base] = value;
					image[base + 1] = value;
					image[base + 2] = value;
				}
			}
			return image;
		}

		static std::vector<unsigned char> MakeHorizontalGradientRGB24(int width, int height)
		{
			std::vector<unsigned char> image(static_cast<size_t>(width * height * Constants::RGB24PixelSize), 0);
			for (int y = 0; y < height; ++y)
			{
				for (int x = 0; x < width; ++x)
				{
					const unsigned char value = static_cast<unsigned char>((x * 255) / (width - 1));
					const int base = ((y * width) + x) * Constants::RGB24PixelSize;
					image[base] = value;
					image[base + 1] = value;
					image[base + 2] = value;
				}
			}
			return image;
		}

		static std::vector<unsigned char> ConvertRGB24ToRGB32(const std::vector<unsigned char>& rgb24, unsigned char alpha)
		{
			const size_t pixelCount = rgb24.size() / Constants::RGB24PixelSize;
			std::vector<unsigned char> rgb32(pixelCount * Constants::RGB32PixelSize, alpha);
			for (size_t pixelIndex = 0; pixelIndex < pixelCount; ++pixelIndex)
			{
				const size_t srcBase = pixelIndex * Constants::RGB24PixelSize;
				const size_t dstBase = pixelIndex * Constants::RGB32PixelSize;
				rgb32[dstBase] = rgb24[srcBase];
				rgb32[dstBase + 1] = rgb24[srcBase + 1];
				rgb32[dstBase + 2] = rgb24[srcBase + 2];
			}
			return rgb32;
		}

		static int SumAbsoluteRGBDifference(const std::vector<unsigned char>& a, const std::vector<unsigned char>& b, int bitDepth)
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

		static std::vector<unsigned char> MakePatternImage(int width, int height, int bitDepth)
		{
			std::vector<unsigned char> image(static_cast<size_t>(width * height * bitDepth), 0);
			for (int pixelIndex = 0; pixelIndex < width * height; ++pixelIndex)
			{
				const int base = pixelIndex * bitDepth;
				image[base] = static_cast<unsigned char>((pixelIndex * 13 + 17) & 0xFF);
				image[base + 1] = static_cast<unsigned char>((pixelIndex * 29 + 41) & 0xFF);
				image[base + 2] = static_cast<unsigned char>((pixelIndex * 53 + 73) & 0xFF);
				if (bitDepth == Constants::RGB32PixelSize)
				{
					image[base + 3] = static_cast<unsigned char>((pixelIndex * 7 + 197) & 0xFF);
				}
			}
			return image;
		}

		static std::vector<unsigned char> MakePackedYUVImage(int width, int height, bool lumaInHighByte)
		{
			std::vector<unsigned char> image(static_cast<size_t>(width * height * 2), 0);
			const int lumaOffset = lumaInHighByte ? 1 : 0;
			const int chromaOffset = lumaInHighByte ? 0 : 1;
			for (int pixelIndex = 0; pixelIndex < width * height; ++pixelIndex)
			{
				image[pixelIndex * 2 + lumaOffset] = static_cast<unsigned char>((pixelIndex * 19 + 23) & 0xFF);
				image[pixelIndex * 2 + chromaOffset] = static_cast<unsigned char>((pixelIndex * 31 + 101) & 0xFF);
			}
			return image;
		}

		static std::vector<unsigned char> RunSerialFilter(
			const std::vector<unsigned char>& input,
			int width,
			int height,
			AltaLuxKernels::KernelImplementation implementation,
			int (CBaseAltaLuxFilter::*processMethod)(void*))
		{
			std::vector<unsigned char> output = input;
			std::unique_ptr<CBaseAltaLuxFilter> filter(CAltaLuxFilterFactory::CreateSpecificAltaLuxFilter(
				ALTALUX_FILTER_SERIAL, width, height));
			Assert::IsTrue(filter != nullptr);
			filter->SetStrength(45);
			filter->SetKernelImplementation(implementation);
			Assert::AreEqual(AL_OK, (filter.get()->*processMethod)(output.data()));
			return output;
		}

		static void AssertVectorsEqual(const std::vector<unsigned char>& expected, const std::vector<unsigned char>& actual)
		{
			Assert::AreEqual(expected.size(), actual.size());
			Assert::IsTrue(memcmp(expected.data(), actual.data(), expected.size()) == 0);
		}

		static void AssertFilterOutputsMatchScalar(AltaLuxKernels::KernelImplementation implementation)
		{
			if (!AltaLuxKernels::IsImplementationSupported(implementation))
			{
				Logger::WriteMessage(L"Requested SIMD kernels are not supported on this CPU; skipping equality checks.");
				return;
			}

			const int width = 64;
			const int height = 48;

			const std::vector<unsigned char> rgb24 = MakePatternImage(width, height, Constants::RGB24PixelSize);
			AssertVectorsEqual(
				RunSerialFilter(rgb24, width, height, AltaLuxKernels::KernelImplementation::Scalar, &CBaseAltaLuxFilter::ProcessRGB24),
				RunSerialFilter(rgb24, width, height, implementation, &CBaseAltaLuxFilter::ProcessRGB24));
			AssertVectorsEqual(
				RunSerialFilter(rgb24, width, height, AltaLuxKernels::KernelImplementation::Scalar, &CBaseAltaLuxFilter::ProcessBGR24),
				RunSerialFilter(rgb24, width, height, implementation, &CBaseAltaLuxFilter::ProcessBGR24));

			const std::vector<unsigned char> rgb32 = MakePatternImage(width, height, Constants::RGB32PixelSize);
			AssertVectorsEqual(
				RunSerialFilter(rgb32, width, height, AltaLuxKernels::KernelImplementation::Scalar, &CBaseAltaLuxFilter::ProcessRGB32),
				RunSerialFilter(rgb32, width, height, implementation, &CBaseAltaLuxFilter::ProcessRGB32));
			AssertVectorsEqual(
				RunSerialFilter(rgb32, width, height, AltaLuxKernels::KernelImplementation::Scalar, &CBaseAltaLuxFilter::ProcessBGR32),
				RunSerialFilter(rgb32, width, height, implementation, &CBaseAltaLuxFilter::ProcessBGR32));

			const std::vector<unsigned char> uyvy = MakePackedYUVImage(width, height, true);
			AssertVectorsEqual(
				RunSerialFilter(uyvy, width, height, AltaLuxKernels::KernelImplementation::Scalar, &CBaseAltaLuxFilter::ProcessUYVY),
				RunSerialFilter(uyvy, width, height, implementation, &CBaseAltaLuxFilter::ProcessUYVY));

			const std::vector<unsigned char> yuyv = MakePackedYUVImage(width, height, false);
			AssertVectorsEqual(
				RunSerialFilter(yuyv, width, height, AltaLuxKernels::KernelImplementation::Scalar, &CBaseAltaLuxFilter::ProcessYUYV),
				RunSerialFilter(yuyv, width, height, implementation, &CBaseAltaLuxFilter::ProcessYUYV));

			const UiState state = MakeProcessingState(45, 25, 25);
			std::vector<unsigned char> scalarMultiscale(rgb24.size(), 0);
			std::vector<unsigned char> implementationMultiscale(rgb24.size(), 0);
			Assert::IsTrue(ProcessMultiscaleImageWithKernels(rgb24.data(), scalarMultiscale.data(),
				width, height, Constants::RGB24PixelSize, state, AltaLuxKernels::KernelImplementation::Scalar));
			Assert::IsTrue(ProcessMultiscaleImageWithKernels(rgb24.data(), implementationMultiscale.data(),
				width, height, Constants::RGB24PixelSize, state, implementation));
			AssertVectorsEqual(scalarMultiscale, implementationMultiscale);

			scalarMultiscale.assign(rgb32.size(), 0);
			implementationMultiscale.assign(rgb32.size(), 0);
			Assert::IsTrue(ProcessMultiscaleImageWithKernels(rgb32.data(), scalarMultiscale.data(),
				width, height, Constants::RGB32PixelSize, state, AltaLuxKernels::KernelImplementation::Scalar));
			Assert::IsTrue(ProcessMultiscaleImageWithKernels(rgb32.data(), implementationMultiscale.data(),
				width, height, Constants::RGB32PixelSize, state, implementation));
			AssertVectorsEqual(scalarMultiscale, implementationMultiscale);
		}

		static void AssertScaleDownBoxMatchesScalar(AltaLuxKernels::KernelImplementation implementation)
		{
			if (!AltaLuxKernels::IsImplementationSupported(implementation))
			{
				Logger::WriteMessage(L"Requested SIMD kernels are not supported on this CPU; skipping downscale equality checks.");
				return;
			}

			const int width = 18;
			const int height = 14;
			const int scaleFactor = 2;
			for (int bitDepth = Constants::RGB24PixelSize; bitDepth <= Constants::RGB32PixelSize; ++bitDepth)
			{
				const std::vector<unsigned char> input = MakePatternImage(width, height, bitDepth);
				const int targetWidth = width / scaleFactor;
				const int targetHeight = height / scaleFactor;
				std::vector<unsigned char> scalar(static_cast<size_t>(targetWidth * targetHeight * bitDepth), 0);
				std::vector<unsigned char> simd(scalar.size(), 0);

				AltaLuxKernels::ScaleDownBox(input.data(), width, height, scalar.data(),
					scaleFactor, bitDepth, AltaLuxKernels::KernelImplementation::Scalar);
				AltaLuxKernels::ScaleDownBox(input.data(), width, height, simd.data(),
					scaleFactor, bitDepth, implementation);
				AssertVectorsEqual(scalar, simd);
			}
		}

		TEST_METHOD(SSSE3FilterOutputsMatchScalar)
		{
			AssertFilterOutputsMatchScalar(AltaLuxKernels::KernelImplementation::SSSE3);
		}

		TEST_METHOD(AVX2FilterOutputsMatchScalar)
		{
			AssertFilterOutputsMatchScalar(AltaLuxKernels::KernelImplementation::AVX2);
		}

		TEST_METHOD(SSSE3ScaleDownBoxMatchesScalar)
		{
			AssertScaleDownBoxMatchesScalar(AltaLuxKernels::KernelImplementation::SSSE3);
		}

		TEST_METHOD(AVX2ScaleDownBoxMatchesScalar)
		{
			AssertScaleDownBoxMatchesScalar(AltaLuxKernels::KernelImplementation::AVX2);
		}

		TEST_METHOD(ApplyPresetSetsExpectedValues)
		{
			UiState state = { 0, 0, 0, 0, false, false, false, 0 };
			Preset preset = { 0, L"Detail", 55, 60, 10 };

			ApplyPreset(state, preset);

			Assert::AreEqual(55, state.strength);
			Assert::AreEqual(60, state.detail);
			Assert::AreEqual(10, state.naturalLook);
		}

		TEST_METHOD(PresetToleranceDetectionWorks)
		{
			UiState state = { 46, 24, 27, 0, false, false, false, 0 };
			Preset preset = { 0, L"Balanced", 45, 25, 25 };

			Assert::IsTrue(IsPresetActive(state, preset, 3));
			Assert::IsFalse(IsPresetActive(state, preset, 1));
		}

		TEST_METHOD(BlendWeightsNormalizeAndRespectBalancedFloor)
		{
			UiState state = { 45, 100, 100, 0, false, false, false, 0 };
			BlendWeights weights = ComputeBlendWeights(state);
			const float sum = weights.fine + weights.balanced + weights.smooth;

			Assert::IsTrue(weights.balanced >= Constants::MinBalancedWeight - 0.0001f);
			Assert::IsTrue(sum > 0.999f && sum < 1.001f);
		}

		TEST_METHOD(LayerStrengthUsesConservativeNonLinearCurve)
		{
			Assert::AreEqual(0, ComputeLayerStrength(0));
			Assert::AreEqual(3, ComputeLayerStrength(10));
			Assert::AreEqual(11, ComputeLayerStrength(25));
			Assert::AreEqual(25, ComputeLayerStrength(45));
			Assert::AreEqual(37, ComputeLayerStrength(60));
			Assert::AreEqual(50, ComputeLayerStrength(75));
			Assert::AreEqual(65, ComputeLayerStrength(90));
			Assert::AreEqual(75, ComputeLayerStrength(100));
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

		TEST_METHOD(ClipHistogramRaisesImpossibleLowClipLimit)
		{
			auto assertClippedHistogramIsValid = [](unsigned int* histogram, unsigned int pixelCount,
				unsigned int requestedClipLimit)
			{
				const unsigned int minimumFeasibleClipLimit =
					(pixelCount + NUM_GRAY_LEVELS - 1) / NUM_GRAY_LEVELS;
				const unsigned int expectedMaxBin =
					requestedClipLimit > minimumFeasibleClipLimit ? requestedClipLimit : minimumFeasibleClipLimit;

				AltaLuxKernels::ClipHistogram(histogram, requestedClipLimit);

				unsigned int actualPixelCount = 0;
				unsigned int actualMaxBin = 0;
				for (int i = 0; i < NUM_GRAY_LEVELS; ++i)
				{
					actualPixelCount += histogram[i];
					if (actualMaxBin < histogram[i])
					{
						actualMaxBin = histogram[i];
					}
				}

				Assert::AreEqual(pixelCount, actualPixelCount);
				Assert::IsTrue(actualMaxBin <= expectedMaxBin);
			};

			const unsigned int pixelCounts[] = { 1, 255, 256, 257, 3900, 4096, 65536 };
			for (const unsigned int pixelCount : pixelCounts)
			{
				for (unsigned int requestedClipLimit = 1; requestedClipLimit <= 20; ++requestedClipLimit)
				{
					unsigned int histogram[NUM_GRAY_LEVELS] = {};
					histogram[42] = pixelCount;
					assertClippedHistogramIsValid(histogram, pixelCount, requestedClipLimit);
				}
			}

			for (unsigned int seed = 1; seed <= 8; ++seed)
			{
				unsigned int sourceHistogram[NUM_GRAY_LEVELS] = {};
				unsigned int pixelCount = 0;
				for (int i = 0; i < NUM_GRAY_LEVELS; ++i)
				{
					sourceHistogram[i] = ((static_cast<unsigned int>(i) * 37U) + (seed * 19U)) % 97U;
					pixelCount += sourceHistogram[i];
				}

				for (unsigned int requestedClipLimit = 1; requestedClipLimit <= 20; ++requestedClipLimit)
				{
					unsigned int histogram[NUM_GRAY_LEVELS] = {};
					memcpy(histogram, sourceHistogram, sizeof(histogram));
					assertClippedHistogramIsValid(histogram, pixelCount, requestedClipLimit);
				}
			}
		}

		TEST_METHOD(DetailAndNaturalAreIndependent)
		{
			UiState baselineState = { 45, 0, 0, 0, false, false, false, 0 };
			UiState detailState = { 45, 100, 0, 0, false, false, false, 0 };
			UiState naturalState = { 45, 0, 100, 0, false, false, false, 0 };
			UiState combinedState = { 45, 100, 100, 0, false, false, false, 0 };

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
			const int bitDepth = Constants::RGB24PixelSize;
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

		TEST_METHOD(MultiscaleProcessingHandlesVeryLowStrengths)
		{
			auto assertVeryLowStrengthsProcess = [](int width, int height, int firstStrength, int lastStrength)
			{
				const std::vector<unsigned char> input = MakePatternImage(width, height, Constants::RGB24PixelSize);

				for (int strength = firstStrength; strength <= lastStrength; ++strength)
				{
					const UiState state = MakeProcessingState(strength, 25, 25);
					std::vector<unsigned char> output(input.size(), 0);
					Assert::IsTrue(ProcessMultiscaleImage(input.data(), output.data(), width, height,
						Constants::RGB24PixelSize, state));

					std::vector<unsigned char> inPlace = input;
					Assert::IsTrue(ProcessMultiscaleImage(inPlace.data(), inPlace.data(), width, height,
						Constants::RGB24PixelSize, state));
				}
			};

			assertVeryLowStrengthsProcess(32, 24, 1, 7);
			assertVeryLowStrengthsProcess(1024, 1024, 1, 3);
		}

		TEST_METHOD(MultiscaleProcessingHandlesFlatLowStrengthWithTightClipLimit)
		{
			const int width = 800;
			const int height = 1248;
			const std::vector<unsigned char> input(static_cast<size_t>(width * height * Constants::RGB24PixelSize), 96);
			std::vector<unsigned char> output(input.size(), 0);

			Assert::IsTrue(ProcessMultiscaleImage(input.data(), output.data(), width, height,
				Constants::RGB24PixelSize, MakeProcessingState(3, 25, 25)));
		}

		TEST_METHOD(MultiscaleProcessingPreservesAlphaForRGB32)
		{
			const int width = 16;
			const int height = 16;
			const int bitDepth = Constants::RGB32PixelSize;
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
			const int bitDepth = Constants::RGB24PixelSize;
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

		TEST_METHOD(MultiscaleProcessingRGB24AndRGB32MatchForSameRGBContent)
		{
			const int width = 16;
			const int height = 16;
			const std::vector<unsigned char> rgb24 = MakeHorizontalGradientRGB24(width, height);
			const std::vector<unsigned char> rgb32 = ConvertRGB24ToRGB32(rgb24, 197);

			std::vector<unsigned char> out24(rgb24.size(), 0);
			std::vector<unsigned char> out32(rgb32.size(), 0);

			const UiState state = MakeProcessingState(45, 25, 25);
			Assert::IsTrue(ProcessMultiscaleImage(rgb24.data(), out24.data(), width, height, Constants::RGB24PixelSize, state));
			Assert::IsTrue(ProcessMultiscaleImage(rgb32.data(), out32.data(), width, height, Constants::RGB32PixelSize, state));

			for (int pixelIndex = 0; pixelIndex < width * height; ++pixelIndex)
			{
				const int base24 = pixelIndex * Constants::RGB24PixelSize;
				const int base32 = pixelIndex * Constants::RGB32PixelSize;
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
			const std::vector<unsigned char> input = MakeCheckerboardRGB24(width, height, 32, 192);
			std::vector<unsigned char> lowDetail(input.size(), 0);
			std::vector<unsigned char> highDetail(input.size(), 0);

			Assert::IsTrue(ProcessMultiscaleImage(input.data(), lowDetail.data(), width, height, Constants::RGB24PixelSize,
				MakeProcessingState(45, 0, 25)));
			Assert::IsTrue(ProcessMultiscaleImage(input.data(), highDetail.data(), width, height, Constants::RGB24PixelSize,
				MakeProcessingState(45, 100, 25)));

			Assert::IsTrue(SumAbsoluteRGBDifference(lowDetail, highDetail, Constants::RGB24PixelSize) > 0);
		}

		TEST_METHOD(NaturalLookChangesGradientOutput)
		{
			const int width = 16;
			const int height = 16;
			const std::vector<unsigned char> input = MakeHorizontalGradientRGB24(width, height);
			std::vector<unsigned char> lowNatural(input.size(), 0);
			std::vector<unsigned char> highNatural(input.size(), 0);

			Assert::IsTrue(ProcessMultiscaleImage(input.data(), lowNatural.data(), width, height, Constants::RGB24PixelSize,
				MakeProcessingState(45, 25, 0)));
			Assert::IsTrue(ProcessMultiscaleImage(input.data(), highNatural.data(), width, height, Constants::RGB24PixelSize,
				MakeProcessingState(45, 25, 100)));

			Assert::IsTrue(SumAbsoluteRGBDifference(lowNatural, highNatural, Constants::RGB24PixelSize) > 0);
		}

		TEST_METHOD(MultiscaleProcessingHandlesImageAboveParallelLayerThreshold)
		{
			const int width = 1024;
			const int height = 1024;
			const int bitDepth = Constants::RGB24PixelSize;
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
			Assert::IsTrue(SumAbsoluteRGBDifference(input, output, bitDepth) > 0);
		}

		TEST_METHOD(SelectionAddRemoveAndUndo)
		{
			std::vector<unsigned char> selection(8, 0);
			const unsigned char addMask[8] = { 0, 255, 255, 0, 0, 255, 0, 0 };
			const unsigned char removeMask[8] = { 0, 0, 255, 0, 0, 0, 0, 0 };
			SelectionHistory history;

			Assert::IsTrue(history.Apply(selection, addMask, selection.size(), SelectionCombineMode::Add));
			Assert::AreEqual(255, static_cast<int>(selection[1]));
			Assert::AreEqual(255, static_cast<int>(selection[2]));
			Assert::IsTrue(history.Apply(selection, removeMask, selection.size(), SelectionCombineMode::Remove));
			Assert::AreEqual(0, static_cast<int>(selection[2]));
			Assert::IsTrue(history.Undo(selection));
			Assert::AreEqual(255, static_cast<int>(selection[2]));
			Assert::IsTrue(history.Undo(selection));
			Assert::IsFalse(HasSelectedPixels(selection));
		}

		TEST_METHOD(SelectionFillIsUndoable)
		{
			std::vector<unsigned char> selection(6, 0);
			SelectionHistory history;
			Assert::IsTrue(history.Fill(selection, 255));
			Assert::IsTrue(HasSelectedPixels(selection));
			Assert::IsTrue(history.Undo(selection));
			Assert::IsFalse(HasSelectedPixels(selection));
		}

		TEST_METHOD(CompositeMaskPreservesAlphaAndEndpoints)
		{
			const unsigned char original[8] = { 10, 20, 30, 77, 40, 50, 60, 88 };
			const unsigned char processed[8] = { 110, 120, 130, 1, 140, 150, 160, 2 };
			const unsigned char mask[2] = { 0, 255 };
			unsigned char output[8] = {};

			Assert::IsTrue(CompositeWithMask(original, processed, output, mask, 2, 1, 4));
			Assert::AreEqual(10, static_cast<int>(output[0]));
			Assert::AreEqual(77, static_cast<int>(output[3]));
			Assert::AreEqual(140, static_cast<int>(output[4]));
			Assert::AreEqual(88, static_cast<int>(output[7]));
		}

		TEST_METHOD(FeatheredMaskExpandsOnePixel)
		{
			const unsigned char binary[9] = { 0, 0, 0, 0, 255, 0, 0, 0, 0 };
			std::vector<unsigned char> alpha;
			Assert::IsTrue(BuildFeatheredMask(binary, 3, 3, 0, alpha));
			for (unsigned char value : alpha)
			{
				Assert::AreEqual(255, static_cast<int>(value));
			}
		}

		TEST_METHOD(PreviewPointMapsToImageCoordinates)
		{
			const RECT rect = { 10, 20, 210, 120 };
			float imageX = 0.0f;
			float imageY = 0.0f;
			Assert::IsTrue(MapPreviewPointToImage(rect, 110, 70, 400, 200, imageX, imageY));
			Assert::IsTrue(imageX > 199.0f && imageX < 202.0f);
			Assert::IsTrue(imageY > 99.0f && imageY < 102.0f);
			Assert::IsFalse(MapPreviewPointToImage(rect, 9, 70, 400, 200, imageX, imageY));
		}
	};

	TEST_CLASS(ChromaCorrectionTests)
	{
	public:
		static std::vector<unsigned char> MakeShadowTestImageRGB24(int width, int height,
			bool textureRightHalf)
		{
			// Left half: flat dark red (luma ~17, far below the darkness threshold).
			// Right half: either a dark red checkerboard of two levels (texture) or
			// a bright neutral gray, per textureRightHalf.
			std::vector<unsigned char> image(static_cast<size_t>(width * height * Constants::RGB24PixelSize), 0);
			for (int y = 0; y < height; ++y)
			{
				for (int x = 0; x < width; ++x)
				{
					const int base = ((y * width) + x) * Constants::RGB24PixelSize;
					if (x < width / 2)
					{
						image[base] = 5;
						image[base + 1] = 5;
						image[base + 2] = 60;
					}
					else if (textureRightHalf)
					{
						const unsigned char red = (((x + y) & 1) == 0) ? 30 : 90;
						image[base] = 3;
						image[base + 1] = 3;
						image[base + 2] = red;
					}
					else
					{
						image[base] = 200;
						image[base + 1] = 200;
						image[base + 2] = 200;
					}
				}
			}
			return image;
		}

		// Chroma magnitude of a pixel measured as the sum of absolute channel
		// distances from its own luma. The chroma stage attenuates exactly this
		// quantity while leaving the luma itself alone.
		static double SumChromaDistance(const std::vector<unsigned char>& image, int width, int height,
			int bitDepth, int x0, int x1)
		{
			const double kr = 0.2126;
			const double kg = 0.7152;
			const double kb = 0.0722;
			double sum = 0.0;
			for (int y = 0; y < height; ++y)
			{
				for (int x = x0; x < x1; ++x)
				{
					const int base = ((y * width) + x) * bitDepth;
					// Buffers are BGR-ordered.
					const double b = image[base];
					const double g = image[base + 1];
					const double r = image[base + 2];
					const double luma = (kb * b) + (kg * g) + (kr * r);
					sum += std::abs(b - luma) + std::abs(g - luma) + std::abs(r - luma);
				}
			}
			return sum;
		}

		static std::vector<unsigned char> MakePatternImageForChroma(int width, int height, int bitDepth)
		{
			std::vector<unsigned char> image(static_cast<size_t>(width * height * bitDepth), 0);
			for (int pixelIndex = 0; pixelIndex < width * height; ++pixelIndex)
			{
				const int base = pixelIndex * bitDepth;
				image[base] = static_cast<unsigned char>((pixelIndex * 13 + 17) & 0xFF);
				image[base + 1] = static_cast<unsigned char>((pixelIndex * 29 + 41) & 0xFF);
				image[base + 2] = static_cast<unsigned char>((pixelIndex * 53 + 73) & 0xFF);
				if (bitDepth == Constants::RGB32PixelSize)
				{
					image[base + 3] = static_cast<unsigned char>((pixelIndex * 7 + 197) & 0xFF);
				}
			}
			return image;
		}

		TEST_METHOD(ChromaCorrectionSkippedAtZeroStrength)
		{
			const int width = 16;
			const int height = 16;
			const int bitDepth = Constants::RGB24PixelSize;
			std::vector<unsigned char> input(static_cast<size_t>(width * height * bitDepth));
			for (size_t i = 0; i < input.size(); ++i)
			{
				input[i] = static_cast<unsigned char>((i * 37U) % 251U);
			}

			std::vector<unsigned char> output(input.size(), 0);
			Assert::IsTrue(ProcessMultiscaleImage(input.data(), output.data(), width, height, bitDepth,
				CoreTests::MakeProcessingState(0, 60, 40, 100)));
			Assert::IsTrue(memcmp(input.data(), output.data(), input.size()) == 0);
		}

		TEST_METHOD(ChromaProtectionAttenuatesLiftedFlatShadowsOnly)
		{
			const int width = 96;
			const int height = 96;
			const int bitDepth = Constants::RGB24PixelSize;
			const std::vector<unsigned char> input = MakeShadowTestImageRGB24(width, height, false);

			std::vector<unsigned char> withoutProtection(input.size(), 0);
			std::vector<unsigned char> withProtection(input.size(), 0);
			Assert::IsTrue(ProcessMultiscaleImage(input.data(), withoutProtection.data(), width, height,
				bitDepth, CoreTests::MakeProcessingState(45, 25, 25, 0)));
			Assert::IsTrue(ProcessMultiscaleImage(input.data(), withProtection.data(), width, height,
				bitDepth, CoreTests::MakeProcessingState(45, 25, 25, 50)));

			// Bright half must stay byte-identical: its darkness risk is zero, so
			// the attenuation is an exact no-op there. The half is per-column, so
			// compare the right-hand segment of every row.
			const int rowBytes = width * bitDepth;
			const int brightStart = (width / 2) * bitDepth;
			for (int y = 0; y < height; ++y)
			{
				const size_t rowOffset = static_cast<size_t>(y) * rowBytes + brightStart;
				Assert::IsTrue(memcmp(withoutProtection.data() + rowOffset,
					withProtection.data() + rowOffset, rowBytes - brightStart) == 0);
			}

			// Flat, strongly lifted, originally dark left half must lose chroma.
			const double before = SumChromaDistance(withoutProtection, width, height, bitDepth, 0, width / 2 - 2);
			const double after = SumChromaDistance(withProtection, width, height, bitDepth, 0, width / 2 - 2);
			Assert::IsTrue(before > 0.0);
			Assert::IsTrue(after < before * 0.95, L"flat shadow chroma was not attenuated");
		}

		TEST_METHOD(ChromaProtectionAffectsTexturedShadowsLessThanFlatOnes)
		{
			const int width = 96;
			const int height = 96;
			const int bitDepth = Constants::RGB24PixelSize;
			const std::vector<unsigned char> input = MakeShadowTestImageRGB24(width, height, true);

			std::vector<unsigned char> withoutProtection(input.size(), 0);
			std::vector<unsigned char> withProtection(input.size(), 0);
			Assert::IsTrue(ProcessMultiscaleImage(input.data(), withoutProtection.data(), width, height,
				bitDepth, CoreTests::MakeProcessingState(45, 25, 25, 0)));
			Assert::IsTrue(ProcessMultiscaleImage(input.data(), withProtection.data(), width, height,
				bitDepth, CoreTests::MakeProcessingState(45, 25, 25, 100)));

			const double flatBefore = SumChromaDistance(withoutProtection, width, height, bitDepth, 2, width / 2 - 2);
			const double flatAfter = SumChromaDistance(withProtection, width, height, bitDepth, 2, width / 2 - 2);
			const double texturedBefore = SumChromaDistance(withoutProtection, width, height, bitDepth, width / 2 + 2, width - 2);
			const double texturedAfter = SumChromaDistance(withProtection, width, height, bitDepth, width / 2 + 2, width - 2);

			Assert::IsTrue(flatBefore > 0.0);
			Assert::IsTrue(texturedBefore > 0.0);
			const double flatReduction = 1.0 - (flatAfter / flatBefore);
			const double texturedReduction = 1.0 - (texturedAfter / texturedBefore);
			Assert::IsTrue(flatReduction > texturedReduction,
				L"flat shadows must lose more chroma than textured ones");
			Assert::IsTrue(flatReduction > 0.05, L"flat shadow chroma was not attenuated");
			Assert::IsTrue(texturedReduction < flatReduction * 0.9,
				L"texture floor did not protect textured shadows");
		}

		TEST_METHOD(GainRiskLutIsMonotonicInGainAndDarkness)
		{
			// More lift means more risk for a dark pixel.
			int previous = ComputeGainRiskQ8(4, 0);
			for (int enhanced = 1; enhanced < 256; ++enhanced)
			{
				const int current = ComputeGainRiskQ8(4, enhanced);
				Assert::IsTrue(current >= previous);
				previous = current;
			}
			Assert::AreEqual(255, ComputeGainRiskQ8(0, 255));

			// Brighter originals mean less risk, vanishing at the darkness band.
			previous = ComputeGainRiskQ8(0, 255);
			for (int original = 1; original < 256; ++original)
			{
				const int current = ComputeGainRiskQ8(original, 255);
				Assert::IsTrue(current <= previous);
				previous = current;
			}
			Assert::AreEqual(0, ComputeGainRiskQ8(64, 255));

			// No lift, no risk: identical luma and a sub-half-stop lift both stay
			// at zero (the +1 epsilon makes (0, 1) a full stop, which is risky).
			Assert::AreEqual(0, ComputeGainRiskQ8(100, 100));
			Assert::AreEqual(0, ComputeGainRiskQ8(50, 52));
			Assert::AreEqual(0, ComputeGainRiskQ8(0, 0));
		}

		TEST_METHOD(ActivityRiskLutFavorsFlatAreas)
		{
			Assert::AreEqual(255, ComputeActivityRiskQ8(0));
			Assert::AreEqual(255, ComputeActivityRiskQ8(1));
			Assert::AreEqual(0, ComputeActivityRiskQ8(6));
			Assert::AreEqual(0, ComputeActivityRiskQ8(255));

			for (int activity = 1; activity < 6; ++activity)
			{
				Assert::IsTrue(ComputeActivityRiskQ8(activity) >= ComputeActivityRiskQ8(activity + 1));
			}
		}

		TEST_METHOD(LocalActivityAndBlurKernelsBehave)
		{
			const int width = 5;
			const int height = 5;
			const int pixelCount = width * height;
			std::vector<unsigned char> flat(pixelCount, 42);
			std::vector<unsigned char> activity(pixelCount, 0);
			AltaLuxKernels::ComputeLocalActivity3x3(flat.data(), activity.data(), width, height,
				AltaLuxKernels::KernelImplementation::Scalar);
			for (int i = 0; i < pixelCount; ++i)
			{
				Assert::AreEqual(0, static_cast<int>(activity[i]));
			}

			std::vector<unsigned char> spike(pixelCount, 0);
			spike[(pixelCount - 1) / 2] = 255;
			AltaLuxKernels::ComputeLocalActivity3x3(spike.data(), activity.data(), width, height,
				AltaLuxKernels::KernelImplementation::Scalar);
			Assert::IsTrue(activity[(pixelCount - 1) / 2] == 255);
			Assert::IsTrue(activity[0] < activity[(pixelCount - 1) / 2]);

			// Blur of a constant map is the same constant...
			std::vector<unsigned char> constant(pixelCount, 77);
			std::vector<unsigned char> temp(pixelCount, 0);
			AltaLuxKernels::BlurRiskMap(constant.data(), temp.data(), width, height,
				AltaLuxKernels::KernelImplementation::Scalar);
			for (int i = 0; i < pixelCount; ++i)
			{
				Assert::AreEqual(77, static_cast<int>(constant[i]));
			}

			// ...and a spike bleeds into its neighbors while losing height.
			std::vector<unsigned char> spikeRisk(pixelCount, 0);
			spikeRisk[(pixelCount - 1) / 2] = 255;
			AltaLuxKernels::BlurRiskMap(spikeRisk.data(), temp.data(), width, height,
				AltaLuxKernels::KernelImplementation::Scalar);
			const int center = (pixelCount - 1) / 2;
			Assert::IsTrue(spikeRisk[center] > 0 && spikeRisk[center] < 255);
			Assert::IsTrue(spikeRisk[center + 1] > 0);
			Assert::IsTrue(spikeRisk[center - 1] > 0);
		}

		static void AssertChromaKernelsMatchScalar(AltaLuxKernels::KernelImplementation implementation)
		{
			if (!AltaLuxKernels::IsImplementationSupported(implementation))
			{
				Logger::WriteMessage(L"Requested SIMD kernels are not supported on this CPU; skipping chroma equality checks.");
				return;
			}

			// Odd dimensions exercise the SIMD tails and the scalar fallbacks.
			const int width = 13;
			const int height = 7;
			const int pixelCount = width * height;

			// The risk-combination kernel is scalar-only (its 64K-entry table
			// lookup measured slower with AVX2 gathers), so the parity surface is
			// the attenuation kernel; a deterministic risk plane drives it.
			std::vector<unsigned char> enhancedLuma(pixelCount);
			for (int i = 0; i < pixelCount; ++i)
			{
				enhancedLuma[i] = static_cast<unsigned char>((i * 13 + 5) & 0xFF);
			}

			std::vector<unsigned char> riskScalar(pixelCount);
			for (int i = 0; i < pixelCount; ++i)
			{
				riskScalar[i] = static_cast<unsigned char>((i * 3) & 0xFF);
			}

			for (int bitDepth = Constants::RGB24PixelSize; bitDepth <= Constants::RGB32PixelSize; ++bitDepth)
			{
				const std::vector<unsigned char> baseImage = MakePatternImageForChroma(width, height, bitDepth);
				std::vector<unsigned char> scalarImage = baseImage;
				std::vector<unsigned char> simdImage = baseImage;

				AltaLuxKernels::ApplyChromaAttenuation(scalarImage.data(), enhancedLuma.data(), riskScalar.data(),
					0, pixelCount, bitDepth, 100, AltaLuxKernels::KernelImplementation::Scalar);
				AltaLuxKernels::ApplyChromaAttenuation(simdImage.data(), enhancedLuma.data(), riskScalar.data(),
					0, pixelCount, bitDepth, 100, implementation);
				Assert::IsTrue(memcmp(scalarImage.data(), simdImage.data(), scalarImage.size()) == 0);

				// Sub-range to mirror the parallel block split; pixels outside
				// the range stay untouched.
				std::vector<unsigned char> partialScalar = baseImage;
				std::vector<unsigned char> partialSimd = baseImage;
				AltaLuxKernels::ApplyChromaAttenuation(partialScalar.data(), enhancedLuma.data(), riskScalar.data(),
					10, 81, bitDepth, 100, AltaLuxKernels::KernelImplementation::Scalar);
				AltaLuxKernels::ApplyChromaAttenuation(partialSimd.data(), enhancedLuma.data(), riskScalar.data(),
					10, 81, bitDepth, 100, implementation);
				Assert::IsTrue(memcmp(partialScalar.data(), partialSimd.data(), partialScalar.size()) == 0);
				Assert::IsTrue(memcmp(baseImage.data(), partialScalar.data(), 10 * bitDepth) == 0);
				Assert::IsTrue(memcmp(baseImage.data() + (81 * bitDepth),
					partialScalar.data() + (81 * bitDepth), baseImage.size() - (81 * bitDepth)) == 0);
			}

			// The row-vectorized activity and blur kernels must match scalar
			// byte-for-byte across the SIMD bodies, the scalar tails and the
			// clamped edges; the width is not a multiple of the vector size so
			// every tail runs, and the odd height exercises row clamping.
			const int stencilWidth = 100;
			const int stencilHeight = 37;
			const int stencilPixels = stencilWidth * stencilHeight;
			std::vector<unsigned char> lumaPlane(stencilPixels);
			for (int i = 0; i < stencilPixels; ++i)
			{
				lumaPlane[i] = static_cast<unsigned char>((i * 37 + 11) & 0xFF);
			}
			std::vector<unsigned char> scalarActivity(stencilPixels);
			std::vector<unsigned char> simdActivity(stencilPixels);
			AltaLuxKernels::ComputeLocalActivity3x3(lumaPlane.data(), scalarActivity.data(),
				stencilWidth, stencilHeight, AltaLuxKernels::KernelImplementation::Scalar);
			AltaLuxKernels::ComputeLocalActivity3x3(lumaPlane.data(), simdActivity.data(),
				stencilWidth, stencilHeight, implementation);
			Assert::IsTrue(memcmp(scalarActivity.data(), simdActivity.data(), simdActivity.size()) == 0);

			std::vector<unsigned char> scalarRisk(simdActivity);
			std::vector<unsigned char> simdRisk(simdActivity);
			std::vector<unsigned char> blurScratch(stencilPixels);
			AltaLuxKernels::BlurRiskMap(scalarRisk.data(), blurScratch.data(), stencilWidth, stencilHeight,
				AltaLuxKernels::KernelImplementation::Scalar);
			AltaLuxKernels::BlurRiskMap(simdRisk.data(), blurScratch.data(), stencilWidth, stencilHeight,
				implementation);
			Assert::IsTrue(memcmp(scalarRisk.data(), simdRisk.data(), simdRisk.size()) == 0);

			// End-to-end parity through the full pipeline with the stage active.
			const int pipelineWidth = 64;
			const int pipelineHeight = 48;
			const UiState state = CoreTests::MakeProcessingState(45, 25, 25, 50);
			for (int bitDepth = Constants::RGB24PixelSize; bitDepth <= Constants::RGB32PixelSize; ++bitDepth)
			{
				const std::vector<unsigned char> pattern = MakePatternImageForChroma(pipelineWidth, pipelineHeight, bitDepth);
				std::vector<unsigned char> scalarOutput(pattern.size(), 0);
				std::vector<unsigned char> simdOutput(pattern.size(), 0);
				Assert::IsTrue(ProcessMultiscaleImageWithKernels(pattern.data(), scalarOutput.data(),
					pipelineWidth, pipelineHeight, bitDepth, state, AltaLuxKernels::KernelImplementation::Scalar));
				Assert::IsTrue(ProcessMultiscaleImageWithKernels(pattern.data(), simdOutput.data(),
					pipelineWidth, pipelineHeight, bitDepth, state, implementation));
				Assert::IsTrue(memcmp(scalarOutput.data(), simdOutput.data(), scalarOutput.size()) == 0);
			}
		}

		TEST_METHOD(SSSE3ChromaKernelsMatchScalar)
		{
			AssertChromaKernelsMatchScalar(AltaLuxKernels::KernelImplementation::SSSE3);
		}

		TEST_METHOD(AVX2ChromaKernelsMatchScalar)
		{
			AssertChromaKernelsMatchScalar(AltaLuxKernels::KernelImplementation::AVX2);
		}

		TEST_METHOD(ChromaProtectionPreservesAlphaAndWorksInPlace)
		{
			const int width = 24;
			const int height = 16;
			const int bitDepth = Constants::RGB32PixelSize;
			std::vector<unsigned char> inPlace = MakePatternImageForChroma(width, height, bitDepth);
			Assert::IsTrue(ProcessMultiscaleImage(inPlace.data(), inPlace.data(), width, height, bitDepth,
				CoreTests::MakeProcessingState(45, 25, 25, 50)));

			const std::vector<unsigned char> original = MakePatternImageForChroma(width, height, bitDepth);
			for (int pixelIndex = 0; pixelIndex < width * height; ++pixelIndex)
			{
				const int alphaOffset = (pixelIndex * bitDepth) + 3;
				Assert::AreEqual(static_cast<int>(original[alphaOffset]), static_cast<int>(inPlace[alphaOffset]));
			}
		}

		TEST_METHOD(ChromaProtectionMatchesAcrossBitDepths)
		{
			const int width = 24;
			const int height = 16;
			const std::vector<unsigned char> rgb24 = MakePatternImageForChroma(width, height, Constants::RGB24PixelSize);
			std::vector<unsigned char> rgb24WithAlpha;
			rgb24WithAlpha.reserve(rgb24.size() / Constants::RGB24PixelSize * Constants::RGB32PixelSize);
			for (int pixelIndex = 0; pixelIndex < width * height; ++pixelIndex)
			{
				const int srcBase = pixelIndex * Constants::RGB24PixelSize;
				rgb24WithAlpha.push_back(rgb24[srcBase]);
				rgb24WithAlpha.push_back(rgb24[srcBase + 1]);
				rgb24WithAlpha.push_back(rgb24[srcBase + 2]);
				rgb24WithAlpha.push_back(197);
			}

			std::vector<unsigned char> out24(rgb24.size(), 0);
			std::vector<unsigned char> out32(rgb24WithAlpha.size(), 0);
			const UiState state = CoreTests::MakeProcessingState(45, 25, 25, 50);
			Assert::IsTrue(ProcessMultiscaleImage(rgb24.data(), out24.data(), width, height,
				Constants::RGB24PixelSize, state));
			Assert::IsTrue(ProcessMultiscaleImage(rgb24WithAlpha.data(), out32.data(), width, height,
				Constants::RGB32PixelSize, state));

			for (int pixelIndex = 0; pixelIndex < width * height; ++pixelIndex)
			{
				const int base24 = pixelIndex * Constants::RGB24PixelSize;
				const int base32 = pixelIndex * Constants::RGB32PixelSize;
				Assert::AreEqual(static_cast<int>(out24[base24]), static_cast<int>(out32[base32]));
				Assert::AreEqual(static_cast<int>(out24[base24 + 1]), static_cast<int>(out32[base32 + 1]));
				Assert::AreEqual(static_cast<int>(out24[base24 + 2]), static_cast<int>(out32[base32 + 2]));
				Assert::AreEqual(197, static_cast<int>(out32[base32 + 3]));
			}
		}
	};
}
