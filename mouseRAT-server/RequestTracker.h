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
};

class RequestTracker {
public:
    static constexpr std::size_t maxOutputBytes = 16 * 1024 * 1024;

    explicit RequestTracker(std::uint32_t requestLimit = std::numeric_limits<std::uint32_t>::max())
        noexcept : requestLimit_(requestLimit) {}

    [[nodiscard]] std::variant<std::uint32_t, RequestError>
    queueCommand(OutboundQueue& outbound, std::string_view command);
    [[nodiscard]] std::optional<RequestError> acceptResponse(const FrameView& frame) noexcept;
    [[nodiscard]] std::uint32_t activeId() const noexcept { return activeId_; }

private:
    std::uint64_t nextId_ = 1;
    std::uint32_t requestLimit_;
    std::uint32_t activeId_ = 0;
    std::size_t outputBytes_ = 0;
};
}
