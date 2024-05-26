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

// The following ifdef block is the standard way of creating macros which make exporting 
// from a DLL simpler. All files within this DLL are compiled with the ALTALUX_EXPORTS
// symbol defined on the command line. this symbol should not be defined on any project
// that uses this DLL. This way any other project whose source files include this file see 
// ALTALUX_API functions as being imported from a DLL, whereas this DLL sees symbols
// defined with this macro as being exported.
#ifdef ALTALUX_EXPORTS
#define ALTALUX_API __declspec(dllexport)
#else
#define ALTALUX_API __declspec(dllimport)
#endif

/*
Params:
- hDib => 24 BPP DIB image (IrfanView will always send a 24 BPP 
							image, means if the original image is less than 24, it will be 
							expanded before the call)
							- hwnd => the parent window (either IrfanView main window, fullscreen 
							etc.) => you can use it or use GetActiveWindow() for the dialog
							- filter => the effect which should be used (if filter < 0 => show 
							effects dialog)
							- rect => if a selection rectangle is used in the image => apply 
							effect on selection only
							- param1 => value of the effect parameter 1
							- param2 => value of the effect parameter 2 (if(param1 == -1 && 
							param2 == -1) // no input => load values from INI)
							- iniFile => IrfanView INI file, if a dialog is showed and the user 
							can set/save/load parameters
							- szAppName => "IrfanView"
							- regID => dummy, just in case we arrange a special called-ID, so 
							only IrfanView can call the DLL
*/
extern "C" {
ALTALUX_API bool __cdecl StartEffects2(HANDLE hDib, HWND hwnd, int filter, RECT rect, int param1, int param2,
                                       char* iniFile, char* szAppName, int regID);
ALTALUX_API bool __cdecl AltaLux_Effects(HANDLE hDib, HWND hwnd, int filter, RECT rect, int param1, int param2,
                                         char* iniFile, char* szAppName, int regID);
ALTALUX_API int __cdecl GetPlugInInfo(char* versionString, char* fileFormats);
}

