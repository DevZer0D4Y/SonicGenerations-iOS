// sonicgenerations - loads HedgeModManager mods into the game's CPKs

#include "mod_loader.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string_view>

#include "hedge_archive.h"
#include "mod_database.h"

#ifndef SG_MODS_STANDALONE
#include <rex/filesystem.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#endif

namespace sg::mods {
namespace {

// Hedgehog archive splits stay near the size the game ships; a merged group
// larger than this gets more splits.
constexpr size_t kMinSplitLimit = 10u << 20;

std::string Lower(std::string s) {
  for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// Files a copy from a Mac or Windows PC leaves behind.
bool IsJunk(const std::string& name) {
  const auto lower = Lower(name);
  return name.empty() || name[0] == '.' || lower == "thumbs.db" || lower == "desktop.ini";
}

// "Sonic.ar.00" -> {"Sonic", 0}; "Sonic.arl" -> {"Sonic", -1}; else nullopt.
std::optional<std::pair<std::string, int>> SplitArchiveName(const std::string& name) {
  const auto lower = Lower(name);
  if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".arl") == 0) {
    return std::make_pair(name.substr(0, name.size() - 4), -1);
  }
  if (lower.size() > 6 && lower.compare(lower.size() - 6, 4, ".ar.") == 0 &&
      std::isdigit(static_cast<unsigned char>(lower[lower.size() - 2])) &&
      std::isdigit(static_cast<unsigned char>(lower[lower.size() - 1]))) {
    return std::make_pair(name.substr(0, name.size() - 6), std::stoi(lower.substr(lower.size() - 2)));
  }
  return std::nullopt;
}

std::string JoinPath(const std::string& dir, const std::string& name) {
  return dir.empty() ? name : dir + "/" + name;
}

std::string SplitName(const std::string& base, int index) {
  char suffix[8];
  std::snprintf(suffix, sizeof(suffix), ".ar.%02d", index);
  return base + suffix;
}

std::optional<std::vector<uint8_t>> ReadHostFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

bool WriteHostFile(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  auto temporary = path;
  temporary += ".tmp";
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    if (!out) return false;
  }
  std::filesystem::rename(temporary, path, ec);
  return !ec;
}

// Identifies an input file for the merge cache.
std::string Stamp(const std::filesystem::path& path) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  const auto time =
      static_cast<long long>(std::filesystem::last_write_time(path, ec).time_since_epoch().count());
  return path.generic_string() + "|" + std::to_string(size) + "|" + std::to_string(time) + "\n";
}

bool HasSuffix(const std::string& lower, std::string_view suffix) {
  return lower.size() > suffix.size() &&
         lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool IsShaderName(const std::string& name) {
  const auto lower = Lower(name);
  return HasSuffix(lower, ".wpu") || HasSuffix(lower, ".wvu");
}

bool IsHavokName(const std::string& name) {
  return HasSuffix(Lower(name), ".hkx");
}

// A shader compiled for the PC version: Direct3D 9 bytecode, whose first word
// is the little-endian version token ps_N_N (0xFFFF0N0N) or vs_N_N (0xFFFE...).
// The Xbox 360 game needs Xenos shaders and fails at boot on these.
bool IsPcShader(std::span<const uint8_t> data) {
  return data.size() >= 4 && data[3] == 0xFF && (data[2] == 0xFF || data[2] == 0xFE) && data[1] <= 3;
}

// A Havok packfile built for the PC version (skeletons, animations,
// collision). The magic 57E0E057 reads the same either way round; the layout
// rules that follow the two magic words say little-endian. Packfiles are
// loaded in place with their pointers patched, so the console's big-endian
// code turns a PC one into wild writes.
bool IsPcHavok(std::span<const uint8_t> data) {
  return data.size() > 0x11 && data[0] == 0x57 && data[1] == 0xE0 && data[2] == 0xE0 &&
         data[3] == 0x57 && data[0x11] == 1;
}

// What PC-only data an archive split holds ("Direct3D 9 shaders", "Havok
// files"), or empty. Reads only the entry headers and a few bytes of each
// shader and Havok file, not the whole split.
std::string PcDataInSplit(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) return {};
  const uint64_t size = uint64_t(in.tellg());
  auto read = [&](uint64_t offset, size_t count) {
    std::vector<uint8_t> bytes(size_t(std::min<uint64_t>(count, offset < size ? size - offset : 0)));
    in.clear();
    in.seekg(std::streamoff(offset));
    in.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
    bytes.resize(size_t(in.gcount()));
    return bytes;
  };
  auto le32 = [](const std::vector<uint8_t>& b, size_t at) {
    return uint32_t(b[at]) | uint32_t(b[at + 1]) << 8 | uint32_t(b[at + 2]) << 16 | uint32_t(b[at + 3]) << 24;
  };
  bool shaders = false, havok = false;
  // Header {0, 16, 20, alignment}; entries {entry size, data size, data
  // offset (from the entry), 8-byte stamp, name\0, padding, data}.
  for (uint64_t pos = 16; pos + 21 <= size && !(shaders && havok);) {
    const auto header = read(pos, 20 + 256);
    if (header.size() < 21) break;
    const uint32_t entry_size = le32(header, 0), data_size = le32(header, 4), data_offset = le32(header, 8);
    if (entry_size < 21 || pos + entry_size > size) break;
    const auto name_end = std::find(header.begin() + 20, header.end(), uint8_t(0));
    const std::string name(header.begin() + 20, name_end);
    if (data_size >= 4 && (IsShaderName(name) || IsHavokName(name))) {
      const auto head = read(pos + data_offset, std::min<uint32_t>(data_size, 0x12));
      shaders = shaders || (IsShaderName(name) && IsPcShader(head));
      havok = havok || (IsHavokName(name) && IsPcHavok(head));
    }
    pos += entry_size;
  }
  if (shaders && havok) return "Direct3D 9 shaders and Havok files";
  if (shaders) return "Direct3D 9 shaders";
  if (havok) return "Havok files";
  return {};
}

