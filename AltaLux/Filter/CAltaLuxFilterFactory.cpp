/*
Project: AltaLux plugin for IrfanView
Author: Stefano Tommesani
Website: http://www.tommesani.com

Microsoft Public License (MS-PL) [OSI Approved License]
The full license text is in the LICENSE file at the root of the repository.
*/

#include <exception>
#include "CAltaLuxFilterFactory.h"
#include "CBaseAltaLuxFilter.h"
#include "CSerialAltaLuxFilter.h"
#include "CParallelSplitLoopAltaLuxFilter.h"

CBaseAltaLuxFilter* CAltaLuxFilterFactory::CreateAltaLuxFilter(int Width, int Height, int HorSlices, int VerSlices)
{
	return CreateSpecificAltaLuxFilter(ALTALUX_FILTER_DEFAULT, Width, Height, HorSlices, VerSlices);
}

CBaseAltaLuxFilter* CAltaLuxFilterFactory::CreateSpecificAltaLuxFilter(int FilterType, int Width, int Height,
	int HorSlices, int VerSlices)
{
	CBaseAltaLuxFilter* NewFilterInstance = nullptr;
	try
	{
		switch (FilterType)
		{
		case ALTALUX_FILTER_SERIAL: NewFilterInstance = new CSerialAltaLuxFilter(Width, Height, HorSlices, VerSlices);
			break;
		case ALTALUX_FILTER_DEFAULT:
		case ALTALUX_FILTER_PARALLEL_SPLIT_LOOP:
		default:
			NewFilterInstance = new CParallelSplitLoopAltaLuxFilter(
				Width, Height, HorSlices, VerSlices);
			break;
		}
	}
	catch (const std::exception&)
	{
		NewFilterInstance = nullptr;
	}
	return NewFilterInstance;
}
