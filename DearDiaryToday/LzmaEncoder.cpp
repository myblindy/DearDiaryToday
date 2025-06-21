#include "pch.h"
#include "LzmaEncoder.h"

using namespace std;

LzmaEncoder::LzmaEncoder(std::unique_ptr<std::ostream> ostream, const ErrorFunc errorFunc)
	: outBuffer(BUFSIZ), errorFunc(errorFunc), ostream(move(ostream))
{
	outBuffer.resize(BUFSIZ);

	if (lzma_easy_encoder(&stream, 0, LZMA_CHECK_CRC64) != LZMA_OK)
		errorFunc(E_FAIL);

	stream.next_out = outBuffer.data();
	stream.avail_out = outBuffer.size();
}

LzmaEncoder::~LzmaEncoder()
{
	// finish the stream
	while (true)
	{
		auto ret = lzma_code(&stream, LZMA_FINISH);
		auto full = stream.avail_out == 0;
		CheckOutput(true);

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

void LzmaEncoder::Encode(std::span<const BYTE> buffer)
{
	stream.next_in = buffer.data();
	stream.avail_in = buffer.size();

	while (true)
	{
		auto ret = lzma_code(&stream, LZMA_RUN);
		auto full = stream.avail_out == 0;
		CheckOutput(false);

		if (ret == LZMA_STREAM_END)
			break;
		else if (ret != LZMA_OK)
		{
			errorFunc(S_FALSE);
			return;
		}
		else if (stream.avail_in == 0 && !full)
			break;
	}
}

void LzmaEncoder::CheckOutput(bool always)
{
	if (stream.avail_out == 0 || always)
	{
		size_t writeSize = outBuffer.size() - stream.avail_out;
		if (writeSize)
			ostream->write((char*)outBuffer.data(), writeSize);

		stream.next_out = outBuffer.data();
		stream.avail_out = outBuffer.size();
	}
}
