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

//=============================================================================
// CLAHE (Contrast Limited Adaptive Histogram Equalization) Filter
//=============================================================================
/// @file CBaseAltaLuxFilter.h
/// @brief Base class for CLAHE image enhancement filters
/// @details This file defines the base interface and common functionality for
///          implementing Contrast Limited Adaptive Histogram Equalization (CLAHE),
///          an advanced image enhancement algorithm that improves local contrast
///          while preventing over-amplification of noise.
///
/// @par CLAHE Algorithm Overview:
/// CLAHE divides the input image into small tiles (contextual regions) and 
/// applies histogram equalization to each tile independently. To prevent over-
/// amplification of noise, histogram bins are clipped at a specified limit.
/// Bilinear interpolation between neighboring tiles eliminates boundary artifacts.
///
/// @par Processing Steps:
/// 1. **Tile Division**: Image divided into NumHorRegions × NumVertRegions tiles
/// 2. **Histogram Calculation**: Compute histogram for each tile
/// 3. **Histogram Clipping**: Clip histogram bins exceeding ClipLimit
/// 4. **Mapping Generation**: Create cumulative distribution function (CDF)
/// 5. **Interpolation**: Apply bilinear interpolation between tiles
///
/// @par Supported Pixel Formats:
/// - Grayscale (8-bit per pixel)
/// - RGB24 (24-bit per pixel)
/// - RGB32 (32-bit per pixel)
/// - BGR24 (24-bit per pixel)
/// - BGR32 (32-bit per pixel)
/// - YUV formats (UYVY, VYUY, YUYV, YVYU)
///
/// @author Stefano Tommesani
/// @version 1.10

//-----------------------------------------------------------------------------
// Return Codes
//-----------------------------------------------------------------------------
/// @defgroup ErrorCodes Filter Error Codes
/// @{

/// Processing completed successfully
const int AL_OK = 0;

/// Input image pointer is null
const int AL_NULL_IMAGE = -1;

/// Image width is not a multiple of 8 (required for alignment)
const int AL_WIDTH_NO_MULTIPLE = -3;

/// Image height is not a multiple of 8 (required for alignment)
const int AL_HEIGHT_NO_MULTIPLE = -4;

/// Insufficient memory to allocate internal buffers
const int AL_OUT_OF_MEMORY = -11;

/// @}

//-----------------------------------------------------------------------------
// Processing Strength Parameters
//-----------------------------------------------------------------------------
/// @defgroup StrengthParams Filter Strength Parameters
/// @{

/// Minimum strength (0 = filter disabled, pass-through mode)
const int AL_MIN_STRENGTH = 0;

/// Default/recommended strength for general use
const int AL_DEFAULT_STRENGTH = 25;

/// Maximum strength (aggressive enhancement)
const int AL_MAX_STRENGTH = 100;

/// Recommended strength for light contrast enhancement
const int AL_LIGHT_CONTRAST_STRENGTH = 5;

/// Recommended strength for heavy contrast enhancement
const int AL_HEAVY_CONTRAST_STRENGTH = 10;

/// @}

//-----------------------------------------------------------------------------
// Type Definitions
//-----------------------------------------------------------------------------
/// @brief Pixel type for 8-bit grayscale images
typedef unsigned char PixelType;

//-----------------------------------------------------------------------------
// Tile Configuration Constants
//-----------------------------------------------------------------------------
/// @defgroup TileConfig Contextual Region (Tile) Configuration
/// @{

/// Maximum number of horizontal tiles (regions)
const unsigned int MAX_HOR_REGIONS = 16;

/// Maximum number of vertical tiles (regions)
const unsigned int MAX_VERT_REGIONS = 16;

/// Default number of horizontal tiles (good balance for most images)
const unsigned int DEFAULT_HOR_REGIONS = 8;

/// Default number of vertical tiles (good balance for most images)
const unsigned int DEFAULT_VERT_REGIONS = 8;

/// Minimum number of horizontal tiles
const unsigned int MIN_HOR_REGIONS = 2;

/// Minimum number of vertical tiles
const unsigned int MIN_VERT_REGIONS = 2;

/// @}

//-----------------------------------------------------------------------------
// Histogram Constants
//-----------------------------------------------------------------------------
/// @defgroup HistogramParams Histogram Parameters
/// @{

/// Number of gray levels in histogram (8-bit = 256 levels)
const unsigned int NUM_GRAY_LEVELS = 256;

/// Maximum gray value (255 for 8-bit)
const unsigned int MAX_GRAY_VALUE = (NUM_GRAY_LEVELS - 1);

