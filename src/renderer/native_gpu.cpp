// tenchuz - the guest GPU without emulation. See native_gpu.h.
//
// The graphics system follows the SDK's GraphicsSystem and
// D3D12GraphicsSystem (src/graphics/graphics_system.cpp,
// src/graphics/d3d12/graphics_system.cpp), and the command processor its
// base CommandProcessor (src/graphics/command_processor.cpp, BSD-3) without
// the host backend: draws, resolves and swaps only consume their packets.

#include "native_gpu.h"

#include "generated/default/tenchuz_pch.h"

#include <windows.h>
// windows.h must come first
#include <dxgi.h>
#include <wrl/client.h>
#pragma comment(lib, "dxgi.lib")

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
#include <rex/hook.h>
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

REXCVAR_DEFINE_BOOL(tenchuz_gpu_wait_sleep, true, "tenchuz",
                    "Without GPU emulation, the D3D's GPU waits sleep until the command processor makes "
                    "progress instead of spinning a core");
REXCVAR_DEFINE_BOOL(tenchuz_native_gpu, true, "tenchuz",
                    "Run without GPU emulation: the native renderer draws, and a native command processor "
                    "executes only what the game observes (fences, interrupts, waits). false: the Xenos GPU "
                    "emulator (rexgpu-xenos) with native output switchable by F10");

REXCVAR_DEFINE_BOOL(tenchuz_display_vblank, true, "tenchuz",
                    "On a display refreshing at a multiple of 60 Hz (60, 120, 180, 240), time the game's "
                    "vblanks by the display's own, so each frame stays on screen equally long");

namespace tenchuz::native {
namespace {

// Bumped whenever the GPU side makes progress the game can poll for: the ring
// read pointer, fence and memory writes, vblanks. The D3D's GPU waits sleep on
// it instead of spinning (the sub_82425790 hook below).
std::atomic<uint32_t> g_progress{0};
std::atomic<double> g_vblank_hz{0.0};  // SetVblankRate
std::atomic<uint64_t> g_display_vblanks{0};  // DisplayVblanks

// The game's vblanks from the display's: one every `per_guest` display
// vblanks, when the display refreshes at a whole multiple of the guest rate.
// On a timer of its own, 60 Hz drifts against the display's 180 (or 60, 120,
// 240): frames then stay on screen for 2 or 4 refreshes now and then instead
// of 3, which shows as stutter when the camera pans. Displays at other rates
// (144 Hz) keep the timer.
class DisplayVblank {
 public:
  // Waits for the next guest vblank. False (at once): not usable now; the
  // caller times the vblank itself.
  bool Wait(double guest_hz) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= next_check_) {
      next_check_ = now + std::chrono::seconds(1);
      Update(guest_hz);
    }
    if (!per_guest_) {
      return false;
    }
    // WaitForVBlank sometimes returns at once (while loading, a display
    // asleep): a vblank counts only half a refresh or more after the last.
    // Many early returns in a row: no clock, the timer until the next check.
    const auto min_gap = std::chrono::duration<double>(0.5 / display_hz_);
    for (uint32_t i = 0, early = 0; i < per_guest_;) {
      if (FAILED(output_->WaitForVBlank())) {
        Drop();
        return false;
      }
      const auto at = std::chrono::steady_clock::now();
      if (at - last_vblank_ < min_gap) {
        if (++early > 16) {
          Drop();
          return false;
        }
        continue;
      }
      last_vblank_ = at;
      ++i;
      g_display_vblanks.fetch_add(1, std::memory_order_relaxed);
    }
    // The game hands its frame over within a millisecond or so of its
    // vblank. Raised at the display's, that lands on a refresh boundary, and
    // the frame shows one refresh early or late at random (2 or 4 at 180 Hz
    // instead of 3). A third of a refresh later, it lands between two.
    LARGE_INTEGER due;
    due.QuadPart = -LONGLONG(1e7 / 3 / display_hz_);
    if (timer_ && SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) {
      WaitForSingleObject(timer_, INFINITE);
    }
    return true;
  }

