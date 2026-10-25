// sonicgenerations - CRILAYLA
//
// Layout: "CRILAYLA", the decompressed size of the compressed part (LE u32),
// the size of that compressed bit stream (LE u32), the stream, then the first
// 0x100 bytes of the file stored raw. The stream is read backwards from its
// end and fills the output backwards: a 0 bit is a literal byte, a 1 bit a
// back-reference of a 13-bit distance (+3) and a length coded as 2, 3, 5 and
// then repeated 8-bit increments, each step continuing while it is all ones.

#include "crilayla.h"

#include <cstring>

namespace sg::cri {

bool IsLayla(std::span<const uint8_t> bytes) {
  return bytes.size() >= 16 && std::memcmp(bytes.data(), "CRILAYLA", 8) == 0;
}

std::optional<std::vector<uint8_t>> DecompressLayla(std::span<const uint8_t> bytes) {
  constexpr size_t kRawPrefix = 0x100;
  if (!IsLayla(bytes)) return std::nullopt;
  auto le32 = [&](size_t at) {
    return uint32_t(bytes[at]) | uint32_t(bytes[at + 1]) << 8 | uint32_t(bytes[at + 2]) << 16 |
           uint32_t(bytes[at + 3]) << 24;
  };
  const size_t body_size = le32(8);
  const size_t stream_size = le32(12);
  if (bytes.size() < 16 + stream_size + kRawPrefix || stream_size == 0) return std::nullopt;

  std::vector<uint8_t> out(body_size + kRawPrefix);
  std::memcpy(out.data(), bytes.data() + 16 + stream_size, kRawPrefix);

  // The bit stream ends right before the raw prefix and is consumed backwards.
  const uint8_t* stream = bytes.data() + 16;
  ptrdiff_t next = ptrdiff_t(stream_size) - 1;
  uint32_t pool = 0;
  int left = 0;
  bool ok = true;
  auto bits = [&](int count) -> uint32_t {
    uint32_t value = 0;
    while (count > 0) {
      if (left == 0) {
        if (next < 0) {
          ok = false;
          return 0;
        }
        pool = stream[next--];
        left = 8;
      }
      const int take = count < left ? count : left;
      value = (value << take) | ((pool >> (left - take)) & ((1u << take) - 1));
      left -= take;
      count -= take;
    }
    return value;
  };

  ptrdiff_t write = ptrdiff_t(out.size()) - 1;
  const ptrdiff_t end = ptrdiff_t(kRawPrefix);
  static constexpr int kLengthBits[] = {2, 3, 5, 8};
  while (write >= end && ok) {
    if (bits(1) == 0) {
      out[size_t(write--)] = uint8_t(bits(8));
      continue;
    }
    const size_t distance = bits(13) + 3;
    size_t length = 3;
    uint32_t step = 0;
    int level = 0;
    for (; level < 4; ++level) {
      step = bits(kLengthBits[level]);
      length += step;
      if (step != (1u << kLengthBits[level]) - 1) break;
    }
    if (level == 4) {
      do {
        step = bits(8);
        length += step;
      } while (step == 0xFF && ok);
    }
    for (size_t i = 0; i < length && write >= end; ++i, --write) {
      const size_t from = size_t(write) + distance;
      if (from >= out.size()) return std::nullopt;
      out[size_t(write)] = out[from];
    }
  }
  if (!ok) return std::nullopt;
  return out;
}

}  // namespace sg::cri
