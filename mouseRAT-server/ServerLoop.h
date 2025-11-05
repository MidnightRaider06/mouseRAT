#pragma once

#include "TCPServer.h"

#include <array>
#include <cstdint>
#include <map>
#include <string_view>

class ServerLoop {
public:
    explicit ServerLoop(TCPServer& listener);
    void run();

private:
    void watch(int operation, int fd, std::uint32_t events, std::uint64_t id);
    void acceptReady();
    void clientReady(std::uint64_t id, std::uint32_t events);
    bool processFrame(std::uint64_t id, Connection& client, const protocol::FrameView& frame);
    void readConsole();
    void handleCommand(std::string_view command);
    void disconnect(std::uint64_t id);

    TCPServer& listener_;
    UniqueFd poller_;
    std::map<std::uint64_t, Connection> clients_;
    std::uint64_t nextId_ = 1;
    std::uint64_t selectedId_ = 0;
    bool running_ = true;

    std::array<char, 4096> line_{};
    std::size_t lineSize_ = 0;
    bool discardLine_ = false;
};
