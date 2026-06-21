#include "DX12Input.h"

#include <Windows.h>

#include "DX12FrameAnalysis.h"
#include "DX12ShaderDump.h"
#include "DX12State.h"

static bool gF8WasDown = false;
static bool gF9WasDown = false;

void DX12PollInput()
{
	const bool f8Down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
	if (f8Down && !gF8WasDown) {
		DX12LogJsonFunc("InputHotkey", "\"key\":\"F8\",\"action\":\"FrameAnalysisRequest\"");
		if (DX12FrameAnalysisBegin()) {
			DX12FrameAnalysisLogJsonFunc("FrameAnalysisArmed", nullptr);
			DX12FrameAnalysisRequestCapture();
			DX12SetOverlayStatus(L"F8 frame analysis armed");
		} else {
			DX12LogJsonFunc("FrameAnalysisBeginFailed", "\"reason\":\"create_directory_failed\"");
			DX12SetOverlayStatus(L"F8 dump failed: cannot create directory");
		}
	}
	gF8WasDown = f8Down;

	const bool f9Down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
	if (f9Down && !gF9WasDown) {
		DX12LogJsonFunc("InputHotkey", "\"key\":\"F9\",\"action\":\"ShaderDumpRequest\"");
		DX12RequestShaderDump();
	}
	gF9WasDown = f9Down;
}
