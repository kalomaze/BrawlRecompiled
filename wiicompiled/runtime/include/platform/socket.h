#pragma once

// Host sockets use the same Winsock/BSD split as netplay/net_socket.cpp.
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <unistd.h>
#endif

namespace RuntimePlatform::Sockets {
#ifdef _WIN32
using Handle = SOCKET;
using Length = int;
inline constexpr Handle Invalid = INVALID_SOCKET;
inline void Close(Handle s) { closesocket(s); }
inline bool WouldBlock() { return WSAGetLastError() == WSAEWOULDBLOCK; }
inline bool Interrupted() { return WSAGetLastError() == WSAEINTR; }
inline void Initialize() {
    static const bool ready = [] { WSADATA data{}; return WSAStartup(MAKEWORD(2, 2), &data) == 0; }();
    if (!ready) throw std::runtime_error("Winsock initialization failed");
}
inline void Nonblocking(Handle s) {
    u_long enabled = 1;
    if (ioctlsocket(s, FIONBIO, &enabled)) throw std::runtime_error("socket nonblocking setup failed");
}
#else
using Handle = int;
using Length = socklen_t;
inline constexpr Handle Invalid = -1;
inline void Close(Handle s) { close(s); }
inline bool WouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK; }
inline bool Interrupted() { return errno == EINTR; }
inline void Initialize() {}
inline void Nonblocking(Handle s) {
    const int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0 || fcntl(s, F_SETFL, flags | O_NONBLOCK) < 0)
        throw std::runtime_error("socket nonblocking setup failed");
}
#endif
inline void NoSigpipe(Handle s) {
#ifdef SO_NOSIGPIPE
    const int enabled = 1;
    if (setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)))
        throw std::runtime_error("socket SIGPIPE setup failed");
#else
    (void)s;
#endif
}
#ifdef MSG_NOSIGNAL
inline constexpr int SendFlags = MSG_NOSIGNAL;
#else
inline constexpr int SendFlags = 0;
#endif
inline void NoDelay(Handle s) {
    const int enabled = 1;
    if (setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&enabled), sizeof(enabled)))
        throw std::runtime_error("socket TCP_NODELAY setup failed");
}
inline sockaddr_in Loopback(unsigned port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<unsigned short>(port));
    return address;
}
}
