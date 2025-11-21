#pragma once

#include <array>
#include <cstdint>
#include <stdexcept>

namespace protocol {

struct Header {
    std::uint8_t type;
    std::uint32_t payloadLength;
    std::uint32_t requestId;
};

inline std::array<std::uint8_t, 16> encodeHeader(std::uint8_t type,
    std::uint32_t payloadLength, std::uint32_t requestId) {
    if (payloadLength > 65536) {
        throw std::invalid_argument("Payload exceeds the v1 frame limit");
    }

    std::array<std::uint8_t, 16> header{ 'M', 'R', 'A', 'T', 1, type, 0, 0 };
    for (unsigned index = 0; index < 4; ++index) {
        const auto shift = 24 - index * 8;
        header[8 + index] = static_cast<std::uint8_t>(payloadLength >> shift);
        header[12 + index] = static_cast<std::uint8_t>(requestId >> shift);
    }

    return header;
}

inline Header decodeHeader(const std::array<std::uint8_t, 16>& bytes) {
    if (bytes[0] != 'M' || bytes[1] != 'R' || bytes[2] != 'A' || bytes[3] != 'T') {
        throw std::invalid_argument("Invalid frame magic");
    }

    if (bytes[4] != 1 || bytes[6] != 0 || bytes[7] != 0) {
        throw std::invalid_argument("Unsupported frame version or flags");
    }

    Header header{ bytes[5], 0, 0 };
    for (unsigned index = 0; index < 4; ++index) {
        header.payloadLength = (header.payloadLength << 8) | bytes[8 + index];
        header.requestId = (header.requestId << 8) | bytes[12 + index];
    }

    if (header.payloadLength > 65536) {
        throw std::invalid_argument("Payload exceeds the v1 frame limit");
    }

    return header;
}

}
