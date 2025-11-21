#include "../mouseRAT-client/Protocol.h"

#include <cassert>

void expectInvalidHeader(const std::array<std::uint8_t, 16>& bytes) {
    bool rejected = false;
    try {
        protocol::decodeHeader(bytes);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }

    assert(rejected);
}

int main() {
    const std::array<std::uint8_t, 16> command{
        0x4d, 0x52, 0x41, 0x54, 1, 0x10, 0, 0, 0, 0, 0, 3, 0, 0, 0, 1
    };
    const std::array<std::uint8_t, 16> complete{
        0x4d, 0x52, 0x41, 0x54, 1, 0x12, 0, 0, 0, 0, 0, 4, 0, 0, 0, 1
    };
    const std::array<std::uint8_t, 16> maximum{
        0x4d, 0x52, 0x41, 0x54, 1, 0x22, 0, 0, 0, 1, 0, 0, 0x12, 0x34, 0xab, 0xcd
    };
    assert(protocol::encodeHeader(0x10, 3, 1) == command);
    assert(protocol::encodeHeader(0x12, 4, 1) == complete);
    assert(protocol::encodeHeader(0x22, 65536, 0x1234abcd) == maximum);

    const auto decodedCommand = protocol::decodeHeader(command);
    assert(decodedCommand.type == 0x10 && decodedCommand.payloadLength == 3
        && decodedCommand.requestId == 1);
    const auto decodedMaximum = protocol::decodeHeader(maximum);
    assert(decodedMaximum.type == 0x22 && decodedMaximum.payloadLength == 65536
        && decodedMaximum.requestId == 0x1234abcd);
    const auto hello = protocol::decodeHeader(protocol::encodeHeader(0x01, 0, 0));
    assert(hello.type == 0x01 && hello.payloadLength == 0 && hello.requestId == 0);

    for (const unsigned index : { 0u, 1u, 2u, 3u, 4u, 6u, 7u }) {
        auto invalid = command;
        invalid[index] ^= 0xff;
        expectInvalidHeader(invalid);
    }

    auto oversized = maximum;
    oversized[11] = 1;
    expectInvalidHeader(oversized);
    oversized[8] = 0xff;
    expectInvalidHeader(oversized);

    bool rejected = false;
    try {
        protocol::encodeHeader(0x22, 65537, 1);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }

    assert(rejected);
}
