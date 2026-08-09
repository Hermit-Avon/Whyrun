#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: test_file INPUT OUTPUT\n";
        return 2;
    }

    const int input = open(argv[1], O_RDONLY | O_CLOEXEC);
    if (input == -1) {
        std::cerr << "open input: " << std::strerror(errno) << '\n';
        return 1;
    }
    const int output = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (output == -1) {
        std::cerr << "open output: " << std::strerror(errno) << '\n';
        close(input);
        return 1;
    }

    std::array<char, 256> buffer{};
    while (true) {
        const ssize_t count = read(input, buffer.data(), buffer.size());
        if (count == 0) {
            break;
        }
        if (count < 0 || write(output, buffer.data(), static_cast<std::size_t>(count)) != count) {
            close(input);
            close(output);
            return 1;
        }
    }
    return close(input) == 0 && close(output) == 0 ? 0 : 1;
}

