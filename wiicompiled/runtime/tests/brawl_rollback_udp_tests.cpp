#include "brawl_rollback_udp.h"
#include "platform/socket.h"
#include <cassert>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <utility>
#include <thread>

namespace Socket = RuntimePlatform::Sockets;
int main() {
    Socket::Initialize();
    auto bound = [] {
        auto socket = ::socket(AF_INET, SOCK_DGRAM, 0);
        assert(socket != Socket::Invalid);
        auto address = Socket::Loopback(0);
        assert(bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        Socket::Length length = sizeof(address);
        assert(getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length) == 0);
        Socket::Nonblocking(socket);
        return std::make_pair(socket, address);
    };
    auto [peer, peerAddress] = bound();
    auto [reservation, nativeAddress] = bound();
    Socket::Close(reservation);
    auto* adapter = BrawlRollback::OpenUdp(ntohs(nativeAddress.sin_port), ntohs(peerAddress.sin_port), 0);
    const char packet[] = "rollback\0input";
    adapter->send_data(BrawlRollback::PeerAddress(), packet, sizeof(packet));
    int count = 0;
    adapter->receive_data(&count);
    char received[128];
    int length = -1;
    const auto receiveDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (length < 0 && std::chrono::steady_clock::now() < receiveDeadline) {
        length = recv(peer, received, sizeof(received), 0);
        if (length < 0) { assert(Socket::WouldBlock()); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    }
    assert(length == sizeof(packet) && std::memcmp(packet, received, sizeof(packet)) == 0);
    assert(sendto(peer, packet, sizeof(packet), 0, reinterpret_cast<sockaddr*>(&nativeAddress), sizeof(nativeAddress)) == sizeof(packet));
    GekkoNetResult** results = nullptr;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!count && std::chrono::steady_clock::now() < deadline) {
        results = adapter->receive_data(&count);
        if (!count) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(count == 1 && results[0]->data_len == sizeof(packet));
    assert(std::memcmp(results[0]->data, packet, sizeof(packet)) == 0);
    std::free(results[0]->data); std::free(results[0]->addr.data); std::free(results[0]);
    BrawlRollback::CloseUdp(); Socket::Close(peer);
}
