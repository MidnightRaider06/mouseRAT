#include "Protocol.h"

#include <cassert>
#include <limits>
#include <stdexcept>

using namespace protocol;

void expectError(const HeaderBytes& bytes, HeaderError expected) {
    const auto result = decodeHeader(bytes);
    assert(std::holds_alternative<HeaderError>(result));
    assert(std::get<HeaderError>(result) == expected);
}

int main() {
    const HeaderBytes command{
        0x4d, 0x52, 0x41, 0x54, 0x01, 0x10, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01,
    };
    const HeaderBytes complete{
        0x4d, 0x52, 0x41, 0x54, 0x01, 0x12, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01,
    };
    const HeaderBytes imageData{
        0x4d, 0x52, 0x41, 0x54, 0x01, 0x22, 0x00, 0x00,
        0x00, 0x01, 0x00, 0x00, 0x89, 0xab, 0xcd, 0xef,
    };
    assert(encodeHeader({MessageType::Command, 3, 1}) == command);
    assert(encodeHeader({MessageType::Complete, 4, 1}) == complete);
    assert(encodeHeader({MessageType::ImageData, 65536, 0x89abcdef}) == imageData);
    const auto decoded = std::get<Header>(decodeHeader(imageData));
    assert(decoded.type == MessageType::ImageData);
    assert(decoded.payloadLength == 65536 && decoded.requestId == 0x89abcdef);

    for (std::size_t offset : {0, 1, 2, 3, 4, 5, 6, 7}) {
        auto invalid = command;
        invalid[offset] = 0xff;
        const auto error = offset < 4 ? HeaderError::InvalidMagic :
                           offset == 4 ? HeaderError::UnsupportedVersion :
                           offset == 5 ? HeaderError::UnknownType : HeaderError::UnsupportedFlags;
        expectError(invalid, error);
    }

    struct Bounds {
        MessageType type;
        std::uint32_t minimum, maximum, requestId;
    };
    const Bounds bounds[] = {
        {MessageType::Hello, 0, 0, 0},
        {MessageType::HelloAck, 0, 0, 0},
        {MessageType::Command, 1, 4096, 1},
        {MessageType::Output, 2, 65536, 1},
        {MessageType::Complete, 4, 4, 1},
        {MessageType::Screenshot, 0, 0, 1},
        {MessageType::ImageBegin, 12, 12, 1},
        {MessageType::ImageData, 1, 65536, 1},
        {MessageType::Ping, 8, 8, 0},
        {MessageType::Pong, 8, 8, 0},
        {MessageType::Error, 4, 1028, 1},
    };

    for (const auto& bound : bounds) {
        for (auto length : {bound.minimum, bound.maximum}) {
            auto bytes = encodeHeader({bound.type, length, bound.requestId});
            assert(std::get<Header>(decodeHeader(bytes)).payloadLength == length);
            bytes[15] = bound.requestId == 0 ? 1 : 0;
            expectError(bytes, HeaderError::InvalidRequestId);
        }

        auto bytes = encodeHeader({bound.type, bound.minimum, bound.requestId});
        const auto badLength = bound.maximum + 1;
        for (std::size_t i = 0; i < 4; ++i) {
            bytes[8 + i] = static_cast<std::uint8_t>(badLength >> (24 - 8 * i));
        }
        expectError(bytes, badLength > maxPayloadSize ? HeaderError::PayloadTooLarge :
                                                     HeaderError::InvalidPayloadLength);

        if (bound.minimum > 0) {
            const auto tooShort = bound.minimum - 1;
            for (std::size_t i = 0; i < 4; ++i) {
                bytes[8 + i] = static_cast<std::uint8_t>(tooShort >> (24 - 8 * i));
            }
            expectError(bytes, HeaderError::InvalidPayloadLength);
        }
    }

    auto oversized = command;
    oversized[8] = oversized[9] = oversized[10] = oversized[11] = 0xff;
    expectError(oversized, HeaderError::PayloadTooLarge);
    const auto maximumId = std::numeric_limits<std::uint32_t>::max();
    assert(std::get<Header>(decodeHeader(encodeHeader({MessageType::Command, 1, maximumId})))
               .requestId == maximumId);

    try {
        (void)encodeHeader({MessageType::Command, 0, 1});
        assert(false);
    } catch (const std::invalid_argument&) {}

    for (const auto text : {"", "ASCII\x7f", "\xc2\x80", "\xdf\xbf", "\xe0\xa0\x80",
                            "\xed\x9f\xbf", "\xee\x80\x80", "\xef\xbf\xbf",
                            "\xf0\x90\x80\x80", "\xf4\x8f\xbf\xbf", "a\xe2\x82\xac!"}) {
        assert(isValidUtf8(text));
    }
    assert(isValidUtf8(std::string_view("a\0b", 3)));

    for (const auto text : {"\x80", "\xbf", "\xc0\x80", "\xc1\xbf", "\xc2", "\xe2\x82",
                            "\xf0\x90\x80", "\xe2(\xa1", "\xe0\x9f\xbf", "\xed\xa0\x80",
                            "\xed\xbf\xbf", "\xf0\x8f\xbf\xbf", "\xf4\x90\x80\x80",
                            "\xf5\x80\x80\x80", "\xff", "ok\xc2\xa2\x80"}) {
        assert(!isValidUtf8(text));
    }
}
