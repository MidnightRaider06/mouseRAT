#pragma once

#include "UniqueFd.h"
#include "RequestTracker.h"

#include <array>
#include <chrono>
#include <netinet/in.h>
#include <optional>

struct Connection {
    using Clock = std::chrono::steady_clock;

    UniqueFd socket;
    sockaddr_in peerAddress{};

    protocol::FrameDecoder decoder{};
    protocol::OutboundQueue outbound{};
    protocol::RequestTracker requests{};
    bool helloReceived = false;

    std::array<char, 4096> input{};
    std::size_t inputOffset = 0;
    std::size_t inputSize = 0;
    Clock::time_point inputAt{};
    std::optional<protocol::FrameView> pendingDisk{};
    std::uint64_t transferId = 0;
    std::uint32_t screenshotRequest = 0;

    Clock::time_point acceptedAt = Clock::now();
    std::optional<Clock::time_point> frameStarted{};
    std::optional<Clock::time_point> requestDeadline{};
    Clock::time_point lastFrameAt = acceptedAt;
    std::optional<Clock::time_point> pongDeadline{};
    std::array<char, 8> pingNonce{};
    std::uint64_t pingSequence = 0;

    [[nodiscard]] std::optional<Clock::time_point> nextDeadline() const noexcept {
        std::optional<Clock::time_point> deadline;
        if (!helloReceived) deadline = acceptedAt + std::chrono::seconds(5);
        if (frameStarted) {
            const auto frameDeadline = *frameStarted + std::chrono::seconds(30);
            if (!deadline || frameDeadline < *deadline) deadline = frameDeadline;
        }
        if (requestDeadline && (!deadline || *requestDeadline < *deadline)) {
            deadline = requestDeadline;
        }
        if (pongDeadline && (!deadline || *pongDeadline < *deadline)) deadline = pongDeadline;
        return deadline;
    }

    [[nodiscard]] std::optional<Clock::time_point> nextWakeup() const noexcept {
        auto deadline = nextDeadline();
        if (helloReceived && !pongDeadline) {
            const auto pingAt = lastFrameAt + std::chrono::seconds(10);
            if (!deadline || pingAt < *deadline) deadline = pingAt;
        }
        return deadline;
    }
};
