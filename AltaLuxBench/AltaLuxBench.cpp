// AltaLux Filter
// by Stefano Tommesani (www.tommesani.com) 2016
// this code is release under the Code Project Open License (CPOL) http://www.codeproject.com/info/cpol10.aspx
// The main points subject to the terms of the License are:
// -   Source Code and Executable Files can be used in commercial applications;
// -   Source Code and Executable Files can be redistributed; and
// -   Source Code can be modified to create derivative works.
// -   No claim of suitability, guarantee, or any warranty whatsoever is provided. The software is provided "as-is".
// -   The Article(s) accompanying the Work may not be distributed or republished without the Author's consent

// AltaLuxBench.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"

#include <iostream>
#include <vector>
#include <algorithm>
#include <ctime>
#include <iomanip>
#include <memory>
#include <string>

#include <Windows.h>

#include <CAltaLuxFilterFactory.h>
#include "..\AltaLux\Kernels\AltaLuxKernels.h"

using namespace std;

// using 4K resolution for testing
const int SAMPLE_WIDTH = 3840;
const int SAMPLE_HEIGHT = 2160;
const int SAMPLE_PIXELS = SAMPLE_WIDTH * SAMPLE_HEIGHT;
const int GRAY_SAMPLE_SIZE = SAMPLE_PIXELS;
const int RGB24_SAMPLE_SIZE = SAMPLE_PIXELS * 3;
const int RGB32_SAMPLE_SIZE = SAMPLE_PIXELS * 4;
const int PACKED_YUV_SAMPLE_SIZE = SAMPLE_PIXELS * 2;
const int ACCUM_SAMPLE_SIZE = SAMPLE_PIXELS * 3;
const int BENCHMARK_SAMPLES = 10;
const int HISTOGRAM_CLIP_BENCHMARK_BATCH = 1024;
const int HISTOGRAM_MAP_BENCHMARK_BATCH = 16384;

const int SCALING_LOG = 15;
const int SCALING_FACTOR = (1 << SCALING_LOG);
const int Y_RED_SCALE = static_cast<int>(0.2126 * SCALING_FACTOR);
const int Y_GREEN_SCALE = static_cast<int>(0.7152 * SCALING_FACTOR);
const int Y_BLUE_SCALE = static_cast<int>(0.0722 * SCALING_FACTOR);
const int WEIGHT_SCALE_LOG2 = 10;
const int WEIGHT_HALF = 1 << (WEIGHT_SCALE_LOG2 - 1);

unsigned char *ReferenceBuffer = nullptr;
unsigned char *InputBuffer = nullptr;

void FillRandomBuffer(unsigned char *Buffer, int BufferSize)
{
	srand(0x5555);
	for (int j = 0; j < BufferSize; j++)
		Buffer[j] = rand() & 0xFF;
}

int ElapsedTimeToMSec(int ElapsedTime)
{
	LARGE_INTEGER TimerFrequency;
	LARGE_INTEGER LargeElapsedTime;
	LargeElapsedTime.HighPart = 0;
	LargeElapsedTime.LowPart = ElapsedTime;
	QueryPerformanceFrequency(&TimerFrequency);
	LargeElapsedTime.QuadPart = (LargeElapsedTime.QuadPart * 1000) / TimerFrequency.QuadPart;
	return LargeElapsedTime.LowPart;
}

void PrintSpeedup(int baselineTicks, int measuredTicks)
{
	const ios_base::fmtflags previousFlags = cout.flags();
	const streamsize previousPrecision = cout.precision();
	cout << " [" << fixed << setprecision(2)
		<< (static_cast<double>(baselineTicks) / static_cast<double>(measuredTicks))
		<< "x vs Scalar]";
	cout.flags(previousFlags);
	cout.precision(previousPrecision);
}

int PrintBenchmarkResults(vector<int>& BenchmarkSamples, int baselineTicks = 0)
{
	sort(BenchmarkSamples.begin(), BenchmarkSamples.end());
	const int medianTicks = BenchmarkSamples[BenchmarkSamples.size() >> 1];
	cout << ElapsedTimeToMSec(medianTicks) << " ms";
	if (baselineTicks > 0)
		PrintSpeedup(baselineTicks, medianTicks);
	cout << " (";
	for (auto item = BenchmarkSamples.begin(); item != BenchmarkSamples.end(); ++item)
		cout << ElapsedTimeToMSec(*item) << "ms  ";
	cout << ")" << endl;
	return medianTicks;
}

