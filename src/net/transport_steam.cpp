// tenchuz - System Link over Steam (see transport.h).
//
// Players meet in a friends-only Steam lobby and frames travel with
// ISteamNetworkingMessages, relayed by Steam: no ports to open. Every lobby
// member is 10.x.y.z on the virtual LAN, from its SteamID, and a broadcast
// goes to every member. Runs as App ID 480 (Spacewar), Valve's shared test
// app.
//
// Attaching to Steam: StartSteam() runs at startup, before SDL opens the
// controllers, and when Steam isn't running yet keeps trying in the
// background. On Windows, SteamAPI_InitEx() loads Steam's overlay into the
// game, which hides the controllers Steam Input takes over (all of them, for
// Spacewar); the input code reads those through the Steam Input API
// (input/steam_input.h). macOS has no virtual controller, so there SDL keeps
// the real one (TenchuzApp::OnPreSetup).
//
// Getting into one lobby: each player opens a lobby when Steam is there.
// Accepting a Steam invite (Steam friends list > Invite to Lobby, or Invite in
// the game menu's Online tab) moves you to the inviter's lobby, Join there (or
// Join Game in Steam) moves you to a friend's, and with
// --tenchuz_steam_autojoin a player alone in their lobby joins a friend's
// Tenchu Z lobby by itself (the lower SteamID hosts, so two players starting
// together meet in one lobby).

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <stdlib.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
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
constexpr const char* kConnectPrefix = "+connect_lobby ";

std::atomic<bool> g_ready{false};

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

void SetEnv(const char* name, const char* value) {
#if defined(_WIN32)
  SetEnvironmentVariableA(name, value);
#else
  setenv(name, value, 1);
#endif
}

// The Steam API library next to the game. On Windows it is delay-loaded, and
// on macOS weakly linked: without it at launch its functions are null.
bool LoadSteamApi() {
#if defined(_WIN32)
  return LoadLibraryW(L"steam_api64.dll") != nullptr;
#else
  return dlsym(RTLD_DEFAULT, "SteamAPI_IsSteamRunning") != nullptr;
#endif
}

// One attempt to attach to Steam. Logs each new reason it couldn't.
bool TryInit() {
  static std::string last_error;
  std::string reason;
  if (!SteamAPI_IsSteamRunning()) {
    reason = "Steam isn't running; online play starts when it is";
  } else {
    SteamErrMsg error = {};
    if (SteamAPI_InitEx(&error) == k_ESteamAPIInitResult_OK) {
      g_ready = true;
      REXLOG_INFO("tenchuz: netplay steam: attached to Steam as App ID {}", kAppId);
      return true;
    }
    reason = std::string("Steam didn't start: ") + error;
  }
  if (reason != last_error) {
    REXLOG_WARN("tenchuz: netplay steam: {}", reason);
    last_error = reason;
  }
  return false;
}

