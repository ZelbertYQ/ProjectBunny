#pragma once

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

void DX12Initialize(HINSTANCE module);
void DX12Shutdown();
void DX12InstallDXGIHooks();

