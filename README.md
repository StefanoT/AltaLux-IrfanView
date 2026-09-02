# AltaLux - Image Enhancement Plugin for IrfanView

AltaLux is a native IrfanView effects plugin for local contrast enhancement. It is
built on CLAHE (Contrast Limited Adaptive Histogram Equalization), but version 2
wraps the algorithm in a simpler user model: one overall strength control, two
look-shaping controls, and a large before/after preview.

Author: Stefano Tommesani

Website: http://www.tommesani.com

Version: 3.0.0.0

License: Microsoft Public License (MS-PL)

## V2 At A Glance

- Replaces the v1 `Intensity` and `Scale` workflow with `Strength`, `Detail`, and
  `Natural look`.
- Adds a large draggable before/after split preview with fit and 1:1 preview modes.
- Adds `Natural`, `Balanced`, and `Detail` presets.
- Introduces a fixed three-layer multiscale pipeline: fine, balanced, and smooth.
- Uses BT.709 luminance, BGR/BGRA-aware processing, and multiplicative color
  reinjection to reduce hue and highlight drift.
- Preserves RGB32 alpha and keeps RGB24/RGB32 behavior aligned for the same RGB
  content.
- Adds runtime kernel dispatch across scalar, SSSE3, and AVX2 implementations,
  plus thresholded parallel layer processing for large images.
- Expands the Microsoft C++ unit test suite to 30 tests covering the v2 core and
  SIMD kernel equivalence.

## Installation

