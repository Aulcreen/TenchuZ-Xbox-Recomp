// tenchuz - System Link on a virtual LAN.
//
// The game reaches every NetDll_* / XNet* import through small XDK shims at
// 0x8250C280..0x8250C6F8 that prepend the caller id (r3 = 1) and branch to the
// import, so hooking the shims sees the game's own arguments.
//
// The SDK's sockets are host Winsock sockets without XNet: no link, no keys,
// XNADDRs that can't be resolved, a stubbed WSAEventSelect. Game instances on
// one PC would also fight over the same ports. So here every socket the game
// opens is virtual: each player has a virtual IPv4 address on a LAN that a
// Transport carries (transport.h), datagram sockets (UDP and VDP) exchange
// frames, stream sockets (TCP) run over the transport's reliable channel, and
// broadcasts go to every peer.
//
// What the game does, from logged runs: Create Game opens UDP :1000, VDP :1001,
// TCP :1003 and a broadcast UDP :1004, waits on each with WSAEventSelect(FD_READ)
// on worker threads, and builds its session info with XNetCreateKey and
// XNetGetTitleXnAddr (sub_823752A0). Its NETMSG_* protocol does the rest.

#include "generated/default/tenchuz_pch.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/kernel/xboxkrnl/threading.h>
#include <rex/logging.h>
#include <rex/system/xthread.h>

#include "transport.h"

#ifndef TENCHUZ_STEAM
#define TENCHUZ_STEAM 0
#endif

REXCVAR_DEFINE_STRING(tenchuz_netplay, "auto", "tenchuz",
                      "System Link network: steam (Steam lobby), local (game instances on this PC), "
                      "auto (steam once Steam is running, local until then) or off (the SDK's sockets)");
REXCVAR_DEFINE_BOOL(tenchuz_net_log, false, "tenchuz", "Log the game's socket, XNet and session calls");

