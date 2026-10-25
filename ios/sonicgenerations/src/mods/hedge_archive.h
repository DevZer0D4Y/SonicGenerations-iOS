// sonicgenerations - Hedgehog Engine archives (.ar.NN splits and their .arl)
//
//   .ar.NN  little-endian: header {0, 16, 20, data alignment}, then entries
//           {entry size, data size, data offset, 8-byte stamp, name\0, padding
//           to the alignment, data}, back to back.
//   .arl    "ARL2", split count, each split's size, then every file name of
//           the group as a length byte + characters.
//
// HedgeModManager's append archives ("+Sonic.ar.00" + "+Sonic.arl") hold only
// the files a mod adds or changes; MergeAppend folds them into the base group.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace sg::hedge {

struct ArchiveFile {
  std::string name;
  uint64_t stamp = 0;
  std::vector<uint8_t> data;
};

struct ArchiveGroup {
  uint32_t alignment = 64;
  std::vector<ArchiveFile> files;
  size_t largest_split = 0;  // of the input, to size the output splits alike
};

// Appends the entries of one split. Returns false if it is malformed.
bool ReadSplit(std::span<const uint8_t> split, ArchiveGroup& group);

// Files with the same name (case-insensitive) are replaced in place; new ones
// are appended. The append group's files win.
void MergeAppend(ArchiveGroup& base, const ArchiveGroup& append);

// Packs the group into splits of at most split_limit bytes (a file larger than
// the limit gets a split of its own) and the matching .arl.
struct PackedGroup {
  std::vector<std::vector<uint8_t>> splits;
  std::vector<uint8_t> arl;
};
PackedGroup Pack(const ArchiveGroup& group, size_t split_limit);

// The split count an .arl declares, or nullopt if it is not an ARL2 file.
std::optional<uint32_t> ArlSplitCount(std::span<const uint8_t> arl);

}  // namespace sg::hedge
