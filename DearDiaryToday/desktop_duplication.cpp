#include "pch.h"
#include "desktop_duplication.h"

using namespace ATL;
using namespace std;

using namespace winrt;
using namespace Windows::Foundation::Numerics;
using namespace Windows::Graphics;
using namespace Windows::Graphics::Capture;
using namespace Windows::Graphics::DirectX;
using namespace Windows::Graphics::DirectX::Direct3D11;

unique_ptr<DesktopDuplication> desktopDuplicationInstance;
void StartDiary(HWND _hWnd, ErrorFunc _errorFunc)
{
	desktopDuplicationInstance = make_unique<DesktopDuplication>(_hWnd, _errorFunc);
}

#define CHECK_PTR(ptr) do { if (!ptr) { errorFunc(S_FALSE); return; } } while (false)
#define CHECK_HR(hr) do { if (FAILED(hr)) { errorFunc(hr); return; } } while (false)

DesktopDuplication::DesktopDuplication(HWND hWnd, ErrorFunc errorFunc)
{
	this->hWnd = hWnd;
	this->errorFunc = errorFunc;

	CHECK_HR(CreateDXGIFactory(IID_PPV_ARGS(dxgiFactory.put())));

	D3D_FEATURE_LEVEL featureLevels[] =
	{
		D3D_FEATURE_LEVEL_11_1,
		D3D_FEATURE_LEVEL_11_0,
		D3D_FEATURE_LEVEL_10_1,
		D3D_FEATURE_LEVEL_10_0,
		D3D_FEATURE_LEVEL_9_1
	};
	D3D_FEATURE_LEVEL featureLevel{};

	CHECK_HR(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
		featureLevels, sizeof(featureLevels) / sizeof(*featureLevels), D3D11_SDK_VERSION, d3d11Device.put(),
		&featureLevel, immediateContext.put()));

	dxgiDevice = d3d11Device.as<IDXGIDevice>();

	{
		com_ptr<IInspectable> d3dRawRtDevice;
		CHECK_HR(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), d3dRawRtDevice.put()));
		d3dRtDevice = d3dRawRtDevice.as<IDirect3DDevice>();
	}

	{
		auto activationFactory = winrt::get_activation_factory<GraphicsCaptureItem>();
		auto interopFactory = activationFactory.as<IGraphicsCaptureItemInterop>();
		interopFactory->CreateForWindow(hWnd, guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(), reinterpret_cast<void**>(put_abi(captureItem)));
	}

	lastFrameSize = captureItem.Size();
	framePool = Direct3D11CaptureFramePool::Create(d3dRtDevice, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, lastFrameSize);
	captureSession = framePool.CreateCaptureSession(captureItem);
	frameArrivedRevoker = framePool.FrameArrived(auto_revoke, { this, &DesktopDuplication::OnFrameArrived });

	OpenNextOutputFile();

	GraphicsCaptureAccess::RequestAccessAsync(GraphicsCaptureAccessKind::Borderless).Completed(
		[this](auto&& result, auto&& status) {
			captureSession.IsBorderRequired(false);
			captureSession.StartCapture();
		});
}

DirectXPixelFormat DesktopDuplication::DxgiPixelFormatToRtPixelFormat(DXGI_FORMAT dxgiFormat) const
{
	switch (dxgiFormat)
	{
	case DXGI_FORMAT_R8G8B8A8_UNORM: return DirectXPixelFormat::R8G8B8A8UIntNormalized;
	case DXGI_FORMAT_B8G8R8A8_UNORM: return DirectXPixelFormat::B8G8R8A8UIntNormalized;
	case DXGI_FORMAT_R16G16B16A16_FLOAT: return DirectXPixelFormat::R16G16B16A16Float;
	default: errorFunc(E_NOTIMPL);
	}
}

