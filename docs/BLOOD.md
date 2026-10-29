# Lasting blood

## Result (2026-10-29)

`src/blood.cpp`, on by default (`--tenchuz_lasting_blood`, Esc menu > Display > "Lasting blood").

- **Blood on the player's clothes stays** until they swim. It no longer fades after 30-60 s,
  and splats stay where they landed.
- **Bloody stealth kills stain the player** (as much as 3 sword hits). Kills without a gush
  (the 281/282/285 kill) don't.
- **Blood pooled on the ground stays** for the rest of the level.
- **Blood keeps stacking:** past 64 splats a map dries into a darker layer under the new blood
  (native renderer; on Mac a full map takes no more).
- With it off, the game behaves as on the Xbox 360.

## How the game does blood

- **Effect pools** at `0x82BDF820`: 240 pools, `0x8400` apart, of 64 slots of 528 bytes. A slot
  is in use while its first word is set. `+32` is its position, `+516` its life, about 1800
  for a clothes splat (30 a second). `sub_82385A98` counts it down for every draw class; near
  the end the slot fades and frees. `sub_8238A2C8` updates the pools each frame and spawns from a
  pool's template (`sub_82389C58`: r3 the slot, r5 the pool).
- **Clothes** are pools 141-144, one blood map per player. `sub_8237C238` renders a map;
  draw class 9 (`sub_82387AE0`) draws its splats.
- **The character event handler** `sub_822E5F60(character, event, r5)` has events 0-27 (jump
  table at `0x822E5FBC`).
  - Events 0-3 stain player 1-4. Each one adds 4 splats (`sub_82381608`), attaches the map's
    texture (table `0x82B7287C`) at `[object 4096+n +4688] + 400`, and sets the coating
    (`+1128` = 3, timer `+1124`).
  - Events 4-7 empty the map (`sub_8237E070`) and detach it.
  - Event 26 is the sword striking a wall.
- **The coating fade** `sub_822E67B8` runs for every character, every frame. When the timer
  runs out it sends events 4-7, 30-40 s after the last hit. A nearby character meeting some
  conditions sends them too.
- **Water washes:**
  - Entering the swim action (player-class virtual `sub_8235E228`, state 0) clears the
    coating.
  - So do two ground flags in the per-character update `sub_82342DB8` (`0x100`, `0x200`, kept
    in `+5524`). Puddles don't set them.
- **Stealth kills:**
  - The bloody ones (animations 324/326/330/334 at `+3636`) spawn a gush at the victim, in
    pools 129/131/133/135, 50-100 units from the player. They also spawn blood on the ground
    in pools 132/136 (life 440).
  - The 281/282/285 kill spawns none of these.
- Player characters have id `4096 + n` at `+160` and their position at `+3760`.

## The fix

- The fade's events 4-7 are dropped.
- Slots in pools 141-144 stop ageing about 3 s after they appear, and slots in 132/136 about
  5 s after (once spread).
- A gush within 250 units of a player sends them 3 stain events, one a frame, with a 3 s
  cooldown.
- **Dried layers (2026-10-29):** a stain on a map that can't take 4 more splats first calls
  `native::DryBloodMap` and empties the map's slots. While the game renders a map
  (`sub_8237C238`), `native::SetBloodMapResolve` marks its resolve. The next one copies the map's
  texture, colours times 0.7 (premultiplied), into the dried layer. Every resolve then puts that
  layer under the new splats (`top + dried * (1 - top.a)`), both by compute through a UAV. Clears
  (events 4-7) and a new level (`sub_8238A2C8` resetting the pools, byte `0x82B6FB70`) drop it.
- Swimming and the water flags send events 4-7 themselves. The game's own wash only fires
  while the coating is set, and the coating still runs out.

## Not yet

- **Washing off by rolling in a puddle.**
- **Blood on walls:** none in the game. The closest is the splatter of a kill through a sliding
  door, not checked.
