// tenchuz - System Link between game instances on this PC (see transport.h).
//
// Instance i (the first free slot) listens on 127.0.0.1:<port + i> and is
// 10.92.0.<i + 1> on the virtual LAN. Broadcasts go to every other slot.
// Loopback UDP is not formally reliable, but with large socket buffers it
// doesn't drop in practice, which is enough for testing.

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <mstcpip.h>
#include <ws2tcpip.h>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

namespace {
int LastError() { return WSAGetLastError(); }
}  // namespace
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>

namespace {
using SOCKET = int;
constexpr SOCKET INVALID_SOCKET = -1;
int closesocket(SOCKET s) { return close(s); }
int LastError() { return errno; }
}  // namespace
#endif

#include <cstring>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "transport.h"

REXCVAR_DEFINE_INT32(tenchuz_net_local_port, 47320, "tenchuz",
                     "First loopback UDP port of --tenchuz_netplay=local (one per game instance)");

namespace tenchuz::net {
namespace {

constexpr int kSlots = 8;
constexpr uint32_t kSubnet = 0x0A5C0000u;  // 10.92.0.0
constexpr uint32_t kMagic = 0x4C565A54u;   // "TZVL"
constexpr size_t kMaxFrame = 65000;

struct Header {
  uint32_t magic;
  uint32_t src_ip;
};

class LocalTransport final : public Transport {
 public:
  ~LocalTransport() override {
    if (socket_ != INVALID_SOCKET) closesocket(socket_);
  }

  bool Start() override {
#if defined(_WIN32)
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
      REXLOG_ERROR("tenchuz: netplay local: WSAStartup failed");
      return false;
    }
#endif
    socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_ == INVALID_SOCKET) {
      REXLOG_ERROR("tenchuz: netplay local: socket failed ({})", LastError());
      return false;
    }
    const int buffer = 8 << 20;
    setsockopt(socket_, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&buffer), sizeof(buffer));
    setsockopt(socket_, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&buffer), sizeof(buffer));
#if defined(_WIN32)
    // Broadcasts reach empty slots too; don't let their ICMP replies fail recv.
    BOOL report = FALSE;
    DWORD unused = 0;
    WSAIoctl(socket_, SIO_UDP_CONNRESET, &report, sizeof(report), nullptr, 0, &unused, nullptr, nullptr);
#endif
    base_port_ = REXCVAR_GET(tenchuz_net_local_port);
    for (int slot = 0; slot < kSlots; ++slot) {
      sockaddr_in addr = Address(slot);
      if (bind(socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
        slot_ = slot;
        break;
      }
    }
    if (slot_ < 0) {
      REXLOG_ERROR("tenchuz: netplay local: ports {}-{} are all in use", base_port_,
                   base_port_ + kSlots - 1);
      return false;
    }
    REXLOG_INFO("tenchuz: netplay local: instance {} on 127.0.0.1:{}, virtual address 10.92.0.{}",
                slot_ + 1, base_port_ + slot_, slot_ + 1);
    return true;
  }

  uint32_t LocalIp() const override { return kSubnet + uint32_t(slot_ + 1); }

  void Send(uint32_t dst_ip, const uint8_t* data, size_t size, bool /*reliable*/) override {
    if (size + sizeof(Header) > kMaxFrame) {
      REXLOG_WARN("tenchuz: netplay local: dropped a {} byte frame", size);
      return;
    }
    std::vector<uint8_t> frame(sizeof(Header) + size);
    const Header header{kMagic, LocalIp()};
    std::memcpy(frame.data(), &header, sizeof(header));
    std::memcpy(frame.data() + sizeof(header), data, size);
    auto send_slot = [&](int slot) {
      sockaddr_in addr = Address(slot);
      sendto(socket_, reinterpret_cast<const char*>(frame.data()), int(frame.size()), 0,
             reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    };
    if (dst_ip == kBroadcastIp) {
      for (int slot = 0; slot < kSlots; ++slot) {
        if (slot != slot_) send_slot(slot);
      }
    } else if ((dst_ip & 0xFFFFFF00u) == kSubnet && (dst_ip & 0xFF) >= 1 && (dst_ip & 0xFF) <= kSlots) {
      send_slot(int(dst_ip & 0xFF) - 1);
    }
  }

  void Receive(int timeout_ms, const Sink& sink) override {
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(socket_, &read_set);
    timeval timeout{0, timeout_ms * 1000};
    if (select(int(socket_) + 1, &read_set, nullptr, nullptr, &timeout) <= 0) return;
    for (;;) {
#if defined(_WIN32)
      u_long pending = 0;
      if (ioctlsocket(socket_, FIONREAD, &pending) != 0 || !pending) return;
#else
      int pending = 0;
      if (ioctl(socket_, FIONREAD, &pending) != 0 || !pending) return;
#endif
      const int got = int(recv(socket_, reinterpret_cast<char*>(buffer_.data()), int(buffer_.size()), 0));
      if (got < int(sizeof(Header))) {
#if defined(_WIN32)
        if (got < 0 && LastError() != WSAECONNRESET) return;
#else
        if (got < 0 && LastError() != ECONNREFUSED) return;
#endif
        continue;
      }
      Header header;
      std::memcpy(&header, buffer_.data(), sizeof(header));
      if (header.magic != kMagic) continue;
      sink(header.src_ip, buffer_.data() + sizeof(Header), size_t(got) - sizeof(Header));
    }
  }

 private:
  sockaddr_in Address(int slot) const {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(uint16_t(base_port_ + slot));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return addr;
  }

  SOCKET socket_ = INVALID_SOCKET;
  int base_port_ = 0;
  int slot_ = -1;
  std::vector<uint8_t> buffer_ = std::vector<uint8_t>(kMaxFrame);
};

}  // namespace

std::unique_ptr<Transport> CreateLocalTransport() { return std::make_unique<LocalTransport>(); }

}  // namespace tenchuz::net