template<typename Fn>
int BenchmarkOperation(const char *BenchmarkName, Fn operation, int baselineTicks = 0)
{
	vector<int> BenchmarkSamples;
	for (int iteration = 0; iteration < BENCHMARK_SAMPLES; iteration++)
	{
		LARGE_INTEGER StartTime, StopTime;
		QueryPerformanceCounter(&StartTime);
		operation();
		QueryPerformanceCounter(&StopTime);
		BenchmarkSamples.push_back(static_cast<int>(StopTime.QuadPart - StartTime.QuadPart));
	}

	cout << BenchmarkName << endl;
	return PrintBenchmarkResults(BenchmarkSamples, baselineTicks);
}

template<typename Fn>
void BenchmarkImplementationGroup(const char *BenchmarkName, Fn operation)
{
	const AltaLuxKernels::KernelImplementation implementations[] =
	{
		AltaLuxKernels::KernelImplementation::Scalar,
		AltaLuxKernels::KernelImplementation::SSSE3,
		AltaLuxKernels::KernelImplementation::AVX2
	};
	const int implementationCount = 3;
	vector<int> samples[implementationCount];
	bool supported[implementationCount] = {};

	for (int i = 0; i < implementationCount; ++i)
	{
		supported[i] = AltaLuxKernels::IsImplementationSupported(implementations[i]);
		if (!supported[i])
		{
			cout << BenchmarkName << " " << AltaLuxKernels::GetImplementationName(implementations[i])
				<< " skipped (unsupported)" << endl;
		}
	}

	for (int iteration = 0; iteration < BENCHMARK_SAMPLES; ++iteration)
	{
		for (int slot = 0; slot < implementationCount; ++slot)
		{
			const int implementationIndex = (slot + iteration) % implementationCount;
			if (!supported[implementationIndex])
			{
				continue;
			}

			LARGE_INTEGER StartTime, StopTime;
			QueryPerformanceCounter(&StartTime);
			operation(implementations[implementationIndex]);
			QueryPerformanceCounter(&StopTime);
			samples[implementationIndex].push_back(static_cast<int>(StopTime.QuadPart - StartTime.QuadPart));
		}
	}

	int scalarTicks = 0;
	for (int i = 0; i < implementationCount; ++i)
	{
		if (!supported[i])
		{
			continue;
		}

		string label = string(BenchmarkName) + " " + AltaLuxKernels::GetImplementationName(implementations[i]);
		cout << label << endl;
		const int medianTicks = PrintBenchmarkResults(samples[i],
			(implementations[i] == AltaLuxKernels::KernelImplementation::Scalar) ? 0 : scalarTicks);
		if (implementations[i] == AltaLuxKernels::KernelImplementation::Scalar)
		{
			scalarTicks = medianTicks;
		}
	}
}

void BenchmarkFilter(CBaseAltaLuxFilter *Filter, const char *FilterName)
{
	// always use the same InputBuffer for all tests to avoid differences due to mem alignment
	BenchmarkOperation(FilterName, [&]()
	{
		memcpy(InputBuffer, ReferenceBuffer, GRAY_SAMPLE_SIZE);
		Filter->ProcessGray(InputBuffer);
	});
}

void BenchmarkFilterImplementations(const vector<unsigned char>& reference, const char *FilterName, int filterType,
	int (CBaseAltaLuxFilter::*processMethod)(void*))
{
	const AltaLuxKernels::KernelImplementation implementations[] =
	{
		AltaLuxKernels::KernelImplementation::Scalar,
		AltaLuxKernels::KernelImplementation::SSSE3,
		AltaLuxKernels::KernelImplementation::AVX2
	};
	vector<unsigned char> inputs[3];
	unique_ptr<CBaseAltaLuxFilter> filters[3];
	for (int i = 0; i < 3; ++i)
	{
		if (!AltaLuxKernels::IsImplementationSupported(implementations[i]))
		{
			continue;
		}

		inputs[i].resize(reference.size());
		filters[i].reset(CAltaLuxFilterFactory::CreateSpecificAltaLuxFilter(
			filterType, SAMPLE_WIDTH, SAMPLE_HEIGHT));
		filters[i]->SetStrength(45);
		filters[i]->SetKernelImplementation(implementations[i]);
	}

	BenchmarkImplementationGroup(FilterName, [&](AltaLuxKernels::KernelImplementation implementation)
	{
		const int index = (implementation == AltaLuxKernels::KernelImplementation::Scalar) ? 0 :
			((implementation == AltaLuxKernels::KernelImplementation::SSSE3) ? 1 : 2);
		memcpy(inputs[index].data(), reference.data(), reference.size());
		(filters[index].get()->*processMethod)(inputs[index].data());
	});
}

