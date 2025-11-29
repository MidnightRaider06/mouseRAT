#include "OutboundQueue.h"

#include <algorithm>

namespace protocol {
namespace {

std::uint32_t read32(std::string_view bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        value = (value << 8) | static_cast<unsigned char>(bytes[offset + index]);
    }

    return value;
}

}

bool OutboundQueue::enqueue(const Header& header, std::string_view payload) {
    if (header.payloadLength != payload.size()) {
        throw std::invalid_argument("Payload length does not match header");
    }

    const auto encoded = encodeHeader(header.type, header.payloadLength, header.requestId);
    validateDirection(header, Direction::FromClient);
    if (header.type == 0x11 && static_cast<unsigned char>(payload[0]) > 2) {
        throw std::invalid_argument("Invalid OUTPUT stream tag");
    }

    if (header.type == 0x7f && (read32(payload, 0) < 1 || read32(payload, 0) > 4
        || !isValidUtf8(payload.substr(4)))) {
        throw std::invalid_argument("Invalid ERROR code or diagnostic text");
    }

    if (header.type == 0x21) {
        const auto total = (std::uint64_t(read32(payload, 0)) << 32) | read32(payload, 4);
        const auto format = read32(payload, 8);
        if (total == 0 || total > 32 * 1024 * 1024 || (format != 1 && format != 2)) {
            throw std::invalid_argument("Invalid IMAGE_BEGIN metadata");
        }
    }

    if (encoded.size() + payload.size() > capacity - size_) {
        return false;
    }

    append({ reinterpret_cast<const char*>(encoded.data()), encoded.size() });
    append(payload);
    return true;
}

void OutboundQueue::append(std::string_view bytes) {
    if (bytes.empty()) {
        return;
    }

    const auto tail = (head_ + size_) % capacity;
    const auto first = (std::min)(bytes.size(), capacity - tail);
    std::copy_n(bytes.data(), first, bytes_.data() + tail);
    if (first < bytes.size()) {
        std::copy_n(bytes.data() + first, bytes.size() - first, bytes_.data());
    }

    size_ += bytes.size();
}

std::string_view OutboundQueue::pending() const {
    return { bytes_.data() + head_, (std::min)(size_, capacity - head_) };
}

void OutboundQueue::advance(std::size_t sent) {
    if (sent > pending().size()) {
        throw std::out_of_range("Send exceeds pending byte range");
    }

    head_ = (head_ + sent) % capacity;
    size_ -= sent;
    if (empty()) {
        head_ = 0;
    }
}

}
