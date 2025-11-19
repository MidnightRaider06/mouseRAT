#include "RequestTracker.h"

#include <cassert>
#include <string>

using namespace protocol;

FrameView response(MessageType type, std::uint32_t id, std::string_view payload) {
    return {{type, static_cast<std::uint32_t>(payload.size()), id}, payload};
}

void drain(OutboundQueue& queue) {
    while (!queue.empty()) {
        queue.advance(queue.pending().size());
    }
}

int main() {
    OutboundQueue outbound;
    RequestTracker tracker;
    const std::string complete(4, '\0');
    const std::string failure("\0\0\0\x02", 4);
    const std::string output("\x01"
                             "A\0B",
                             4);
    assert(tracker.acceptResponse(response(MessageType::Complete, 1, complete)) ==
           RequestError::NoActiveRequest);

    assert(std::get<std::uint32_t>(tracker.queueCommand(outbound, "dir")) == 1);
    const auto queuedSize = outbound.size();
    assert(std::get<RequestError>(tracker.queueCommand(outbound, "whoami")) == RequestError::Busy);
    assert(outbound.size() == queuedSize);
    drain(outbound);
    assert(outbound.empty() && tracker.activeId() == 1);

    assert(!tracker.acceptResponse(response(MessageType::Output, 1, output)));
    assert(!tracker.acceptResponse(response(MessageType::Output, 1, output)));
    assert(tracker.activeId() == 1);
    assert(tracker.acceptResponse(response(MessageType::Complete, 2, complete)) ==
           RequestError::WrongRequestId);
    assert(tracker.acceptResponse(response(MessageType::ImageData, 1, "x")) == RequestError::UnexpectedType);
    assert(tracker.acceptResponse(response(MessageType::Complete, 1, "x")) == RequestError::InvalidPayload);
    assert(tracker.acceptResponse(response(MessageType::Error, 1, "bad")) == RequestError::InvalidPayload);
    assert(tracker.acceptResponse(response(MessageType::Error, 1, std::string(1029, 'x'))) ==
           RequestError::InvalidPayload);
    for (const auto& invalid :
         {std::string(4, '\0'), std::string("\0\0\0\x05", 4), std::string("\x01\0\0\x02", 4),
          failure + "\xed\xa0\x80", failure + std::string(1023, 'x') + "\xc2"}) {
        assert(tracker.acceptResponse(response(MessageType::Error, 1, invalid)) ==
               RequestError::InvalidPayload);
        assert(tracker.activeId() == 1);
    }

    auto mismatched = response(MessageType::Complete, 1, complete);
    mismatched.header.payloadLength = 3;
    assert(tracker.acceptResponse(mismatched) == RequestError::InvalidPayload);
    assert(tracker.acceptResponse(response(MessageType::Output, 1, "\x03x")) == RequestError::InvalidPayload);
    assert(tracker.activeId() == 1);

    assert(!tracker.acceptResponse(response(MessageType::Complete, 1, failure)));
    assert(tracker.activeId() == 0);
    assert(tracker.acceptResponse(response(MessageType::Complete, 1, complete)) ==
           RequestError::NoActiveRequest);

    assert(std::get<std::uint32_t>(tracker.queueCommand(outbound, "echo")) == 2);
    drain(outbound);
    assert(!tracker.acceptResponse(response(MessageType::Complete, 2, complete)));
    assert(std::get<std::uint32_t>(tracker.queueCommand(outbound, "echo")) == 3);
    drain(outbound);
    assert(!tracker.acceptResponse(response(MessageType::Error, 3, failure)));
    assert(tracker.activeId() == 0);

    RequestTracker admission;

    for (const auto& invalid :
         {std::string{}, std::string(4097, 'x'), std::string("a\0b", 3), std::string("\xc0\x80"),
          std::string("\xed\xa0\x80"), std::string("echo \xe2\x82")}) {
        assert(std::get<RequestError>(admission.queueCommand(outbound, invalid)) ==
               RequestError::InvalidCommand);
        assert(admission.activeId() == 0 && outbound.empty());
    }

    const std::string maximum(maxPayloadSize, 'x');
    assert(outbound.enqueue({MessageType::ImageData, maxPayloadSize, 8}, maximum));
    const auto remaining = OutboundQueue::capacity - outbound.size() - headerSize;
    assert(outbound.enqueue({MessageType::ImageData, static_cast<std::uint32_t>(remaining), 9},
                            std::string_view(maximum).substr(0, remaining)));
    assert(std::get<RequestError>(admission.queueCommand(outbound, "dir")) == RequestError::QueueFull);
    assert(admission.activeId() == 0 && outbound.size() == OutboundQueue::capacity);
    drain(outbound);
    assert(std::get<std::uint32_t>(admission.queueCommand(outbound, std::string(4096, 'x'))) == 1);
    drain(outbound);
    assert(!admission.acceptResponse(response(MessageType::Complete, 1, complete)));

    RequestTracker limited(2);

    for (std::uint32_t id = 1; id <= 2; ++id) {
        assert(std::get<std::uint32_t>(limited.queueCommand(outbound, "dir")) == id);
        drain(outbound);
        assert(!limited.acceptResponse(response(MessageType::Complete, id, complete)));
    }

    assert(std::get<RequestError>(limited.queueCommand(outbound, "dir")) == RequestError::IdExhausted);
    assert(outbound.empty() && limited.activeId() == 0);

    assert(std::get<std::uint32_t>(tracker.queueCommand(outbound, "dir")) == 4);
    drain(outbound);
    auto validOutput = maximum;
    validOutput[0] = '\0';
    const auto chunk = response(MessageType::Output, 4, validOutput);
    const auto dataPerFrame = validOutput.size() - 1;

    for (std::size_t sent = 0; sent < RequestTracker::maxOutputBytes / dataPerFrame; ++sent) {
        assert(!tracker.acceptResponse(chunk));
    }

    const std::string tail(RequestTracker::maxOutputBytes % dataPerFrame + 1, '\0');
    assert(!tracker.acceptResponse(response(MessageType::Output, 4, tail)));
    assert(tracker.acceptResponse(response(MessageType::Output, 4, std::string(2, '\0'))) ==
           RequestError::OutputLimitExceeded);
    assert(tracker.activeId() == 4);
    assert(!tracker.acceptResponse(response(MessageType::Error, 4, failure)));

    for (std::uint32_t code = 1; code <= 4; ++code) {
        const std::string command = "echo \xe2\x82\xac";
        const auto id = std::get<std::uint32_t>(tracker.queueCommand(outbound, command));
        assert(outbound.pending().substr(headerSize) == command);
        drain(outbound);
        assert(!tracker.acceptResponse(response(MessageType::Output, id, std::string("\0\xff\0", 3))));

        std::string error(4, '\0');
        error[3] = static_cast<char>(code);

        if (code != 1) {
            error += std::string(1022, 'x') + "\xc2\xa2";
        }

        assert(!tracker.acceptResponse(response(MessageType::Error, id, error)));
        assert(tracker.activeId() == 0);
    }
}