template<typename Fn>
void BenchmarkAllImplementations(const char *KernelName, Fn operation)
{
	BenchmarkImplementationGroup(KernelName, [&](AltaLuxKernels::KernelImplementation implementation)
	{
		operation(implementation);
	});
}

void BenchmarkCriticalKernels()
{
	vector<unsigned char> rgb24(RGB24_SAMPLE_SIZE);
	vector<unsigned char> rgb32(RGB32_SAMPLE_SIZE);
	vector<unsigned char> packedYUV(PACKED_YUV_SAMPLE_SIZE);
	vector<unsigned char> luma(SAMPLE_PIXELS);
	vector<unsigned char> target(RGB32_SAMPLE_SIZE);
	vector<unsigned char> packedYUVTarget(PACKED_YUV_SAMPLE_SIZE);
	vector<unsigned char> downscaledRGB24((SAMPLE_WIDTH / 2) * (SAMPLE_HEIGHT / 2) * 3);
	vector<unsigned char> downscaledRGB32((SAMPLE_WIDTH / 2) * (SAMPLE_HEIGHT / 2) * 4);
	vector<unsigned char> interpolationSource(SAMPLE_PIXELS);
	vector<unsigned char> interpolationTarget(SAMPLE_PIXELS);
	vector<unsigned int> accum(ACCUM_SAMPLE_SIZE);
	vector<unsigned int> mapLeftUp(256);
	vector<unsigned int> mapRightUp(256);
	vector<unsigned int> mapLeftBottom(256);
	vector<unsigned int> mapRightBottom(256);
	vector<unsigned int> histogram(256);
	vector<unsigned int> clipHistogramBatch(HISTOGRAM_CLIP_BENCHMARK_BATCH * 256);
	vector<unsigned int> clipHistogramBatchSource(HISTOGRAM_CLIP_BENCHMARK_BATCH * 256);
	vector<unsigned int> mapHistogramBatch(HISTOGRAM_MAP_BENCHMARK_BATCH * 256);
	vector<unsigned int> mapHistogramBatchSource(HISTOGRAM_MAP_BENCHMARK_BATCH * 256);
	int reciprocalLut[256] = {};

	FillRandomBuffer(rgb24.data(), static_cast<int>(rgb24.size()));
	FillRandomBuffer(rgb32.data(), static_cast<int>(rgb32.size()));
	FillRandomBuffer(packedYUV.data(), static_cast<int>(packedYUV.size()));
	FillRandomBuffer(luma.data(), static_cast<int>(luma.size()));
	FillRandomBuffer(target.data(), static_cast<int>(target.size()));
	FillRandomBuffer(interpolationSource.data(), static_cast<int>(interpolationSource.size()));
	for (int i = 1; i < 256; ++i)
		reciprocalLut[i] = (1 << 16) / i;
	for (int i = 0; i < ACCUM_SAMPLE_SIZE; ++i)
		accum[i] = static_cast<unsigned int>((i * 37) & 0x3FFFF);
	for (int i = 0; i < 256; ++i)
	{
		mapLeftUp[i] = static_cast<unsigned int>((i * 17 + 3) & 0xFF);
		mapRightUp[i] = static_cast<unsigned int>((i * 19 + 29) & 0xFF);
		mapLeftBottom[i] = static_cast<unsigned int>((i * 23 + 47) & 0xFF);
		mapRightBottom[i] = static_cast<unsigned int>((i * 31 + 71) & 0xFF);
	}
	for (size_t i = 0; i < clipHistogramBatchSource.size(); ++i)
		clipHistogramBatchSource[i] = static_cast<unsigned int>(384 + ((i * 37) & 0x1FF));
	for (size_t i = 0; i < mapHistogramBatchSource.size(); ++i)
		mapHistogramBatchSource[i] = static_cast<unsigned int>(384 + ((i * 53) & 0x1FF));

	cout << endl << "Critical kernel implementations" << endl;
	BenchmarkAllImplementations("RGB24 Extract Luma", [&](AltaLuxKernels::KernelImplementation implementation)
	{
		AltaLuxKernels::ExtractRGBLuma(rgb24.data(), luma.data(), SAMPLE_PIXELS, 3,
			Y_RED_SCALE, Y_GREEN_SCALE, Y_BLUE_SCALE, SCALING_LOG, implementation);
	});
	BenchmarkAllImplementations("RGB32 Inject Luma", [&](AltaLuxKernels::KernelImplementation implementation)
	{
		memcpy(target.data(), rgb32.data(), target.size());
		AltaLuxKernels::InjectRGBLuma(target.data(), luma.data(), SAMPLE_PIXELS, 4,
			Y_RED_SCALE, Y_GREEN_SCALE, Y_BLUE_SCALE, SCALING_LOG, reciprocalLut, implementation);
	});
	BenchmarkAllImplementations("RGB24 Scale Down Box 2x", [&](AltaLuxKernels::KernelImplementation implementation)
	{
		AltaLuxKernels::ScaleDownBox(rgb24.data(), SAMPLE_WIDTH, SAMPLE_HEIGHT,
			downscaledRGB24.data(), 2, 3, implementation);
	});
	BenchmarkAllImplementations("RGB32 Scale Down Box 2x", [&](AltaLuxKernels::KernelImplementation implementation)
	{
		AltaLuxKernels::ScaleDownBox(rgb32.data(), SAMPLE_WIDTH, SAMPLE_HEIGHT,
			downscaledRGB32.data(), 2, 4, implementation);
	});
	BenchmarkAllImplementations("Packed YUV Extract Luma", [&](AltaLuxKernels::KernelImplementation implementation)
	{
		AltaLuxKernels::ExtractPackedYUVLuma(packedYUV.data(), luma.data(), SAMPLE_PIXELS,
			AltaLuxKernels::PackedYUVLumaPosition::HighByte, implementation);
	});
	BenchmarkAllImplementations("Packed YUV Inject Luma", [&](AltaLuxKernels::KernelImplementation implementation)
	{
		memcpy(packedYUVTarget.data(), packedYUV.data(), packedYUVTarget.size());
		AltaLuxKernels::InjectPackedYUVLuma(packedYUVTarget.data(), luma.data(), SAMPLE_PIXELS,
			AltaLuxKernels::PackedYUVLumaPosition::HighByte, implementation);
	});
	BenchmarkAllImplementations("Multiscale Accumulate Layer", [&](AltaLuxKernels::KernelImplementation implementation)
	{
		AltaLuxKernels::AccumulateLayer(accum.data(), rgb32.data(), 0, SAMPLE_PIXELS, 4,
			341, false, implementation);
	});
	BenchmarkAllImplementations("Multiscale Write Image", [&](AltaLuxKernels::KernelImplementation implementation)
	{
		AltaLuxKernels::WriteAccumulatedImage(target.data(), accum.data(), 0, SAMPLE_PIXELS, 4,
			WEIGHT_SCALE_LOG2, WEIGHT_HALF, implementation);
	});
	BenchmarkAllImplementations("CLAHE Make Histogram", [&](AltaLuxKernels::KernelImplementation implementation)
	{
		const int tileWidth = SAMPLE_WIDTH / 8;
		const int tileHeight = SAMPLE_HEIGHT / 8;
		for (int tileY = 0; tileY < 8; ++tileY)
		{
			for (int tileX = 0; tileX < 8; ++tileX)
			{
				const unsigned char* tile = interpolationSource.data()
					+ (tileY * tileHeight * SAMPLE_WIDTH)
					+ (tileX * tileWidth);
				AltaLuxKernels::MakeHistogram(tile, SAMPLE_WIDTH, tileWidth, tileHeight,
					histogram.data(), implementation);
			}
		}
	});
	BenchmarkAllImplementations("CLAHE Clip Histogram", [&](AltaLuxKernels::KernelImplementation implementation)
	{
		memcpy(clipHistogramBatch.data(), clipHistogramBatchSource.data(),
			clipHistogramBatch.size() * sizeof(unsigned int));
		for (size_t offset = 0; offset < clipHistogramBatch.size(); offset += 256)
		{
			AltaLuxKernels::ClipHistogram(clipHistogramBatch.data() + offset, 700, implementation);
		}
	});
	BenchmarkAllImplementations("CLAHE Map Histogram", [&](AltaLuxKernels::KernelImplementation implementation)
	{
		memcpy(mapHistogramBatch.data(), mapHistogramBatchSource.data(),
			mapHistogramBatch.size() * sizeof(unsigned int));
		for (size_t offset = 0; offset < mapHistogramBatch.size(); offset += 256)
		{
			AltaLuxKernels::MapHistogram(mapHistogramBatch.data() + offset,
				(SAMPLE_WIDTH / 8) * (SAMPLE_HEIGHT / 8), implementation);
		}
	});
	BenchmarkAllImplementations("CLAHE Interpolate", [&](AltaLuxKernels::KernelImplementation implementation)
	{
		const unsigned int tileWidth = SAMPLE_WIDTH / 8;
		const unsigned int tileHeight = SAMPLE_HEIGHT / 8;
		memcpy(interpolationTarget.data(), interpolationSource.data(), interpolationTarget.size());
		for (int tileY = 0; tileY < 8; ++tileY)
		{
			for (int tileX = 0; tileX < 8; ++tileX)
			{
				unsigned char* tile = interpolationTarget.data()
					+ (tileY * tileHeight * SAMPLE_WIDTH)
					+ (tileX * tileWidth);
				AltaLuxKernels::Interpolate(tile, SAMPLE_WIDTH,
					mapLeftUp.data(), mapRightUp.data(), mapLeftBottom.data(), mapRightBottom.data(),
					tileWidth, tileHeight, implementation);
			}
		}
	});

	cout << endl << "Serial filter implementation comparison" << endl;
	BenchmarkFilterImplementations(packedYUV, "Serial UYVY", ALTALUX_FILTER_SERIAL, &CBaseAltaLuxFilter::ProcessUYVY);
	BenchmarkFilterImplementations(rgb24, "Serial BGR24", ALTALUX_FILTER_SERIAL, &CBaseAltaLuxFilter::ProcessBGR24);
	BenchmarkFilterImplementations(rgb32, "Serial BGR32", ALTALUX_FILTER_SERIAL, &CBaseAltaLuxFilter::ProcessBGR32);

	cout << endl << "Parallel Split Loop filter implementation comparison" << endl;
	BenchmarkFilterImplementations(packedYUV, "Parallel Split Loop UYVY", ALTALUX_FILTER_PARALLEL_SPLIT_LOOP, &CBaseAltaLuxFilter::ProcessUYVY);
	BenchmarkFilterImplementations(rgb24, "Parallel Split Loop BGR24", ALTALUX_FILTER_PARALLEL_SPLIT_LOOP, &CBaseAltaLuxFilter::ProcessBGR24);
	BenchmarkFilterImplementations(rgb32, "Parallel Split Loop BGR32", ALTALUX_FILTER_PARALLEL_SPLIT_LOOP, &CBaseAltaLuxFilter::ProcessBGR32);
}