namespace {

using tenchuz::net::kBroadcastIp;
using Clock = std::chrono::steady_clock;

// Winsock values (the Xbox uses the Windows ones).
constexpr uint32_t kAfInet = 2;
constexpr uint32_t kSockStream = 1;
constexpr uint32_t kSockDgram = 2;
constexpr uint32_t kFdRead = 0x01, kFdWrite = 0x02, kFdAccept = 0x08, kFdConnect = 0x10, kFdClose = 0x20;
constexpr uint32_t kMsgPeek = 0x2;
constexpr uint32_t kFionbio = 0x8004667E, kFionread = 0x4004667F;
constexpr uint32_t kSolSocket = 0xFFFF, kSoBroadcast = 0x20, kSoError = 0x1007, kSoType = 0x1008;
constexpr uint32_t kWsaEInval = 10022, kWsaEWouldBlock = 10035, kWsaEAlready = 10037,
                   kWsaENotSock = 10038, kWsaEDestAddrReq = 10039, kWsaEMsgSize = 10040,
                   kWsaEAfNoSupport = 10047, kWsaEAddrInUse = 10048, kWsaEConnReset = 10054,
                   kWsaEIsConn = 10056, kWsaENotConn = 10057, kWsaETimedOut = 10060,
                   kWsaEConnRefused = 10061;
constexpr uint32_t kLoopbackIp = 0x7F000001u;

// XNet values.
constexpr uint32_t kXnAddrStatic = 0x04;
constexpr uint32_t kLinkActive100Full = 0x01 | 0x02 | 0x08;

// Frames between players.
enum : uint8_t { kFrameDatagram = 1, kFrameSyn, kFrameSynAck, kFrameData, kFrameFin, kFrameRst };
constexpr uint8_t kFromInitiator = 0x01;
constexpr size_t kStreamChunk = 16 * 1024;
constexpr size_t kMaxQueuedDatagrams = 512;

#pragma pack(push, 1)
struct Frame {
  uint8_t kind;
  uint8_t flags;
  uint16_t src_port;
  uint16_t dst_port;
  uint16_t proto;
  uint32_t conn;
};
#pragma pack(pop)

struct Datagram {
  uint32_t ip;
  uint16_t port;
  std::vector<uint8_t> data;
};

enum class State { kIdle, kListening, kConnecting, kConnected, kRefused };

struct Socket {
  uint32_t handle = 0;
  uint32_t type = 0;
  uint32_t proto = 0;
  uint16_t port = 0;
  bool nonblocking = false;
  bool broadcast = false;
  uint32_t event = 0;
  uint32_t event_mask = 0;
  uint32_t error = 0;  // SO_ERROR
  // Datagram sockets.
  std::deque<Datagram> datagrams;
  uint32_t default_ip = 0;
  uint16_t default_port = 0;
  // Stream sockets.
  State state = State::kIdle;
  uint32_t peer_ip = 0;
  uint16_t peer_port = 0;
  uint32_t conn = 0;
  bool initiator = false;
  bool fin_sent = false;
  bool peer_closed = false;
  bool reset = false;
  std::deque<uint8_t> stream;
  std::deque<std::shared_ptr<Socket>> backlog;
  size_t backlog_max = 8;
};

using ConnKey = std::tuple<uint32_t, uint32_t, bool>;  // peer ip, conn id, we initiated

struct Net {
  std::mutex mutex;
  std::condition_variable changed;
  std::unique_ptr<tenchuz::net::Transport> transport;
  uint32_t local_ip = 0;
  std::string name;  // from the transport (Steam name); empty keeps the profile's
  bool waiting_for_steam = false;  // on local until Steam is there (Pump switches)
  uint8_t* base = nullptr;         // guest memory, for PatchUsers after a switch
  std::unordered_map<uint32_t, std::shared_ptr<Socket>> sockets;
  std::map<ConnKey, std::shared_ptr<Socket>> connections;
  uint32_t next_handle = 0x5E000004u;
  uint16_t next_port = 49152;
  uint32_t next_conn = 1;
  uint8_t kid[8] = {};
  std::mt19937_64 random{std::random_device{}()};
};

Net& N() {
  static Net* net = new Net;  // never destroyed: the pump thread outlives statics
  return *net;
}

bool Logging() { return REXCVAR_GET(tenchuz_net_log); }
bool Budget(std::atomic<int>& left) { return left.fetch_sub(1, std::memory_order_relaxed) > 0; }

std::string IpText(uint32_t ip) {
  return fmt::format("{}.{}.{}.{}", ip >> 24, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
}

std::string Hex(const uint8_t* data, size_t size, size_t max = 48) {
  std::string out;
  const size_t n = std::min(size, max);
  for (size_t i = 0; i < n; ++i) out += fmt::format("{:02X}", data[i]);
  if (n < size) out += "..";
  return out;
}

void Pump();

bool WantsSteam(const std::string& mode) { return TENCHUZ_STEAM && (mode == "steam" || mode == "auto"); }

// Steam once it's there (StartSteam at startup), and until then the local
// network, which Pump swaps for Steam when Steam comes.
std::unique_ptr<tenchuz::net::Transport> StartTransport(const std::string& mode) {
  std::unique_ptr<tenchuz::net::Transport> transport;
#if TENCHUZ_STEAM
  if (WantsSteam(mode)) {
    tenchuz::net::StartSteam();
    transport = tenchuz::net::CreateSteamTransport();
    if (!transport->Start()) {
      transport.reset();
      N().waiting_for_steam = true;
    }
  }
#else
  if (mode == "steam") REXLOG_WARN("tenchuz: this build has no Steam (no Steamworks SDK at build time)");
#endif
  if (!transport && mode != "off") {
    transport = tenchuz::net::CreateLocalTransport();
    if (!transport->Start()) transport.reset();
  }
  return transport;
}

// Starts the transport on first use. False: --tenchuz_netplay=off, or it
// couldn't start; the hooks then hand everything to the SDK.
bool Active() {
  static std::once_flag once;
  static bool active = false;
  std::call_once(once, [] {
    const std::string mode = REXCVAR_GET(tenchuz_netplay);
    if (mode == "off") return;
    if (mode != "auto" && mode != "steam" && mode != "local") {
      REXLOG_WARN("tenchuz: unknown --tenchuz_netplay={}", mode);
      return;
    }
    auto transport = StartTransport(mode);
    if (!transport) {
      REXLOG_WARN("tenchuz: System Link is off (the network didn't start)");
      return;
    }
    Net& n = N();
    n.local_ip = transport->LocalIp();
    n.name = transport->PlayerName();
    n.next_conn = uint32_t(n.random()) | 1;
    n.transport = std::move(transport);
    std::thread(Pump).detach();
    active = true;
  });
  return active;
}

bool IsBroadcast(uint32_t ip) { return ip == kBroadcastIp || (ip & 0xFF) == 0xFF; }

uint32_t Normalize(uint32_t ip) {
  Net& n = N();
  return (ip == kLoopbackIp || ip == 0) ? n.local_ip : ip;
}

uint32_t Ready(const Socket& s) {
  uint32_t ready = 0;
  if (s.type == kSockDgram) {
    if (!s.datagrams.empty()) ready |= kFdRead;
    return ready | kFdWrite;
  }
  switch (s.state) {
    case State::kListening:
      if (!s.backlog.empty()) ready |= kFdAccept;
      break;
    case State::kConnected:
      if (!s.stream.empty() || s.peer_closed || s.reset) ready |= kFdRead;
      if (s.peer_closed || s.reset) ready |= kFdClose;
      if (!s.reset) ready |= kFdWrite;
      if (s.initiator) ready |= kFdConnect;
      break;
    case State::kRefused:
      ready |= kFdConnect;
      break;
    default:
      break;
  }
  return ready;
}

// After a socket's state changed (lock held): wake select/blocking calls and
// set the socket's WSAEventSelect event.
void Notify(Socket& s) {
  if (s.event && (Ready(s) & s.event_mask)) {
    rex::kernel::xboxkrnl::xeNtSetEvent(s.event, nullptr);
  }
  N().changed.notify_all();
}

std::shared_ptr<Socket> Find(uint32_t handle) {
  Net& n = N();
  auto it = n.sockets.find(handle);
  return it == n.sockets.end() ? nullptr : it->second;
}

std::shared_ptr<Socket> BoundDatagram(uint16_t port) {
  for (auto& [handle, s] : N().sockets) {
    if (s->type == kSockDgram && s->port == port) return s;
  }
  return nullptr;
}

std::shared_ptr<Socket> Listener(uint16_t port) {
  for (auto& [handle, s] : N().sockets) {
    if (s->type == kSockStream && s->state == State::kListening && s->port == port) return s;
  }
  return nullptr;
}

bool PortInUse(uint32_t type, uint16_t port) {
  for (auto& [handle, s] : N().sockets) {
    if (s->type == type && s->port == port) return true;
  }
  return false;
}

uint16_t EphemeralPort(uint32_t type) {
  Net& n = N();
  for (int i = 0; i < 16384; ++i) {
    const uint16_t port = n.next_port;
    n.next_port = n.next_port == 65535 ? 49152 : uint16_t(n.next_port + 1);
    if (!PortInUse(type, port)) return port;
  }
  return 0;
}

std::vector<uint8_t> MakeFrame(uint8_t kind, uint8_t flags, uint16_t src_port, uint16_t dst_port,
                               uint16_t proto, uint32_t conn, const uint8_t* data = nullptr,
                               size_t size = 0) {
  std::vector<uint8_t> frame(sizeof(Frame) + size);
  const Frame header{kind, flags, src_port, dst_port, proto, conn};
  std::memcpy(frame.data(), &header, sizeof(header));
  if (size) std::memcpy(frame.data() + sizeof(header), data, size);
  return frame;
}

void DeliverLocked(uint32_t src_ip, const uint8_t* data, size_t size);

// Sends a frame to a player (lock held). Frames to ourselves are delivered
// directly; broadcasts don't come back to the sender.
void Route(uint32_t dst_ip, const std::vector<uint8_t>& frame, bool reliable) {
  Net& n = N();
  if (IsBroadcast(dst_ip)) {
    n.transport->Send(kBroadcastIp, frame.data(), frame.size(), reliable);
  } else if (Normalize(dst_ip) == n.local_ip) {
    DeliverLocked(n.local_ip, frame.data(), frame.size());
  } else {
    n.transport->Send(dst_ip, frame.data(), frame.size(), reliable);
  }
}

void SendStream(Socket& s, uint8_t kind, const uint8_t* data = nullptr, size_t size = 0) {
  Route(s.peer_ip, MakeFrame(kind, s.initiator ? kFromInitiator : 0, s.port, s.peer_port, 0, s.conn, data, size),
        true);
}

void Disconnect(Socket& s, bool reset) {
  Net& n = N();
  if (s.state == State::kConnected || s.state == State::kConnecting) {
    if (reset) {
      SendStream(s, kFrameRst);
    } else if (!s.fin_sent) {
      SendStream(s, kFrameFin);
      s.fin_sent = true;
    }
    n.connections.erase(ConnKey{s.peer_ip, s.conn, s.initiator});
  }
  for (auto& child : s.backlog) Disconnect(*child, true);
  s.backlog.clear();
}

void DeliverLocked(uint32_t src_ip, const uint8_t* data, size_t size) {
  Net& n = N();
  static std::atomic<int> log_budget{400};
  if (size < sizeof(Frame)) return;
  Frame f;
  std::memcpy(&f, data, sizeof(f));
  const uint8_t* payload = data + sizeof(Frame);
  const size_t payload_size = size - sizeof(Frame);
  if (Logging() && f.kind != kFrameDatagram && Budget(log_budget)) {
    REXLOG_INFO("net frame {} from {}:{} to :{} conn {:08X} ({} bytes)", f.kind, IpText(src_ip),
                f.src_port, f.dst_port, f.conn, payload_size);
  }

  if (f.kind == kFrameDatagram) {
    auto s = BoundDatagram(f.dst_port);
    if (!s || s->datagrams.size() >= kMaxQueuedDatagrams) return;
    s->datagrams.push_back({src_ip, f.src_port, std::vector<uint8_t>(payload, payload + payload_size)});
    Notify(*s);
    return;
  }

  if (f.kind == kFrameSyn) {
    auto listener = Listener(f.dst_port);
    if (!listener || listener->backlog.size() >= listener->backlog_max ||
        n.connections.count(ConnKey{src_ip, f.conn, false})) {
      Route(src_ip, MakeFrame(kFrameRst, 0, f.dst_port, f.src_port, 0, f.conn), true);
      return;
    }
    auto child = std::make_shared<Socket>();
    child->type = kSockStream;
    child->proto = listener->proto;
    child->port = listener->port;
    child->state = State::kConnected;
    child->peer_ip = src_ip;
    child->peer_port = f.src_port;
    child->conn = f.conn;
    child->nonblocking = listener->nonblocking;
    child->event = listener->event;
    child->event_mask = listener->event_mask;
    n.connections[ConnKey{src_ip, f.conn, false}] = child;
    listener->backlog.push_back(child);
    SendStream(*child, kFrameSynAck);
    Notify(*listener);
    return;
  }

  // The rest belongs to a connection; the sender's role picks ours.
  const bool we_initiated = !(f.flags & kFromInitiator);
  auto it = n.connections.find(ConnKey{src_ip, f.conn, we_initiated});
  if (it == n.connections.end()) {
    if (f.kind == kFrameData || f.kind == kFrameSynAck) {
      Route(src_ip, MakeFrame(kFrameRst, we_initiated ? kFromInitiator : 0, f.dst_port, f.src_port, 0, f.conn),
            true);
    }
    return;
  }
  auto s = it->second;
  switch (f.kind) {
    case kFrameSynAck:
      if (s->state == State::kConnecting) s->state = State::kConnected;
      break;
    case kFrameData:
      s->stream.insert(s->stream.end(), payload, payload + payload_size);
      break;
    case kFrameFin:
      s->peer_closed = true;
      break;
    case kFrameRst:
      if (s->state == State::kConnecting) {
        s->state = State::kRefused;
        s->error = kWsaEConnRefused;
      } else {
        s->reset = true;
      }
      n.connections.erase(it);
      break;
    default:
      return;
  }
  Notify(*s);
}

void PatchUsers(uint8_t* base);

#if TENCHUZ_STEAM
// While System Link is closed (no sockets), moves from the local network to
// Steam once Steam is there: new address, Steam name, players' XUIDs again.
void MaybeSwitchToSteam() {
  Net& n = N();
  if (!n.waiting_for_steam || !tenchuz::net::SteamReady()) return;
  std::lock_guard lock(n.mutex);
  if (!n.sockets.empty()) {
    static bool logged = false;
    if (!logged) REXLOG_INFO("tenchuz: netplay: Steam is here; System Link moves to it when this game closes");
    logged = true;
    return;
  }
  auto steam = tenchuz::net::CreateSteamTransport();
  if (!steam->Start()) return;
  n.waiting_for_steam = false;
  n.transport = std::move(steam);
  n.local_ip = n.transport->LocalIp();
  n.name = n.transport->PlayerName();
  n.connections.clear();
  if (n.base) PatchUsers(n.base);
  REXLOG_INFO("tenchuz: netplay: System Link is on Steam now ({})", IpText(n.local_ip));
}
#endif

void Pump() {
  Net& n = N();
  for (;;) {
#if TENCHUZ_STEAM
    MaybeSwitchToSteam();
#endif
    n.transport->Receive(20, [&](uint32_t src_ip, const uint8_t* data, size_t size) {
      std::lock_guard lock(n.mutex);
      DeliverLocked(src_ip, data, size);
    });
  }
}

// Guest-side helpers --------------------------------------------------------

void Fail(PPCContext& ctx, uint32_t error) {
  rex::system::XThread::SetLastError(error);
  ctx.r3.s64 = -1;
  if (Logging() && error != kWsaEWouldBlock) {
    REXLOG_INFO("net   error {} (lr {:08X})", error, uint32_t(ctx.lr));
  }
}

struct Addr {
  uint32_t ip;
  uint16_t port;
};

Addr LoadAddr(uint8_t* base, uint32_t ptr) { return {REX_LOAD_U32(ptr + 4), REX_LOAD_U16(ptr + 2)}; }

void StoreAddr(uint8_t* base, uint32_t ptr, uint32_t len_ptr, Addr addr) {
  if (!ptr) return;
  REX_STORE_U16(ptr, uint16_t(kAfInet));
  REX_STORE_U16(ptr + 2, addr.port);
  REX_STORE_U32(ptr + 4, addr.ip);
  REX_STORE_U32(ptr + 8, 0);
  REX_STORE_U32(ptr + 12, 0);
  if (len_ptr) REX_STORE_U32(len_ptr, 16);
}

// Waits on a blocking socket until ready() (lock held). False on timeout.
template <typename Pred>
bool Wait(std::unique_lock<std::mutex>& lock, Pred ready, int timeout_ms = -1) {
  Net& n = N();
  if (timeout_ms < 0) {
    n.changed.wait(lock, ready);
    return true;
  }
  return n.changed.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready);
}

// XNADDR for a virtual address: the address itself, and a made-up MAC.
void StoreXnAddr(uint8_t* base, uint32_t ptr, uint32_t ip) {
  REX_STORE_U32(ptr, ip);
  REX_STORE_U32(ptr + 4, 0);
  REX_STORE_U16(ptr + 8, 0);
  const uint8_t mac[6] = {0x02, 0x54, 0x5A, uint8_t(ip >> 16), uint8_t(ip >> 8), uint8_t(ip)};
  for (int i = 0; i < 6; ++i) REX_STORE_U8(ptr + 10 + i, mac[i]);
  for (int i = 0; i < 20; ++i) REX_STORE_U8(ptr + 16 + i, 0);
}

void FillRandom(uint8_t* base, uint32_t ptr, uint32_t size) {
  Net& n = N();
  for (uint32_t i = 0; i < size; ++i) REX_STORE_U8(ptr + i, uint8_t(n.random()));
}

#define NET_LOG(...)                       \
  do {                                     \
    if (Logging()) REXLOG_INFO(__VA_ARGS__); \
  } while (0)

// Pass-through with a generic argument log.
#define NET_TRACE(sub, name, nargs)                                                         \
  REX_EXTERN(__imp__##sub);                                                                 \
  REX_HOOK_RAW(sub) {                                                                       \
    if (!Logging()) return __imp__##sub(ctx, base);                                         \
    const uint32_t a[8] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,                  \
                           ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32};                \
    const uint32_t lr = uint32_t(ctx.lr);                                                   \
    __imp__##sub(ctx, base);                                                                \
    std::string args;                                                                       \
    for (int i = 0; i < (nargs); ++i) args += fmt::format("{}{:08X}", i ? " " : "", a[i]);  \
    REXLOG_INFO("net {}({}) = {:08X} lr {:08X}", name, args, ctx.r3.u32, lr);               \
  }

}  // namespace

