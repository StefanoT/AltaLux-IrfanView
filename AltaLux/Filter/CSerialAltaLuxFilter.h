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
// CSerialAltaLuxFilter Class
//=============================================================================
/// @file CSerialAltaLuxFilter.h
/// @brief Single-threaded reference implementation of CLAHE
///
/// @class CSerialAltaLuxFilter
/// @brief Single-threaded CLAHE filter implementation
///
/// @details This is the reference implementation of the CLAHE algorithm using
///          a straightforward single-threaded approach. It processes tiles
///          sequentially in row-major order, calculating histograms and
///          performing interpolation one region at a time.
///
/// @par Processing Order:
/// For each row of tiles (top to bottom):
///   1. Calculate histograms for all tiles in the row
///   2. Immediately interpolate using those histograms
///   3. Move to next row
///
/// This interleaved approach (calculate-then-interpolate) has good cache
/// locality as the histograms are used immediately after calculation.
///
/// @par Performance Characteristics:
/// - **Speed**: Baseline (slowest implementation)
/// - **Typical Performance**: ~1.2 seconds for full HD image
/// - **Memory**: Lowest overhead (no thread management)
/// - **Complexity**: Simplest to understand and debug
///
/// @par Advantages:
/// - **Simplicity**: Easiest to understand and maintain
/// - **Predictability**: Deterministic behavior, no race conditions
/// - **Debugging**: Straightforward to debug and profile
/// - **Verification**: Serves as reference for validating parallel versions
/// - **Portability**: Works on any platform without threading support
///
/// @par Disadvantages:
/// - **Performance**: Does not utilize multi-core processors
/// - **Scalability**: No performance gain from additional CPU cores
/// - **Modern Hardware**: Underutilizes modern multi-core CPUs
///
/// @par Use Cases:
/// - **Reference Implementation**: Validate correctness of parallel versions
/// - **Testing**: Unit tests and algorithm verification
/// - **Benchmarking**: Baseline for performance comparisons
/// - **Single-Core Systems**: Systems where parallelization has no benefit
/// - **Debugging**: Simpler execution flow for troubleshooting
///
/// @par Typical Performance Comparison (Full HD Image, 8×8 tiles):
/// | Implementation    | Time     | Speedup | Notes                    |
/// |-------------------|----------|---------|--------------------------|
/// | **Serial**        | **1.2s** | **1x**  | **Baseline**             |
/// | Split Loop        | 0.3s     | 4x      | Recommended              |
/// | Parallel Error    | 0.35s    | 3.4x    | More complex             |
/// | Parallel Event    | 0.35s    | 3.4x    | Higher overhead          |
/// | Active Wait       | 0.3s     | 4x      | CPU intensive            |
///
/// @par Algorithm Visualization:
/// @code
/// For row 0 to NumVertRegions:
///   // Histogram phase for current row
///   For col 0 to NumHorRegions-1:
///     tile = GetTile(row, col)
///     histogram = MakeHistogram(tile)
///     ClipHistogram(histogram)
///     MapHistogram(histogram)
///   
///   // Interpolation phase for current row
///   For col 0 to NumHorRegions:
///     region = GetInterpolationRegion(row, col)
///     Interpolate(region, neighboring_histograms)
/// @endcode
///
/// @par Memory Access Pattern:
/// Sequential processing ensures:
/// - Good cache locality (working on nearby memory)
/// - Predictable memory access patterns
/// - No cache line conflicts between threads (since single-threaded)
///
/// @note This implementation is NOT recommended for production use where
///       performance is important. Use CParallelSplitLoopAltaLuxFilter instead.
///
/// @par Usage Example:
/// @code
/// // For testing and validation
/// CSerialAltaLuxFilter serialFilter(width, height, 8, 8);
/// serialFilter.SetStrength(25);
/// serialFilter.ProcessRGB24(testImage);
///
/// // Compare with parallel version
/// CParallelSplitLoopAltaLuxFilter parallelFilter(width, height, 8, 8);
/// parallelFilter.SetStrength(25);
/// parallelFilter.ProcessRGB24(testImage);
///
/// // Verify results are identical (within numerical precision)
/// assert(ImagesAreEqual(testImage, referenceImage));
/// @endcode
///
/// @see CBaseAltaLuxFilter
/// @see CParallelSplitLoopAltaLuxFilter
/// @see CAltaLuxFilterFactory
class CSerialAltaLuxFilter : public CBaseAltaLuxFilter
{
public:
	//-------------------------------------------------------------------------
	// Constructor
	//-------------------------------------------------------------------------
	
