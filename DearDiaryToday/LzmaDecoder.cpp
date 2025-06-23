#include "pch.h"
#include "LzmaDecoder.h"

using namespace std;

LzmaDecoder::LzmaDecoder(std::unique_ptr<std::istream> istream, const ErrorFunc errorFunc)
	: inBuffer(BUFSIZ), errorFunc(errorFunc), istream(move(istream))
{
	inBuffer.resize(BUFSIZ);

	if (lzma_stream_decoder(&stream, UINT64_MAX, 0) != LZMA_OK)
		errorFunc(E_FAIL);

	stream.next_in = nullptr;
	stream.avail_in = 0;
}

LzmaDecoder::~LzmaDecoder()
{
	lzma_end(&stream);
}

size_t LzmaDecoder::Decode(std::span<BYTE> outSpan)
{
	stream.next_out = outSpan.data();
	stream.avail_out = outSpan.size();

	while (stream.avail_out > 0 && !istream->eof())
	{
		if (stream.avail_in == 0)
		{
			stream.next_in = inBuffer.data();
			istream->read(reinterpret_cast<char*>(inBuffer.data()), inBuffer.size());
			stream.avail_in = istream->gcount();
		}

		lzma_ret ret = lzma_code(&stream, LZMA_RUN);

		if (ret == LZMA_STREAM_END)
			break;
		if (ret != LZMA_OK)
			break;// errorFunc(E_FAIL);		// if error, write what we can and stop
	}

	return outSpan.size() - stream.avail_out;
}

bool LzmaDecoder::Skip(size_t size)
{
	auto mem = _malloca(size);
	if (!mem)
		return false;

	size_t bytesRead = Decode(std::span<BYTE>(reinterpret_cast<BYTE*>(mem), size));
	_freea(mem);

	return bytesRead == size;
}
