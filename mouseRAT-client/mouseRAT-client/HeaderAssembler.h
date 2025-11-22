#pragma once

#include "Protocol.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string_view>

namespace protocol {

class HeaderAssembler {
public:
    std::optional<Header> consume(std::string_view input, std::size_t& consumed) {
        consumed = 0;
        if (failed_) {
            throw std::logic_error("Header assembler belongs to a failed session");
        }

        const auto count = (std::min)(bytes_.size() - used_, input.size());
        if (count != 0) {
            std::copy_n(input.data(), count, bytes_.data() + used_);
        }

        used_ += count;
        consumed = count;
        if (used_ != bytes_.size()) {
            return std::nullopt;
        }

        try {
            const auto header = decodeHeader(bytes_);
            used_ = 0;
            return header;
        } catch (const std::invalid_argument&) {
            failed_ = true;
            throw;
        }
    }

    bool hasPartialHeader() const {
        return !failed_ && used_ != 0;
    }

private:
    std::array<std::uint8_t, 16> bytes_{};
    std::size_t used_ = 0;
    bool failed_ = false;
};

}
