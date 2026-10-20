// tenchuz - the guest GPU without emulation. See native_gpu.h.
//
// The graphics system follows the SDK's GraphicsSystem and
// D3D12GraphicsSystem (src/graphics/graphics_system.cpp,
// src/graphics/d3d12/graphics_system.cpp), and the command processor its
// base CommandProcessor (src/graphics/command_processor.cpp, BSD-3) without
// the host backend: draws, resolves and swaps only consume their packets.

#include "native_gpu.h"

#include "generated/default/tenchuz_pch.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>

#include <fmt/format.h>
#include <rex/assert.h>
#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/memory/ring_buffer.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/kernel_state.h>
#include <rex/system/mmio_handler.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>
#include <rex/system/xvideo.h>
#include <rex/thread.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/ui/presenter.h>
#include <rex/ui/windowed_app_context.h>

REXCVAR_DEFINE_BOOL(tenchuz_native_gpu, true, "tenchuz",
                    "Run without GPU emulation: the native renderer draws, and a native command processor "
                    "executes only what the game observes (fences, interrupts, waits). false: the Xenos GPU "
                    "emulator (rexgpu-xenos) with native output switchable by F10");

namespace tenchuz::native {
namespace {

namespace gfx = rex::graphics;
namespace xenos = rex::graphics::xenos;
using rex::memory::RingBuffer;
using rex::X_STATUS;  // the SDK's X_STATUS_* macros name it unqualified

constexpr uint32_t kRegisterCount = 0x5003;  // the SDK's RegisterFile::kRegisterCount
constexpr int32_t kFakeOcclusionSampleCount = 1000;

class NativeGraphicsSystem final : public rex::system::IGraphicsSystem {
 public:
  ~NativeGraphicsSystem() override { Shutdown(); }

  X_STATUS SetupPresentation(rex::ui::WindowedAppContext* app_context) override {
    if (presenter_) {
      return X_STATUS_SUCCESS;
    }
    if (!provider_) {
      provider_ = rex::ui::d3d12::D3D12Provider::Create();
      if (!provider_) {
        REXLOG_ERROR("tenchuz: native GPU: unable to create the D3D12 device");
        return X_STATUS_UNSUCCESSFUL;
      }
    }
    app_context_ = app_context;
    auto lost = [](bool, bool) { rex::FatalError("Graphics device lost (probably due to an internal error)"); };
    if (app_context_) {
      app_context_->CallInUIThreadSynchronous([this, lost]() { presenter_ = provider_->CreatePresenter(lost); });
    } else {
      presenter_ = provider_->CreatePresenter(lost);
    }
    if (!presenter_) {
      REXLOG_ERROR("tenchuz: native GPU: unable to create the presenter");
      return X_STATUS_UNSUCCESSFUL;
    }
    return X_STATUS_SUCCESS;
  }

  X_STATUS SetupGuestGpu(rex::runtime::FunctionDispatcher* function_dispatcher,
                         rex::system::KernelState* kernel_state) override {
    memory_ = function_dispatcher->memory();
    function_dispatcher_ = function_dispatcher;
    kernel_state_ = kernel_state;
    if (!provider_) {
      provider_ = rex::ui::d3d12::D3D12Provider::Create();
    }

    // GPU registers at 0x7FC80000-0x7FC8FFFF.
    memory_->AddVirtualMappedRange(0x7FC80000, 0xFFFF0000, 0x0000FFFF, this, &ReadRegisterThunk,
                                   &WriteRegisterThunk);

    running_ = true;
    ring_thread_ = rex::system::object_ref<rex::system::XHostThread>(
        new rex::system::XHostThread(kernel_state_, 128 * 1024, 0, [this]() {
          RingThreadMain();
          return 0;
        }));
    ring_thread_->set_name("GPU Commands (native)");
    ring_thread_->Create();

    // Vblank at the guest video mode's refresh rate.
    vsync_thread_ = rex::system::object_ref<rex::system::XHostThread>(
        new rex::system::XHostThread(kernel_state_, 128 * 1024, 0, [this]() {
          rex::system::X_VIDEO_MODE video_mode;
          rex::kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
          const double refresh_hz = std::max(1.0, double(float(video_mode.refresh_rate)));
          const uint64_t interval = std::max<uint64_t>(
              1, uint64_t(double(rex::chrono::Clock::guest_tick_frequency()) / refresh_hz));
          uint64_t last = rex::chrono::Clock::QueryGuestTickCount();
          while (running_) {
            const uint64_t now = rex::chrono::Clock::QueryGuestTickCount();
            while (now - last >= interval) {
              ++counter_;  // the "GPU vblank counter" EVENT_WRITE_SHD can write
              DispatchInterrupt(0, 2);
              last += interval;
            }
            rex::thread::Sleep(std::chrono::milliseconds(1));
          }
          return 0;
        }));
    vsync_thread_->set_name("GPU VSync (native)");
    vsync_thread_->Create();
    REXLOG_INFO("tenchuz: native GPU: running without GPU emulation");
    return X_STATUS_SUCCESS;
  }

