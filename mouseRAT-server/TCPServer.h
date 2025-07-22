// TCPServer.h
#pragma once

#include <iostream>
#include <cstring>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>

class TCPServer {
private:
    int serverSocket;
    int port;
    sockaddr_in service;

    std::string clientIP;
    int clientPort;

public:
    TCPServer(int portNumber);
    ~TCPServer();
    bool bindSocket();
    bool listenSocket();
    int acceptConnection();
    void closeSocket();
    std::string getClientIP();
    int getClientPort();
};
