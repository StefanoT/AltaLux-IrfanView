# AltaLux - Image Enhancement Plugin for IrfanView

AltaLux is a native IrfanView effects plugin for local contrast enhancement. It
is built on CLAHE (Contrast Limited Adaptive Histogram Equalization), wrapped in
a simple user model: one overall strength control, two look-shaping controls, a
shadow chroma protection stage, a zoomable before/after preview, and optional
AI-assisted object selection.

Author: Stefano Tommesani

Website: http://www.tommesani.com

Version: 3.0.0.0

License: Microsoft Public License (MS-PL), see the root `LICENSE` file

## At A Glance

- One `Strength` control drives the internal CLAHE layers; `Detail` and
  `Natural look` shape the layer blend; `Chroma protection` suppresses colored
  shadow noise.
- Large before/after split preview with a draggable divider, continuous
  wheel zoom, panning, and `Fit` / `1:1` view resets. Preview processing runs
  in the background so the dialog stays responsive.
- `Natural`, `Balanced`, and `Detail` presets.
- Fixed three-layer multiscale pipeline: fine (16 regions), balanced (8), and
  smooth (4) CLAHE passes, blended into the final image.
- BT.709 luminance, BGR/BGRA-aware processing, and multiplicative color
  reinjection with a scale cap to reduce hue and highlight drift.
- `Chroma protection`: a confidence map derived from the original and enhanced
  luminance attenuates chroma in aggressively lifted, flat shadows.
- Optional AI object selection add-on (x64 only): MobileSAM-generated object
  masks through ONNX Runtime, running locally.
- Preserves RGB32 alpha and keeps RGB24/RGB32 behavior aligned for the same RGB
  content.
- Runtime kernel dispatch across scalar, SSSE3, and AVX2 implementations, plus
  thresholded parallel layer processing for large images.
- 40 Microsoft C++ unit tests covering the processing core, SIMD kernel
  equivalence, the chroma stage, and the selection helpers.

## Installation