/// Minimum gray value (always 0)
const unsigned int MIN_GRAY_VALUE = 0;

/// @}

//-----------------------------------------------------------------------------
// Clip Limit Constants
//-----------------------------------------------------------------------------
/// @defgroup ClipLimits Histogram Clipping Parameters
/// @brief Controls the degree of contrast enhancement
/// @details Higher clip limits allow more contrast enhancement but may amplify
///          noise. Lower clip limits provide more conservative enhancement.
/// @{

/// Default clip limit (balanced enhancement)
const float DEFAULT_CLIP_LIMIT = 2.0f;

/// Minimum clip limit (conservative enhancement)
const float MIN_CLIP_LIMIT = 1.0f;

/// Maximum clip limit (aggressive enhancement)
const float MAX_CLIP_LIMIT = 5.0f;

/// @}

//-----------------------------------------------------------------------------
// Internal Buffer Size
//-----------------------------------------------------------------------------
/// @brief Size of internal image buffer with safety padding
/// @note Extra padding helps prevent buffer overruns in edge cases
#define IMAGE_BUFFER_SIZE	(OriginalImageWidth * (OriginalImageHeight + 1))

//=============================================================================
// CBaseAltaLuxFilter Class
//=============================================================================
/// @class CBaseAltaLuxFilter
/// @brief Abstract base class for CLAHE filter implementations
///
/// @details This class provides the common interface and shared functionality
///          for all CLAHE filter implementations. Derived classes implement
///          the Run() method with different parallelization strategies:
///          - Serial (single-threaded reference implementation)
///          - Parallel Split Loop (recommended for most use cases)
///          - Parallel with Events
///          - Parallel with Active Waiting
///
/// @par Memory Management:
/// The class manages an internal buffer (ImageBuffer) for intermediate processing.
/// This buffer is allocated when strength > 0 and freed when strength = 0.
///
/// @par Thread Safety:
/// This class is NOT thread-safe. Each instance should be used by a single thread.
/// Multiple threads can safely use separate instances.
///
/// @par Usage Example:
/// @code
/// // Create filter instance (8x8 tiles)
/// CBaseAltaLuxFilter* filter = CAltaLuxFilterFactory::CreateAltaLuxFilter(
///     imageWidth, imageHeight, 8, 8);
///
/// // Configure processing strength (0-100)
/// filter->SetStrength(25);  // Default strength
///
/// // Process RGB24 image
/// int result = filter->ProcessRGB24(imageData);
/// if (result != AL_OK) {
///     // Handle error
/// }
///
/// // Clean up
/// delete filter;
/// @endcode
///
/// @see CAltaLuxFilterFactory
/// @see CSerialAltaLuxFilter
/// @see CParallelSplitLoopAltaLuxFilter
class CBaseAltaLuxFilter
{
public:
	//-------------------------------------------------------------------------
	// Constructor & Destructor
	//-------------------------------------------------------------------------
	
	/// @brief Constructs a CLAHE filter with specified parameters
	/// @param Width Image width in pixels (must be multiple of 8)
	/// @param Height Image height in pixels (must be multiple of 8)
	/// @param HorSlices Number of horizontal tiles (2-16, default 8)
	/// @param VerSlices Number of vertical tiles (2-16, default 8)
	/// @note Image dimensions are adjusted to be evenly divisible by tile count
	/// @note Actual processing dimensions may be smaller than original if not evenly divisible
	CBaseAltaLuxFilter(int Width, int Height, int HorSlices = DEFAULT_HOR_REGIONS, int VerSlices = DEFAULT_VERT_REGIONS);
	
	/// @brief Destructor - releases internal buffers
	virtual ~CBaseAltaLuxFilter();

	//-------------------------------------------------------------------------
	// Configuration Methods
	//-------------------------------------------------------------------------
	
	/// @brief Set processing strength (intensity of enhancement)
	/// @param _Strength Enhancement strength from AL_MIN_STRENGTH to AL_MAX_STRENGTH
	///                  - 0: Disabled (pass-through, no processing)
	///                  - 1-24: Light enhancement
	///                  - 25: Default/recommended (balanced)
	///                  - 26-50: Moderate enhancement
	///                  - 51-100: Strong enhancement
	/// @note When strength is 0, internal buffer is freed to save memory
	/// @note Higher values provide more dramatic contrast but may amplify noise
	/// @see AL_MIN_STRENGTH, AL_DEFAULT_STRENGTH, AL_MAX_STRENGTH
	void SetStrength(int _Strength = AL_DEFAULT_STRENGTH);
	
