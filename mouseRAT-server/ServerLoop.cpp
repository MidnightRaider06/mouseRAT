#include "ServerLoop.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <system_error>
#include <sys/epoll.h>

namespace {
constexpr std::uint64_t listenerId = 0;
constexpr std::uint64_t consoleId = std::numeric_limits<std::uint64_t>::max();
constexpr std::size_t maxClients = 64;
constexpr std::string_view marker = " & echo ### END OF OUTPUT ###";

class ConsoleMode {
public:
    ConsoleMode() : flags_(fcntl(STDIN_FILENO, F_GETFL)) {
        if (flags_ < 0 || fcntl(STDIN_FILENO, F_SETFL, flags_ | O_NONBLOCK) < 0) {
            throw std::system_error(errno, std::generic_category(), "nonblocking stdin");
        }
    }

    ~ConsoleMode() { fcntl(STDIN_FILENO, F_SETFL, flags_); }
    ConsoleMode(const ConsoleMode&) = delete;
    ConsoleMode& operator=(const ConsoleMode&) = delete;

private:
    int flags_;
};
}

ServerLoop::ServerLoop(TCPServer& listener)
    : listener_(listener), poller_(epoll_create1(EPOLL_CLOEXEC)) {
    if (poller_.get() < 0) {
        throw std::system_error(errno, std::generic_category(), "epoll_create1");
    }

    watch(EPOLL_CTL_ADD, listener_.descriptor(), EPOLLIN, listenerId);
    watch(EPOLL_CTL_ADD, STDIN_FILENO, EPOLLIN, consoleId);
}

void ServerLoop::watch(int operation, int fd, std::uint32_t events, std::uint64_t id) {
    epoll_event interest{};
    interest.events = events;
    interest.data.u64 = id;

    if (epoll_ctl(poller_.get(), operation, fd, &interest) < 0) {
        throw std::system_error(errno, std::generic_category(), "epoll_ctl");
    }
}

void ServerLoop::run() {
    ConsoleMode consoleMode;
    std::array<epoll_event, 32> events{};
    std::cout << "Controls: :clients, :use <id>, quit" << std::endl;

    while (running_) {
        const int count = epoll_wait(poller_.get(), events.data(), events.size(), -1);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) throw std::system_error(errno, std::generic_category(), "epoll_wait");

        for (int i = 0; i < count && running_; ++i) {
            const auto id = events[i].data.u64;
            if (id == listenerId) acceptReady();
            else if (id == consoleId) readConsole();
            else clientReady(id, events[i].events);
        }
    }
}

void ServerLoop::acceptReady() {
    for (int accepted = 0; accepted < 16; ++accepted) {
        auto client = listener_.acceptConnection();
        if (!client) break;

        if (clients_.size() == maxClients || nextId_ == consoleId) {
            std::cerr << "Client limit reached" << std::endl;
            continue;
        }

        const auto id = nextId_++;
        watch(EPOLL_CTL_ADD, client->socket.get(), EPOLLIN | EPOLLRDHUP, id);
        clients_.emplace(id, std::move(*client));
        if (clients_.size() == 1) selectedId_ = id;
        std::cout << "[client " << id << "] connected" << std::endl;
    }
}

void ServerLoop::disconnect(std::uint64_t id) {
    clients_.erase(id);
    if (selectedId_ == id) selectedId_ = 0;
    std::cout << "[client " << id << "] disconnected" << std::endl;
}

