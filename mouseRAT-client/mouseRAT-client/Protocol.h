#pragma once

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace protocol {

enum class Direction { FromServer, FromClient };

struct Header {
    std::uint8_t type;
    std::uint32_t payloadLength;
    std::uint32_t requestId;
};

bool isValidCommandText(std::string_view text);
bool isValidUtf8(std::string_view text);
void validateDirection(const Header& header, Direction direction);

inline void validateHeader(const Header& header) {
    struct Rule {
        std::uint8_t type;
        std::uint32_t minimum, maximum;
        bool control;
    };
    constexpr Rule rules[]{
        { 0x01, 0, 0, true },       // HELLO
        { 0x02, 0, 0, true },       // HELLO_ACK
        { 0x10, 1, 4096, false },   // COMMAND
        { 0x11, 2, 65536, false },  // OUTPUT
        { 0x12, 4, 4, false },      // COMPLETE
        { 0x20, 0, 0, false },      // SCREENSHOT
        { 0x21, 12, 12, false },    // IMAGE_BEGIN
        { 0x22, 1, 65536, false },  // IMAGE_DATA
        { 0x30, 8, 8, true },       // PING
        { 0x31, 8, 8, true },       // PONG
        { 0x7f, 4, 1028, false }    // ERROR
    };

    for (const auto& rule : rules) {
        if (header.type == rule.type) {
            if (header.payloadLength < rule.minimum || header.payloadLength > rule.maximum) {
                throw std::invalid_argument("Invalid payload length for message type");
            }

            if ((header.requestId == 0) != rule.control) {
                throw std::invalid_argument("Invalid request ID for message type");
            }

            return;
        }
    }

    throw std::invalid_argument("Unknown message type");
}

inline std::array<std::uint8_t, 16> encodeHeader(std::uint8_t type,
    std::uint32_t payloadLength, std::uint32_t requestId) {
    validateHeader({ type, payloadLength, requestId });

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

    validateHeader(header);

    return header;
}

}
