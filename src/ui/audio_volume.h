// tenchuz - the game's master volume (--tenchuz_volume, 0-100), set on the
// process's audio session.

#pragma once

namespace tenchuz::audio {

// Sets the session volume from tenchuz_volume. False if there is no output
// device or session yet.
bool ApplyVolume();

}  // namespace tenchuz::audio
