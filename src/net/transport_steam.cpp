// tenchuz - System Link over Steam (see transport.h).
//
// Players meet in a friends-only Steam lobby and frames travel with
// ISteamNetworkingMessages, relayed by Steam: no ports to open. Every lobby
// member is 10.x.y.z on the virtual LAN, from its SteamID, and a broadcast
// goes to every member. Runs as App ID 480 (Spacewar), Valve's shared test
// app.
//
// Getting into one lobby: each player opens a lobby when the game starts.
// Accepting a Steam invite (Steam friends list > Invite to Lobby) moves you to
// the inviter's lobby, and with --tenchuz_steam_autojoin a player alone in
// their lobby joins a friend's Tenchu Z lobby by itself (the lower SteamID
// hosts, so two players starting together meet in one lobby).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <steam/isteamnetworkingmessages.h>
#include <steam/isteamnetworkingutils.h>
#include <steam/steam_api.h>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "transport.h"

REXCVAR_DEFINE_BOOL(tenchuz_steam_autojoin, true, "tenchuz",
                    "Steam System Link: when alone in your lobby, join a friend's Tenchu Z lobby");

namespace tenchuz::net {
namespace {

constexpr uint32_t kAppId = 480;  // Spacewar
constexpr int kChannel = 0;
constexpr int kMaxMembers = 4;
constexpr const char* kLobbyKey = "tenchuz";

uint32_t VirtualIp(CSteamID id) {
  uint32_t h = id.GetAccountID() * 2654435761u;
  h ^= h >> 15;
  uint32_t ip = 0x0A000000u | (h & 0x00FFFFFFu);
  const uint32_t last = ip & 0xFF;
  if (last == 0 || last == 0xFF) ip ^= 0x01;  // not a network or broadcast address
  return ip;
}

std::string IpText(uint32_t ip) {
  return std::to_string(ip >> 24) + "." + std::to_string((ip >> 16) & 0xFF) + "." +
         std::to_string((ip >> 8) & 0xFF) + "." + std::to_string(ip & 0xFF);
}

class SteamSession {
 public:
  SteamSession() {
    self_ = SteamUser()->GetSteamID();
    local_ip_ = VirtualIp(self_);
    SteamNetworkingUtils()->InitRelayNetworkAccess();
    lobby_created_.Set(SteamMatchmaking()->CreateLobby(k_ELobbyTypeFriendsOnly, kMaxMembers), this,
                       &SteamSession::OnLobbyCreated);
    REXLOG_INFO("tenchuz: netplay steam: {} ({}), virtual address {}", SteamFriends()->GetPersonaName(),
                self_.ConvertToUint64(), IpText(local_ip_));
  }

  uint32_t local_ip() const { return local_ip_; }

  void Send(uint32_t dst_ip, const uint8_t* data, size_t size, bool reliable) {
    const int flags = (reliable ? k_nSteamNetworkingSend_Reliable : k_nSteamNetworkingSend_UnreliableNoNagle) |
                      k_nSteamNetworkingSend_AutoRestartBrokenSession;
    std::vector<CSteamID> targets;
    {
      std::lock_guard lock(peers_mutex_);
      if (dst_ip == kBroadcastIp) {
        for (auto& [ip, id] : peers_) targets.push_back(id);
      } else if (auto it = peers_.find(dst_ip); it != peers_.end()) {
        targets.push_back(it->second);
      }
    }
    for (CSteamID id : targets) {
      SteamNetworkingIdentity identity;
      identity.SetSteamID(id);
      SteamNetworkingMessages()->SendMessageToUser(identity, data, uint32(size), flags, kChannel);
    }
  }

  void Receive(int timeout_ms, const Transport::Sink& sink) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
      SteamAPI_RunCallbacks();
      MaybeAutoJoin();
      SteamNetworkingMessage_t* messages[32];
      const int count = SteamNetworkingMessages()->ReceiveMessagesOnChannel(kChannel, messages, 32);
      for (int i = 0; i < count; ++i) {
        const CSteamID from = messages[i]->m_identityPeer.GetSteamID();
        sink(VirtualIp(from), static_cast<const uint8_t*>(messages[i]->m_pData), size_t(messages[i]->m_cbSize));
        messages[i]->Release();
      }
      if (count > 0 || std::chrono::steady_clock::now() >= deadline) return;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  std::string name() const { return SteamFriends()->GetPersonaName(); }

