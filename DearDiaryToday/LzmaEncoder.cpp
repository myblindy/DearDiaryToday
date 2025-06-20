#include "pch.h"
#include "LzmaEncoder.h"

using namespace std;

LzmaEncoder::LzmaEncoder(const std::ostream& ostream, const ErrorFunc errorFunc)
{
	this->ostream = &ostream;
	this->errorFunc = errorFunc;

	if (lzma_easy_encoder(&stream, 0, LZMA_CHECK_CRC64) != LZMA_OK)
		errorFunc(E_FAIL);
}

LzmaEncoder::~LzmaEncoder()
{
	lzma_end(&stream);
}

void LzmaEncoder::Encode(std::span<char*> buffer)
{
}
