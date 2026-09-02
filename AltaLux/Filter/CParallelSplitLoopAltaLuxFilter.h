/*
Project: AltaLux plugin for IrfanView
Author: Stefano Tommesani
Website: http://www.tommesani.com

Microsoft Public License (MS-PL) [OSI Approved License]
The full license text is in the LICENSE file at the root of the repository.
*/

#pragma once

#include "CBaseAltaLuxFilter.h"

/// @file CParallelSplitLoopAltaLuxFilter.h
/// @brief Parallel CLAHE implementation using two independent PPL loops.
///
/// @class CParallelSplitLoopAltaLuxFilter
/// @brief CLAHE filter that separates map generation from interpolation.
///
/// @details The first parallel loop builds and clips every tile histogram. The
///          second parallel loop interpolates non-overlapping output regions
///          from those read-only maps. The separate loops form the required
///          synchronization barrier without explicit locking.
///
/// @see CBaseAltaLuxFilter
/// @see CSerialAltaLuxFilter
/// @see CAltaLuxFilterFactory
class CParallelSplitLoopAltaLuxFilter : public CBaseAltaLuxFilter
{
public:
	/// @brief Construct parallel split-loop CLAHE filter.
	/// @param Width Image width in pixels.
	/// @param Height Image height in pixels.
	/// @param HorSlices Number of horizontal tiles.
	/// @param VerSlices Number of vertical tiles.
	CParallelSplitLoopAltaLuxFilter(int Width, int Height, int HorSlices = DEFAULT_HOR_REGIONS,
	                                int VerSlices = DEFAULT_VERT_REGIONS) :
		CBaseAltaLuxFilter(Width, Height, HorSlices, VerSlices)
	{
	}

protected:
	/// @brief Execute CLAHE with split-loop parallel processing.
	/// @return AL_OK on success, or an AL_* error code on failure.
	///
	/// @details Phase 1 writes each tile map once. Phase 2 reads those maps and
	///          writes disjoint image regions, including half-width/half-height
	///          border regions that reuse the closest tile map.
	int Run() override;
};
