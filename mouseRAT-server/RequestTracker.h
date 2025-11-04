#pragma once

#include "FrameDecoder.h"
#include "OutboundQueue.h"

#include <limits>

namespace protocol {
enum class RequestError {
    Busy,
    IdExhausted,
    InvalidCommand,
    QueueFull,
    NoActiveRequest,
    WrongRequestId,
    UnexpectedType,
    InvalidPayload,
    OutputLimitExceeded,
    ImageLimitExceeded,
    ImageSizeMismatch,
};

struct ImageInfo {
    std::uint64_t totalBytes;
    ImageFormat format;
};

class RequestTracker {
public:
    static constexpr std::size_t maxOutputBytes = 16 * 1024 * 1024;
    static constexpr std::uint64_t maxImageBytes = 32 * 1024 * 1024;

    explicit RequestTracker(std::uint32_t requestLimit = std::numeric_limits<std::uint32_t>::max())
        noexcept : requestLimit_(requestLimit) {}

    [[nodiscard]] std::variant<std::uint32_t, RequestError>
    queueCommand(OutboundQueue& outbound, std::string_view command);
    [[nodiscard]] std::variant<std::uint32_t, RequestError> queueScreenshot(OutboundQueue& outbound);
    [[nodiscard]] std::optional<RequestError> acceptResponse(const FrameView& frame) noexcept;
    [[nodiscard]] std::uint32_t activeId() const noexcept { return activeId_; }
    [[nodiscard]] std::optional<ImageInfo> imageInfo() const noexcept { return image_; }

private:
    std::variant<std::uint32_t, RequestError>
    queueRequest(OutboundQueue& outbound, MessageType type, std::string_view payload);

    std::uint64_t nextId_ = 1;
    std::uint32_t requestLimit_;
    std::uint32_t activeId_ = 0;
    MessageType activeType_ = MessageType::Command;
    std::uint64_t receivedBytes_ = 0;
    std::optional<ImageInfo> image_;
};
}
