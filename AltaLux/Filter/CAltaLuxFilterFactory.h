/*
Project: AltaLux plugin for IrfanView
Author: Stefano Tommesani
Website: http://www.tommesani.com

Microsoft Public License (MS-PL) [OSI Approved License]
The full license text is in the LICENSE file at the root of the repository.
*/

#pragma once

#include "CBaseAltaLuxFilter.h"

/// @file CAltaLuxFilterFactory.h
/// @brief Factory for creating CLAHE filter instances.

/// @defgroup FilterTypes Filter implementation ids
/// @{

/// Default production filter.
const int ALTALUX_FILTER_DEFAULT = 0;

/// Single-threaded reference filter.
const int ALTALUX_FILTER_SERIAL = 1;

/// Parallel split-loop filter.
const int ALTALUX_FILTER_PARALLEL_SPLIT_LOOP = 2;

/// Legacy ids kept for callers that may still pass older strategy values.
/// The factory currently maps unsupported strategies to the default filter.
const int ALTALUX_FILTER_PARALLEL_ERROR = 3;
const int ALTALUX_FILTER_PARALLEL_EVENT = 4;
const int ALTALUX_FILTER_ACTIVE_WAIT = 5;

/// @}

/// @class CAltaLuxFilterFactory
/// @brief Creates AltaLux filter implementations.
///
/// @details Factory methods return owning raw pointers to preserve the original
///          plugin API shape. Callers must delete a non-null returned filter.
class CAltaLuxFilterFactory
{
public:
	/// @brief Create a filter using the default strategy.
	/// @param Width Image width in pixels.
	/// @param Height Image height in pixels.
	/// @param HorSlices Number of horizontal tiles.
	/// @param VerSlices Number of vertical tiles.
	/// @return Pointer to a new filter, or nullptr on allocation failure.
	static CBaseAltaLuxFilter* CreateAltaLuxFilter(
		int Width,
		int Height,
		int HorSlices = DEFAULT_HOR_REGIONS,
		int VerSlices = DEFAULT_VERT_REGIONS);

	/// @brief Create a filter with a specific implementation id.
	/// @param FilterType One of the ALTALUX_FILTER_* ids.
	/// @param Width Image width in pixels.
	/// @param Height Image height in pixels.
	/// @param HorSlices Number of horizontal tiles.
	/// @param VerSlices Number of vertical tiles.
	/// @return Pointer to a new filter, or nullptr on allocation failure.
	static CBaseAltaLuxFilter* CreateSpecificAltaLuxFilter(
		int FilterType,
		int Width,
		int Height,
		int HorSlices = DEFAULT_HOR_REGIONS,
		int VerSlices = DEFAULT_VERT_REGIONS);
};