	/// @brief Set the number of tiles (contextual regions)
	/// @param HorSlices Number of horizontal tiles (2-16)
	/// @param VerSlices Number of vertical tiles (2-16)
	/// @note More tiles = finer local detail enhancement but slower processing
	/// @note Fewer tiles = faster processing but less localized enhancement
	/// @note Values outside valid range are automatically clamped
	void SetSlices(int HorSlices, int VerSlices);
	
	/// @brief Check if filter is enabled (strength > 0)
	/// @return true if filter will process images, false if pass-through mode
	bool IsEnabled() const;

	//-------------------------------------------------------------------------
	// Image Processing Methods - YUV Formats
	//-------------------------------------------------------------------------
	
	/// @brief Process UYVY format image (U-Y-V-Y packed)
	/// @param Image Pointer to image data (must not be null)
	/// @return AL_OK on success, error code on failure
	/// @note Only Y (luminance) channel is processed, UV (chrominance) unchanged
	/// @note Format: [U0 Y0 V0 Y1] [U1 Y2 V1 Y3] ...
	/// @see AL_OK, AL_NULL_IMAGE, AL_OUT_OF_MEMORY
	int ProcessUYVY(void* Image);
	
	/// @brief Process VYUY format image (V-Y-U-Y packed)
	/// @param Image Pointer to image data (must not be null)
	/// @return AL_OK on success, error code on failure
	/// @note Only Y (luminance) channel is processed, UV (chrominance) unchanged
	int ProcessVYUY(void* Image);
	
	/// @brief Process YUYV format image (Y-U-Y-V packed)
	/// @param Image Pointer to image data (must not be null)
	/// @return AL_OK on success, error code on failure
	/// @note Only Y (luminance) channel is processed, UV (chrominance) unchanged
	/// @note Format: [Y0 U0 Y1 V0] [Y2 U1 Y3 V1] ...
	int ProcessYUYV(void* Image);
	
	/// @brief Process YVYU format image (Y-V-Y-U packed)
	/// @param Image Pointer to image data (must not be null)
	/// @return AL_OK on success, error code on failure
	/// @note Only Y (luminance) channel is processed, UV (chrominance) unchanged
	int ProcessYVYU(void* Image);

	//-------------------------------------------------------------------------
	// Image Processing Methods - Grayscale
	//-------------------------------------------------------------------------
	
	/// @brief Process grayscale image (8 bits per pixel)
	/// @param Image Pointer to image data (must not be null)
	/// @return AL_OK on success, error code on failure
	/// @note Most efficient format as no color space conversion is needed
	/// @note Image is processed in-place
	int ProcessGray(void* Image);

	//-------------------------------------------------------------------------
	// Image Processing Methods - RGB Formats
	//-------------------------------------------------------------------------
	
	/// @brief Process 24-bit RGB image (8 bits per channel, no alpha)
	/// @param Image Pointer to image data (must not be null)
	/// @return AL_OK on success, error code on failure
	/// @note Format: [R G B] [R G B] ... (3 bytes per pixel)
	/// @note Color is preserved by enhancing luminance and adjusting RGB proportionally
	/// @see ProcessGeneric for implementation details
	int ProcessRGB24(void* Image);
	
	/// @brief Process 32-bit RGB image (8 bits per channel + alpha/padding)
	/// @param Image Pointer to image data (must not be null)
	/// @return AL_OK on success, error code on failure
	/// @note Format: [R G B A] [R G B A] ... (4 bytes per pixel)
	/// @note Alpha channel (4th byte) is preserved unchanged
	int ProcessRGB32(void* Image);

	//-------------------------------------------------------------------------
	// Image Processing Methods - BGR Formats
	//-------------------------------------------------------------------------
	
	/// @brief Process 24-bit BGR image (8 bits per channel, no alpha)
	/// @param Image Pointer to image data (must not be null)
	/// @return AL_OK on success, error code on failure
	/// @note Format: [B G R] [B G R] ... (3 bytes per pixel)
	/// @note Common format for Windows DIB (Device Independent Bitmap)
	int ProcessBGR24(void* Image);
	
	/// @brief Process 32-bit BGR image (8 bits per channel + alpha/padding)
	/// @param Image Pointer to image data (must not be null)
	/// @return AL_OK on success, error code on failure
	/// @note Format: [B G R A] [B G R A] ... (4 bytes per pixel)
	int ProcessBGR32(void* Image);

	//-------------------------------------------------------------------------
	// Helper Methods (used by derived classes)
	//-------------------------------------------------------------------------
	
