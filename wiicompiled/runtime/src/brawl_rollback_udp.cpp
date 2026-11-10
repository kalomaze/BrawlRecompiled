#include "brawl_rollback_udp.h"
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <stdexcept>
#include <cerrno>
#include <vector>
#include "platform/socket.h"
namespace BrawlRollback {
namespace {
namespace Socket = RuntimePlatform::Sockets;
using Clock=std::chrono::steady_clock;
struct Datagram {Clock::time_point due;std::vector<char> bytes;};
std::deque<Datagram> pending;
std::vector<GekkoNetResult*> received;
unsigned delay=0;
char peerId[]="peer";
GekkoNetAddress peerAddress{peerId,4};
Socket::Handle socketFd=Socket::Invalid;
sockaddr_in peer{};
void Send(GekkoNetAddress*,const char* data,int length) {
    if(length<=0 || length>65507) throw std::runtime_error("rollback: invalid UDP packet size");
    pending.push_back({Clock::now()+std::chrono::milliseconds(delay),{data,data+length}});
}
GekkoNetResult** Receive(int* count) {
    while(!pending.empty() && pending.front().due<=Clock::now()) {
        auto& d=pending.front();
        if(sendto(socketFd,d.bytes.data(),static_cast<int>(d.bytes.size()),0,reinterpret_cast<sockaddr*>(&peer),sizeof(peer))<0)
            throw std::runtime_error("rollback: UDP send failed");
        pending.pop_front();
    }
    received.clear();
    for(unsigned packets=0;packets<1024;++packets) {
        char buffer[65536];sockaddr_in from{};Socket::Length size=sizeof(from);
        const auto n=recvfrom(socketFd,buffer,sizeof(buffer),0,reinterpret_cast<sockaddr*>(&from),&size);
        if(n<0) {
            if(Socket::WouldBlock()) break;
            if(Socket::Interrupted()) continue;
            throw std::runtime_error("rollback: UDP receive failed");
        }
        if(from.sin_addr.s_addr!=peer.sin_addr.s_addr || from.sin_port!=peer.sin_port) continue;
        auto* r=static_cast<GekkoNetResult*>(std::malloc(sizeof(GekkoNetResult)));
        r->data=std::malloc(n);r->data_len=n;std::memcpy(r->data,buffer,n);
        r->addr={std::malloc(4),4};std::memcpy(r->addr.data,peerId,4);
        received.push_back(r);
    }
    *count=received.size();return received.data();
}
GekkoNetAdapter adapter{Send,Receive,std::free};
}
GekkoNetAddress* PeerAddress(){return &peerAddress;}
void CloseUdp() {
    if(socketFd!=Socket::Invalid) Socket::Close(socketFd);
    socketFd=Socket::Invalid;
    pending.clear();received.clear();
}
GekkoNetAdapter* OpenUdp(unsigned localPort,unsigned peerPort,unsigned delayMs) {
    if(socketFd!=Socket::Invalid || !localPort || localPort>65535 || !peerPort || peerPort>65535 || delayMs>1000)
        throw std::runtime_error("rollback: invalid UDP configuration");
    Socket::Initialize();
    socketFd=socket(AF_INET,SOCK_DGRAM,0);
    sockaddr_in local{};local.sin_family=AF_INET;local.sin_addr.s_addr=htonl(INADDR_LOOPBACK);local.sin_port=htons(localPort);
    if(socketFd==Socket::Invalid || bind(socketFd,reinterpret_cast<sockaddr*>(&local),sizeof(local))<0)
        throw std::runtime_error("rollback: UDP bind failed");
    Socket::Nonblocking(socketFd);
    peer=local;peer.sin_port=htons(peerPort);delay=delayMs;
    return &adapter;
}
}
