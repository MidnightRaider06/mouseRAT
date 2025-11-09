#include "ServerLoop.h"

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

std::uint32_t readStatus(std::string_view payload) noexcept {
    std::uint32_t value = 0;
    for (unsigned char byte : payload.substr(0, 4)) value = (value << 8) | byte;
    return value;
}

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
        expireClients();
        sendHeartbeats();
        const int count = epoll_wait(poller_.get(), events.data(), events.size(), waitTimeout());
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) throw std::system_error(errno, std::generic_category(), "epoll_wait");

        expireClients();
        for (int i = 0; i < count && running_; ++i) {
            const auto id = events[i].data.u64;
            if (id == listenerId) acceptReady();
            else if (id == consoleId) readConsole();
            else clientReady(id, events[i].events);
        }
    }
}

int ServerLoop::waitTimeout() const {
    std::optional<Connection::Clock::time_point> earliest;
    for (const auto& [id, client] : clients_) {
        const auto deadline = client.nextWakeup();
        if (deadline && (!earliest || *deadline < *earliest)) earliest = deadline;
    }
    if (!earliest) return -1;

    const auto remaining = *earliest - Connection::Clock::now();
    if (remaining <= Connection::Clock::duration::zero()) return 0;
    return static_cast<int>(std::chrono::ceil<std::chrono::milliseconds>(remaining).count());
}

void ServerLoop::expireClients() {
    const auto now = Connection::Clock::now();
    for (auto it = clients_.begin(); it != clients_.end();) {
        const auto current = it++;
        const auto deadline = current->second.nextDeadline();
        if (deadline && now >= *deadline) {
            const auto id = current->first;
            const auto& client = current->second;
            const char* reason = "frame timeout";
            if (!client.helloReceived) reason = "handshake timeout";
            else if (client.requestDeadline == deadline) reason = "request timeout";
            else if (client.pongDeadline == deadline) reason = "heartbeat timeout";
            std::cerr << "[client " << id << "] " << reason << std::endl;
            disconnect(id);
        }
    }
}

