#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstring>

int main(int argc, char** argv) {
    if (argc != 2) {
        return 2;
    }

    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd == -1) {
        return 1;
    }

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (std::strlen(argv[1]) >= sizeof(address.sun_path)) {
        close(fd);
        return 2;
    }
    std::strcpy(address.sun_path, argv[1]);
    const auto length = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + std::strlen(address.sun_path) + 1);
    const int result = connect(fd, reinterpret_cast<sockaddr*>(&address), length);
    const int saved_errno = errno;
    close(fd);
    return result == -1 && saved_errno == ENOENT ? 0 : 1;
}
