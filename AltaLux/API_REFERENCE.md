# AltaLux API Reference

## Table of Contents
1. [Core Classes](#core-classes)
2. [Factory Pattern](#factory-pattern)
3. [Filter Implementations](#filter-implementations)
4. [Helper Classes](#helper-classes)
5. [Constants and Enumerations](#constants-and-enumerations)
6. [Plugin Interface](#plugin-interface)

---

## Core Classes

### CBaseAltaLuxFilter (Abstract Base Class)

Base class defining the CLAHE filter interface. All filter implementations inherit from this class.

#### Constructor
```cpp
CBaseAltaLuxFilter(
    int Width,          // Image width in pixels
    int Height,         // Image height in pixels
    int HorSlices = 8,  // Number of horizontal tiles (2-16)
    int VerSlices = 8   // Number of vertical tiles (2-16)
);
```

#### Public Methods

##### SetStrength
```cpp
void SetStrength(int strength = AL_DEFAULT_STRENGTH);
```
Configures enhancement intensity.
- **Parameters**: 
  - `strength`: 0-100 (0=disabled, 25=default, 100=maximum)
- **Effect**: Higher values = more dramatic contrast
- **Note**: 0 frees internal buffer to save memory

##### SetSlices
```cpp
void SetSlices(int HorSlices, int VerSlices);
```
Configures tile grid size.
- **Parameters**: 
  - `HorSlices`: 2-16 horizontal tiles
  - `VerSlices`: 2-16 vertical tiles
- **Effect**: More tiles = finer local detail, slower processing
- **Note**: Values automatically clamped to valid range

##### IsEnabled
```cpp
bool IsEnabled() const;
```
- **Returns**: `true` if filter will process images (strength > 0)

##### Process Methods
```cpp
int ProcessGray(void* Image);   // 8-bit grayscale
int ProcessRGB24(void* Image);  // 24-bit RGB (3 bytes/pixel)
int ProcessRGB32(void* Image);  // 32-bit RGB (4 bytes/pixel)
int ProcessBGR24(void* Image);  // 24-bit BGR (3 bytes/pixel)
int ProcessBGR32(void* Image);  // 32-bit BGR (4 bytes/pixel)
int ProcessUYVY(void* Image);   // UYVY packed format
int ProcessVYUY(void* Image);   // VYUY packed format
int ProcessYUYV(void* Image);   // YUYV packed format
int ProcessYVYU(void* Image);   // YVYU packed format
```
- **Parameters**: 
  - `Image`: Pointer to image data (must not be null)
- **Returns**: Error code (AL_OK on success)
- **Note**: Image dimensions must match constructor parameters

#### Protected Members
```cpp
int ImageWidth;              // Processing width
int ImageHeight;             // Processing height
int OriginalImageWidth;      // Original image width
int OriginalImageHeight;     // Original image height
unsigned char* ImageBuffer;  // Internal grayscale buffer
int Strength;                // Current strength (0-100)
unsigned int NumHorRegions;  // Horizontal tile count
unsigned int NumVertRegions; // Vertical tile count
int RegionWidth;             // Tile width in pixels
int RegionHeight;            // Tile height in pixels
float ClipLimit;             // Histogram clip limit
```

#### Protected Methods (For Derived Classes)

##### Run (Pure Virtual)
```cpp
virtual int Run() = 0;
```
Main processing method - must be implemented by derived classes.
- **Returns**: AL_OK on success, error code on failure
- **Implementation**: Each derived class provides different parallelization strategy

##### ClipHistogram
```cpp
void ClipHistogram(
    unsigned int* pHistogram,  // Histogram to clip (256 elements)
    unsigned int ClipLimit     // Maximum bin count
);
```
Clips histogram bins and redistributes excess pixels.

##### MakeHistogram
```cpp
void MakeHistogram(
    PixelType* pImage,         // Pointer to tile
    unsigned int* pHistogram   // Output histogram (256 elements)
);
```
Computes histogram of a tile.

##### MapHistogram
```cpp
void MapHistogram(
    unsigned int* pHistogram,  // Histogram to map (modified in-place)
    unsigned int NumOfPixels   // Total pixels in tile
);
```
Converts histogram to cumulative distribution function (equalization mapping).

##### Interpolate
```cpp
void Interpolate(
    PixelType* pImage,            // Region to interpolate
    unsigned int* pulMapLU,       // Upper-left tile mapping
    unsigned int* pulMapRU,       // Upper-right tile mapping
    unsigned int* pulMapLB,       // Lower-left tile mapping
    unsigned int* pulMapRB,       // Lower-right tile mapping
    unsigned int MatrixWidth,     // Region width
    unsigned int MatrixHeight     // Region height
);
```
Applies bilinear interpolation between four neighboring tile mappings.

---

## Factory Pattern

### CAltaLuxFilterFactory

Factory for creating filter instances with different parallelization strategies.

#### Static Methods

##### CreateAltaLuxFilter (Default Strategy)
```cpp
static CBaseAltaLuxFilter* CreateAltaLuxFilter(
    int Width,
    int Height,
    int HorSlices = DEFAULT_HOR_REGIONS,  // Default: 8
    int VerSlices = DEFAULT_VERT_REGIONS  // Default: 8
);
```
Creates filter using recommended implementation (ParallelSplitLoop).
- **Returns**: Pointer to filter instance or `nullptr` on failure
- **Note**: Caller responsible for deletion

##### CreateSpecificAltaLuxFilter
```cpp
static CBaseAltaLuxFilter* CreateSpecificAltaLuxFilter(
    int FilterType,    // Strategy constant (see below)
    int Width,
    int Height,
    int HorSlices = DEFAULT_HOR_REGIONS,
    int VerSlices = DEFAULT_VERT_REGIONS
);
```
Creates filter with specific implementation strategy.
- **FilterType**: One of:
  - `ALTALUX_FILTER_DEFAULT` (0) - ParallelSplitLoop
  - `ALTALUX_FILTER_SERIAL` (1) - Single-threaded
  - `ALTALUX_FILTER_PARALLEL_SPLIT_LOOP` (2) - Two-phase parallel
  - `ALTALUX_FILTER_PARALLEL_ERROR` (3) - Error-based sync
  - `ALTALUX_FILTER_PARALLEL_EVENT` (4) - Event-based sync
  - `ALTALUX_FILTER_ACTIVE_WAIT` (5) - Active waiting
- **Returns**: Pointer to filter instance or `nullptr` on failure

#### Usage Example
```cpp
// Recommended (production use)
CBaseAltaLuxFilter* filter = 
    CAltaLuxFilterFactory::CreateAltaLuxFilter(1920, 1080, 8, 8);

// For benchmarking specific implementation
CBaseAltaLuxFilter* serialFilter = 
    CAltaLuxFilterFactory::CreateSpecificAltaLuxFilter(
        ALTALUX_FILTER_SERIAL, 1920, 1080, 8, 8);

// Use filter
if (filter) {
    filter->SetStrength(25);
    int result = filter->ProcessRGB24(imageData);
    delete filter;  // Caller must delete
}
```

---

## Filter Implementations

### CSerialAltaLuxFilter

Single-threaded reference implementation.

```cpp
CSerialAltaLuxFilter(
    int Width,
    int Height,
    int HorSlices = DEFAULT_HOR_REGIONS,
    int VerSlices = DEFAULT_VERT_REGIONS
);
```

**Characteristics**:
- Simplest implementation
- Slowest performance (~1.2s for Full HD)
- Best for debugging and validation
- Deterministic behavior

**Use Cases**:
- Unit testing
- Algorithm validation
- Baseline for performance comparison
- Single-core systems

### CParallelSplitLoopAltaLuxFilter (Recommended)

Two-phase parallel implementation with implicit synchronization.

```cpp
CParallelSplitLoopAltaLuxFilter(
    int Width,
    int Height,
    int HorSlices = DEFAULT_HOR_REGIONS,
    int VerSlices = DEFAULT_VERT_REGIONS
);
```

**Characteristics**:
- Fast performance (~0.3s for Full HD, 4× speedup)
- Simple implementation (uses `concurrency::parallel_for`)
- Good scalability (up to 8 cores)
- Automatic thread management

**Algorithm**:
1. **Phase 1**: Parallel histogram calculation
   - All tiles processed concurrently
   - No dependencies between threads
2. **Implicit Barrier**: All Phase 1 threads complete
3. **Phase 2**: Parallel interpolation
   - All regions processed concurrently
   - Reads shared histograms (read-only)

**Use Cases**:
- Production deployment (default choice)
- General-purpose processing
- Best balance of performance and complexity

### CParallelEventAltaLuxFilter

Event-based synchronization using Windows kernel objects.

```cpp
CParallelEventAltaLuxFilter(
    int Width,
    int Height,
    int HorSlices = DEFAULT_HOR_REGIONS,
    int VerSlices = DEFAULT_VERT_REGIONS
);
```

**Characteristics**:
- Explicit thread coordination using events
- Moderate performance (~0.35s for Full HD)
- Higher overhead (kernel transitions)
- Complex implementation

**Use Cases**:
- Educational (understanding synchronization)
- Experimentation with explicit coordination

### CParallelActiveWaitAltaLuxFilter

Busy-waiting synchronization (spin locks).

```cpp
CParallelActiveWaitAltaLuxFilter(
    int Width,
    int Height,
    int HorSlices = DEFAULT_HOR_REGIONS,
    int VerSlices = DEFAULT_VERT_REGIONS
);
```

**Characteristics**:
- Lowest latency synchronization
- Good performance (~0.3s for Full HD)
- Consumes CPU while waiting
- Best with dedicated cores

**Use Cases**:
- Real-time processing requirements
- Systems with dedicated CPU cores
- Low-latency applications

---

## Helper Classes

### ScopedBitmapHeader

RAII wrapper for Windows DIB memory management.

```cpp
class ScopedBitmapHeader {
public:
    ScopedBitmapHeader(HANDLE hDib);  // Locks memory
    ~ScopedBitmapHeader();             // Unlocks memory
    
    LPBITMAPINFOHEADER operator->() const;  // Access header members
    BITMAPINFOHEADER operator*() const;     // Get header copy
    BYTE* GetImageBits() const;             // Get pixel data pointer
};
```

#### Usage Example
```cpp
// Automatic memory management (exception-safe)
{
    ScopedBitmapHeader header(hDib);
    int width = header->biWidth;      // Access member
    int height = header->biHeight;    // Access member
    BYTE* pixels = header.GetImageBits();
    // Process image...
}  // Memory automatically unlocked here
```

**Features**:
- Automatic locking/unlocking
- Exception-safe (RAII pattern)
- Non-copyable (prevents double-unlock)
- Prevents memory leaks

---

## Constants and Enumerations

### Error Codes
```cpp
const int AL_OK = 0;                    // Success
const int AL_NULL_IMAGE = -1;           // Null image pointer
const int AL_WIDTH_NO_MULTIPLE = -3;    // Width not multiple of 8
const int AL_HEIGHT_NO_MULTIPLE = -4;   // Height not multiple of 8
const int AL_OUT_OF_MEMORY = -11;       // Memory allocation failed
```

### Strength Parameters
```cpp
const int AL_MIN_STRENGTH = 0;          // Disabled (pass-through)
const int AL_DEFAULT_STRENGTH = 25;     // Recommended default
const int AL_MAX_STRENGTH = 100;        // Maximum enhancement
const int AL_LIGHT_CONTRAST_STRENGTH = 5;
const int AL_HEAVY_CONTRAST_STRENGTH = 10;
```

### Tile Configuration
```cpp
const unsigned int MIN_HOR_REGIONS = 2;     // Minimum tiles
const unsigned int DEFAULT_HOR_REGIONS = 8;  // Default tiles
const unsigned int MAX_HOR_REGIONS = 16;    // Maximum tiles
const unsigned int MIN_VERT_REGIONS = 2;
const unsigned int DEFAULT_VERT_REGIONS = 8;
const unsigned int MAX_VERT_REGIONS = 16;
```

### Histogram Constants
```cpp
const unsigned int NUM_GRAY_LEVELS = 256;   // 8-bit histogram
const unsigned int MAX_GRAY_VALUE = 255;
const unsigned int MIN_GRAY_VALUE = 0;
```

### Clip Limits
```cpp
const float MIN_CLIP_LIMIT = 1.0f;      // Conservative
const float DEFAULT_CLIP_LIMIT = 2.0f;  // Balanced
const float MAX_CLIP_LIMIT = 5.0f;      // Aggressive
```

---

## Plugin Interface

### IrfanView Entry Points

#### StartEffects2 / AltaLux_Effects
```cpp
ALTALUX_API bool __cdecl StartEffects2(
    HANDLE hDib,        // DIB handle
    HWND hwnd,          // Parent window
    int filter,         // Reserved (unused)
    RECT rect,          // Selection: {left, top, WIDTH, HEIGHT}
    int param1,         // Strength (0-100) or -1 for GUI
    int param2,         // Scale (2-16) or -1 for GUI
    char* iniFile,      // INI file path
    char* szAppName,    // Application name
    int regID           // Reserved (unused)
);
```

**Parameters**:
- `hDib`: Windows global memory handle containing DIB
- `hwnd`: Parent window for dialogs
- `filter`: Reserved for future use (currently unused)
- `rect`: **Special format** - {X_offset, Y_offset, WIDTH, HEIGHT}
  - Note: `.right` is WIDTH, `.bottom` is HEIGHT (not coordinates!)
- `param1`: Enhancement strength or -1 to show GUI
- `param2`: Tile grid size or -1 to show GUI
- `iniFile`: Path to IrfanView INI file for settings
- `szAppName`: Application name (typically "IrfanView")
- `regID`: Reserved for future registration system

**Returns**:
- `true`: Processing succeeded (or user clicked OK in GUI)
- `false`: Processing failed or unsupported format

**Modes**:
1. **GUI Mode** (param1 == -1 OR param2 == -1):
   - Shows interactive dialog
   - Loads settings from INI
   - Real-time preview
   - Saves settings to INI on OK
2. **Direct Mode** (param1 >= 0 AND param2 >= 0):
   - Applies filter immediately
   - No GUI, no user interaction
   - Useful for batch processing

#### GetPlugInInfo
```cpp
ALTALUX_API int __cdecl GetPlugInInfo(
    char* versionString,  // Output: version number
    char* fileFormats     // Output: plugin description
);
```

**Parameters**:
- `versionString`: Buffer for version (e.g., "1.10")
- `fileFormats`: Buffer for description (e.g., "AltaLux image enhancement filter")

**Returns**: Always 0 (success)

---

## Complete Usage Example

```cpp
#include "Filter/CAltaLuxFilterFactory.h"
#include "ScopedBitmapHeader.h"

// Process image with AltaLux
bool ProcessImage(HANDLE hDib, int strength, int gridSize) {
    // Lock and access DIB
    ScopedBitmapHeader header(hDib);
    
    // Validate format
    if (header->biBitCount != 24 && header->biBitCount != 32) {
        return false;  // Unsupported bit depth
    }
    
    // Get image properties
    int width = abs(header->biWidth);
    int height = abs(header->biHeight);
    int bitDepth = header->biBitCount / 8;
    
    // Create filter
    CBaseAltaLuxFilter* filter = 
        CAltaLuxFilterFactory::CreateAltaLuxFilter(
            width, height, gridSize, gridSize);
    
    if (!filter) {
        return false;  // Failed to create filter
    }
    
    // Configure filter
    filter->SetStrength(strength);
    
    // Get pixel data
    BYTE* pixels = header.GetImageBits();
    
    // Process based on format
    int result;
    if (bitDepth == 3) {
        result = filter->ProcessRGB24(pixels);
    } else {
        result = filter->ProcessRGB32(pixels);
    }
    
    // Clean up
    delete filter;
    
    return (result == AL_OK);
}  // Memory automatically unlocked here

// Usage
int main() {
    HANDLE hDib = LoadDIBFromFile("image.bmp");
    bool success = ProcessImage(hDib, 25, 8);
    SaveDIBToFile(hDib, "enhanced.bmp");
    GlobalFree(hDib);
    return success ? 0 : 1;
}
```

---

## Thread Safety

### Thread-Safe Operations
- Creating multiple filter instances (different objects)
- Processing different images concurrently (different hDib handles)
- Factory methods (CreateAltaLuxFilter, CreateSpecificAltaLuxFilter)

### NOT Thread-Safe
- Using same filter instance from multiple threads
- Processing same image (hDib) from multiple threads
- Modifying filter parameters while processing
- GlobalLock/GlobalUnlock of same hDib from multiple threads

### Best Practices
```cpp
// Good: Each thread has own filter
std::vector<std::thread> threads;
for (int i = 0; i < numImages; i++) {
    threads.emplace_back([i]() {
        CBaseAltaLuxFilter* filter = 
            CAltaLuxFilterFactory::CreateAltaLuxFilter(w, h);
        filter->ProcessRGB24(images[i]);
        delete filter;
    });
}

// Bad: Sharing filter between threads
CBaseAltaLuxFilter* sharedFilter = 
    CAltaLuxFilterFactory::CreateAltaLuxFilter(w, h);
std::thread t1([&]() { sharedFilter->ProcessRGB24(image1); });
std::thread t2([&]() { sharedFilter->ProcessRGB24(image2); });
// RACE CONDITION!
```

---

## Performance Tips

### Optimal Settings
```cpp
// Fast processing (coarse detail)
filter->SetSlices(4, 4);   // 16 tiles total
filter->SetStrength(25);

// Balanced (recommended)
filter->SetSlices(8, 8);   // 64 tiles total
filter->SetStrength(25);

// High quality (fine detail)
filter->SetSlices(12, 12); // 144 tiles total
filter->SetStrength(25);
```

### Memory Considerations
```cpp
// Disable filter when not needed to save memory
filter->SetStrength(0);  // Frees internal buffer

// Re-enable for processing
filter->SetStrength(25); // Reallocates buffer
```

### Choosing Implementation
```cpp
// Production: Use default (ParallelSplitLoop)
auto filter = CAltaLuxFilterFactory::CreateAltaLuxFilter(w, h);

// Debugging: Use serial
auto filter = CAltaLuxFilterFactory::CreateSpecificAltaLuxFilter(
    ALTALUX_FILTER_SERIAL, w, h);

// Low-latency: Use active wait (if dedicated cores available)
auto filter = CAltaLuxFilterFactory::CreateSpecificAltaLuxFilter(
    ALTALUX_FILTER_ACTIVE_WAIT, w, h);
```

---

**End of API Reference**