// Sockets ---------------------------------------------------------------------

REX_EXTERN(__imp__sub_8250C530);
REX_HOOK_RAW(sub_8250C530) {  // socket(af, type, protocol)
  if (!Active()) return __imp__sub_8250C530(ctx, base);
  const uint32_t af = ctx.r3.u32, type = ctx.r4.u32, proto = ctx.r5.u32;
  if (af != kAfInet || (type != kSockStream && type != kSockDgram)) return Fail(ctx, kWsaEAfNoSupport);
  Net& n = N();
  std::lock_guard lock(n.mutex);
  auto s = std::make_shared<Socket>();
  s->handle = n.next_handle;
  n.next_handle += 4;
  s->type = type;
  s->proto = proto;
  n.sockets[s->handle] = s;
  ctx.r3.u64 = s->handle;
  NET_LOG("net socket(type {} proto {}) = {:08X}", type, proto, s->handle);
}

REX_EXTERN(__imp__sub_8250C548);
REX_HOOK_RAW(sub_8250C548) {  // closesocket(s)
  if (!Active()) return __imp__sub_8250C548(ctx, base);
  const uint32_t handle = ctx.r3.u32;
  Net& n = N();
  std::lock_guard lock(n.mutex);
  auto s = Find(handle);
  if (!s) return Fail(ctx, kWsaENotSock);
  Disconnect(*s, false);
  n.sockets.erase(handle);
  n.changed.notify_all();
  ctx.r3.u64 = 0;
  NET_LOG("net closesocket({:08X})", handle);
}