 private:
  void OnLobbyCreated(LobbyCreated_t* result, bool io_failure) {
    if (io_failure || result->m_eResult != k_EResultOK) {
      REXLOG_WARN("tenchuz: netplay steam: couldn't open a lobby (result {})", int(result->m_eResult));
      return;
    }
    const CSteamID lobby(result->m_ulSteamIDLobby);
    SteamMatchmaking()->SetLobbyData(lobby, kLobbyKey, "1");
    REXLOG_INFO("tenchuz: netplay steam: opened lobby {}; friends can join from Steam (Invite to Lobby)",
                lobby.ConvertToUint64());
  }

  STEAM_CALLBACK(SteamSession, OnLobbyEnter, LobbyEnter_t) {
    if (pParam->m_EChatRoomEnterResponse != k_EChatRoomEnterResponseSuccess) {
      REXLOG_WARN("tenchuz: netplay steam: couldn't enter lobby {} (response {})", pParam->m_ulSteamIDLobby,
                  pParam->m_EChatRoomEnterResponse);
      return;
    }
    const CSteamID lobby(pParam->m_ulSteamIDLobby);
    if (lobby_.IsValid() && lobby_ != lobby) SteamMatchmaking()->LeaveLobby(lobby_);
    lobby_ = lobby;
    SteamFriends()->SetRichPresence("status", "Tenchu Z: System Link");
    UpdatePeers();
  }

  STEAM_CALLBACK(SteamSession, OnLobbyChatUpdate, LobbyChatUpdate_t) {
    if (CSteamID(pParam->m_ulSteamIDLobby) != lobby_) return;
    const CSteamID who(pParam->m_ulSteamIDUserChanged);
    const bool joined = pParam->m_rgfChatMemberStateChange & k_EChatMemberStateChangeEntered;
    REXLOG_INFO("tenchuz: netplay steam: {} {} the lobby", SteamFriends()->GetFriendPersonaName(who),
                joined ? "joined" : "left");
    UpdatePeers();
  }

  STEAM_CALLBACK(SteamSession, OnJoinRequested, GameLobbyJoinRequested_t) {
    REXLOG_INFO("tenchuz: netplay steam: joining {}'s lobby", SteamFriends()->GetFriendPersonaName(pParam->m_steamIDFriend));
    SteamMatchmaking()->JoinLobby(pParam->m_steamIDLobby);
  }

  STEAM_CALLBACK(SteamSession, OnSessionRequest, SteamNetworkingMessagesSessionRequest_t) {
    const CSteamID who = pParam->m_identityRemote.GetSteamID();
    bool known = false;
    {
      std::lock_guard lock(peers_mutex_);
      for (auto& [ip, id] : peers_) known |= id == who;
    }
    if (!known && lobby_.IsValid()) {
      UpdatePeers();
      std::lock_guard lock(peers_mutex_);
      for (auto& [ip, id] : peers_) known |= id == who;
    }
    if (known || SteamFriends()->GetFriendRelationship(who) == k_EFriendRelationshipFriend) {
      SteamNetworkingMessages()->AcceptSessionWithUser(pParam->m_identityRemote);
    } else {
      REXLOG_WARN("tenchuz: netplay steam: ignored a connection from {}, not in the lobby", who.ConvertToUint64());
    }
  }

  STEAM_CALLBACK(SteamSession, OnSessionFailed, SteamNetworkingMessagesSessionFailed_t) {
    REXLOG_WARN("tenchuz: netplay steam: connection to {} failed: {}",
                pParam->m_info.m_identityRemote.GetSteamID().ConvertToUint64(), pParam->m_info.m_szEndDebug);
  }

  STEAM_CALLBACK(SteamSession, OnLobbyDataUpdate, LobbyDataUpdate_t) {
    const CSteamID lobby(pParam->m_ulSteamIDLobby);
    if (!pParam->m_bSuccess || lobby == lobby_ || pParam->m_ulSteamIDMember != pParam->m_ulSteamIDLobby) return;
    auto it = std::find(candidates_.begin(), candidates_.end(), lobby);
    if (it == candidates_.end()) return;
    candidates_.erase(it);
    if (std::strcmp(SteamMatchmaking()->GetLobbyData(lobby, kLobbyKey), "1") != 0) return;
    if (Alone()) {
      REXLOG_INFO("tenchuz: netplay steam: joining a friend's lobby {}", lobby.ConvertToUint64());
      SteamMatchmaking()->JoinLobby(lobby);
    }
  }

