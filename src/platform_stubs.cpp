// tenchuz - what macOS (and other non-Windows builds) runs in place of the
// Windows-only parts: the native D3D12 renderer (the Xenos GPU plugin draws
// instead, over Vulkan and MoltenVK), the Windows debugging tools, PNG
// screenshots through WIC, the per-app volume and the SDK runtime fixes.

#include "generated/default/tenchuz_pch.h"

#include <filesystem>
#include <memory>

#include <rex/cvar.h>
#include <rex/system/interfaces/graphics.h>

#include "debug_tools.h"
#include "png_writer.h"
#include "profiler.h"
#include "renderer/native_gpu.h"
#include "renderer/native_renderer.h"
#include "sdk_fixes.h"
#include "ui/audio_volume.h"

// Saved by the game menu on every platform; only Windows applies it.
REXCVAR_DEFINE_INT32(tenchuz_volume, 100, "tenchuz/Audio", "Master volume, 0-100 (Windows)").range(0, 100);

namespace tenchuz::native {

bool NativeGpu() { return false; }
std::unique_ptr<rex::system::IGraphicsSystem> CreateNativeGraphicsSystem() { return nullptr; }
void SetVblankRate(double) {}
void EnableDeviceDiagnostics() {}
void Initialize(rex::Runtime*) {}
bool PresentNative() { return false; }
void TogglePresentNative() {}
void RequestFrameDump() {}
void RequestScreenshot(const std::filesystem::path&) {}
void OnSwap(uint8_t*, uint32_t) {}
int LastFrameDepthDraws() { return -1; }

}  // namespace tenchuz::native

namespace tenchuz::profiler {
void OnMainLoopFrame() {}
void RequestCapture() {}
void SetOutputDir(const std::filesystem::path&) {}
}  // namespace tenchuz::profiler

namespace tenchuz::debug_tools {
void OnMainLoopFrame(uint8_t*) {}
void SetOutputDir(const std::filesystem::path&) {}
void RequestImageDump() {}
}  // namespace tenchuz::debug_tools

namespace tenchuz {
bool WritePng(const std::filesystem::path&, uint32_t, uint32_t, const uint8_t*, uint32_t) { return false; }
}  // namespace tenchuz

namespace tenchuz::audio {
bool ApplyVolume() { return false; }
}  // namespace tenchuz::audio

namespace tenchuz::sdk_fixes {
void Apply() {}
}  // namespace tenchuz::sdk_fixes

// The mid-asm hook after VdSwap (config/default.toml) only matters to the
// native renderer.
void TenchuzAfterVdSwap(PPCRegister&) {}
