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

#include "stdafx.h"
#include "CppUnitTest.h"

#include "..\AltaLux\Filter\CBaseAltaLuxFilter.h"
#include "..\AltaLux\Filter\CAltaLuxFilterFactory.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace AltaLuxUnitTest
{
	/// <summary>
	/// test different strategies for vectorization of the AltaLux kernel
	/// </summary>
	TEST_CLASS(TestStrategies)
	{
	public:
		const int IMAGE_WIDTH = 1024;
		const int IMAGE_HEIGHT = 768;
		const int RGBA_PIXEL_SIZE = 4;
		const int IMAGE_SIZE = (IMAGE_WIDTH * IMAGE_HEIGHT * RGBA_PIXEL_SIZE);

		unsigned char *InputImage;
		unsigned char *SerialImage;
		unsigned char *ParallelImage;

		TEST_METHOD_INITIALIZE(SetupBitmaps)
		{
			// create input image
			InputImage = new unsigned char[IMAGE_SIZE];
			// fill RGBA image with random data
			srand(0x5555);
			for (int j = 0; j < IMAGE_SIZE; j++)
				InputImage[j] = rand() % 256;

			SerialImage = new unsigned char[IMAGE_SIZE];
			memcpy(SerialImage, InputImage, IMAGE_SIZE);
			CBaseAltaLuxFilter *SerialCode = CAltaLuxFilterFactory::CreateSpecificAltaLuxFilter(ALTALUX_FILTER_SERIAL, IMAGE_WIDTH, IMAGE_HEIGHT);
			SerialCode->ProcessRGB32(SerialImage);

			ParallelImage = new unsigned char[IMAGE_SIZE];
			memcpy(ParallelImage, InputImage, IMAGE_SIZE);
		}

		TEST_METHOD_CLEANUP(FreeBitmaps)
		{
			// free images
			delete[] SerialImage;
			delete[] ParallelImage;
			delete[] InputImage;
		}

		TEST_METHOD(ParallelSplitLoopTest)
		{
			CBaseAltaLuxFilter *ParallelCode = CAltaLuxFilterFactory::CreateSpecificAltaLuxFilter(ALTALUX_FILTER_PARALLEL_SPLIT_LOOP, IMAGE_WIDTH, IMAGE_HEIGHT);
			ParallelCode->ProcessRGB32(ParallelImage);
			Assert::IsTrue(memcmp(SerialImage, ParallelImage, IMAGE_SIZE) == 0);
			Assert::IsFalse(memcmp(InputImage, ParallelImage, IMAGE_SIZE) == 0);
		}

		TEST_METHOD(ParallelEventTest)
		{
			CBaseAltaLuxFilter *ParallelCode = CAltaLuxFilterFactory::CreateSpecificAltaLuxFilter(ALTALUX_FILTER_PARALLEL_EVENT, IMAGE_WIDTH, IMAGE_HEIGHT);
			ParallelCode->ProcessRGB32(ParallelImage);
			Assert::IsTrue(memcmp(SerialImage, ParallelImage, IMAGE_SIZE) == 0);
			Assert::IsFalse(memcmp(InputImage, ParallelImage, IMAGE_SIZE) == 0);
		}

		TEST_METHOD(ParallelActiveWaitTest)
		{
			CBaseAltaLuxFilter *ParallelCode = CAltaLuxFilterFactory::CreateSpecificAltaLuxFilter(ALTALUX_FILTER_ACTIVE_WAIT, IMAGE_WIDTH, IMAGE_HEIGHT);
			ParallelCode->ProcessRGB32(ParallelImage);
			Assert::IsTrue(memcmp(SerialImage, ParallelImage, IMAGE_SIZE) == 0);
			Assert::IsFalse(memcmp(InputImage, ParallelImage, IMAGE_SIZE) == 0);
		}
	};
}