1. Build or download `AltaLux.dll`.
2. Copy it to the IrfanView `Plugins` folder.
   - 64-bit default: `C:\Program Files\IrfanView\Plugins\`
   - 32-bit default: `C:\Program Files (x86)\IrfanView\Plugins\`
3. Restart IrfanView.
4. Open AltaLux from IrfanView's image effects menu.

Use a DLL build that matches the IrfanView architecture. Release packages from
the [releases page](https://github.com/StefanoT/AltaLux-IrfanView/releases) are
versioned zips that each unzip to `AltaLux.dll`:

- `AltaLux-x64-<version>.zip` for 64-bit IrfanView
- `AltaLux-Win32-<version>.zip` for 32-bit IrfanView
- `AltaLux-AI-x64-<version>.zip` for the optional AI add-on (x64 only)

### Optional AI object selection

The x64 plug-in can apply AltaLux through MobileSAM-generated object masks. The
base `AltaLux.dll` remains fully functional without the AI files. To enable
object selection, extract every file from the `AltaLux-AI-x64-<version>.zip`
release package directly into the same IrfanView `Plugins` folder as
`AltaLux.dll`; do not create a subfolder. The package contains
`AltaLuxSegmentation.dll`, the ONNX Runtime DLLs, the encoder/decoder `.onnx`
models, and a manifest with checksums.

AltaLux loads the segmentation module by absolute path from its own directory;
it does not search the process working directory or `PATH`. If any component is
absent, the normal whole-image filter remains available.

The add-on runs locally through ONNX Runtime's CPU provider. No image data is
uploaded and AltaLux does not download models at runtime.

## Interactive Usage

1. Open an image in IrfanView.
2. Launch AltaLux from the effects menu.
3. Adjust `Strength`, `Detail`, `Natural look`, and `Chroma protection`.
4. Drag the vertical divider to compare original and processed output.
5. Hold `Space` (or `Ctrl`) to temporarily show the original.
6. Double-click the divider to recenter it.
7. Use `Natural`, `Balanced`, or `Detail` as starting presets.
8. Scroll the mouse wheel over the preview to zoom toward the cursor; drag the
   preview with the left button to pan while zoomed. The middle button also
   pans in every mode. `Fit` and `1:1` reset the view.
9. Press `OK` to apply or `Cancel` to discard.

When the optional AI package is installed, use `Select objects`, choose `Add`
or `Remove`, and click objects in the preview. A click without dragging picks
the object under the cursor; dragging pans instead (the middle button also
pans). `Undo`, `Clear`, `Select all`, the `Show mask overlay` checkbox, and
`Edge softness` refine how the filtered result is blended with the original.

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

### Chroma Protection

Attenuates chroma toward neutral in shadows that AltaLux lifted aggressively, so
opened-up shadows stay free of the purple/green speckles that shadow recovery
makes conspicuous.

- `0` disables the stage entirely.
- The default `50` maps to a conservative maximum attenuation of 25%.
- `100` maps to 50%; the stage can never fully desaturate a pixel.
- Only originally dark, strongly lifted, flat pixels are touched: bright or
  barely lifted areas keep their color byte-for-byte, and textured shadows keep
  most of theirs.
- The stage is corrective, not an enhancement: it never changes luminance.

## Presets

| Preset | Strength | Detail | Natural look |
| --- | ---: | ---: | ---: |
| Natural | 35 | 10 | 55 |
| Balanced | 45 | 25 | 25 |
| Detail | 55 | 60 | 10 |

The default startup state is `Balanced` unless saved settings override it.

## Processing Model

AltaLux processes RGB24/RGB32 image data through three fixed CLAHE passes,
blends those layer outputs, and writes the weighted multiscale result directly.

The layer constants are region counts, not pixel tile sizes. More regions means
smaller CLAHE tiles and more local/detail-sensitive behavior. For small images
the region counts are clamped so no region ends up wider or taller than the
image itself.

| Layer | Constant | Region grid | Visual role |
| --- | --- | --- | --- |
| Fine | `Constants::FineRegions` | 16 x 16 | Small tiles, local detail and texture |
| Balanced | `Constants::BalancedRegions` | 8 x 8 | Main enhancement backbone |
| Smooth | `Constants::SmoothRegions` | 4 x 4 | Larger tiles, smoother tonal rendering |

The processing order in `ProcessMultiscaleImage()` is:

1. Copy the source image to the output buffer (skipped when invoked in place),
   which also carries the fourth byte of RGB32 pixels.
2. Compute the layer blend weights from `Detail` and `Natural look`, and the
   internal layer strength from `Strength`.
3. Render the fine, balanced, and smooth CLAHE layers into scratch buffers:
   sequentially through one buffer below the parallel-layer threshold, or
   through three buffers processed concurrently at or above it.
4. Accumulate the weighted BGR channels of every layer into an integer
   accumulator (planar for RGB32, interleaved triplets for RGB24).
5. Write the weighted multiscale enhancement to the output image.
6. When `Chroma protection` is above zero, attenuate chroma in aggressively
   lifted flat shadows (see below). The original luma needed by this stage is
   extracted before step 5, because the write-back is in place.

No kernel ever writes the fourth byte of a 32-bit pixel, so RGB32 alpha and
padding survive the whole pipeline untouched.

### Layer Strength Curve

`Strength` controls the CLAHE strength used inside each layer through a
conservative non-linear curve (`layer strength = 75 * (user strength / 100)^1.4`),
so high slider values do not drive the internal CLAHE pass to `100`.

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
to `0.35` toward the smooth layer. The balanced layer is floored at `0.20` (any
deficit is recovered by scaling the fine and smooth weights down), and weights
are normalized before processing.

### Shadow Chroma Correction

After the multiscale blend, `Chroma protection` runs a corrective pass that
suppresses colored shadow noise. It derives a per-pixel risk map from the
luminance processing itself — comparing the original luma `Y` with the enhanced
luma `Y'` of the finished image — and attenuates chroma only where that map says
the original color is unreliable. The map is deliberately decoupled from the
CLAHE internals: it sees only `Y` and `Y'`, so it stays valid if the enhancement
changes.

The risk per pixel is the product of three terms:

| Term | Meaning | Range |
| --- | --- | --- |
| Gain risk | `log2((Y'+1)/(Y+1))` in stops, smoothstepped from +0.5 to +2 stops | strong lift means questionable chroma |
| Darkness risk | `1 - smoothstep(8, 64, Y)` | near-black colors are unreliable, midtones are not |
| Activity risk | `1 - smoothstep(1, 6, T)`, `T` = 3x3 mean absolute deviation of the original luma | flat areas show chroma noise, edges read as structure |

The gain and darkness terms are folded into a prebuilt 65536-entry lookup table
indexed by `(Y << 8) | Y'`; the activity term comes from a 256-entry table. The
combined risk is `R = R_gain * R_darkness * (0.3 + 0.7 * R_activity)`, so
textured areas keep 30% of the risk, then blurred with a separable `[1 2 1]`
kernel to avoid visible per-pixel variation. The final strength
`S = S_max * R` with `S_max = Chroma protection / 200` moves every color
channel toward the neutral gray of its enhanced luma:

```text
c' = Y' + (c - Y') * (1 - S)
```