// Mods made for the PC version can ship its own builds of platform-specific
// files: shaders (Direct3D 9) and Havok packfiles (little-endian). The Xbox
// 360 game crashes or hangs on either, so the archive groups and loose files
// holding them are dropped; the rest of the mod still loads.
// Stamp(split) -> PcDataInSplit's answer, "-" for none.
using ScanResults = std::map<std::string, std::string>;

std::string CachedPcData(const std::filesystem::path& split, ScanResults& results) {
  auto& result = results[Stamp(split)];
  if (result.empty()) {
    result = PcDataInSplit(split);
    if (result.empty()) result = "-";
  }
  return result == "-" ? std::string() : result;
}

void DropPcOnlyData(CpkPlan& plan, const std::string& cpk, ScanResults& results,
                    std::vector<std::string>* warnings) {
  std::map<std::string, std::string> bad;  // UpperPath of the group or file -> what it holds
  for (const auto& [key, file] : plan.files) {
    const auto name = std::filesystem::path(file.path).filename().string();
    if (IsShaderName(name) || IsHavokName(name)) {
      std::ifstream in(file.host, std::ios::binary);
      uint8_t head[0x12] = {};
      in.read(reinterpret_cast<char*>(head), sizeof(head));
      if (IsShaderName(name) && IsPcShader(head)) bad[key] = "Direct3D 9 shaders";
      if (IsHavokName(name) && IsPcHavok(head)) bad[key] = "Havok files";
      continue;
    }
    const auto parts = SplitArchiveName(name);
    if (!parts || parts->second < 0) continue;
    const auto dir = std::filesystem::path(file.path).parent_path().generic_string();
    const auto base = UpperPath(JoinPath(dir, parts->first)) + ".AR";
    if (bad.count(base)) continue;
    if (auto what = CachedPcData(file.host, results); !what.empty()) bad[base] = what;
  }
  std::erase_if(plan.files, [&](const auto& item) {
    const auto name = std::filesystem::path(item.second.path).filename().string();
    const auto parts = SplitArchiveName(name);
    const auto dir = std::filesystem::path(item.second.path).parent_path().generic_string();
    return bad.count(item.first) || (parts && bad.count(UpperPath(JoinPath(dir, parts->first)) + ".AR"));
  });
  std::erase_if(plan.appends, [&](const AppendArchive& append) {
    for (const auto& split : append.splits) {
      if (auto what = CachedPcData(split, results); !what.empty()) {
        bad["+" + UpperPath(append.base) + ".AR"] = what;
        return true;
      }
    }
    return false;
  });
  if (warnings) {
    for (const auto& [path, what] : bad) {
      warnings->push_back("Skipped " + cpk + "/" + path + ": it holds PC " + what +
                          ", which the Xbox 360 game cannot use");
    }
  }
}

}  // namespace

