#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "window.hpp"

#include <algorithm>
#include <stdexcept>

namespace swiv3d::host {

class Window::Impl {
public:
    Impl() {
        const auto instance = GetModuleHandleW(nullptr);
        WNDCLASSW type{};
        type.hInstance = instance;
        type.lpfnWndProc = procedure;
        type.lpszClassName = L"Swiv3dNativeFlight";
        type.hCursor = LoadCursor(nullptr, IDC_ARROW);
        if (!RegisterClassW(&type)) {
            throw std::runtime_error("Cannot register native flight window");
        }
        RECT rectangle{0, 0, 960, 720};
        AdjustWindowRect(&rectangle, WS_OVERLAPPEDWINDOW, FALSE);
        handle_ = CreateWindowW(type.lpszClassName,
            L"OpenSWIV3D | libsmacker (c) 2012-2021 Greg Kennedy, LGPL 2.1+ | see NOTICE.md",
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, rectangle.right - rectangle.left,
            rectangle.bottom - rectangle.top, nullptr, nullptr, instance, this);
        if (!handle_) {
            throw std::runtime_error("Cannot create native flight window");
        }
        info_.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info_.bmiHeader.biWidth = static_cast<LONG>(image_width);
        info_.bmiHeader.biHeight = -static_cast<LONG>(image_height);
        info_.bmiHeader.biPlanes = 1;
        info_.bmiHeader.biBitCount = 32;
        info_.bmiHeader.biCompression = BI_RGB;
        ShowWindow(handle_, SW_SHOW);
    }

    ~Impl() {
        if (IsWindow(handle_)) {
            DestroyWindow(handle_);
        }
        release_surface();
    }

    bool poll() {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                return false;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return IsWindow(handle_) != FALSE;
    }

    bool focused() const { return GetForegroundWindow() == handle_; }
    KeyboardState keys() const { return keys_; }
    void escape_action(EscapeAction action) { escape_action_ = action; }

    void present(const MissionFrame& frame) {
        pixels_.resize(frame.pixels.size());
        const auto expand = [](std::uint32_t value) { return (value << 2) | (value >> 4); };
        for (std::size_t i = 0; i < frame.pixels.size(); ++i) {
            const auto index = frame.pixels[i] * 3;
            pixels_[i] = (expand(frame.palette[index]) << 16) | (expand(frame.palette[index + 1]) << 8) |
                         expand(frame.palette[index + 2]);
        }
        InvalidateRect(handle_, nullptr, FALSE);
        UpdateWindow(handle_);
    }

private:
    void release_surface() {
        if (!surface_) { return; }
        SelectObject(surface_, previous_bitmap_);
        DeleteObject(surface_bitmap_);
        DeleteDC(surface_);
        surface_ = nullptr;
        surface_bitmap_ = nullptr;
        previous_bitmap_ = nullptr;
    }

    bool resize_surface(HDC dc, int width, int height) {
        if (surface_ && surface_width_ == width && surface_height_ == height) { return true; }
        const auto surface = CreateCompatibleDC(dc);
        if (!surface) { return false; }
        const auto bitmap = CreateCompatibleBitmap(dc,width,height);
        if (!bitmap) { DeleteDC(surface); return false; }
        const auto previous = SelectObject(surface,bitmap);
        if (!previous) { DeleteObject(bitmap); DeleteDC(surface); return false; }
        release_surface();
        surface_ = surface; surface_bitmap_ = bitmap; previous_bitmap_ = previous;
        surface_width_ = width; surface_height_ = height;
        return true;
    }

    static LRESULT CALLBACK procedure(HWND handle, UINT message, WPARAM word, LPARAM value) {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(handle, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(value)->lpCreateParams);
            SetWindowLongPtrW(handle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) {
            return DefWindowProcW(handle, message, word, value);
        }
        switch (message) {
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP: {
            const auto scan = word == VK_PAUSE ? pause_scan :
                static_cast<unsigned>((value >> 16) & 0x7f) | ((value & (1L << 24)) != 0 ? 0x80U : 0U);
            self->keys_[scan] = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
            if (word == VK_ESCAPE && self->keys_[scan] && self->escape_action_ == EscapeAction::quit) {
                DestroyWindow(handle);
            }
            return 0;
        }
        case WM_KILLFOCUS:
            self->keys_.fill(false);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            const auto dc = BeginPaint(handle, &paint);
            RECT area{};
            GetClientRect(handle, &area);
            if (area.right <= 0 || area.bottom <= 0 || !self->resize_surface(dc,area.right,area.bottom)) {
                EndPaint(handle,&paint);
                return 0;
            }
            const auto surface = self->surface_;
            FillRect(surface, &area, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            const int width = std::min(area.right, area.bottom * 4 / 3);
            const int height = width * 3 / 4;
            if (!self->pixels_.empty()) {
                SetStretchBltMode(surface, COLORONCOLOR);
                StretchDIBits(surface, (area.right - width) / 2, (area.bottom - height) / 2, width, height,
                              0, 0, static_cast<int>(image_width), static_cast<int>(image_height),
                              self->pixels_.data(), &self->info_, DIB_RGB_COLORS, SRCCOPY);
            }
            BitBlt(dc,0,0,area.right,area.bottom,surface,0,0,SRCCOPY);
            EndPaint(handle, &paint);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(handle, message, word, value);
        }
    }

    HWND handle_ = nullptr;
    HDC surface_ = nullptr;
    HBITMAP surface_bitmap_ = nullptr;
    HGDIOBJ previous_bitmap_ = nullptr;
    int surface_width_ = 0, surface_height_ = 0;
    BITMAPINFO info_{};
    KeyboardState keys_{};
    EscapeAction escape_action_ = EscapeAction::quit;
    std::vector<std::uint32_t> pixels_;
};

Window::Window() : impl_(std::make_unique<Impl>()) {}
Window::~Window() = default;
bool Window::poll() { return impl_->poll(); }
bool Window::focused() const { return impl_->focused(); }
KeyboardState Window::keys() const { return impl_->keys(); }
void Window::present(const MissionFrame& frame) { impl_->present(frame); }
void Window::escape_action(EscapeAction action) { impl_->escape_action(action); }

}
