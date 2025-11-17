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
constexpr std::uint64_t diskId = consoleId - 1;
constexpr std::size_t maxClients = 64;

std::filesystem::path screenshotDirectory() {
    const auto timestamp = std::chrono::system_clock::now().time_since_epoch().count();
    return std::filesystem::path("screenshots") /
           ("run-" + std::to_string(timestamp) + "-" + std::to_string(getpid()));
}

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
    : listener_(listener), poller_(epoll_create1(EPOLL_CLOEXEC)),
      screenshots_(screenshotDirectory()), disk_(std::ref(screenshots_)), transfers_(disk_) {
    if (poller_.get() < 0) {
        throw std::system_error(errno, std::generic_category(), "epoll_create1");
    }

    watch(EPOLL_CTL_ADD, listener_.descriptor(), EPOLLIN, listenerId);
    watch(EPOLL_CTL_ADD, STDIN_FILENO, EPOLLIN, consoleId);
    watch(EPOLL_CTL_ADD, disk_.descriptor(), EPOLLIN, diskId);
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
    std::cout << "Controls: :clients, :use <id>, screenshot, quit" << std::endl;

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
            else if (id == diskId) diskReady();
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
        if (wasEmpty) updateInterest(id, client);
    }
}

void ServerLoop::acceptReady() {
    for (int accepted = 0; accepted < 16; ++accepted) {
        auto client = listener_.acceptConnection();
        if (!client) break;

        if (clients_.size() == maxClients || nextId_ == diskId) {
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
    transfers_.cancelClient(id);
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
    if (client.pendingDisk && (events & (EPOLLHUP | EPOLLERR))) {
        disconnect(id);
        return;
    }

    if (!client.pendingDisk && (events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR))) {
        ssize_t received;
        do {
            received = recv(client.socket.get(), client.input.data(), client.input.size(), 0);
        } while (received < 0 && errno == EINTR);

        if (received > 0) {
            client.inputOffset = 0;
            client.inputSize = static_cast<std::size_t>(received);
            client.inputAt = Connection::Clock::now();
            if (!processInput(id, client)) {
                std::cerr << "[client " << id << "] protocol violation" << std::endl;
                disconnect(id);
                return;
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

    updateInterest(id, client);
}

void ServerLoop::updateInterest(std::uint64_t id, const Connection& client) {
    const auto reads = client.pendingDisk ? 0u : EPOLLIN | EPOLLRDHUP;
    watch(EPOLL_CTL_MOD, client.socket.get(), reads | (client.outbound.empty() ? 0u : EPOLLOUT), id);
}

ScreenshotAdmission ServerLoop::submitImage(std::uint64_t id, Connection& client) {
    const auto& frame = *client.pendingDisk;
    if (frame.header.type == protocol::MessageType::ImageBegin) {
        const auto admitted = transfers_.begin(id, frame.header.requestId, frame.payload);
        if (admitted.status == ScreenshotAdmission::Accepted) client.transferId = admitted.transferId;
        return admitted.status;
    }
    if (frame.header.type == protocol::MessageType::ImageData) {
        return transfers_.write(client.transferId, frame.payload);
    }
    const auto admitted = transfers_.commit(client.transferId);
    if (admitted == ScreenshotAdmission::Accepted) client.requestDeadline.reset();
    return admitted;
}

bool ServerLoop::processInput(std::uint64_t id, Connection& client) {
    while (true) {
        if (const auto deadline = client.nextDeadline(); deadline && Connection::Clock::now() >= *deadline) {
            return false;
        }
        if (client.pendingDisk) {
            const auto admitted = submitImage(id, client);
            if (admitted == ScreenshotAdmission::Full) return true;
            if (admitted != ScreenshotAdmission::Accepted) return false;
            client.pendingDisk.reset();
        }
        if (client.inputOffset == client.inputSize) return true;

        if (!client.decoder.hasPartialFrame()) client.frameStarted = client.inputAt;
        if (const auto deadline = client.nextDeadline(); deadline && Connection::Clock::now() >= *deadline) {
            return false;
        }
        const auto step = client.decoder.consume(std::string_view(client.input.data() + client.inputOffset,
                                                                client.inputSize - client.inputOffset));
        client.inputOffset += step.consumed;
        if (std::holds_alternative<protocol::HeaderError>(step.result)) return false;
        const auto frame = std::get_if<protocol::FrameView>(&step.result);
        if (!frame) return true;
        client.frameStarted.reset();
        if (!processFrame(id, client, *frame)) return false;
        client.lastFrameAt = client.inputAt;
    }
}

void ServerLoop::diskReady() {
    for (int count = 0; count < 16; ++count) {
        const auto result = transfers_.takeCompletion();
        if (!result) break;
        const auto& job = result->disk;
        if (result->firstFailure || (job.operation == DiskOperation::Cancel && job.error)) {
            std::cerr << "[client " << result->clientId << " request " << result->requestId
                      << "] storage error: " << job.error.message() << std::endl;
            if (clients_.find(result->clientId) != clients_.end()) disconnect(result->clientId);
        }
        if (!result->released) continue;
        if (job.operation == DiskOperation::Commit) {
            std::cout << "[client " << result->clientId << " request " << result->requestId
                      << "] saved " << screenshots_.destination(job.transferId, result->format) << std::endl;
        } else {
            std::cout << "[client " << result->clientId << " request " << result->requestId
                      << "] screenshot canceled" << std::endl;
        }
        const auto found = clients_.find(result->clientId);
        if (found != clients_.end() && found->second.transferId == job.transferId) {
            found->second.transferId = 0;
            found->second.screenshotRequest = 0;
        }
    }

    auto it = clients_.upper_bound(resumeAfter_);
    bool first = true;
    for (std::size_t remaining = clients_.size(); remaining && !clients_.empty(); --remaining) {
        if (it == clients_.end()) it = clients_.begin();
        const auto current = it++;
        auto& client = current->second;
        if (!client.pendingDisk) continue;
        const auto id = current->first;
        if (first) { resumeAfter_ = id; first = false; }
        if (!processInput(id, client)) disconnect(id);
        else updateInterest(id, client);
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
    if (client.requests.acceptResponse(frame)) return false;
    if (client.screenshotRequest != 0 && frame.header.type != MessageType::Error) {
        client.pendingDisk = frame;
        return true;
    }
    if (client.screenshotRequest != 0) {
        transfers_.cancelClient(id);
        if (client.transferId == 0) client.screenshotRequest = 0;
    }
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
    if (client.screenshotRequest != 0 && client.requests.activeId() == 0) {
        std::cerr << "Selected client's screenshot is awaiting storage completion" << std::endl;
        return;
    }

    const bool hadPending = !client.outbound.empty();
    const auto queued = command == "screenshot" ? client.requests.queueScreenshot(client.outbound) :
                                                client.requests.queueCommand(client.outbound, command);
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
    if (command == "screenshot") client.screenshotRequest = std::get<std::uint32_t>(queued);
    if (!hadPending) updateInterest(selectedId_, client);
    std::cout << "[client " << selectedId_ << " request " << std::get<std::uint32_t>(queued)
              << "] queued" << std::endl;
}
