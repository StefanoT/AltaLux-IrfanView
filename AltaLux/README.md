# AltaLux - Advanced Image Enhancement Plugin for IrfanView

## Overview

AltaLux is a high-performance image enhancement plugin for IrfanView that implements CLAHE (Contrast Limited Adaptive Histogram Equalization). CLAHE is an advanced algorithm that dramatically improves local contrast while preventing over-amplification of noise, making it ideal for enhancing photos with uneven lighting or low contrast.

**Author**: Stefano Tommesani  
**Website**: http://www.tommesani.com  
**Version**: 1.10  
**License**: Microsoft Public License (MS-PL)

## Features

### Core Capabilities
- ✨ **Advanced Enhancement**: State-of-the-art CLAHE algorithm for superior local contrast
- 🚀 **High Performance**: Multi-threaded processing utilizing modern multi-core CPUs (4× speedup on quad-core)
- 🎨 **Format Support**: RGB24, RGB32, BGR24, BGR32, and multiple YUV formats
- ⚙️ **Configurable**: Adjustable strength (0-100) and tile grid size (2×2 to 16×16)
- 👁️ **Interactive Preview**: Real-time preview with multiple parameter variations
- 🌙 **Dark Mode**: Native Windows dark mode support
- 🎯 **Selection Support**: Process entire image or selected region only

### Image Quality
- **Local Contrast**: Enhances detail in shadows and highlights independently
- **Noise Control**: Histogram clipping prevents amplification of sensor noise
- **Color Preservation**: Processes luminance while maintaining color balance
- **Artifact-Free**: Bilinear interpolation eliminates tile boundary artifacts

## Installation

1. Download the latest release (`AltaLux.dll`)
2. Copy `AltaLux.dll` to IrfanView's `Plugins` folder:
   - Default location: `C:\Program Files\IrfanView\Plugins\`
3. Restart IrfanView
4. Access via: `Image → Effects → AltaLux`

## Usage

### GUI Mode (Interactive)

1. Open an image in IrfanView
2. Select `Image → Effects → AltaLux`
3. Adjust parameters:
   - **Intensity Slider** (0-100): Enhancement strength
     - 0: No effect (pass-through)
     - 25: Recommended default
     - 100: Maximum enhancement
   - **Scale Slider** (2-16): Tile grid size
     - Lower values: Faster processing, coarser enhancement
     - 8: Recommended default (8×8 grid)
     - Higher values: Finer detail, slower processing
4. Preview variations:
   - **Center**: Current settings
   - **Top**: Weaker intensity
   - **Bottom**: Stronger intensity
   - **Left**: Coarser grid
   - **Right**: Finer grid
   - **Top-left corner**: Original image
5. Click **OK** to apply or **Cancel** to discard

### Batch Mode (Automation)

For batch processing or scripting:

```
i_view32.exe /effect=(AltaLux,25,8) input.jpg
```

Parameters: `(PluginName, Intensity, Scale)`

### Selection Processing

1. Select a region using IrfanView's selection tool
2. Apply AltaLux as usual
3. Only the selected region will be enhanced

## Technical Details

### CLAHE Algorithm

CLAHE enhances images through the following steps:

1. **Tile Division**: Image divided into NxN tiles (contextual regions)
2. **Histogram Calculation**: Compute gray-level histogram for each tile
3. **Histogram Clipping**: Clip bins exceeding threshold to prevent noise amplification
4. **Mapping Generation**: Create cumulative distribution function (equalization mapping)
5. **Interpolation**: Apply bilinear interpolation between tiles to eliminate artifacts

### Color Processing

AltaLux preserves color while enhancing contrast:

1. Convert RGB to luminance: `Y = 0.299*R + 0.587*G + 0.114*B`
2. Apply CLAHE to luminance channel
3. Calculate luminance difference: `ΔY = Y_enhanced - Y_original`
4. Adjust RGB channels: `R' = R + ΔY`, `G' = G + ΔY`, `B' = B + ΔY`
5. Clamp values to valid range [0, 255]

This approach maintains color ratios and prevents hue shifts.

### Performance Characteristics

#### Typical Processing Times (1920×1080 image, 8×8 tiles)

| Implementation           | Time    | Speedup | CPU Usage |
|--------------------------|---------|---------|-----------|
| Serial (single-threaded) | 1.2s    | 1.0×    | 1 core    |
| Parallel Split Loop      | 0.3s    | 4.0×    | All cores |
| Parallel Event           | 0.35s   | 3.4×    | All cores |
| Parallel Active Wait     | 0.3s    | 4.0×    | All cores |

**Default**: Parallel Split Loop (best balance of performance and simplicity)

#### Scalability
- Good scaling up to 8 CPU cores
- Diminishing returns beyond 8 cores (memory bandwidth becomes bottleneck)
- Preview mode uses downsampled image for real-time responsiveness

### Architecture

#### Class Hierarchy

