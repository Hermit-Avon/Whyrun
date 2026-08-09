#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstring>

int main(int argc, char** argv) {
    if (argc != 2) {
        return 2;
    }
    const pid_t child = fork();
    if (child == -1) {
        return 1;
    }
    if (child == 0) {
        const int fd = open(argv[1], O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd == -1) {
            _exit(1);
        }
        constexpr char message[] = "written by child\n";
        const bool okay = write(fd, message, std::strlen(message)) ==
                              static_cast<ssize_t>(std::strlen(message)) &&
                          close(fd) == 0;
        _exit(okay ? 0 : 1);
    }
    int status{};
    if (waitpid(child, &status, 0) == -1) {
        return 1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

