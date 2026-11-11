#pragma once
#include "gekkonet.h"
namespace BrawlRollback {
// Single session, loopback-only prototype transport. Delay is applied to actual
// UDP datagrams, including input history and acknowledgments.
GekkoNetAdapter* OpenUdp(unsigned localPort,unsigned peerPort,unsigned delayMs);
GekkoNetAddress* PeerAddress();
void CloseUdp();
}
