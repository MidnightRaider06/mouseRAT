#include "OutboundQueue.h"

#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <string>

using namespace protocol;

std::string wireFrame(const Header& header, std::string_view payload) {
    const auto encoded = encodeHeader(header);
    std::string wire(encoded.begin(), encoded.end());
    wire.append(payload);
    return wire;
}

std::string drain(OutboundQueue& queue, std::size_t writeSize) {
    std::string wire;
    while (!queue.empty()) {
        const auto pending = queue.pending();
        const auto sent = std::min(pending.size(), writeSize);
        assert(sent > 0);
        wire.append(pending.substr(0, sent));
        queue.advance(sent);
    }
    assert(queue.size() == 0 && queue.pending().empty());
    return wire;
}

int main() {
    const Header hello{MessageType::HelloAck, 0, 0};
    const Header command{MessageType::Command, 3, 1};
    OutboundQueue queue;
    assert(queue.empty());
    queue.advance(0);

    std::string text = "dir";
    assert(queue.enqueue(hello, {}));
    assert(queue.enqueue(command, text));
    text[0] = 'x';
    const auto expected = wireFrame(hello, {}) + wireFrame(command, "dir");
    const auto before = std::string(queue.pending());
    queue.advance(0);
    assert(queue.pending() == before);
    assert(drain(queue, 1) == expected);

    const std::string payload(maxPayloadSize, '\xa5');
    const Header image{MessageType::ImageData, maxPayloadSize, 2};
    const auto imageWire = wireFrame(image, payload);
    assert(queue.enqueue(image, payload));
    const auto originalSize = queue.size();
    assert(!queue.enqueue(image, payload));
    assert(queue.size() == originalSize && queue.pending() == imageWire);

    const auto remaining = OutboundQueue::capacity - queue.size() - headerSize;
    const Header second{MessageType::ImageData, static_cast<std::uint32_t>(remaining), 3};
    const std::string binary(remaining, '\0');
    assert(queue.enqueue(second, binary));
    assert(queue.size() == OutboundQueue::capacity);
    assert(!queue.enqueue(hello, {}));
    assert(drain(queue, 7) == imageWire + wireFrame(second, binary));

    assert(queue.enqueue(image, payload));
    queue.advance(13);
    const auto wrappedLength = OutboundQueue::capacity - queue.size() - headerSize;
    const Header wrapped{MessageType::ImageData, static_cast<std::uint32_t>(wrappedLength), 4};
    const auto wrappedPayload = std::string_view(payload).substr(0, wrappedLength);
    assert(queue.enqueue(wrapped, wrappedPayload));
    assert(queue.size() == OutboundQueue::capacity);
    assert(queue.pending().size() == OutboundQueue::capacity - 13);
    assert(drain(queue, 4093) == imageWire.substr(13) + wireFrame(wrapped, wrappedPayload));

    const Header nearEnd{MessageType::ImageData, static_cast<std::uint32_t>(remaining - 8), 5};
    const auto nearPayload = std::string_view(payload).substr(0, nearEnd.payloadLength);
    assert(queue.enqueue(image, payload));
    assert(queue.enqueue(nearEnd, nearPayload));
    queue.advance(13);
    assert(queue.enqueue(hello, {}));
    const auto prefix = imageWire + wireFrame(nearEnd, nearPayload);
    assert(drain(queue, 4093) == prefix.substr(13) + wireFrame(hello, {}));

    assert(queue.enqueue(hello, {}));
    try {
        queue.advance(queue.pending().size() + 1);
        assert(false);
    } catch (const std::out_of_range&) {}
    assert(queue.pending() == wireFrame(hello, {}));

    try {
        (void)queue.enqueue(command, "di");
        assert(false);
    } catch (const std::invalid_argument&) {}
    try {
        (void)queue.enqueue({MessageType::Command, 0, 1}, {});
        assert(false);
    } catch (const std::invalid_argument&) {}
    assert(drain(queue, 16) == wireFrame(hello, {}));
}
