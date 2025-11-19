#include "RequestTracker.h"

#include <cassert>
#include <limits>
#include <string>

using namespace protocol;

FrameView response(MessageType type, std::uint32_t id, std::string_view payload) {
    return {{type, static_cast<std::uint32_t>(payload.size()), id}, payload};
}

std::string metadata(std::uint64_t total, std::uint32_t format) {
    std::string bytes(12, '\0');

    for (std::size_t i = 0; i < 8; ++i) {
        bytes[i] = static_cast<char>(total >> (56 - 8 * i));
    }

    for (std::size_t i = 0; i < 4; ++i) {
        bytes[8 + i] = static_cast<char>(format >> (24 - 8 * i));
    }

    return bytes;
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
    const std::string failure("\0\0\0\x03", 4);
    assert(std::get<std::uint32_t>(tracker.queueScreenshot(outbound)) == 1);
    assert(outbound.size() == headerSize);
    assert(std::get<RequestError>(tracker.queueCommand(outbound, "dir")) == RequestError::Busy);
    assert(std::get<RequestError>(tracker.queueScreenshot(outbound)) == RequestError::Busy);
    drain(outbound);
    assert(tracker.activeId() == 1 && !tracker.imageInfo());

    assert(tracker.acceptResponse(response(MessageType::ImageData, 1, "x")) == RequestError::UnexpectedType);
    assert(tracker.acceptResponse(response(MessageType::Complete, 1, complete)) ==
           RequestError::UnexpectedType);
    assert(tracker.acceptResponse(response(MessageType::ImageBegin, 2, metadata(5, 1))) ==
           RequestError::WrongRequestId);
    assert(tracker.acceptResponse(response(MessageType::ImageBegin, 1, "short")) ==
           RequestError::InvalidPayload);
    for (auto format : {0u, 3u, 256u, 257u}) {
        assert(tracker.acceptResponse(response(MessageType::ImageBegin, 1, metadata(5, format))) ==
               RequestError::InvalidPayload);
    }

    assert(tracker.acceptResponse(response(MessageType::ImageBegin, 1, metadata(0, 1))) ==
           RequestError::InvalidPayload);
    for (auto size : {RequestTracker::maxImageBytes + 1, std::uint64_t{1} << 32,
                      std::numeric_limits<std::uint64_t>::max()}) {
        assert(tracker.acceptResponse(response(MessageType::ImageBegin, 1, metadata(size, 1))) ==
               RequestError::ImageLimitExceeded);
        assert(!tracker.imageInfo());
    }

    assert(!tracker.acceptResponse(response(MessageType::ImageBegin, 1, metadata(5, 1))));
    assert(tracker.imageInfo()->totalBytes == 5 && tracker.imageInfo()->format == ImageFormat::Png);
    assert(tracker.acceptResponse(response(MessageType::ImageBegin, 1, metadata(9, 2))) ==
           RequestError::UnexpectedType);
    assert(tracker.imageInfo()->totalBytes == 5 && tracker.imageInfo()->format == ImageFormat::Png);
    assert(tracker.acceptResponse(response(MessageType::Output, 1, std::string(2, '\0'))) ==
           RequestError::UnexpectedType);
    assert(tracker.acceptResponse(response(MessageType::ImageData, 1, {})) == RequestError::InvalidPayload);
    assert(
        tracker.acceptResponse(response(MessageType::ImageData, 1, std::string(maxPayloadSize + 1, 'x'))) ==
        RequestError::InvalidPayload);
    assert(tracker.acceptResponse(response(MessageType::ImageData, 1, "excess")) ==
           RequestError::ImageSizeMismatch);
    assert(!tracker.acceptResponse(response(MessageType::ImageData, 1, "ab")));
    assert(tracker.acceptResponse(response(MessageType::Complete, 1, complete)) ==
           RequestError::ImageSizeMismatch);
    assert(!tracker.acceptResponse(response(MessageType::ImageData, 1, "cde")));
    assert(tracker.acceptResponse(response(MessageType::Complete, 1, failure)) ==
           RequestError::InvalidPayload);
    assert(!tracker.acceptResponse(response(MessageType::Complete, 1, complete)));
    assert(tracker.activeId() == 0 && !tracker.imageInfo());
    assert(tracker.acceptResponse(response(MessageType::ImageData, 1, "x")) == RequestError::NoActiveRequest);

    assert(std::get<std::uint32_t>(tracker.queueCommand(outbound, "dir")) == 2);
    drain(outbound);
    assert(tracker.acceptResponse(response(MessageType::ImageBegin, 2, metadata(1, 2))) ==
           RequestError::UnexpectedType);
    assert(!tracker.acceptResponse(response(MessageType::Complete, 2, complete)));
    assert(std::get<std::uint32_t>(tracker.queueScreenshot(outbound)) == 3);
    drain(outbound);
    assert(!tracker.acceptResponse(response(MessageType::Error, 3, failure)));

    assert(std::get<std::uint32_t>(tracker.queueScreenshot(outbound)) == 4);
    drain(outbound);
    assert(!tracker.acceptResponse(response(MessageType::ImageBegin, 4, metadata(2, 2))));
    assert(tracker.imageInfo()->format == ImageFormat::Bmp);
    assert(!tracker.acceptResponse(response(MessageType::ImageData, 4, "x")));
    assert(!tracker.acceptResponse(response(MessageType::Error, 4, failure)));
    assert(!tracker.imageInfo() && tracker.activeId() == 0);

    assert(std::get<std::uint32_t>(tracker.queueScreenshot(outbound)) == 5);
    drain(outbound);
    assert(!tracker.acceptResponse(
        response(MessageType::ImageBegin, 5, metadata(RequestTracker::maxImageBytes, 2))));
    const std::string chunk(maxPayloadSize, '\0');

    for (std::uint64_t bytes = 0; bytes < RequestTracker::maxImageBytes; bytes += chunk.size()) {
        assert(!tracker.acceptResponse(response(MessageType::ImageData, 5, chunk)));
    }

    assert(tracker.acceptResponse(response(MessageType::ImageData, 5, "x")) ==
           RequestError::ImageSizeMismatch);
    assert(!tracker.acceptResponse(response(MessageType::Complete, 5, complete)));

    RequestTracker limited(1);
    assert(std::get<std::uint32_t>(limited.queueScreenshot(outbound)) == 1);
    drain(outbound);
    assert(!limited.acceptResponse(response(MessageType::Error, 1, failure)));
    assert(std::get<RequestError>(limited.queueCommand(outbound, "dir")) == RequestError::IdExhausted);

    assert(outbound.enqueue({MessageType::ImageData, maxPayloadSize, 8}, chunk));
    const auto remaining = OutboundQueue::capacity - outbound.size() - headerSize;
    assert(outbound.enqueue({MessageType::ImageData, static_cast<std::uint32_t>(remaining), 9},
                            std::string_view(chunk).substr(0, remaining)));
    assert(std::get<RequestError>(tracker.queueScreenshot(outbound)) == RequestError::QueueFull);
    assert(tracker.activeId() == 0 && !tracker.imageInfo());
    drain(outbound);
    assert(std::get<std::uint32_t>(tracker.queueScreenshot(outbound)) == 6);
    drain(outbound);
    assert(!tracker.acceptResponse(response(MessageType::Error, 6, failure)));
}
