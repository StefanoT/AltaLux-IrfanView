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

//=============================================================================
// AltaLux IrfanView Plugin Interface
//=============================================================================
/// @file AltaLux.h
/// @brief Public interface for AltaLux IrfanView image enhancement plugin
///
/// @mainpage AltaLux Image Enhancement Plugin
///
/// @section intro_sec Introduction
/// AltaLux is a high-performance image enhancement plugin for IrfanView that
/// implements CLAHE (Contrast Limited Adaptive Histogram Equalization). CLAHE
/// is an advanced technique that improves local contrast while preventing
/// over-amplification of noise.
///
/// @section features_sec Features
/// - **Advanced Enhancement**: CLAHE algorithm for superior local contrast
/// - **High Performance**: Parallel processing utilizing multi-core CPUs
/// - **Multiple Formats**: Supports RGB24, RGB32, BGR24, BGR32, and YUV formats
/// - **Configurable**: Adjustable strength (0-100) and tile size (2×2 to 16×16)
/// - **Interactive Preview**: Real-time preview with multiple parameter variations
/// - **Dark Mode Support**: Native Windows dark mode integration
/// - **Selection Support**: Process entire image or selected region
///
/// @section algorithm_sec Algorithm Overview
/// CLAHE enhances images by:
/// 1. Dividing the image into small tiles (contextual regions)
/// 2. Computing histogram for each tile independently
/// 3. Clipping histogram to prevent noise amplification
/// 4. Equalizing each tile using its own histogram
/// 5. Interpolating between tiles to eliminate artifacts
///
/// @section usage_sec Usage
/// The plugin is loaded by IrfanView and appears in the Effects menu.
/// Users can:
/// - Adjust enhancement strength (0-100, default 25)
/// - Configure tile grid size (2-16 tiles per dimension, default 8)
/// - Preview changes in real-time
/// - See variations with different parameters
///
/// @section performance_sec Performance
/// - Serial implementation: ~1.2 seconds for Full HD
/// - Parallel implementation: ~0.3 seconds for Full HD (4× speedup)
/// - Scales well with CPU core count (up to 8 cores)
///
/// @section api_sec Plugin API
/// IrfanView plugins export two main functions:
/// - GetPlugInInfo() - Returns version and description
/// - StartEffects2() / AltaLux_Effects() - Processes the image
///
/// @see StartEffects2
/// @see GetPlugInInfo
/// @see CBaseAltaLuxFilter
/// @see CAltaLuxFilterFactory

//-----------------------------------------------------------------------------
// DLL Export/Import Macro
//-----------------------------------------------------------------------------
/// @brief Macro for exporting/importing DLL functions
/// @details When ALTALUX_EXPORTS is defined (during DLL compilation),
///          functions are exported. Otherwise, they're imported.
///          This is the standard Windows DLL pattern.
#ifdef ALTALUX_EXPORTS
#define ALTALUX_API __declspec(dllexport)
#else
#define ALTALUX_API __declspec(dllimport)
#endif

