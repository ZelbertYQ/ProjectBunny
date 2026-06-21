#include "DX12Overlay.h"

#include "DX12State.h"

static int DrawOverlayTextBlock(HDC dc, const wchar_t *text, int x, int y)
{
	int lineHeight = 0;
	TEXTMETRICW metrics = {};
	if (GetTextMetricsW(dc, &metrics))
		lineHeight = metrics.tmHeight + 3;
	if (!lineHeight)
		lineHeight = 18;

	int maxWidth = 0;
	int lines = 0;
	const wchar_t *lineStart = text;
	for (const wchar_t *p = text; ; ++p) {
		if (*p == L'\n' || *p == L'\0') {
			int len = static_cast<int>(p - lineStart);
			SIZE size = {};
			GetTextExtentPoint32W(dc, lineStart, len, &size);
			if (size.cx > maxWidth)
				maxWidth = size.cx;
			lines++;
			if (*p == L'\0')
				break;
			lineStart = p + 1;
		}
	}

	RECT background = {
		x - 4,
		y - 4,
		x + maxWidth + 8,
		y + lines * lineHeight + 4
	};
	FillRect(dc, &background, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

	int drawY = y;
	lineStart = text;
	for (const wchar_t *p = text; ; ++p) {
		if (*p == L'\n' || *p == L'\0') {
			int len = static_cast<int>(p - lineStart);
			TextOutW(dc, x, drawY, lineStart, len);
			drawY += lineHeight;
			if (*p == L'\0')
				break;
			lineStart = p + 1;
		}
	}
	return background.bottom;
}

static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
	switch (msg) {
	case WM_NCHITTEST:
		return HTTRANSPARENT;
	case WM_TIMER:
		InvalidateRect(hwnd, nullptr, TRUE);
		return 0;
	case WM_PAINT:
	{
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(hwnd, &ps);
		RECT client;
		GetClientRect(hwnd, &client);
		HBRUSH transparentBrush = CreateSolidBrush(RGB(1, 1, 1));
		FillRect(dc, &client, transparentBrush);
		DeleteObject(transparentBrush);

		HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
		HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;

		wchar_t text[512];
		DX12GetOverlayStatus(text, ARRAYSIZE(text));

		SetBkMode(dc, TRANSPARENT);
		SetTextColor(dc, RGB(0, 255, 0));
		DrawOverlayTextBlock(dc, text, 16, 12);

		if (oldFont)
			SelectObject(dc, oldFont);
		EndPaint(hwnd, &ps);
		return 0;
	}
	case WM_CLOSE:
		DestroyWindow(hwnd);
		return 0;
	case WM_DESTROY:
		DX12SetOverlayWindow(nullptr);
		return 0;
	default:
		return DefWindowProcW(hwnd, msg, wparam, lparam);
	}
}

DWORD WINAPI DX12OverlayThread(void*)
{
	const wchar_t className[] = L"3DMigotoDX12LightweightOverlay";

	WNDCLASSW wc = {};
	wc.lpfnWndProc = OverlayWndProc;
	wc.hInstance = DX12GetModule();
	wc.lpszClassName = className;
	wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	RegisterClassW(&wc);

	int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
	int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
	int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
	int height = 96;

	HWND hwnd = CreateWindowExW(
		WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
		className,
		L"3DMigoto DX12 Lightweight Overlay",
		WS_POPUP,
		x, y, width, height,
		nullptr, nullptr, DX12GetModule(), nullptr);

	if (!hwnd) {
		DX12LogJsonFunc("OverlayWindowCreate",
			"\"status\":\"failed\",\"error\":%lu", GetLastError());
		return 0;
	}

	DX12SetOverlayWindow(hwnd);
	SetLayeredWindowAttributes(hwnd, RGB(1, 1, 1), 220, LWA_COLORKEY | LWA_ALPHA);
	ShowWindow(hwnd, SW_SHOWNOACTIVATE);
	SetWindowPos(hwnd, HWND_TOPMOST, x, y, width, height,
		SWP_NOACTIVATE | SWP_SHOWWINDOW);
	SetTimer(hwnd, 1, 250, nullptr);

	DX12LogJsonFunc("OverlayWindowCreate",
		"\"status\":\"ok\",\"hwnd\":\"%p\"", hwnd);

	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}

	return 0;
}

void DX12DrawSwapChainText(IDXGISwapChain *swapChain)
{
	if (!swapChain)
		return;

	DXGI_SWAP_CHAIN_DESC desc = {};
	if (FAILED(swapChain->GetDesc(&desc)) || !desc.OutputWindow || !IsWindow(desc.OutputWindow))
		return;

	HDC dc = GetDC(desc.OutputWindow);
	if (!dc)
		return;

	int oldBkMode = SetBkMode(dc, TRANSPARENT);
	COLORREF oldTextColor = SetTextColor(dc, RGB(0, 255, 0));
	HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
	HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
	wchar_t text[512];
	DX12GetOverlayStatus(text, ARRAYSIZE(text));
	DrawOverlayTextBlock(dc, text, 20, 20);

	if (oldFont)
		SelectObject(dc, oldFont);
	SetTextColor(dc, oldTextColor);
	SetBkMode(dc, oldBkMode);
	ReleaseDC(desc.OutputWindow, dc);
}

void DX12CloseOverlayWindow()
{
	HWND hwnd = DX12GetOverlayWindow();
	if (hwnd)
		PostMessageW(hwnd, WM_CLOSE, 0, 0);
}
