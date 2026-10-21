// tenchuz - PNG files through the Windows Imaging Component.

#pragma once

#include <cstdint>
#include <filesystem>

namespace tenchuz {

// Writes `width` x `height` RGBA8 pixels (rows `pitch` bytes apart) as a PNG,
// alpha dropped.
bool WritePng(const std::filesystem::path& path, uint32_t width, uint32_t height, const uint8_t* rgba,
              uint32_t pitch);

}  // namespace tenchuz