  bool has_presentation() const override { return presenter_ != nullptr; }
  rex::ui::GraphicsProvider* provider() const override { return provider_.get(); }
  rex::ui::Presenter* presenter() const override { return presenter_.get(); }

  void SetInterruptCallback(uint32_t callback, uint32_t user_data) override {
    interrupt_callback_ = callback;
    interrupt_callback_data_ = user_data;
    REXLOG_INFO("tenchuz: native GPU: interrupt callback {:08X} ({:08X})", callback, user_data);
  }

  void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) override {
    read_ptr_index_ = 0;
    primary_buffer_ptr_ = ptr;
    primary_buffer_size_ = uint32_t(1) << (size_log2 + 3);
  }

  void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) override {
    read_ptr_writeback_ptr_ = ptr;
    (void)block_size_log2;
  }

  void Shutdown() override {
    if (!running_.exchange(false) && !ring_thread_ && !presenter_ && !provider_) {
      return;
    }
    write_event_->Set();
    for (auto* thread : {&ring_thread_, &vsync_thread_}) {
      if (*thread) {
        (*thread)->Wait(0, 0, 0, nullptr);
        thread->reset();
      }
    }
    if (presenter_) {
      if (app_context_) {
        app_context_->CallInUIThreadSynchronous([this]() { presenter_.reset(); });
      }
      presenter_.reset();
    }
    provider_.reset();
  }

 private:
  // --- MMIO (SDK GraphicsSystem::ReadRegister / WriteRegister) --------------

  static uint32_t ReadRegisterThunk(void*, void* context, uint32_t address) {
    return static_cast<NativeGraphicsSystem*>(context)->ReadRegister(address);
  }
  static void WriteRegisterThunk(void*, void* context, uint32_t address, uint32_t value) {
    static_cast<NativeGraphicsSystem*>(context)->WriteMmio(address, value);
  }

  uint32_t ReadRegister(uint32_t address) {
    const uint32_t r = (address & 0xFFFF) / 4;
    rex::system::X_VIDEO_MODE video_mode;
    switch (r) {
      case 0x0F00:  // RB_EDRAM_TIMING
        return 0x08100748;
      case 0x0F01:  // RB_BC_CONTROL
        return 0x0000200E;
      case 0x194C:  // R500_D1MODE_V_COUNTER
        rex::kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
        return std::min(uint32_t(video_mode.display_height), 0x0FFFu);
      case 0x1951:  // interrupt status: vblank
        return 1;
      case 0x1961:  // AVIVO_D1MODE_VIEWPORT_SIZE
        rex::kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
        return (std::min(uint32_t(video_mode.display_width), 0x0FFFu) << 16) |
               std::min(uint32_t(video_mode.display_height), 0x0FFFu);
      default:
        return r < kRegisterCount ? regs_[r] : 0;
    }
  }

  void WriteMmio(uint32_t address, uint32_t value) {
    const uint32_t r = (address & 0xFFFF) / 4;
    if (r == 0x01C5) {  // CP_RB_WPTR: the D3D kicked commands
      write_ptr_index_.store(value);
      write_event_->Set();
    }
    if (r < kRegisterCount) {
      regs_[r] = value;
    }
  }

  void DispatchInterrupt(uint32_t source, uint32_t cpu) {
    if (!interrupt_callback_) {
      return;
    }
    auto* thread = rex::system::XThread::GetCurrentThread();
    if (!thread) {
      return;
    }
    thread->SetActiveCpu(uint8_t(cpu == 0xFFFFFFFF ? 2 : cpu));
    uint64_t args[] = {source, interrupt_callback_data_};
    function_dispatcher_->ExecuteInterrupt(thread->thread_state(), interrupt_callback_, args, 2);
  }

  // --- Command processor (SDK CommandProcessor, without a host backend) -----

