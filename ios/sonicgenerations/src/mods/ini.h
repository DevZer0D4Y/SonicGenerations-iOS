// sonicgenerations - the small INI dialect of HedgeModManager's mod.ini

#pragma once

#include <cstddef>
#include <filesystem>
#include <map>
#include <string>

namespace sg::mods {

// [Section] headers, Key=Value lines, ';' and '#' comments. Values may be
// quoted; section and key names compare case-insensitively, as HMM does.
class Ini {
 public:
  bool Read(const std::filesystem::path& path);

  bool Has(const std::string& section) const;
  std::string Get(const std::string& section, const std::string& key,
                  const std::string& fallback = {}) const;
  size_t GetSize(const std::string& section, const std::string& key, size_t fallback = 0) const;
  bool GetBool(const std::string& section, const std::string& key, bool fallback = false) const;

 private:
  std::map<std::string, std::map<std::string, std::string>> sections_;
};

}  // namespace sg::mods