	/// @brief Process a single row of tiles (used in parallel implementations)
	/// @param uiY Row index (0 to NumVertRegions)
	/// @param ulClipLimit Histogram clip limit
	/// @param pulMapArray Pointer to mapping array
	/// @note This method is called by parallel implementations to process rows concurrently
	void ProcessRow(int uiY, unsigned int ulClipLimit, unsigned int* pulMapArray);
	
	/// @brief Calculate grayscale mappings for a row of tiles
	/// @param uiY Row index (0 to NumVertRegions-1)
	/// @param ulClipLimit Histogram clip limit
	/// @param pulMapArray Pointer to mapping array
	/// @note Computes histogram, clips it, and generates equalization mapping
	void CalcGraylevelMappings(int uiY, unsigned int ulClipLimit, unsigned int* pulMapArray);

protected:
	//-------------------------------------------------------------------------
	// Image Properties
	//-------------------------------------------------------------------------
	
	/// Width of image region to be processed (may be less than original)
	int ImageWidth;
	
	/// Height of image region to be processed (may be less than original)
	int ImageHeight;
	
	/// Original image width as provided by user
	int OriginalImageWidth;
	
	/// Original image height as provided by user
	int OriginalImageHeight;
	
	/// Internal buffer for grayscale processing (allocated on demand)
	unsigned char* ImageBuffer;
	
	/// Current processing strength (0-100)
	int Strength;

	//-------------------------------------------------------------------------
	// Tile Configuration
	//-------------------------------------------------------------------------
	
	/// Number of horizontal tiles (contextual regions)
	unsigned int NumHorRegions;
	
	/// Number of vertical tiles (contextual regions)
	unsigned int NumVertRegions;
	
	/// Width of each tile in pixels
	int RegionWidth;
	
	/// Height of each tile in pixels
	int RegionHeight;
	
	/// Current histogram clip limit (calculated from Strength)
	float ClipLimit;

	//-------------------------------------------------------------------------
	// Core Processing Method (must be implemented by derived classes)
	//-------------------------------------------------------------------------
	
	/// @brief Execute the CLAHE algorithm on ImageBuffer
	/// @return AL_OK on success, error code on failure
	/// @note This is the main processing method implemented by derived classes
	/// @note Each implementation uses a different parallelization strategy
	/// @note Input/output is ImageBuffer (grayscale 8-bit)
	/// @par Algorithm Steps:
	/// 1. Check if processing is enabled (ClipLimit > 1.0)
	/// 2. Allocate mapping array for all tiles
	/// 3. For each tile: compute histogram, clip, and create mapping
	/// 4. Interpolate between tiles to generate output
	/// @see CSerialAltaLuxFilter::Run
	/// @see CParallelSplitLoopAltaLuxFilter::Run
	virtual int Run() = 0;

	//-------------------------------------------------------------------------
	// Histogram Processing Methods
	//-------------------------------------------------------------------------
	
	/// @brief Clip histogram bins exceeding the specified limit
	/// @param pHistogram Pointer to histogram array (NUM_GRAY_LEVELS elements)
	/// @param ClipLimit Maximum allowed bin count
	/// @note Excess pixels are redistributed uniformly across all bins
	/// @note This prevents over-amplification of noise in uniform regions
	/// @par Algorithm:
	/// 1. Count total excess pixels (bins > ClipLimit)
	/// 2. Clip all bins to ClipLimit
	/// 3. Redistribute excess uniformly across all bins
	/// 4. Repeat until all excess is distributed
	void ClipHistogram(unsigned int* pHistogram, unsigned int ClipLimit);
	
	/// @brief Compute histogram of a single tile
	/// @param pImage Pointer to top-left corner of tile
	/// @param pHistogram Output histogram array (NUM_GRAY_LEVELS elements)
	/// @note Histogram counts frequency of each gray level (0-255)
	/// @note Only processes RegionWidth × RegionHeight area
	void MakeHistogram(PixelType* pImage, unsigned int* pHistogram);
	
	/// @brief Generate cumulative distribution function (CDF) mapping
	/// @param pHistogram Input/output histogram array (modified in-place)
	/// @param NumOfPixels Total number of pixels in tile
	/// @note Converts histogram to equalization mapping [0..255]
	/// @note Each bin becomes the cumulative sum up to that gray level
	/// @note Result is scaled to output range [0..255]
	void MapHistogram(unsigned int* pHistogram, unsigned int NumOfPixels);
	