  void RingThreadMain() {
    while (running_) {
      uint32_t write_index = write_ptr_index_.load();
      if (read_ptr_index_ == write_index) {
        // Spin a little (waiting on the event costs more), then sleep on it.
        uint32_t loops = 0;
        do {
          if (loops > 500) {
            rex::thread::Wait(write_event_.get(), true, std::chrono::milliseconds(5));
          }
          rex::thread::MaybeYield();
          ++loops;
          write_index = write_ptr_index_.load();
        } while (running_ && read_ptr_index_ == write_index);
        if (!running_) {
          break;
        }
      }
      RingBuffer reader(memory_->TranslatePhysical(primary_buffer_ptr_), primary_buffer_size_);
      reader.set_read_offset(read_ptr_index_ * sizeof(uint32_t));
      reader.set_write_offset(write_index * sizeof(uint32_t));
      do {
        if (!ExecutePacket(&reader)) {
          LogOnce(0x100, "tenchuz: native GPU: failed to execute a primary ring packet");
          break;
        }
      } while (reader.read_count());
      read_ptr_index_ = write_index;
      if (read_ptr_writeback_ptr_) {
        rex::memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(read_ptr_writeback_ptr_),
                                              read_ptr_index_);
      }
    }
  }

  void ExecuteIndirectBuffer(uint32_t ptr, uint32_t count) {
    RingBuffer reader(memory_->TranslatePhysical(ptr), count * sizeof(uint32_t));
    reader.set_write_offset(count * sizeof(uint32_t));
    do {
      if (!ExecutePacket(&reader)) {
        LogOnce(0x101, "tenchuz: native GPU: failed to execute an indirect buffer packet");
        break;
      }
    } while (reader.read_count());
  }

  void WriteRegister(uint32_t index, uint32_t value) {
    if (index >= kRegisterCount) {
      return;
    }
    const_cast<volatile uint32_t&>(regs_[index]) = value;  // volatile for WAIT_REG_MEM
    if (index >= gfx::XE_GPU_REG_SCRATCH_REG0 && index <= gfx::XE_GPU_REG_SCRATCH_REG7) {
      const uint32_t scratch = index - gfx::XE_GPU_REG_SCRATCH_REG0;
      if ((1u << scratch) & regs_[gfx::XE_GPU_REG_SCRATCH_UMSK]) {
        rex::memory::store_and_swap<uint32_t>(
            memory_->TranslatePhysical(regs_[gfx::XE_GPU_REG_SCRATCH_ADDR] + scratch * 4), value);
      }
    } else if (index == gfx::XE_GPU_REG_COHER_STATUS_HOST) {
      // Coherency request: cleared (made coherent) when a WAIT_REG_MEM polls it.
      const_cast<volatile uint32_t&>(regs_[index]) |= 0x80000000u;
    }
    // DC_LUT (gamma ramp) writes need no state machine here: the native
    // renderer takes the ramp from the D3D call that writes it.
  }

  void WriteRegistersFromRing(RingBuffer* reader, uint32_t first, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
      WriteRegister(first + i, reader->ReadAndSwap<uint32_t>());
    }
  }

  uint32_t ReadPolled(uint32_t address, bool is_memory) {
    if (is_memory) {
      const uint32_t value = rex::memory::load<uint32_t>(memory_->TranslatePhysical(address & ~3u));
      return xenos::GpuSwap(value, xenos::Endian(address & 3));
    }
    if (address == gfx::XE_GPU_REG_COHER_STATUS_HOST && (regs_[address] & 0x80000000u)) {
      const_cast<volatile uint32_t&>(regs_[address]) = 0;  // MakeCoherent
    }
    return address < kRegisterCount ? const_cast<volatile uint32_t&>(regs_[address]) : 0;
  }

  static bool Compare(uint32_t function, uint32_t value, uint32_t ref) {
    switch (function & 7) {
      case 1: return value < ref;
      case 2: return value <= ref;
      case 3: return value == ref;
      case 4: return value != ref;
      case 5: return value >= ref;
      case 6: return value > ref;
      case 7: return true;
      default: return false;
    }
  }

  void StoreGuest(uint32_t address, uint32_t value) {
    value = xenos::GpuSwap(value, xenos::Endian(address & 3));
    rex::memory::store(memory_->TranslatePhysical(address & ~3u), value);
  }

  bool ExecutePacket(RingBuffer* reader) {
    const uint32_t packet = reader->ReadAndSwap<uint32_t>();
    if (packet == 0) {
      return true;
    }
    switch (packet >> 30) {
      case 0: {  // registers in sequence (or one, repeatedly)
        const uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
        if (reader->read_count() < count * sizeof(uint32_t)) {
          return false;
        }
        const uint32_t base = packet & 0x7FFF;
        const bool one = (packet >> 15) & 1;
        for (uint32_t i = 0; i < count; ++i) {
          WriteRegister(one ? base : base + i, reader->ReadAndSwap<uint32_t>());
        }
        return true;
      }
      case 1: {  // two registers
        const uint32_t a = reader->ReadAndSwap<uint32_t>(), b = reader->ReadAndSwap<uint32_t>();
        WriteRegister(packet & 0x7FF, a);
        WriteRegister((packet >> 11) & 0x7FF, b);
        return true;
      }
      case 2:  // no-op
        return true;
      default:
        return ExecutePacketType3(reader, packet);
    }
  }

  bool ExecutePacketType3(RingBuffer* reader, uint32_t packet) {
    const uint32_t opcode = (packet >> 8) & 0x7F;
    const uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
    if (reader->read_count() < count * sizeof(uint32_t)) {
      return false;
    }
    // Predicated packets run only for selected bins (tiles); swaps never.
    if ((packet & 1) && (!(bin_select_ & bin_mask_) || opcode == xenos::PM4_XE_SWAP)) {
      reader->AdvanceRead(count * sizeof(uint32_t));
      return true;
    }
    auto skip_rest = [&](uint32_t read) { reader->AdvanceRead((count - read) * sizeof(uint32_t)); };
    switch (opcode) {
      case xenos::PM4_INTERRUPT: {
        const uint32_t cpu_mask = reader->ReadAndSwap<uint32_t>();
        for (uint32_t n = 0; n < 6; ++n) {
          if (cpu_mask & (1u << n)) {
            DispatchInterrupt(1, n);
          }
        }
        skip_rest(1);
        return true;
      }
      case xenos::PM4_XE_SWAP:  // VdSwap's packet: presented by the native renderer at VdSwap
        ++counter_;
        reader->AdvanceRead(count * sizeof(uint32_t));
        return true;
      case xenos::PM4_INDIRECT_BUFFER:
      case xenos::PM4_INDIRECT_BUFFER_PFD: {
        const uint32_t list = xenos::GpuToCpu(xenos::CpuToGpu(reader->ReadAndSwap<uint32_t>()));
        const uint32_t length = reader->ReadAndSwap<uint32_t>() & 0xFFFFF;
        skip_rest(2);
        ExecuteIndirectBuffer(list, length);
        return true;
      }
      case xenos::PM4_WAIT_REG_MEM: {
        const uint32_t info = reader->ReadAndSwap<uint32_t>();
        const uint32_t address = reader->ReadAndSwap<uint32_t>();
        const uint32_t ref = reader->ReadAndSwap<uint32_t>();
        const uint32_t mask = reader->ReadAndSwap<uint32_t>();
        const uint32_t wait = reader->ReadAndSwap<uint32_t>();
        skip_rest(5);
        while (!Compare(info, ReadPolled(address, info & 0x10) & mask, ref)) {
          if (wait >= 0x100) {
            rex::thread::Sleep(std::chrono::milliseconds(wait / 0x100));
            rex::thread::SyncMemory();
            if (!running_) {
              return false;
            }
          } else {
            rex::thread::MaybeYield();
          }
        }
        return true;
      }
      case xenos::PM4_REG_RMW: {
        const uint32_t info = reader->ReadAndSwap<uint32_t>();
        const uint32_t and_mask = reader->ReadAndSwap<uint32_t>();
        const uint32_t or_mask = reader->ReadAndSwap<uint32_t>();
        skip_rest(3);
        uint32_t value = regs_[info & 0x1FFF];
        value &= (info >> 31) & 1 ? regs_[and_mask & 0x1FFF] : and_mask;
        value |= (info >> 30) & 1 ? regs_[or_mask & 0x1FFF] : or_mask;
        WriteRegister(info & 0x1FFF, value);
        return true;
      }
      case xenos::PM4_REG_TO_MEM: {
        const uint32_t reg = reader->ReadAndSwap<uint32_t>();
        const uint32_t address = reader->ReadAndSwap<uint32_t>();
        skip_rest(2);
        StoreGuest(address, reg < kRegisterCount ? regs_[reg] : 0);
        return true;
      }
      case xenos::PM4_MEM_WRITE: {
        uint32_t address = reader->ReadAndSwap<uint32_t>();
        for (uint32_t i = 1; i < count; ++i, address += 4) {
          StoreGuest(address, reader->ReadAndSwap<uint32_t>());
        }
        return true;
      }
      case xenos::PM4_COND_WRITE: {
        const uint32_t info = reader->ReadAndSwap<uint32_t>();
        const uint32_t poll = reader->ReadAndSwap<uint32_t>();
        const uint32_t ref = reader->ReadAndSwap<uint32_t>();
        const uint32_t mask = reader->ReadAndSwap<uint32_t>();
        const uint32_t target = reader->ReadAndSwap<uint32_t>();
        const uint32_t data = reader->ReadAndSwap<uint32_t>();
        skip_rest(6);
        if (Compare(info, ReadPolled(poll, info & 0x10) & mask, ref)) {
          if (info & 0x100) {
            StoreGuest(target, data);
          } else {
            WriteRegister(target, data);
          }
        }
        return true;
      }
      case xenos::PM4_EVENT_WRITE: {
        WriteRegister(gfx::XE_GPU_REG_VGT_EVENT_INITIATOR, reader->ReadAndSwap<uint32_t>() & 0x3F);
        skip_rest(1);
        return true;
      }
      case xenos::PM4_EVENT_WRITE_SHD: {  // fence: a value (or the vblank counter) to memory
        const uint32_t initiator = reader->ReadAndSwap<uint32_t>();
        const uint32_t address = reader->ReadAndSwap<uint32_t>();
        const uint32_t value = reader->ReadAndSwap<uint32_t>();
        skip_rest(3);
        WriteRegister(gfx::XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
        StoreGuest(address, (initiator >> 31) & 1 ? counter_.load() : value);
        return true;
      }
      case xenos::PM4_EVENT_WRITE_EXT: {  // screen extents of earlier draws: everything
        const uint32_t initiator = reader->ReadAndSwap<uint32_t>();
        const uint32_t address = reader->ReadAndSwap<uint32_t>();
        skip_rest(2);
        WriteRegister(gfx::XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
        const uint16_t extents[] = {0, xenos::kTexture2DCubeMaxWidthHeight >> 3, 0,
                                    xenos::kTexture2DCubeMaxWidthHeight >> 3, 0, 1};
        rex::memory::copy_and_swap_16_unaligned(memory_->TranslatePhysical(address & ~3u), extents,
                                                std::size(extents));
        return true;
      }
      case xenos::PM4_EVENT_WRITE_ZPD: {  // occlusion query: a fixed number of samples passed
        WriteRegister(gfx::XE_GPU_REG_VGT_EVENT_INITIATOR, reader->ReadAndSwap<uint32_t>() & 0x3F);
        skip_rest(1);
        auto* counts = memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(
            regs_[gfx::XE_GPU_REG_RB_SAMPLE_COUNT_ADDR]);
        if (counts) {
          // The D3D marks a finished query (D3DISSUE_END) with 0xFFFFFEED.
          const uint32_t finished = rex::byte_swap(0xFFFFFEEDu);
          const bool end = counts->ZPass_A == finished || counts->ZPass_B == finished ||
                           counts->ZFail_A == finished || counts->ZFail_B == finished;
          std::memset(counts, 0, sizeof(*counts));
          if (end) {
            counts->ZPass_A = kFakeOcclusionSampleCount;
            counts->Total_A = kFakeOcclusionSampleCount;
          }
        }
        return true;
      }
      case xenos::PM4_SET_CONSTANT: {
        const uint32_t offset_type = reader->ReadAndSwap<uint32_t>();
        static constexpr uint32_t kBases[] = {0x4000, 0x4800, 0x4900, 0x4908, 0x2000};
        const uint32_t kind = (offset_type >> 16) & 0xFF;
        if (kind < 5) {
          WriteRegistersFromRing(reader, kBases[kind] + (offset_type & 0x7FF), count - 1);
        } else {
          skip_rest(1);
        }
        return true;
      }
      case xenos::PM4_SET_CONSTANT2:
      case xenos::PM4_SET_SHADER_CONSTANTS:
        WriteRegistersFromRing(reader, reader->ReadAndSwap<uint32_t>() & 0xFFFF, count - 1);
        return true;
      case xenos::PM4_VIZ_QUERY: {  // reported visible
        const uint32_t id_end = reader->ReadAndSwap<uint32_t>();
        skip_rest(1);
        const uint32_t id = id_end & 0x3F;
        if (id_end & 0x100) {
          regs_[id < 32 ? gfx::XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_0 : gfx::XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_1] |=
              1u << (id & 31);
        }
        return true;
      }
      case xenos::PM4_SET_BIN_MASK_LO:
        bin_mask_ = (bin_mask_ & ~0xFFFFFFFFull) | reader->ReadAndSwap<uint32_t>();
        skip_rest(1);
        return true;
      case xenos::PM4_SET_BIN_MASK_HI:
        bin_mask_ = (bin_mask_ & 0xFFFFFFFFull) | (uint64_t(reader->ReadAndSwap<uint32_t>()) << 32);
        skip_rest(1);
        return true;
      case xenos::PM4_SET_BIN_SELECT_LO:
        bin_select_ = (bin_select_ & ~0xFFFFFFFFull) | reader->ReadAndSwap<uint32_t>();
        skip_rest(1);
        return true;
      case xenos::PM4_SET_BIN_SELECT_HI:
        bin_select_ = (bin_select_ & 0xFFFFFFFFull) | (uint64_t(reader->ReadAndSwap<uint32_t>()) << 32);
        skip_rest(1);
        return true;
      case xenos::PM4_SET_BIN_MASK:
      case xenos::PM4_SET_BIN_SELECT: {
        const uint64_t hi = reader->ReadAndSwap<uint32_t>(), lo = reader->ReadAndSwap<uint32_t>();
        (opcode == xenos::PM4_SET_BIN_MASK ? bin_mask_ : bin_select_) = (hi << 32) | lo;
        skip_rest(2);
        return true;
      }
      // Draws (recorded by the native renderer from the D3D calls), shader
      // loads, constants from memory and the rest: their words are consumed.
      // LOAD_ALU_CONSTANT and IM_LOAD only read memory into GPU state.
      case xenos::PM4_DRAW_INDX:
      case xenos::PM4_DRAW_INDX_2:
      case xenos::PM4_LOAD_ALU_CONSTANT:
      case xenos::PM4_IM_LOAD:
      case xenos::PM4_IM_LOAD_IMMEDIATE:
      case xenos::PM4_INVALIDATE_STATE:
      case xenos::PM4_ME_INIT:
      case xenos::PM4_NOP:
      case xenos::PM4_CONTEXT_UPDATE:
      case xenos::PM4_WAIT_FOR_IDLE:
        reader->AdvanceRead(count * sizeof(uint32_t));
        return true;
      default:
        LogOnce(opcode, "tenchuz: native GPU: PM4 opcode {:02X} not executed", opcode);
        reader->AdvanceRead(count * sizeof(uint32_t));
        return true;
    }
  }

  template <typename... Args>
  void LogOnce(uint32_t key, fmt::format_string<Args...> format, Args&&... args) {
    if (key < logged_.size() && !logged_[key].exchange(true)) {
      REXLOG_WARN("{}", fmt::format(format, std::forward<Args>(args)...));
    }
  }

  rex::memory::Memory* memory_ = nullptr;
  rex::runtime::FunctionDispatcher* function_dispatcher_ = nullptr;
  rex::system::KernelState* kernel_state_ = nullptr;
  rex::ui::WindowedAppContext* app_context_ = nullptr;
  std::unique_ptr<rex::ui::d3d12::D3D12Provider> provider_;
  std::unique_ptr<rex::ui::Presenter> presenter_;

  uint32_t regs_[kRegisterCount] = {};  // shared by the MMIO and the command processor
  uint32_t interrupt_callback_ = 0, interrupt_callback_data_ = 0;
  std::atomic<uint32_t> counter_{0};

  std::atomic<bool> running_{false};
  rex::system::object_ref<rex::system::XHostThread> ring_thread_, vsync_thread_;
  std::unique_ptr<rex::thread::Event> write_event_ = rex::thread::Event::CreateAutoResetEvent(false);
  uint32_t primary_buffer_ptr_ = 0, primary_buffer_size_ = 0;
  uint32_t read_ptr_index_ = 0, read_ptr_writeback_ptr_ = 0;
  std::atomic<uint32_t> write_ptr_index_{0};
  uint64_t bin_mask_ = ~0ull, bin_select_ = 0;
  std::array<std::atomic<bool>, 0x102> logged_{};
};

}  // namespace

bool NativeGpu() {
  return REXCVAR_GET(tenchuz_native_gpu);
}

std::unique_ptr<rex::system::IGraphicsSystem> CreateNativeGraphicsSystem() {
  return std::make_unique<NativeGraphicsSystem>();
}

}  // namespace tenchuz::native
