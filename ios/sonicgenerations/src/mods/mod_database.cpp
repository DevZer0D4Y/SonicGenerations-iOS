// sonicgenerations - the installed mods, their order and which are enabled
//
// Ported from the UnleashedRecompiled iOS mod manager, for the Sonic
// Generations layout: mods replace files inside the game's CPKs rather than
// through the game's own loader, and DLL or HMM-code mods are PC code.

#include "mod_database.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <stdexcept>
#include <string_view>

#include "ini.h"

namespace sg::mods {
namespace {

std::string Lower(std::string s) {
  for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool VersionAtLeast(const std::string& actual, const std::string& required) {
  if (required.empty() || actual == required) return true;
  auto parse = [](const std::string& text, std::vector<unsigned>& result) {
    size_t start = 0;
    while (start < text.size()) {
      const auto end = text.find('.', start);
      const auto length = end == std::string::npos ? text.size() - start : end - start;
      unsigned value = 0;
      const auto conversion =
          std::from_chars(text.data() + start, text.data() + start + length, value);
      if (conversion.ec != std::errc{} || conversion.ptr != text.data() + start + length) {
        return false;
      }
      result.push_back(value);
      if (end == std::string::npos) break;
      start = end + 1;
      if (start == text.size()) return false;
    }
    return !result.empty();
  };
  std::vector<unsigned> a, b;
  if (!parse(actual, a) || !parse(required, b)) return false;
  a.resize(std::max(a.size(), b.size()));
  b.resize(a.size());
  return a >= b;
}

// The CPK folders this loader reads from an include directory.
bool HasDiskFiles(const std::filesystem::path& include_dir) {
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(include_dir / "disk", ec)) {
    if (!entry.is_directory(ec)) continue;
    const auto name = Lower(entry.path().filename().string());
    if (name == "bb" || name == "bb2" || name == "bb3") {
      for (const auto& file : std::filesystem::recursive_directory_iterator(entry.path(), ec)) {
        if (file.is_regular_file(ec)) return true;
      }
    }
  }
  return false;
}

}  // namespace

std::filesystem::path DatabasePath(const std::filesystem::path& root) {
  return root / "ios-mods.ini";
}

std::filesystem::path ModsPath(const std::filesystem::path& root) { return root / "mods"; }

std::filesystem::path ResolveInside(const std::filesystem::path& base, std::string value) {
  std::replace(value.begin(), value.end(), '\\', '/');
  std::filesystem::path path = std::u8string_view(reinterpret_cast<const char8_t*>(value.c_str()));
  // Windows absolute paths ("C:/...") cannot be transplanted into the sandbox.
  if (value.find(':') != std::string::npos || path.is_absolute()) return {};
  auto result = (base / path).lexically_normal();
  auto relative = result.lexically_relative(base.lexically_normal());
  if (relative.empty() || *relative.begin() == "..") return {};
  return result;
}

std::vector<std::filesystem::path> IncludeDirs(const std::filesystem::path& mod_ini) {
  Ini ini;
  std::vector<std::filesystem::path> dirs;
  if (!ini.Read(mod_ini)) return dirs;
  const auto base = mod_ini.parent_path();
  const size_t count = std::min<size_t>(ini.GetSize("Main", "IncludeDirCount", 0), 256);
  for (size_t i = 0; i < count; ++i) {
    auto dir = ResolveInside(base, ini.Get("Main", "IncludeDir" + std::to_string(i)));
    std::error_code ec;
    if (!dir.empty() && std::filesystem::is_directory(dir, ec)) dirs.push_back(std::move(dir));
  }
  return dirs;
}

std::string CompatibilityIssue(const std::filesystem::path& mod_ini) {
  try {
    if (std::filesystem::is_symlink(mod_ini) || std::filesystem::is_symlink(mod_ini.parent_path())) {
      return "Symbolic links are not supported in mods.";
    }
    Ini ini;
    if (!ini.Read(mod_ini)) return "Cannot read mod.ini.";
    if (ini.GetSize("Main", "DependsCount", 0) > 256) return "Invalid dependency count.";
    const auto base = mod_ini.parent_path();
    const size_t count = ini.GetSize("Main", "IncludeDirCount", 0);
    if (count == 0 || count > 256) return "Missing or invalid include directories.";
    bool has_files = false;
    for (size_t i = 0; i < count; ++i) {
      const auto value = ini.Get("Main", "IncludeDir" + std::to_string(i));
      const auto path = ResolveInside(base, value);
      std::error_code ec;
      if (value.empty() || path.empty() || !std::filesystem::is_directory(path, ec)) {
        return "Include directory is missing or outside the mod folder.";
      }
      has_files = has_files || HasDiskFiles(path);
    }
    for (const auto& file : std::filesystem::recursive_directory_iterator(base)) {
      if (file.is_symlink()) return "Symbolic links are not supported in mods.";
    }
    if (!has_files) return "Has no files in disk/bb, disk/bb2 or disk/bb3 to load.";
    return {};
  } catch (const std::exception& ex) {
    return std::string("Cannot inspect mod: ") + ex.what();
  }
}

std::string CodeNote(const std::filesystem::path& mod_ini) {
  Ini ini;
  if (!ini.Read(mod_ini)) return {};
  const bool dll = !ini.Get("Main", "DLLFile").empty();
  const bool code = !ini.Get("Main", "CodeFile").empty();
  if (!dll && !code) return {};
  return std::string("Data only: its ") + (dll ? (code ? "DLL and code file" : "DLL") : "code file") +
         " (new moves, options) are Windows PC code and are skipped.";
}

std::vector<ModEntry> Scan(const std::filesystem::path& root) {
  const auto mods_path = ModsPath(root);
  std::filesystem::create_directories(mods_path);
  Ini database;
  database.Read(DatabasePath(root));
  std::vector<ModEntry> entries;
  for (const auto& folder : std::filesystem::directory_iterator(mods_path)) {
    if (folder.is_symlink() || !folder.is_directory()) continue;
    const auto ini_path = folder.path() / "mod.ini";
    if (!std::filesystem::is_regular_file(ini_path) || std::filesystem::is_symlink(ini_path)) {
      continue;
    }
    Ini ini;
    ini.Read(ini_path);
    ModEntry entry;
    entry.ini_path = ini_path;
    entry.title = ini.Get("Desc", "Title", folder.path().filename().string());
    entry.author = ini.Get("Desc", "Author");
    entry.version = ini.Get("Desc", "Version");
    entry.issue = CompatibilityIssue(ini_path);
    entry.note = CodeNote(ini_path);
    entries.push_back(std::move(entry));
  }
  std::sort(entries.begin(), entries.end(),
            [](const ModEntry& a, const ModEntry& b) { return a.ini_path < b.ini_path; });
  // Restore the saved order, disabled mods included.
  std::vector<ModEntry> ordered;
  const auto count = std::min<size_t>(database.GetSize("Installed", "Count", 0), 10000);
  for (size_t i = 0; i < count; ++i) {
    const auto path = ResolveInside(root, database.Get("Installed", "Mod" + std::to_string(i)));
    auto found = std::find_if(entries.begin(), entries.end(),
                              [&](const ModEntry& e) { return e.ini_path == path; });
    if (found == entries.end()) continue;
    found->enabled =
        found->issue.empty() && database.GetBool("Installed", "Enabled" + std::to_string(i));
    ordered.push_back(std::move(*found));
    entries.erase(found);
  }
  ordered.insert(ordered.end(), entries.begin(), entries.end());
  return ordered;
}

std::string SelectionIssue(const std::vector<ModEntry>& entries) {
  for (const auto& entry : entries) {
    if (!entry.enabled) continue;
    Ini ini;
    if (!ini.Read(entry.ini_path)) return "Cannot read " + entry.title + ".";
    const auto count = ini.GetSize("Main", "DependsCount", 0);
    if (count > 256) return "Invalid dependency count in " + entry.title + ".";
    for (size_t i = 0; i < count; ++i) {
      const auto value = ini.Get("Main", "Depends" + std::to_string(i));
      std::vector<std::string> fields;
      size_t start = 0;
      for (;;) {
        const auto end = value.find('|', start);
        fields.push_back(value.substr(start, end == std::string::npos ? end : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
      }
      if (fields.size() < 3 || fields[0].empty()) {
        return "Invalid dependency declaration in " + entry.title + ".";
      }
      const std::string required = fields.size() > 3 ? fields[3] : "";
      bool satisfied = false;
      for (const auto& candidate : entries) {
        if (!candidate.enabled || candidate.ini_path == entry.ini_path || !candidate.issue.empty()) {
          continue;
        }
        Ini dependency;
        if (dependency.Read(candidate.ini_path) &&
            dependency.Get("Main", "ID") == fields[0] &&
            VersionAtLeast(dependency.Get("Desc", "Version"), required)) {
          satisfied = true;
          break;
        }
      }
      if (!satisfied) {
        return entry.title + " requires an enabled compatible dependency: " + fields[1] +
               (required.empty() ? "." : " (version " + required + " or newer).");
      }
    }
  }
  return {};
}

bool Save(const std::filesystem::path& root, const std::vector<ModEntry>& entries,
          std::string& error) {
  try {
    for (const auto& entry : entries) {
      if (!entry.enabled) continue;
      const auto issue = CompatibilityIssue(entry.ini_path);
      if (!issue.empty()) throw std::runtime_error(entry.title + ": " + issue);
    }
    const auto selection_issue = SelectionIssue(entries);
    if (!selection_issue.empty()) throw std::runtime_error(selection_issue);
    auto temporary = DatabasePath(root);
    temporary += ".tmp";
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    stream << "[Installed]\nCount=" << entries.size() << '\n';
    std::vector<size_t> active;
    for (size_t i = 0; i < entries.size(); ++i) {
      const auto path = entries[i].ini_path.lexically_relative(root).generic_string();
      if (path.find_first_of("\"\r\n") != std::string::npos ||
          ResolveInside(root, path) != entries[i].ini_path) {
        throw std::runtime_error("Invalid mod path.");
      }
      const bool enabled = entries[i].enabled && CompatibilityIssue(entries[i].ini_path).empty();
      stream << "Mod" << i << "=\"" << path << "\"\nEnabled" << i << '=' << enabled << '\n';
      if (enabled) active.push_back(i);
    }
    // HedgeModManager's ModsDB layout, for anyone comparing with a PC setup.
    stream << "[Main]\nActiveModCount=" << active.size() << '\n';
    for (size_t i = 0; i < active.size(); ++i) {
      stream << "ActiveMod" << i << "=ios" << active[i] << '\n';
    }
    stream << "[Mods]\n";
    for (size_t i : active) {
      stream << "ios" << i << "=\"" << entries[i].ini_path.lexically_relative(root).generic_string()
             << "\"\n";
    }
    stream.close();
    if (!stream) throw std::runtime_error("Could not write mod settings.");
    std::filesystem::rename(temporary, DatabasePath(root));
    return true;
  } catch (const std::exception& ex) {
    error = ex.what();
    return false;
  }
}

bool Import(const std::filesystem::path& root, const std::filesystem::path& folder,
            std::string& error) {
  std::filesystem::path staging;
  bool owns_staging = false;
  try {
    if (!std::filesystem::is_regular_file(folder / "mod.ini") || std::filesystem::is_symlink(folder)) {
      throw std::runtime_error("Select the extracted folder containing mod.ini.");
    }
    for (const auto& file : std::filesystem::recursive_directory_iterator(folder)) {
      if (file.is_symlink() || (!file.is_regular_file() && !file.is_directory())) {
        throw std::runtime_error("Mod folders must contain ordinary files and folders.");
      }
    }
    const auto name = folder.filename();
    if (name.empty() || name.string().find_first_of("\"\r\n") != std::string::npos) {
      throw std::runtime_error("Invalid mod folder name.");
    }
    const auto destination = ModsPath(root) / name;
    std::filesystem::create_directories(destination.parent_path());
    if (std::filesystem::exists(destination)) {
      throw std::runtime_error(
          "A mod with this folder name is already installed. Rename the source folder first.");
    }
    staging = root / "mod-import.tmp";
    if (std::filesystem::exists(staging)) {
      throw std::runtime_error(
          "An unfinished import exists at mod-import.tmp. Remove it using Files and try again.");
    }
    owns_staging = true;
    std::filesystem::copy(folder, staging, std::filesystem::copy_options::recursive);
    std::filesystem::rename(staging, destination);
    return true;
  } catch (const std::exception& ex) {
    error = ex.what();
    if (owns_staging) {
      std::error_code ec;
      std::filesystem::remove_all(staging, ec);
    }
    return false;
  }
}

std::vector<std::filesystem::path> EnabledMods(const std::filesystem::path& root) {
  std::vector<std::filesystem::path> mods;
  Ini database;
  if (!database.Read(DatabasePath(root))) return mods;
  const auto count = std::min<size_t>(database.GetSize("Installed", "Count", 0), 10000);
  for (size_t i = 0; i < count; ++i) {
    if (!database.GetBool("Installed", "Enabled" + std::to_string(i))) continue;
    auto path = ResolveInside(root, database.Get("Installed", "Mod" + std::to_string(i)));
    if (!path.empty() && CompatibilityIssue(path).empty()) mods.push_back(std::move(path));
  }
  return mods;
}

}  // namespace sg::mods
