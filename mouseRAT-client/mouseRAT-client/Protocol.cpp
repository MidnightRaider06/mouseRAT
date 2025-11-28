#include "Protocol.h"

#include <Windows.h>

namespace protocol {
bool isValidCommandText(std::string_view text) {
    return !text.empty() && text.size() <= 4096
        && text.find('\0') == std::string_view::npos
        && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
            static_cast<int>(text.size()), nullptr, 0) > 0;
}
}
