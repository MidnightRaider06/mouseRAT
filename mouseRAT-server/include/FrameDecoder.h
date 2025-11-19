#pragma once

#include "Protocol.h"

#include <optional>
#include <string_view>

namespace protocol {
struct FrameView {
    Header header;

    // Borrows decoder storage until the next consume call
    std::string_view payload;
};

struct DecodeStep {
    std::size_t consumed;
    std::variant<std::monostate, FrameView, HeaderError> result;
};

class FrameDecoder {
public:
    [[nodiscard]] DecodeStep consume(std::string_view input) noexcept;
    [[nodiscard]] bool hasPartialFrame() const noexcept;

private:
    HeaderBytes headerBytes_{};
    std::size_t headerUsed_ = 0;
    std::optional<Header> header_;

    std::array<char, maxPayloadSize> payload_{};
    std::size_t payloadUsed_ = 0;
    std::optional<HeaderError> error_;
};
}