  bool Alone() const { return !lobby_.IsValid() || SteamMatchmaking()->GetNumLobbyMembers(lobby_) <= 1; }

  // Every few seconds while alone: look for friends in a Tenchu Z lobby with
  // a lower SteamID, and ask for that lobby's data (OnLobbyDataUpdate joins).
  void MaybeAutoJoin() {
    if (!REXCVAR_GET(tenchuz_steam_autojoin)) return;
    const auto now = std::chrono::steady_clock::now();
    if (now < next_scan_) return;
    next_scan_ = now + std::chrono::seconds(3);
    if (!lobby_.IsValid() || !Alone()) return;
    const int friends = SteamFriends()->GetFriendCount(k_EFriendFlagImmediate);
    for (int i = 0; i < friends; ++i) {
      const CSteamID id = SteamFriends()->GetFriendByIndex(i, k_EFriendFlagImmediate);
      FriendGameInfo_t game;
      if (!SteamFriends()->GetFriendGamePlayed(id, &game) || game.m_gameID.AppID() != kAppId) continue;
      if (!game.m_steamIDLobby.IsValid() || game.m_steamIDLobby == lobby_) continue;
      if (id.ConvertToUint64() > self_.ConvertToUint64()) continue;  // they come to us
      if (std::find(candidates_.begin(), candidates_.end(), game.m_steamIDLobby) == candidates_.end()) {
        candidates_.push_back(game.m_steamIDLobby);
      }
      SteamMatchmaking()->RequestLobbyData(game.m_steamIDLobby);
    }
  }

  void UpdatePeers() {
    std::unordered_map<uint32_t, CSteamID> peers;
    const int members = lobby_.IsValid() ? SteamMatchmaking()->GetNumLobbyMembers(lobby_) : 0;
    for (int i = 0; i < members; ++i) {
      const CSteamID id = SteamMatchmaking()->GetLobbyMemberByIndex(lobby_, i);
      if (id == self_) continue;
      const uint32_t ip = VirtualIp(id);
      if (ip == local_ip_ || peers.count(ip)) {
        REXLOG_WARN("tenchuz: netplay steam: {} has the same virtual address as another player", id.ConvertToUint64());
        continue;
      }
      peers[ip] = id;
    }
    std::string list;
    for (auto& [ip, id] : peers) list += " " + std::string(SteamFriends()->GetFriendPersonaName(id)) + "=" + IpText(ip);
    REXLOG_INFO("tenchuz: netplay steam: lobby {} with {} other player(s):{}", lobby_.ConvertToUint64(), peers.size(),
                list);
    std::lock_guard lock(peers_mutex_);
    peers_ = std::move(peers);
  }

  CSteamID self_;
  uint32_t local_ip_ = 0;
  CSteamID lobby_;
  CCallResult<SteamSession, LobbyCreated_t> lobby_created_;
  std::vector<CSteamID> candidates_;
  std::chrono::steady_clock::time_point next_scan_{};
  std::mutex peers_mutex_;
  std::unordered_map<uint32_t, CSteamID> peers_;
};

class SteamTransport final : public Transport {
 public:
  bool Start() override {
    // steam_api64.dll is delay-loaded: without it the game still runs.
    if (!LoadLibraryW(L"steam_api64.dll")) {
      REXLOG_WARN("tenchuz: netplay steam: steam_api64.dll is missing next to the game");
      return false;
    }
    // Spacewar, without a steam_appid.txt.
    SetEnvironmentVariableW(L"SteamAppId", L"480");
    SetEnvironmentVariableW(L"SteamGameId", L"480");
    SteamErrMsg error = {};
    if (SteamAPI_InitEx(&error) != k_ESteamAPIInitResult_OK) {
      REXLOG_WARN("tenchuz: netplay steam: Steam didn't start: {}", error);
      return false;
    }
    session_ = std::make_unique<SteamSession>();
    return true;
  }

  uint32_t LocalIp() const override { return session_->local_ip(); }
  void Send(uint32_t dst_ip, const uint8_t* data, size_t size, bool reliable) override {
    session_->Send(dst_ip, data, size, reliable);
  }
  void Receive(int timeout_ms, const Sink& sink) override { session_->Receive(timeout_ms, sink); }
  std::string PlayerName() const override { return session_->name(); }

 private:
  std::unique_ptr<SteamSession> session_;
};

}  // namespace

std::unique_ptr<Transport> CreateSteamTransport() { return std::make_unique<SteamTransport>(); }

}  // namespace tenchuz::net
