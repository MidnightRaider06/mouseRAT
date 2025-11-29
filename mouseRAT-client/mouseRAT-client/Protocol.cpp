#include "Protocol.h"

#include <limits>
#include <Windows.h>

namespace protocol {
bool isValidCommandText(std::string_view text) {
    return !text.empty() && text.size() <= 4096
        && text.find('\0') == std::string_view::npos
        && isValidUtf8(text);
}

bool isValidUtf8(std::string_view text) {
    return text.empty() || (text.size() <= static_cast<std::size_t>((std::numeric_limits<int>::max)())
        && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
            static_cast<int>(text.size()), nullptr, 0) > 0);
}

void validateDirection(const Header& header, Direction direction) {
    const bool bidirectional = header.type == 0x30 || header.type == 0x31;
    const bool fromServer = header.type == 0x02 || header.type == 0x10 || header.type == 0x20;
    const bool fromClient = header.type == 0x01 || header.type == 0x11 || header.type == 0x12
        || header.type == 0x21 || header.type == 0x22 || header.type == 0x7f;
    if (!bidirectional && !(direction == Direction::FromServer ? fromServer : fromClient)) {
        throw std::invalid_argument("Invalid message direction");
    }
}
}
