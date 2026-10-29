// tenchuz - lasting blood (docs/BLOOD.md).
//
// Blood lives in effect pools: 64 slots of 528 bytes each at 0x82BDF820 +
// pool * 0x8400, a slot in use while its first word is set. Each slot has a
// life (+516) counted down by sub_82385A98 at about 30 a second; near the
// end it fades and the slot frees.
//
// Sword hits stain the player's clothes: the character event handler
// (sub_822E5F60, events 0-3 for player 1-4) spawns 4 splats into that
// player's blood map, pool 141-144, and attaches the map's texture to the
// model. The game takes it off again:
//  - the coating fade (sub_822E67B8, every character every frame): 30-40 s
//    after the last hit it sends events 4-7, which empty the map and detach
//    it. Other characters standing close can trigger the same events there.
//  - each splat's life (about 1800, a minute).
// And it washes it off in water: entering the swim action (player class
// virtual sub_8235E228, state 0) and two ground flags of the per-character
// update (sub_82342DB8, flags kept in +5524).
//
// Bloody stealth kills spawn a gush at the victim (pools 129, 131, 133 and
// 135, about 50-100 units from the player) and a pool of blood on the
// ground (132 and 136, life 440). The game stains no one for them.
//
// With --tenchuz_lasting_blood (default):
//  - the fade's events are dropped, and splats on clothes and blood on the
//    ground stop ageing once they've spread, so they stay where they are
//    until the player swims (clothes) or the level ends (ground). A full
//    map takes no more splats.
//  - a gush next to a player stains them like 3 sword hits.
//  - a hit on a full map dries it first: the renderer keeps all the map
//    shows, darkened, as a layer under it (native::DryBloodMap), and the
//    map's splats are dropped, so new blood lands on top of the old. Each
//    drying darkens the older layers again. Without the native renderer a
//    full map takes no more splats.
// The coating state itself still runs out as before (it may matter to
// gameplay); the washes call events 4-7 themselves, since the game's own
// wash only fires while that state is set.

#include "generated/default/tenchuz_pch.h"

#include <bit>
#include <chrono>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "renderer/native_renderer.h"

REXCVAR_DEFINE_BOOL(tenchuz_lasting_blood, true, "tenchuz",
                    "Keep blood on the player's clothes until they swim, and on the ground, instead of "
                    "letting it fade; bloody stealth kills stain the player");

REX_EXTERN(__imp__sub_822E5F60);

