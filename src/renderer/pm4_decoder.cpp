// tenchuz - PM4 command-stream decoder. See pm4_decoder.h.
//
// Packet semantics follow the SDK command processor (src/graphics/
// command_processor.cpp): type-0/1 register writes, SET_CONSTANT and
// LOAD_ALU_CONSTANT ranges (ALU 0x4000, fetch 0x4800, bool 0x4900, loop 0x4908,
// registers 0x2000), SET_CONSTANT2 / SET_SHADER_CONSTANTS absolute ranges.

#include "pm4_decoder.h"

#include "generated/default/tenchuz_pch.h"

#include <cstring>

namespace tenchuz::pm4 {
namespace {

enum Opcode : uint32_t {
  kRegRmw = 0x21,
  kDrawIndx = 0x22,
  kImLoad = 0x27,
  kImLoadImmediate = 0x2B,
  kSetConstant = 0x2D,
  kLoadAluConstant = 0x2F,
  kDrawIndxBin = 0x34,
  kDrawIndx2Bin = 0x35,
  kDrawIndx2 = 0x36,
  kIndirectBufferPfd = 0x37,
  kIndirectBuffer = 0x3F,
  kSetConstant2 = 0x55,
  kSetShaderConstants = 0x56,
  kSetBinMaskLo = 0x60,
  kSetBinMaskHi = 0x61,
  kSetBinSelectLo = 0x62,
  kSetBinSelectHi = 0x63,
};

constexpr uint32_t kConstantBases[] = {0x4000, 0x4800, 0x4900, 0x4908, 0x2000};

// Guest physical addresses are mirrored at 0xA0000000 in the guest address space.
inline uint32_t LoadPhysical32(uint8_t* base, uint32_t physical) {
  return REX_LOAD_U32(0xA0000000u | (physical & 0x1FFFFFFFu));
}

}  // namespace

Decoder::Decoder() {
  std::memset(regs, 0, sizeof(regs));
  ResetWritten();
}

void Decoder::ResetWritten() {
  std::memset(written, 0, sizeof(written));
}

void Decoder::Write(uint32_t reg, uint32_t value) {
  if (reg < kRegisterCount) {
    regs[reg] = value;
    written[reg] = true;
  }
}

uint32_t Decoder::Decode(uint8_t* base, uint32_t address, uint32_t end) {
  while (address + 4 <= end) {
    const uint32_t header = REX_LOAD_U32(address);
    const uint32_t type = header >> 30;
    uint32_t size = 1;  // in dwords, including the header
    if (type == 0) {
      size = 1 + ((header >> 16) & 0x3FFF) + 1;
    } else if (type == 1) {
      size = 3;
    } else if (type == 3) {
      size = 1 + ((header >> 16) & 0x3FFF) + 1;
    }
    if (address + size * 4 > end) {
      break;  // packet not fully written yet
    }
    auto word = [&](uint32_t i) { return REX_LOAD_U32(address + 4 * i); };
    ++packets;
    last_packet_address = address;
    last_packet_header = header;

    if (type == 0) {
      if (header == 0) {
        ++zero_headers;
      }
      const uint32_t base_reg = header & 0x7FFF;
      const bool one_reg = (header >> 15) & 1;
      for (uint32_t i = 0; i + 1 < size; ++i) {
        Write(one_reg ? base_reg : base_reg + i, word(1 + i));
      }
    } else if (type == 1) {
      Write(header & 0x7FF, word(1));
      Write((header >> 11) & 0x7FF, word(2));
    } else if (type == 3) {
      const uint32_t opcode = (header >> 8) & 0x7F;
      const uint32_t count = size - 1;
      switch (opcode) {
        case kSetConstant: {
          const uint32_t offset_type = word(1);
          const uint32_t kind = (offset_type >> 16) & 0xFF;
          if (kind < 5) {
            const uint32_t first = kConstantBases[kind] + (offset_type & 0x7FF);
            for (uint32_t i = 0; i + 1 < count; ++i) {
              Write(first + i, word(2 + i));
            }
          }
          break;
        }
        case kSetConstant2:
        case kSetShaderConstants: {
          const uint32_t first = word(1) & 0xFFFF;
          for (uint32_t i = 0; i + 1 < count; ++i) {
            Write(first + i, word(2 + i));
          }
          break;
        }
        case kLoadAluConstant: {
          const uint32_t source = word(1) & 0x3FFFFFFF;
          const uint32_t offset_type = word(2);
          const uint32_t kind = (offset_type >> 16) & 0xFF;
          const uint32_t dwords = word(3) & 0xFFF;
          if (kind < 5) {
            const uint32_t first = kConstantBases[kind] + (offset_type & 0x7FF);
            for (uint32_t i = 0; i < dwords; ++i) {
              Write(first + i, LoadPhysical32(base, source + 4 * i));
            }
          }
          break;
        }
        case kRegRmw: {
          const uint32_t info = word(1);
          const uint32_t reg = info & 0x1FFF;
          const uint32_t and_mask = (info >> 31) ? regs[word(2) & 0x1FFF] : word(2);
          const uint32_t or_mask = ((info >> 30) & 1) ? regs[word(3) & 0x1FFF] : word(3);
          Write(reg, (regs[reg] & and_mask) | or_mask);
          break;
        }
        case kImLoad: {
          const uint32_t addr_type = word(1);
          ShaderLoad load{addr_type & ~3u, word(2) & 0xFFFF, false};
          ((addr_type & 3) == 0 ? vertex_shader : pixel_shader) = load;
          break;
        }
        case kImLoadImmediate: {
          const uint32_t shader_type = word(1) & 3;
          ShaderLoad load{address, word(2) & 0xFFFF, true};
          (shader_type == 0 ? vertex_shader : pixel_shader) = load;
          break;
        }
        case kDrawIndx:
        case kDrawIndx2:
        case kDrawIndxBin:
        case kDrawIndx2Bin:
          if (on_draw) {
            DrawInfo info{opcode,
                          {count > 0 ? word(1) : 0, count > 1 ? word(2) : 0, count > 2 ? word(3) : 0,
                           count > 3 ? word(4) : 0},
                          address,
                          (header & 1) != 0};
            on_draw(info);
          }
          break;
        case kIndirectBuffer:
        case kIndirectBufferPfd:
          ++indirect_buffers;  // replays of recorded commands (tiling); not followed
          break;
        case kSetBinMaskLo:
          bin_mask = (bin_mask & ~0xFFFFFFFFull) | word(1);
          break;
        case kSetBinMaskHi:
          bin_mask = (bin_mask & 0xFFFFFFFFull) | (uint64_t(word(1)) << 32);
          break;
        case kSetBinSelectLo:
          bin_select = (bin_select & ~0xFFFFFFFFull) | word(1);
          break;
        case kSetBinSelectHi:
          bin_select = (bin_select & 0xFFFFFFFFull) | (uint64_t(word(1)) << 32);
          break;
        default:
          break;
      }
    }
    address += size * 4;
  }
  return address;
}

}  // namespace tenchuz::pm4
