// sonicgenerations - CRILAYLA, the LZ compression of files inside a CPK

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace sg::cri {

bool IsLayla(std::span<const uint8_t> bytes);

// Returns nullopt for corrupt input.
std::optional<std::vector<uint8_t>> DecompressLayla(std::span<const uint8_t> bytes);

}  // namespace sg::cri