```
CBaseAltaLuxFilter (abstract base)
├── CSerialAltaLuxFilter (reference implementation)
├── CParallelSplitLoopAltaLuxFilter (default, recommended)
├── CParallelErrorAltaLuxFilter (experimental)
├── CParallelEventAltaLuxFilter (synchronization study)
└── CParallelActiveWaitAltaLuxFilter (low-latency)
```

#### Key Components

- **CBaseAltaLuxFilter**: Abstract base defining CLAHE interface
- **CAltaLuxFilterFactory**: Factory for creating filter instances
- **ScopedBitmapHeader**: RAII wrapper for Windows DIB memory management
- **UIDraw**: GUI rendering and preview generation
- **AltaLux.cpp**: IrfanView plugin interface implementation

### Parallelization Strategy

#### Split-Loop Approach (Default)

The algorithm is divided into two phases with implicit synchronization:

**Phase 1: Histogram Calculation** (Embarrassingly Parallel)
```cpp
parallel_for(each_tile_row) {
    for (each_tile_in_row) {
        MakeHistogram(tile)
        ClipHistogram(tile)
        MapHistogram(tile)
    }
}
// Implicit barrier - all threads complete Phase 1
```

**Phase 2: Interpolation** (Embarrassingly Parallel)
```cpp
parallel_for(each_interpolation_row) {
    for (each_region_in_row) {
        Interpolate(region, neighboring_histograms)
    }
}
```

**Advantages**:
- No explicit synchronization code
- Near-linear scalability
- Good cache locality
- Simple to understand and maintain

## Configuration

### INI File Settings

Settings are stored in IrfanView's INI file under `[AltaLux]` section:

```ini
[AltaLux]
Intensity=25    ; Enhancement strength (0-100)
Scale=8         ; Tile grid size (2-16)
```

Location: `%APPDATA%\IrfanView\i_view32.ini` or installation directory

### Parameter Guidelines

#### Intensity (Strength)
- **0**: Disabled (pass-through mode, saves memory)
- **1-10**: Subtle enhancement (good for already well-exposed images)
- **15-25**: Moderate enhancement (recommended for most photos)
- **30-50**: Strong enhancement (for very dark or flat images)
- **60-100**: Extreme enhancement (may amplify noise, use carefully)

#### Scale (Tile Grid Size)
- **2×2**: Fastest, very coarse (global enhancement)
- **4×4**: Fast, coarse (good for large images)
- **8×8**: **Recommended** (best balance)
- **12×12**: Finer detail, slower
- **16×16**: Finest detail, slowest (use for small images or specific needs)

**Rule of Thumb**: More tiles = finer local detail but slower processing and more memory

### Image Size Considerations

| Image Resolution | Recommended Grid | Notes                           |
|------------------|------------------|---------------------------------|
| ≤ 1280×720       | 4×4 to 8×8       | Small images need fewer tiles   |
| 1920×1080        | 8×8              | Sweet spot for Full HD          |
| 2560×1440        | 8×8 to 12×12     | Can use finer grid              |
| 3840×2160 (4K)   | 12×12 to 16×16   | Benefit from finer detail       |

## Use Cases

### Recommended Applications
✅ **Excellent for:**
- Backlit photos (subjects in shadow)
- Night photography (reveal shadow detail)
- Scanned documents (improve readability)
- Medical imaging (enhance tissue detail)
- Surveillance footage (improve visibility)
- Low-contrast images (bring out hidden detail)
- HDR-like enhancement (single image)

❌ **Not recommended for:**
- Images with high sensor noise (will amplify noise)
- Already well-exposed photos (may look unnatural)
- Artistic photos with intentional lighting (may ruin the mood)

### Comparison with Other Techniques

| Technique                  | Global/Local | Noise Amplification | Speed      |
|----------------------------|--------------|---------------------|------------|
| Brightness/Contrast        | Global       | Low                 | Very Fast  |
| Curves/Levels              | Global       | Low                 | Very Fast  |
| Unsharp Mask               | Local        | High                | Fast       |
| Standard Histogram Eq.     | Global       | Very High           | Fast       |
| **CLAHE (AltaLux)**        | **Local**    | **Controlled**      | **Medium** |
| True HDR (multi-exposure)  | Local        | Very Low            | Slow       |

## Development

### Building from Source

#### Requirements
- Visual Studio 2017 or later
- Windows SDK
- C++14 or later

#### Build Steps
1. Open `AltaLux.sln` in Visual Studio
2. Select configuration:
   - **Release** for production use (optimized)
   - **Debug** for development (with symbols)
3. Select platform:
   - **x86** for 32-bit IrfanView
   - **x64** for 64-bit IrfanView
4. Build → Build Solution (F7)
5. Output: `Release\AltaLux.dll` or `x64\Release\AltaLux.dll`

### Project Structure

