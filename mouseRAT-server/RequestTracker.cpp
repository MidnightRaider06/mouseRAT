#include "RequestTracker.h"

namespace protocol {
std::variant<std::uint32_t, RequestError>
RequestTracker::queueCommand(OutboundQueue& outbound, std::string_view command) {
    if (activeId_ != 0) return RequestError::Busy;
    if (nextId_ > requestLimit_) return RequestError::IdExhausted;
    if (command.empty() || command.size() > 4096 || command.find('\0') != std::string_view::npos) {
        return RequestError::InvalidCommand;
    }

    const auto id = static_cast<std::uint32_t>(nextId_);
    const Header header{MessageType::Command, static_cast<std::uint32_t>(command.size()), id};
    if (!outbound.enqueue(header, command)) return RequestError::QueueFull;

    activeId_ = id;
    ++nextId_;
    outputBytes_ = 0;
    return id;
}

std::optional<RequestError> RequestTracker::acceptResponse(const FrameView& frame) noexcept {
    if (activeId_ == 0) return RequestError::NoActiveRequest;
    if (frame.header.requestId != activeId_) return RequestError::WrongRequestId;
    if (frame.header.payloadLength != frame.payload.size()) return RequestError::InvalidPayload;

    switch (frame.header.type) {
    case MessageType::Output:
        if (frame.payload.size() < 2 || frame.payload.size() > maxPayloadSize ||
            static_cast<unsigned char>(frame.payload.front()) > 2) {
            return RequestError::InvalidPayload;
        }
        if (frame.payload.size() - 1 > maxOutputBytes - outputBytes_) {
            return RequestError::OutputLimitExceeded;
        }
        outputBytes_ += frame.payload.size() - 1;
        return std::nullopt;

    case MessageType::Complete:
        if (frame.payload.size() != 4) return RequestError::InvalidPayload;
        break;

    case MessageType::Error:
        if (frame.payload.size() < 4 || frame.payload.size() > 1028) {
            return RequestError::InvalidPayload;
        }
        break;

    default:
        return RequestError::UnexpectedType;
    }

    activeId_ = 0;
    outputBytes_ = 0;
    return std::nullopt;
}
}
