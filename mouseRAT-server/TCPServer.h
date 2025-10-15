// TCPServer.h
#pragma once

#include "UniqueFd.h"
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

    std::string clientIP;
    int clientPort = 0;

public:
    TCPServer(int portNumber);
    ~TCPServer() = default;
    bool bindSocket();
    bool listenSocket();
    UniqueFd acceptConnection();
    std::string getClientIP();
    int getClientPort();
};