REX_EXTERN(__imp__sub_8250C558);
REX_HOOK_RAW(sub_8250C558) {  // shutdown(s, how)
  if (!Active()) return __imp__sub_8250C558(ctx, base);
  const uint32_t handle = ctx.r3.u32, how = ctx.r4.u32;
  Net& n = N();
  std::lock_guard lock(n.mutex);
  auto s = Find(handle);
  if (!s) return Fail(ctx, kWsaENotSock);
  if (how != 0 && s->state == State::kConnected && !s->fin_sent) {
    SendStream(*s, kFrameFin);
    s->fin_sent = true;
  }
  ctx.r3.u64 = 0;
  NET_LOG("net shutdown({:08X}, {})", handle, how);
}

REX_EXTERN(__imp__sub_8250C568);
REX_HOOK_RAW(sub_8250C568) {  // ioctlsocket(s, cmd, argp)
  if (!Active()) return __imp__sub_8250C568(ctx, base);
  const uint32_t handle = ctx.r3.u32, cmd = ctx.r4.u32, arg = ctx.r5.u32;
  Net& n = N();
  std::lock_guard lock(n.mutex);
  auto s = Find(handle);
  if (!s) return Fail(ctx, kWsaENotSock);
  if (cmd == kFionbio && arg) {
    s->nonblocking = REX_LOAD_U32(arg) != 0;
  } else if (cmd == kFionread && arg) {
    const size_t available = s->type == kSockDgram
                                 ? (s->datagrams.empty() ? 0 : s->datagrams.front().data.size())
                                 : s->stream.size();
    REX_STORE_U32(arg, uint32_t(available));
  }
  ctx.r3.u64 = 0;
  NET_LOG("net ioctlsocket({:08X}, {:08X})", handle, cmd);
}

REX_EXTERN(__imp__sub_8250C580);
REX_HOOK_RAW(sub_8250C580) {  // setsockopt(s, level, optname, optval, optlen)
  if (!Active()) return __imp__sub_8250C580(ctx, base);
  const uint32_t handle = ctx.r3.u32, level = ctx.r4.u32, opt = ctx.r5.u32, val = ctx.r6.u32,
                 len = ctx.r7.u32;
  Net& n = N();
  std::lock_guard lock(n.mutex);
  auto s = Find(handle);
  if (!s) return Fail(ctx, kWsaENotSock);
  if (level == kSolSocket && opt == kSoBroadcast && val && len >= 4) {
    s->broadcast = REX_LOAD_U32(val) != 0;
  }
  ctx.r3.u64 = 0;
  NET_LOG("net setsockopt({:08X}, {:X}, {:X})", handle, level, opt);
}

REX_EXTERN(__imp__sub_8250C5A0);
REX_HOOK_RAW(sub_8250C5A0) {  // getsockopt(s, level, optname, optval, optlen)
  if (!Active()) return __imp__sub_8250C5A0(ctx, base);
  const uint32_t handle = ctx.r3.u32, level = ctx.r4.u32, opt = ctx.r5.u32, val = ctx.r6.u32,
                 len_ptr = ctx.r7.u32;
  Net& n = N();
  std::lock_guard lock(n.mutex);
  auto s = Find(handle);
  if (!s) return Fail(ctx, kWsaENotSock);
  uint32_t value = 0;
  if (level == kSolSocket && opt == kSoType) value = s->type;
  if (level == kSolSocket && opt == kSoBroadcast) value = s->broadcast;
  if (level == kSolSocket && opt == kSoError) std::swap(value, s->error);
  if (val) REX_STORE_U32(val, value);
  if (len_ptr) REX_STORE_U32(len_ptr, 4);
  ctx.r3.u64 = 0;
  NET_LOG("net getsockopt({:08X}, {:X}, {:X}) = {}", handle, level, opt, value);
}

