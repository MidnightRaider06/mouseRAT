#pragma once

#include "UniqueFd.h"
#include "RequestTracker.h"

#include <netinet/in.h>

struct Connection {
    UniqueFd socket;
    sockaddr_in peerAddress{};

    protocol::FrameDecoder decoder{};
    protocol::OutboundQueue outbound{};
    protocol::RequestTracker requests{};
    bool helloReceived = false;
};
