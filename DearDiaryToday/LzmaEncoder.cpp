#include "pch.h"
#include "LzmaEncoder.h"

using namespace std;

LzmaEncoder::LzmaEncoder(std::unique_ptr<std::ostream> ostream, const ErrorFunc errorFunc)
	: inBuffer(BUFSIZ), outBuffer(BUFSIZ)
{
	this->ostream = move(ostream);
	this->errorFunc = errorFunc;

	if (lzma_easy_encoder(&stream, 0, LZMA_CHECK_CRC64) != LZMA_OK)
		errorFunc(E_FAIL);
}

LzmaEncoder::~LzmaEncoder()
{
	// finish the stream
	while (true)
	{
		auto ret = lzma_code(&stream, LZMA_FINISH);
		CheckOutput();

		if (ret == LZMA_STREAM_END)
			break;
		else if (ret != LZMA_OK)
		{
			errorFunc(S_FALSE);
			return;
		}
	}

	lzma_end(&stream);
}

void LzmaEncoder::Encode(std::span<char*> buffer)
{
	stream.next_in = nullptr;
	stream.avail_in = 0;
	stream.next_out = outBuffer.data();
	stream.avail_out = BUFSIZ;

	while (buffer.size())
	{
		// fill buffer if empty
		if (stream.avail_in == 0)
		{
			stream.next_in = inBuffer.data();
			inBuffer.resize(stream.avail_in = min(buffer.size(), inBuffer.size()));
			copy(buffer.begin(), buffer.begin() + stream.avail_in, inBuffer.begin());
			buffer = { buffer.begin() + stream.avail_in, buffer.end() };
		}

		auto ret = lzma_code(&stream, LZMA_RUN);
		CheckOutput();

		if (ret == LZMA_STREAM_END)
			break;
		else if (ret != LZMA_OK)
		{
			errorFunc(S_FALSE);
			return;
		}
	}
}

void LzmaEncoder::CheckOutput()
{
	if (stream.avail_out == 0)
	{
		size_t writeSize = outBuffer.size() - stream.avail_out;
		ostream->write((char*)outBuffer.data(), writeSize);

		stream.next_out = outBuffer.data();
		stream.avail_out = outBuffer.size();
	}
}
