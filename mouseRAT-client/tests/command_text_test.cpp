#include "../mouseRAT-client/FrameAssembler.h"

#include <cassert>
#include <string>

int main() {
    for (const std::string_view text : { "x", "dir", "\xc2\x80", "\xdf\xbf", "\xe0\xa0\x80",
        "\xed\x9f\xbf", "\xee\x80\x80", "\xef\xbf\xbf", "\xf0\x90\x80\x80", "\xf4\x8f\xbf\xbf" }) {
        assert(protocol::isValidCommandText(text));
        const auto header = protocol::encodeHeader(0x10, static_cast<std::uint32_t>(text.size()), 1);
        const auto wire = std::string(reinterpret_cast<const char*>(header.data()), header.size())
            + std::string(text);
        protocol::FrameAssembler assembler;
        std::size_t consumed;
        const auto prefixSize = wire.size() - 1;
        assert(!assembler.consume(std::string_view(wire).substr(0, prefixSize), consumed)
            && consumed == prefixSize);
        const auto frame = assembler.consume(std::string_view(wire).substr(prefixSize), consumed);
        assert(frame && frame->payload == text && consumed == 1);
    }

    const std::string_view invalid[]{ "\x80", "\xc0\xaf", "\xe0\x80\x80", "\xed\xa0\x80",
        "\xf0\x80\x80\x80", "\xf4\x90\x80\x80", "\xf5\x80\x80\x80", "\xc2", "\xe2\x82",
        "\xf0\x90\x80", "\xe2(\xa1", std::string_view("a\0b", 3) };
    for (const auto text : invalid) {
        assert(!protocol::isValidCommandText(text));
        const auto header = protocol::encodeHeader(0x10, static_cast<std::uint32_t>(text.size()), 1);
        const auto wire = std::string(reinterpret_cast<const char*>(header.data()), header.size())
            + std::string(text);
        protocol::FrameAssembler assembler;
        std::size_t consumed;
        try {
            assembler.consume(wire, consumed);
            assert(false);
        } catch (const std::invalid_argument&) {
        }

        assert(consumed == wire.size() && !assembler.hasPartialFrame());
        try {
            assembler.consume({}, consumed);
            assert(false);
        } catch (const std::logic_error&) {
        }

        assert(consumed == 0);
    }

    assert(!protocol::isValidCommandText({}));
    assert(protocol::isValidCommandText(std::string(4096, 'a')));
    assert(!protocol::isValidCommandText(std::string(4097, 'a')));
}
