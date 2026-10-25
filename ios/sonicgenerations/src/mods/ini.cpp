// sonicgenerations - the small INI dialect of HedgeModManager's mod.ini

#include "ini.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>

namespace sg::mods {
namespace {

std::string Lower(std::string s) {
  for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string Trim(const std::string& s) {
  const auto first = s.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const auto last = s.find_last_not_of(" \t\r\n");
  return s.substr(first, last - first + 1);
}

}  // namespace

bool Ini::Read(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  sections_.clear();
  std::string line, section;
  bool first = true;
  while (std::getline(in, line)) {
    if (first && line.size() >= 3 && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
    first = false;
    line = Trim(line);
    if (line.empty() || line[0] == ';' || line[0] == '#') continue;
    if (line.front() == '[') {
      const auto close = line.find(']');
      section = Lower(Trim(line.substr(1, close == std::string::npos ? std::string::npos : close - 1)));
      sections_[section];
      continue;
    }
    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    auto value = Trim(line.substr(eq + 1));
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
      value = value.substr(1, value.size() - 2);
    }
    sections_[section][Lower(Trim(line.substr(0, eq)))] = value;
  }
  return true;
}

bool Ini::Has(const std::string& section) const { return sections_.count(Lower(section)) != 0; }

std::string Ini::Get(const std::string& section, const std::string& key,
                     const std::string& fallback) const {
  const auto s = sections_.find(Lower(section));
  if (s == sections_.end()) return fallback;
  const auto k = s->second.find(Lower(key));
  return k == s->second.end() ? fallback : k->second;
}

size_t Ini::GetSize(const std::string& section, const std::string& key, size_t fallback) const {
  const auto text = Get(section, key);
  size_t value = 0;
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  return ec == std::errc{} && end == text.data() + text.size() && !text.empty() ? value : fallback;
}

bool Ini::GetBool(const std::string& section, const std::string& key, bool fallback) const {
  const auto text = Lower(Get(section, key));
  if (text == "1" || text == "true") return true;
  if (text == "0" || text == "false") return false;
  return fallback;
}

}  // namespace sg::mods
