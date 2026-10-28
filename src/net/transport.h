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
#include <vector>

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

// At startup (TenchuzApp::OnPreSetup), before SDL opens the controllers: with
// --tenchuz_netplay auto or steam, attaches to Steam (system_link.cpp).
void StartNetwork();

// Steam lobby members (transport_steam.cpp; built when the Steamworks SDK is
// in third_party/steamworks, TENCHUZ_STEAM). StartSteam() attaches to Steam,
// now or, while Steam isn't running, in the background once it is;
// SteamReady() says it has. Start() fails until then.
void StartSteam();
bool SteamReady();
std::unique_ptr<Transport> CreateSteamTransport();

// For the game menu's Online tab.
struct OnlineFriend {
  uint64_t id = 0;
  std::string name;
  bool in_game = false;   // in a Tenchu Z lobby: Join
  bool with_you = false;  // in your lobby
};
// A public Tenchu Z lobby anyone can join (--tenchuz_steam_public).
struct PublicLobby {
  uint64_t id = 0;
  std::string host;
  int players = 0, max_players = 0;
};
struct OnlineStatus {
  bool steam = false;           // in a Steam lobby
  bool steam_starting = false;  // attached, System Link not on Steam yet
  std::string name;
  int lobby_players = 0;
  std::vector<OnlineFriend> friends;  // online, Tenchu Z players first
  std::vector<PublicLobby> lobbies;   // other players' public lobbies, fullest first
};
OnlineStatus GetOnlineStatus();
void JoinFriend(uint64_t id);
void JoinLobby(uint64_t lobby);
void InviteFriend(uint64_t id);

}  // namespace tenchuz::net