void ServerLoop::clientReady(std::uint64_t id, std::uint32_t events) {
    auto found = clients_.find(id);
    if (found == clients_.end()) return;
    auto& client = found->second;

    if (events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
        std::array<char, 4096> response{};
        ssize_t received;
        do {
            received = recv(client.socket.get(), response.data(), response.size(), 0);
        } while (received < 0 && errno == EINTR);

        if (received > 0) {
            std::cout << "[client " << id << "] ";
            std::cout.write(response.data(), received);
            std::cout << std::flush;

            // The legacy protocol has no reliable response boundary
            if (client.bytesSent == client.outboundSize) {
                client.outboundSize = 0;
                client.bytesSent = 0;
            }
        } else if (received == 0 || (errno != EAGAIN && errno != EWOULDBLOCK) ||
                   (events & (EPOLLHUP | EPOLLERR))) {
            disconnect(id);
            return;
        }
    }

    if ((events & EPOLLOUT) && client.bytesSent < client.outboundSize) {
        const auto sent = send(client.socket.get(), client.outbound.data() + client.bytesSent,
                               client.outboundSize - client.bytesSent, MSG_NOSIGNAL);
        if (sent < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (sent <= 0) {
            disconnect(id);
            return;
        }

        client.bytesSent += static_cast<std::size_t>(sent);
        if (client.bytesSent == client.outboundSize) {
            watch(EPOLL_CTL_MOD, client.socket.get(), EPOLLIN | EPOLLRDHUP, id);
        }
    }
}

void ServerLoop::readConsole() {
    std::array<char, 256> input{};
    const auto received = read(STDIN_FILENO, input.data(), input.size());
    if (received < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) return;
    if (received < 0) throw std::system_error(errno, std::generic_category(), "read stdin");
    if (received == 0) {
        running_ = false;
        return;
    }

    for (ssize_t i = 0; i < received && running_; ++i) {
        if (input[i] == '\n') {
            if (discardLine_) std::cerr << "Input line too long" << std::endl;
            else handleCommand(std::string_view(line_.data(), lineSize_));
            lineSize_ = 0;
            discardLine_ = false;
        } else if (!discardLine_) {
            if (lineSize_ == line_.size()) discardLine_ = true;
            else line_[lineSize_++] = input[i];
        }
    }
}

void ServerLoop::handleCommand(std::string_view command) {
    if (!command.empty() && command.back() == '\r') command.remove_suffix(1);
    if (command.empty()) return;
    if (command == "quit") {
        running_ = false;
        return;
    }

    if (command == ":clients") {
        for (const auto& [id, client] : clients_) {
            char address[INET_ADDRSTRLEN]{};
            inet_ntop(AF_INET, &client.peerAddress.sin_addr, address, sizeof(address));
            std::cout << "[client " << id << "] " << address << ':'
                      << ntohs(client.peerAddress.sin_port)
                      << (id == selectedId_ ? " selected" : "") << std::endl;
        }
        return;
    }

    if (command.substr(0, 5) == ":use ") {
        std::uint64_t id = 0;
        const auto result = std::from_chars(command.data() + 5, command.data() + command.size(), id);
        if (result.ec != std::errc{} || result.ptr != command.data() + command.size() ||
            clients_.find(id) == clients_.end()) {
            std::cerr << "Unknown client ID" << std::endl;
        } else {
            selectedId_ = id;
            std::cout << "Selected client " << id << std::endl;
        }
        return;
    }

    if (command.front() == ':') {
        std::cerr << "Unknown console control" << std::endl;
        return;
    }
    if (command.substr(0, 10) == "screenshot") {
        std::cerr << "Screenshots are not implemented" << std::endl;
        return;
    }

    auto found = clients_.find(selectedId_);
    if (found == clients_.end()) {
        std::cerr << "No client selected" << std::endl;
        return;
    }

    auto& client = found->second;
    if (command.size() > client.outbound.size() - marker.size() ||
        command.find('\0') != std::string_view::npos) {
        std::cerr << "Command exceeds the legacy limit or contains a NUL byte" << std::endl;
        return;
    }
    if (client.outboundSize != 0) {
        std::cerr << "Selected client still has a pending command" << std::endl;
        return;
    }

    std::copy(command.begin(), command.end(), client.outbound.begin());
    std::copy(marker.begin(), marker.end(), client.outbound.begin() + command.size());
    client.outboundSize = command.size() + marker.size();
    client.bytesSent = 0;
    watch(EPOLL_CTL_MOD, client.socket.get(), EPOLLIN | EPOLLRDHUP | EPOLLOUT, selectedId_);
}