int _tmain(int argc, _TCHAR* argv[])
{
	cout << "AltaLux Benchmark by Stefano Tommesani www.tommesani.com" << endl;
	// create image buffers
	ReferenceBuffer = new unsigned char[GRAY_SAMPLE_SIZE];
	FillRandomBuffer(ReferenceBuffer, GRAY_SAMPLE_SIZE);
	InputBuffer = new unsigned char[GRAY_SAMPLE_SIZE];

	BenchmarkCriticalKernels();

	cout << endl << "Filter strategy comparison (grayscale input)" << endl;
	CBaseAltaLuxFilter *SerialFilter = CAltaLuxFilterFactory::CreateSpecificAltaLuxFilter(ALTALUX_FILTER_SERIAL, SAMPLE_WIDTH, SAMPLE_HEIGHT);
	CBaseAltaLuxFilter *ParallelSplitLoopFilter = CAltaLuxFilterFactory::CreateSpecificAltaLuxFilter(ALTALUX_FILTER_PARALLEL_SPLIT_LOOP, SAMPLE_WIDTH, SAMPLE_HEIGHT);

	BenchmarkFilter(SerialFilter, "Serial");
	BenchmarkFilter(ParallelSplitLoopFilter, "Parallel Split Loop");

	delete SerialFilter;
	delete ParallelSplitLoopFilter;

	delete[] ReferenceBuffer;
	delete[] InputBuffer;
	cout << "Testing completed" << endl;
	char WaitForUser;
	cin >> WaitForUser;
	return 0;
}
