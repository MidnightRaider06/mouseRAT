#pragma once

#include "OutboundQueue.h"

#include <chrono>
#include <system_error>

namespace protocol {

// Call immediately after connect with a nonblocking socket
// The caller closes the socket if the handshake throws
inline void performHandshake(SOCKET socket, std::chrono::steady_clock::time_point connectedAt) {
    using Clock = std::chrono::steady_clock;
    const auto deadline = connectedAt + std::chrono::seconds(5);
    OutboundQueue outbound;
    if (!outbound.enqueue({ 0x01, 0, 0 }, {})) {
        throw std::logic_error("Cannot queue HELLO");
    }

    std::array<std::uint8_t, 16> acknowledgment{};
    std::size_t used = 0;
    while (used != acknowledgment.size()) {
        const auto now = Clock::now();
        if (now >= deadline) {
            throw std::runtime_error("Handshake timeout");
        }

        const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now).count();
        timeval timeout{ static_cast<long>(remaining / 1000000), static_cast<long>(remaining % 1000000) };
        const bool sending = !outbound.empty();
        fd_set ready{};
        FD_SET(socket, &ready);
        const auto selected = select(0, sending ? nullptr : &ready, sending ? &ready : nullptr, nullptr, &timeout);
        if (selected == SOCKET_ERROR) {
            const auto error = WSAGetLastError();
            throw std::system_error(error, std::system_category(), "Handshake select");
        }

        if (selected == 0) {
            continue;
        }

        if (sending) {
            (void)outbound.sendTo(socket);
            continue;
        }

        const auto received = recv(socket, reinterpret_cast<char*>(acknowledgment.data()) + used,
            static_cast<int>(acknowledgment.size() - used), 0);
        if (received == SOCKET_ERROR) {
            const auto error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK) {
                continue;
            }

            throw std::system_error(error, std::system_category(), "Handshake recv");
        }

        if (received == 0) {
            throw std::runtime_error("Disconnected before HELLO_ACK");
        }

        used += static_cast<std::size_t>(received);
    }

    if (Clock::now() >= deadline) {
        throw std::runtime_error("Handshake timeout");
    }

    if (decodeHeader(acknowledgment).type != 0x02) {
        throw std::invalid_argument("Expected HELLO_ACK");
    }
}

}
