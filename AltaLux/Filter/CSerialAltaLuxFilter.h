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

#pragma once

#include "CBaseAltaLuxFilter.h"

/// @file CSerialAltaLuxFilter.h
/// @brief Single-threaded reference implementation of CLAHE.
///
/// @class CSerialAltaLuxFilter
/// @brief Reference CLAHE filter used for validation and benchmarking.
///
/// @details Processes CLAHE tiles in row-major order on one thread. The serial
///          path is intentionally simple so tests and benchmarks can compare
///          optimized paths against a deterministic baseline.
///
/// @see CBaseAltaLuxFilter
/// @see CParallelSplitLoopAltaLuxFilter
/// @see CAltaLuxFilterFactory
class CSerialAltaLuxFilter : public CBaseAltaLuxFilter
{
public:
	/// @brief Construct serial CLAHE filter.
	/// @param Width Image width in pixels.
	/// @param Height Image height in pixels.
	/// @param HorSlices Number of horizontal tiles.
	/// @param VerSlices Number of vertical tiles.
	CSerialAltaLuxFilter(int Width, int Height, int HorSlices = DEFAULT_HOR_REGIONS,
	                     int VerSlices = DEFAULT_VERT_REGIONS) :
		CBaseAltaLuxFilter(Width, Height, HorSlices, VerSlices)
	{
	}

protected:
	/// @brief Execute CLAHE sequentially.
	/// @return AL_OK on success, or an AL_* error code on failure.
	///
	/// @details Builds the map for the current tile row before interpolating
	///          regions that depend on the current and previous rows. Border
	///          regions reuse the closest available tile map.
	int Run() override;
};