void DesktopDuplication::OnFrameArrived(Direct3D11CaptureFramePool const& sender, Windows::Foundation::IInspectable const&)
{
	auto newFrame = sender.TryGetNextFrame();
	auto newFrameSize = newFrame.ContentSize();

	auto newFrameSurface = newFrame.Surface();

	// get frame texture
	com_ptr<ID3D11Texture2D> newFrameTexture;
	{
		auto access = newFrameSurface.as<IDirect3DDxgiInterfaceAccess>();
		CHECK_HR(access->GetInterface(guid_of<ID3D11Texture2D>(), newFrameTexture.put_void()));
	}

	// create a staging texture
	D3D11_TEXTURE2D_DESC desc{};
	newFrameTexture->GetDesc(&desc);
	desc.Usage = D3D11_USAGE_STAGING;
	desc.BindFlags = 0;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	desc.MiscFlags = 0;

	com_ptr<ID3D11Texture2D> stagingTexture;
	CHECK_HR(d3d11Device->CreateTexture2D(&desc, nullptr, stagingTexture.put()));
	immediateContext->CopyResource(stagingTexture.get(), newFrameTexture.get());
	newFrameTexture = nullptr;
	newFrameSurface = nullptr;

	auto newFrameRtPixelFormat = DxgiPixelFormatToRtPixelFormat(desc.Format);

	// try to map for reading
	D3D11_MAPPED_SUBRESOURCE mappedResource{};
	CHECK_HR(immediateContext->Map(stagingTexture.get(), 0, D3D11_MAP_READ, 0, &mappedResource));
	DumpFrameData(mappedResource, desc.Format, newFrameSize);
	immediateContext->Unmap(stagingTexture.get(), 0);

	// resize the frame pool if the size has changed
	if (newFrameSize != lastFrameSize)
	{
		lastFrameSize = newFrameSize;
		framePool.Recreate(d3dRtDevice, newFrameRtPixelFormat, 2, lastFrameSize);
	}
}

void DesktopDuplication::OpenNextOutputFile()
{
	outputFileIndex = (outputFileIndex + 1) % MAX_DIARY_FILES;

	filesystem::path diaryPath = filesystem::current_path() / ".diary";
	filesystem::create_directory(diaryPath);

	auto diaryFilePath = diaryPath / ("diary_" + to_string(outputFileIndex) + ".dat");
	outputFile.close();
	outputFile.open(diaryFilePath, ios::binary | ios::out | ios::trunc);

	outputFileFrameCount = 0;
}

void DesktopDuplication::DumpFrameData(const D3D11_MAPPED_SUBRESOURCE& mappedResource, DXGI_FORMAT format, SizeInt32 newFrameSize)
{
	auto bytesPerPixel = GetFormatBytesPerPixel(format);

	// don't know why the size doesn't match the buffer
	if (mappedResource.RowPitch * newFrameSize.Height > mappedResource.DepthPitch)
		return;

	hr_time_point now = hr_clock::now();
	if (outputFileFrameCount == 0) frameTimePoint = now;

	auto time_span_ns = duration_cast<chrono::nanoseconds> (now - frameTimePoint).count();

	// max frame rate
	if (outputFileFrameCount == 0 || time_span_ns >= 1.0 / MAX_FRAME_RATE * chrono::nanoseconds(1s).count())
	{
		outputFile.write(reinterpret_cast<const char*>(&newFrameSize.Width), sizeof(newFrameSize.Width));
		outputFile.write(reinterpret_cast<const char*>(&newFrameSize.Height), sizeof(newFrameSize.Height));
		outputFile.write(reinterpret_cast<const char*>(&format), sizeof(format));

		outputFile.write(reinterpret_cast<const char*>(&time_span_ns), sizeof(time_span_ns));

		for (int y = 0; y < newFrameSize.Height; ++y)
			outputFile.write(reinterpret_cast<const char*>(mappedResource.pData) + y * mappedResource.RowPitch,
				newFrameSize.Width * bytesPerPixel);

		++outputFileFrameCount;
		OutputDebugStringA(std::format("Frame: {}\n", outputFileFrameCount).c_str());
		frameTimePoint = now;

		// file switch?
		if (outputFileFrameCount > MAX_FRAMES_PER_DIARY_FILE)
			OpenNextOutputFile();
	}
}

int DesktopDuplication::GetFormatBytesPerPixel(DXGI_FORMAT format) const
{
	switch (format)
	{
	case DXGI_FORMAT_R8G8B8A8_UNORM: return 4;
	case DXGI_FORMAT_B8G8R8A8_UNORM: return 4;
	case DXGI_FORMAT_R16G16B16A16_FLOAT: return 8;
	default: errorFunc(E_NOTIMPL);
	}
}

