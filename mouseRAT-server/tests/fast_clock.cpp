#include <chrono>
#include <sys/epoll.h>

using Clock = std::chrono::steady_clock;

extern "C" Clock::time_point realNow() asm("__real__ZNSt6chrono3_V212steady_clock3nowEv");
extern "C" Clock::time_point wrappedNow() asm("__wrap__ZNSt6chrono3_V212steady_clock3nowEv");

Clock::time_point wrappedNow() {
    const auto now = realNow();
    static const auto origin = now;
    return origin + (now - origin) * 10;
}

extern "C" int __real_epoll_wait(int, epoll_event*, int, int);

extern "C" int __wrap_epoll_wait(int fd, epoll_event* events, int count, int timeout) {
    const auto scaled = timeout < 0 ? timeout : timeout / 10 + (timeout % 10 != 0);
    return __real_epoll_wait(fd, events, count, scaled);
}
