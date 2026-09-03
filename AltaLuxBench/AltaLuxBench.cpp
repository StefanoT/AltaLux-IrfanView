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
const int BENCHMARK_SAMPLES = 15;
const int BENCHMARK_WARMUP_OPERATIONS = 3;
const int BENCHMARK_MIN_SAMPLE_MSEC = 100;
const int BENCHMARK_MAX_BATCH_OPERATIONS = 512;
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

LONGLONG GetTimerFrequency()
{
	LARGE_INTEGER TimerFrequency;
	QueryPerformanceFrequency(&TimerFrequency);
	return TimerFrequency.QuadPart;
}

double ElapsedTimeToMSec(double ElapsedTime)
{
	return (ElapsedTime * 1000.0) / static_cast<double>(GetTimerFrequency());
}

LONGLONG MsecToElapsedTicks(int Milliseconds)
{
	return (GetTimerFrequency() * Milliseconds) / 1000;
}

void PrintElapsedMSec(double elapsedTicks)
{
	const ios_base::fmtflags previousFlags = cout.flags();
	const streamsize previousPrecision = cout.precision();
	const double elapsedMSec = ElapsedTimeToMSec(elapsedTicks);
	cout << fixed << setprecision(elapsedMSec < 10.0 ? 3 : 1) << elapsedMSec << " ms";
	cout.flags(previousFlags);
	cout.precision(previousPrecision);
}

void PrintSpeedup(double baselineTicks, double measuredTicks)
{
	const ios_base::fmtflags previousFlags = cout.flags();
	const streamsize previousPrecision = cout.precision();
	cout << " [" << fixed << setprecision(2)
		<< (baselineTicks / measuredTicks)
		<< "x vs Scalar]";
	cout.flags(previousFlags);
	cout.precision(previousPrecision);
}

double BenchmarkSampleTicksPerOperation(LONGLONG sampleTicks, int repetitions)
{
	return static_cast<double>(sampleTicks) / static_cast<double>(repetitions);
}

double PrintBenchmarkResults(vector<LONGLONG>& BenchmarkSamples, int repetitions, double baselineTicks = 0.0)
{
	sort(BenchmarkSamples.begin(), BenchmarkSamples.end());
	const LONGLONG medianSampleTicks = BenchmarkSamples[BenchmarkSamples.size() >> 1];
	const double medianTicks = BenchmarkSampleTicksPerOperation(medianSampleTicks, repetitions);
	PrintElapsedMSec(medianTicks);
	if (baselineTicks > 0)
		PrintSpeedup(baselineTicks, medianTicks);
	cout << " per op, batch " << repetitions << "x: ";
	PrintElapsedMSec(static_cast<double>(medianSampleTicks));
	cout << " (";
	for (auto item = BenchmarkSamples.begin(); item != BenchmarkSamples.end(); ++item)
	{
		PrintElapsedMSec(BenchmarkSampleTicksPerOperation(*item, repetitions));
		cout << "  ";
	}
	cout << ")" << endl;
	return medianTicks;
}

template<typename Fn>
LONGLONG MeasureBenchmarkBatch(Fn operation, int repetitions)
{
	LARGE_INTEGER StartTime, StopTime;
	QueryPerformanceCounter(&StartTime);
	for (int i = 0; i < repetitions; ++i)
		operation();
	QueryPerformanceCounter(&StopTime);
	return StopTime.QuadPart - StartTime.QuadPart;
}

template<typename Fn>
void WarmupBenchmark(Fn operation)
{
	for (int i = 0; i < BENCHMARK_WARMUP_OPERATIONS; ++i)
		operation();
}

template<typename Fn>
int CalibrateBenchmarkBatchSize(Fn operation)
{
	int repetitions = 1;
	const LONGLONG minSampleTicks = MsecToElapsedTicks(BENCHMARK_MIN_SAMPLE_MSEC);
	while (repetitions < BENCHMARK_MAX_BATCH_OPERATIONS)
	{
		const LONGLONG elapsedTicks = MeasureBenchmarkBatch(operation, repetitions);
		if (elapsedTicks >= minSampleTicks)
			break;
		repetitions *= 2;
	}
	return repetitions;
}

