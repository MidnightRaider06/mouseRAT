#pragma once

#include "Protocol.h"

#include <string_view>

namespace protocol {
class OutboundQueue {
public:
    static constexpr std::size_t capacity = 128 * 1024;

    [[nodiscard]] bool enqueue(const Header& header, std::string_view payload);
    [[nodiscard]] std::string_view pending() const noexcept;
    void advance(std::size_t sent);

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

private:
    void append(std::string_view bytes) noexcept;

    std::array<char, capacity> bytes_{};
    std::size_t head_ = 0;
    std::size_t size_ = 0;
};
}
