#include "RequestTracker.h"

namespace protocol {
namespace {
std::uint64_t readInteger(std::string_view bytes) noexcept {
    std::uint64_t value = 0;

    for (unsigned char byte : bytes) {
        value = (value << 8) | byte;
    }

    return value;
}
}

std::variant<std::uint32_t, RequestError> RequestTracker::queueCommand(OutboundQueue& outbound,
                                                                       std::string_view command) {
    return queueRequest(outbound, MessageType::Command, command);
}

std::variant<std::uint32_t, RequestError> RequestTracker::queueScreenshot(OutboundQueue& outbound) {
    return queueRequest(outbound, MessageType::Screenshot, {});
}

std::variant<std::uint32_t, RequestError>
RequestTracker::queueRequest(OutboundQueue& outbound, MessageType type, std::string_view payload) {
    if (activeId_ != 0) {
        return RequestError::Busy;
    }

    if (nextId_ > requestLimit_) {
        return RequestError::IdExhausted;
    }

    if (type == MessageType::Command &&
        (payload.empty() || payload.size() > 4096 || payload.find('\0') != std::string_view::npos ||
         !isValidUtf8(payload))) {
        return RequestError::InvalidCommand;
    }

    const auto id = static_cast<std::uint32_t>(nextId_);
    const Header header{type, static_cast<std::uint32_t>(payload.size()), id};

    if (!outbound.enqueue(header, payload)) {
        return RequestError::QueueFull;
    }

    activeId_ = id;
    activeType_ = type;
    ++nextId_;
    receivedBytes_ = 0;
    image_.reset();
    return id;
}

std::optional<RequestError> RequestTracker::acceptResponse(const FrameView& frame) noexcept {
    if (activeId_ == 0) {
        return RequestError::NoActiveRequest;
    }

    if (frame.header.requestId != activeId_) {
        return RequestError::WrongRequestId;
    }

    if (frame.header.payloadLength != frame.payload.size()) {
        return RequestError::InvalidPayload;
    }

    switch (frame.header.type) {
        case MessageType::Output:
            if (activeType_ != MessageType::Command) {
                return RequestError::UnexpectedType;
            }

            if (frame.payload.size() < 2 || frame.payload.size() > maxPayloadSize ||
                static_cast<unsigned char>(frame.payload.front()) > 2) {
                return RequestError::InvalidPayload;
            }

            if (frame.payload.size() - 1 > maxOutputBytes - receivedBytes_) {
                return RequestError::OutputLimitExceeded;
            }

            receivedBytes_ += frame.payload.size() - 1;
            return std::nullopt;

        case MessageType::ImageBegin: {
            if (activeType_ != MessageType::Screenshot || image_) {
                return RequestError::UnexpectedType;
            }

            if (frame.payload.size() != 12) {
                return RequestError::InvalidPayload;
            }

            const auto total = readInteger(frame.payload.substr(0, 8));
            const auto format = readInteger(frame.payload.substr(8, 4));

            if (total == 0 || (format != 1 && format != 2)) {
                return RequestError::InvalidPayload;
            }

            if (total > maxImageBytes) {
                return RequestError::ImageLimitExceeded;
            }

            image_ = ImageInfo{total, static_cast<ImageFormat>(format)};
            return std::nullopt;
        }

        case MessageType::ImageData:
            if (activeType_ != MessageType::Screenshot || !image_) {
                return RequestError::UnexpectedType;
            }

            if (frame.payload.empty() || frame.payload.size() > maxPayloadSize) {
                return RequestError::InvalidPayload;
            }

            if (frame.payload.size() > image_->totalBytes - receivedBytes_) {
                return RequestError::ImageSizeMismatch;
            }

            receivedBytes_ += frame.payload.size();
            return std::nullopt;

        case MessageType::Complete:
            if (frame.payload.size() != 4) {
                return RequestError::InvalidPayload;
            }

            if (activeType_ == MessageType::Screenshot) {
                if (!image_) {
                    return RequestError::UnexpectedType;
                }

                if (readInteger(frame.payload) != 0) {
                    return RequestError::InvalidPayload;
                }

                if (receivedBytes_ != image_->totalBytes) {
                    return RequestError::ImageSizeMismatch;
                }
            }

            break;

        case MessageType::Error: {
            if (frame.payload.size() < 4 || frame.payload.size() > 1028) {
                return RequestError::InvalidPayload;
            }

            const auto code = readInteger(frame.payload.substr(0, 4));

            if (code < 1 || code > 4 || !isValidUtf8(frame.payload.substr(4))) {
                return RequestError::InvalidPayload;
            }

            break;
        }

        default:
            return RequestError::UnexpectedType;
    }

    activeId_ = 0;
    receivedBytes_ = 0;
    image_.reset();
    return std::nullopt;
}
}
