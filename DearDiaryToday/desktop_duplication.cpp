#include "pch.h"
#include "desktop_duplication.h"

using namespace ATL;
using namespace std;

ErrorFunc errorFunc{};
HWND hWnd{};
int diaryIndex{};

static void DiaryProc();

void StartDiary(HWND _hWnd, ErrorFunc _errorFunc)
{
	errorFunc = _errorFunc;
	hWnd = _hWnd;

	thread(DiaryProc).detach();
}

static filesystem::path GetDiaryFilePath(int index)
{
	filesystem::path diaryPath = filesystem::current_path() / ".diary";
	filesystem::create_directory(diaryPath);

	auto diaryFile = diaryPath / ("diary_" + to_string(index) + ".dat");
	return diaryFile;
}

static void DiaryProc()
{
#define CHECK_PTR(ptr) do { if (!ptr) { errorFunc(S_FALSE); return; } } while (false)
#define CHECK_HR(hr) do { if (FAILED(hr)) { errorFunc(hr); return; } } while (false)

	auto currentDesktop = OpenInputDesktop(0, NULL, GENERIC_ALL);
	CHECK_PTR(currentDesktop);

	auto desktopAttached = SetThreadDesktop(currentDesktop) != 0;
	CloseDesktop(currentDesktop);
	if (!desktopAttached) {
		errorFunc(S_FALSE);
		return;
	}

	D3D_FEATURE_LEVEL featureLevels[] =
	{
		D3D_FEATURE_LEVEL_11_0,
		D3D_FEATURE_LEVEL_10_1,
		D3D_FEATURE_LEVEL_10_0,
		D3D_FEATURE_LEVEL_9_1
	};
	D3D_FEATURE_LEVEL featureLevel{};

	CComPtr<ID3D11Device> d3d11Device;
	CComPtr<ID3D11DeviceContext> immediateContext;
	CHECK_HR(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
		featureLevels, sizeof(featureLevels) / sizeof(*featureLevels), D3D11_SDK_VERSION, &d3d11Device,
		&featureLevel, &immediateContext));

	RECT rect{};
	GetWindowRect(hWnd, &rect);

	CComPtr<IDXGIOutput> dxgiOutput;
	DXGI_OUTPUT_DESC outputDesc;
	{
		CComPtr<IDXGIAdapter> dxgiAdapter;
		{
			CComQIPtr<IDXGIDevice> dxgiDevice(d3d11Device);
			CHECK_HR(dxgiDevice->GetAdapter(&dxgiAdapter));
		}

		for (int screenIndex = 0; ; ++screenIndex)
		{
			CComPtr<IDXGIOutput> dxgiOutputTemp;
			auto hr = dxgiAdapter->EnumOutputs(screenIndex, &dxgiOutputTemp);
			if (hr == DXGI_ERROR_NOT_FOUND)
				break;
			CHECK_HR(hr);

			dxgiOutputTemp->GetDesc(&outputDesc);
			if (outputDesc.DesktopCoordinates.left <= rect.left && outputDesc.DesktopCoordinates.right >= rect.left &&
				outputDesc.DesktopCoordinates.top <= rect.top && outputDesc.DesktopCoordinates.bottom >= rect.top)
			{
				dxgiOutput = dxgiOutputTemp;
				break;
			}
		}
	}

	CComPtr<IDXGIOutputDuplication> outputDuplication;
	CComQIPtr<IDXGIOutput5> dxgiOutput5(dxgiOutput);
	{
		dxgiOutput.Release();

		DXGI_FORMAT supportedFormats[] = { DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT };
		CHECK_HR(dxgiOutput5->DuplicateOutput1(d3d11Device, 0,
			sizeof(supportedFormats) / sizeof(*supportedFormats), supportedFormats,
			&outputDuplication));
	}

	int outputFileIndex{};
	ofstream outputFile(GetDiaryFilePath(outputFileIndex));
	int frameCount = 0;

	UINT metadataSize = 0;
	vector<RECT> dirtyRects;
	while (true)
	{
		{
			DXGI_OUTDUPL_FRAME_INFO frameInfo{};

			D3D11_TEXTURE2D_DESC desc{};
			CComPtr<ID3D11Texture2D> lastFrameTexture;
			{
				CComPtr<IDXGIResource> desktopResource;
				auto hr = outputDuplication->AcquireNextFrame(500, &frameInfo, &desktopResource);
				if (hr == DXGI_ERROR_WAIT_TIMEOUT)
					continue;
				CHECK_HR(hr);

				CHECK_HR(desktopResource.QueryInterface(&lastFrameTexture));
				lastFrameTexture->GetDesc(&desc);
			}

			assert(desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM
				|| desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT);

			CComPtr<ID3D11Texture2D> stagingTexture;

			if (frameInfo.TotalMetadataBufferSize > 0)
			{
				if (frameInfo.TotalMetadataBufferSize > metadataSize)
					dirtyRects.resize((metadataSize = frameInfo.TotalMetadataBufferSize) / sizeof(RECT));

				UINT dirtyRectSizeRequired = metadataSize;
				CHECK_HR(outputDuplication->GetFrameDirtyRects(metadataSize, dirtyRects.data(), &dirtyRectSizeRequired));
			}

			GetWindowRect(hWnd, &rect);

			// remove any dirty rects that are outside the window
			dirtyRects.erase(remove_if(dirtyRects.begin(), dirtyRects.end(), [&](const RECT& r) {
				return r.left >= rect.right || r.right <= rect.left || r.top >= rect.bottom || r.bottom <= rect.top;
				}), dirtyRects.end());

			if (dirtyRects.size() == 0 && frameCount)
			{
				// no dirty rects, we don't need a keyframe, just skip
				CHECK_HR(outputDuplication->ReleaseFrame());
				continue;
			}

			// copy the texture to a staging texture
			D3D11_MAPPED_SUBRESOURCE mappedResource;
			D3D11_TEXTURE2D_DESC stagingDesc = desc;
			stagingDesc.Usage = D3D11_USAGE_STAGING;
			stagingDesc.BindFlags = 0;
			stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			stagingDesc.MiscFlags = 0;

			CHECK_HR(d3d11Device->CreateTexture2D(&stagingDesc, nullptr, &stagingTexture));
			immediateContext->CopyResource(stagingTexture, lastFrameTexture);
			lastFrameTexture.Release();
			CHECK_HR(outputDuplication->ReleaseFrame());

			CHECK_HR(immediateContext->Map(stagingTexture, 0, D3D11_MAP_READ, 0, &mappedResource));

			bool isFullScreenDirty = dirtyRects.size() == 1 && dirtyRects[0].left == rect.left &&
				dirtyRects[0].top == rect.top && dirtyRects[0].right == rect.right && dirtyRects[0].bottom == rect.bottom;

			if (frameCount == 0 || isFullScreenDirty)
			{
				auto dim = rect.right - rect.left;
				outputFile.write((const char*)&dim, sizeof(dim));
				dim = rect.bottom - rect.top;
				outputFile.write((const char*)&dim, sizeof(dim));
				outputFile.write((const char*)&desc.Format, sizeof(desc.Format));
				size_t zeroCount = 0;
				outputFile.write((const char*)&zeroCount, sizeof(zeroCount));

				// dump the entire window
				for (auto y = rect.top; y < rect.bottom; ++y)
					outputFile.write(
						(const char*)(mappedResource.pData) + y * mappedResource.RowPitch + rect.left * 4,
						(rect.right - rect.left) * 4);
			}
			else if (dirtyRects.size() > 0)
			{
				auto dim = rect.right - rect.left;
				outputFile.write((const char*)&dim, sizeof(dim));
				dim = rect.bottom - rect.top;
				outputFile.write((const char*)&dim, sizeof(dim));
				outputFile.write((const char*)&desc.Format, sizeof(desc.Format));
				size_t dirtyRectCount = dirtyRects.size();
				outputFile.write((const char*)&dirtyRectCount, sizeof(dirtyRectCount));

				for (auto& dirtyRect : dirtyRects)
				{
					outputFile.write((const char*)&dirtyRect.left, sizeof(dirtyRect.left));
					outputFile.write((const char*)&dirtyRect.top, sizeof(dirtyRect.top));
					outputFile.write((const char*)&dirtyRect.right, sizeof(dirtyRect.right));
					outputFile.write((const char*)&dirtyRect.bottom, sizeof(dirtyRect.bottom));

					for (auto y = dirtyRect.top; y < dirtyRect.bottom; ++y)
						outputFile.write(
							(const char*)(mappedResource.pData) + y * mappedResource.RowPitch + dirtyRect.left * 4,
							(dirtyRect.right - dirtyRect.left) * 4);
				}
			}

			immediateContext->Unmap(stagingTexture, 0);
			++frameCount;
		}

	}
}