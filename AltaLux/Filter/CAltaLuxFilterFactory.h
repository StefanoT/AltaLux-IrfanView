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

//=============================================================================
// Filter Type Constants
//=============================================================================
/// @file CAltaLuxFilterFactory.h
/// @brief Factory for creating CLAHE filter instances with different strategies
/// @details This factory implements the Strategy pattern, allowing selection
///          between different parallelization approaches at runtime.

/// @defgroup FilterTypes Filter Implementation Types
/// @brief Available CLAHE filter implementation strategies
/// @{

/// @brief Default filter (currently ParallelSplitLoop - recommended)
/// @note This automatically selects the best general-purpose implementation
const int ALTALUX_FILTER_DEFAULT = 0;

/// @brief Serial (single-threaded) implementation
/// @details Reference implementation used for testing and validation.
///          Slowest but most straightforward to understand.
/// @note Typical performance: ~1.2 seconds for full HD image
/// @see CSerialAltaLuxFilter
const int ALTALUX_FILTER_SERIAL = 1;

/// @brief Parallel implementation using split loop strategy
/// @details Divides processing into two parallel phases:
///          - Phase 1: Calculate histograms for all tiles in parallel
///          - Phase 2: Interpolate all tiles in parallel
///          Uses implicit synchronization barrier between phases.
/// @note Recommended for most use cases (best performance/complexity ratio)
/// @note Typical performance: ~0.3-0.5 seconds for full HD image
/// @see CParallelSplitLoopAltaLuxFilter
const int ALTALUX_FILTER_PARALLEL_SPLIT_LOOP = 2;

/// @brief Parallel implementation with error-based synchronization
/// @details Uses error/return codes for thread synchronization.
///          More complex but provides fine-grained control.
/// @note Experimental - may not provide benefits over split loop
/// @see CParallelErrorAltaLuxFilter
const int ALTALUX_FILTER_PARALLEL_ERROR = 3;

/// @brief Parallel implementation using Windows Events
/// @details Uses kernel event objects for explicit thread synchronization.
///          Provides precise control over thread coordination.
/// @note Higher overhead due to kernel transitions
/// @note Useful for understanding explicit synchronization patterns
/// @see CParallelEventAltaLuxFilter
const int ALTALUX_FILTER_PARALLEL_EVENT = 4;

/// @brief Parallel implementation with active waiting
/// @details Uses busy-waiting (spin locks) for thread synchronization.
///          May consume more CPU but has lower latency.
/// @note Best for systems with dedicated CPU cores
/// @note May waste CPU time on systems under heavy load
/// @see CParallelActiveWaitAltaLuxFilter
const int ALTALUX_FILTER_ACTIVE_WAIT = 5;

/// @}

//=============================================================================
// CAltaLuxFilterFactory Class
//=============================================================================
/// @class CAltaLuxFilterFactory
/// @brief Factory class for creating AltaLux filter instances
///
/// @details This class implements the Factory Method pattern to create
///          instances of CBaseAltaLuxFilter with different implementation
///          strategies. The factory allows runtime selection of parallelization
///          approach, useful for:
///          - Performance testing and benchmarking
///          - Platform-specific optimization
///          - Feature testing during development
///
/// @par Design Pattern: Factory Method
/// The factory encapsulates object creation logic, allowing clients to
/// create filters without knowing implementation details.
///
/// @par Usage Example:
/// @code
/// // Create default filter (recommended for production)
/// CBaseAltaLuxFilter* filter = 
///     CAltaLuxFilterFactory::CreateAltaLuxFilter(width, height, 8, 8);
///
/// // Create specific filter for benchmarking
/// CBaseAltaLuxFilter* serialFilter = 
///     CAltaLuxFilterFactory::CreateSpecificAltaLuxFilter(
///         ALTALUX_FILTER_SERIAL, width, height, 8, 8);
///
/// // Use filter...
///
/// // Clean up (caller is responsible for deletion)
/// delete filter;
/// delete serialFilter;
/// @endcode
///
/// @note All factory methods return raw pointers. Caller is responsible
///       for deletion. Consider using smart pointers (std::unique_ptr) to
///       manage ownership automatically.
///
/// @warning If filter creation fails, methods return nullptr.
///          Always check return value before use.
///
/// @see CBaseAltaLuxFilter
/// @see CSerialAltaLuxFilter
/// @see CParallelSplitLoopAltaLuxFilter
class CAltaLuxFilterFactory
{
public:
	//-------------------------------------------------------------------------
	// Factory Methods
	//-------------------------------------------------------------------------
	
