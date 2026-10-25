/**
 * @file        filesystem/overlay_file.h
 * @brief       Open file backed by an entry's FileOverlay
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <memory>

#include <rex/filesystem/file.h>
#include <rex/filesystem/file_overlay.h>

namespace rex::filesystem {

class OverlayFile : public File {
 public:
  OverlayFile(uint32_t file_access, Entry* entry, std::shared_ptr<FileOverlay> overlay)
      : File(file_access, entry), overlay_(std::move(overlay)) {}

  void Destroy() override { delete this; }

  X_STATUS ReadSync(std::span<uint8_t> buffer, size_t byte_offset,
                    size_t* out_bytes_read) override;
  X_STATUS WriteSync(std::span<const uint8_t> buffer, size_t byte_offset,
                     size_t* out_bytes_written) override;

 private:
  // Keeps the contents alive while this handle is open.
  std::shared_ptr<FileOverlay> overlay_;
};

}  // namespace rex::filesystem