class SteamSession;
std::atomic<SteamSession*> g_session{nullptr};

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
    g_session = this;
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
    const auto start = std::chrono::steady_clock::now();
    for (CSteamID id : targets) {
      SteamNetworkingIdentity identity;
      identity.SetSteamID(id);
      SteamNetworkingMessages()->SendMessageToUser(identity, data, uint32(size), flags, kChannel);
    }
    // The game's thread waits here: a slow send is a frame the game misses.
    const auto took = std::chrono::steady_clock::now() - start;
    static std::atomic<int> slow_budget{20};
    if (took > std::chrono::milliseconds(50) && slow_budget.fetch_sub(1) > 0) {
      REXLOG_WARN("tenchuz: netplay steam: sending took {} ms",
                  std::chrono::duration_cast<std::chrono::milliseconds>(took).count());
    }
  }

  void Receive(int timeout_ms, const Transport::Sink& sink) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
      SteamAPI_RunCallbacks();
      RunActions();
      MaybeScan();
      SteamNetworkingMessage_t* messages[32];
      const int count = SteamNetworkingMessages()->ReceiveMessagesOnChannel(kChannel, messages, 32);
      for (int i = 0; i < count; ++i) {
        const CSteamID from = messages[i]->m_identityPeer.GetSteamID();
        Heard(from);
        sink(VirtualIp(from), static_cast<const uint8_t*>(messages[i]->m_pData), size_t(messages[i]->m_cbSize));
        messages[i]->Release();
      }
      CheckQuiet();
      if (count > 0 || std::chrono::steady_clock::now() >= deadline) return;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  std::string name() const { return SteamFriends()->GetPersonaName(); }

  OnlineStatus Status() {
    std::lock_guard lock(status_mutex_);
    return status_;
  }

  // From the game menu (UI thread): runs on the network thread, which runs
  // the Steam callbacks.
  void Post(std::function<void()> action) {
    std::lock_guard lock(actions_mutex_);
    actions_.push_back(std::move(action));
  }

  void JoinFriend(uint64_t id) {
    FriendGameInfo_t game;
    if (SteamFriends()->GetFriendGamePlayed(CSteamID(id), &game) && game.m_steamIDLobby.IsValid()) {
      REXLOG_INFO("tenchuz: netplay steam: joining {}'s lobby", SteamFriends()->GetFriendPersonaName(CSteamID(id)));
      SteamMatchmaking()->JoinLobby(game.m_steamIDLobby);
    }
  }

  void InviteFriend(uint64_t id) {
    if (!lobby_.IsValid()) return;
    const bool sent = SteamMatchmaking()->InviteUserToLobby(lobby_, CSteamID(id));
    REXLOG_INFO("tenchuz: netplay steam: invite to {} {}", SteamFriends()->GetFriendPersonaName(CSteamID(id)),
                sent ? "sent" : "not sent");
  }

 private:
  void OnLobbyCreated(LobbyCreated_t* result, bool io_failure) {
    if (io_failure || result->m_eResult != k_EResultOK) {
      REXLOG_WARN("tenchuz: netplay steam: couldn't open a lobby (result {})", int(result->m_eResult));
      return;
    }
    const CSteamID lobby(result->m_ulSteamIDLobby);
    SteamMatchmaking()->SetLobbyData(lobby, kLobbyKey, "1");
    REXLOG_INFO("tenchuz: netplay steam: opened lobby {}; friends can join from Steam or the game menu",
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
    // "connect" gives friends Join Game in the Steam friends list
    // (GameRichPresenceJoinRequested_t below).
    SteamFriends()->SetRichPresence("status", "Tenchu Z: System Link");
    SteamFriends()->SetRichPresence("connect", (kConnectPrefix + std::to_string(lobby.ConvertToUint64())).c_str());
    UpdatePeers();
    next_scan_ = {};
  }

  STEAM_CALLBACK(SteamSession, OnLobbyChatUpdate, LobbyChatUpdate_t) {
    if (CSteamID(pParam->m_ulSteamIDLobby) != lobby_) return;
    const CSteamID who(pParam->m_ulSteamIDUserChanged);
    const bool joined = pParam->m_rgfChatMemberStateChange & k_EChatMemberStateChangeEntered;
    REXLOG_INFO("tenchuz: netplay steam: {} {} the lobby", SteamFriends()->GetFriendPersonaName(who),
                joined ? "joined" : "left");
    UpdatePeers();
    next_scan_ = {};
  }

  STEAM_CALLBACK(SteamSession, OnJoinRequested, GameLobbyJoinRequested_t) {
    REXLOG_INFO("tenchuz: netplay steam: joining {}'s lobby", SteamFriends()->GetFriendPersonaName(pParam->m_steamIDFriend));
    SteamMatchmaking()->JoinLobby(pParam->m_steamIDLobby);
  }

  STEAM_CALLBACK(SteamSession, OnRichPresenceJoinRequested, GameRichPresenceJoinRequested_t) {
    const std::string connect = pParam->m_rgchConnect;
    if (!connect.starts_with(kConnectPrefix)) return;
    const CSteamID lobby(uint64(std::strtoull(connect.c_str() + std::strlen(kConnectPrefix), nullptr, 10)));
    if (!lobby.IsValid() || lobby == lobby_) return;
    REXLOG_INFO("tenchuz: netplay steam: joining {}'s lobby", SteamFriends()->GetFriendPersonaName(pParam->m_steamIDFriend));
    SteamMatchmaking()->JoinLobby(lobby);
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
    auto it = lobbies_.find(lobby.ConvertToUint64());
    if (it == lobbies_.end()) return;
    it->second.tenchu = IsTenchuLobby(lobby);
    next_scan_ = {};  // the scan joins or lists it
  }

  static bool IsTenchuLobby(CSteamID lobby) {
    return std::strcmp(SteamMatchmaking()->GetLobbyData(lobby, kLobbyKey), "1") == 0;
  }

  bool Alone() const { return !lobby_.IsValid() || SteamMatchmaking()->GetNumLobbyMembers(lobby_) <= 1; }

  void RunActions() {
    std::vector<std::function<void()>> actions;
    {
      std::lock_guard lock(actions_mutex_);
      actions.swap(actions_);
    }
    for (auto& action : actions) action();
  }

  // Every few seconds: the friends list for the game menu, and the lobbies
  // of friends running App ID 480 (their data says whether it's Tenchu Z).
  // While alone, a friend's Tenchu Z lobby with a lower SteamID gets joined
  // (OnLobbyDataUpdate).
  void MaybeScan() {
    const auto now = std::chrono::steady_clock::now();
    if (now < next_scan_) return;
    next_scan_ = now + std::chrono::seconds(3);
    OnlineStatus status;
    status.steam = true;
    status.name = SteamFriends()->GetPersonaName();
    status.lobby_players = lobby_.IsValid() ? SteamMatchmaking()->GetNumLobbyMembers(lobby_) : 0;
    bool alone = lobby_.IsValid() && Alone();
    const int friends = SteamFriends()->GetFriendCount(k_EFriendFlagImmediate);
    for (int i = 0; i < friends; ++i) {
      const CSteamID id = SteamFriends()->GetFriendByIndex(i, k_EFriendFlagImmediate);
      if (SteamFriends()->GetFriendPersonaState(id) == k_EPersonaStateOffline) continue;
      OnlineFriend entry;
      entry.id = id.ConvertToUint64();
      entry.name = SteamFriends()->GetFriendPersonaName(id);
      FriendGameInfo_t game;
      const bool spacewar = SteamFriends()->GetFriendGamePlayed(id, &game) && game.m_gameID.AppID() == kAppId;
      if (spacewar && game.m_steamIDLobby.IsValid()) {
        if (game.m_steamIDLobby == lobby_) {
          entry.with_you = true;
        } else {
          // Its data says whether it's Tenchu Z or another App ID 480 game:
          // asked once, and again every 30 s while it isn't.
          LobbyInfo& info = lobbies_[game.m_steamIDLobby.ConvertToUint64()];
          if (!info.tenchu && now >= info.next_request) {
            info.next_request = now + std::chrono::seconds(30);
            SteamMatchmaking()->RequestLobbyData(game.m_steamIDLobby);
          }
          entry.in_game = info.tenchu || IsTenchuLobby(game.m_steamIDLobby);
        }
        if (entry.in_game && alone && REXCVAR_GET(tenchuz_steam_autojoin) &&
            id.ConvertToUint64() < self_.ConvertToUint64()) {
          REXLOG_INFO("tenchuz: netplay steam: joining {}'s lobby", entry.name);
          SteamMatchmaking()->JoinLobby(game.m_steamIDLobby);
          alone = false;
        }
      }
      LogFriend(entry, spacewar, spacewar && game.m_steamIDLobby.IsValid());
      status.friends.push_back(std::move(entry));
    }
    std::stable_sort(status.friends.begin(), status.friends.end(), [](const OnlineFriend& a, const OnlineFriend& b) {
      return (a.with_you || a.in_game) > (b.with_you || b.in_game);
    });
    std::lock_guard lock(status_mutex_);
    status_ = std::move(status);
  }

  // A lobby member sending nothing for kQuietAfter is logged with what Steam
  // says about the connection, and again when it sends: whether a lost
  // connection was the network or the other game. (The game sends several
  // times a second while players are together.)
  static constexpr auto kQuietAfter = std::chrono::seconds(2);

  void Heard(CSteamID from) {
    const auto now = std::chrono::steady_clock::now();
    Peer& peer = heard_[from.ConvertToUint64()];
    if (peer.quiet) {
      REXLOG_WARN("tenchuz: netplay steam: {} sent again after {:.1f} s", SteamFriends()->GetFriendPersonaName(from),
                  std::chrono::duration<double>(now - peer.last).count());
      peer.quiet = false;
    }
    peer.last = now;
  }

  void CheckQuiet() {
    const auto now = std::chrono::steady_clock::now();
    if (now < next_quiet_check_) return;
    next_quiet_check_ = now + std::chrono::milliseconds(250);
    for (auto& [id, peer] : heard_) {
      if (peer.quiet || now - peer.last < kQuietAfter) continue;
      bool member = false;
      {
        std::lock_guard lock(peers_mutex_);
        for (auto& [ip, peer_id] : peers_) member |= peer_id.ConvertToUint64() == id;
      }
      if (!member) continue;  // left: just gone
      peer.quiet = true;
      SteamNetworkingIdentity identity;
      identity.SetSteamID64(id);
      SteamNetConnectionInfo_t info = {};
      SteamNetConnectionRealTimeStatus_t status = {};
      const int state = SteamNetworkingMessages()->GetSessionConnectionInfo(identity, &info, &status);
      REXLOG_WARN(
          "tenchuz: netplay steam: nothing from {} for {:.1f} s (Steam connection state {}, ping {} ms, "
          "quality {:.2f} here, {:.2f} there){}{}",
          SteamFriends()->GetFriendPersonaName(CSteamID(uint64(id))),
          std::chrono::duration<double>(now - peer.last).count(), state, status.m_nPing,
          status.m_flConnectionQualityLocal, status.m_flConnectionQualityRemote, info.m_szEndDebug[0] ? ": " : "",
          info.m_szEndDebug);
    }
  }

  // What a friend is doing in App ID 480, logged when it changes: why a
  // friend's game shows up or not.
  void LogFriend(const OnlineFriend& entry, bool spacewar, bool lobby) {
    const char* state = !spacewar        ? "not in App ID 480"
                        : !lobby         ? "in App ID 480 without a lobby"
                        : entry.with_you ? "in your lobby"
                        : entry.in_game  ? "in a Tenchu Z lobby"
                                         : "in an App ID 480 lobby, not known as Tenchu Z yet";
    auto [it, added] = friend_states_.try_emplace(entry.id, state);
    if (!added && it->second == state) return;
    it->second = state;
    if (added && !spacewar) return;  // most friends: not worth a line each
    REXLOG_INFO("tenchuz: netplay steam: {} is {}", entry.name, state);
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
  struct LobbyInfo {
    bool tenchu = false;
    std::chrono::steady_clock::time_point next_request{};
  };
  std::unordered_map<uint64_t, LobbyInfo> lobbies_;  // friends' App ID 480 lobbies
  std::unordered_map<uint64_t, std::string> friend_states_;  // LogFriend
  struct Peer {
    std::chrono::steady_clock::time_point last;
    bool quiet = false;
  };
  std::unordered_map<uint64_t, Peer> heard_;  // Heard, CheckQuiet (network thread)
  std::chrono::steady_clock::time_point next_quiet_check_{};
  std::chrono::steady_clock::time_point next_scan_{};
  std::mutex peers_mutex_;
  std::unordered_map<uint32_t, CSteamID> peers_;
  std::mutex status_mutex_;
  OnlineStatus status_;
  std::mutex actions_mutex_;
  std::vector<std::function<void()>> actions_;
};

class SteamTransport final : public Transport {
 public:
  ~SteamTransport() override {
    if (g_session == session_.get()) g_session = nullptr;
  }

  bool Start() override {
    if (!g_ready) return false;
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

void StartSteam() {
  static std::once_flag once;
  std::call_once(once, [] {
    if (!LoadSteamApi()) {
      REXLOG_WARN("tenchuz: netplay steam: the Steam API library is missing next to the game");
      return;
    }
    // Spacewar, without a steam_appid.txt.
    SetEnv("SteamAppId", "480");
    SetEnv("SteamGameId", "480");
    if (TryInit()) return;
    std::thread([] {
      while (!TryInit()) std::this_thread::sleep_for(std::chrono::seconds(5));
    }).detach();
  });
}

bool SteamReady() { return g_ready; }

std::unique_ptr<Transport> CreateSteamTransport() { return std::make_unique<SteamTransport>(); }

OnlineStatus GetOnlineStatus() {
  if (SteamSession* session = g_session) return session->Status();
  OnlineStatus status;
  status.steam_starting = g_ready;
  return status;
}

void JoinFriend(uint64_t id) {
  if (SteamSession* session = g_session) session->Post([session, id] { session->JoinFriend(id); });
}

void InviteFriend(uint64_t id) {
  if (SteamSession* session = g_session) session->Post([session, id] { session->InviteFriend(id); });
}

}  // namespace tenchuz::net