template<typename Fn>
double BenchmarkOperation(const char *BenchmarkName, Fn operation, double baselineTicks = 0.0)
{
	WarmupBenchmark(operation);
	const int repetitions = CalibrateBenchmarkBatchSize(operation);

	vector<LONGLONG> BenchmarkSamples;
	for (int iteration = 0; iteration < BENCHMARK_SAMPLES; iteration++)
		BenchmarkSamples.push_back(MeasureBenchmarkBatch(operation, repetitions));

	cout << BenchmarkName << endl;
	return PrintBenchmarkResults(BenchmarkSamples, repetitions, baselineTicks);
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
	vector<LONGLONG> samples[implementationCount];
	int repetitions[implementationCount] = {};
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

	for (int i = 0; i < implementationCount; ++i)
	{
		if (!supported[i])
		{
			continue;
		}

		WarmupBenchmark([&]()
		{
			operation(implementations[i]);
		});
		repetitions[i] = CalibrateBenchmarkBatchSize([&]()
		{
			operation(implementations[i]);
		});
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

			samples[implementationIndex].push_back(MeasureBenchmarkBatch([&]()
			{
				operation(implementations[implementationIndex]);
			}, repetitions[implementationIndex]));
		}
	}

	double scalarTicks = 0.0;
	for (int i = 0; i < implementationCount; ++i)
	{
		if (!supported[i])
		{
			continue;
		}

		string label = string(BenchmarkName) + " " + AltaLuxKernels::GetImplementationName(implementations[i]);
		cout << label << endl;
		const double medianTicks = PrintBenchmarkResults(samples[i], repetitions[i],
			(implementations[i] == AltaLuxKernels::KernelImplementation::Scalar) ? 0.0 : scalarTicks);
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

template<typename Fn>
void BenchmarkScalarOnlyKernel(const char *KernelName, Fn operation)
{
	// Scalar-only kernels (histogram work, interpolation) run the same code in
	// every tier, so there is one timing instead of three identical ones.
	BenchmarkOperation((string(KernelName) + " Scalar (all tiers)").c_str(), operation);
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
	vector<unsigned char> activityPlane(SAMPLE_PIXELS);
	vector<unsigned char> riskPlane(SAMPLE_PIXELS);
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
	vector<unsigned int> gainRiskLut(256 * 256);
	vector<unsigned int> activityRiskLut(256);
	int reciprocalLut[256] = {};

	FillRandomBuffer(rgb24.data(), static_cast<int>(rgb24.size()));
	FillRandomBuffer(rgb32.data(), static_cast<int>(rgb32.size()));
	FillRandomBuffer(packedYUV.data(), static_cast<int>(packedYUV.size()));
	FillRandomBuffer(luma.data(), static_cast<int>(luma.size()));
	FillRandomBuffer(target.data(), static_cast<int>(target.size()));
	FillRandomBuffer(interpolationSource.data(), static_cast<int>(interpolationSource.size()));
	FillRandomBuffer(activityPlane.data(), static_cast<int>(activityPlane.size()));
	FillRandomBuffer(riskPlane.data(), static_cast<int>(riskPlane.size()));
	for (int i = 1; i < 256; ++i)
		reciprocalLut[i] = (1 << 16) / i;
	for (int i = 0; i < ACCUM_SAMPLE_SIZE; ++i)
		accum[i] = static_cast<unsigned int>((i * 37) & 0x3FFFF);
	for (size_t k = 0; k < gainRiskLut.size(); ++k)
		gainRiskLut[k] = static_cast<unsigned int>((k * 2654435761U) >> 24) & 0xFFU;
	for (size_t k = 0; k < activityRiskLut.size(); ++k)
		activityRiskLut[k] = static_cast<unsigned int>(((k * 97U) + 13U) & 0xFFU);
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
	BenchmarkScalarOnlyKernel("Chroma Compute Risk", [&]()
	{
		// Two luma planes from the same random source stand in for original and
		// enhanced luma; the risk map itself lands in the activity scratch plane
		// because only timing matters here.
		AltaLuxKernels::ComputeChromaRisk(luma.data(), luma.data(), activityPlane.data(),
			riskPlane.data(), SAMPLE_PIXELS, gainRiskLut.data(), activityRiskLut.data(), 77);
	});
	BenchmarkAllImplementations("RGB32 Chroma Attenuate", [&](AltaLuxKernels::KernelImplementation implementation)
	{
		memcpy(target.data(), rgb32.data(), target.size());
		AltaLuxKernels::ApplyChromaAttenuation(target.data(), luma.data(), riskPlane.data(),
			0, SAMPLE_PIXELS, 4, 64, implementation);
	});
	BenchmarkAllImplementations("RGB24 Chroma Attenuate", [&](AltaLuxKernels::KernelImplementation implementation)
	{
		memcpy(target.data(), rgb24.data(), rgb24.size());
		AltaLuxKernels::ApplyChromaAttenuation(target.data(), luma.data(), riskPlane.data(),
			0, SAMPLE_PIXELS, 3, 64, implementation);
	});
	BenchmarkScalarOnlyKernel("Chroma Activity 3x3", [&]()
	{
		AltaLuxKernels::ComputeLocalActivity3x3(luma.data(), activityPlane.data(),
			SAMPLE_WIDTH, SAMPLE_HEIGHT);
	});
	BenchmarkScalarOnlyKernel("Chroma Blur Risk Map", [&]()
	{
		AltaLuxKernels::BlurRiskMap(riskPlane.data(), activityPlane.data(),
			SAMPLE_WIDTH, SAMPLE_HEIGHT);
	});
	BenchmarkScalarOnlyKernel("CLAHE Make Histogram", [&]()
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
					histogram.data());
			}
		}
	});
	BenchmarkScalarOnlyKernel("CLAHE Clip Histogram", [&]()
	{
		memcpy(clipHistogramBatch.data(), clipHistogramBatchSource.data(),
			clipHistogramBatch.size() * sizeof(unsigned int));
		for (size_t offset = 0; offset < clipHistogramBatch.size(); offset += 256)
		{
			AltaLuxKernels::ClipHistogram(clipHistogramBatch.data() + offset, 700);
		}
	});
	BenchmarkScalarOnlyKernel("CLAHE Map Histogram", [&]()
	{
		memcpy(mapHistogramBatch.data(), mapHistogramBatchSource.data(),
			mapHistogramBatch.size() * sizeof(unsigned int));
		for (size_t offset = 0; offset < mapHistogramBatch.size(); offset += 256)
		{
			AltaLuxKernels::MapHistogram(mapHistogramBatch.data() + offset,
				(SAMPLE_WIDTH / 8) * (SAMPLE_HEIGHT / 8));
		}
	});
	BenchmarkScalarOnlyKernel("CLAHE Interpolate", [&]()
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
					tileWidth, tileHeight);
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
