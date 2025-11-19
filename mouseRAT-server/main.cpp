#include "ServerLoop.h"

#include <iostream>

int main() {
    try {
        TCPServer server(5555);

        if (!server.bindSocket() || !server.listenSocket()) {
            return 1;
        }

        ServerLoop loop(server);
        loop.run();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Server: " << error.what() << '\n';
        return 1;
    }
}