void ServerLoop::sendHeartbeats() {
    const auto now = Connection::Clock::now();
    for (auto it = clients_.begin(); it != clients_.end();) {
        const auto current = it++;
        auto& client = current->second;
        if (!client.helloReceived || client.pongDeadline ||
            now < client.lastFrameAt + std::chrono::seconds(10)) continue;

        const auto id = current->first;
        if (client.pingSequence == std::numeric_limits<std::uint64_t>::max()) {
            std::cerr << "[client " << id << "] heartbeat nonce exhausted" << std::endl;
            disconnect(id);
            continue;
        }

        const auto sequence = client.pingSequence + 1;
        std::array<char, 8> nonce{};
        for (std::size_t i = 0; i < nonce.size(); ++i) {
            nonce[i] = static_cast<char>(sequence >> (56 - 8 * i));
        }
        const bool wasEmpty = client.outbound.empty();
        if (!client.outbound.enqueue({protocol::MessageType::Ping, 8, 0},
                                     std::string_view(nonce.data(), nonce.size()))) {
            std::cerr << "[client " << id << "] heartbeat queue full" << std::endl;
            disconnect(id);
            continue;
        }

        client.pingSequence = sequence;
        client.pingNonce = nonce;
        client.pongDeadline = Connection::Clock::now() + std::chrono::seconds(30);
        if (wasEmpty) watch(EPOLL_CTL_MOD, client.socket.get(), EPOLLIN | EPOLLRDHUP | EPOLLOUT, id);
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
    const auto found = clients_.find(id);
    if (found != clients_.end() && found->second.requests.activeId() != 0) {
        std::cerr << "[client " << id << " request " << found->second.requests.activeId()
                  << "] interrupted" << std::endl;
    }
    clients_.erase(id);
    if (selectedId_ == id) selectedId_ = 0;
    std::cout << "[client " << id << "] disconnected" << std::endl;
}

void ServerLoop::clientReady(std::uint64_t id, std::uint32_t events) {
    auto found = clients_.find(id);
    if (found == clients_.end()) return;
    auto& client = found->second;
    if (const auto deadline = client.nextDeadline(); deadline && Connection::Clock::now() >= *deadline) {
        expireClients();
        return;
    }
    const bool hadPending = !client.outbound.empty();

    if (events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
        std::array<char, 4096> response{};
        ssize_t received;
        do {
            received = recv(client.socket.get(), response.data(), response.size(), 0);
        } while (received < 0 && errno == EINTR);

        if (received > 0) {
            const auto receivedAt = Connection::Clock::now();
            std::string_view input(response.data(), static_cast<std::size_t>(received));
            while (!input.empty()) {
                if (!client.decoder.hasPartialFrame()) client.frameStarted = receivedAt;
                if (const auto deadline = client.nextDeadline();
                    deadline && Connection::Clock::now() >= *deadline) {
                    expireClients();
                    return;
                }
                const auto step = client.decoder.consume(input);
                input.remove_prefix(step.consumed);
                const auto frame = std::get_if<protocol::FrameView>(&step.result);
                if (frame) client.frameStarted.reset();
                if (std::holds_alternative<protocol::HeaderError>(step.result) ||
                    (frame && !processFrame(id, client, *frame))) {
                    std::cerr << "[client " << id << "] protocol violation" << std::endl;
                    disconnect(id);
                    return;
                }
                if (!frame) break;
                client.lastFrameAt = receivedAt;
            }
        } else if (received == 0 || (errno != EAGAIN && errno != EWOULDBLOCK) ||
                   (events & (EPOLLHUP | EPOLLERR))) {
            if (client.decoder.hasPartialFrame()) {
                std::cerr << "[client " << id << "] truncated frame" << std::endl;
            }
            disconnect(id);
            return;
        }
    }

    if ((events & EPOLLOUT) && !client.outbound.empty()) {
        const auto pending = client.outbound.pending();
        const auto sent = send(client.socket.get(), pending.data(), pending.size(), MSG_NOSIGNAL);
        if (sent > 0) {
            client.outbound.advance(static_cast<std::size_t>(sent));
        } else if (sent == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
            disconnect(id);
            return;
        }
    }

    if (hadPending != !client.outbound.empty()) {
        watch(EPOLL_CTL_MOD, client.socket.get(), EPOLLIN | EPOLLRDHUP |
              (client.outbound.empty() ? 0u : EPOLLOUT), id);
    }
}

bool ServerLoop::processFrame(std::uint64_t id, Connection& client, const protocol::FrameView& frame) {
    using protocol::MessageType;
    if (!client.helloReceived) {
        if (frame.header.type != MessageType::Hello) return false;
        if (!client.outbound.enqueue({MessageType::HelloAck, 0, 0}, {})) return false;

        client.helloReceived = true;
        std::cout << "[client " << id << "] ready" << std::endl;
        return true;
    }

    if (frame.header.type == MessageType::Ping) {
        return client.outbound.enqueue({MessageType::Pong, 8, 0}, frame.payload);
    }
    if (frame.header.type == MessageType::Pong) {
        if (!client.pongDeadline ||
            frame.payload != std::string_view(client.pingNonce.data(), client.pingNonce.size())) {
            return false;
        }
        client.pongDeadline.reset();
        return true;
    }
    if (frame.header.type != MessageType::Output && frame.header.type != MessageType::Complete &&
        frame.header.type != MessageType::Error) return false;
    if (client.requests.acceptResponse(frame)) return false;
    if (client.requests.activeId() == 0) client.requestDeadline.reset();

    std::cout << "[client " << id << " request " << frame.header.requestId << "] ";
    if (frame.header.type == MessageType::Output) {
        std::cout.write(frame.payload.data() + 1, frame.payload.size() - 1);
        std::cout << std::flush;
    } else if (frame.header.type == MessageType::Complete) {
        std::cout << "complete (exit " << readStatus(frame.payload) << ')' << std::endl;
    } else {
        std::cout << "error " << readStatus(frame.payload) << ": ";
        std::cout.write(frame.payload.data() + 4, frame.payload.size() - 4);
        std::cout << std::endl;
    }
    return true;
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
                      << (client.helloReceived ? " ready" : " awaiting HELLO")
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
    if (!client.helloReceived) {
        std::cerr << "Selected client is awaiting HELLO" << std::endl;
        return;
    }

    const bool hadPending = !client.outbound.empty();
    const auto queued = client.requests.queueCommand(client.outbound, command);
    if (const auto error = std::get_if<protocol::RequestError>(&queued)) {
        switch (*error) {
        case protocol::RequestError::Busy:
            std::cerr << "Selected client still has a pending command" << std::endl;
            break;
        case protocol::RequestError::InvalidCommand:
            std::cerr << "Command must be 1 to 4096 bytes without NUL and valid UTF-8" << std::endl;
            break;
        case protocol::RequestError::IdExhausted:
            std::cerr << "Request IDs exhausted, reconnect client" << std::endl;
            break;
        default:
            std::cerr << "Selected client's outbound queue is full" << std::endl;
        }
        return;
    }

    client.requestDeadline = Connection::Clock::now() + std::chrono::minutes(5);
    if (!hadPending) {
        watch(EPOLL_CTL_MOD, client.socket.get(), EPOLLIN | EPOLLRDHUP | EPOLLOUT, selectedId_);
    }
    std::cout << "[client " << selectedId_ << " request " << std::get<std::uint32_t>(queued)
              << "] queued" << std::endl;
}
