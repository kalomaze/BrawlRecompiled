#include "brawl_rollback_udp.h"
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <stdexcept>
#include <cerrno>
#include <vector>
#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
namespace BrawlRollback {
namespace {
using Clock=std::chrono::steady_clock;
struct Datagram {Clock::time_point due;std::vector<char> bytes;};
std::deque<Datagram> pending;
std::vector<GekkoNetResult*> received;
unsigned delay=0;
char peerId[]="peer";
GekkoNetAddress peerAddress{peerId,4};
#ifndef _WIN32
int socketFd=-1;
sockaddr_in peer{};
void Send(GekkoNetAddress*,const char* data,int length) {
    if(length<=0 || length>65507) throw std::runtime_error("rollback: invalid UDP packet size");
    pending.push_back({Clock::now()+std::chrono::milliseconds(delay),{data,data+length}});
}
GekkoNetResult** Receive(int* count) {
    while(!pending.empty() && pending.front().due<=Clock::now()) {
        auto& d=pending.front();
        if(sendto(socketFd,d.bytes.data(),d.bytes.size(),0,reinterpret_cast<sockaddr*>(&peer),sizeof(peer))<0)
            throw std::runtime_error("rollback: UDP send failed");
        pending.pop_front();
    }
    received.clear();
    for(unsigned packets=0;packets<1024;++packets) {
        char buffer[65536];sockaddr_in from{};socklen_t size=sizeof(from);
        const auto n=recvfrom(socketFd,buffer,sizeof(buffer),MSG_DONTWAIT,reinterpret_cast<sockaddr*>(&from),&size);
        if(n<0) {
            if(errno==EAGAIN || errno==EWOULDBLOCK) break;
            if(errno==EINTR) continue;
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
#endif
}
GekkoNetAddress* PeerAddress(){return &peerAddress;}
void CloseUdp() {
#ifndef _WIN32
    if(socketFd>=0) close(socketFd);
    socketFd=-1;
#endif
    pending.clear();received.clear();
}
GekkoNetAdapter* OpenUdp(unsigned localPort,unsigned peerPort,unsigned delayMs) {
#ifdef _WIN32
    throw std::runtime_error("rollback UDP prototype currently requires POSIX");
#else
    if(socketFd>=0 || !localPort || localPort>65535 || !peerPort || peerPort>65535 || delayMs>1000)
        throw std::runtime_error("rollback: invalid UDP configuration");
    socketFd=socket(AF_INET,SOCK_DGRAM,0);
    sockaddr_in local{};local.sin_family=AF_INET;local.sin_addr.s_addr=htonl(INADDR_LOOPBACK);local.sin_port=htons(localPort);
    if(socketFd<0 || bind(socketFd,reinterpret_cast<sockaddr*>(&local),sizeof(local))<0)
        throw std::runtime_error("rollback: UDP bind failed");
    peer=local;peer.sin_port=htons(peerPort);delay=delayMs;
    return &adapter;
#endif
}
}
