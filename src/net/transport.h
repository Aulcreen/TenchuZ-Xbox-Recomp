// tenchuz - how System Link frames travel between players.
//
// The virtual LAN (system_link.cpp) gives each player a virtual IPv4 address
// and hands its frames to a Transport: loopback UDP between game instances on
// one PC (transport_local.cpp), or Steam peer-to-peer (transport_steam.cpp).

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace tenchuz::net {

// Destination meaning "every peer".
constexpr uint32_t kBroadcastIp = 0xFFFFFFFFu;

class Transport {
 public:
  using Sink = std::function<void(uint32_t src_ip, const uint8_t* data, size_t size)>;

  virtual ~Transport() = default;

  // False if the transport can't run (logged).
  virtual bool Start() = 0;
  // This player's virtual address (host byte order).
  virtual uint32_t LocalIp() const = 0;
  // Unreliable frames may be dropped; reliable ones arrive once, in order.
  virtual void Send(uint32_t dst_ip, const uint8_t* data, size_t size, bool reliable) = 0;
  // Waits up to timeout_ms for frames and hands each to sink.
  virtual void Receive(int timeout_ms, const Sink& sink) = 0;
  // The player's name for other players, if the transport has one.
  virtual std::string PlayerName() const { return {}; }
};

// Game instances on this PC, over 127.0.0.1 (for testing).
std::unique_ptr<Transport> CreateLocalTransport();

// Steam lobby members (transport_steam.cpp; built when the Steamworks SDK is
// in third_party/steamworks, TENCHUZ_STEAM).
std::unique_ptr<Transport> CreateSteamTransport();

}  // namespace tenchuz::net
