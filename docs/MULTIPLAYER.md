# Multiplayer: System Link over a virtual LAN

Tenchu Z's System Link mode (Mode Select > System Link) runs over a virtual LAN.
Each player gets a virtual IPv4 address, and the game's sockets travel over Steam
peer-to-peer, or over loopback UDP between game instances on one PC. Xbox LIVE
modes are not covered.

Code: `src/net/system_link.cpp` (virtual sockets and XNet),
`src/net/transport_local.cpp`, `src/net/transport_steam.cpp`.

## Using it

- `--tenchuz_netplay=auto` (default): Steam if this build has the Steamworks SDK and Steam
  is running, else local. `steam`, `local` and `off` force one (`off` leaves the SDK's
  sockets, where System Link says no Ethernet cable is connected).
- Local: start two instances with `--tenchuz_netplay=local`, each with its own
  `--user_data_root` copy of the save. With Steam running, `auto` would put both on the same
  Steam account and virtual address. Instance N listens on `127.0.0.1:47320+N-1`
  (`--tenchuz_net_local_port`) and is `10.92.0.N`. One picks Create Game, the other Search
  for Game.
- Steam: needs the Steamworks SDK in `third_party/steamworks` at build time (the zip's
  `sdk` folder, as `steamworks/` or `steamworks/sdk/`; built with v1.65). It is never
  committed.
  The game runs as App ID 480 (Spacewar), and `steam_api64.dll` is copied next to the exe.
  Each player opens a friends-only lobby at start. Invite a friend from the Steam friends
  list (Invite to Lobby), or let `--tenchuz_steam_autojoin` (default on) move a player who
  is alone into a friend's Tenchu Z lobby (the lower SteamID hosts). Then Create Game and
  Search for Game as on a LAN. The Steam name is the in-game name.
- `--tenchuz_net_log=true` logs every socket, XNet and stream frame.

## What the game does

The game reaches every `NetDll_*` / `XNet*` import through XDK shims at
`0x8250C280..0x8250C6F8`. The shims prepend the caller id (`r3 = 1`), and the hooks
replace them. The game runs its own System Link sessions (no XSession calls):

- Create Game (`sub_823752A0`): UDP `:1000`, VDP `:1001`, TCP `:1003` (listening) and a
  broadcast UDP `:1004`. Session info from `XNetCreateKey` + `XNetGetTitleXnAddr`.
  Worker threads wait on each socket with `WSAEventSelect` + `WSAWaitForMultipleEvents`.
- Search: broadcast `NETMSG_FIND_GAME` to `255.255.255.255:1004`. The host answers with
  a `NETMSG_GAME_FOUND` broadcast. `NETMSG_SESSIONINFO` on `:1000` fills in the details.
- Join: `NETMSG_JOIN` (XUID and name) on `:1000`, then `NETMSG_WELCOME`. The client
  connects TCP to the host's `:1003`, which carries u16-length-prefixed messages. In the
  lobby, `NETMSG_USERDATA` (player state, 30 Hz), `NETMSG_CONNECTION` (keepalive, ~3 Hz)
  and `NETMSG_VOICECOM` (~every 3 s) flow on `:1000`.
- Message numbers come from the name switch at `sub_82376048`: 1 FIND_GAME, 2 GAME_FOUND,
  3 JOIN, 4 WELCOME, 5 SESSIONSUSPENSION, 6 HEARTBEAT, 7 ADDPLAYER, 8 DELPLAYER,
  9 GETHOSTTIME, 10 USERDATA, 11 VOICEINFO, 12 VOICEDATA, 13 LEAVE, 14 START, 15 ENDING,
  16 END, 17 SESSIONINFO, 18 REGISTER, 19 REGISTERED, 20 GOODBY, 21 VOICECOM,
  22 CONNECTION, 23 SESSION_DELETE. The u16 type is at offset 4.

## What had to change (SDK gaps)

- `XNetGetEthernetLinkStatus` returned 0, and System Link refused to open ("no Ethernet
  cable"). Now: active, 100 Mbps, full duplex.
- `XNetCreateKey` was a stub that failed, so Create Game stopped on a black screen. Now:
  random system-link keys. `XNetRandom` returned 0xBB bytes, the same nonce in every
  instance. Now: real random bytes.
- XNADDR: `XNetGetTitleXnAddr` returned loopback for everyone, and `XnAddrToInAddr` failed.
  Now: the XNADDR holds the virtual address (and a made-up MAC), and both conversions are
  identities.
- Every profile has the XUID `B13EBABEBABEBABE`, so the client lost the host right after
  `NETMSG_WELCOME`. The sign-in cache that `sub_8228BF30` fills (`0x82B57390`, 4 x 32 bytes;
  current user's XUID at `0x82B57430`) now gets the virtual address as the XUID's low half.
  The SDK keeps its own XUID for the save folder, and `tenchu.bin` doesn't store it.
- `WSAWaitForMultipleEvents` passed the timeout as an absolute NT time and reported a
  timeout as event 0. So the TCP reader thread called `recv` on a socket that wasn't
  connected yet, treated the error as fatal and closed it, and the client timed out on
  "Connecting". Now: relative timeouts, wait-any, WSA return codes.
- `WSAEventSelect` was a stub. Virtual sockets set the event themselves.
- `XUserMuteListQuery` (XLIVEBASE 0x5800E) is unimplemented in the SDK. It returns "not
  muted" from the game-side wrapper (`sub_8255EA60`).

## Status

- Local, two instances on one PC (2026-10-20): search, join, lobby with both players
  moving, mission select ("Client players should gather at Rikimaru's house"), and both
  players loaded into Mission 1 together.
- Steam, one PC (2026-10-20): Steam starts as App ID 480, a friends-only lobby opens, and
  Create Game hosts with the Steam name as the player name. Still untested: a second player
  joining over Steam, which needs another Steam account on another PC.
