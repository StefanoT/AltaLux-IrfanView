/*
Project: AltaLux plugin for IrfanView
Author: Stefano Tommesani
Website: http://www.tommesani.com

Microsoft Public License (MS-PL) [OSI Approved License]

This license governs use of the accompanying software. If you use the software, you accept this license. If you do not accept the license, do not use the software.

1. Definitions
The terms "reproduce," "reproduction," "derivative works," and "distribution" have the same meaning here as under U.S. copyright law.
A "contribution" is the original software, or any additions or changes to the software.
A "contributor" is any person that distributes its contribution under this license.
"Licensed patents" are a contributor's patent claims that read directly on its contribution.

2. Grant of Rights
(A) Copyright Grant- Subject to the terms of this license, including the license conditions and limitations in section 3, each contributor grants you a non-exclusive, worldwide, royalty-free copyright license to reproduce its contribution, prepare derivative works of its contribution, and distribute its contribution or any derivative works that you create.
(B) Patent Grant- Subject to the terms of this license, including the license conditions and limitations in section 3, each contributor grants you a non-exclusive, worldwide, royalty-free license under its licensed patents to make, have made, use, sell, offer for sale, import, and/or otherwise dispose of its contribution in the software or derivative works of the contribution in the software.

3. Conditions and Limitations
(A) No Trademark License- This license does not grant you rights to use any contributors' name, logo, or trademarks.
(B) If you bring a patent claim against any contributor over patents that you claim are infringed by the software, your patent license from such contributor to the software ends automatically.
(C) If you distribute any portion of the software, you must retain all copyright, patent, trademark, and attribution notices that are present in the software.
(D) If you distribute any portion of the software in source code form, you may do so only under this license by including a complete copy of this license with your distribution. If you distribute any portion of the software in compiled or object code form, you may only do so under a license that complies with this license.
(E) The software is licensed "as-is." You bear the risk of using it. The contributors give no express warranties, guarantees or conditions. You may have additional consumer rights under your local laws which this license cannot change. To the extent permitted under your local laws, the contributors exclude the implied warranties of merchantability, fitness for a particular purpose and non-infringement.
*/

//#include "stdafx.h"
#include <exception>
#include "CAltaLuxFilterFactory.h"
#include "CBaseAltaLuxFilter.h"
#include "CSerialAltaLuxFilter.h"
#include "CParallelSplitLoopAltaLuxFilter.h"
#include "CParallelErrorAltaLuxFilter.h"
#include "CParallelEventAltaLuxFilter.h"
#include "CParallelActiveWaitAltaLuxFilter.h"

#ifdef ENABLE_LOGGING
#include "..\Log\easylogging++.h"
#endif // ENABLE_LOGGING

/// <summary>
/// create an instance of the AltaLux filter using the default strategy
/// </summary>
/// <param name="Width"></param>
/// <param name="Height"></param>
/// <param name="HorSlices"></param>
/// <param name="VerSlices"></param>
/// <returns>Instance of AltaLux filter</returns>
CBaseAltaLuxFilter* CAltaLuxFilterFactory::CreateAltaLuxFilter(int Width, int Height, int HorSlices, int VerSlices)
{
	return CreateSpecificAltaLuxFilter(ALTALUX_FILTER_DEFAULT, Width, Height, HorSlices, VerSlices);
}

/// <summary>
/// create an instance of the AltaLux filter with the specified strategy. Used for testing and benchmarking
/// </summary>
/// <param name="FilterType">Strategy to use, refer to ALTALUX_FILTER_XXX constants</param>
/// <param name="Width"></param>
/// <param name="Height"></param>
/// <param name="HorSlices"></param>
/// <param name="VerSlices"></param>
/// <returns>Instance of AltaLux filter</returns>
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
		case ALTALUX_FILTER_PARALLEL_ERROR: NewFilterInstance = new CParallelErrorAltaLuxFilter(
			Width, Height, HorSlices, VerSlices);
			break;
		case ALTALUX_FILTER_PARALLEL_EVENT: NewFilterInstance = new CParallelEventAltaLuxFilter(
			Width, Height, HorSlices, VerSlices);
			break;
		case ALTALUX_FILTER_ACTIVE_WAIT: NewFilterInstance = new CParallelActiveWaitAltaLuxFilter(
			Width, Height, HorSlices, VerSlices);
			break;
		case ALTALUX_FILTER_DEFAULT:
		case ALTALUX_FILTER_PARALLEL_SPLIT_LOOP:
		default:
			NewFilterInstance = new CParallelSplitLoopAltaLuxFilter(
				Width, Height, HorSlices, VerSlices);
			break;
		}
	}
	catch (std::exception& e)
	{
#ifdef ENABLE_LOGGING
		LOG(ERROR) << "Exception running filter instance (114): " << e.what();
#endif
		NewFilterInstance = nullptr;
}
	return NewFilterInstance;
}
