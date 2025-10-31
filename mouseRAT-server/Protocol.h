#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <variant>

namespace protocol {
constexpr std::size_t headerSize = 16;
constexpr std::uint32_t maxPayloadSize = 65536;
constexpr std::uint8_t version = 1;
using HeaderBytes = std::array<std::uint8_t, headerSize>;

enum class MessageType : std::uint8_t {
    Hello = 0x01,
    HelloAck = 0x02,
    Command = 0x10,
    Output = 0x11,
    Complete = 0x12,
    Screenshot = 0x20,
    ImageBegin = 0x21,
    ImageData = 0x22,
    Ping = 0x30,
    Pong = 0x31,
    Error = 0x7f,
};

struct Header {
    MessageType type;
    std::uint32_t payloadLength;
    std::uint32_t requestId;
};

enum class HeaderError {
    InvalidMagic,
    UnsupportedVersion,
    UnsupportedFlags,
    UnknownType,
    PayloadTooLarge,
    InvalidPayloadLength,
    InvalidRequestId,
};

using DecodeResult = std::variant<Header, HeaderError>;

[[nodiscard]] HeaderBytes encodeHeader(const Header& header);
[[nodiscard]] DecodeResult decodeHeader(const HeaderBytes& bytes) noexcept;
}