This is luma-neutral (it preserves `Y'` exactly) and provably stays inside byte
range. Zero-risk pixels — bright, barely lifted, or strongly textured — are
reproduced byte-for-byte. The stage costs roughly 35 ms per 4K image: the
activity and blur passes are row-vectorized (2.4-3.8x under SSSE3/AVX2),
leaving the scalar 64K-table risk pass as the dominant cost, and the
attenuation kernel itself runs 3-4x faster under AVX2.

## Color And Format Handling

Windows DIB image data is handled as BGR/BGRA in the plugin path. The pipeline
routes layer processing through the BGR filter variants so the luminance
coefficients match the actual channel order.

The CLAHE filter:

1. Extracts luminance using BT.709 coefficients:
   `Y = 0.2126 R + 0.7152 G + 0.0722 B`.
2. Applies CLAHE to the luminance buffer.
3. Reinjects luminance by multiplicative color scaling against the cached
   original luma.
4. Uses a Q16 reciprocal lookup table instead of per-pixel division.
5. Caps the scale factor (via a shared per-channel lookup table) so saturated
   highlights do not drift in hue.

This is not the old additive `R/G/B + deltaY` model. It is designed to preserve
channel ratios better while still enhancing local contrast.

## Performance And Memory

The multiscale pipeline runs three CLAHE passes per image, so the implementation
compensates with:

- Runtime selection of the best supported kernel path: AVX2, then SSSE3, then
  scalar. SIMD kernels are bit-exact against the scalar baseline and delegate
  their tails to the next lower tier.
- SIMD paths for RGB/BGR luma extraction and injection, packed YUV luma
  extraction and injection, 2x box downscaling, multiscale accumulation and
  output, and the chroma attenuation/activity/blur kernels.
- Sequential layer processing for smaller images to avoid task and allocation
  overhead.
- Blocked parallel accumulation/output for images at or above 200,000 pixels.
- Parallel processing of the fine, balanced, and smooth layer passes for images
  at or above 1,000,000 pixels.
- A 1 KiB reciprocal lookup table for color scaling and a shared 1 KiB
  scale-cap table, replacing per-pixel division and the older 64 KiB
  two-dimensional scale table.
- Scalar-only implementations for the histogram and interpolation kernels and
  the 64K-table risk pass, where SIMD designs were measured slower.

For RGB24/RGB32 multiscale processing, the main allocations are:

- An accumulator with three `uint32` values per pixel.
- One scratch layer buffer for the sequential layer path.
- Three full layer buffers for the large-image parallel layer path.
- CLAHE internal luminance buffers and histogram mapping arrays.
- When `Chroma protection` is active: a cached original-luma plane plus
  enhanced-luma, activity, and risk byte planes.

The large-image path intentionally trades memory for wall-clock latency.

## Settings

Settings are stored under the `[AltaLux]` section:

```ini
[AltaLux]
Strength=45
Detail=25
NaturalLook=25
ChromaProtection=50
Zoom=0
WindowRect=-480,60,1280,860
```

The old `Intensity` key is read as a fallback only when `Strength` is absent.
The old `Scale` setting is not surfaced in the UI or parameter model. A missing
`ChromaProtection` key falls back to the default `50`; set it to `0` to keep the
pre-3.0 output byte-identical. `Zoom=1` reopens the preview in the 1:1 view.
`WindowRect` stores the dialog's last `left,top,width,height` in screen pixels
so its size and position survive across invocations, clamped to the monitor's
work area; a missing or invalid value keeps the default centered placement.

## Direct Invocation And Batch Parameters

The non-dialog path is intentionally simplified for v2+. It no longer preserves
the old v1 `(Intensity, Scale)` parameter model.

When AltaLux is invoked with parameters instead of opening the dialog:

- `param1` initializes `Strength` (clamped to 0-100).
- `Detail` uses the default value `25`.
- `Natural look` uses the default value `25`.
- `Chroma protection` uses the default value `50`.
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
  AltaLux.h                   exported plugin interface and INI documentation
  AltaLuxCore.cpp/.h          UI state, presets, geometry, and multiscale processing core
  AltaLuxVersion.h            single source of truth for the plugin version
  ChromaCorrection.cpp/.h     shadow chroma correction stage (risk map + attenuation)
  ScopedBitmapHeader.h        RAII wrapper around DIB GlobalLock/GlobalUnlock
  UIDraw/
    UIDraw.cpp/.h             preview rendering and split comparison drawing
  Filter/
    CBaseAltaLuxFilter.*      shared CLAHE implementation
    CAltaLuxFilterFactory.*   filter factory
    CSerialAltaLuxFilter.*    serial reference implementation
    CParallelSplitLoop*       default parallel implementation
  Kernels/
    Kernels.cpp/.h            scalar/SSSE3/AVX2 dispatch layer and kernel contract
    KernelsScalar.cpp         scalar baseline kernels
    KernelsSSSE3.cpp          SSSE3 kernels
    KernelsAVX2.cpp           AVX2 kernels
  Segmentation/
    SegmentationModule.cpp/.h optional AI add-on loader (versioned ABI)
    SelectionCore.cpp/.h      mask history, feathering, and compositing
