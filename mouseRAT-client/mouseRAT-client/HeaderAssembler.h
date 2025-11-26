#pragma once

#include "Protocol.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string_view>

namespace protocol {

class HeaderAssembler {
public:
    explicit HeaderAssembler(Direction direction = Direction::FromServer) : direction_(direction) {
    }

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
            const bool bidirectional = header.type == 0x30 || header.type == 0x31;
            const bool fromServer = header.type == 0x02 || header.type == 0x10 || header.type == 0x20;
            if (!bidirectional && ((direction_ == Direction::FromServer) != fromServer)) {
                throw std::invalid_argument("Invalid message direction");
            }

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
    Direction direction_;
    std::array<std::uint8_t, 16> bytes_{};
    std::size_t used_ = 0;
    bool failed_ = false;
};

}
