#include "pch.h"
#include "desktop_duplication.h"

using namespace ATL;
using namespace std;

using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Numerics;
using namespace Windows::Graphics;
using namespace Windows::Graphics::Capture;
using namespace Windows::Graphics::DirectX;
using namespace Windows::Graphics::DirectX::Direct3D11;

com_ptr<DesktopDuplication> desktopDuplicationInstance;
void StartDiary(HWND _hWnd, ErrorFunc _errorFunc)
{
	MFStartup(MF_VERSION);
	desktopDuplicationInstance = make_self<DesktopDuplication>(_hWnd, _errorFunc);
}

void ExportDiaryVideo(LPWSTR outputPath, ExportDiaryVideoCompletion completion, void* completionArg)
{
	desktopDuplicationInstance->ExportVideo(outputPath, completion, completionArg);
}

void __stdcall StopDiary(StopDiaryCompletion completion, void* completionArg)
{
	if (!desktopDuplicationInstance)
		return; // nothing to stop

	thread([=] {
		desktopDuplicationInstance->StopDiaryAndWait();
		desktopDuplicationInstance = nullptr;
		completion(completionArg);
		}).detach();
}

#define CHECK_PTR(ptr) do { if (!ptr) { errorFunc(S_FALSE); return; } } while (false)
#define CHECK_HR(hr) do { if (FAILED(hr)) { errorFunc(hr); return; } } while (false)
#define CHECK_HR_RET(hr) do { if (FAILED(hr)) { errorFunc(hr); return hr; } } while (false)
#define CHECK_HR_CR(hr) do { if (FAILED(hr)) { errorFunc(hr); co_return; } } while (false)

DesktopDuplication::DesktopDuplication(HWND hWnd, ErrorFunc errorFunc)
{
	this->hWnd = hWnd;
	this->errorFunc = errorFunc;
	InitializeCriticalSection(&fileAccessCriticalSection);

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
		com_ptr<::IInspectable> d3dRawRtDevice;
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

	auto InitAsync = [this] -> fire_and_forget {
		auto self = get_strong();
		auto res = co_await GraphicsCaptureAccess::RequestAccessAsync(GraphicsCaptureAccessKind::Borderless);
		if (res == Windows::Security::Authorization::AppCapabilityAccess::AppCapabilityAccessStatus::Allowed)
			self->captureSession.IsBorderRequired(false);
		self->captureSession.StartCapture();
		};
	InitAsync();
}

static int roundUp(int numToRound, int multiple)
{
	assert(multiple && ((multiple & (multiple - 1)) == 0));
	return (numToRound + multiple - 1) & -multiple;
}

