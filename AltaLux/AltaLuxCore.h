#pragma once

#include <Windows.h>

#include "Kernels/AltaLuxKernels.h"

struct UiState
{
	int strength;
	int detail;
	int naturalLook;
	bool zoomToSelection;
	bool compareHoldOriginal;
	bool draggingSplit;
	int splitX;
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
bool ProcessMultiscaleImage(const unsigned char* sourceImage, unsigned char* targetImage, int width, int height,
	int bitDepth, const UiState& state);
bool ProcessMultiscaleImageWithKernels(const unsigned char* sourceImage, unsigned char* targetImage,
	int width, int height, int bitDepth, const UiState& state,
	AltaLuxKernels::KernelImplementation implementation);