std::map<std::string, CpkPlan> PlanMods(const std::vector<std::filesystem::path>& mod_inis,
                                        std::vector<std::string>* warnings,
                                        const std::filesystem::path& scan_cache) {
  std::map<std::string, CpkPlan> plans;
  // Lowest priority first, so a later (higher) mod overwrites an earlier one.
  for (auto mod = mod_inis.rbegin(); mod != mod_inis.rend(); ++mod) {
    for (const auto& include : IncludeDirs(*mod)) {
      std::error_code ec;
      for (const auto& folder : std::filesystem::directory_iterator(include / "disk", ec)) {
        const auto cpk = Lower(folder.path().filename().string());
        if (!folder.is_directory(ec) || (cpk != "bb" && cpk != "bb2" && cpk != "bb3")) continue;
        auto& plan = plans[cpk];
        std::map<std::string, AppendArchive> appends;  // this mod's, by upper base
        for (const auto& file : std::filesystem::recursive_directory_iterator(folder.path(), ec)) {
          const auto name = file.path().filename().string();
          if (!file.is_regular_file(ec) || IsJunk(name)) continue;
          const auto relative = file.path().lexically_relative(folder.path()).parent_path();
          const auto dir = relative.empty() || relative == "." ? std::string()
                                                               : relative.generic_string();
          if (name[0] == '+') {
            const auto parts = SplitArchiveName(name.substr(1));
            if (!parts) {
              if (warnings) warnings->push_back("Ignored " + name + ": not an archive split");
              continue;
            }
            const auto base = JoinPath(dir, parts->first);
            auto& append = appends[UpperPath(base)];
            append.base = base;
            if (parts->second >= 0) append.splits.push_back(file.path());
            continue;
          }
          const auto path = JoinPath(dir, name);
          plan.files[UpperPath(path)] = {path, file.path(), std::filesystem::file_size(file.path(), ec)};
        }
        for (auto& [key, append] : appends) {
          std::sort(append.splits.begin(), append.splits.end());
          if (!append.splits.empty()) plan.appends.push_back(std::move(append));
        }
      }
    }
  }
  // The cache holds "stamp line\tanswer" lines; a stamp ends in '\n' itself.
  ScanResults previous, results;
  if (!scan_cache.empty()) {
    if (auto bytes = ReadHostFile(scan_cache)) {
      std::istringstream lines(std::string(bytes->begin(), bytes->end()));
      for (std::string line; std::getline(lines, line);) {
        const auto tab = line.rfind('\t');
        if (tab != std::string::npos) previous[line.substr(0, tab) + "\n"] = line.substr(tab + 1);
      }
    }
  }
  results = previous;
  for (auto& [cpk, plan] : plans) DropPcOnlyData(plan, cpk, results, warnings);
  if (!scan_cache.empty() && results != previous) {
    std::string text;
    for (const auto& [stamp, answer] : results) {
      text += stamp.substr(0, stamp.size() - 1) + "\t" + answer + "\n";
    }
    WriteHostFile(scan_cache, std::vector<uint8_t>(text.begin(), text.end()));
  }
  return plans;
}

