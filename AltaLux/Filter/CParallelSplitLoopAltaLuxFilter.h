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
// CParallelSplitLoopAltaLuxFilter Class
//=============================================================================
/// @file CParallelSplitLoopAltaLuxFilter.h
/// @brief Parallel CLAHE implementation using split-loop strategy
///
/// @class CParallelSplitLoopAltaLuxFilter
/// @brief CLAHE filter with two-phase parallel processing
///
/// @details This implementation divides the CLAHE algorithm into two distinct
///          parallel phases with an implicit synchronization barrier between them:
///
/// @par Phase 1: Histogram Calculation (Data Preparation)
/// All tile histograms are calculated in parallel. Each thread processes
/// multiple tiles independently with no shared state. This phase is
/// embarrassingly parallel with perfect scaling.
///
/// @par Phase 2: Interpolation (Image Generation)
/// All interpolation regions are processed in parallel. Each thread reads
/// from the pre-calculated histograms and writes to non-overlapping image
/// regions. No synchronization needed within phase.
///
/// @par Synchronization Strategy:
/// The concurrency::parallel_for construct provides an implicit barrier
/// between loop invocations. The second parallel_for only begins after
/// all threads in the first parallel_for complete.
///
/// @par Performance Characteristics:
/// - **Scalability**: Near-linear scaling with CPU cores (up to ~8 cores)
/// - **Memory Access**: Good cache locality within each tile
/// - **Overhead**: Minimal (two parallel_for invocations)
/// - **Typical Performance**: 3-4x faster than serial on quad-core CPU
///
/// @par Advantages:
/// - Simple implementation (no explicit synchronization code)
/// - Excellent performance for most use cases
/// - Easy to understand and maintain
/// - Minimal overhead from parallelization
/// - Good cache behavior (each thread works on contiguous data)
///
/// @par Disadvantages:
/// - Two separate parallel regions (slight overhead vs. single parallel region)
/// - All histogram calculations must complete before any interpolation starts
///   (could be optimized with pipelined approach)
///
/// @par Thread Safety:
/// Each parallel_for iteration works on independent data:
/// - Phase 1: Each thread writes to different histogram locations
/// - Phase 2: Each thread reads shared histograms (read-only) and writes
///            to non-overlapping image regions
///
/// @par Comparison with Other Implementations:
/// | Implementation        | Complexity | Performance | Recommended |
/// |-----------------------|------------|-------------|-------------|
/// | Serial                | Simple     | Baseline    | Testing     |
/// | **Split Loop**        | **Simple** | **~4x**     | **Yes**     |
/// | Parallel Error        | Medium     | ~3.5x       | No          |
/// | Parallel Event        | Complex    | ~3.5x       | No          |
/// | Active Wait           | Complex    | ~4x         | Special     |
///
/// @note This is the default and recommended implementation for production use
///
/// @par Usage Example:
/// @code
/// // Typically created via factory
/// CBaseAltaLuxFilter* filter = 
///     CAltaLuxFilterFactory::CreateAltaLuxFilter(width, height);
///
/// // Or directly (for testing)
/// CParallelSplitLoopAltaLuxFilter filter(1920, 1080, 8, 8);
/// filter.SetStrength(25);
/// filter.ProcessRGB24(imageData);
/// @endcode
///
/// @see CBaseAltaLuxFilter
/// @see CSerialAltaLuxFilter
/// @see CAltaLuxFilterFactory
class CParallelSplitLoopAltaLuxFilter : public CBaseAltaLuxFilter
{
public:
	//-------------------------------------------------------------------------
	// Constructor
	//-------------------------------------------------------------------------
	
	/// @brief Construct parallel split-loop filter
	/// @param Width Image width in pixels
	/// @param Height Image height in pixels
	/// @param HorSlices Number of horizontal tiles (default: 8)
	/// @param VerSlices Number of vertical tiles (default: 8)
	/// @note Parameters passed directly to base class constructor
	/// @see CBaseAltaLuxFilter::CBaseAltaLuxFilter
	CParallelSplitLoopAltaLuxFilter(int Width, int Height, int HorSlices = DEFAULT_HOR_REGIONS,
	                                int VerSlices = DEFAULT_VERT_REGIONS) :
		CBaseAltaLuxFilter(Width, Height, HorSlices, VerSlices)
	{
	}

protected:
	//-------------------------------------------------------------------------
	// Core Processing Method
	//-------------------------------------------------------------------------
	
	/// @brief Execute CLAHE algorithm with two-phase parallel processing
	/// @return AL_OK on success, error code on failure
	///
	/// @details Implements CLAHE using split-loop parallelization:
	///
	/// @par Algorithm Steps:
	/// 1. **Early Exit**: If ClipLimit == 1.0, return immediately (no processing)
	/// 2. **Setup**: Calculate clip limit from strength parameter
	/// 3. **Phase 1 - Parallel Histogram Processing**:
	///    - For each tile row (NumVertRegions iterations):
	///      - For each tile in row (NumHorRegions iterations):
	///        - Extract tile's pixel data
	///        - Compute histogram (MakeHistogram)
	///        - Clip histogram bins (ClipHistogram)
	///        - Generate mapping CDF (MapHistogram)
	///    - Implicit barrier: all tiles complete before Phase 2
	/// 4. **Phase 2 - Parallel Interpolation**:
	///    - For each interpolation row (NumVertRegions+1 iterations):
	///      - For each interpolation region in row (NumHorRegions+1 iterations):
	///        - Get four neighboring tile mappings
	///        - Apply bilinear interpolation
	///        - Write enhanced pixels to image
	///
	/// @par Parallelization Details:
	/// Uses Microsoft PPL (Parallel Patterns Library) concurrency::parallel_for:
	/// @code
	/// // Phase 1: Histogram calculation
	/// concurrency::parallel_for(0, NumVertRegions, [&](int uiY) {
	///     // Each thread processes independent tiles
	///     // No synchronization needed
	/// });
	///
	/// // Implicit barrier here - all Phase 1 threads complete
	///
	/// // Phase 2: Interpolation
	/// concurrency::parallel_for(0, NumVertRegions+1, [&](int uiY) {
	///     // Each thread processes independent regions
	///     // Reads shared histograms (read-only, no races)
	/// });
	/// @endcode
	///
	/// @par Memory Access Pattern:
	/// - **Phase 1**: Sequential read from image tiles, random write to histograms
	/// - **Phase 2**: Random read from histograms, sequential write to image
	/// - Good cache locality within each tile (RegionWidth × RegionHeight)
	///
	/// @par Performance Optimization Tips:
	/// - Larger tiles (fewer regions) = less overhead but coarser enhancement
	/// - Smaller tiles (more regions) = finer enhancement but more overhead
	/// - Sweet spot for most images: 8×8 tiles (DEFAULT_HOR_REGIONS × DEFAULT_VERT_REGIONS)
	///
	/// @par Error Conditions:
	/// - Returns AL_OUT_OF_MEMORY if mapping array allocation fails
	/// - Returns AL_OK if processing disabled (ClipLimit == 1.0)
	///
	/// @note Thread count automatically determined by PPL based on CPU cores
	/// @note No manual thread management or synchronization required
	///
	/// @see CBaseAltaLuxFilter::Run
	/// @see CBaseAltaLuxFilter::MakeHistogram
	/// @see CBaseAltaLuxFilter::ClipHistogram
	/// @see CBaseAltaLuxFilter::MapHistogram
	/// @see CBaseAltaLuxFilter::Interpolate
	int Run() override;
};