AltaLuxSegmentation/          optional MobileSAM ONNX add-on DLL project
AltaLuxUnitTest/
  TestStrategies.cpp          Microsoft C++ unit tests (40)
AltaLuxBench/                 kernel and filter benchmark project
tools/                        MobileSAM ONNX export and model bundle verification
```

The root `README.md` is the current guide. `AltaLuxSegmentation/README.md`
documents the AI add-on files and model provenance.

## Building

Requirements:

- Visual Studio with the `v145` platform toolset, which all configurations of
  all projects target.
- Windows SDK 10 or newer.
- Microsoft C++ Unit Test framework for `AltaLuxUnitTest`.
- The `microsoft.ml.onnxruntime` NuGet package (referenced by its install path)
  is only needed to build `AltaLuxSegmentation`; the main plugin and the unit
  tests build without it.

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

The unit test suite contains 40 tests. Coverage includes:

- Serial vs. parallel CLAHE strategy equivalence, end to end.
- SSSE3 and AVX2 end-to-end filter output equivalence against scalar kernels.
- SSSE3 and AVX2 2x box downscale equivalence against scalar kernels.
- SSSE3 and AVX2 chroma attenuation equivalence against scalar kernels,
  including sub-ranges, both pixel strides, and the full pipeline with the
  stage active.
- Preset application and preset tolerance.
- Blend-weight normalization and balanced-layer floor.
- Conservative non-linear layer-strength mapping and its monotonic clamping.
- Histogram clipping behavior when the requested clip limit is below the
  feasible per-bin minimum.
- Independent `Detail` and `Natural look` behavior, including sensitivity on
  checkerboard and gradient inputs.
- Safe layer region clamping and fine/balanced/smooth region semantics.
- Preview rectangle fitting and 1:1 crop mode.
- Zero-strength no-op behavior, very-low-strength handling, and flat images
  with a tight clip limit.
- RGB32 alpha preservation, including with chroma correction active and
  in-place.
- RGB24/RGB32 consistency for identical RGB content, including with chroma
  correction active.
- Flat-image grayscale behavior.
- The large-image parallel-layer path above the 1,000,000-pixel threshold.
- Chroma correction skipped at zero strength.
- Chroma attenuation in lifted flat shadows, with bright areas byte-identical
  and textured shadows retaining more chroma than flat ones.
- Gain and activity risk table monotonicity.
- Local activity and risk blur kernel behavior.
- AI selection helpers: mask add/remove/undo, undoable fill, compositing with
  alpha and endpoint preservation, one-pixel feather expansion, and
  preview-to-image coordinate mapping.

## Troubleshooting

**Plugin does not appear in IrfanView**

- Confirm the DLL is in the correct IrfanView `Plugins` folder.
- Confirm DLL architecture matches IrfanView architecture.
- Restart IrfanView after copying the DLL.

**The result looks noisy**

- Lower `Strength`.
- Lower `Detail`.
- Raise `Chroma protection` if the noise is colored speckles in shadows.
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

- Preview processing uses a box-downscaled working image for responsiveness.
- Final apply processes the selected/full-resolution region.

## Changelog

### Version 3.0.0.0

- Added `Chroma protection`: a corrective post-blend stage that derives a
  per-pixel risk map from the original and enhanced luminance (stops-based gain,
  original darkness, 3x3 local activity) and attenuates chroma toward the
  enhanced luma in aggressively lifted flat shadows, suppressing colored shadow
  noise without touching luminance, bright areas, or textured detail. Exposed as
  a 0-100 slider (default 50 = 25% max attenuation), persisted as
  `ChromaProtection` in the INI file; `0` keeps prior output byte-identical.
- Added the optional AI object selection add-on: MobileSAM-generated object
  masks applied through a feathered blend, shipped as the separate, versioned
  `AltaLux-AI-x64-<version>.zip` release package (x64 only).
- Replaced the fit/1:1 preview toggle with continuous zoom: the mouse wheel
  zooms toward the cursor and dragging pans the zoomed image, with `Fit` and
  `1:1` as view resets. Preview processing moved to a background worker with
  stale-result discarding, and the preview source uses box-filter resampling.
- Added `Ctrl` as a hold-to-compare modifier alongside `Space`.
- The dialog remembers its size and position (`WindowRect`), clamped to the
  monitor work area, and preview labels and the comparison handle scale with
  display DPI.
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

Microsoft Public License (MS-PL). See the `LICENSE` file at the root of this
repository.
