# AltaLux - Image Enhancement Plugin for IrfanView

AltaLux is a native IrfanView effects plugin that enhances local contrast using CLAHE
(Contrast Limited Adaptive Histogram Equalization). The current implementation uses a
fixed three-layer multiscale pipeline and a modern compare-first dialog with three
high-level controls.

Author: Stefano Tommesani  
Website: http://www.tommesani.com  
Version: 2.0.0.0  
License: Microsoft Public License (MS-PL)

## Current User Experience

The v2 dialog is built around a large before/after preview and three primary sliders:

- **Strength**: overall enhancement amount.
- **Detail**: shifts the blend toward the fine-detail layer.
- **Natural look**: shifts the blend toward the smoother, more natural layer.

The UI also includes:

- A draggable vertical split preview.
- Spacebar hold-to-compare, including when child controls have focus.
- Double-click on the split divider to recenter it.
- A `Zoom` toggle for fit-to-preview vs. 1:1 crop preview.
- Three presets: `Natural`, `Balanced`, and `Detail`.
- Short helper text under `Detail` and `Natural look`.

The old v1 `Intensity` + `Scale` UI and preview-variation grid are no longer the
primary interface.

## Presets

| Preset | Strength | Detail | Natural look |
|--------|----------|--------|--------------|
| Natural | 35 | 10 | 55 |
| Balanced | 45 | 25 | 25 |
| Detail | 55 | 60 | 10 |

The default startup state is the `Balanced` preset unless saved settings override it.

## How The Multiscale Pipeline Works

AltaLux v2 processes each image through three fixed CLAHE passes, then blends the
three outputs and mixes the blended result back with the original image.

Important terminology: the constants are region counts, not pixel tile sizes.
More regions means smaller CLAHE tiles and more local/detail-sensitive behavior.

| Layer | Constant | Region grid | Visual role |
|-------|----------|-------------|-------------|
| Fine | `Constants::FineRegions` | 16 x 16 | Small tiles, more local detail and texture |
| Balanced | `Constants::BalancedRegions` | 8 x 8 | Main enhancement backbone |
| Smooth | `Constants::SmoothRegions` | 4 x 4 | Large tiles, smoother/natural rendering |

The processing order in `ProcessMultiscaleImage()` is:

1. Create the fine, balanced, and smooth CLAHE layer outputs.
2. Compute blend weights from `Detail` and `Natural look`.
3. Accumulate the weighted RGB/BGR channels into an integer accumulator.
4. Blend the accumulated enhancement back toward the original using `Strength`.
5. Preserve alpha for RGB32/BGRA images.

Current implementation detail: `Strength` directly controls the final blend amount.
The CLAHE strength used inside the three layer passes is derived from `Strength`
with a conservative non-linear curve, so high slider values do not drive the
internal CLAHE pass all the way to `100`.

Representative internal layer-strength mapping:

| Strength | Layer CLAHE strength |
|----------|----------------------|
| 0 | 0 |
| 10 | 11 |
| 25 | 15 |
| 45 | 20 |
| 60 | 26 |
| 75 | 31 |
| 90 | 38 |
| 100 | 42 |

## Blend Weights

`ComputeBlendWeights()` derives layer weights from the two shaping sliders:

- Base weights: fine `0.15`, balanced `0.60`, smooth `0.25`.
- `Detail` can shift up to `0.35` toward fine.
- `Natural look` can shift up to `0.35` toward smooth.
- Balanced is floored at `0.20`.
- Weights are normalized internally.

`Detail` and `Natural look` are independent. They are not opposites, and both can be
raised at the same time.

## Color Processing

Windows DIB data is handled as BGR/BGRA in the plugin path. The multiscale pipeline
routes layer processing through the BGR filter variants so luminance coefficients
match the actual byte order.

The CLAHE filter:

1. Extracts luminance using BT.709 coefficients:
   `Y = 0.2126 R + 0.7152 G + 0.0722 B`.
2. Applies CLAHE to the luminance buffer.
3. Reinjects luminance through multiplicative color scaling.
4. Uses a Q16 reciprocal lookup table to avoid per-pixel division.
5. Caps the scale factor to avoid channel overflow and hue drift in highlights.

