/**
 * @file        filesystem/file_overlay.h
 * @brief       Replacement contents for a game file, served by the VFS
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace rex::filesystem {

// Read-only contents the guest sees in place of an existing file, for mod
// loaders that rearrange a game archive without copying it. Attach one with
// Entry::SetOverlay after the VFS is mounted and before the guest starts:
// the entry then reports the overlay's size, and VirtualFileSystem::OpenFile
// opens the overlay instead of the original.
class FileOverlay {
 public:
  virtual ~FileOverlay() = default;

  virtual uint64_t size() const = 0;

  // Reads from byte_offset; a read past the end is short. Returns false on a
  // host read error. Called concurrently from guest threads.
  virtual bool Read(uint64_t byte_offset, std::span<uint8_t> buffer, size_t* out_bytes_read) = 0;
};

}  // namespace rex::filesystem
