#pragma once
#include "stdafx.h"

class ScopedBitmapHeader final {
public:
	ScopedBitmapHeader(HANDLE hDib) : hDib(hDib) {
		pbBmHdr = (LPBITMAPINFOHEADER)GlobalLock(hDib);
	}

	~ScopedBitmapHeader() {
		GlobalUnlock(hDib);
	}

	LPBITMAPINFOHEADER operator->() const {
		return pbBmHdr;
	}

	BITMAPINFOHEADER operator*() const {
		return *pbBmHdr;
	}

private:
	LPBITMAPINFOHEADER pbBmHdr = nullptr;
	HANDLE hDib;
};