1. Build or download `AltaLux.dll`.
2. Copy it to the IrfanView `Plugins` folder.
   - 64-bit default: `C:\Program Files\IrfanView\Plugins\`
   - 32-bit default: `C:\Program Files (x86)\IrfanView\Plugins\`
3. Restart IrfanView.
4. Open AltaLux from IrfanView's image effects menu.

Use a DLL build that matches the IrfanView architecture.

### Optional AI object selection

The x64 plug-in can apply AltaLux through MobileSAM-generated object masks. The
base `AltaLux.dll` remains fully functional without the AI files. To enable
object selection, extract every file from `AltaLux-AI-x64.zip` directly into
the same IrfanView `Plugins` folder as `AltaLux.dll`; do not create a subfolder.

The add-on runs locally through ONNX Runtime's CPU provider. No image data is
uploaded and AltaLux does not download models at runtime.

## Interactive Usage

1. Open an image in IrfanView.
2. Launch AltaLux from the effects menu.
3. Adjust `Strength`, `Detail`, and `Natural look`.
4. Drag the vertical divider to compare original and processed output.
5. Hold `Space` to temporarily show the original.
6. Double-click the divider to recenter it.
7. Use `Natural`, `Balanced`, or `Detail` as starting presets.
8. Use `Zoom` to switch between fit preview and 1:1 crop preview.
9. Press `OK` to apply or `Cancel` to discard.

When the optional AI package is installed, use `Select objects`, choose `Add`
or `Remove`, and click objects in the preview. `Undo`, `Clear`, `Select all`,
the mask overlay, and `Edge softness` refine how the filtered result is blended.

If an IrfanView selection is active, AltaLux processes only the selected area.

## Controls

### Strength

Controls how strongly CLAHE is applied inside the multiscale layers.

- `0` is a no-op.
- Mid values are intended for normal use.
- High values increase local contrast but can reveal noise or make already
  well-exposed images look less natural.

### Detail

Moves the layer blend toward the fine 16-region CLAHE pass. Raise it when you want
more small-structure contrast, such as texture, scanned document detail, or fine
shadow information.

### Natural Look

Moves the layer blend toward the smooth 4-region CLAHE pass. Raise it when the
image starts looking too busy, gritty, or locally over-processed.

`Detail` and `Natural look` are independent controls. They are not opposites, and
both can be raised at the same time.

## Presets

| Preset | Strength | Detail | Natural look |
| --- | ---: | ---: | ---: |
| Natural | 35 | 10 | 55 |
| Balanced | 45 | 25 | 25 |
| Detail | 55 | 60 | 10 |

The default startup state is `Balanced` unless saved settings override it.

## Processing Model

AltaLux v2 processes RGB24/RGB32 image data through three fixed CLAHE passes,
blends those layer outputs, and writes the weighted multiscale result directly.

The layer constants are region counts, not pixel tile sizes. More regions means
smaller CLAHE tiles and more local/detail-sensitive behavior.

| Layer | Constant | Region grid | Visual role |
| --- | --- | --- | --- |
| Fine | `Constants::FineRegions` | 16 x 16 | Small tiles, local detail and texture |
| Balanced | `Constants::BalancedRegions` | 8 x 8 | Main enhancement backbone |
| Smooth | `Constants::SmoothRegions` | 4 x 4 | Larger tiles, smoother tonal rendering |

The processing order in `ProcessMultiscaleImage()` is:

1. Copy the source image into a layer buffer.
2. Process the fine, balanced, and smooth CLAHE layers.
3. Compute blend weights from `Detail` and `Natural look`.
4. Accumulate weighted BGR channels into an integer accumulator.
5. Write the weighted multiscale enhancement to the output image.
6. Preserve alpha for RGB32/BGRA images.

### Layer Strength Curve

`Strength` controls the CLAHE strength used inside each layer through a
conservative non-linear curve, so high slider values do not drive the internal
CLAHE pass to `100`.

| User strength | Internal CLAHE layer strength |
| ---: | ---: |
| 0 | 0 |
| 10 | 3 |
| 25 | 11 |
| 45 | 25 |
| 60 | 37 |
| 75 | 50 |
| 90 | 65 |
| 100 | 75 |

### Blend Weights

`ComputeBlendWeights()` starts from these base weights:

| Layer | Base weight |
| --- | ---: |
| Fine | 0.15 |
| Balanced | 0.60 |
| Smooth | 0.25 |

`Detail` can shift up to `0.35` toward the fine layer. `Natural look` can shift up
to `0.35` toward the smooth layer. The balanced layer is floored at `0.20`, and
weights are normalized before processing.

## Color And Format Handling

Windows DIB image data is handled as BGR/BGRA in the plugin path. The v2 pipeline
routes layer processing through the BGR filter variants so the luminance
coefficients match the actual channel order.

The CLAHE filter:

1. Extracts luminance using BT.709 coefficients:
   `Y = 0.2126 R + 0.7152 G + 0.0722 B`.
2. Applies CLAHE to the luminance buffer.
3. Reinjects luminance by multiplicative color scaling.
4. Uses a Q16 reciprocal lookup table instead of per-pixel division.
5. Caps the scale factor so saturated highlights do not drift in hue.

This is not the old additive `R/G/B + deltaY` model. It is designed to preserve
channel ratios better while still enhancing local contrast.

## Performance And Memory

Version 2 does more work than the old single-scale path because it runs three
CLAHE passes. The implementation adds several compensating optimizations:

- Runtime selection of the best supported kernel path: AVX2, then SSSE3, then
  scalar.
- SIMD paths for RGB/BGR luma extraction and injection, packed YUV luma
  extraction and injection, 2x box downscaling, multiscale accumulation, and
  weighted output.
- Sequential layer processing for smaller images to avoid task and allocation
  overhead.
- Blocked parallel accumulation/output for images at or above 200,000 pixels.
- Parallel processing of the fine, balanced, and smooth layer passes for images at
  or above 1,000,000 pixels.
- A 1 KiB reciprocal lookup table for color scaling, replacing the older 64 KiB
  two-dimensional scale table.
- SSSE3 and AVX2 implementations for legacy packed YUV luma paths instead of
  32-bit inline assembly.
- Scalar fallbacks for operations where SIMD is not a clear win or cannot safely
  express the required memory access pattern.

For RGB24/RGB32 multiscale processing, the main allocations are:

- An accumulator with three `uint32` values per pixel.
- One scratch layer buffer for the sequential layer path.
- Three full layer buffers for the large-image parallel layer path.
- CLAHE internal luminance buffers and histogram mapping arrays.

The large-image path intentionally trades memory for wall-clock latency.

## Settings

Settings are stored under the `[AltaLux]` section:

```ini
[AltaLux]
Strength=45
Detail=25
NaturalLook=25
Zoom=0
```

The old `Intensity` key is read as a fallback only when `Strength` is absent. The
old `Scale` setting is not surfaced in the v2 UI or parameter model.

## Direct Invocation And Batch Parameters

The current non-dialog path is intentionally simplified for v2. It no longer
preserves the old v1 `(Intensity, Scale)` parameter model.

When AltaLux is invoked with parameters instead of opening the dialog:

- `param1` initializes `Strength`.
- `Detail` uses the default value `25`.
- `Natural look` uses the default value `25`.
- `param2` is accepted by the IrfanView effect signature but no longer controls
  CLAHE scale or tile count.

Example:

```text
i_view32.exe /effect=(AltaLux,45,0) input.jpg
```

This is a breaking behavior change for scripts that relied on `param2` as the v1
scale value.

## Project Structure

```text
AltaLux/
  AltaLux.cpp                 IrfanView plugin entry points and Win32 dialog
  AltaLuxCore.cpp/.h          v2 UI state, presets, geometry, and processing core
  ScopedBitmapHeader.h        RAII wrapper around DIB GlobalLock/GlobalUnlock
  UIDraw/
    UIDraw.cpp/.h             preview rendering and split comparison drawing
  Filter/
    CBaseAltaLuxFilter.*      shared CLAHE implementation
    CAltaLuxFilterFactory.*   filter factory
    CSerialAltaLuxFilter.*    serial reference implementation
    CParallelSplitLoop*       default parallel implementation
  Kernels/
    AltaLuxKernels.*          scalar/SSSE3/AVX2 dispatch layer
    AltaLuxKernelsScalar.cpp  scalar baseline kernels
    AltaLuxKernelsSSSE3.cpp   SSSE3 kernels and fallbacks
    AltaLuxKernelsAvx2.cpp    AVX2 kernels and fallbacks
