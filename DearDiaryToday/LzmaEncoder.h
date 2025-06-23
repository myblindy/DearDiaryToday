#pragma once

class LzmaEncoder final
{
	std::unique_ptr<std::ostream> ostream;
	std::vector<BYTE> outBuffer;
	const ErrorFunc errorFunc;
	lzma_stream stream = LZMA_STREAM_INIT;

	void CheckOutput(bool always);

public:
	LzmaEncoder(std::unique_ptr<std::ostream>, const ErrorFunc);
	~LzmaEncoder();

	void Encode(std::span<const BYTE>);
	void Encode(std::span<BYTE> data) { Encode({ reinterpret_cast<const BYTE*>(data.data()), data.size() }); }
	void Encode(std::span<const char> data) { Encode({ reinterpret_cast<const BYTE*>(data.data()), data.size() }); }
	void Encode(std::span<char> data) { Encode({ reinterpret_cast<const BYTE*>(data.data()), data.size() }); }

	template<typename T>
	void Encode(const T& data)
	{
		Encode({ reinterpret_cast<const BYTE*>(&data), sizeof(T) });
	}
};

