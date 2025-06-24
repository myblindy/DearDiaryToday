#include "pch.h"
#include "desktop_duplication.h"
#include "LzmaDecoder.h"

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

bool __stdcall InitializeDiary(ErrorFunc _errorFunc)
{
	MFStartup(MF_VERSION);
	desktopDuplicationInstance = make_self<DesktopDuplication>(_errorFunc);

	for (int i = 0; i < MAX_DIARY_FILES; ++i)
	{
		auto diaryFilePath = DesktopDuplication::GetDiaryFilePath(i, false);
		if (filesystem::exists(diaryFilePath))
			return true; // found a diary file
	}
	return false; // no diary files found
}

void StartDiary(HWND hWnd)
{
	// delete all left over diary files
	for (int i = 0; i < MAX_DIARY_FILES; ++i)
	{
		auto diaryFilePath = DesktopDuplication::GetDiaryFilePath(i, false);

		error_code ec;
		filesystem::remove(diaryFilePath, ec);
	}

	desktopDuplicationInstance->Start(hWnd);
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

DesktopDuplication::DesktopDuplication(ErrorFunc errorFunc)
	: errorFunc(errorFunc)
{
	InitializeCriticalSection(&fileAccessCriticalSection);

	frameProcessingThread = thread([=] {
		hr_time_point frameTimePoint{};
		int outputFileFrameCount{};

		while (!stopping)
		{
			FrameData frameData;
			while (!stopping && frames.try_dequeue(frameData))
			{
				if (outputFileFrameCount == 0) frameTimePoint = frameData.now;

				auto time_span_ns = duration_cast<chrono::nanoseconds> (frameData.now - frameTimePoint).count();

				// frame rate limiter
				if (outputFileFrameCount == 0 || time_span_ns >= 1.0 / MAX_FRAME_RATE * chrono::nanoseconds(1s).count())
				{
					// NV12 requires the height to be a multiple of 2, and we might as well do it here
					auto roundFrameWidth = roundUp(frameData.width, 2);
					auto roundFrameHeight = roundUp(frameData.height, 2);

					EnterCriticalSection(&fileAccessCriticalSection);
					lzmaEncoder->Encode(roundFrameWidth);
					lzmaEncoder->Encode(roundFrameHeight);
					lzmaEncoder->Encode(frameData.format);
					lzmaEncoder->Encode(time_span_ns);

					for (int y = 0; y < frameData.height; ++y)
					{
						lzmaEncoder->Encode({
							reinterpret_cast<const BYTE*>(frameData.data.data()) + (frameData.height - y - 1) * frameData.stride,
							static_cast<size_t>(frameData.width * 4) });
						if (frameData.width < roundFrameWidth)
						{
							// pad end of row if necessary
							lzmaEncoder->Encode(uint32_t{});
						}
					}
					if (frameData.height < roundFrameHeight)
					{
						// pad end of frame if necessary
						char* row = (char*)_malloca(roundFrameWidth * 4);
						assert(row);
						memset(row, 0, roundFrameWidth * 4);
						lzmaEncoder->Encode(span{ row, static_cast<size_t>(roundFrameWidth * 4) });
						_freea(row);
					}

					LeaveCriticalSection(&fileAccessCriticalSection);

					++outputFileFrameCount;
					frameTimePoint = frameData.now;

					// file switch?
					if (outputFileFrameCount > MAX_FRAMES_PER_DIARY_FILE)
					{
						OpenNextOutputFile();
						outputFileFrameCount = 0;
					}
				}
			}

			WaitForSingleObject(newFrameReadyEvent.get(), 10);
		}
		});

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

}

IAsyncAction DesktopDuplication::Start(HWND hWnd)
{
	auto self = get_strong();

	{
		auto activationFactory = winrt::get_activation_factory<GraphicsCaptureItem>();
		auto interopFactory = activationFactory.as<IGraphicsCaptureItemInterop>();
		interopFactory->CreateForWindow(hWnd, guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(), reinterpret_cast<void**>(put_abi(captureItem)));
	}

	lastFrameSize = captureItem.Size();
	framePool = Direct3D11CaptureFramePool::CreateFreeThreaded(d3dRtDevice, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, lastFrameSize);
	captureSession = framePool.CreateCaptureSession(captureItem);
	frameArrivedRevoker = framePool.FrameArrived(auto_revoke, { this, &DesktopDuplication::OnFrameArrived });

	OpenNextOutputFile();

	auto res = co_await GraphicsCaptureAccess::RequestAccessAsync(GraphicsCaptureAccessKind::Borderless);
	if (res == Windows::Security::Authorization::AppCapabilityAccess::AppCapabilityAccessStatus::Allowed)
		self->captureSession.IsBorderRequired(false);
	self->captureSession.StartCapture();
}


void DesktopDuplication::ExportVideo(wstring outputPath, ExportDiaryVideoCompletion completion, void* completionArg)
{
	EnterCriticalSection(&fileAccessCriticalSection);
	// close the current output file, and rename them to temporary names so we can parse them in peace
	lzmaEncoder.reset();

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

	com_ptr<IMFSinkWriter> sinkWriter;
	DWORD streamIndex{};
	CHECK_HR(MFCreateSinkWriterFromURL(outputPath.data(), nullptr, nullptr, sinkWriter.put()));

	if (maxFrameSize.Width > 0 && maxFrameSize.Height > 0)
	{
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
			CHECK_HR(MFTEnumEx(MFT_CATEGORY_VIDEO_PROCESSOR,
				MFT_ENUM_FLAG_TRANSCODE_ONLY | MFT_ENUM_FLAG_SORTANDFILTER,
				&inputType, &outputType, &mftActivators, &mftActivatorsCount));
			CHECK_HR(mftActivators[0]->ActivateObject(IID_PPV_ARGS(frameTransform.put())));
			CoTaskMemFree(mftActivators);
		}
		vector<DWORD> inputStreams, outputStreams;
		{
			DWORD inputStreamCount{}, outputStreamCount{};
			CHECK_HR(frameTransform->GetStreamCount(&inputStreamCount, &outputStreamCount));
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
				CHECK_HR(hr);

			com_ptr<IMFMediaType> mediaTypeOut, mediaTypeIn;
			CHECK_HR(MFCreateMediaType(mediaTypeOut.put()));
			CHECK_HR(mediaTypeOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
			CHECK_HR(mediaTypeOut->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12));
			CHECK_HR(MFSetAttributeSize(mediaTypeOut.get(), MF_MT_FRAME_SIZE, maxFrameSize.Width, maxFrameSize.Height));
			CHECK_HR(MFSetAttributeRatio(mediaTypeOut.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));

			CHECK_HR(MFCreateMediaType(mediaTypeIn.put()));
			CHECK_HR(mediaTypeIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
			CHECK_HR(mediaTypeIn->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32));
			CHECK_HR(MFSetAttributeSize(mediaTypeIn.get(), MF_MT_FRAME_SIZE, maxFrameSize.Width, maxFrameSize.Height));
			CHECK_HR(MFSetAttributeRatio(mediaTypeIn.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));

			frameTransform->SetInputType(inputStreams[0], mediaTypeIn.get(), 0);
			frameTransform->SetOutputType(outputStreams[0], mediaTypeOut.get(), 0);
		}

		MFT_OUTPUT_STREAM_INFO outputStreamInfo{};
		CHECK_HR(frameTransform->GetOutputStreamInfo(outputStreams[0], &outputStreamInfo));
		if (outputStreamInfo.cbSize == 0)
			outputStreamInfo.cbSize = maxFrameSize.Width * maxFrameSize.Height * 4;

		com_ptr<IMFMediaBuffer> outputBuffer;
		CHECK_HR(MFCreateMemoryBuffer(outputStreamInfo.cbSize, outputBuffer.put()));

		com_ptr<IMFSample> outputSample;
		CHECK_HR(MFCreateSample(outputSample.put()));
		CHECK_HR(outputSample->AddBuffer(outputBuffer.get()));

		MFT_OUTPUT_DATA_BUFFER mftOutputData{};
		mftOutputData.dwStreamID = outputStreams[0];
		mftOutputData.pSample = outputSample.get();

		{
			com_ptr<IMFMediaType> mediaTypeOut, mediaTypeIn;
			CHECK_HR(MFCreateMediaType(mediaTypeOut.put()));
			CHECK_HR(mediaTypeOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
			CHECK_HR(mediaTypeOut->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264));
			CHECK_HR(mediaTypeOut->SetUINT32(MF_MT_AVG_BITRATE, DIARY_VIDEO_BITRATE));
			CHECK_HR(MFSetAttributeSize(mediaTypeOut.get(), MF_MT_FRAME_SIZE, maxFrameSize.Width, maxFrameSize.Height));
			CHECK_HR(MFSetAttributeRatio(mediaTypeOut.get(), MF_MT_FRAME_RATE, 30, 1)); // 30 FPS
			CHECK_HR(MFSetAttributeRatio(mediaTypeOut.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
			CHECK_HR(mediaTypeOut->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
			CHECK_HR(mediaTypeOut->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));
			CHECK_HR(mediaTypeOut->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High));

			CHECK_HR(MFCreateMediaType(mediaTypeIn.put()));
			CHECK_HR(mediaTypeIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
			CHECK_HR(mediaTypeIn->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12));
			CHECK_HR(MFSetAttributeSize(mediaTypeIn.get(), MF_MT_FRAME_SIZE, maxFrameSize.Width, maxFrameSize.Height));
			CHECK_HR(MFSetAttributeRatio(mediaTypeIn.get(), MF_MT_FRAME_RATE, 30, 1)); // 30 FPS
			CHECK_HR(MFSetAttributeRatio(mediaTypeIn.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));

			com_ptr<IMFAttributes> attributes;
			CHECK_HR(MFCreateAttributes(attributes.put(), 1));
			CHECK_HR(attributes->SetUINT32(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_Quality));
			CHECK_HR(attributes->SetUINT32(CODECAPI_AVEncCommonQuality, 40));
			CHECK_HR(attributes->SetUINT32(CODECAPI_AVEncMPVGOPSize, 4));

			CHECK_HR(sinkWriter->AddStream(mediaTypeOut.get(), &streamIndex));
			CHECK_HR(sinkWriter->SetInputMediaType(streamIndex, mediaTypeIn.get(), attributes.get()));
			CHECK_HR(sinkWriter->BeginWriting());
		}

		int64_t frameTimePointNs{};
		int frameIndex{};
		for (auto& diaryFilePath : diaryFilePaths)
		{
			{
				LzmaDecoder decoder(make_unique<ifstream>(diaryFilePath, ios::binary | ios::in), errorFunc);

				while (true)
				{
					int width{}, height{};
					DXGI_FORMAT format{};
					hr_time_point::rep frameTimeNs{};
					decoder.Decode(width);
					if (decoder.IsEof())
						break; // end of file
					decoder.Decode(height);
					if (decoder.IsEof())
						break; // end of file
					decoder.Decode(format);
					if (decoder.IsEof())
						break; // end of file
					decoder.Decode(frameTimeNs);
					if (decoder.IsEof())
						break; // end of file

					// advance the time
					frameTimePointNs += frameTimeNs;
					auto bpp = GetFormatBytesPerPixel(format);

					{
						// MFT transform
						com_ptr<IMFSample> sample;
						CHECK_HR(MFCreateSample(sample.put()));
						CHECK_HR(sample->SetSampleTime(frameTimePointNs / 100));

						com_ptr<IMFMediaBuffer> mediaBuffer;
						CHECK_HR(MFCreateAlignedMemoryBuffer(maxFrameSize.Width * maxFrameSize.Height * bpp, sizeof(void*), mediaBuffer.put()));

						BYTE* data = nullptr;
						CHECK_HR(mediaBuffer->Lock(&data, nullptr, nullptr));

						auto rowPadding = maxFrameSize.Width - width;
						auto yOffset = (maxFrameSize.Height - height) * maxFrameSize.Width * bpp;
						if (!rowPadding)
							decoder.Decode({ data + yOffset, static_cast<size_t>(width * height * bpp) });
						else
							for (int y = 0; y < height; ++y)
							{
								decoder.Decode({ data + y * maxFrameSize.Width * bpp + yOffset, static_cast<size_t>(width * bpp) });
								memset(data + y * maxFrameSize.Width * bpp + yOffset + width * bpp, 0, rowPadding * bpp);
							}

						CHECK_HR(mediaBuffer->Unlock());
						CHECK_HR(mediaBuffer->SetCurrentLength(width * height * bpp));

						CHECK_HR(sample->AddBuffer(mediaBuffer.get()));
						CHECK_HR(frameTransform->ProcessInput(inputStreams[0], sample.get(), 0));

						completion(++frameIndex / (float)frameCount, completionArg);
					}

					// samples
					CHECK_HR(WriteTransformOutputSamplesToSink(frameTransform, sinkWriter, mftOutputData));
				}
			}
			filesystem::remove(diaryFilePath, ec);
		}

		// drain the MFT
		frameTransform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
		CHECK_HR(WriteTransformOutputSamplesToSink(frameTransform, sinkWriter, mftOutputData));
	}

	sinkWriter->Finalize();

	completion(-1, completionArg); // signal completion
}

void DesktopDuplication::StopDiaryAndWait()
{
	stopping = true;

	captureSession.Close();
	frameProcessingThread.join();
	lzmaEncoder.reset();

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
	if (stopping) return;

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
	WriteRecordedImageToCircularFrameBuffer(mappedResource, desc.Format, newFrameSize);
	immediateContext->Unmap(stagingTexture.get(), 0);

	// resize the frame pool if the size has changed
	if (newFrameSize != lastFrameSize)
	{
		lastFrameSize = newFrameSize;
		framePool.Recreate(d3dRtDevice, newFrameRtPixelFormat, 2, lastFrameSize);
	}
}

std::filesystem::path DesktopDuplication::GetDiaryFilePath(int index, bool create)
{
	filesystem::path diaryPath = filesystem::current_path() / ".diary";

	if (create)
		filesystem::create_directory(diaryPath);

	return diaryPath / ("diary_" + to_string(index) + ".dat");
}

void DesktopDuplication::OpenNextOutputFile()
{
	outputFileIndex = (outputFileIndex + 1) % MAX_DIARY_FILES;

	lzmaEncoder = make_unique<LzmaEncoder>(
		make_unique<ofstream>(GetDiaryFilePath(outputFileIndex, true), ios::binary | ios::out | ios::trunc),
		errorFunc);
}

void DesktopDuplication::WriteRecordedImageToCircularFrameBuffer(const D3D11_MAPPED_SUBRESOURCE& mappedResource, DXGI_FORMAT format, SizeInt32 newFrameSize)
{
	auto bytesPerPixel = GetFormatBytesPerPixel(format);
	assert(bytesPerPixel == 4);

	// don't know why the size doesn't match the buffer
	if (mappedResource.RowPitch * newFrameSize.Height > mappedResource.DepthPitch)
		return;

	vector<BYTE> frameBytes(mappedResource.RowPitch * newFrameSize.Height);
	copy(reinterpret_cast<const BYTE*>(mappedResource.pData),
		reinterpret_cast<const BYTE*>(mappedResource.pData) + mappedResource.RowPitch * newFrameSize.Height,
		frameBytes.begin());

	frames.try_enqueue({ newFrameSize.Width, newFrameSize.Height, (int)mappedResource.RowPitch,
		format, hr_clock::now(), move(frameBytes) });
	SetEvent(newFrameReadyEvent.get()); // signal that a new frame is ready
}

Windows::Graphics::SizeInt32 DesktopDuplication::GetMaximumSavedFrameSize(const vector<filesystem::path>& partPaths, int& frameCount) const
{
	SizeInt32 maxSize{};
	frameCount = 0;

	for (const auto& partPath : partPaths)
	{
		LzmaDecoder decoder(make_unique<ifstream>(partPath, ios::binary | ios::in), errorFunc);

		while (true)
		{
			SizeInt32 size{};
			decoder.Decode(size.Width);
			if (decoder.IsEof())
				break;
			decoder.Decode(size.Height);
			if (decoder.IsEof())
				break;
			DXGI_FORMAT format{};
			decoder.Decode(format);
			if (decoder.IsEof())
				break;
			decoder.Skip(sizeof(chrono::nanoseconds::rep)); // skip timestamp
			decoder.Skip(size.Width * size.Height * GetFormatBytesPerPixel(format)); // skip pixel data

			maxSize.Width = max(maxSize.Width, size.Width);
			maxSize.Height = max(maxSize.Height, size.Height);
			++frameCount;
		}
	}

	return maxSize;
}

HRESULT DesktopDuplication::WriteTransformOutputSamplesToSink(com_ptr<IMFTransform>& frameTransform,
	com_ptr<IMFSinkWriter>& sinkWriter, MFT_OUTPUT_DATA_BUFFER& mftOutputData) const
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