namespace {

constexpr uint32_t kPools = 0x82BDF820;  // effect pools, kPoolStride apart
constexpr uint32_t kPoolStride = 0x8400;
constexpr uint32_t kPoolCount = 240;
constexpr uint32_t kSlotSize = 528;
constexpr uint32_t kSlots = 64;
constexpr uint32_t kSlotPosition = 32;  // x, y, z floats
constexpr uint32_t kSlotLife = 516;     // about 30 a second
constexpr uint32_t kFirstBloodMap = 141;  // pools 141-144: players 1-4
constexpr uint32_t kSplatsPerHit = 4;
constexpr uint32_t kPoolsReady = 0x82B6FB70;  // byte: 0 until sub_8238A2C8 resets the pools (a new level)
constexpr uint32_t kPlayers = 4;
constexpr uint32_t kGushPools[] = {129, 131, 133, 135};
constexpr uint32_t kGroundBloodPools[] = {132, 136};
constexpr float kGushReach = 250.0f;  // how close a gush stains a player
constexpr int kStainsPerGush = 3;
constexpr auto kGushCooldown = std::chrono::seconds(3);  // one kill's gush spawns over a second or two
// Life a slot spends before it holds: splats settle in about 3 s, ground
// blood spreads for about 5.
constexpr int32_t kSettleSplat = 90;
constexpr int32_t kSettleGround = 150;

constexpr uint32_t kEventStainPlayer1 = 0;  // events 0-3: blood on player 1-4
constexpr uint32_t kEventClearPlayer1 = 4;  // events 4-7: clear player 1-4's blood map
// Character fields.
constexpr uint32_t kId = 160;  // 4096 + n for player n
constexpr uint32_t kPosition = 3760;  // x, y, z floats
constexpr uint32_t kActionState = 4644;
constexpr uint32_t kWaterFlags = 5524;  // set by sub_82342DB8 while on water ground
constexpr uint32_t kPlayerId = 4096;

thread_local int g_in_fade = 0;

// Per pool slot: its life when it appeared and last frame.
int32_t g_start[kPoolCount * kSlots], g_last[kPoolCount * kSlots];
// Players' characters, and gushes waiting to stain them.
uint32_t g_player_character[kPlayers];
int g_pending_stains[kPlayers];
std::chrono::steady_clock::time_point g_last_gush[kPlayers];

bool Lasting() { return REXCVAR_GET(tenchuz_lasting_blood); }

uint32_t SlotAddress(uint32_t player, uint32_t slot) {
  return kPools + (kFirstBloodMap + player) * kPoolStride + slot * kSlotSize;
}

// Before a stain: dries a map that can't take another hit.
void DryIfFull(uint8_t* base, uint32_t player) {
  uint32_t used = 0;
  for (uint32_t k = 0; k < kSlots; ++k) used += REX_LOAD_U32(SlotAddress(player, k)) != 0;
  if (used + kSplatsPerHit <= kSlots || !tenchuz::native::BloodMapLayers()) return;
  tenchuz::native::DryBloodMap(int(player));
  for (uint32_t k = 0; k < kSlots; ++k) REX_STORE_U32(SlotAddress(player, k), 0);
}

int32_t Settle(uint32_t pool) {
  if (pool >= kFirstBloodMap && pool < kFirstBloodMap + kPlayers) return kSettleSplat;
  for (uint32_t p : kGroundBloodPools) {
    if (pool == p) return kSettleGround;
  }
  return -1;  // not held
}

float LoadFloat(uint8_t* base, uint32_t address) { return std::bit_cast<float>(REX_LOAD_U32(address)); }

// Player index 0-3 of a character, or -1.
int PlayerOf(uint8_t* base, uint32_t character) {
  const uint32_t id = REX_LOAD_U32(character + kId);
  return id >= kPlayerId && id < kPlayerId + kPlayers ? int(id - kPlayerId) : -1;
}

// Sends a character event (r5 = 0: r3 isn't used by events 0-7).
void Event(PPCContext& ctx, uint8_t* base, uint32_t character, uint32_t event) {
  const PPCContext saved = ctx;
  ctx.r3.u64 = character;
  ctx.r4.u64 = event;
  ctx.r5.u64 = 0;
  __imp__sub_822E5F60(ctx, base);
  ctx = saved;
}

}  // namespace

// The character event handler: drops the coating fade's clears, dries full
// maps before a stain, and drops the dried layer with a clear.
REX_HOOK_RAW(sub_822E5F60) {
  const uint32_t event = ctx.r4.u32 & 0xFFFF;
  if (event >= kEventClearPlayer1 && event < kEventClearPlayer1 + kPlayers) {
    if (g_in_fade && Lasting()) return;
    tenchuz::native::ClearBloodMap(int(event - kEventClearPlayer1));
  }
  if (event < kEventStainPlayer1 + kPlayers && Lasting()) DryIfFull(base, event - kEventStainPlayer1);
  __imp__sub_822E5F60(ctx, base);
}

// A blood map's render (r3 its pool): its resolve gets the dried layer.
REX_EXTERN(__imp__sub_8237C238);
REX_HOOK_RAW(sub_8237C238) {
  const uint32_t pool = ctx.r3.u32;
  const bool map = pool >= kFirstBloodMap && pool < kFirstBloodMap + kPlayers;
  if (map) tenchuz::native::SetBloodMapResolve(int(pool - kFirstBloodMap));
  __imp__sub_8237C238(ctx, base);
  if (map) tenchuz::native::SetBloodMapResolve(-1);
}