IAsyncAction DesktopDuplication::ExportVideo(wstring outputPath, ExportDiaryVideoCompletion completion, void* completionArg)
{
	EnterCriticalSection(&fileAccessCriticalSection);
	// close the current output file, and rename them to temporary names so we can parse them in peace
	outputFile.close();

	int partIdx = 0;
	vector<filesystem::path> diaryFilePaths;
	error_code ec;
	for (int i = outputFileIndex + 1; i < MAX_DIARY_FILES; ++i)
	{
		auto srcDiaryFilePath = GetDiaryFilePath(i, false);
		if (!filesystem::exists(srcDiaryFilePath, ec))
			continue; // skip if the file doesn't exist

		auto dstDiaryFilePath = srcDiaryFilePath.parent_path() / ("tmp_part_" + to_string(partIdx++));
		diaryFilePaths.push_back(dstDiaryFilePath);
		filesystem::remove(dstDiaryFilePath, ec);
		filesystem::rename(srcDiaryFilePath, dstDiaryFilePath);
	}
	for (int i = 0; i <= outputFileIndex; ++i)
	{
		auto srcDiaryFilePath = GetDiaryFilePath(i, false);
		if (!filesystem::exists(srcDiaryFilePath, ec))
			continue; // skip if the file doesn't exist

		auto dstDiaryFilePath = srcDiaryFilePath.parent_path() / ("tmp_part_" + to_string(partIdx++));
		diaryFilePaths.push_back(dstDiaryFilePath);
		filesystem::remove(dstDiaryFilePath, ec);
		filesystem::rename(srcDiaryFilePath, dstDiaryFilePath);
	}

	OpenNextOutputFile();

	LeaveCriticalSection(&fileAccessCriticalSection);

	// read the max frame size
	int frameCount{};
	auto maxFrameSize = GetMaximumSavedFrameSize(diaryFilePaths, frameCount);

	// we must convert RGB32 to NV12
	com_ptr<IMFTransform> frameTransform;
	{
		MFT_REGISTER_TYPE_INFO inputType{}, outputType{};
		inputType.guidMajorType = MFMediaType_Video;
		inputType.guidSubtype = MFVideoFormat_RGB32;
		outputType.guidMajorType = MFMediaType_Video;
		outputType.guidSubtype = MFVideoFormat_NV12;
		IMFActivate** mftActivators{};
		UINT32 mftActivatorsCount{};
		CHECK_HR_CR(MFTEnumEx(MFT_CATEGORY_VIDEO_PROCESSOR,
			MFT_ENUM_FLAG_TRANSCODE_ONLY | MFT_ENUM_FLAG_SORTANDFILTER,
			&inputType, &outputType, &mftActivators, &mftActivatorsCount));
		CHECK_HR_CR(mftActivators[0]->ActivateObject(IID_PPV_ARGS(frameTransform.put())));
		CoTaskMemFree(mftActivators);
	}
	vector<DWORD> inputStreams, outputStreams;
	{
		DWORD inputStreamCount{}, outputStreamCount{};
		CHECK_HR_CR(frameTransform->GetStreamCount(&inputStreamCount, &outputStreamCount));
		inputStreams.resize(inputStreamCount);
		outputStreams.resize(outputStreamCount);
		auto hr = frameTransform->GetStreamIDs(inputStreamCount, inputStreams.data(), outputStreamCount, outputStreams.data());
		if (hr == E_NOTIMPL)
		{
			// some MFTs don't support GetStreamIDs, so we just assume the first stream is the input and output
			inputStreams[0] = 0;
			outputStreams[0] = 0;
		}
		else
			CHECK_HR_CR(hr);

		com_ptr<IMFMediaType> mediaTypeOut, mediaTypeIn;
		CHECK_HR_CR(MFCreateMediaType(mediaTypeOut.put()));
		CHECK_HR_CR(mediaTypeOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
		CHECK_HR_CR(mediaTypeOut->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12));
		CHECK_HR_CR(MFSetAttributeSize(mediaTypeOut.get(), MF_MT_FRAME_SIZE, maxFrameSize.Width, roundUp(maxFrameSize.Height, 2)));
		CHECK_HR_CR(MFSetAttributeRatio(mediaTypeOut.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));

		CHECK_HR_CR(MFCreateMediaType(mediaTypeIn.put()));
		CHECK_HR_CR(mediaTypeIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
		CHECK_HR_CR(mediaTypeIn->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32));
		CHECK_HR_CR(MFSetAttributeSize(mediaTypeIn.get(), MF_MT_FRAME_SIZE, maxFrameSize.Width, roundUp(maxFrameSize.Height, 2)));
		CHECK_HR_CR(MFSetAttributeRatio(mediaTypeIn.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));

		frameTransform->SetInputType(inputStreams[0], mediaTypeIn.get(), 0);
		frameTransform->SetOutputType(outputStreams[0], mediaTypeOut.get(), 0);
	}

	MFT_OUTPUT_STREAM_INFO outputStreamInfo{};
	CHECK_HR_CR(frameTransform->GetOutputStreamInfo(outputStreams[0], &outputStreamInfo));
	if (outputStreamInfo.cbSize == 0)
		outputStreamInfo.cbSize = maxFrameSize.Width * roundUp(maxFrameSize.Height, 2) * 4;

	com_ptr<IMFMediaBuffer> outputBuffer;
	CHECK_HR_CR(MFCreateMemoryBuffer(outputStreamInfo.cbSize, outputBuffer.put()));

	com_ptr<IMFSample> outputSample;
	CHECK_HR_CR(MFCreateSample(outputSample.put()));
	CHECK_HR_CR(outputSample->AddBuffer(outputBuffer.get()));

	MFT_OUTPUT_DATA_BUFFER mftOutputData{};
	mftOutputData.dwStreamID = outputStreams[0];
	mftOutputData.pSample = outputSample.get();

	com_ptr<IMFSinkWriter> sinkWriter;
	DWORD streamIndex{};
	CHECK_HR_CR(MFCreateSinkWriterFromURL(outputPath.data(), nullptr, nullptr, sinkWriter.put()));

	{
		com_ptr<IMFMediaType> mediaTypeOut, mediaTypeIn;
		CHECK_HR_CR(MFCreateMediaType(mediaTypeOut.put()));
		CHECK_HR_CR(mediaTypeOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
		CHECK_HR_CR(mediaTypeOut->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264));
		CHECK_HR_CR(mediaTypeOut->SetUINT32(MF_MT_AVG_BITRATE, DIARY_VIDEO_BITRATE));
		CHECK_HR_CR(MFSetAttributeSize(mediaTypeOut.get(), MF_MT_FRAME_SIZE, maxFrameSize.Width, roundUp(maxFrameSize.Height, 2)));
		CHECK_HR_CR(MFSetAttributeRatio(mediaTypeOut.get(), MF_MT_FRAME_RATE, 30, 1)); // 30 FPS
		CHECK_HR_CR(MFSetAttributeRatio(mediaTypeOut.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
		CHECK_HR_CR(mediaTypeOut->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
		CHECK_HR_CR(mediaTypeOut->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));

		CHECK_HR_CR(MFCreateMediaType(mediaTypeIn.put()));
		CHECK_HR_CR(mediaTypeIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
		CHECK_HR_CR(mediaTypeIn->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12));
		CHECK_HR_CR(MFSetAttributeSize(mediaTypeIn.get(), MF_MT_FRAME_SIZE, maxFrameSize.Width, roundUp(maxFrameSize.Height, 2)));
		CHECK_HR_CR(MFSetAttributeRatio(mediaTypeIn.get(), MF_MT_FRAME_RATE, 30, 1)); // 30 FPS
		CHECK_HR_CR(MFSetAttributeRatio(mediaTypeIn.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));

		CHECK_HR_CR(sinkWriter->AddStream(mediaTypeOut.get(), &streamIndex));
		CHECK_HR_CR(sinkWriter->SetInputMediaType(streamIndex, mediaTypeIn.get(), nullptr));
		CHECK_HR_CR(sinkWriter->BeginWriting());
	}

	int64_t frameTimePointNs{};
	int frameIndex{};
	for (auto& diaryFilePath : diaryFilePaths)
	{
		ifstream inputFile(diaryFilePath, ios::binary | ios::in);

		while (true)
		{
			int width{}, height{};
			DXGI_FORMAT format{};
			hr_time_point::rep frameTimeNs{};
			inputFile.read(reinterpret_cast<char*>(&width), sizeof(width));
			if (inputFile.eof())
				break; // end of file
			inputFile.read(reinterpret_cast<char*>(&height), sizeof(height));
			inputFile.read(reinterpret_cast<char*>(&format), sizeof(format));
			inputFile.read(reinterpret_cast<char*>(&frameTimeNs), sizeof(frameTimeNs));

			// advance the time
			frameTimePointNs += frameTimeNs;
			auto bpp = GetFormatBytesPerPixel(format);

			{
				// MFT transform
				com_ptr<IMFSample> sample;
				CHECK_HR_CR(MFCreateSample(sample.put()));
				CHECK_HR_CR(sample->SetSampleTime(frameTimePointNs / 100));

				com_ptr<IMFMediaBuffer> mediaBuffer;
				CHECK_HR_CR(MFCreateAlignedMemoryBuffer(width * roundUp(height, 2) * bpp, sizeof(void*), mediaBuffer.put()));

				BYTE* data = nullptr;
				CHECK_HR_CR(mediaBuffer->Lock(&data, nullptr, nullptr));
				inputFile.read((char*)data, width * height * bpp);
				CHECK_HR_CR(mediaBuffer->Unlock());
				CHECK_HR_CR(mediaBuffer->SetCurrentLength(width * roundUp(height, 2) * bpp));

				CHECK_HR_CR(sample->AddBuffer(mediaBuffer.get()));
				CHECK_HR_CR(frameTransform->ProcessInput(inputStreams[0], sample.get(), 0));

				completion(++frameIndex / (float)frameCount, completionArg);
			}

			// samples
			CHECK_HR_CR(WriteTransformOutputSamplesToSink(frameTransform, sinkWriter, mftOutputData));
		}

		// delete the part file
		inputFile.close();
		filesystem::remove(diaryFilePath, ec);
	}

	// drain the MFT
	frameTransform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
	CHECK_HR_CR(WriteTransformOutputSamplesToSink(frameTransform, sinkWriter, mftOutputData));

	sinkWriter->Finalize();

	completion(-1, completionArg); // signal completion
}

void DesktopDuplication::StopDiaryAndWait()
{
	captureSession.Close();
	outputFile.close();

	for (int diaryIdx = 0;; ++diaryIdx)
	{
		auto diaryFilePath = GetDiaryFilePath(diaryIdx, false);
		if (!filesystem::exists(diaryFilePath))
			break; // no more diary files

		error_code ec;
		while (!filesystem::remove(diaryFilePath, ec))
		{
			// keep retrying to delete these files until the capture is done with them
		}
	}
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
		CHECK_HR(access->GetInterface(IID_PPV_ARGS(newFrameTexture.put())));
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

std::filesystem::path DesktopDuplication::GetDiaryFilePath(int index, bool create) const
{
	filesystem::path diaryPath = filesystem::current_path() / ".diary";

	if (create)
		filesystem::create_directory(diaryPath);

	return diaryPath / ("diary_" + to_string(index) + ".dat");
}

void DesktopDuplication::OpenNextOutputFile()
{
	outputFileIndex = (outputFileIndex + 1) % MAX_DIARY_FILES;

	outputFile.close();
	outputFile.open(GetDiaryFilePath(outputFileIndex, true), ios::binary | ios::out | ios::trunc);

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
		EnterCriticalSection(&fileAccessCriticalSection);
		outputFile.write(reinterpret_cast<const char*>(&newFrameSize.Width), sizeof(newFrameSize.Width));
		outputFile.write(reinterpret_cast<const char*>(&newFrameSize.Height), sizeof(newFrameSize.Height));
		outputFile.write(reinterpret_cast<const char*>(&format), sizeof(format));

		outputFile.write(reinterpret_cast<const char*>(&time_span_ns), sizeof(time_span_ns));

		for (int y = 0; y < newFrameSize.Height; ++y)
			outputFile.write(reinterpret_cast<const char*>(mappedResource.pData) + (newFrameSize.Height - y - 1) * mappedResource.RowPitch,
				newFrameSize.Width * bytesPerPixel);
		LeaveCriticalSection(&fileAccessCriticalSection);

		++outputFileFrameCount;
		frameTimePoint = now;

		// file switch?
		if (outputFileFrameCount > MAX_FRAMES_PER_DIARY_FILE)
			OpenNextOutputFile();
	}
}

Windows::Graphics::SizeInt32 DesktopDuplication::GetMaximumSavedFrameSize(const vector<filesystem::path>& partPaths, int& frameCount) const
{
	SizeInt32 maxSize{};
	frameCount = 0;

	for (const auto& partPath : partPaths)
	{
		ifstream inputFile(partPath, ios::binary | ios::in);

		while (true)
		{
			SizeInt32 size{};
			inputFile.read(reinterpret_cast<char*>(&size.Width), sizeof(size.Width));
			if (inputFile.eof())
				break;
			inputFile.read(reinterpret_cast<char*>(&size.Height), sizeof(size.Height));
			DXGI_FORMAT format{};
			inputFile.read(reinterpret_cast<char*>(&format), sizeof(format));
			inputFile.seekg(sizeof(chrono::nanoseconds::rep), ios::cur); // skip timestamp
			inputFile.seekg(size.Width * size.Height * GetFormatBytesPerPixel(format), ios::cur); // skip pixel data
			if (inputFile.eof())
				break;

			maxSize.Width = max(maxSize.Width, size.Width);
			maxSize.Height = max(maxSize.Height, size.Height);
			++frameCount;
		}
	}

	return maxSize;
}

HRESULT DesktopDuplication::WriteTransformOutputSamplesToSink(com_ptr<IMFTransform>& frameTransform,
	com_ptr<IMFSinkWriter>& sinkWriter, MFT_OUTPUT_DATA_BUFFER& mftOutputData)
{
nextSample:
	DWORD outputStatus = 0;
	auto outputHr = frameTransform->ProcessOutput(0, 1, &mftOutputData, &outputStatus);
	if (outputHr == S_OK)
	{
		CHECK_HR_RET(sinkWriter->WriteSample(mftOutputData.dwStreamID, mftOutputData.pSample));
		goto nextSample;
	}
	else if (outputHr == MF_E_TRANSFORM_NEED_MORE_INPUT)
		return S_OK;
	else if (FAILED(outputHr))
		CHECK_HR_RET(outputHr);

	return E_NOTIMPL;
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