AltaLuxUnitTest/
  TestStrategies.cpp          Microsoft C++ unit tests
AltaLuxBench/
  kernel and filter benchmark project
```

The root `README.md` is the current v2 guide. The older `AltaLux/README.md` file
has been removed so there is one canonical README.

## Building

Requirements:

- Visual Studio with the MSVC toolsets referenced by the project files.
  - Most current configurations target `v145`.
  - The legacy Win32 Debug configuration targets `v141_xp`.
- Windows SDK 10 or newer.
- Microsoft C++ Unit Test framework for `AltaLuxUnitTest`.

If your Visual Studio installation does not include the exact toolset, retarget
`PlatformToolset` in the project properties before building.

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

Adjust the Visual Studio path if your installation uses a different edition or
version.

## Tests

The current unit test suite contains 30 tests. Coverage includes:

- Serial vs. parallel CLAHE strategy equivalence.
- SSSE3 and AVX2 output equivalence against scalar kernels.
- SSSE3 and AVX2 2x box downscale equivalence against scalar kernels.
- Preset application and preset tolerance.
- Blend-weight normalization and balanced-layer floor.
- Conservative non-linear layer-strength mapping.
- Monotonic layer-strength clamping.
- Histogram clipping behavior when the requested clip limit is below the
  feasible per-bin minimum.
- Independent `Detail` and `Natural look` behavior.
- Safe layer region clamping.
- Fine/balanced/smooth layer ordering.
- Preview rectangle fitting and 1:1 crop mode.
- Zero-strength no-op behavior.
- RGB32 alpha preservation.
- RGB24/RGB32 consistency for identical RGB content.
- Flat-image grayscale behavior.
- Detail sensitivity on checkerboard input.
- Natural-look sensitivity on gradient input.
- The large-image parallel-layer path above the 1,000,000-pixel threshold.

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
- The large-image path parallelizes the three layer passes, but performance can
  still be limited by memory bandwidth.

**Preview and final output differ slightly**

- Preview processing uses a scaled working image for responsiveness.
- Final apply processes the selected/full-resolution region.

## Changelog

### Version 3.0.0.0

- Added the optional AI object selection add-on: MobileSAM-generated object
  masks applied through a feathered blend, shipped as the separate
  `AltaLux-AI-x64.zip` package (x64 only).
- Unified version reporting: the version resource and the version string
  reported to IrfanView now derive from a single header
  (`AltaLux/AltaLuxVersion.h`).
- Removed stale v1 documentation (Intensity/Scale parameter model, removed
  parallelization strategies) and the dead `ENABLE_LOGGING` code path.
- Standardized source license headers on a short form pointing at the root
  `LICENSE` file.

### Version 2.0.2.0

- Raised the internal v2 layer-strength ceiling to `75` while keeping the
  non-linear user-strength curve.
- Stabilized histogram clipping for very low clip limits.
- Replaced the SSE2 kernel tier with SSSE3 and added AVX2 dispatch coverage.
- Expanded benchmarks to compare scalar, SSSE3, and AVX2 kernels and filter
  paths.
- Removed unsupported event, active-wait, and error-based filter implementations;
  legacy strategy IDs now map to the default split-loop filter.
- Expanded Microsoft C++ unit coverage to 25 tests.

### Version 2.0.0.0

- Replaced the v1 `Intensity` and `Scale` UI with `Strength`, `Detail`, and
  `Natural look`.
- Removed the preview-variation grid from the primary workflow.
- Added a large before/after split preview with draggable divider.
- Added spacebar hold-to-compare and split double-click recenter.
- Added fit and 1:1 zoom preview modes.
- Added `Natural`, `Balanced`, and `Detail` presets.
- Added the shared `AltaLuxCore` module for v2 state, presets, geometry,
  weights, and multiscale processing.
- Added fixed fine, balanced, and smooth CLAHE layers.
- Added conservative non-linear internal CLAHE strength mapping.
- Corrected layer semantics: fine = 16 regions, balanced = 8 regions, smooth =
  4 regions.
- Added BT.709 luminance extraction and BGR/BGRA routing for Windows DIB data.
- Added hue-preserving scale capping for highlight handling.
- Added SIMD accumulation/output and thresholded parallel multiscale work.
- Replaced the older 64 KiB color scale table with a 1 KiB reciprocal table.
- Modernized packed YUV luma copy/writeback paths with SIMD intrinsics.
- Preserved RGB32 alpha in v2 processing.
- Updated project files to include `AltaLuxCore` and newer MSVC toolsets.
- Expanded Microsoft C++ unit coverage for core v2 behavior.

### Version 1.9.1.92

- v1 CLAHE dialog with intensity and scale controls.
- Preview variation grid.
- Dark mode support.
- Multiple parallel CLAHE strategies.
- Multiplicative color reinjection for improved color preservation.

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
