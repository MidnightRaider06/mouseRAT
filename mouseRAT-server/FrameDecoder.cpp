#include "FrameDecoder.h"

#include <algorithm>

namespace protocol {
DecodeStep FrameDecoder::consume(std::string_view input) noexcept {
    if (error_) {
        return {0, *error_};
    }

    if (input.empty()) {
        return {0, std::monostate{}};
    }

    std::size_t consumed = 0;

    if (!header_) {
        const auto count = std::min(headerSize - headerUsed_, input.size());
        std::copy_n(input.data(), count, headerBytes_.data() + headerUsed_);
        headerUsed_ += count;
        consumed += count;

        if (headerUsed_ != headerSize) {
            return {consumed, std::monostate{}};
        }

        const auto decoded = decodeHeader(headerBytes_);

        if (const auto error = std::get_if<HeaderError>(&decoded)) {
            error_ = *error;
            return {consumed, *error_};
        }

        header_ = std::get<Header>(decoded);
    }

    const auto count = std::min(header_->payloadLength - payloadUsed_, input.size() - consumed);

    if (count > 0) {
        std::copy_n(input.data() + consumed, count, payload_.data() + payloadUsed_);
    }

    payloadUsed_ += count;
    consumed += count;

    if (payloadUsed_ != header_->payloadLength) {
        return {consumed, std::monostate{}};
    }

    const FrameView frame{*header_, std::string_view(payload_.data(), payloadUsed_)};
    headerUsed_ = 0;
    header_.reset();
    payloadUsed_ = 0;
    return {consumed, frame};
}

bool FrameDecoder::hasPartialFrame() const noexcept {
    return !error_ && headerUsed_ != 0;
}
}
