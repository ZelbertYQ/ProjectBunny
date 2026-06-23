#include "DX12Input.h"

#include <Windows.h>

#include "DX12FrameAnalysis.h"
#include "DX12ModRuntime.h"
#include "DX12ShaderDump.h"
#include "DX12ShaderHunt.h"
#include "DX12State.h"

static bool gF8WasDown = false;
static bool gF9WasDown = false;
static bool gF10WasDown = false;
static bool gNumpad0WasDown = false;
static bool gNumpadAddWasDown = false;
static bool gNumpad9WasDown = false;

struct RepeatKeyState
{
	bool down = false;
	DWORD firstDownTick = 0;
	DWORD lastRepeatTick = 0;
};

static RepeatKeyState gRepeatNumpad1;
static RepeatKeyState gRepeatNumpad2;
static RepeatKeyState gRepeatNumpad4;
static RepeatKeyState gRepeatNumpad5;
static RepeatKeyState gRepeatNumpad7;
static RepeatKeyState gRepeatNumpad8;

static void ResetHuntRepeatKeys()
{
	gRepeatNumpad1 = RepeatKeyState();
	gRepeatNumpad2 = RepeatKeyState();
	gRepeatNumpad4 = RepeatKeyState();
	gRepeatNumpad5 = RepeatKeyState();
	gRepeatNumpad7 = RepeatKeyState();
	gRepeatNumpad8 = RepeatKeyState();
}

static bool KeyPressed(int vk, bool *wasDown)
{
	const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
	const bool pressed = down && !*wasDown;
	*wasDown = down;
	return pressed;
}

static bool KeyPressedOrRepeated(int vk, RepeatKeyState *state)
{
	if (!state)
		return false;

	const DWORD now = GetTickCount();
	const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
	if (!down) {
		*state = RepeatKeyState();
		return false;
	}

	if (!state->down) {
		state->down = true;
		state->firstDownTick = now;
		state->lastRepeatTick = now;
		return true;
	}

	const DWORD initialDelayMs = 350;
	const DWORD repeatMs = 85;
	if (now - state->firstDownTick >= initialDelayMs &&
	    now - state->lastRepeatTick >= repeatMs) {
		state->lastRepeatTick = now;
		return true;
	}

	return false;
}

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

	const bool f10Down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
	if (f10Down && !gF10WasDown) {
		DX12LogJsonFunc("InputHotkey", "\"key\":\"F10\",\"action\":\"ModRuntimeReload\"");
		DX12ModRuntimeReload();
	}
	gF10WasDown = f10Down;

	const bool decimalDown = (GetAsyncKeyState(VK_DECIMAL) & 0x8000) != 0;
	if (!decimalDown && KeyPressed(VK_NUMPAD0, &gNumpad0WasDown)) {
		DX12LogJsonFunc("InputHotkey", "\"key\":\"Numpad0\",\"action\":\"HuntToggle\"");
		DX12HuntToggle();
	}

	if (!DX12HuntIsEnabled()) {
		ResetHuntRepeatKeys();
		return;
	}

	if (!decimalDown && KeyPressed(VK_ADD, &gNumpadAddWasDown)) {
		DX12LogJsonFunc("InputHotkey", "\"key\":\"NumpadAdd\",\"action\":\"HuntResetSelection\"");
		DX12HuntResetSelection();
	}
	if (!decimalDown && KeyPressed(VK_NUMPAD9, &gNumpad9WasDown)) {
		DX12LogJsonFunc("InputHotkey", "\"key\":\"Numpad9\",\"action\":\"HuntCopySelectedHash\"");
		DX12HuntCopySelectedHash();
	}
	if (KeyPressedOrRepeated(VK_NUMPAD1, &gRepeatNumpad1)) {
		if (decimalDown)
			DX12HuntPreviousCS();
		else
			DX12HuntPreviousPS();
	}
	if (KeyPressedOrRepeated(VK_NUMPAD2, &gRepeatNumpad2)) {
		if (decimalDown)
			DX12HuntNextCS();
		else
			DX12HuntNextPS();
	}
	if (!decimalDown && KeyPressedOrRepeated(VK_NUMPAD4, &gRepeatNumpad4))
		DX12HuntPreviousVS();
	if (!decimalDown && KeyPressedOrRepeated(VK_NUMPAD5, &gRepeatNumpad5))
		DX12HuntNextVS();
	if (KeyPressedOrRepeated(VK_NUMPAD7, &gRepeatNumpad7)) {
		if (decimalDown)
			DX12HuntPreviousVB();
		else
			DX12HuntPreviousIB();
	}
	if (KeyPressedOrRepeated(VK_NUMPAD8, &gRepeatNumpad8)) {
		if (decimalDown)
			DX12HuntNextVB();
		else
			DX12HuntNextIB();
	}
}