This is not the old additive `R/G/B + deltaY` model.

## Performance

The default single-scale CLAHE implementation is `CParallelSplitLoopAltaLuxFilter`.
The v2 pipeline runs three CLAHE passes, so it is more expensive than v1.

Additional v2 optimizations currently implemented:

- SIMD accumulation and final blend paths.
- Sequential layer processing for small images to avoid unnecessary task and memory overhead.
- Parallel layer processing for images at or above `1,000,000` pixels using
  `concurrency::parallel_invoke`.
- Blocked parallel accumulation/blending for larger buffers.

The thresholded layer strategy means:

- Preview-sized images usually use the memory-efficient sequential layer path.
- Large final images can process fine, balanced, and smooth layers concurrently.
- Three separate layer buffers are allocated only for the large-image parallel path.

Performance can still be memory-bandwidth limited because each layer is a full image
buffer and the CLAHE filters are internally parallel too.

## Memory Use

For RGB24/RGB32 multiscale processing, the core allocations are:

- An accumulator with three `uint32` values per pixel.
- A scratch layer buffer for the sequential path.
- Three layer buffers for the large-image parallel path.
- CLAHE internal luminance buffers and histogram mapping arrays.

For large images, the parallel layer path intentionally trades memory for wall-clock
latency.

## Installation

1. Build or download `AltaLux.dll`.
2. Copy it to the IrfanView `Plugins` folder.
   - 64-bit default: `C:\Program Files\IrfanView\Plugins\`
   - 32-bit default: `C:\Program Files (x86)\IrfanView\Plugins\`
3. Restart IrfanView.
4. Open from IrfanView's image effects menu.

Use a DLL build that matches the IrfanView architecture.

## Interactive Usage

1. Open an image in IrfanView.
2. Launch AltaLux from the effects menu.
3. Adjust `Strength`, `Detail`, and `Natural look`.
4. Drag the preview split divider to compare original vs. processed.
5. Hold `Space` to temporarily show the original.
6. Use `Natural`, `Balanced`, or `Detail` presets as starting points.
7. Use `Zoom` to switch between fit preview and 1:1 crop preview.
8. Press `OK` to apply or `Cancel` to discard.

If an IrfanView selection is active, AltaLux processes only the selected/cropped area.

## Direct Invocation / Batch Parameters

The current direct/non-dialog path is intentionally simplified for v2. It no longer
preserves the old v1 `(Intensity, Scale)` parameter model.

When AltaLux is invoked with parameters instead of opening the dialog:

- `param1` initializes `Strength`.
- `Detail` uses the default value `25`.
- `Natural look` uses the default value `25`.
- `param2` is not used as a scale/tile parameter.

This is a breaking change from v1 and is intentional for the current implementation.

## Settings

Settings are stored under the `[AltaLux]` section:

```ini
[AltaLux]
Strength=45
Detail=25
NaturalLook=25
Zoom=0
```

The old `Intensity` key is read as a fallback only when `Strength` is absent.
The old `Scale` setting is not surfaced in the v2 UI or parameter model.

## Project Structure

```text
AltaLux/
  AltaLux.cpp                 IrfanView plugin entry points and Win32 dialog
  AltaLuxCore.cpp/.h          v2 state, weights, geometry, multiscale processing
  ScopedBitmapHeader.h        RAII wrapper around DIB GlobalLock/GlobalUnlock
  UIDraw/
    UIDraw.cpp/.h             preview rendering and split comparison drawing
  Filter/
    CBaseAltaLuxFilter.*      shared CLAHE implementation
    CAltaLuxFilterFactory.*   filter factory
    CSerialAltaLuxFilter.*    serial reference implementation
    CParallelSplitLoop*       default parallel implementation
    CParallelEvent*           event-based implementation
    CParallelActiveWait*      active-wait implementation
AltaLuxUnitTest/
  TestStrategies.cpp          Microsoft C++ unit tests
AltaLuxBench/
  legacy benchmark project
