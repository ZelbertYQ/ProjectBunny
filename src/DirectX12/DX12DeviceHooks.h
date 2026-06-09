#pragma once

#include <Windows.h>

void DX12HookDevice(IUnknown *device);
void DX12HookDeviceFactory(IUnknown *factory);
void DX12HookDeviceFromCommandQueue(IUnknown *commandQueue);
