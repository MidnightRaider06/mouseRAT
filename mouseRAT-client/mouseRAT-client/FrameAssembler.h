#pragma once

#include "HeaderAssembler.h"

#include <algorithm>
#include <array>
#include <optional>
#include <string_view>

namespace protocol {

struct FrameView {
    Header header;
    // Borrows assembler storage until the next consume call
    std::string_view payload;
};

class FrameAssembler {
public:
    explicit FrameAssembler(Direction direction = Direction::FromServer) : headers_(direction) {
    }

    std::optional<FrameView> consume(std::string_view input, std::size_t& consumed) {
        consumed = 0;
        if (!header_) {
            header_ = headers_.consume(input, consumed);
            if (!header_) {
                return std::nullopt;
            }
        }

        const auto count = (std::min)(header_->payloadLength - used_, input.size() - consumed);
        if (count != 0) {
            std::copy_n(input.data() + consumed, count, payload_.data() + used_);
        }

        used_ += count;
        consumed += count;
        if (used_ != header_->payloadLength) {
            return std::nullopt;
        }

        const FrameView frame{ *header_, { payload_.data(), used_ } };
        header_.reset();
        used_ = 0;
        return frame;
    }

    bool hasPartialFrame() const {
        return header_.has_value() || headers_.hasPartialHeader();
    }

private:
    HeaderAssembler headers_;
    std::optional<Header> header_;
    std::array<char, 65536> payload_{};
    std::size_t used_ = 0;
};

}
