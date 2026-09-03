#pragma once

#include "Kernels/AltaLuxKernels.h"

// Shadow chroma correction, driven by a confidence map derived from the Y
// processing. The stage compares the original luma Y with the enhanced luma
// Y' of the finished image: pixels that were originally dark, were lifted by
// several stops, and sit in a flat neighborhood get their chroma attenuated
// toward the neutral gray of their enhanced luma, which suppresses the colored
// shadow noise that shadow recovery makes conspicuous. Bright or barely lifted
// pixels keep their chroma byte-for-byte, and the luma itself never changes.
//
// chromaProtection is the 0..100 UI slider; it scales the maximum attenuation
// linearly (50 -> 25%, 100 -> 50%) so the stage stays corrective and can never
// fully desaturate a pixel. The risk map is computed from Y and Y' only, so it
// stays valid regardless of how the luma enhancement itself is implemented.

// Attenuates the chroma of targetImage (BGR, enhanced by the multiscale stage)
// using the risk map derived from targetImage and the cached original luma
// plane. Returns false on invalid arguments; a chromaProtection of 0 is a
// documented no-op that returns true.
bool ApplyChromaCorrection(unsigned char* targetImage, const unsigned char* originalLuma,
	int width, int height, int bitDepth, int chromaProtection,
	AltaLuxKernels::KernelImplementation implementation);

// Extracts BT.709 luma from a BGR24/BGR32 image into a packed width*height
// plane. Exposed so the multiscale pipeline can cache the original luma before
// the in-place write-back of the enhanced image destroys it.
void ExtractBgrLuma(const unsigned char* image, unsigned char* luma, int pixelCount,
	int bitDepth, AltaLuxKernels::KernelImplementation implementation);

// Q8 risk of the luma-gain/darkness term alone, as a pure function of the
// original and enhanced luma bytes. Test hook into the prebuilt lookup table.
int ComputeGainRiskQ8(int originalLuma, int enhancedLuma);

// Q8 risk of the local-activity term alone, as a pure function of the 3x3
// mean-absolute-deviation byte. Test hook into the prebuilt lookup table.
int ComputeActivityRiskQ8(int activity);
