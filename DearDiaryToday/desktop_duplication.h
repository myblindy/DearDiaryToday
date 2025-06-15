#pragma once

extern "C" {
	typedef void (*ErrorFunc)(HRESULT);
  	void __declspec(dllexport) __stdcall StartDiary(HWND, ErrorFunc);
}