```
AltaLux/
├── AltaLux.cpp              # Plugin interface, main entry points
├── AltaLux.h                # Public API declarations
├── Filter/
│   ├── CBaseAltaLuxFilter.cpp        # Base class implementation
│   ├── CBaseAltaLuxFilter.h          # Base class interface
│   ├── CAltaLuxFilterFactory.cpp     # Factory implementation
│   ├── CAltaLuxFilterFactory.h       # Factory interface
│   ├── CSerialAltaLuxFilter.cpp      # Single-threaded reference
│   ├── CSerialAltaLuxFilter.h
│   ├── CParallelSplitLoopAltaLuxFilter.cpp  # Default parallel (recommended)
│   ├── CParallelSplitLoopAltaLuxFilter.h
│   ├── CParallelEventAltaLuxFilter.cpp      # Event-based parallel
│   ├── CParallelEventAltaLuxFilter.h
│   ├── CParallelActiveWaitAltaLuxFilter.cpp # Active-wait parallel
│   └── CParallelActiveWaitAltaLuxFilter.h
├── UIDraw/
│   ├── UIDraw.cpp           # GUI rendering and preview
│   └── UIDraw.h
├── ScopedBitmapHeader.h     # RAII memory management
└── resource.h               # Windows resource definitions
```

### Adding New Parallelization Strategies

To implement a new parallelization approach:

1. Create new class inheriting from `CBaseAltaLuxFilter`
2. Implement `Run()` method with your strategy
3. Add constant to `CAltaLuxFilterFactory.h`
4. Update factory in `CAltaLuxFilterFactory.cpp`
5. Benchmark against existing implementations

Example:
```cpp
// MyCustomFilter.h
class CMyCustomFilter : public CBaseAltaLuxFilter {
public:
    CMyCustomFilter(int w, int h, int hs, int vs)
        : CBaseAltaLuxFilter(w, h, hs, vs) {}
protected:
    int Run() override {
        // Your custom parallelization strategy
    }
};
```

## Troubleshooting

### Common Issues

**Problem**: Plugin doesn't appear in IrfanView menu  
**Solution**: 
- Verify DLL is in correct Plugins folder
- Check that DLL matches IrfanView architecture (32-bit vs 64-bit)
- Restart IrfanView completely

**Problem**: Image looks noisy after enhancement  
**Solution**:
- Reduce Intensity slider value
- Input image may have high sensor noise
- Consider noise reduction before applying AltaLux

**Problem**: Processing is slow  
**Solution**:
- Reduce Scale (fewer tiles = faster)
- Close other applications to free CPU
- Ensure using Release build (not Debug)

**Problem**: Colors look wrong  
**Solution**:
- AltaLux preserves color ratios; unusual results may indicate:
  - Image was already heavily processed
  - Extreme enhancement settings
  - Try reducing Intensity

**Problem**: Preview looks different from result  
**Solution**:
- Preview uses downsampled image for speed
- Final result processes full resolution
- This is normal and expected behavior

## Performance Tuning

### For Maximum Speed
- Use 4×4 or 6×6 grid (fewer tiles)
- Process at lower resolution first, then upscale
- Close other applications
- Use Parallel Split Loop (default)

### For Maximum Quality
- Use 12×12 or 16×16 grid (more tiles)
- Process at full resolution
- Adjust Intensity conservatively
- Compare with original frequently

### Memory Usage
- Intensity=0: Minimal (no processing buffer)
- Typical: ~ImageSize + (NumTiles × 256 × 4 bytes)
- Example: 1920×1080, 8×8 tiles ≈ 6MB + 64KB = ~6.1MB

## Algorithm References

The CLAHE algorithm is based on:

- Karel Zuiderveld, "Contrast Limited Adaptive Histogram Equalization," 
  Graphics Gems IV, Academic Press, 1994, pp. 474-485.

- Pizer, S. M., et al., "Adaptive histogram equalization and its variations,"
  Computer Vision, Graphics, and Image Processing, 1987.

## License

Microsoft Public License (MS-PL)

This is an OSI-approved open source license. Key points:
- ✅ Commercial use allowed
- ✅ Modification allowed
- ✅ Distribution allowed
- ✅ Private use allowed
- ⚠️ Must include license text
- ⚠️ Patent claims end if you sue over patents

Full license text in source files.

## Contributing

Contributions welcome! Areas of interest:
- SIMD optimizations (AVX2, SSE)
- ARM/NEON support
- Additional parallelization strategies
- GPU acceleration (CUDA/OpenCL)
- Algorithm improvements
- Documentation improvements

## Changelog

### Version 1.10 (Current)
- Comprehensive documentation and code comments
- Dark mode support for Windows 10/11
- Improved GUI with real-time preview
- Resizable dialog window
- Multiple preview variations
- Performance optimizations

### Version 1.00 (Initial)
- Initial release
- CLAHE implementation
- Multiple parallelization strategies
- IrfanView plugin interface

## Contact

**Author**: Stefano Tommesani  
**Website**: http://www.tommesani.com  
**Email**: [Contact via website]

## Acknowledgments

- IrfanView by Irfan Skiljan for the excellent image viewer
- Karel Zuiderveld for the original CLAHE algorithm
- Contributors and testers

---

**Made with ❤️ for the IrfanView community**
