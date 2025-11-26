#include "../mouseRAT-client/Protocol.h"
#include "../mouseRAT-client/HeaderAssembler.h"

#include <cassert>
#include <string>

void checkHeaderAssembly() {
    const auto command = protocol::encodeHeader(0x10, 3, 1);
    const std::string_view wire(reinterpret_cast<const char*>(command.data()), command.size());
    for (std::size_t split = 0; split <= wire.size(); ++split) {
        protocol::HeaderAssembler assembler;
        std::size_t consumed;
        auto header = assembler.consume(wire.substr(0, split), consumed);
        assert(consumed == split && header.has_value() == (split == wire.size()));
        if (!header) {
            assert(assembler.hasPartialHeader() == (split != 0));
            assert(!assembler.consume({}, consumed) && consumed == 0);
            header = assembler.consume(wire.substr(split), consumed);
            assert(consumed == wire.size() - split);
        }

        assert(header && header->type == 0x10 && header->payloadLength == 3
            && header->requestId == 1 && !assembler.hasPartialHeader());
    }

    protocol::HeaderAssembler assembler;
    std::size_t consumed;
    const auto next = protocol::encodeHeader(0x20, 0, 2);
    const auto withPayload = std::string(wire) + "dir"
        + std::string(reinterpret_cast<const char*>(next.data()), next.size());
    assert(assembler.consume(withPayload, consumed) && consumed == 16);
    assert(std::string_view(withPayload).substr(consumed, 3) == "dir");
    const auto header = assembler.consume(std::string_view(withPayload).substr(consumed + 3), consumed);
    assert(header && header->type == 0x20 && header->requestId == 2 && consumed == 16);

    auto invalid = command;
    invalid[0] = 'X';
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        bool rejected = false;
        try {
            const auto& bytes = attempt == 0 ? invalid : command;
            assembler.consume({ reinterpret_cast<const char*>(bytes.data()), bytes.size() }, consumed);
        } catch (const std::exception&) {
            rejected = true;
        }

        assert(rejected && consumed == (attempt == 0 ? 16u : 0u));
    }
}

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
    checkHeaderAssembly();

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

    for (const unsigned index : { 0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u }) {
        auto invalid = command;
        invalid[index] ^= 0xff;
        expectInvalidHeader(invalid);
    }

    struct Bounds {
        std::uint8_t type;
        std::uint32_t minimum, maximum, requestId;
        bool fromServer;
    };
    const Bounds bounds[]{
        { 0x01, 0, 0, 0, false }, { 0x02, 0, 0, 0, true }, { 0x10, 1, 4096, 1, true },
        { 0x11, 2, 65536, 1, false }, { 0x12, 4, 4, 1, false }, { 0x20, 0, 0, 1, true },
        { 0x21, 12, 12, 1, false }, { 0x22, 1, 65536, 1, false }, { 0x30, 8, 8, 0, true },
        { 0x31, 8, 8, 0, true }, { 0x7f, 4, 1028, 1, false }
    };
    for (const auto& bound : bounds) {
        for (const auto direction : { protocol::Direction::FromServer, protocol::Direction::FromClient }) {
            protocol::HeaderAssembler assembler(direction);
            const auto bytes = protocol::encodeHeader(bound.type, bound.minimum, bound.requestId);
            std::size_t consumed;
            bool accepted = false;
            try {
                accepted = assembler.consume(
                    { reinterpret_cast<const char*>(bytes.data()), bytes.size() }, consumed).has_value();
            } catch (const std::invalid_argument&) {
            }

            const bool expected = bound.type == 0x30 || bound.type == 0x31
                || ((direction == protocol::Direction::FromServer) == bound.fromServer);
            assert(accepted == expected && consumed == 16);
            if (!accepted) {
                bool failed = false;
                try {
                    assembler.consume({}, consumed);
                } catch (const std::logic_error&) {
                    failed = true;
                }

                assert(failed && consumed == 0);
            }
        }

        for (const auto length : { bound.minimum, bound.maximum }) {
            auto bytes = protocol::encodeHeader(bound.type, length, bound.requestId);
            assert(protocol::decodeHeader(bytes).payloadLength == length);
            bytes[15] = bound.requestId == 0 ? 1 : 0;
            expectInvalidHeader(bytes);
        }

        for (const auto length : { bound.minimum - 1, bound.maximum + 1 }) {
            auto bytes = protocol::encodeHeader(bound.type, bound.minimum, bound.requestId);
            for (unsigned index = 0; index < 4; ++index) {
                bytes[8 + index] = static_cast<std::uint8_t>(length >> (24 - index * 8));
            }

            expectInvalidHeader(bytes);
        }
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
