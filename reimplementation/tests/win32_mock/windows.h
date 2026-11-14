#pragma once

// Test-only Win32 surface. The production window source is compiled unchanged.
#include <cstdint>

#define CALLBACK
using BOOL = int;
using UINT = unsigned;
using DWORD = std::uint32_t;
using LONG = std::int32_t;
using LONG_PTR = std::intptr_t;
using WPARAM = std::uintptr_t;
using LPARAM = std::intptr_t;
using LRESULT = std::intptr_t;
using HWND = void*;
using HINSTANCE = void*;
using HCURSOR = void*;
using HBRUSH = void*;
using HGDIOBJ = void*;
using HDC = void*;
using HBITMAP = void*;
using WNDPROC = LRESULT (*)(HWND, UINT, WPARAM, LPARAM);

struct RECT { LONG left, top, right, bottom; };
struct POINT { LONG x,y; };
constexpr int ERROR_SUCCESS = 0;
constexpr int VK_LBUTTON = 1, VK_RBUTTON = 2, VK_MBUTTON = 4;
std::uint64_t GetTickCount64();
BOOL GetCursorPos(POINT*);
BOOL ClientToScreen(HWND,POINT*);
BOOL SetCursorPos(int,int);
short GetAsyncKeyState(int);
struct CREATESTRUCTW { void* lpCreateParams; };
struct PAINTSTRUCT {};
struct MSG { HWND hwnd; UINT message; WPARAM wParam; LPARAM lParam; };
struct WNDCLASSW {
    HINSTANCE hInstance;
    WNDPROC lpfnWndProc;
    const wchar_t* lpszClassName;
    HCURSOR hCursor;
};
struct BITMAPINFOHEADER {
    DWORD biSize;
    LONG biWidth, biHeight;
    std::uint16_t biPlanes, biBitCount;
    DWORD biCompression;
};
struct BITMAPINFO { BITMAPINFOHEADER bmiHeader; };

constexpr BOOL FALSE = 0;
constexpr BOOL TRUE = 1;
constexpr UINT WM_NCCREATE = 0x0081;
constexpr UINT WM_KEYDOWN = 0x0100;
constexpr UINT WM_KEYUP = 0x0101;
constexpr UINT WM_SYSKEYDOWN = 0x0104;
constexpr UINT WM_SYSKEYUP = 0x0105;
constexpr UINT WM_KILLFOCUS = 0x0008;
constexpr UINT WM_PAINT = 0x000f;
constexpr UINT WM_ERASEBKGND = 0x0014;
constexpr UINT WM_DESTROY = 0x0002;
constexpr UINT WM_QUIT = 0x0012;
constexpr WPARAM VK_ESCAPE = 0x1b;
constexpr WPARAM VK_PAUSE = 0x13;
constexpr WPARAM VK_CONTROL = 0x11;
constexpr WPARAM VK_LEFT = 0x25;
constexpr WPARAM VK_UP = 0x26;
constexpr WPARAM VK_RIGHT = 0x27;
constexpr WPARAM VK_DOWN = 0x28;
constexpr WPARAM VK_OEM_COMMA = 0xbc;
constexpr WPARAM VK_OEM_PERIOD = 0xbe;
constexpr int GWLP_USERDATA = -21;
constexpr DWORD WS_OVERLAPPEDWINDOW = 0x00cf0000;
constexpr int CW_USEDEFAULT = static_cast<int>(0x80000000U);
constexpr DWORD BI_RGB = 0;
constexpr int SW_SHOW = 5;
constexpr UINT PM_REMOVE = 1;
constexpr int BLACK_BRUSH = 4;
constexpr int COLORONCOLOR = 3;
constexpr UINT DIB_RGB_COLORS = 0;
constexpr DWORD SRCCOPY = 0x00cc0020;
constexpr const char* IDC_ARROW = "arrow";

HINSTANCE GetModuleHandleW(const wchar_t*);
HCURSOR LoadCursor(HINSTANCE, const char*);
unsigned short RegisterClassW(const WNDCLASSW*);
BOOL AdjustWindowRect(RECT*, DWORD, BOOL);
HWND CreateWindowW(const wchar_t*, const wchar_t*, DWORD, int, int, int, int, HWND, void*, HINSTANCE, void*);
LONG_PTR SetWindowLongPtrW(HWND, int, LONG_PTR);
LONG_PTR GetWindowLongPtrW(HWND, int);
LRESULT DefWindowProcW(HWND, UINT, WPARAM, LPARAM);
BOOL ShowWindow(HWND, int);
BOOL IsWindow(HWND);
BOOL DestroyWindow(HWND);
BOOL PeekMessageW(MSG*, HWND, UINT, UINT, UINT);
BOOL TranslateMessage(const MSG*);
LRESULT DispatchMessageW(const MSG*);
HWND GetForegroundWindow();
BOOL InvalidateRect(HWND, const RECT*, BOOL);
BOOL UpdateWindow(HWND);
HDC BeginPaint(HWND, PAINTSTRUCT*);
BOOL EndPaint(HWND, const PAINTSTRUCT*);
BOOL GetClientRect(HWND, RECT*);
HGDIOBJ GetStockObject(int);
int FillRect(HDC, const RECT*, HBRUSH);
HDC CreateCompatibleDC(HDC);
HBITMAP CreateCompatibleBitmap(HDC, int, int);
HGDIOBJ SelectObject(HDC,HGDIOBJ);
BOOL DeleteObject(HGDIOBJ);
BOOL DeleteDC(HDC);
BOOL BitBlt(HDC,int,int,int,int,HDC,int,int,DWORD);
int SetStretchBltMode(HDC, int);
int StretchDIBits(HDC, int, int, int, int, int, int, int, int, const void*, const BITMAPINFO*, UINT, DWORD);
void PostQuitMessage(int);
