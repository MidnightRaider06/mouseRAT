#include "../mouseRAT-client/OutboundQueue.h"
#include "../mouseRAT-client/FrameAssembler.h"

#include <cassert>
#include <string>

template <typename Error, typename Action>
void expectThrow(Action action) {
    bool rejected = false;
    try {
        action();
    } catch (const Error&) {
        rejected = true;
    }

    assert(rejected);
}

bool put(protocol::OutboundQueue& queue, std::uint8_t type, std::uint32_t id, std::string_view payload) {
    return queue.enqueue({ type, static_cast<std::uint32_t>(payload.size()), id }, payload);
}

std::string wire(std::uint8_t type, std::uint32_t id, std::string_view payload) {
    const auto header = protocol::encodeHeader(type, static_cast<std::uint32_t>(payload.size()), id);
    return std::string(reinterpret_cast<const char*>(header.data()), header.size()) + std::string(payload);
}

std::string drain(protocol::OutboundQueue& queue, std::size_t chunk) {
    std::string bytes;
    while (!queue.empty()) {
        const auto pending = queue.pending().substr(0, chunk);
        bytes.append(pending);
        queue.advance(pending.size());
    }

    return bytes;
}

void partialSends() {
    protocol::OutboundQueue queue;
    std::string output(65536, '\0');
    output[1] = static_cast<char>(0xff);
    const auto second = output.substr(0, 65496);
    const std::string nonce(8, '\0');
    const auto expected = wire(0x11, 7, output) + wire(0x11, 7, second) + wire(0x31, 0, nonce);
    assert(put(queue, 0x11, 7, output) && put(queue, 0x11, 7, second));
    output[1] = 'X';
    assert(queue.size() == queue.capacity - 8);
    assert(!put(queue, 0x31, 0, nonce) && queue.size() == queue.capacity - 8);

    std::string sent(queue.pending().substr(0, 33));
    queue.advance(33);
    assert(put(queue, 0x31, 0, nonce));
    const auto size = queue.size();
    queue.advance(0);
    assert(queue.size() == size);
    expectThrow<std::out_of_range>([&] { queue.advance(queue.pending().size() + 1); });
    assert(queue.size() == size);
    sent += drain(queue, 7);
    assert(sent == expected && queue.empty() && queue.pending().empty());

    protocol::FrameAssembler decoder(protocol::Direction::FromClient);
    std::string_view remaining(sent);
    for (const auto type : { 0x11, 0x11, 0x31 }) {
        std::size_t consumed;
        const auto frame = decoder.consume(remaining, consumed);
        assert(frame && frame->header.type == type);
        remaining.remove_prefix(consumed);
    }

    assert(remaining.empty() && !decoder.hasPartialFrame());
    assert(put(queue, 0x11, 7, output) && put(queue, 0x11, 7, output.substr(0, 65504)));
    assert(queue.size() == queue.capacity && !put(queue, 0x31, 0, nonce));
    queue.advance(24);
    assert(put(queue, 0x31, 0, nonce) && queue.size() == queue.capacity);
    (void)drain(queue, 4096);
    expectThrow<std::out_of_range>([&] { queue.advance(1); });
}

void payloadValidation() {
    protocol::OutboundQueue queue;
    expectThrow<std::invalid_argument>([&] { (void)queue.enqueue({ 0x11, 3, 1 }, "xx"); });
    expectThrow<std::invalid_argument>([&] { (void)put(queue, 0x10, 1, "x"); });
    expectThrow<std::invalid_argument>([&] { (void)put(queue, 0x02, 0, {}); });
    expectThrow<std::invalid_argument>([&] { (void)put(queue, 0x20, 1, {}); });
    expectThrow<std::invalid_argument>([&] { (void)put(queue, 0xff, 0, {}); });
    expectThrow<std::invalid_argument>([&] { (void)put(queue, 0x11, 1, std::string{ '\3', 'x' }); });
    expectThrow<std::invalid_argument>([&] { (void)put(queue, 0x11, 1, "x"); });

    std::string error(4, '\0');
    for (const char code : { '\0', '\5' }) {
        error[3] = code;
        expectThrow<std::invalid_argument>([&] { (void)put(queue, 0x7f, 1, error); });
    }

    error[3] = '\2';
    error += "\xed\xa0\x80";
    expectThrow<std::invalid_argument>([&] { (void)put(queue, 0x7f, 1, error); });
    std::string begin(12, '\0');
    begin[11] = 1;
    expectThrow<std::invalid_argument>([&] { (void)put(queue, 0x21, 1, begin); });
    begin[4] = 2;
    begin[7] = 1;
    expectThrow<std::invalid_argument>([&] { (void)put(queue, 0x21, 1, begin); });
    begin[7] = 0;
    begin[11] = 3;
    expectThrow<std::invalid_argument>([&] { (void)put(queue, 0x21, 1, begin); });
    begin[0] = 1;
    begin[11] = 1;
    expectThrow<std::invalid_argument>([&] { (void)put(queue, 0x21, 1, begin); });
    assert(queue.empty());

    begin[0] = 0;
    for (const char format : { '\1', '\2' }) {
        begin[11] = format;
        assert(put(queue, 0x21, 1, begin));
    }

    error.assign(4, '\0');
    for (const char code : { '\1', '\2', '\3', '\4' }) {
        error[3] = code;
        assert(put(queue, 0x7f, 1, error));
    }

    error += '\0';
    assert(put(queue, 0x7f, 1, error));
    for (const char stream : { '\0', '\1', '\2' }) {
        assert(put(queue, 0x11, 1, std::string{ stream, static_cast<char>(0xff) }));
    }

    assert(put(queue, 0x01, 0, {}));
    assert(put(queue, 0x12, 1, std::string(4, '\0')));
    assert(put(queue, 0x22, 1, std::string(65536, '\0')));
    assert(put(queue, 0x30, 0, std::string(8, '\0')));
    (void)drain(queue, 1);
}

int main() {
    partialSends();
    payloadValidation();
}
