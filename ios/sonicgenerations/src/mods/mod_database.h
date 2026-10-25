// sonicgenerations - the installed mods, their order and which are enabled
//
// Mods are HedgeModManager mod folders (mod.ini + disk/bb, disk/bb2,
// disk/bb3) under <root>/mods. The selection is stored in <root>/ios-mods.ini
// with paths relative to <root>, so it survives app updates, which move the
// app container. The first enabled mod wins a conflict, as at the top of
// HedgeModManager's list.

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace sg::mods {

struct ModEntry {
  std::filesystem::path ini_path;
  std::string title;
  std::string author;
  std::string version;
  std::string issue;  // why the mod cannot be enabled, or empty
  std::string note;   // what of an enabled mod is left out (its PC code), or empty
  bool enabled = false;
};

std::filesystem::path DatabasePath(const std::filesystem::path& root);
std::filesystem::path ModsPath(const std::filesystem::path& root);

// Resolves a mod.ini path value (Windows separators allowed) inside base;
// empty if it is absolute or leaves base.
std::filesystem::path ResolveInside(const std::filesystem::path& base, std::string value);

// The include directories of a mod, in mod.ini order.
// The PC code a mod.ini names (DLLFile, CodeFile), which is skipped while its
// data loads; empty if it names none.
std::string CodeNote(const std::filesystem::path& mod_ini);

std::vector<std::filesystem::path> IncludeDirs(const std::filesystem::path& mod_ini);

std::string CompatibilityIssue(const std::filesystem::path& mod_ini);

// Installed mods in saved order; newly found ones come last, disabled.
std::vector<ModEntry> Scan(const std::filesystem::path& root);
// A problem with the enabled set (a missing dependency), or empty.
std::string SelectionIssue(const std::vector<ModEntry>& entries);
bool Save(const std::filesystem::path& root, const std::vector<ModEntry>& entries,
          std::string& error);
// Copies an extracted mod folder (the one holding mod.ini) into <root>/mods.
bool Import(const std::filesystem::path& root, const std::filesystem::path& folder,
            std::string& error);
// Enabled mods from the saved selection, highest priority first.
std::vector<std::filesystem::path> EnabledMods(const std::filesystem::path& root);

}  // namespace sg::mods