  ~DisplayVblank() {
    if (timer_) CloseHandle(timer_);
  }

 private:
  void Drop() {
    g_display_vblanks.store(0, std::memory_order_relaxed);
    output_.Reset();
    monitor_ = nullptr;
    per_guest_ = 0;
  }

  // The display the game window is on, and how many of its vblanks make one
  // of the guest's.
  void Update(double guest_hz) {
    HWND window = nullptr;
    if (REXCVAR_GET(tenchuz_display_vblank)) {
      EnumWindows(
          [](HWND hwnd, LPARAM param) -> BOOL {
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) {
              return TRUE;
            }
            *reinterpret_cast<HWND*>(param) = hwnd;
            return FALSE;
          },
          LPARAM(&window));
    }
    if (!window || IsIconic(window)) {
      Drop();
      return;
    }
    HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW info = {};
    info.cbSize = sizeof(info);
    DEVMODEW mode = {};
    mode.dmSize = sizeof(mode);
    if (!GetMonitorInfoW(monitor, &info) || !EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode)) {
      Drop();
      return;
    }
    // Display rates are rounded (180 for 179.96 Hz): within 1 Hz of a multiple.
    const double display_hz = double(mode.dmDisplayFrequency);
    const uint32_t per_guest = uint32_t(display_hz / guest_hz + 0.5);
    const bool multiple = per_guest >= 1 && per_guest <= 8 && std::abs(display_hz - per_guest * guest_hz) <= 1.0;
    if (!multiple) {
      if (per_guest_ || !logged_) {
        REXLOG_INFO("tenchuz: native GPU: display at {} Hz isn't a multiple of {} Hz; vblanks on a timer",
                    mode.dmDisplayFrequency, guest_hz);
        logged_ = true;
      }
      Drop();
      return;
    }
    if (monitor == monitor_ && output_ && per_guest == per_guest_) {
      return;
    }
    if (!factory_ && FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory_)))) {
      Drop();
      return;
    }
    Drop();
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    for (UINT a = 0; !output_ && SUCCEEDED(factory_->EnumAdapters(a, adapter.ReleaseAndGetAddressOf())); ++a) {
      Microsoft::WRL::ComPtr<IDXGIOutput> output;
      for (UINT o = 0; SUCCEEDED(adapter->EnumOutputs(o, output.ReleaseAndGetAddressOf())); ++o) {
        DXGI_OUTPUT_DESC desc;
        if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor) {
          output_ = output;
          break;
        }
      }
    }
    if (!output_) {
      factory_.Reset();  // displays changed since it was made
      return;
    }
    monitor_ = monitor;
    per_guest_ = per_guest;
    display_hz_ = display_hz;
    REXLOG_INFO("tenchuz: native GPU: vblanks from the display's ({} Hz, every {})", mode.dmDisplayFrequency,
                per_guest);
    logged_ = true;
  }

  Microsoft::WRL::ComPtr<IDXGIFactory1> factory_;
  Microsoft::WRL::ComPtr<IDXGIOutput> output_;
  HMONITOR monitor_ = nullptr;
  uint32_t per_guest_ = 0;
  double display_hz_ = 60;
  std::chrono::steady_clock::time_point last_vblank_{};
  HANDLE timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  bool logged_ = false;
  std::chrono::steady_clock::time_point next_check_{};
};

