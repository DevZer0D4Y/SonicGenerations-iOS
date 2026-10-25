/**
 * @file        filesystem/overlay_file.cpp
 * @brief       Open file backed by an entry's FileOverlay
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "overlay_file.h"

#include <rex/filesystem.h>

namespace rex::filesystem {

X_STATUS OverlayFile::ReadSync(std::span<uint8_t> buffer, size_t byte_offset,
                               size_t* out_bytes_read) {
  if (!(file_access_ & (FileAccess::kGenericRead | FileAccess::kFileReadData))) {
    return X_STATUS_ACCESS_DENIED;
  }
  // Same contract as HostPathFile: reading at or past the end is END_OF_FILE.
  if (byte_offset >= overlay_->size()) {
    *out_bytes_read = 0;
    return X_STATUS_END_OF_FILE;
  }
  if (!overlay_->Read(byte_offset, buffer, out_bytes_read)) {
    return X_STATUS_END_OF_FILE;
  }
  return X_STATUS_SUCCESS;
}

X_STATUS OverlayFile::WriteSync(std::span<const uint8_t> /*buffer*/, size_t /*byte_offset*/,
                                size_t* out_bytes_written) {
  *out_bytes_written = 0;
  return X_STATUS_ACCESS_DENIED;
}

}  // namespace rex::filesystem
