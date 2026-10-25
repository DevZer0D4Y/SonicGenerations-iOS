// sonicgenerations - loads HedgeModManager mods into the game's CPKs
//
// A mod's disk/bb, disk/bb2 and disk/bb3 folders mirror the inside of bb.cpk,
// bb2.cpk and bb3.cpk. Each file there replaces the CPK file of the same path,
// or is added to the CPK. Files named "+Name.ar.NN" / "+Name.arl" are append
// archives: their entries are merged into the "Name" archive group (a merged
// copy is written to the cache folder, rebuilt only when its inputs change).
// The modded CPKs are then served through the VFS (cpk_overlay.h).

#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "cpk_overlay.h"

namespace rex::filesystem {
class VirtualFileSystem;
}

namespace sg::mods {

struct AppendArchive {
  std::string base;  // "Dir/Name": the group the entries go into
  std::vector<std::filesystem::path> splits;  // +Name.ar.00, .01, ...
};

// What the enabled mods put in one CPK.
struct CpkPlan {
  std::map<std::string, Replacement> files;  // by UpperPath; the top mod's file
  std::vector<AppendArchive> appends;        // lowest priority first
};

// Keyed by CPK name ("bb", "bb2", "bb3"). Mods are given highest priority first.
// Archives holding PC-only data (Direct3D 9 shaders, little-endian Havok
// packfiles) are left out with a warning. Checking an archive split reads its
// entry headers; with scan_cache (a file), the results are kept there and
// reused while a split's path, size and time are unchanged.
std::map<std::string, CpkPlan> PlanMods(const std::vector<std::filesystem::path>& mod_inis,
                                        std::vector<std::string>* warnings,
                                        const std::filesystem::path& scan_cache = {});

// Turns a plan into the CPK's final replacements, merging append archives into
// cache_dir. Returns nullopt (and a warning) if an archive cannot be merged.
std::optional<std::vector<Replacement>> ResolvePlan(const CpkView& cpk, const CpkPlan& plan,
                                                    const std::filesystem::path& cache_dir,
                                                    std::vector<std::string>* warnings);

struct LoadSummary {
  size_t mods = 0;
  size_t files = 0;  // files replaced or added across the CPKs
  std::vector<std::string> warnings;
};

// Attaches the modded CPKs to the mounted game. Run after the VFS is set up
// and before the game starts. root holds mods/ and ios-mods.ini.
LoadSummary ApplyMods(rex::filesystem::VirtualFileSystem& vfs, const std::filesystem::path& root,
                      const std::filesystem::path& cache_dir);

}  // namespace sg::mods
