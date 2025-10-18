#pragma once

#include "UniqueFd.h"

#include <netinet/in.h>

struct Connection {
    UniqueFd socket;
    sockaddr_in peerAddress{};
};
