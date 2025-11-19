#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

extern "C" ssize_t __real_write(int, const void*, std::size_t);
extern "C" int __real_fsync(int);
extern "C" int __real_epoll_ctl(int, int, int, epoll_event*);

namespace {
void marker(const std::string& path) {
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
    if (fd >= 0) close(fd);
}
}

extern "C" ssize_t __wrap_write(int fd, const void* data, std::size_t size) {
    struct stat status{};
    if (fstat(fd, &status) == 0 && S_ISREG(status.st_mode)) {
        static std::atomic<bool> first{true};
        const char* gate = std::getenv("MOUSERAT_TEST_DISK_GATE");
        if (gate && first.exchange(false)) {
            marker(std::string(gate) + ".blocked");
            int attempts = 0;
            while (access((std::string(gate) + ".release").c_str(), F_OK) != 0) {
                if (++attempts == 1000) { errno = ETIMEDOUT; return -1; }
                const timespec delay{0, 10000000};
                nanosleep(&delay, nullptr);
            }
        }
        const char* failure = std::getenv("MOUSERAT_TEST_DISK_FAIL");
        if (failure && std::string_view(failure) == "write") { errno = ENOSPC; return -1; }
    }
    return __real_write(fd, data, size);
}

extern "C" int __wrap_fsync(int fd) {
    const char* failure = std::getenv("MOUSERAT_TEST_DISK_FAIL");
    if (failure && std::string_view(failure) == "fsync") { errno = ENOSPC; return -1; }
    return __real_fsync(fd);
}

extern "C" int __wrap_epoll_ctl(int poller, int operation, int fd, epoll_event* event) {
    const int result = __real_epoll_ctl(poller, operation, fd, event);
    const char* gate = std::getenv("MOUSERAT_TEST_DISK_GATE");
    if (result == 0 && gate && operation == EPOLL_CTL_MOD && !(event->events & EPOLLIN)) {
        marker(std::string(gate) + ".paused");
    }
    return result;
}
