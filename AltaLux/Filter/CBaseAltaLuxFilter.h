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

#pragma once

/// CAltaLux::Process return values
const int AL_OK                 = 0;
const int AL_NULL_IMAGE         = -1;   //< Image pointer is null
const int AL_WIDTH_NO_MULTIPLE  = -3;	//< x-resolution no multiple of 8
const int AL_HEIGHT_NO_MULTIPLE = -4;	//< y-resolution no multiple of 8
const int AL_OUT_OF_MEMORY      = -11;  //< no memory left to alloc internal buffers

/// Parameters for CAltaLux::SetStrength
const int AL_MIN_STRENGTH		= 0;
const int AL_DEFAULT_STRENGTH	= 25;
const int AL_MAX_STRENGTH		= 100;
const int AL_LIGHT_CONTRAST_STRENGTH  = 5;
const int AL_HEAVY_CONTRAST_STRENGTH  = 10;

typedef unsigned char PixelType;	 //< for 8 bpp grayscale images

const unsigned int MAX_HOR_REGIONS = 16;	//< max # contextual regions in x-direction
const unsigned int MAX_VERT_REGIONS = 16;	//< max # contextual regions in y-direction

const unsigned int DEFAULT_HOR_REGIONS = 8;	 //< default # contextual regions in x-direction
const unsigned int DEFAULT_VERT_REGIONS = 8; //< default # contextual regions in y-direction

const unsigned int MIN_HOR_REGIONS = 2;		//< min # contextual regions in x-direction
const unsigned int MIN_VERT_REGIONS = 2;	//< min # contextual regions in y-direction

const unsigned int NUM_GRAY_LEVELS = 256;
const unsigned int MAX_GRAY_VALUE = (NUM_GRAY_LEVELS - 1);
const unsigned int MIN_GRAY_VALUE = 0;

const float DEFAULT_CLIP_LIMIT	= 2.0f;
const float MIN_CLIP_LIMIT		= 1.0f;
const float MAX_CLIP_LIMIT		= 5.0f;

#define IMAGE_BUFFER_SIZE	(OriginalImageWidth * (OriginalImageHeight + 1))

class CBaseAltaLuxFilter {
public:
	CBaseAltaLuxFilter(int Width, int Height, int HorSlices = DEFAULT_HOR_REGIONS, int VerSlices = DEFAULT_VERT_REGIONS);
	~CBaseAltaLuxFilter();
	void SetStrength(int _Strength = AL_DEFAULT_STRENGTH);	//< set processing strength,
	void SetSlices(int HorSlices, int VerSlices);
	//< from AL_MIN_STRENGTH (which leaves the image as is)
	//< to AL_MAX_STRENGTH (which drastically enhances the contrast)
	bool IsEnabled();
	int ProcessUYVY(void *Image);	//< UYVY Image
	int ProcessVYUY(void *Image);	//< VYUY Image
	int ProcessYUYV(void *Image);	//< YUYV Image
	int ProcessYVYU(void *Image);	//< YVYU Image
	int ProcessGray(void *Image);	//< grayscale, 8-bit per pixel Image
	int ProcessRGB24(void *Image);	//< 24 bit per pixel RGB Image
	int ProcessRGB32(void *Image);	//< 32 bit per pixel RGB Image
	int ProcessBGR24(void *Image);	//< 24 bit per pixel BGR Image
	int ProcessBGR32(void *Image);	//< 32 bit per pixel BGR Image

	void ProcessRow(int uiY, unsigned int ulClipLimit, unsigned int *pulMapArray);
	void CalcGraylevelMappings(int uiY, unsigned int ulClipLimit, unsigned int *pulMapArray);

protected:
	int ImageWidth;
	int ImageHeight;
	int OriginalImageWidth;
	int OriginalImageHeight;
	unsigned char *ImageBuffer;
	int Strength;
	/// internal settings
	unsigned int NumHorRegions;
	unsigned int NumVertRegions;
	int RegionWidth;
	int RegionHeight;
	float ClipLimit;

	/// <summary>
	/// processes incoming image
	/// </summary>
	/// <returns>error code, refer to AL_XXX codes</returns>
	/// <remarks>
	/// The output image will have the same minimum and maximum value as the input
	/// image. A clip limit smaller than 1 results in standard(non - contrast limited) AHE.
	/// </remarks>
	virtual int Run() = 0;
	void ClipHistogram(unsigned int *pHistogram, unsigned int ClipLimit);
	void MakeHistogram(PixelType *pImage, unsigned int* pHistogram);
	void MapHistogram(unsigned int *pHistogram, unsigned int NumOfPixels);
	void Interpolate(PixelType *pImage, unsigned int *pulMapLU,
		unsigned int *pulMapRU, unsigned int *pulMapLB, unsigned int *pulMapRB,
		unsigned int MatrixWidth, unsigned int MatrixHeight);

	int ProcessGeneric(void *Image, int FirstFactor, int SecondFactor,
		int ThirdFactor, int PixelOffset);
};