#pragma once

#include "DiskWorker.h"
#include "Protocol.h"

#include <filesystem>
#include <map>
#include <string>
#include <utility>

class ScreenshotStore {
public:
    static constexpr std::size_t maxTransfers = 64;

    explicit ScreenshotStore(std::filesystem::path directory) : directory_(std::move(directory)) {}
    ScreenshotStore(const ScreenshotStore&) = delete;
    ScreenshotStore& operator=(const ScreenshotStore&) = delete;

    [[nodiscard]] std::error_code operator()(const DiskJob& job);
    [[nodiscard]] std::filesystem::path destination(std::uint64_t id, protocol::ImageFormat format) const;

private:
    struct Transfer {
        UniqueFd file;
        std::string temporary;
        std::string finalPath;
        std::uint64_t expected = 0;
        std::uint64_t received = 0;
        protocol::ImageFormat format{};
        std::array<char, 8> prefix{};
        std::size_t prefixSize = 0;
        std::error_code failure{};
        bool temporaryExists = false;

        ~Transfer();
    };

    std::error_code begin(const DiskJob& job);
    std::error_code fail(std::uint64_t id, std::error_code error) noexcept;
    static std::error_code removeTemporary(Transfer& transfer) noexcept;

    std::filesystem::path directory_;
    std::map<std::uint64_t, Transfer> transfers_;
    std::uint64_t lastBegin_ = 0;
};
