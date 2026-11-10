#include "platform/local_control_socket.h"
#include <iostream>

// Small server for the Python transport contract tests; exercises the production
// channel, including fragmented requests, large replies and authentication.
int main(int argc, char** argv) {
    if (argc != 3) return 3;
    try {
        RuntimePlatform::LocalControlSocket socket;
        socket.Open(std::filesystem::u8path(argv[1]), "tcp", argv[2]);
        bool done = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (!done) {
            socket.Poll([&](const std::string& line) {
                if (line == "large") socket.Send(std::string(131072, 'x'));
                else if (line == "quit") { socket.Send("ok"); done = true; }
                else socket.Send(line);
            });
            if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error("test timed out");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
