// tenchuz - PM4 command-stream decoder (native renderer milestone N2).
//
// Replays the register-writing parts of the Xenos command stream the game's
// D3D library emits, the way the GPU's command processor would, so the GPU
// state at each draw can be known without the emulator. Used as ground truth
// when learning how the D3D device object shadows GPU registers.

#pragma once

#include <cstdint>
#include <functional>

namespace tenchuz::pm4 {

constexpr uint32_t kRegisterCount = 0x5003;

struct DrawInfo {
  uint32_t opcode;     // 0x22 DRAW_INDX, 0x36 DRAW_INDX_2, 0x34/0x35 binned variants
  uint32_t words[4];   // first payload dwords (DRAW_INDX: viz query, initiator, index base, index size)
  uint32_t address;    // guest address of the packet
  bool predicated;
};

struct ShaderLoad {
  uint32_t address;    // IM_LOAD: guest physical ucode address; IM_LOAD_IMMEDIATE: packet address
  uint32_t size_dwords;
  bool immediate;
};

class Decoder {
 public:
  Decoder();

  // Decode complete packets in [address, end). Returns the address just past
  // the last complete packet (decoding stops at a partially written packet).
  uint32_t Decode(uint8_t* base, uint32_t address, uint32_t end);

  // Mark every register as not yet written (start of an observation).
  void ResetWritten();

  uint32_t regs[kRegisterCount];
  bool written[kRegisterCount];
  ShaderLoad vertex_shader{};
  ShaderLoad pixel_shader{};
  uint64_t bin_mask = ~0ull;
  uint64_t bin_select = 0;

  std::function<void(const DrawInfo&)> on_draw;

  // The last complete packet decoded: its address and header as read then.
  uint32_t last_packet_address = 0;
  uint32_t last_packet_header = 0;

  // Statistics.
  uint64_t packets = 0;
  uint64_t indirect_buffers = 0;
  uint64_t zero_headers = 0;

 private:
  void Write(uint32_t reg, uint32_t value);
};

}  // namespace tenchuz::pm4