//static void DiaryProc()
//{
//	int outputFileIndex{};
//	ofstream outputFile(GetDiaryFilePath(outputFileIndex));
//	int frameCount = 0;
//
//	UINT metadataSize = 0;
//	vector<RECT> dirtyRects;
//	while (true)
//	{
//		{
//			DXGI_OUTDUPL_FRAME_INFO frameInfo{};
//
//			D3D11_TEXTURE2D_DESC desc{};
//			CComPtr<ID3D11Texture2D> lastFrameTexture;
//			{
//				CComPtr<IDXGIResource> desktopResource;
//				auto hr = outputDuplication->AcquireNextFrame(500, &frameInfo, &desktopResource);
//				if (hr == DXGI_ERROR_WAIT_TIMEOUT)
//					continue;
//				CHECK_HR(hr);
//
//				CHECK_HR(desktopResource.QueryInterface(&lastFrameTexture));
//				lastFrameTexture->GetDesc(&desc);
//			}
//
//			assert(desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM
//				|| desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT);
//
//			CComPtr<ID3D11Texture2D> stagingTexture;
//
//			if (frameInfo.TotalMetadataBufferSize > 0)
//			{
//				if (frameInfo.TotalMetadataBufferSize > metadataSize)
//					dirtyRects.resize((metadataSize = frameInfo.TotalMetadataBufferSize) / sizeof(RECT));
//
//				UINT dirtyRectSizeRequired = metadataSize;
//				CHECK_HR(outputDuplication->GetFrameDirtyRects(metadataSize, dirtyRects.data(), &dirtyRectSizeRequired));
//			}
//
//			GetWindowRect(hWnd, &rect);
//
//			// remove any dirty rects that are outside the window
//			dirtyRects.erase(remove_if(dirtyRects.begin(), dirtyRects.end(), [&](const RECT& r) {
//				return r.left >= rect.right || r.right <= rect.left || r.top >= rect.bottom || r.bottom <= rect.top;
//				}), dirtyRects.end());
//
//			if (dirtyRects.size() == 0 && frameCount)
//			{
//				// no dirty rects, we don't need a keyframe, just skip
//				CHECK_HR(outputDuplication->ReleaseFrame());
//				continue;
//			}
//
//			// copy the texture to a staging texture
//			D3D11_MAPPED_SUBRESOURCE mappedResource;
//			D3D11_TEXTURE2D_DESC stagingDesc = desc;
//			stagingDesc.Usage = D3D11_USAGE_STAGING;
//			stagingDesc.BindFlags = 0;
//			stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
//			stagingDesc.MiscFlags = 0;
//
//			CHECK_HR(d3d11Device->CreateTexture2D(&stagingDesc, nullptr, &stagingTexture));
//			immediateContext->CopyResource(stagingTexture, lastFrameTexture);
//			lastFrameTexture.Release();
//			CHECK_HR(outputDuplication->ReleaseFrame());
//
//			CHECK_HR(immediateContext->Map(stagingTexture, 0, D3D11_MAP_READ, 0, &mappedResource));
//
//			bool isFullScreenDirty = dirtyRects.size() == 1 && dirtyRects[0].left == rect.left &&
//				dirtyRects[0].top == rect.top && dirtyRects[0].right == rect.right && dirtyRects[0].bottom == rect.bottom;
//
//			if (frameCount == 0 || isFullScreenDirty)
//			{
//				auto dim = rect.right - rect.left;
//				outputFile.write((const char*)&dim, sizeof(dim));
//				dim = rect.bottom - rect.top;
//				outputFile.write((const char*)&dim, sizeof(dim));
//				outputFile.write((const char*)&desc.Format, sizeof(desc.Format));
//				size_t zeroCount = 0;
//				outputFile.write((const char*)&zeroCount, sizeof(zeroCount));
//
//				// dump the entire window
//				for (auto y = rect.top; y < rect.bottom; ++y)
//					outputFile.write(
//						(const char*)(mappedResource.pData) + y * mappedResource.RowPitch + rect.left * 4,
//						(rect.right - rect.left) * 4);
//			}
//			else if (dirtyRects.size() > 0)
//			{
//				auto dim = rect.right - rect.left;
//				outputFile.write((const char*)&dim, sizeof(dim));
//				dim = rect.bottom - rect.top;
//				outputFile.write((const char*)&dim, sizeof(dim));
//				outputFile.write((const char*)&desc.Format, sizeof(desc.Format));
//				size_t dirtyRectCount = dirtyRects.size();
//				outputFile.write((const char*)&dirtyRectCount, sizeof(dirtyRectCount));
//
//				for (auto& dirtyRect : dirtyRects)
//				{
//					outputFile.write((const char*)&dirtyRect.left, sizeof(dirtyRect.left));
//					outputFile.write((const char*)&dirtyRect.top, sizeof(dirtyRect.top));
//					outputFile.write((const char*)&dirtyRect.right, sizeof(dirtyRect.right));
//					outputFile.write((const char*)&dirtyRect.bottom, sizeof(dirtyRect.bottom));
//
//					for (auto y = dirtyRect.top; y < dirtyRect.bottom; ++y)
//						outputFile.write(
//							(const char*)(mappedResource.pData) + y * mappedResource.RowPitch + dirtyRect.left * 4,
//							(dirtyRect.right - dirtyRect.left) * 4);
//				}
//			}
//
//			immediateContext->Unmap(stagingTexture, 0);
//			++frameCount;
//		}
//
//	}
//}