//=============================================================================
// IrfanView Plugin Interface Functions
//=============================================================================
/// @brief C-linkage export block (prevents C++ name mangling)
/// @details IrfanView expects standard C function names, not mangled C++ names.
///          The extern "C" ensures functions are exported with predictable names.
extern "C" {

//-----------------------------------------------------------------------------
// Main Processing Function
//-----------------------------------------------------------------------------

/// @brief Process image with AltaLux CLAHE enhancement (IrfanView interface)
/// @param hDib Handle to Windows DIB (Device Independent Bitmap)
/// @param hwnd Parent window handle for dialogs
/// @param filter Filter selection (currently unused, reserved for future)
/// @param rect Selection rectangle or full image dimensions
/// @param param1 Enhancement strength (0-100) or -1 to show GUI
/// @param param2 Tile grid size (2-16) or -1 to show GUI
/// @param iniFile Path to IrfanView INI file for settings persistence
/// @param szAppName Application name (typically "IrfanView")
/// @param regID Registration ID (reserved for future use)
/// @return true on success, false on failure
///
/// @details This is the main entry point called by IrfanView when the user
///          selects the AltaLux filter. The function signature is defined by
///          IrfanView's plugin interface specification.
///
/// @par Image Format Requirements:
/// - **Bit Depth**: IrfanView converts all images to 24-bit RGB before calling
/// - **Alignment**: Width and height should be multiples of 8 for best performance
/// - **Color Space**: RGB format (Red, Green, Blue)
///
/// @par Parameter Interpretation:
/// - **param1 and param2 both -1**: Show GUI dialog, load settings from INI
/// - **param1 >= 0 and param2 >= 0**: Apply filter directly without GUI
/// - **param1 only -1**: Show GUI (param2 ignored)
/// - **param2 only -1**: Show GUI (param1 ignored)
///
/// @par Selection Rectangle (rect parameter):
/// The rect parameter format is unusual - it contains:
/// - rect.left: X offset of selection
/// - rect.top: Y offset of selection
/// - rect.right: **WIDTH** of selection (not right coordinate!)
/// - rect.bottom: **HEIGHT** of selection (not bottom coordinate!)
/// This differs from standard Windows RECT where right/bottom are coordinates.
///
/// @par Processing Modes:
/// 1. **GUI Mode** (param1 == -1 or param2 == -1):
///    - Display interactive dialog with preview
///    - Load previous settings from INI file
///    - Allow user to adjust strength and grid size
///    - Show multiple preview variations
///    - Save settings to INI on OK
/// 2. **Direct Mode** (param1 >= 0 and param2 >= 0):
///    - Apply filter immediately with specified parameters
///    - No GUI, no user interaction
///    - Useful for batch processing or automation
///
/// @par INI File Format:
/// Settings are stored in IrfanView's INI file under [AltaLux] section:
/// @code
/// [AltaLux]
/// Intensity=25    ; Enhancement strength (0-100)
/// Scale=8         ; Tile grid size (2-16)
/// @endcode
///
/// @par Error Handling:
/// - Returns false if image pointer is null
/// - Returns false if bit depth is unsupported
/// - Returns false if memory allocation fails
/// - Returns true but skips processing if user cancels dialog
///
/// @par Performance Notes:
/// - Cropped regions are extracted and normalized before processing
/// - Preview uses downsampled image for responsiveness
/// - Full-resolution processing uses parallel implementation
///
/// @par Implementation Details:
/// The function performs these steps:
/// 1. Lock global memory and access bitmap header
/// 2. Validate image format (24/32 bpp, single plane)
/// 3. Determine if processing full image or selection
/// 4. If GUI requested: show dialog with real-time preview
/// 5. Apply CLAHE to luminance channel (preserving color ratios)
/// 6. Unlock memory and return result
///
/// @warning The hDib handle must remain valid throughout processing.
///          Do not close or free the handle while function is executing.
///
/// @note This function is thread-safe IF each call uses a different hDib.
///       Multiple threads should not process the same hDib concurrently.
///
/// @see GetPlugInInfo
/// @see CBaseAltaLuxFilter
/// @see ScopedBitmapHeader
ALTALUX_API bool __cdecl StartEffects2(
	HANDLE hDib,        ///< [in] Handle to 24 BPP DIB image data
	HWND hwnd,          ///< [in] Parent window for dialogs (IrfanView main window)
	int filter,         ///< [in] Effect selection (reserved, currently unused)
	RECT rect,          ///< [in] Selection rect: {left, top, WIDTH, HEIGHT}
	int param1,         ///< [in] Enhancement strength (0-100) or -1 for GUI
	int param2,         ///< [in] Tile grid size (2-16) or -1 for GUI
	char* iniFile,      ///< [in] Path to IrfanView INI file
	char* szAppName,    ///< [in] Application name (typically "IrfanView")
	int regID           ///< [in] Registration ID (reserved for future use)
);

/// @brief Alias for StartEffects2 (alternative entry point name)
/// @details Some versions of IrfanView may call AltaLux_Effects instead of
///          StartEffects2. This function has identical behavior.
/// @see StartEffects2
ALTALUX_API bool __cdecl AltaLux_Effects(
	HANDLE hDib,
	HWND hwnd,
	int filter,
	RECT rect,
	int param1,
	int param2,
	char* iniFile,
	char* szAppName,
	int regID
);

//-----------------------------------------------------------------------------
// Plugin Information Function
//-----------------------------------------------------------------------------

/// @brief Get plugin version and description information
/// @param versionString Output buffer for version number (must be at least 16 bytes)
/// @param fileFormats Output buffer for plugin description (must be at least 256 bytes)
/// @return Always returns 0 (success)
///
/// @details This function is called by IrfanView when loading the plugin to
///          retrieve identification information displayed in the UI.
///
/// @par Version String Format:
/// Version follows semantic versioning: "Major.Minor"
/// - Current version: "1.10"
/// - Format: "X.YZ" where X is major, YZ is minor
///
/// @par Description String:
/// Brief description of plugin functionality shown in IrfanView menus:
/// "AltaLux image enhancement filter"
///
/// @par Buffer Requirements:
/// - versionString: Minimum 16 bytes (actual: ~5 bytes used)
/// - fileFormats: Minimum 256 bytes (actual: ~35 bytes used)
/// IrfanView allocates these buffers, plugin writes to them.
///
/// @warning Do not write beyond buffer sizes. Use sprintf with known limits.
///
/// @par Usage by IrfanView:
/// @code
/// char version[16];
/// char description[256];
/// GetPlugInInfo(version, description);
/// // IrfanView displays: "AltaLux image enhancement filter v1.10"
/// @endcode
///
/// @note This function is called during plugin enumeration, before any
///       image processing occurs.
///
/// @see StartEffects2
ALTALUX_API int __cdecl GetPlugInInfo(
	char* versionString,  ///< [out] Buffer for version string (e.g., "1.10")
	char* fileFormats     ///< [out] Buffer for description string
);

} // extern "C"
