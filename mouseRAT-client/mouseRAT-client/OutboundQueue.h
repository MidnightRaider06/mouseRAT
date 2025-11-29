#pragma once

#include "Protocol.h"

#include <array>
#include <cstddef>
#include <string_view>

namespace protocol {

// Owned by one network thread
class OutboundQueue {
public:
    static constexpr std::size_t capacity = 128 * 1024;

    [[nodiscard]] bool enqueue(const Header& header, std::string_view payload);
    // Borrows queue storage until the next mutation
    [[nodiscard]] std::string_view pending() const;
    void advance(std::size_t sent);

    [[nodiscard]] std::size_t size() const {
        return size_;
    }

    [[nodiscard]] bool empty() const {
        return size_ == 0;
    }

private:
    void append(std::string_view bytes);

    std::array<char, capacity> bytes_{};
    std::size_t head_ = 0;
    std::size_t size_ = 0;
};

}
