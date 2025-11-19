#include "FrameDecoder.h"

#include <algorithm>
#include <cassert>
#include <string>

using namespace protocol;

std::string wireFrame(MessageType type, std::uint32_t id, std::string_view payload) {
    const auto header = encodeHeader({type, static_cast<std::uint32_t>(payload.size()), id});
    std::string wire(header.begin(), header.end());
    wire.append(payload);
    return wire;
}

void expectFrame(const DecodeStep& step, MessageType type, std::uint32_t id,
                 std::string_view payload) {
    assert(std::holds_alternative<FrameView>(step.result));
    const auto frame = std::get<FrameView>(step.result);
    assert(frame.header.type == type && frame.header.requestId == id);
    assert(frame.header.payloadLength == payload.size());
    assert(frame.payload == payload);
}

int main() {
    const std::string payload("\x01" "A\0MRAT", 7);
    const auto wire = wireFrame(MessageType::Output, 1, payload);

    for (std::size_t split = 0; split < wire.size(); ++split) {
        FrameDecoder decoder;
        const auto first = decoder.consume(std::string_view(wire).substr(0, split));
        assert(first.consumed == split);
        assert(std::holds_alternative<std::monostate>(first.result));
        assert(decoder.hasPartialFrame() == (split != 0));

        const auto empty = decoder.consume({});
        assert(empty.consumed == 0 && std::holds_alternative<std::monostate>(empty.result));
        assert(decoder.hasPartialFrame() == (split != 0));

        const auto second = decoder.consume(std::string_view(wire).substr(split));
        assert(second.consumed == wire.size() - split);
        expectFrame(second, MessageType::Output, 1, payload);
        assert(!decoder.hasPartialFrame());
    }

    FrameDecoder bytes;
    for (std::size_t i = 0; i < wire.size(); ++i) {
        const auto step = bytes.consume(std::string_view(wire).substr(i, 1));
        assert(step.consumed == 1);
        if (i + 1 == wire.size()) expectFrame(step, MessageType::Output, 1, payload);
        else assert(std::holds_alternative<std::monostate>(step.result));
    }

    const auto hello = wireFrame(MessageType::Hello, 0, {});
    FrameDecoder emptyPayload;
    for (std::size_t i = 0; i < hello.size(); ++i) {
        const auto step = emptyPayload.consume(std::string_view(hello).substr(i, 1));
        assert(step.consumed == 1);
        if (i + 1 == hello.size()) expectFrame(step, MessageType::Hello, 0, {});
        else assert(std::holds_alternative<std::monostate>(step.result));
    }

    const auto joined = hello + wire + hello;
    FrameDecoder coalesced;
    std::size_t offset = 0;
    for (std::size_t expected : {hello.size(), wire.size(), hello.size()}) {
        const auto step = coalesced.consume(std::string_view(joined).substr(offset));
        assert(step.consumed == expected);
        if (offset == hello.size()) expectFrame(step, MessageType::Output, 1, payload);
        else expectFrame(step, MessageType::Hello, 0, {});
        offset += step.consumed;
    }
    assert(offset == joined.size() && !coalesced.hasPartialFrame());

    const auto tail = wire + wire.substr(0, 9);
    FrameDecoder incomplete;
    assert(incomplete.consume(tail).consumed == wire.size());
    const auto partial = incomplete.consume(std::string_view(tail).substr(wire.size()));
    assert(partial.consumed == 9 && incomplete.hasPartialFrame());
    assert(std::holds_alternative<std::monostate>(partial.result));
    expectFrame(incomplete.consume(std::string_view(wire).substr(9)), MessageType::Output, 1, payload);

    std::string maximum(maxPayloadSize, '\xa5');
    maximum.back() = '\0';
    const auto large = wireFrame(MessageType::ImageData, 2, maximum);
    FrameDecoder bounded;
    for (std::size_t position = 0; position < large.size();) {
        const auto chunk = std::string_view(large).substr(position, 4093);
        const auto step = bounded.consume(chunk);
        assert(step.consumed == chunk.size());
        position += step.consumed;
        if (position == large.size()) expectFrame(step, MessageType::ImageData, 2, maximum);
        else assert(std::holds_alternative<std::monostate>(step.result));
    }

    for (const auto expected : {HeaderError::InvalidMagic, HeaderError::PayloadTooLarge}) {
        auto invalid = wire;
        if (expected == HeaderError::InvalidMagic) invalid[0] = '?';
        else invalid[8] = invalid[9] = invalid[10] = invalid[11] = '\xff';

        FrameDecoder failed;
        assert(failed.consume(std::string_view(invalid).substr(0, 13)).consumed == 13);
        const auto rejected = failed.consume(std::string_view(invalid).substr(13));
        assert(rejected.consumed == headerSize - 13);
        assert(std::get<HeaderError>(rejected.result) == expected);
        const auto retry = failed.consume(hello);
        assert(retry.consumed == 0 && std::get<HeaderError>(retry.result) == expected);
    }
}
