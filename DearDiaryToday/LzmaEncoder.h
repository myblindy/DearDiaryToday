#pragma once

#include "desktop_duplication.h"

class LzmaEncoder final
{
	std::unique_ptr<std::ostream> ostream;
	std::vector<BYTE> inBuffer, outBuffer;
	ErrorFunc errorFunc;
	lzma_stream stream = LZMA_STREAM_INIT;

	void CheckOutput();

public:
	LzmaEncoder(std::unique_ptr<std::ostream>, const ErrorFunc);
	~LzmaEncoder();

	void Encode(std::span<char*>);
};

