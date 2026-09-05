#pragma once

#include <Windows.h>

#include "Kernels/Kernels.h"

struct UiState
{
	int strength;
	int detail;
	int naturalLook;
	int chromaProtection;
	bool zoomToSelection;
	bool compareHoldOriginal;
	bool draggingSplit;
	int splitX;
	// Continuous zoom relative to the fit scale (1.0 = fit, larger = zoomed in)
	double zoomFactor;
	// Pan of the image rect center from the preview center, in client pixels
	int panX;
	int panY;
	bool draggingPan;
	int panLastX;
	int panLastY;
	// Left-button press point while a drag may still turn out to be a pick
	int panDragOriginX;
	int panDragOriginY;
	// True while a selective-mode press has not yet moved enough to count as
	// a pan, so the release triggers an object pick at these source coords
	bool pendingPick;
	float pendingPickX;
	float pendingPickY;
};

struct BlendWeights
{
	float fine;
	float balanced;
	float smooth;
};

struct Preset
{
	int buttonId;
	const wchar_t* name;
	int strength;
	int detail;
	int naturalLook;
};

struct Constants
{
	static constexpr int RGB24PixelSize = 3;
	static constexpr int RGB32PixelSize = 4;

	static constexpr int FineRegions = 16;
	static constexpr int BalancedRegions = 8;
	static constexpr int SmoothRegions = 4;

	static constexpr int DefaultStrength = 45;
	static constexpr int DefaultDetail = 25;
	static constexpr int DefaultNatural = 25;
	static constexpr int DefaultChromaProtection = 50;

	// Ceiling of the shadow chroma attenuation at slider 100, in Q8 (128 = 50%
	// chroma reduction): the stage is corrective and never fully desaturates.
	static constexpr int MaxChromaAttenuationQ8 = 128;

	static constexpr int MinLayerStrength = 0;
	static constexpr int MaxLayerStrength = 75;

	static constexpr float BaseFine = 0.15f;
	static constexpr float BaseBalanced = 0.60f;
	static constexpr float BaseSmooth = 0.25f;

	static constexpr float MaxDetailShift = 0.35f;
	static constexpr float MaxNaturalShift = 0.35f;
	static constexpr float MinBalancedWeight = 0.20f;
};

int ClampInt(int value, int minimum, int maximum);
int ComputeLayerStrength(int userStrength);
BlendWeights ComputeBlendWeights(const UiState& state);
int GetSafeLayerRegions(int width, int height, int preferredRegions, int minRegions, int maxRegions);
void ApplyPreset(UiState& state, const Preset& preset);
bool IsPresetActive(const UiState& state, const Preset& preset, int tolerance);
RECT FitImageRect(const RECT& container, int imageWidth, int imageHeight);
RECT GetPreviewImageRect(int imageWidth, int imageHeight, const RECT& rectPosition, bool noRescaling);
RECT GetZoomedImageRect(int imageWidth, int imageHeight, const RECT& container, double zoomFactor, int panX, int panY);
void ClampPanOffsets(int imageWidth, int imageHeight, const RECT& container, double zoomFactor, int& panX, int& panY);
double GetActualPixelScale(int imageWidth, int imageHeight, const RECT& container, double zoomFactor);
bool ProcessMultiscaleImage(const unsigned char* sourceImage, unsigned char* targetImage, int width, int height,
	int bitDepth, const UiState& state);
bool ProcessMultiscaleImageWithKernels(const unsigned char* sourceImage, unsigned char* targetImage,
	int width, int height, int bitDepth, const UiState& state,
	AltaLuxKernels::KernelImplementation implementation);