	/// @brief Apply bilinear interpolation between four tile mappings
	/// @param pImage Pointer to image region to interpolate
	/// @param pulMapLU Upper-left tile mapping
	/// @param pulMapRU Upper-right tile mapping
	/// @param pulMapLB Lower-left tile mapping
	/// @param pulMapRB Lower-right tile mapping
	/// @param MatrixWidth Width of region to interpolate
	/// @param MatrixHeight Height of region to interpolate
	/// @note This eliminates blocky artifacts at tile boundaries
	/// @note Each pixel value is weighted by distance to neighboring tiles
	/// @par Interpolation Formula:
	/// For pixel at (x,y) in region:
	/// @code
	/// NewValue = (h-y)*(w-x)*MapLU[old] + (h-y)*x*MapRU[old]
	///          + y*(w-x)*MapLB[old]     + y*x*MapRB[old]
	/// where w = MatrixWidth, h = MatrixHeight
	/// @endcode
	void Interpolate(PixelType* pImage, unsigned int* pulMapLU,
	                 unsigned int* pulMapRU, unsigned int* pulMapLB, unsigned int* pulMapRB,
	                 unsigned int MatrixWidth, unsigned int MatrixHeight);

	//-------------------------------------------------------------------------
	// Generic RGB Processing
	//-------------------------------------------------------------------------
	
	/// @brief Generic RGB/BGR processing with configurable channel order
	/// @param Image Pointer to image data
	/// @param FirstFactor Scale factor for first channel (R or B)
	/// @param SecondFactor Scale factor for second channel (G)
	/// @param ThirdFactor Scale factor for third channel (B or R)
	/// @param PixelOffset Bytes per pixel (3 for 24-bit, 4 for 32-bit)
	/// @return AL_OK on success, error code on failure
	/// @note Converts RGB to luminance (Y), processes Y, then adjusts RGB
	/// @note Color ratios are preserved to avoid hue shifts
	/// @par Luminance Calculation:
	/// Y = 0.299*R + 0.587*G + 0.114*B (ITU-R BT.601 standard)
	/// @par Color Preservation:
	/// After processing Y to Y', each RGB component is adjusted:
	/// R' = R + (Y' - Y), clamped to [0, 255]
	int ProcessGeneric(void* Image, int FirstFactor, int SecondFactor,
	                   int ThirdFactor, int PixelOffset);

	/// @brief Extracts Y (luminance) component from RGB image into ImageBuffer
	/// @param Image Pointer to source RGB image data (must not be null)
	/// @param FirstFactor Scaling factor for first color component (0.299 * 32768 for R in RGB)
	/// @param SecondFactor Scaling factor for second color component (0.587 * 32768 for G)
	/// @param ThirdFactor Scaling factor for third color component (0.114 * 32768 for B)
	/// @param PixelOffset Bytes per pixel (3 for RGB24, 4 for RGB32)
	/// @details Converts RGB to luminance using ITU-R BT.601 formula:
	///          Y = 0.299*R + 0.587*G + 0.114*B
	///          Uses fixed-point arithmetic (scale by 2^15) for performance.
	///          Result stored in ImageBuffer for CLAHE processing.
	/// @note This method is optimized with SSE2 SIMD when available (2-4x faster)
	void ExtractYComponent(void* Image, int FirstFactor, int SecondFactor, int ThirdFactor, int PixelOffset);

	/// @brief Injects processed Y component back into RGB image using multiplicative scaling
	/// @param Image Pointer to RGB image data to modify (must not be null)
	/// @param ImageBuffer Pointer to processed luminance buffer (enhanced Y values)
	/// @param FirstFactor Scaling factor for first color component (same as ExtractYComponent)
	/// @param SecondFactor Scaling factor for second color component
	/// @param ThirdFactor Scaling factor for third color component
	/// @param PixelOffset Bytes per pixel (3 for RGB24, 4 for RGB32)
	/// @details Preserves color (hue and saturation) while applying luminance enhancement:
	///          1. Calculate original Y from current RGB
	///          2. Lookup pre-computed scale factor from table (eliminates division)
	///          3. Apply multiplicative scaling: R' = R × scale, G' = G × scale, B' = B × scale
	///          4. Clamp each channel to [0, 255]
	///          This preserves color ratios perfectly, preventing hue shifts and desaturation.
	///          Uses lookup table (256 KB) for ~1.8-2.5x speedup over division.
	/// @note Called after CLAHE processing to apply enhancement to color image
	/// @note Black pixels (Y=0) are handled specially to avoid division by zero
	/// @note Lookup table initialized once on first call (one-time cost)
	void InjectYComponent(void* Image, void* ImageBuffer, int FirstFactor, int SecondFactor, int ThirdFactor, int PixelOffset);

};
