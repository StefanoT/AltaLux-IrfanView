/*
Project: AltaLux plugin for IrfanView
Author: Stefano Tommesani
Website: http://www.tommesani.com

Microsoft Public License (MS-PL) [OSI Approved License]
The full license text is in the LICENSE file at the root of the repository.
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
/// - **Advanced Enhancement**: Multiscale CLAHE pipeline for local contrast
/// - **High Performance**: Parallel layer processing with runtime scalar/SSSE3/AVX2 kernel dispatch
/// - **Multiple Formats**: Processes BGR24 and BGR32 data (Windows DIB order)
/// - **Configurable**: Strength, Detail, and Natural look controls, plus presets
/// - **Interactive Preview**: Draggable before/after split preview with fit and 1:1 modes
/// - **Dark Mode Support**: Native Windows dark mode integration
/// - **Selection Support**: Process entire image, selected region, or AI-assisted object selection (x64)
///
/// @section algorithm_sec Algorithm Overview
/// The v2 pipeline runs CLAHE at three tile scales (fine 16x16, balanced 8x8,
/// smooth 4x4 regions), blends the three layer outputs by weight, and lerps the
/// result against the original using Strength. Each CLAHE layer:
/// 1. Divides the luminance image into tiles (contextual regions)
/// 2. Computing histogram for each tile independently
/// 3. Clipping histogram to prevent noise amplification
/// 4. Equalizing each tile using its own histogram
/// 5. Interpolating between tiles to eliminate artifacts
///
/// @section usage_sec Usage
/// The plugin is loaded by IrfanView and appears in the Effects menu.
/// Users can:
/// - Adjust Strength, Detail, and Natural look (0-100 each)
/// - Start from the Natural, Balanced, or Detail presets
/// - Compare original and processed halves in the split preview
/// - Process the whole image or only the active selection
///
/// @section performance_sec Performance
/// Processing uses the parallel split-loop CLAHE filter and the best supported
/// scalar, SSSE3, or AVX2 kernel implementation selected at runtime.
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
/// @param param1 Strength (0-100) or -1 to show GUI
/// @param param2 Accepted for IrfanView signature compatibility; -1 shows the
///               GUI. In direct (batch) mode it no longer controls processing:
///               Detail and Natural look use their default value of 25.
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
/// - **Bit Depth**: 24 or 32 bits per pixel, single plane
/// - **Alignment**: Width and height are normalized to multiples of 8
/// - **Color Space**: BGR/BGRA byte order (standard Windows DIB layout)
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
///    - Display interactive dialog with before/after split preview
///    - Load previous settings from INI
///    - Let the user adjust Strength, Detail, and Natural look
///    - Offer Natural / Balanced / Detail presets
///    - Save settings to INI on OK
/// 2. **Direct Mode** (param1 >= 0 and param2 >= 0):
///    - Apply filter immediately with param1 as Strength
///    - Detail and Natural look use their default value (25)
///    - No GUI, no user interaction
///    - Useful for batch processing or automation
///
/// @par INI File Format:
/// Settings are stored in IrfanView's INI file under [AltaLux] section:
/// @code
/// [AltaLux]
/// Strength=45     ; Overall strength (0-100)
/// Detail=25       ; Blend toward the fine CLAHE layer (0-100)
/// NaturalLook=25  ; Blend toward the smooth CLAHE layer (0-100)
/// Zoom=0          ; 1 = open in 1:1 preview mode
/// @endcode
/// The legacy v1 key Intensity is read as a fallback when Strength is absent.
/// The v1 Scale key is no longer used.
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
/// - Current version: defined by ALTALUX_VERSION_DISPLAY_STRING in AltaLuxVersion.h
/// - Format: "X.YZ" where X is major, YZ is minor
/// - The same header drives the FILEVERSION/PRODUCTVERSION in AltaLux.rc
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
/// @warning Do not write beyond buffer sizes. Use bounded string writes.
///
/// @par Usage by IrfanView:
/// @code
/// char version[16];
/// char description[256];
/// GetPlugInInfo(version, description);
/// // IrfanView displays: "AltaLux image enhancement filter v2.00"
/// @endcode
///
/// @note This function is called during plugin enumeration, before any
///       image processing occurs.
///
/// @see StartEffects2
ALTALUX_API int __cdecl GetPlugInInfo(
	char* versionString,
	char* fileFormats
);

} // extern "C"
