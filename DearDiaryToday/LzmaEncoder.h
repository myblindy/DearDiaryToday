#pragma once

#include "desktop_duplication.h"

class LzmaEncoder final
{
	const std::ostream* ostream;
	ErrorFunc errorFunc;
	lzma_stream stream = LZMA_STREAM_INIT;

public:
	LzmaEncoder(const std::ostream&, const ErrorFunc);
	~LzmaEncoder();

	void Encode(std::span<char*>);
};

