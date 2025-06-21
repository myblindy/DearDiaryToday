#pragma once

class LzmaDecoder
{
	std::unique_ptr<std::istream> istream;
	std::vector<BYTE> inBuffer;
	const ErrorFunc errorFunc;
	lzma_stream stream = LZMA_STREAM_INIT;

public:
	LzmaDecoder(std::unique_ptr<std::istream>, const ErrorFunc);
	~LzmaDecoder();

	bool IsEof() const { return istream->eof(); }

	size_t Decode(std::span<BYTE>);

	template<typename T>
	bool Decode(T& data)
	{
		return Decode({ reinterpret_cast<BYTE*>(&data), sizeof(T) }) == sizeof(T);
	}

	bool Skip(size_t size);
};

