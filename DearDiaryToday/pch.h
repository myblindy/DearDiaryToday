#pragma once 

#define WIN32_LEAN_AND_MEAN     
#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <atlbase.h>

#include <d3d11.h>
#include <dxgi1_5.h>
#include <DirectXMath.h>
#pragma comment(lib, "D3D11.lib")

#include <vector>
#include <thread>
#include <filesystem>
#include <fstream>
#include "framework.h"
