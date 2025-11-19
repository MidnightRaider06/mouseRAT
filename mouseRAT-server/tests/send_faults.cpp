#include <algorithm>
#include <cerrno>
#include <sys/socket.h>

extern "C" ssize_t __real_send(int fd, const void* data, std::size_t size, int flags);

extern "C" ssize_t __wrap_send(int fd, const void* data, std::size_t size, int flags) {
    static unsigned calls = 0;
    ++calls;

    if (calls <= 2) {
        errno = calls == 1 ? EINTR : EAGAIN;
        return -1;
    }

    return __real_send(fd, data, std::min(size, std::size_t{7}), flags);
}
