// sonicgenerations - a CPK with files replaced or added, served without a copy
//
// The game reads bb.cpk, bb2.cpk and bb3.cpk through CRI's file system, which
// finds a file through the archive's table of contents (TOC). A modded CPK is
// presented to the guest in place of the original, laid out like CRI packs it:
//
//   [0, 2048)            the original header sector, with updated offsets
//   [2048, content)      a rebuilt TOC (sorted, as the original, by the
//                        upper-cased "Dir/File" path)
//   [content, ...)       the original files, moved by a whole number of
//                        2048-byte sectors when the TOC grew
//   then                 each replacement or new file, read from the mod
//   then                 a rebuilt ETOC (per-file update stamps)
//
// Nothing is decompressed or copied: replaced files keep their old bytes in
// the original content, unreferenced, and new data stays in the mod folder.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rex/filesystem/file_overlay.h>

#include "cri_utf.h"

namespace sg::mods {

// Reads from the original archive; same contract as FileOverlay::Read.
using ReadFn = std::function<bool(uint64_t offset, std::span<uint8_t> buffer, size_t* read)>;

class CpkView {
 public:
  static std::optional<CpkView> Open(ReadFn read, uint64_t size, std::string* error);

  // The row of a file (path is "Dir/File" with '/' separators), ignoring case.
  std::optional<size_t> Find(std::string_view path) const;
  std::string PathOf(size_t row) const;
  size_t file_count() const { return toc_.rows.size(); }
  // A file's contents, decompressed.
  std::optional<std::vector<uint8_t>> ReadFile(size_t row) const;

 private:
  friend class CpkBuilder;
  ReadFn read_;
  uint64_t size_ = 0;
  std::vector<uint8_t> first_sector_;  // [0, toc_offset): header chunk and CRI's notice
  cri::Table header_, toc_, etoc_;
  bool has_etoc_ = false;
  uint64_t toc_offset_ = 0, content_offset_ = 0, content_size_ = 0, align_ = 1;
};

struct Replacement {
  std::string path;            // inside the CPK, "Dir/File"
  std::filesystem::path host;  // the file served in its place
  uint64_t size = 0;
};

// Returns nullptr and sets error if a replacement cannot be laid out.
std::shared_ptr<rex::filesystem::FileOverlay> BuildCpkOverlay(
    const CpkView& cpk, const std::vector<Replacement>& replacements, std::string* error);

std::string UpperPath(std::string_view path);

}  // namespace sg::mods
