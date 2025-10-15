// TCPServer.cpp
#include "TCPServer.h"
#include <cerrno>
#include <system_error>

TCPServer::TCPServer(int portNumber)
    : serverSocket(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)), port(portNumber) {
    if (serverSocket.get() < 0) {
        throw std::system_error(errno, std::generic_category(), "socket");
    } else {
        std::cout << "socket " << serverSocket.get() << " opened!" << std::endl;
    }

    memset(&service, 0, sizeof(service));
    service.sin_family = AF_INET;
    service.sin_port = htons(port);
    service.sin_addr.s_addr = INADDR_ANY;
}


bool TCPServer::bindSocket() {
    if (bind(serverSocket.get(), (struct sockaddr*)&service, sizeof(service)) < 0) {
        std::cerr << "bind() failed: " << std::strerror(errno) << std::endl;
        return false;
    }
    std::cout << "bind() is OK!" << std::endl;
    return true;
}

bool TCPServer::listenSocket() {
    if (listen(serverSocket.get(), 5) < 0) {
        std::cerr << "listen() failed: " << std::strerror(errno) << std::endl;
        return false;
    }
    std::cout << "listen() is OK, waiting for connections…" << std::endl;
    return true;
}

UniqueFd TCPServer::acceptConnection() {
    sockaddr_in clientAddr{};
    socklen_t clientLen = sizeof(clientAddr);
    int fd;
    do {
        fd = accept4(serverSocket.get(), reinterpret_cast<sockaddr*>(&clientAddr),
                     &clientLen, SOCK_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    UniqueFd accepted(fd);
    if (fd < 0) {
        throw std::system_error(errno, std::generic_category(), "accept4");
    }
    char ipStr[INET_ADDRSTRLEN]{};
    if (!inet_ntop(AF_INET, &clientAddr.sin_addr, ipStr, sizeof(ipStr))) {
        throw std::system_error(errno, std::generic_category(), "inet_ntop");
    }
    clientIP = ipStr;
    clientPort = ntohs(clientAddr.sin_port);
    std::cout << "Connection from " << clientIP << ':' << clientPort << std::endl;
    return accepted;
}

std::string TCPServer::getClientIP() { return clientIP; }
int TCPServer::getClientPort() { return clientPort; }