REX_EXTERN(__imp__sub_8250C5C0);
REX_HOOK_RAW(sub_8250C5C0) {  // getsockname(s, name, namelen)
  if (!Active()) return __imp__sub_8250C5C0(ctx, base);
  const uint32_t handle = ctx.r3.u32, name = ctx.r4.u32, len_ptr = ctx.r5.u32;
  Net& n = N();
  std::lock_guard lock(n.mutex);
  auto s = Find(handle);
  if (!s) return Fail(ctx, kWsaENotSock);
  StoreAddr(base, name, len_ptr, {n.local_ip, s->port});
  ctx.r3.u64 = 0;
  NET_LOG("net getsockname({:08X}) = :{}", handle, s->port);
}

REX_EXTERN(__imp__sub_8250C5D8);
REX_HOOK_RAW(sub_8250C5D8) {  // bind(s, name, namelen)
  if (!Active()) return __imp__sub_8250C5D8(ctx, base);
  const uint32_t handle = ctx.r3.u32, name = ctx.r4.u32;
  Net& n = N();
  std::lock_guard lock(n.mutex);
  auto s = Find(handle);
  if (!s) return Fail(ctx, kWsaENotSock);
  if (!name || s->port) return Fail(ctx, kWsaEInval);
  uint16_t port = LoadAddr(base, name).port;
  if (!port) port = EphemeralPort(s->type);
  if (PortInUse(s->type, port)) return Fail(ctx, kWsaEAddrInUse);
  s->port = port;
  ctx.r3.u64 = 0;
  NET_LOG("net bind({:08X}, :{})", handle, port);
}

REX_EXTERN(__imp__sub_8250C5F0);
REX_HOOK_RAW(sub_8250C5F0) {  // connect(s, name, namelen)
  if (!Active()) return __imp__sub_8250C5F0(ctx, base);
  const uint32_t handle = ctx.r3.u32, name = ctx.r4.u32;
  Net& n = N();
  std::unique_lock lock(n.mutex);
  auto s = Find(handle);
  if (!s) return Fail(ctx, kWsaENotSock);
  if (!name) return Fail(ctx, kWsaEInval);
  const Addr to = LoadAddr(base, name);
  if (!s->port) s->port = EphemeralPort(s->type);
  if (s->type == kSockDgram) {
    s->default_ip = to.ip;
    s->default_port = to.port;
    ctx.r3.u64 = 0;
    NET_LOG("net connect({:08X}, {}:{}) (datagram)", handle, IpText(to.ip), to.port);
    return;
  }
  if (s->state == State::kConnected) return Fail(ctx, kWsaEIsConn);
  if (s->state == State::kConnecting) return Fail(ctx, kWsaEAlready);
  s->state = State::kConnecting;
  s->peer_ip = Normalize(to.ip);
  s->peer_port = to.port;
  s->conn = n.next_conn++;
  s->initiator = true;
  n.connections[ConnKey{s->peer_ip, s->conn, true}] = s;
  NET_LOG("net connect({:08X}, {}:{}) conn {:08X}", handle, IpText(s->peer_ip), to.port, s->conn);
  SendStream(*s, kFrameSyn);
  if (s->nonblocking) {
    if (s->state == State::kConnected) {
      ctx.r3.u64 = 0;
      return;
    }
    return Fail(ctx, s->state == State::kRefused ? kWsaEConnRefused : kWsaEWouldBlock);
  }
  Wait(lock, [&] { return s->state != State::kConnecting; }, 10000);
  if (s->state == State::kConnected) {
    ctx.r3.u64 = 0;
  } else if (s->state == State::kRefused) {
    Fail(ctx, kWsaEConnRefused);
  } else {
    Disconnect(*s, true);
    s->state = State::kIdle;
    Fail(ctx, kWsaETimedOut);
  }
}

REX_EXTERN(__imp__sub_8250C608);
REX_HOOK_RAW(sub_8250C608) {  // listen(s, backlog)
  if (!Active()) return __imp__sub_8250C608(ctx, base);
  const uint32_t handle = ctx.r3.u32, backlog = ctx.r4.u32;
  Net& n = N();
  std::lock_guard lock(n.mutex);
  auto s = Find(handle);
  if (!s) return Fail(ctx, kWsaENotSock);
  if (s->type != kSockStream || s->state != State::kIdle) return Fail(ctx, kWsaEInval);
  if (!s->port) s->port = EphemeralPort(s->type);
  s->state = State::kListening;
  s->backlog_max = std::clamp<uint32_t>(backlog, 1, 64);
  ctx.r3.u64 = 0;
  NET_LOG("net listen({:08X}, :{})", handle, s->port);
}

REX_EXTERN(__imp__sub_8250C618);
REX_HOOK_RAW(sub_8250C618) {  // accept(s, addr, addrlen)
  if (!Active()) return __imp__sub_8250C618(ctx, base);
  const uint32_t handle = ctx.r3.u32, addr = ctx.r4.u32, len_ptr = ctx.r5.u32;
  Net& n = N();
  std::unique_lock lock(n.mutex);
  auto s = Find(handle);
  if (!s) return Fail(ctx, kWsaENotSock);
  if (s->state != State::kListening) return Fail(ctx, kWsaEInval);
  if (s->backlog.empty()) {
    if (s->nonblocking) return Fail(ctx, kWsaEWouldBlock);
    Wait(lock, [&] { return !s->backlog.empty() || !Find(handle); });
    if (s->backlog.empty()) return Fail(ctx, kWsaEInval);
  }
  auto child = s->backlog.front();
  s->backlog.pop_front();
  child->handle = n.next_handle;
  n.next_handle += 4;
  n.sockets[child->handle] = child;
  StoreAddr(base, addr, len_ptr, {child->peer_ip, child->peer_port});
  ctx.r3.u64 = child->handle;
  NET_LOG("net accept({:08X}) = {:08X} from {}:{}", handle, child->handle, IpText(child->peer_ip),
          child->peer_port);
}

