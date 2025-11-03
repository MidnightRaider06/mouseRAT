#include "OutboundQueue.h"

#include <algorithm>
#include <stdexcept>

namespace protocol {
bool OutboundQueue::enqueue(const Header& header, std::string_view payload) {
    if (header.payloadLength != payload.size()) {
        throw std::invalid_argument("Payload length does not match header");
    }

    const auto encoded = encodeHeader(header);
    if (encoded.size() + payload.size() > capacity - size_) return false;

    append({reinterpret_cast<const char*>(encoded.data()), encoded.size()});
    append(payload);
    return true;
}

void OutboundQueue::append(std::string_view bytes) noexcept {
    if (bytes.empty()) return;

    const auto tail = (head_ + size_) % capacity;
    const auto first = std::min(bytes.size(), capacity - tail);
    std::copy_n(bytes.data(), first, bytes_.data() + tail);
    if (first < bytes.size()) {
        std::copy_n(bytes.data() + first, bytes.size() - first, bytes_.data());
    }
    size_ += bytes.size();
}

std::string_view OutboundQueue::pending() const noexcept {
    return {bytes_.data() + head_, std::min(size_, capacity - head_)};
}

void OutboundQueue::advance(std::size_t sent) {
    if (sent > pending().size()) throw std::out_of_range("Send exceeds pending byte range");

    head_ = (head_ + sent) % capacity;
    size_ -= sent;
    if (empty()) head_ = 0;
}
}