std::optional<std::vector<Replacement>> ResolvePlan(const CpkView& cpk, const CpkPlan& plan,
                                                    const std::filesystem::path& cache_dir,
                                                    std::vector<std::string>* warnings) {
  std::map<std::string, Replacement> files = plan.files;
  // Group the appends by target archive, keeping priority order.
  std::vector<std::string> bases;
  std::map<std::string, std::vector<const AppendArchive*>> by_base;
  for (const auto& append : plan.appends) {
    auto& list = by_base[UpperPath(append.base)];
    if (list.empty()) bases.push_back(append.base);
    list.push_back(&append);
  }

  for (const auto& base : bases) {
    const auto& appends = by_base[UpperPath(base)];
    // The base group as the game would load it: a mod's replacement, else the CPK's.
    auto read = [&](const std::string& path) -> std::optional<std::vector<uint8_t>> {
      if (auto it = files.find(UpperPath(path)); it != files.end()) return ReadHostFile(it->second.host);
      if (auto row = cpk.Find(path)) return cpk.ReadFile(*row);
      return std::nullopt;
    };
    std::string key;
    for (const auto* append : appends) {
      for (const auto& split : append->splits) key += Stamp(split);
    }
    for (const auto& path : {base + ".arl", SplitName(base, 0)}) {
      if (auto it = files.find(UpperPath(path)); it != files.end()) key += Stamp(it->second.host);
    }

    const auto out_dir = (cache_dir / base).parent_path();
    const auto out_name = std::filesystem::path(base).filename().string();
    const auto key_path = out_dir / (out_name + ".merge-key");
    std::vector<std::filesystem::path> outputs;
    bool cached = false;
    if (auto previous = ReadHostFile(key_path)) {
      const std::string text(previous->begin(), previous->end());
      std::istringstream lines(text);
      std::string first;
      std::getline(lines, first);
      if (text.substr(first.size() + 1) == key) {
        const auto count = std::stoul(first);
        cached = count > 0;
        for (unsigned long i = 0; i < count && cached; ++i) {
          outputs.push_back(out_dir / SplitName(out_name, int(i)));
          cached = std::filesystem::exists(outputs.back());
        }
        cached = cached && std::filesystem::exists(out_dir / (out_name + ".arl"));
      }
    }

    if (!cached) {
      hedge::ArchiveGroup group;
      if (auto arl = read(base + ".arl")) {
        const auto count = hedge::ArlSplitCount(*arl);
        for (uint32_t i = 0; count && i < *count; ++i) {
          auto split = read(SplitName(base, int(i)));
          if (!split || !hedge::ReadSplit(*split, group)) {
            if (warnings) warnings->push_back("Cannot read " + SplitName(base, int(i)) + " to merge into");
            return std::nullopt;
          }
        }
      }
      for (const auto* append : appends) {
        hedge::ArchiveGroup extra;
        for (const auto& split : append->splits) {
          auto bytes = ReadHostFile(split);
          if (!bytes || !hedge::ReadSplit(*bytes, extra)) {
            if (warnings) warnings->push_back("Cannot read append archive " + split.string());
            return std::nullopt;
          }
        }
        hedge::MergeAppend(group, extra);
      }
      const auto packed = hedge::Pack(group, std::max(group.largest_split, kMinSplitLimit));
      outputs.clear();
      for (size_t i = 0; i < packed.splits.size(); ++i) {
        outputs.push_back(out_dir / SplitName(out_name, int(i)));
        if (!WriteHostFile(outputs.back(), packed.splits[i])) {
          if (warnings) warnings->push_back("Cannot write " + outputs.back().string());
          return std::nullopt;
        }
      }
      const std::string header = std::to_string(outputs.size()) + "\n" + key;
      if (!WriteHostFile(out_dir / (out_name + ".arl"), packed.arl) ||
          !WriteHostFile(key_path, std::vector<uint8_t>(header.begin(), header.end()))) {
        if (warnings) warnings->push_back("Cannot write the merged " + base + ".arl");
        return std::nullopt;
      }
    }

    // The merged group replaces the base's .arl and splits. Splits of the
    // original beyond the merged count stay in the CPK, unreferenced.
    std::error_code ec;
    for (size_t i = 0; i < outputs.size(); ++i) {
      const auto path = SplitName(base, int(i));
      files[UpperPath(path)] = {path, outputs[i], std::filesystem::file_size(outputs[i], ec)};
    }
    const auto arl = out_dir / (out_name + ".arl");
    files[UpperPath(base + ".arl")] = {base + ".arl", arl, std::filesystem::file_size(arl, ec)};
  }

  std::vector<Replacement> result;
  result.reserve(files.size());
  for (auto& [key, replacement] : files) result.push_back(std::move(replacement));
  return result;
}

#ifndef SG_MODS_STANDALONE
LoadSummary ApplyMods(rex::filesystem::VirtualFileSystem& vfs, const std::filesystem::path& root,
                      const std::filesystem::path& cache_dir) {
  LoadSummary summary;
  const auto mods = EnabledMods(root);
  summary.mods = mods.size();
  if (mods.empty()) return summary;
  for (const auto& mod : mods) REXLOG_INFO("Mod: {}", mod.parent_path().filename().string());

  auto plans = PlanMods(mods, &summary.warnings, cache_dir / "pc-data-scan.txt");
  for (auto& [name, plan] : plans) {
    if (plan.files.empty() && plan.appends.empty()) continue;
    const std::string guest_path = "game:\\disk\\" + name + ".cpk";
    auto* entry = vfs.ResolvePath(guest_path);
    rex::filesystem::File* raw = nullptr;
    if (!entry || XFAILED(entry->Open(rex::filesystem::FileAccess::kFileReadData, &raw)) || !raw) {
      summary.warnings.push_back("Mods for " + name + ".cpk skipped: the game has no " + guest_path);
      continue;
    }
    std::shared_ptr<rex::filesystem::File> original(raw, [](rex::filesystem::File* f) { f->Destroy(); });
    ReadFn read = [original](uint64_t offset, std::span<uint8_t> buffer, size_t* got) {
      return XSUCCEEDED(original->ReadSync(buffer, size_t(offset), got));
    };
    std::string error;
    auto cpk = CpkView::Open(read, entry->size(), &error);
    if (!cpk) {
      summary.warnings.push_back(name + ".cpk: " + error);
      continue;
    }
    auto replacements = ResolvePlan(*cpk, plan, cache_dir / name, &summary.warnings);
    if (!replacements) continue;
    auto overlay = BuildCpkOverlay(*cpk, *replacements, &error);
    if (!overlay) {
      summary.warnings.push_back(name + ".cpk: " + error);
      continue;
    }
    entry->SetOverlay(std::move(overlay));
    summary.files += replacements->size();
    REXLOG_INFO("Mods: {}.cpk serves {} replaced or added files", name, replacements->size());
  }
  for (const auto& warning : summary.warnings) REXLOG_WARN("Mods: {}", warning);
  return summary;
}
#endif

}  // namespace sg::mods