namespace {

// Takes up to len bytes for recv/recvfrom (lock held). -1: an error was set.
int32_t Receive(PPCContext& ctx, uint8_t* base, std::unique_lock<std::mutex>& lock, Socket& s,
                uint32_t buf, uint32_t len, uint32_t flags, Addr* from) {
  const bool peek = flags & kMsgPeek;
  if (s.type == kSockDgram) {
    if (s.datagrams.empty()) {
      if (s.nonblocking) return Fail(ctx, kWsaEWouldBlock), -1;
      const uint32_t handle = s.handle;
      Wait(lock, [&] { return !s.datagrams.empty() || !Find(handle); });
      if (s.datagrams.empty()) return Fail(ctx, kWsaENotSock), -1;
    }
    Datagram& d = s.datagrams.front();
    const uint32_t size = uint32_t(d.data.size());
    const uint32_t n = std::min(size, len);
    if (n) std::memcpy(REX_RAW_ADDR(buf), d.data.data(), n);
    if (from) *from = {d.ip, d.port};
    if (!peek) s.datagrams.pop_front();
    if (size > len) return Fail(ctx, kWsaEMsgSize), -1;
    return int32_t(n);
  }
  if (s.state != State::kConnected) return Fail(ctx, kWsaENotConn), -1;
  if (s.stream.empty() && !s.peer_closed && !s.reset) {
    if (s.nonblocking) return Fail(ctx, kWsaEWouldBlock), -1;
    const uint32_t handle = s.handle;
    Wait(lock, [&] { return !s.stream.empty() || s.peer_closed || s.reset || !Find(handle); });
  }
  if (s.stream.empty()) {
    if (s.reset) return Fail(ctx, kWsaEConnReset), -1;
    return 0;
  }
  const uint32_t n = std::min<uint32_t>(uint32_t(s.stream.size()), len);
  std::copy_n(s.stream.begin(), n, REX_RAW_ADDR(buf));
  if (!peek) s.stream.erase(s.stream.begin(), s.stream.begin() + n);
  if (from) *from = {s.peer_ip, s.peer_port};
  return int32_t(n);
}

// Sends for send/sendto (lock held). -1: an error was set.
int32_t Send(PPCContext& ctx, uint8_t* base, Socket& s, uint32_t buf, uint32_t len, const Addr* to) {
  const uint8_t* data = REX_RAW_ADDR(buf);
  if (s.type == kSockDgram) {
    Addr dst = to ? *to : Addr{s.default_ip, s.default_port};
    if (!dst.port) return Fail(ctx, kWsaEDestAddrReq), -1;
    if (!s.port) s.port = EphemeralPort(s.type);
    Route(dst.ip, MakeFrame(kFrameDatagram, 0, s.port, dst.port, uint16_t(s.proto), 0, data, len), false);
    return int32_t(len);
  }
  if (s.state != State::kConnected) return Fail(ctx, kWsaENotConn), -1;
  if (s.reset) return Fail(ctx, kWsaEConnReset), -1;
  for (uint32_t offset = 0; offset < len; offset += uint32_t(kStreamChunk)) {
    SendStream(s, kFrameData, data + offset, std::min<size_t>(kStreamChunk, len - offset));
  }
  return int32_t(len);
}

}  // namespace

REX_EXTERN(__imp__sub_8250C650);
REX_HOOK_RAW(sub_8250C650) {  // recv(s, buf, len, flags)
  if (!Active()) return __imp__sub_8250C650(ctx, base);
  static std::atomic<int> budget{400};
  const uint32_t handle = ctx.r3.u32, buf = ctx.r4.u32, len = ctx.r5.u32, flags = ctx.r6.u32;
  Net& n = N();
  std::unique_lock lock(n.mutex);
  auto s = Find(handle);
  if (!s) return Fail(ctx, kWsaENotSock);
  const int32_t got = Receive(ctx, base, lock, *s, buf, len, flags, nullptr);
  if (got >= 0) ctx.r3.s64 = got;
  if (got >= 0 && Logging() && Budget(budget)) {
    REXLOG_INFO("net recv({:08X}) = {} {}", handle, got, Hex(REX_RAW_ADDR(buf), uint32_t(got)));
  }
}

REX_EXTERN(__imp__sub_8250C668);
REX_HOOK_RAW(sub_8250C668) {  // recvfrom(s, buf, len, flags, from, fromlen)
  if (!Active()) return __imp__sub_8250C668(ctx, base);
  static std::atomic<int> budget{400};
  const uint32_t handle = ctx.r3.u32, buf = ctx.r4.u32, len = ctx.r5.u32, flags = ctx.r6.u32,
                 from = ctx.r7.u32, from_len = ctx.r8.u32;
  Net& n = N();
  std::unique_lock lock(n.mutex);
  auto s = Find(handle);
  if (!s) return Fail(ctx, kWsaENotSock);
  Addr source{};
  const int32_t got = Receive(ctx, base, lock, *s, buf, len, flags, &source);
  if (got < 0) return;
  StoreAddr(base, from, from_len, source);
  ctx.r3.s64 = got;
  if (Logging() && Budget(budget)) {
    REXLOG_INFO("net recvfrom({:08X}) = {} from {}:{} {}", handle, got, IpText(source.ip), source.port,
                Hex(REX_RAW_ADDR(buf), uint32_t(got)));
  }
}

REX_EXTERN(__imp__sub_8250C688);
REX_HOOK_RAW(sub_8250C688) {  // send(s, buf, len, flags)
  if (!Active()) return __imp__sub_8250C688(ctx, base);
  static std::atomic<int> budget{400};
  const uint32_t handle = ctx.r3.u32, buf = ctx.r4.u32, len = ctx.r5.u32;
  Net& n = N();
  std::lock_guard lock(n.mutex);
  auto s = Find(handle);
  if (!s) return Fail(ctx, kWsaENotSock);
  const int32_t sent = Send(ctx, base, *s, buf, len, nullptr);
  if (sent >= 0) ctx.r3.s64 = sent;
  if (Logging() && Budget(budget)) {
    REXLOG_INFO("net send({:08X}, {}) = {} {}", handle, len, sent, Hex(REX_RAW_ADDR(buf), len));
  }
}

REX_EXTERN(__imp__sub_8250C6A0);
REX_HOOK_RAW(sub_8250C6A0) {  // sendto(s, buf, len, flags, to, tolen)
  if (!Active()) return __imp__sub_8250C6A0(ctx, base);
  static std::atomic<int> budget{400};
  const uint32_t handle = ctx.r3.u32, buf = ctx.r4.u32, len = ctx.r5.u32, to_ptr = ctx.r7.u32;
  Net& n = N();
  std::lock_guard lock(n.mutex);
  auto s = Find(handle);
  if (!s) return Fail(ctx, kWsaENotSock);
  Addr to{};
  if (to_ptr) to = LoadAddr(base, to_ptr);
  const int32_t sent = Send(ctx, base, *s, buf, len, to_ptr ? &to : nullptr);
  if (sent >= 0) ctx.r3.s64 = sent;
  if (Logging() && Budget(budget)) {
    REXLOG_INFO("net sendto({:08X}, {}:{}, {}) = {} {}", handle, IpText(to.ip), to.port, len, sent,
                Hex(REX_RAW_ADDR(buf), len));
  }
}

