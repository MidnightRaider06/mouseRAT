#pragma once

#include "UniqueFd.h"

#include <array>
#include <cstddef>
#include <netinet/in.h>

struct Connection {
    UniqueFd socket;
    sockaddr_in peerAddress{};

    std::array<char, 199> outbound{};
    std::size_t outboundSize = 0;
    std::size_t bytesSent = 0;
};
