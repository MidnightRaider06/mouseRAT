#include "TCPServer.h"

#include <cassert>
#include <cerrno>
#include <fcntl.h>
#include <sys/epoll.h>

int main() {
    TCPServer server(0);
    assert(server.bindSocket());
    assert(server.listenSocket());
    assert(fcntl(server.descriptor(), F_GETFL) & O_NONBLOCK);
    assert(!server.acceptConnection());

    sockaddr_in address{};
    socklen_t length = sizeof(address);
    assert(getsockname(server.descriptor(), reinterpret_cast<sockaddr*>(&address), &length) == 0);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    UniqueFd poller(epoll_create1(EPOLL_CLOEXEC));
    assert(poller.get() >= 0);

    epoll_event interest{};
    interest.events = EPOLLIN;
    interest.data.fd = server.descriptor();
    assert(epoll_ctl(poller.get(), EPOLL_CTL_ADD, server.descriptor(), &interest) == 0);

    epoll_event ready{};
    assert(epoll_wait(poller.get(), &ready, 1, 0) == 0);

    UniqueFd peerA(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    UniqueFd peerB(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    assert(peerA.get() >= 0 && peerB.get() >= 0);
    assert(connect(peerA.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    assert(connect(peerB.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);

    assert(epoll_wait(poller.get(), &ready, 1, 1000) == 1);
    assert(ready.data.fd == server.descriptor());
    assert(ready.events & EPOLLIN);

    auto first = server.acceptConnection();
    assert(first);
    assert(epoll_wait(poller.get(), &ready, 1, 1000) == 1);

    auto second = server.acceptConnection();
    assert(second);
    assert(first->peerAddress.sin_port != second->peerAddress.sin_port);

    assert(!server.acceptConnection());
    assert(epoll_wait(poller.get(), &ready, 1, 0) == 0);

    const int firstFd = first->socket.get();
    const int secondFd = second->socket.get();
    assert(fcntl(firstFd, F_GETFL) & O_NONBLOCK);
    assert(fcntl(firstFd, F_GETFD) & FD_CLOEXEC);

    first.reset();
    second.reset();

    assert(fcntl(firstFd, F_GETFD) == -1 && errno == EBADF);
    assert(fcntl(secondFd, F_GETFD) == -1 && errno == EBADF);
}
