// tenchuz - guest texture loading for the native renderer (milestone N4.3).
//
// Port of the SDK's D3D12 texture cache load path (src/graphics/d3d12/
// texture_cache.cpp and pipeline/texture/cache.cpp): the texture key and guest
// layout come from the fetch constant, the SDK's per-format compute shaders
// untile and byte-swap the guest data from the draw path's shared-memory mirror
// into a buffer, and the buffer is copied into the host texture. Textures are
// cached by key and reloaded when a hash of their guest memory changes (guest
// memory is not write-watched yet).

#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include <rex/graphics/xenos.h>
#include <rex/ui/d3d12/d3d12_api.h>

namespace tenchuz::native {

class TextureLoader {
 public:
  struct Texture {
    ID3D12Resource* resource = nullptr;
    DXGI_FORMAT format_unsigned = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT format_signed = DXGI_FORMAT_UNKNOWN;  // view of the same data, if there is one
    // Guest component -> host component, 3 bits each (SDK host format swizzle).
    uint32_t host_format_swizzle = 0;
    rex::graphics::xenos::DataDimension dimension = rex::graphics::xenos::DataDimension::k2DOrStacked;
    uint32_t mip_levels = 1;
    uint32_t array_size = 1;
  };

  struct Stats {
    uint32_t loads = 0;
    uint32_t loaded_kb = 0;
    uint32_t hashed_kb = 0;
    uint32_t unsupported = 0;
    uint32_t textures = 0;  // in the cache
  };

  // Copies a guest physical range into the shared-memory mirror.
  using Upload = std::function<void(uint32_t physical, uint32_t size)>;

  TextureLoader();
  ~TextureLoader();

  bool Initialize(ID3D12Device* device, ID3D12Resource* shared_memory, uint64_t shared_memory_size);

  // Start of a frame slot (after its fence has been waited on).
  void BeginFrame(uint32_t slot);

  // The texture for `fetch`, loaded (again) on `list` if its guest data
  // changed, in a shader resource state. Sets the list's descriptor heaps and
  // compute state when it loads. False if the texture can't be loaded.
  bool Get(ID3D12GraphicsCommandList* list, const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch,
           uint8_t* base, const Upload& upload, Texture& out);

  Stats TakeFrameStats();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tenchuz::native
