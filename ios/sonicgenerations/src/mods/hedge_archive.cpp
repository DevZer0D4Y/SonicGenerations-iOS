// sonicgenerations - Hedgehog Engine archives

#include "hedge_archive.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_map>

namespace sg::hedge {
namespace {

constexpr uint32_t kHeaderSize = 16;
constexpr uint32_t kEntryHeaderSize = 20;

uint32_t Le32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

void PutLe32(std::vector<uint8_t>& out, uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(uint8_t(v >> (8 * i)));
}

void PutLe64(std::vector<uint8_t>& out, uint64_t v) {
  for (int i = 0; i < 8; ++i) out.push_back(uint8_t(v >> (8 * i)));
}

std::string Upper(std::string s) {
  for (auto& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

size_t AlignUp(size_t v, size_t a) { return (v + a - 1) / a * a; }

// The size an entry takes when written at position `at` of a split.
size_t EntrySize(size_t at, const ArchiveFile& file, uint32_t alignment) {
  const size_t data_at = AlignUp(at + kEntryHeaderSize + file.name.size() + 1, alignment);
  return data_at - at + file.data.size();
}

}  // namespace

bool ReadSplit(std::span<const uint8_t> split, ArchiveGroup& group) {
  if (split.size() < kHeaderSize || Le32(split.data()) != 0 ||
      Le32(split.data() + 4) != kHeaderSize || Le32(split.data() + 8) != kEntryHeaderSize) {
    return false;
  }
  const uint32_t alignment = Le32(split.data() + 12);
  if (alignment == 0 || alignment > 0x10000) return false;
  group.alignment = alignment;
  group.largest_split = std::max(group.largest_split, split.size());
  size_t at = kHeaderSize;
  while (at < split.size()) {
    if (split.size() - at < kEntryHeaderSize) return false;
    const uint8_t* e = split.data() + at;
    const uint32_t entry_size = Le32(e), data_size = Le32(e + 4), data_offset = Le32(e + 8);
    if (entry_size == 0 || entry_size > split.size() - at || data_offset > entry_size ||
        data_size > entry_size - data_offset || data_offset < kEntryHeaderSize + 1) {
      return false;
    }
    const auto* name = e + kEntryHeaderSize;
    const auto* name_end =
        static_cast<const uint8_t*>(std::memchr(name, 0, data_offset - kEntryHeaderSize));
    if (!name_end) return false;
    ArchiveFile file;
    file.name.assign(reinterpret_cast<const char*>(name), name_end - name);
    std::memcpy(&file.stamp, e + 12, 8);
    file.data.assign(e + data_offset, e + data_offset + data_size);
    group.files.push_back(std::move(file));
    at += entry_size;
  }
  return true;
}

void MergeAppend(ArchiveGroup& base, const ArchiveGroup& append) {
  std::unordered_map<std::string, size_t> index;
  for (size_t i = 0; i < base.files.size(); ++i) index.emplace(Upper(base.files[i].name), i);
  for (const auto& file : append.files) {
    auto [it, inserted] = index.emplace(Upper(file.name), base.files.size());
    if (inserted) {
      base.files.push_back(file);
    } else {
      base.files[it->second] = file;
    }
  }
}

PackedGroup Pack(const ArchiveGroup& group, size_t split_limit) {
  PackedGroup packed;
  auto start_split = [&] {
    auto& split = packed.splits.emplace_back();
    PutLe32(split, 0);
    PutLe32(split, kHeaderSize);
    PutLe32(split, kEntryHeaderSize);
    PutLe32(split, group.alignment);
  };
  start_split();
  for (const auto& file : group.files) {
    auto* split = &packed.splits.back();
    if (split->size() > kHeaderSize &&
        split->size() + EntrySize(split->size(), file, group.alignment) > split_limit) {
      start_split();
      split = &packed.splits.back();
    }
    const size_t at = split->size();
    const size_t data_at = AlignUp(at + kEntryHeaderSize + file.name.size() + 1, group.alignment);
    PutLe32(*split, uint32_t(data_at - at + file.data.size()));
    PutLe32(*split, uint32_t(file.data.size()));
    PutLe32(*split, uint32_t(data_at - at));
    PutLe64(*split, file.stamp);
    split->insert(split->end(), file.name.begin(), file.name.end());
    split->resize(data_at, 0);
    split->insert(split->end(), file.data.begin(), file.data.end());
  }

  auto& arl = packed.arl;
  arl.insert(arl.end(), {'A', 'R', 'L', '2'});
  PutLe32(arl, uint32_t(packed.splits.size()));
  for (const auto& split : packed.splits) PutLe32(arl, uint32_t(split.size()));
  for (const auto& file : group.files) {
    const size_t length = std::min<size_t>(file.name.size(), 255);
    arl.push_back(uint8_t(length));
    arl.insert(arl.end(), file.name.begin(), file.name.begin() + length);
  }
  return packed;
}

std::optional<uint32_t> ArlSplitCount(std::span<const uint8_t> arl) {
  if (arl.size() < 8 || std::memcmp(arl.data(), "ARL2", 4) != 0) return std::nullopt;
  const uint32_t count = Le32(arl.data() + 4);
  if (count == 0 || count > 1000 || arl.size() < 8 + size_t(count) * 4) return std::nullopt;
  return count;
}

}  // namespace sg::hedge
