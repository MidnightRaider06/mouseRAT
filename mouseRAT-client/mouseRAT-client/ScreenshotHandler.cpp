#include "ScreenshotHandler.h"

#include <Windows.h>

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <system_error>

namespace {

struct CaptureResources {
    DPI_AWARENESS_CONTEXT previousDpi = nullptr;
    HDC screen = nullptr, memory = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previousBitmap = nullptr;

    ~CaptureResources() {
        if (previousBitmap) {
            SelectObject(memory, previousBitmap);
        }

        if (bitmap) {
            DeleteObject(bitmap);
        }

        if (memory) {
            DeleteDC(memory);
        }

        if (screen) {
            ReleaseDC(nullptr, screen);
        }

        if (previousDpi) {
            SetThreadDpiAwarenessContext(previousDpi);
        }
    }
};

}

std::vector<char> ScreenshotHandler::captureBmp() {
    CaptureResources capture;
    capture.previousDpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);
    if (!capture.previousDpi) {
        throw std::runtime_error("Cannot establish screenshot DPI context");
    }

    const auto x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const auto y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const auto width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const auto height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (width <= 0 || height <= 0) {
        throw std::runtime_error("No virtual desktop is available");
    }

    constexpr auto headerBytes = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    const auto stride = (std::uint64_t(width) * 3 + 3) & ~std::uint64_t(3);
    const auto pixelBytes = stride * std::uint64_t(height);
    if (pixelBytes > maxImageBytes - headerBytes) {
        throw std::length_error("Screenshot exceeds the 32 MiB limit");
    }

    capture.screen = GetDC(nullptr);
    if (!capture.screen) {
        throw std::runtime_error("Cannot acquire the desktop DC");
    }

    capture.memory = CreateCompatibleDC(capture.screen);
    if (!capture.memory) {
        throw std::runtime_error("Cannot create the capture DC");
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 24;
    info.bmiHeader.biCompression = BI_RGB;
    info.bmiHeader.biSizeImage = static_cast<DWORD>(pixelBytes);
    void* pixels = nullptr;
    capture.bitmap = CreateDIBSection(capture.screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!capture.bitmap || !pixels) {
        throw std::runtime_error("Cannot allocate the capture bitmap");
    }

    const auto previous = SelectObject(capture.memory, capture.bitmap);
    if (!previous || previous == HGDI_ERROR) {
        throw std::runtime_error("Cannot select the capture bitmap");
    }

    capture.previousBitmap = previous;
    std::memset(pixels, 0, static_cast<std::size_t>(pixelBytes));
    if (!BitBlt(capture.memory, 0, 0, width, height, capture.screen, x, y, SRCCOPY | CAPTUREBLT)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "BitBlt");
    }

    if (!GdiFlush()) {
        throw std::runtime_error("Cannot flush captured pixels");
    }

    BITMAPFILEHEADER file{};
    file.bfType = 0x4d42;
    file.bfSize = static_cast<DWORD>(headerBytes + pixelBytes);
    file.bfOffBits = static_cast<DWORD>(headerBytes);
    std::vector<char> bytes(file.bfSize);
    std::memcpy(bytes.data(), &file, sizeof(file));
    std::memcpy(bytes.data() + sizeof(file), &info.bmiHeader, sizeof(info.bmiHeader));
    std::memcpy(bytes.data() + headerBytes, pixels, static_cast<std::size_t>(pixelBytes));
    return bytes;
}
