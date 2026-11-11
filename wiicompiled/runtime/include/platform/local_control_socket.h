#pragma once
#include "platform/socket.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#ifndef _WIN32
#include <sys/un.h>
#endif

namespace RuntimePlatform {
// One launcher controls one child. TCP is loopback-only and requires the
// launcher's per-process token before accepting any commands. Unix sockets
// remain available for existing POSIX launchers, selected explicitly.
class LocalControlSocket {
    Sockets::Handle listener = Sockets::Invalid, client = Sockets::Invalid;
    std::filesystem::path endpoint;
    std::string pending, token;
    bool authenticated = false, tcp = false;
public:
    LocalControlSocket() = default;
    LocalControlSocket(const LocalControlSocket&) = delete;
    LocalControlSocket& operator=(const LocalControlSocket&) = delete;
    ~LocalControlSocket() {
        if (client != Sockets::Invalid) Sockets::Close(client);
        if (listener != Sockets::Invalid) Sockets::Close(listener);
        if (!endpoint.empty()) { std::error_code error; std::filesystem::remove(endpoint, error); }
    }
    bool IsOpen() const { return listener != Sockets::Invalid; }
    void Open(const std::filesystem::path& path, const std::string& transport, const std::string& secret) {
        if (IsOpen() || std::filesystem::exists(path)) throw std::runtime_error("control endpoint already exists");
        Sockets::Initialize();
        if (transport == "tcp") {
            if (secret.size() != 64 || secret.find_first_not_of("0123456789abcdef") != std::string::npos)
                throw std::runtime_error("TCP control requires a 32-byte hexadecimal token");
            token = secret; tcp = true;
            listener = socket(AF_INET, SOCK_STREAM, 0);
            auto address = Sockets::Loopback(0);
            if (listener == Sockets::Invalid || bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)))
                throw std::runtime_error("control TCP bind failed");
            if (listen(listener, 1)) throw std::runtime_error("control listen failed");
            Sockets::Nonblocking(listener);
            Sockets::Length length = sizeof(address);
            if (getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length))
                throw std::runtime_error("control endpoint lookup failed");
            // Publish only after listen succeeds. The endpoint contains no secret.
            auto temporary = path; temporary += ".tmp";
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            file << ntohs(address.sin_port) << '\n'; file.close();
            if (!file) throw std::runtime_error("control endpoint write failed");
            std::filesystem::rename(temporary, path);
            endpoint = path;
        } else if (transport == "unix") {
#ifdef _WIN32
            throw std::runtime_error("Unix control transport is unavailable on Windows; select TCP");
#else
            sockaddr_un address{}; address.sun_family = AF_UNIX;
            const auto name = path.string();
            if (name.size() >= sizeof(address.sun_path)) throw std::runtime_error("control socket path too long");
            std::copy(name.begin(), name.end(), address.sun_path);
            listener = socket(AF_UNIX, SOCK_STREAM, 0);
            if (listener == Sockets::Invalid || bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)))
                throw std::runtime_error("control Unix bind failed");
            endpoint = path;
            if (listen(listener, 1)) throw std::runtime_error("control listen failed");
            Sockets::Nonblocking(listener);
            authenticated = true;
#endif
        } else throw std::runtime_error("unknown control transport");
    }
    void Send(const std::string& line) {
        const std::string bytes = line + '\n';
        size_t offset = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (offset < bytes.size()) {
            const int n = send(client, bytes.data() + offset, static_cast<int>(bytes.size() - offset), Sockets::SendFlags);
            if (n > 0) { offset += n; continue; }
            if (n < 0 && Sockets::Interrupted()) continue;
            if (n < 0 && Sockets::WouldBlock()) {
                if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("control send timed out");
                std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue;
            }
            throw std::runtime_error("control send failed");
        }
    }
    template<class Command> void Poll(Command command) {
        if (client == Sockets::Invalid) {
            client = accept(listener, nullptr, nullptr);
            if (client == Sockets::Invalid) {
                if (Sockets::WouldBlock() || Sockets::Interrupted()) return;
                throw std::runtime_error("control accept failed");
            }
            Sockets::Nonblocking(client); Sockets::NoSigpipe(client);
            if (tcp) Sockets::NoDelay(client);
        }
        char bytes[8192];
        for (;;) {
            const int n = recv(client, bytes, sizeof(bytes), 0);
            if (n < 0) {
                if (Sockets::WouldBlock()) break;
                if (Sockets::Interrupted()) continue;
                throw std::runtime_error("control receive failed");
            }
            if (!n) throw std::runtime_error("online controller disconnected");
            pending.append(bytes, n);
            if (pending.size() > 262144) throw std::runtime_error("control request too large");
        }
        for (size_t end; (end = pending.find('\n')) != std::string::npos;) {
            auto line = pending.substr(0, end); pending.erase(0, end + 1);
            if (!authenticated) {
                if (line != "auth " + token) throw std::runtime_error("control authentication failed");
                authenticated = true; token.clear(); Send("ok");
            } else command(line);
        }
    }
};
}