void Progress() {
  g_progress.fetch_add(1, std::memory_order_release);
  WakeByAddressAll(&g_progress);
}

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
          const double mode_hz = std::max(1.0, double(float(video_mode.refresh_rate)));
          double hz = -1;  // forces the first computation
          uint64_t interval = 1;
          uint64_t last = rex::chrono::Clock::QueryGuestTickCount();
          // Waits until each vblank on a high-resolution timer: Sleep(1)
          // sleeps a whole 15.6 ms scheduler tick at the default timer
          // resolution, which bunched vblanks and capped frames near 64/s.
          HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                TIMER_ALL_ACCESS);
          const double ticks_per_100ns = double(rex::chrono::Clock::guest_tick_frequency()) / 1e7;
          DisplayVblank display;
          while (running_) {
            if (g_vblank_hz.load(std::memory_order_relaxed) <= 0 && display.Wait(mode_hz)) {
              ++counter_;
              DispatchInterrupt(0, 2);
              Progress();
              last = rex::chrono::Clock::QueryGuestTickCount();  // the timer picks up from here
              continue;
            }
            const uint64_t now = rex::chrono::Clock::QueryGuestTickCount();
            const double want = g_vblank_hz.load(std::memory_order_relaxed);
            if (want != hz) {  // a new rate starts from now, without a burst of catch-up vblanks
              hz = want;
              interval = std::max<uint64_t>(
                  1, uint64_t(double(rex::chrono::Clock::guest_tick_frequency()) / (hz > 0 ? hz : mode_hz)));
              last = std::max(last, now - std::min(now - last, interval));
            }
            while (now - last >= interval) {
              ++counter_;  // the "GPU vblank counter" EVENT_WRITE_SHD can write
              DispatchInterrupt(0, 2);
              Progress();
              last += interval;
            }
            // Until the next vblank, at most 2 ms (rate changes, shutdown).
            const uint64_t until = last + interval - std::min(last + interval, rex::chrono::Clock::QueryGuestTickCount());
            const LONGLONG wait_100ns = std::clamp<LONGLONG>(LONGLONG(double(until) / ticks_per_100ns), 1, 20000);
            LARGE_INTEGER due;
            due.QuadPart = -wait_100ns;
            if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
              WaitForSingleObject(timer, INFINITE);
            } else {
              rex::thread::Sleep(std::chrono::milliseconds(1));
            }
          }
          if (timer) CloseHandle(timer);
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
      Progress();
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
    Progress();
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
        // Until it holds: woken as soon as anything moves (a vblank, the
        // command processor, a CPU write the D3D signals through Progress),
        // else every millisecond. Sleeping whole milliseconds between checks
        // released the game up to a millisecond late after each vblank, and
        // frames came out unevenly (18-20 ms ones followed by short ones).
        while (!Compare(info, ReadPolled(address, info & 0x10) & mask, ref)) {
          if (wait >= 0x100) {
            const uint32_t seen = g_progress.load(std::memory_order_acquire);
            if (Compare(info, ReadPolled(address, info & 0x10) & mask, ref)) break;
            WaitOnAddress(&g_progress, const_cast<uint32_t*>(&seen), sizeof(seen), 1);
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

void SetVblankRate(double hz) { g_vblank_hz.store(hz, std::memory_order_relaxed); }

uint64_t DisplayVblanks() { return g_display_vblanks.load(std::memory_order_relaxed); }

std::unique_ptr<rex::system::IGraphicsSystem> CreateNativeGraphicsSystem() {
  return std::make_unique<NativeGraphicsSystem>();
}

}  // namespace tenchuz::native

// One step of the D3D's GPU-wait loops (sub_824267D0 and others call it until
// the read pointer or a fence gets there; nonzero: keep waiting). On the
// console it spins on yield hints; here that held a host core at 100% for the
// rest of every frame. Without GPU emulation, sleep until the command
// processor makes progress instead (1 ms at most, in case the condition comes
// from somewhere else).
REX_EXTERN(__imp__sub_82425790);
REX_HOOK_RAW(sub_82425790) {
  const uint32_t seen = tenchuz::native::g_progress.load(std::memory_order_acquire);
  __imp__sub_82425790(ctx, base);
  if (ctx.r3.u32 && tenchuz::native::NativeGpu() && REXCVAR_GET(tenchuz_gpu_wait_sleep)) {
    WaitOnAddress(&tenchuz::native::g_progress, const_cast<uint32_t*>(&seen), sizeof(seen), 1);
  }
}