REX_EXTERN(__imp__sub_8250C630);
REX_HOOK_RAW(sub_8250C630) {  // select(nfds, readfds, writefds, exceptfds, timeout)
  if (!Active()) return __imp__sub_8250C630(ctx, base);
  const uint32_t sets[3] = {ctx.r4.u32, ctx.r5.u32, ctx.r6.u32};
  const uint32_t timeout = ctx.r7.u32;
  Net& n = N();
  std::unique_lock lock(n.mutex);

  std::vector<uint32_t> handles[3];
  for (int i = 0; i < 3; ++i) {
    if (!sets[i]) continue;
    const uint32_t count = std::min<uint32_t>(REX_LOAD_U32(sets[i]), 64);
    for (uint32_t j = 0; j < count; ++j) handles[i].push_back(REX_LOAD_U32(sets[i] + 4 + j * 4));
  }
  auto ready = [&](int set, uint32_t handle) {
    auto s = Find(handle);
    if (!s) return false;
    const uint32_t r = Ready(*s);
    if (set == 0) return (r & (kFdRead | kFdAccept)) != 0;
    if (set == 1) return (r & kFdWrite) != 0;
    return s->state == State::kRefused;
  };
  auto count = [&] {
    int total = 0;
    for (int i = 0; i < 3; ++i) {
      for (uint32_t h : handles[i]) total += ready(i, h);
    }
    return total;
  };
  if (!count()) {
    int timeout_ms = -1;
    if (timeout) {
      timeout_ms = int(REX_LOAD_U32(timeout) * 1000 + REX_LOAD_U32(timeout + 4) / 1000);
    }
    if (timeout_ms != 0) Wait(lock, [&] { return count() > 0; }, timeout_ms);
  }
  int total = 0;
  for (int i = 0; i < 3; ++i) {
    if (!sets[i]) continue;
    uint32_t kept = 0;
    for (uint32_t h : handles[i]) {
      if (!ready(i, h)) continue;
      const uint32_t at = sets[i] + 4 + kept++ * 4;  // REX_STORE_U32 evaluates its address twice
      REX_STORE_U32(at, h);
    }
    REX_STORE_U32(sets[i], kept);
    total += int(kept);
  }
  ctx.r3.s64 = total;
}

REX_EXTERN(__imp__sub_8250C6C0);
REX_HOOK_RAW(sub_8250C6C0) {  // WSAEventSelect(s, event, mask)
  if (!Active()) return __imp__sub_8250C6C0(ctx, base);
  const uint32_t handle = ctx.r3.u32, event = ctx.r4.u32, mask = ctx.r5.u32;
  Net& n = N();
  std::lock_guard lock(n.mutex);
  auto s = Find(handle);
  if (!s) return Fail(ctx, kWsaENotSock);
  s->event = event;
  s->event_mask = mask;
  s->nonblocking = true;
  Notify(*s);
  ctx.r3.u64 = 0;
  NET_LOG("net WSAEventSelect({:08X}, {:08X}, {:X})", handle, event, mask);
}

REX_EXTERN(__imp__sub_8250C6F8);
REX_HOOK_RAW(sub_8250C6F8) {  // WSAWaitForMultipleEvents(count, events, wait_all, timeout, alertable)
  // The SDK passes the timeout (ms) as an absolute NT time, so every wait
  // ends at once, reports a timeout as event 0, and waits for all events when
  // asked for any. The game's TCP reader then called recv on a socket that
  // wasn't connected yet and closed it for good.
  if (!Active()) return __imp__sub_8250C6F8(ctx, base);
  const uint32_t count = ctx.r3.u32, events = ctx.r4.u32, wait_all = ctx.r5.u32,
                 timeout_ms = ctx.r6.u32, alertable = ctx.r7.u32;
  constexpr uint32_t kWaitFailed = 0xFFFFFFFFu, kWaitIoCompletion = 0xC0, kWaitTimeout = 0x102;
  if (!count || count > 64 || !events) {
    rex::system::XThread::SetLastError(87);  // ERROR_INVALID_PARAMETER
    ctx.r3.u64 = kWaitFailed;
    return;
  }
  uint64_t timeout = uint64_t(-int64_t(timeout_ms) * 10000);  // relative, 100 ns units
  auto* handles = reinterpret_cast<rex::be<uint32_t>*>(REX_RAW_ADDR(events));
  const uint32_t status = rex::kernel::xboxkrnl::xeNtWaitForMultipleObjectsEx(
      count, handles, wait_all ? 0 : 1, 1, alertable, timeout_ms == 0xFFFFFFFFu ? nullptr : &timeout);
  if (status < count) {
    ctx.r3.u64 = status;  // WSA_WAIT_EVENT_0 + index
  } else if (status >= 0x80 && status < 0x80 + count) {
    ctx.r3.u64 = status - 0x80;  // abandoned
  } else if (status == 0xC0 || status == 0x101) {
    ctx.r3.u64 = kWaitIoCompletion;
  } else if (status == 0x102) {
    ctx.r3.u64 = kWaitTimeout;
  } else {
    rex::system::XThread::SetLastError(6);  // ERROR_INVALID_HANDLE
    ctx.r3.u64 = kWaitFailed;
  }
}

// XNet ------------------------------------------------------------------------

REX_EXTERN(__imp__sub_8250C468);
REX_HOOK_RAW(sub_8250C468) {  // XNetGetEthernetLinkStatus()
  // The SDK reports no link, and System Link says no Ethernet cable is connected.
  if (!Active()) return __imp__sub_8250C468(ctx, base);
  ctx.r3.u64 = kLinkActive100Full;
}

REX_EXTERN(__imp__sub_8250C458);
REX_HOOK_RAW(sub_8250C458) {  // XNetGetTitleXnAddr(pxna)
  if (!Active()) return __imp__sub_8250C458(ctx, base);
  StoreXnAddr(base, ctx.r3.u32, N().local_ip);
  ctx.r3.u64 = kXnAddrStatic;
}

REX_EXTERN(__imp__sub_8250C390);
REX_HOOK_RAW(sub_8250C390) {  // XNetXnAddrToInAddr(pxna, pxnkid, pina)
  if (!Active()) return __imp__sub_8250C390(ctx, base);
  const uint32_t pxna = ctx.r3.u32, pina = ctx.r5.u32;
  if (!pxna || !pina) return void(ctx.r3.u64 = kWsaEInval);
  const uint32_t ip = REX_LOAD_U32(pxna);
  REX_STORE_U32(pina, ip);
  ctx.r3.u64 = 0;
  NET_LOG("net XNetXnAddrToInAddr = {}", IpText(ip));
}

REX_EXTERN(__imp__sub_8250C3A8);
REX_HOOK_RAW(sub_8250C3A8) {  // XNetInAddrToXnAddr(ina, pxna, pxnkid)
  if (!Active()) return __imp__sub_8250C3A8(ctx, base);
  const uint32_t ip = Normalize(ctx.r3.u32), pxna = ctx.r4.u32, pxnkid = ctx.r5.u32;
  if (pxna) StoreXnAddr(base, pxna, ip);
  if (pxnkid) {
    std::lock_guard lock(N().mutex);
    for (int i = 0; i < 8; ++i) REX_STORE_U8(pxnkid + i, N().kid[i]);
  }
  ctx.r3.u64 = 0;
  NET_LOG("net XNetInAddrToXnAddr({})", IpText(ip));
}

