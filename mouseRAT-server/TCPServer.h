// TCPServer.h
#pragma once

#include "Connection.h"

#include <optional>
#include <iostream>
#include <cstring>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>

class TCPServer {
private:
    UniqueFd serverSocket;
    int port;
    sockaddr_in service;

public:
    TCPServer(int portNumber);
    ~TCPServer() = default;

    bool bindSocket();
    bool listenSocket();

    [[nodiscard]] int descriptor() const noexcept {
        return serverSocket.get();
    }

    std::optional<Connection> acceptConnection();
};
