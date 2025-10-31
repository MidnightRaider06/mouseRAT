#include "Protocol.h"

#include <optional>
#include <stdexcept>

namespace protocol {
namespace {
void write32(HeaderBytes& bytes, std::size_t offset, std::uint32_t value) noexcept {
    for (std::size_t i = 0; i < 4; ++i) {
        bytes[offset + i] = static_cast<std::uint8_t>(value >> (24 - 8 * i));
    }
}

std::uint32_t read32(const HeaderBytes& bytes, std::size_t offset) noexcept {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value = (value << 8) | bytes[offset + i];
    }
    return value;
}

std::optional<HeaderError> validate(const Header& header) noexcept {
    std::uint32_t minimum = 0;
    std::uint32_t maximum = 0;
    bool control = false;

    switch (header.type) {
    case MessageType::Hello:
    case MessageType::HelloAck: control = true; break;
    case MessageType::Command: minimum = 1; maximum = 4096; break;
    case MessageType::Output: minimum = 2; maximum = maxPayloadSize; break;
    case MessageType::Complete: minimum = maximum = 4; break;
    case MessageType::Screenshot: break;
    case MessageType::ImageBegin: minimum = maximum = 12; break;
    case MessageType::ImageData: minimum = 1; maximum = maxPayloadSize; break;
    case MessageType::Ping:
    case MessageType::Pong: minimum = maximum = 8; control = true; break;
    case MessageType::Error: minimum = 4; maximum = 1028; break;
    default: return HeaderError::UnknownType;
    }

    if (header.payloadLength > maxPayloadSize) return HeaderError::PayloadTooLarge;
    if (header.payloadLength < minimum || header.payloadLength > maximum) {
        return HeaderError::InvalidPayloadLength;
    }
    if ((header.requestId == 0) != control) return HeaderError::InvalidRequestId;

    return std::nullopt;
}
}

HeaderBytes encodeHeader(const Header& header) {
    if (validate(header)) throw std::invalid_argument("Invalid outgoing protocol header");

    HeaderBytes bytes{'M', 'R', 'A', 'T', version, static_cast<std::uint8_t>(header.type), 0, 0};
    write32(bytes, 8, header.payloadLength);
    write32(bytes, 12, header.requestId);
    return bytes;
}

DecodeResult decodeHeader(const HeaderBytes& bytes) noexcept {
    if (bytes[0] != 'M' || bytes[1] != 'R' || bytes[2] != 'A' || bytes[3] != 'T') {
        return HeaderError::InvalidMagic;
    }
    if (bytes[4] != version) return HeaderError::UnsupportedVersion;
    if (bytes[6] != 0 || bytes[7] != 0) return HeaderError::UnsupportedFlags;

    const Header header{static_cast<MessageType>(bytes[5]), read32(bytes, 8), read32(bytes, 12)};
    if (const auto error = validate(header)) return *error;

    return header;
}
}