REX_EXTERN(__imp__sub_8250C360);
REX_HOOK_RAW(sub_8250C360) {  // XNetCreateKey(pxnkid, pxnkey)
  if (!Active()) return __imp__sub_8250C360(ctx, base);
  const uint32_t kid = ctx.r3.u32, key = ctx.r4.u32;
  std::lock_guard lock(N().mutex);
  FillRandom(base, kid, 8);
  REX_STORE_U8(kid, REX_LOAD_U8(kid) & 0x1F);  // XNET_XNKID_SYSTEM_LINK
  FillRandom(base, key, 16);
  ctx.r3.u64 = 0;
  NET_LOG("net XNetCreateKey");
}

REX_EXTERN(__imp__sub_8250C370);
REX_HOOK_RAW(sub_8250C370) {  // XNetRegisterKey(pxnkid, pxnkey)
  if (!Active()) return __imp__sub_8250C370(ctx, base);
  const uint32_t kid = ctx.r3.u32;
  std::lock_guard lock(N().mutex);
  if (kid) {
    for (int i = 0; i < 8; ++i) N().kid[i] = REX_LOAD_U8(kid + i);
  }
  ctx.r3.u64 = 0;
  NET_LOG("net XNetRegisterKey");
}

REX_EXTERN(__imp__sub_8250C380);
REX_HOOK_RAW(sub_8250C380) {  // XNetUnregisterKey(pxnkid)
  if (!Active()) return __imp__sub_8250C380(ctx, base);
  ctx.r3.u64 = 0;
  NET_LOG("net XNetUnregisterKey");
}

REX_EXTERN(__imp__sub_8250C350);
REX_HOOK_RAW(sub_8250C350) {  // XNetRandom(buffer, length)
  // The SDK fills 0xBB, so every instance made the same session nonce.
  if (!Active()) return __imp__sub_8250C350(ctx, base);
  std::lock_guard lock(N().mutex);
  FillRandom(base, ctx.r3.u32, ctx.r4.u32);
  ctx.r3.u64 = 0;
}

REX_EXTERN(__imp__sub_8250C3C0);
REX_HOOK_RAW(sub_8250C3C0) {  // XNetQosListen(pxnkid, pb, cb, bps, flags)
  if (!Active()) return __imp__sub_8250C3C0(ctx, base);
  ctx.r3.u64 = 0;
  NET_LOG("net XNetQosListen(flags {:X})", ctx.r7.u32);
}

NET_TRACE(sub_8250C3E0, "XNetQosLookup", 8)
NET_TRACE(sub_8250C448, "XNetQosRelease", 1)
NET_TRACE(sub_8250C280, "XNetStartup", 1)
NET_TRACE(sub_8250C348, "XNetCleanup", 0)

// Players --------------------------------------------------------------------

// The SDK gives every profile the XUID B13EBABEBABEBABE, so two players had
// the same one and the client lost the host right after its join request was
// answered. The game copies each signed-in user's XUID into a table when
// sign-in changes (sub_8228BF30); give the copies this player's virtual
// address as their low half. The SDK keeps its own XUID for the save folder,
// and the save itself doesn't store it. With Steam, the name other players
// see (the profile's is "User") becomes the Steam name.
namespace {

void PatchUsers(uint8_t* base) {
  constexpr uint32_t kUsers = 0x82B57390;       // 4 x 32 bytes: XUID, ..., state at +12, name at +16
  constexpr uint32_t kActiveUser = 0x82B57430;  // XUID of the user playing
  constexpr uint32_t kNameSize = 16;
  const Net& n = N();
  auto patch = [&](uint32_t at) {
    const uint64_t xuid = REX_LOAD_U64(at);
    if (xuid && uint32_t(xuid) != n.local_ip) REX_STORE_U64(at, (xuid & 0xFFFFFFFF00000000ull) | n.local_ip);
  };
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t user = kUsers + i * 32;
    if (!REX_LOAD_U32(user + 12)) continue;
    patch(user);
    if (i == 0 && !n.name.empty()) {
      for (uint32_t j = 0; j < kNameSize; ++j) {
        const char c = j + 1 < kNameSize && j < n.name.size() ? n.name[j] : 0;
        REX_STORE_U8(user + 16 + j, uint8_t(c >= 0x20 && c < 0x7F ? c : (c ? '?' : 0)));
      }
    }
  }
  patch(kActiveUser);
}

}  // namespace

REX_EXTERN(__imp__sub_8228BF30);
REX_HOOK_RAW(sub_8228BF30) {
  __imp__sub_8228BF30(ctx, base);
  if (!Active()) return;
  N().base = base;
  PatchUsers(base);
}

namespace tenchuz::net {

void StartNetwork() {
  if (WantsSteam(REXCVAR_GET(tenchuz_netplay))) StartSteam();
}

#if !TENCHUZ_STEAM
void StartSteam() {}
bool SteamReady() { return false; }
OnlineStatus GetOnlineStatus() { return {}; }
void JoinFriend(uint64_t) {}
void InviteFriend(uint64_t) {}
#endif

}  // namespace tenchuz::net

// XUserMuteListQuery(user_index, xuid): XLIVEBASE message 0x5800E, which the
// SDK doesn't implement (it logs an error per player). Nobody is muted.
REX_HOOK_RAW(sub_8255EA60) { ctx.r3.u64 = 0; }

// XSession* / XMsg wrappers (XDK library code that ends in the XGI app), for
// the log. The 0x8240Exxx XMsg/XUser wrappers are D3D-trace hooks.
NET_TRACE(sub_8255F0C8, "XSession_F0C8", 8)
NET_TRACE(sub_8255F258, "XSession_F258", 8)
NET_TRACE(sub_8255F308, "XSession_F308", 8)
NET_TRACE(sub_8255F3B0, "XSession_F3B0", 8)
NET_TRACE(sub_8255F458, "XSession_F458", 8)
NET_TRACE(sub_8255F530, "XSession_F530", 8)
NET_TRACE(sub_8255F5C8, "XSession_F5C8", 8)
NET_TRACE(sub_8255F678, "XSession_F678", 8)
NET_TRACE(sub_8255F768, "XSession_F768", 8)
NET_TRACE(sub_8255F850, "XSession_F850", 8)
NET_TRACE(sub_8254D8A0, "XMsgStartIORequestEx", 8)
