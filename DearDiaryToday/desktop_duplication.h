#pragma once

#include "LzmaEncoder.h"

extern "C" {
	bool __declspec(dllexport) __stdcall InitializeDiary(ErrorFunc);

	void __declspec(dllexport) __stdcall StartDiary(HWND);

	typedef void (*ExportDiaryVideoCompletion)(float, void*);
	void __declspec(dllexport) __stdcall  ExportDiaryVideo(LPWSTR, ExportDiaryVideoCompletion, void*);

	typedef void (*StopDiaryCompletion)(void*);
	void __declspec(dllexport) __stdcall StopDiary(StopDiaryCompletion, void*);
}

constexpr int MAX_DIARY_FILES = 2;
constexpr int MAX_FRAME_RATE = 30;
constexpr int MAX_FRAMES_PER_DIARY_FILE = 10 * MAX_FRAME_RATE;

constexpr int DIARY_VIDEO_BITRATE = 5000 * 1024;

struct DesktopDuplication : winrt::implements<DesktopDuplication, ::IInspectable>
{
	DesktopDuplication(ErrorFunc);
	winrt::Windows::Foundation::IAsyncAction Start(HWND);
	void ExportVideo(std::wstring, ExportDiaryVideoCompletion, void*);
	void StopDiaryAndWait();

	static std::filesystem::path GetDiaryFilePath(int index, bool create);

private:
	volatile bool stopping{};
	const ErrorFunc errorFunc;
	int outputFileIndex = -1;
	std::unique_ptr<LzmaEncoder> lzmaEncoder;

	CRITICAL_SECTION fileAccessCriticalSection;

	struct FrameData
	{
		int width, height, stride;
		DXGI_FORMAT format;
		hr_time_point now;
		std::vector<BYTE> data;
	};
	moodycamel::BlockingReaderWriterCircularBuffer<FrameData> frames{ 10 };
	winrt::handle newFrameReadyEvent{ CreateEvent(nullptr, FALSE, FALSE, nullptr) };
	std::thread frameProcessingThread;

	winrt::com_ptr<IDXGIFactory> dxgiFactory;
	winrt::com_ptr<ID3D11Device> d3d11Device;
	winrt::com_ptr<ID3D11DeviceContext> immediateContext;
	winrt::com_ptr<IDXGIDevice> dxgiDevice;

	winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice d3dRtDevice;
	winrt::Windows::Graphics::Capture::GraphicsCaptureItem captureItem{ nullptr };
	winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool framePool{ nullptr };
	winrt::Windows::Graphics::Capture::GraphicsCaptureSession captureSession{ nullptr };

	winrt::Windows::Graphics::SizeInt32 lastFrameSize{};

	void OnFrameArrived(
		winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& sender,
		winrt::Windows::Foundation::IInspectable const&);
	winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::FrameArrived_revoker frameArrivedRevoker;

	winrt::Windows::Graphics::DirectX::DirectXPixelFormat DxgiPixelFormatToRtPixelFormat(DXGI_FORMAT) const;
	int GetFormatBytesPerPixel(DXGI_FORMAT) const;

	void OpenNextOutputFile();
	void WriteRecordedImageToCircularFrameBuffer(const D3D11_MAPPED_SUBRESOURCE&, DXGI_FORMAT, winrt::Windows::Graphics::SizeInt32);

	winrt::Windows::Graphics::SizeInt32 GetMaximumSavedFrameSize(const std::vector<std::filesystem::path>& partPaths, int& frameCount) const;

	HRESULT WriteTransformOutputSamplesToSink(winrt::com_ptr<IMFTransform>& frameTransform,
		winrt::com_ptr<IMFSinkWriter>& sinkWriter, MFT_OUTPUT_DATA_BUFFER& mftOutputData) const;
};