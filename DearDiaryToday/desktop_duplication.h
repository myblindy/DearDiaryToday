#pragma once

extern "C" {
	typedef void (*ErrorFunc)(HRESULT);
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
	winrt::Windows::Foundation::IAsyncAction ExportVideo(std::wstring, ExportDiaryVideoCompletion, void*);
	void StopDiaryAndWait();

	static std::filesystem::path GetDiaryFilePath(int index, bool create);

private:
	volatile bool stopping{};
	ErrorFunc errorFunc;
	int outputFileIndex = -1;
	std::ofstream outputFile;
	int outputFileFrameCount{};
	hr_time_point frameTimePoint;

	CRITICAL_SECTION fileAccessCriticalSection;

	winrt::com_ptr<IDXGIFactory> dxgiFactory;
	winrt::com_ptr<ID3D11Device> d3d11Device;
	winrt::com_ptr<ID3D11DeviceContext> immediateContext;
	winrt::com_ptr<IDXGIDevice> dxgiDevice;

	winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice d3dRtDevice;
	winrt::Windows::Graphics::Capture::GraphicsCaptureItem captureItem = { nullptr };
	winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool framePool = { nullptr };
	winrt::Windows::Graphics::Capture::GraphicsCaptureSession captureSession = { nullptr };

	winrt::Windows::Graphics::SizeInt32 lastFrameSize = {};

	void OnFrameArrived(
		winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& sender,
		winrt::Windows::Foundation::IInspectable const&);
	winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::FrameArrived_revoker frameArrivedRevoker;

	winrt::Windows::Graphics::DirectX::DirectXPixelFormat DxgiPixelFormatToRtPixelFormat(DXGI_FORMAT) const;
	int GetFormatBytesPerPixel(DXGI_FORMAT) const;

	void OpenNextOutputFile();
	void WriteRecordedImageToFile(const D3D11_MAPPED_SUBRESOURCE&, DXGI_FORMAT, winrt::Windows::Graphics::SizeInt32);

	winrt::Windows::Graphics::SizeInt32 GetMaximumSavedFrameSize(const std::vector<std::filesystem::path>& partPaths, int& frameCount) const;

	HRESULT WriteTransformOutputSamplesToSink(winrt::com_ptr<IMFTransform>& frameTransform,
		winrt::com_ptr<IMFSinkWriter>& sinkWriter, MFT_OUTPUT_DATA_BUFFER& mftOutputData) const;
};