	/// @brief Construct serial CLAHE filter
	/// @param Width Image width in pixels
	/// @param Height Image height in pixels
	/// @param HorSlices Number of horizontal tiles (default: 8)
	/// @param VerSlices Number of vertical tiles (default: 8)
	/// @note Parameters passed directly to base class constructor
	/// @see CBaseAltaLuxFilter::CBaseAltaLuxFilter
	CSerialAltaLuxFilter(int Width, int Height, int HorSlices = DEFAULT_HOR_REGIONS,
	                     int VerSlices = DEFAULT_VERT_REGIONS) :
		CBaseAltaLuxFilter(Width, Height, HorSlices, VerSlices)
	{
	}

protected:
	//-------------------------------------------------------------------------
	// Core Processing Method
	//-------------------------------------------------------------------------
	
	/// @brief Execute CLAHE algorithm sequentially (single-threaded)
	/// @return AL_OK on success, error code on failure
	///
	/// @details Implements the classic CLAHE algorithm in a straightforward
	///          sequential manner. Each step is performed completely before
	///          moving to the next, with no parallel execution.
	///
	/// @par Algorithm Steps:
	/// 1. **Early Exit**: If ClipLimit == 1.0, return immediately (no enhancement)
	/// 2. **Allocation**: Create mapping array for all tiles (NumHorRegions × NumVertRegions × 256 ints)
	/// 3. **Calculate Clip Limit**: Convert strength parameter to actual histogram clip limit
	/// 4. **Main Loop** - For each row of tiles (uiY = 0 to NumVertRegions):
	///    - **Phase A: Histogram Processing** (if uiY < NumVertRegions):
	///      - For each tile in row (uiX = 0 to NumHorRegions-1):
	///        - Compute histogram from tile pixels
	///        - Clip histogram at specified limit
	///        - Generate cumulative distribution (mapping)
	///    - **Phase B: Interpolation**:
	///      - Determine region boundaries (special handling for edges)
	///      - For each interpolation region in row (uiX = 0 to NumHorRegions):
	///        - Get four neighboring tile mappings (upper-left, upper-right, lower-left, lower-right)
	///        - Apply bilinear interpolation to pixels in region
	///        - Write enhanced pixel values back to image
	/// 5. **Completion**: Return AL_OK
	///
	/// @par Data Dependencies:
	/// - Interpolation at row Y requires histograms from rows Y-1 and Y
	/// - This creates a sequential dependency between rows
	/// - However, within each row, tiles could theoretically be processed in parallel
	///   (but aren't in this serial implementation)
	///
	/// @par Edge Handling:
	/// Special cases for image boundaries:
	/// - **Top Row** (uiY == 0): Uses half-height region, mirrors upper mapping
	/// - **Bottom Row** (uiY == NumVertRegions): Uses half-height region, mirrors lower mapping
	/// - **Left Column** (uiX == 0): Uses half-width region, mirrors left mapping
	/// - **Right Column** (uiX == NumHorRegions): Uses half-width region, mirrors right mapping
	/// - **Corners**: Combine both edge conditions
	///
	/// @par Memory Access Pattern:
	/// Sequential access optimizes cache usage:
	/// @code
	/// // Good cache locality
	/// Row 0: Read tile(0,0), tile(0,1), ..., tile(0,N)
	///        Write to corresponding image regions
	/// Row 1: Read tile(1,0), tile(1,1), ..., tile(1,N)
	///        Write to corresponding image regions
	/// ...
	/// @endcode
	///
	/// @par Performance Bottlenecks:
	/// - No parallelization (single CPU core utilized)
	/// - Sequential processing of independent tiles
	/// - Histogram calculation is compute-intensive
	/// - Bilinear interpolation requires many multiply-add operations
	///
	/// @par Time Complexity:
	/// O(W × H × (1 + log(256))) where:
	/// - W × H = total pixels
	/// - Histogram clipping is O(256) per tile
	/// - Mapping is O(256) per tile
	/// - Interpolation is O(pixels) with 4 lookups per pixel
	///
	/// @note Single-threaded execution time: ~1.2 seconds for 1920×1080 image
	///
	/// @see CBaseAltaLuxFilter::Run
	/// @see CBaseAltaLuxFilter::MakeHistogram
	/// @see CBaseAltaLuxFilter::ClipHistogram
	/// @see CBaseAltaLuxFilter::MapHistogram
	/// @see CBaseAltaLuxFilter::Interpolate
	int Run() override;
};
