// tenchuz - the game's master volume (audio_volume.h).

#include "audio_volume.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>

#include <rex/cvar.h>

REXCVAR_DEFINE_INT32(tenchuz_volume, 100, "tenchuz/Audio", "Master volume, 0-100").range(0, 100);

namespace tenchuz::audio {

// The volume of this process's audio session on the default output device,
// the one the SDK's audio (SDL, WASAPI) plays through: the game's slider in
// the Windows volume mixer.
bool ApplyVolume() {
  using Microsoft::WRL::ComPtr;
  const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  bool ok = false;
  {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioSessionManager> manager;
    ComPtr<ISimpleAudioVolume> volume;
    ok = SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    IID_PPV_ARGS(&enumerator))) &&
         SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) &&
         SUCCEEDED(device->Activate(__uuidof(IAudioSessionManager), CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void**>(manager.GetAddressOf()))) &&
         SUCCEEDED(manager->GetSimpleAudioVolume(nullptr, FALSE, &volume)) &&
         SUCCEEDED(volume->SetMasterVolume(std::clamp(REXCVAR_GET(tenchuz_volume), 0, 100) / 100.0f, nullptr));
  }
  if (SUCCEEDED(init)) CoUninitialize();
  return ok;
}

}  // namespace tenchuz::audio
