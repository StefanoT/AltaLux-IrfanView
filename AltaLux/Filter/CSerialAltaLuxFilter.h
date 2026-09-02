/*
Project: AltaLux plugin for IrfanView
Author: Stefano Tommesani
Website: http://www.tommesani.com

Microsoft Public License (MS-PL) [OSI Approved License]
The full license text is in the LICENSE file at the root of the repository.
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