// The pools' update: a new level resets them, and the dried layers go too.
REX_EXTERN(__imp__sub_8238A2C8);
REX_HOOK_RAW(sub_8238A2C8) {
  const bool reset = REX_LOAD_U8(kPoolsReady) == 0;
  __imp__sub_8238A2C8(ctx, base);
  if (reset && REX_LOAD_U8(kPoolsReady) != 0) {
    for (uint32_t n = 0; n < kPlayers; ++n) tenchuz::native::ClearBloodMap(int(n));
  }
}

// The coating fade.
REX_EXTERN(__imp__sub_822E67B8);
REX_HOOK_RAW(sub_822E67B8) {
  ++g_in_fade;
  __imp__sub_822E67B8(ctx, base);
  --g_in_fade;
}

// A slot's update: holds blood once it has settled.
REX_EXTERN(__imp__sub_82385A98);
REX_HOOK_RAW(sub_82385A98) {
  const uint32_t slot = ctx.r3.u32;
  if (slot >= kPools && slot < kPools + kPoolCount * kPoolStride && Lasting()) {
    const uint32_t pool = (slot - kPools) / kPoolStride, offset = (slot - kPools) % kPoolStride;
    const int32_t settle = Settle(pool);
    if (settle >= 0 && offset < kSlots * kSlotSize) {
      const uint32_t i = pool * kSlots + offset / kSlotSize;
      const int32_t life = int32_t(REX_LOAD_U32(slot + kSlotLife));
      if (life > g_last[i]) g_start[i] = life;  // a new splat
      g_last[i] = life;
      if (life > 0 && life <= g_start[i] - settle) return;
    }
  }
  __imp__sub_82385A98(ctx, base);
}

// An effect spawn (r3 the slot, r5 its pool): a gush stains the players
// beside it.
REX_EXTERN(__imp__sub_82389C58);
REX_HOOK_RAW(sub_82389C58) {
  const uint32_t slot = ctx.r3.u32, pool = ctx.r5.u32;
  __imp__sub_82389C58(ctx, base);
  bool gush = false;
  for (uint32_t p : kGushPools) gush |= pool == p;
  if (!gush || !Lasting()) return;
  for (uint32_t n = 0; n < kPlayers; ++n) {
    const uint32_t character = g_player_character[n];
    const auto now = std::chrono::steady_clock::now();
    if (!character || PlayerOf(base, character) != int(n) || now - g_last_gush[n] < kGushCooldown) continue;
    float d2 = 0;
    for (uint32_t k = 0; k < 3; ++k) {
      const float d = LoadFloat(base, slot + kSlotPosition + 4 * k) - LoadFloat(base, character + kPosition + 4 * k);
      d2 += d * d;
    }
    if (d2 < kGushReach * kGushReach) {
      g_pending_stains[n] = kStainsPerGush;
      g_last_gush[n] = now;
    }
  }
}

// The swim action: entering it washes the blood off.
REX_EXTERN(__imp__sub_8235E228);
REX_HOOK_RAW(sub_8235E228) {
  const uint32_t character = ctx.r3.u32;
  const bool entering = REX_LOAD_U32(character + kActionState) == 0;
  __imp__sub_8235E228(ctx, base);
  const int player = PlayerOf(base, character);
  if (entering && player >= 0 && Lasting()) Event(ctx, base, character, kEventClearPlayer1 + uint32_t(player));
}

// The per-character update: washes on the game's water ground flags, and
// stains players a gush reached.
REX_EXTERN(__imp__sub_82342DB8);
REX_HOOK_RAW(sub_82342DB8) {
  const uint32_t character = ctx.r3.u32;
  const uint32_t water_before = REX_LOAD_U32(character + kWaterFlags) & 3;
  __imp__sub_82342DB8(ctx, base);
  const int player = PlayerOf(base, character);
  if (player < 0) return;
  g_player_character[player] = character;
  if (!Lasting()) return;
  const uint32_t water = REX_LOAD_U32(character + kWaterFlags) & 3;
  if (water & ~water_before) Event(ctx, base, character, kEventClearPlayer1 + uint32_t(player));
  if (g_pending_stains[player] > 0) {  // one hit's worth a frame
    --g_pending_stains[player];
    Event(ctx, base, character, kEventStainPlayer1 + uint32_t(player));
  }
}
