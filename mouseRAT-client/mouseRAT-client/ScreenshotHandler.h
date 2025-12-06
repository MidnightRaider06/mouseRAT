#pragma once

#include <cstddef>
#include <vector>

class ScreenshotHandler {
public:
    static constexpr std::size_t maxImageBytes = 32 * 1024 * 1024;

    // Returns a complete binary BMP file
    [[nodiscard]] static std::vector<char> captureBmp();
};