	/// @brief Create a filter instance using the default strategy
	/// @param Width Image width in pixels
	/// @param Height Image height in pixels
	/// @param HorSlices Number of horizontal tiles (default: 8)
	/// @param VerSlices Number of vertical tiles (default: 8)
	/// @return Pointer to new filter instance, or nullptr on failure
	/// @note Default strategy is currently ALTALUX_FILTER_PARALLEL_SPLIT_LOOP
	/// @note Caller is responsible for deleting the returned pointer
	/// @warning Always check for nullptr before using returned pointer
	/// @par Example:
	/// @code
	/// CBaseAltaLuxFilter* filter = 
	///     CAltaLuxFilterFactory::CreateAltaLuxFilter(1920, 1080);
	/// if (filter) {
	///     filter->SetStrength(25);
	///     filter->ProcessRGB24(imageData);
	///     delete filter;
	/// }
	/// @endcode
	static CBaseAltaLuxFilter* CreateAltaLuxFilter(
		int Width, 
		int Height, 
		int HorSlices = DEFAULT_HOR_REGIONS,
		int VerSlices = DEFAULT_VERT_REGIONS);
	
	/// @brief Create a filter instance with specific implementation strategy
	/// @param FilterType Strategy to use (see ALTALUX_FILTER_* constants)
	/// @param Width Image width in pixels
	/// @param Height Image height in pixels
	/// @param HorSlices Number of horizontal tiles (default: 8)
	/// @param VerSlices Number of vertical tiles (default: 8)
	/// @return Pointer to new filter instance, or nullptr on failure
	/// @note Primarily used for testing, benchmarking, and comparison
	/// @note Caller is responsible for deleting the returned pointer
	/// @warning Always check for nullptr before using returned pointer
	/// @par Benchmark Example:
	/// @code
	/// // Compare performance of different strategies
	/// const int strategies[] = {
	///     ALTALUX_FILTER_SERIAL,
	///     ALTALUX_FILTER_PARALLEL_SPLIT_LOOP,
	///     ALTALUX_FILTER_ACTIVE_WAIT
	/// };
	///
	/// for (int strategy : strategies) {
	///     CBaseAltaLuxFilter* filter = 
	///         CAltaLuxFilterFactory::CreateSpecificAltaLuxFilter(
	///             strategy, 1920, 1080);
	///     if (filter) {
	///         auto start = high_resolution_clock::now();
	///         filter->ProcessRGB24(imageData);
	///         auto end = high_resolution_clock::now();
	///         // Log performance...
	///         delete filter;
	///     }
	/// }
	/// @endcode
	/// @see ALTALUX_FILTER_SERIAL
	/// @see ALTALUX_FILTER_PARALLEL_SPLIT_LOOP
	/// @see ALTALUX_FILTER_PARALLEL_ERROR
	/// @see ALTALUX_FILTER_PARALLEL_EVENT
	/// @see ALTALUX_FILTER_ACTIVE_WAIT
	static CBaseAltaLuxFilter* CreateSpecificAltaLuxFilter(
		int FilterType, 
		int Width, 
		int Height,
		int HorSlices = DEFAULT_HOR_REGIONS,
		int VerSlices = DEFAULT_VERT_REGIONS);
};
