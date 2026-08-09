#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

int main() {
    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd == -1) {
        return 1;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(1);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    const int result = connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    close(fd);
    return result == -1 ? 0 : 1;
}

