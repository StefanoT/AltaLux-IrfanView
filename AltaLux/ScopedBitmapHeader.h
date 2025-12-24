#pragma once
#include "stdafx.h"

//=============================================================================
// ScopedBitmapHeader Class
//=============================================================================
/// @file ScopedBitmapHeader.h
/// @brief RAII wrapper for Windows DIB (Device Independent Bitmap) headers
///
/// @class ScopedBitmapHeader
/// @brief Scope-based management of Windows HGLOBAL bitmap memory
///
/// @details This class implements the RAII (Resource Acquisition Is Initialization)
///          pattern for safely accessing Windows DIB data. It automatically locks
///          the global memory handle on construction and unlocks it on destruction,
///          ensuring that memory is always properly released even if exceptions occur.
///
/// @par Windows DIB Memory Management:
/// Windows DIB data is stored in global memory (HGLOBAL):
/// 1. GlobalLock() - Locks memory and returns pointer
/// 2. Access data through pointer
/// 3. GlobalUnlock() - Unlocks memory (must always be called)
///
/// Without RAII, forgetting to unlock can cause memory leaks and system instability.
///
/// @par Design Pattern: RAII (Resource Acquisition Is Initialization)
/// The resource (locked memory) is acquired in the constructor and released
/// in the destructor, guaranteeing cleanup regardless of how scope is exited
/// (normal return, exception, early return, etc.).
///
/// @par Thread Safety:
/// This class is NOT thread-safe. Each HGLOBAL should be accessed by only
/// one ScopedBitmapHeader instance at a time. GlobalLock() does not provide
/// thread synchronization.
///
/// @par Usage Example:
/// @code
/// // Old way (manual management, error-prone):
/// LPBITMAPINFOHEADER pHeader = (LPBITMAPINFOHEADER)GlobalLock(hDib);
/// if (pHeader) {
///     // ... process image ...
///     GlobalUnlock(hDib);  // Easy to forget!
/// }
///
/// // New way (automatic management, exception-safe):
/// {
///     ScopedBitmapHeader header(hDib);
///     int width = header->biWidth;
///     int height = header->biHeight;
///     BYTE* pixels = header.GetImageBits();
///     // ... process image ...
/// }  // Automatically unlocked here
/// @endcode
///
/// @par Exception Safety:
/// Strong exception guarantee - if an exception is thrown during image
/// processing, the destructor still runs and memory is properly unlocked.
///
/// @warning Do not store the pointers returned by operator-> or GetImageBits()
///          beyond the lifetime of the ScopedBitmapHeader object. The memory
///          becomes invalid when the object is destroyed.
///
/// @see BITMAPINFOHEADER
/// @see GlobalLock
/// @see GlobalUnlock
class ScopedBitmapHeader final {
public:
	//-------------------------------------------------------------------------
	// Constructor & Destructor
	//-------------------------------------------------------------------------
	
	/// @brief Construct and lock global memory handle
	/// @param hDib Windows HGLOBAL handle to bitmap data
	/// @note Calls GlobalLock() immediately
	/// @warning If GlobalLock() fails, pbBmHdr will be nullptr
	/// @par Typical Usage:
	/// @code
	/// ScopedBitmapHeader header(hDib);
	/// if (header->biSize > 0) {  // Verify lock succeeded
	///     // Access bitmap data
	/// }
	/// @endcode
	ScopedBitmapHeader(HANDLE hDib) : hDib(hDib) {
		pbBmHdr = (LPBITMAPINFOHEADER)GlobalLock(hDib);
	}

	/// @brief Destructor - unlocks global memory
	/// @note Always called when object goes out of scope
	/// @note Automatically handles cleanup even if exceptions thrown
	~ScopedBitmapHeader() {
		GlobalUnlock(hDib);
	}

	//-------------------------------------------------------------------------
	// Deleted Operations (Prevent Copying)
	//-------------------------------------------------------------------------
	
	/// @brief Copy constructor deleted (non-copyable)
	/// @note Copying would cause double-unlock, leading to undefined behavior
	ScopedBitmapHeader(const ScopedBitmapHeader&) = delete;
	
	/// @brief Copy assignment deleted (non-copyable)
	ScopedBitmapHeader& operator=(const ScopedBitmapHeader&) = delete;

	//-------------------------------------------------------------------------
	// Access Operators
	//-------------------------------------------------------------------------
	
	/// @brief Access bitmap header members via pointer syntax
	/// @return Pointer to locked BITMAPINFOHEADER
	/// @note Allows natural member access: header->biWidth, header->biHeight, etc.
	/// @warning Pointer becomes invalid after object destruction
	/// @par Example:
	/// @code
	/// ScopedBitmapHeader header(hDib);
	/// int width = header->biWidth;      // Access member
	/// int height = header->biHeight;    // Access member
	/// int bpp = header->biBitCount;     // Access member
	/// @endcode
	LPBITMAPINFOHEADER operator->() const {
		return pbBmHdr;
	}

	/// @brief Get copy of bitmap header structure
	/// @return Copy of BITMAPINFOHEADER structure
	/// @note Returns value copy, not reference
	/// @note Useful when you need to store header info beyond scope
	/// @par Example:
	/// @code
	/// BITMAPINFOHEADER headerCopy;
	/// {
	///     ScopedBitmapHeader header(hDib);
	///     headerCopy = *header;  // Safe - makes a copy
	/// }
	/// // headerCopy still valid here
	/// @endcode
	BITMAPINFOHEADER operator*() const {
		return *pbBmHdr;
	}

	//-------------------------------------------------------------------------
	// Pixel Data Access
	//-------------------------------------------------------------------------
	
	/// @brief Get pointer to raw pixel data
	/// @return Pointer to first pixel (immediately after header)
	/// @note Pixel data begins at: header + sizeof(BITMAPINFOHEADER)
	/// @warning Pointer becomes invalid after object destruction
	/// @par DIB Structure:
	/// @code
	/// [BITMAPINFOHEADER] <- header data
	/// [Color Palette]     <- optional, only for <= 8bpp
	/// [Pixel Data]        <- returned by this function
	/// @endcode
	/// @par Example:
	/// @code
	/// ScopedBitmapHeader header(hDib);
	/// BYTE* pixels = header.GetImageBits();
	/// 
	/// // Calculate stride (row width in bytes, including padding)
	/// int stride = ((header->biWidth * header->biBitCount + 31) / 32) * 4;
	/// 
	/// // Access pixel at (x, y)
	/// int bytesPerPixel = header->biBitCount / 8;
	/// BYTE* pixel = pixels + (y * stride) + (x * bytesPerPixel);
	/// @endcode
	/// @note DIB rows are stored bottom-to-top (y=0 is bottom of image)
	/// @note Each row is padded to multiple of 4 bytes (DWORD alignment)
	BYTE *GetImageBits() const {
		return (BYTE *)pbBmHdr + (WORD)pbBmHdr->biSize;
	}

private:
	/// @brief Pointer to locked bitmap header
	/// @note nullptr if GlobalLock() failed
	LPBITMAPINFOHEADER pbBmHdr = nullptr;
	
	/// @brief Global memory handle (stored for unlock)
	HANDLE hDib;
};
