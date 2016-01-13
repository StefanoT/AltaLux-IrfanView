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

#include <Windows.h>

#include <CAltaLuxFilterFactory.h>

using namespace std;
// using 4K resolution for testing
const int SAMPLE_WIDTH = 3840;
const int SAMPLE_HEIGHT = 2160;
const int SAMPLE_SIZE = SAMPLE_WIDTH * SAMPLE_HEIGHT;
const int BENCHMARK_SAMPLES = 10;

unsigned char *ReferenceBuffer = nullptr;
unsigned char *InputBuffer = nullptr;

void FillRandomBuffer(unsigned char *Buffer, int BufferSize)
{
	srand(time(NULL));
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

void PrintBenchmarkResults(vector<int>& BenchmarkSamples)
{
	sort(BenchmarkSamples.begin(), BenchmarkSamples.end());
	cout << ElapsedTimeToMSec(BenchmarkSamples[BenchmarkSamples.size() >> 1]) << " ms (";
	for (auto item = BenchmarkSamples.begin(); item != BenchmarkSamples.end(); ++item)
		cout << ElapsedTimeToMSec(*item) << "ms  ";
	cout << ")" << endl;
}

void BenchmarkFilter(CBaseAltaLuxFilter *Filter, char *FilterName)
{
	// always use the same InputBuffer for all testes to avoid differences due to mem alignment
	vector<int> BenchmarkSamples;

	for (int iteration = 0; iteration < BENCHMARK_SAMPLES; iteration++)
	{
		memcpy(InputBuffer, ReferenceBuffer, SAMPLE_SIZE);
		LARGE_INTEGER StartTime, StopTime;
		QueryPerformanceCounter(&StartTime);
		Filter->ProcessGray(InputBuffer);
		QueryPerformanceCounter(&StopTime);
		int ElapsedTime = (int)(StopTime.QuadPart - StartTime.QuadPart);
		BenchmarkSamples.push_back(ElapsedTime);
	}

	cout << FilterName << endl;
	PrintBenchmarkResults(BenchmarkSamples);
}

int _tmain(int argc, _TCHAR* argv[])
{
	cout << "AltaLux Benchmark by Stefano Tommesani www.tommesani.com" << endl;	
	// create image buffers
	ReferenceBuffer = new unsigned char[SAMPLE_SIZE];
	FillRandomBuffer(ReferenceBuffer, SAMPLE_SIZE);
	InputBuffer = new unsigned char[SAMPLE_SIZE];

	// create filter instances
	CBaseAltaLuxFilter *SerialFilter = CAltaLuxFilterFactory::CreateSpecificAltaLuxFilter(ALTALUX_FILTER_SERIAL, SAMPLE_WIDTH, SAMPLE_HEIGHT);
	CBaseAltaLuxFilter *ParallelErrorFilter = CAltaLuxFilterFactory::CreateSpecificAltaLuxFilter(ALTALUX_FILTER_PARALLEL_ERROR, SAMPLE_WIDTH, SAMPLE_HEIGHT);
	CBaseAltaLuxFilter *ParallelSplitLoopFilter = CAltaLuxFilterFactory::CreateSpecificAltaLuxFilter(ALTALUX_FILTER_PARALLEL_SPLIT_LOOP, SAMPLE_WIDTH, SAMPLE_HEIGHT);
	CBaseAltaLuxFilter *ParallelEventFilter = CAltaLuxFilterFactory::CreateSpecificAltaLuxFilter(ALTALUX_FILTER_PARALLEL_EVENT, SAMPLE_WIDTH, SAMPLE_HEIGHT);
	CBaseAltaLuxFilter *ParallelActiveWaitFilter = CAltaLuxFilterFactory::CreateSpecificAltaLuxFilter(ALTALUX_FILTER_ACTIVE_WAIT, SAMPLE_WIDTH, SAMPLE_HEIGHT);

	BenchmarkFilter(SerialFilter, "Serial");
	BenchmarkFilter(ParallelErrorFilter, "Parallel Error");
	BenchmarkFilter(ParallelSplitLoopFilter, "Parallel Split Loop");
	BenchmarkFilter(ParallelEventFilter, "Parallel Event");
	BenchmarkFilter(ParallelActiveWaitFilter, "Parallel Active Wait");

	delete SerialFilter;
	delete ParallelErrorFilter;
	delete ParallelSplitLoopFilter;
	delete ParallelEventFilter;
	delete ParallelActiveWaitFilter;

	delete[] ReferenceBuffer;
	delete[] InputBuffer;
	cout << "Testing completed" << endl;
	char WaitForUser;
	cin >> WaitForUser;
	return 0;
}