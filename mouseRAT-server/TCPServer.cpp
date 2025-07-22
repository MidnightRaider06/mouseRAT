// TCPServer.cpp
#include "TCPServer.h"
#include <sstream>

TCPServer::TCPServer(int portNumber) : port(portNumber) {
    serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket < 0) {
        std::cerr << "Unable to open socket" << std::endl;
        exit(EXIT_FAILURE);
    } else {
        std::cout << "socket " << serverSocket << " opened!" << std::endl;
    }

    memset(&service, 0, sizeof(service));
    service.sin_family = AF_INET;
    service.sin_port = htons(port);
    service.sin_addr.s_addr = INADDR_ANY;
}

TCPServer::~TCPServer() {
    closeSocket();
}

bool TCPServer::bindSocket() {
    if (bind(serverSocket, (struct sockaddr*)&service, sizeof(service)) < 0) {
        std::cerr << "bind() failed" << std::endl;
        return false;
    }
    std::cout << "bind() is OK!" << std::endl;
    return true;
}

bool TCPServer::listenSocket() {
    if (listen(serverSocket, 5) < 0) {
        std::cerr << "listen() failed" << std::endl;
        return false;
    }
    std::cout << "listen() is OK, waiting for connections…" << std::endl;
    return true;
}

#include <sstream> // For stringstream if needed

int TCPServer::acceptConnection() {
    sockaddr_in clientAddr;
    socklen_t clientLen = sizeof(clientAddr);

    int acceptSocket = accept(serverSocket, (struct sockaddr*)&clientAddr, &clientLen);
    if (acceptSocket < 0) {
        std::cerr << "accept() failed" << std::endl;
    } else {
        char ipStr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, ipStr, INET_ADDRSTRLEN);
        clientIP = ipStr; // store in class member variable
        clientPort = ntohs(clientAddr.sin_port); // store in class member variable

        std::cout << "accept() is OK! Connection from: "
                  << clientIP << ":" << clientPort << std::endl;
    }
    return acceptSocket;
}


void TCPServer::closeSocket() {
    if (serverSocket >= 0) {
        shutdown(serverSocket, SHUT_RDWR);
        close(serverSocket);
        serverSocket = -1;
        std::cout << "Socket closed." << std::endl;
    }
}


std::string TCPServer::getClientIP() {
    return clientIP;
}

int TCPServer::getClientPort() {
    return clientPort;
}