```

## Building

Requirements:

- Visual Studio with MSVC toolset `v143` or newer.
- Windows SDK 10.
- Microsoft C++ Unit Test framework for the test project.

The project files still contain legacy output paths targeting IrfanView plugin
folders. For local development it is safer to override `OutDir` and `IntDir`.

Example x64 plugin build:

```powershell
New-Item -ItemType Directory -Force .build-out,.build-out\obj | Out-Null
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" `
  AltaLux\AltaLux.vcxproj `
  /p:Configuration=Debug `
  /p:Platform=x64 `
  /p:OutDir="$PWD\.build-out\" `
  /p:IntDir="$PWD\.build-out\obj\" `
  /p:TargetName=AltaLux
```

Example unit test build:

```powershell
New-Item -ItemType Directory -Force .test-out,.test-out\obj | Out-Null
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" `
  AltaLuxUnitTest\AltaLuxUnitTest.vcxproj `
  /p:Configuration=Debug `
  /p:Platform=Win32 `
  /p:OutDir="$PWD\.test-out\" `
  /p:IntDir="$PWD\.test-out\obj\"
```

Run tests with:

```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\Extensions\TestPlatform\vstest.console.exe" `
  ".test-out\AltaLuxUnitTest.dll"
```

## Tests

The current unit test suite covers:

- Serial vs. parallel CLAHE strategy equivalence.
- Presets and preset tolerance.
- Blend-weight normalization and balanced floor.
- Conservative non-linear layer-strength mapping.
- Independent `Detail` and `Natural look` behavior.
- Safe layer region clamping.
- Fine/balanced/smooth region ordering.
- Preview rectangle fitting.
- Zero-strength no-op behavior.
- RGB32 alpha preservation.
- RGB24/RGB32 consistency for identical RGB content.
- `Detail` sensitivity on checkerboard input.
- `Natural look` sensitivity on gradient input.
- A `1024 x 1024` image path that exercises the parallel-layer threshold.

At the time of this update, the test suite contains 20 tests.

## Troubleshooting

**Plugin does not appear in IrfanView**

- Confirm the DLL is in the correct IrfanView `Plugins` folder.
- Confirm DLL architecture matches IrfanView architecture.
- Restart IrfanView after copying the DLL.

**The result looks noisy**

- Lower `Strength`.
- Lower `Detail`.
- Use the `Natural` preset as a starting point.
- Apply noise reduction before AltaLux for high-ISO images.

**The result looks too flat or too smooth**

- Increase `Detail`.
- Lower `Natural look`.
- Increase `Strength` moderately.

**Processing is slow**

- Use a Release build for normal use.
- Large images run three CLAHE passes.
- The large-image path parallelizes the three layer passes, but performance can still be limited by memory bandwidth.

**Preview and final output differ slightly**

- Preview processing uses a scaled working image.
- Final apply processes the selected/full-resolution region.

## Changelog

### Version 2.0.0.0

- Replaced v1 `Intensity` + `Scale` UI with `Strength`, `Detail`, and `Natural look`.
- Removed the preview-variation grid from the primary workflow.
- Added large before/after split preview with draggable divider.
- Added spacebar hold-to-compare and split double-click recenter.
- Added Natural/Balanced/Detail presets.
- Added fixed three-layer multiscale pipeline.
- Added conservative non-linear internal CLAHE strength mapping.
- Corrected layer semantics: fine = 16 regions, balanced = 8 regions, smooth = 4 regions.
- Added BT.709 luminance extraction and BGR/BGRA routing for Windows DIB data.
- Added SIMD accumulation/blending.
- Added thresholded parallel processing for the three independent CLAHE layer passes.
- Added Microsoft C++ unit coverage for core v2 behavior and parallel-layer threshold.

### Version 1.9.1.92

- v1 CLAHE dialog with intensity and scale controls.
- Preview variation grid.
- Dark mode support.
- Multiple parallel CLAHE strategies.

### Version 1.0

- Initial IrfanView plugin integration.
- CLAHE implementation.

## References

- Karel Zuiderveld, "Contrast Limited Adaptive Histogram Equalization,"
  Graphics Gems IV, Academic Press, 1994.
- Pizer, S. M., et al., "Adaptive histogram equalization and its variations,"
  Computer Vision, Graphics, and Image Processing, 1987.

## License

Microsoft Public License (MS-PL). See source headers for license text.
