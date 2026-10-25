// sonicgenerations - mod loader tests, run on a Mac against the game's CPKs
//
//   bash tests/run-mod-tests.sh "<extracted disc folder>/disk"
//
// 1. Every @UTF table of bb/bb2/bb3.cpk parses and re-serializes to the same
//    bytes, and a CPK overlay with no mods reads back identical to the file.
// 2. Replacing and adding files: the overlaid CPK, parsed again from the
//    overlay alone, serves the new files and every other file unchanged.
// 3. An append archive merges into the archive group it names.
// 4. The mod database: import, order, enable/disable, DLL mods as data only.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <unistd.h>
#include <string>
#include <vector>

#include "../src/mods/cpk_overlay.h"
#include "../src/mods/cri_utf.h"
#include "../src/mods/crilayla.h"
#include "../src/mods/hedge_archive.h"
#include "../src/mods/mod_database.h"
#include "../src/mods/mod_loader.h"

namespace fs = std::filesystem;
using namespace sg;

static int failures = 0;
#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                         \
    }                                                                     \
  } while (0)

static mods::ReadFn FileReader(const fs::path& path) {
  auto file = std::shared_ptr<std::FILE>(std::fopen(path.c_str(), "rb"), [](std::FILE* f) {
    if (f) std::fclose(f);
  });
  return [file](uint64_t offset, std::span<uint8_t> buffer, size_t* got) {
    *got = ::pread(fileno(file.get()), buffer.data(), buffer.size(), off_t(offset));
    return *got != size_t(-1);
  };
}

static mods::ReadFn OverlayReader(std::shared_ptr<rex::filesystem::FileOverlay> overlay) {
  return [overlay](uint64_t offset, std::span<uint8_t> buffer, size_t* got) {
    return overlay->Read(offset, buffer, got);
  };
}

static std::vector<uint8_t> ReadRange(const mods::ReadFn& read, uint64_t offset, size_t size) {
  std::vector<uint8_t> out(size);
  size_t got = 0;
  if (!read(offset, out, &got)) got = 0;
  out.resize(got);
  return out;
}

static void WriteFile(const fs::path& path, const std::vector<uint8_t>& bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
                                              std::streamsize(bytes.size()));
}

static void WriteText(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}

static uint64_t Le64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

// 1. Round trips.
static void TestRoundTrip(const fs::path& cpk_path) {
  const auto size = fs::file_size(cpk_path);
  auto read = FileReader(cpk_path);
  for (uint64_t offset : {uint64_t(0)}) {
    auto head = ReadRange(read, offset, 16);
    auto table = ReadRange(read, offset + 16, size_t(Le64(head.data() + 8)));
    auto parsed = cri::Table::Parse(table);
    CHECK(parsed && parsed->Serialize() == table);
    if (!parsed) return;
    for (const char* column : {"TocOffset", "EtocOffset"}) {
      const uint64_t at = parsed->GetNumber(0, column);
      auto chunk_head = ReadRange(read, at, 16);
      auto chunk = ReadRange(read, at + 16, size_t(Le64(chunk_head.data() + 8)));
      auto sub = cri::Table::Parse(chunk);
      CHECK(sub && sub->Serialize() == chunk);
    }
  }

  std::string error;
  auto cpk = mods::CpkView::Open(read, size, &error);
  CHECK(cpk.has_value());
  if (!cpk) {
    std::fprintf(stderr, "  %s\n", error.c_str());
    return;
  }
  auto overlay = mods::BuildCpkOverlay(*cpk, {}, &error);
  CHECK(overlay && overlay->size() == size);
  if (!overlay) return;
  // The whole file, in uneven chunks so reads straddle segment boundaries.
  std::mt19937 rng(1);
  const size_t kMax = 8u << 20;
  std::vector<uint8_t> a(kMax), b(kMax);
  uint64_t offset = 0;
  bool same = true;
  while (offset < size && same) {
    const size_t n = std::min<uint64_t>(size - offset, 1 + rng() % kMax);
    size_t got_a = 0, got_b = 0;
    read(offset, std::span(a.data(), n), &got_a);
    overlay->Read(offset, std::span(b.data(), n), &got_b);
    same = got_a == n && got_b == n && std::memcmp(a.data(), b.data(), n) == 0;
    if (!same) std::fprintf(stderr, "  differs near offset %llu\n", (unsigned long long)offset);
    offset += n;
  }
  CHECK(same);
  std::printf("round trip ok: %s (%zu files)\n", cpk_path.filename().c_str(), cpk->file_count());
}

// 2 + 3. Replacement, addition and append merge.
static void TestMods(const fs::path& cpk_path, const fs::path& scratch) {
  const auto size = fs::file_size(cpk_path);
  std::string error;
  auto cpk = mods::CpkView::Open(FileReader(cpk_path), size, &error);
  CHECK(cpk.has_value());
  if (!cpk) return;
  const auto sonic_arl = cpk->Find("Languages/Japanese/sonic.ARL");
  CHECK(sonic_arl.has_value());
  if (!sonic_arl) return;

  // A mod: replaces #Application.arl, adds a file in a new folder and one at
  // the root, and appends to the Japanese Sonic archive group (one file).
  const auto mod = scratch / "mods" / "TestMod";
  WriteText(mod / "mod.ini",
            "[Main]\nIncludeDir0=\".\"\nIncludeDirCount=1\nID=\"test\"\n[Desc]\nTitle=\"Test mod\"\n");
  std::vector<uint8_t> replaced(100003);
  for (size_t i = 0; i < replaced.size(); ++i) replaced[i] = uint8_t(i * 7);
  WriteFile(mod / "disk/bb3/#Application.arl", replaced);
  WriteFile(mod / "disk/bb3/ZZTest/New File.bin", {1, 2, 3, 4, 5});
  WriteFile(mod / "disk/bb3/AAA.bin", {9});
  WriteFile(mod / "disk/bb3/.DS_Store", {0});

  hedge::ArchiveGroup append;
  append.files.push_back({"mat_qte_en_001.dds", 0, std::vector<uint8_t>(5000, 0xAB)});
  append.files.push_back({"added_by_mod.dds", 0, std::vector<uint8_t>(333, 0xCD)});
  auto packed = hedge::Pack(append, 1u << 30);
  WriteFile(mod / "disk/bb3/Languages/Japanese/+Sonic.ar.00", packed.splits[0]);
  WriteFile(mod / "disk/bb3/Languages/Japanese/+Sonic.arl", packed.arl);

  std::vector<std::string> warnings;
  auto plans = mods::PlanMods({mod / "mod.ini"}, &warnings);
  CHECK(plans.count("bb3") == 1);
  CHECK(plans["bb3"].files.size() == 3);  // .DS_Store and the + files are not plain files
  CHECK(plans["bb3"].appends.size() == 1);
  auto replacements = mods::ResolvePlan(*cpk, plans["bb3"], scratch / "cache" / "bb3", &warnings);
  CHECK(replacements.has_value());
  for (const auto& w : warnings) std::fprintf(stderr, "  warning: %s\n", w.c_str());
  if (!replacements) return;
  auto overlay = mods::BuildCpkOverlay(*cpk, *replacements, &error);
  CHECK(overlay != nullptr);
  if (!overlay) return;

  auto modded = mods::CpkView::Open(OverlayReader(overlay), overlay->size(), &error);
  CHECK(modded.has_value());
  if (!modded) return;
  CHECK(modded->file_count() == cpk->file_count() + 2);
  auto check_file = [&](const char* path, const std::vector<uint8_t>& want) {
    auto row = modded->Find(path);
    CHECK(row.has_value());
    if (row) CHECK(modded->ReadFile(*row) == want);
  };
  check_file("#Application.arl", replaced);
  check_file("zztest/new file.bin", {1, 2, 3, 4, 5});
  check_file("AAA.bin", {9});

  // Sorted as CRI sorts, so its binary search finds every file.
  std::vector<std::string> paths;
  for (size_t i = 0; i < modded->file_count(); ++i) paths.push_back(mods::UpperPath(modded->PathOf(i)));
  CHECK(std::is_sorted(paths.begin(), paths.end()));

  // The merged Sonic group: the original entry replaced, the new one added.
  auto arl_row = modded->Find("languages/japanese/Sonic.arl");
  auto split_row = modded->Find("Languages/Japanese/Sonic.ar.00");
  CHECK(arl_row && split_row);
  if (arl_row && split_row) {
    auto arl = modded->ReadFile(*arl_row);
    CHECK(arl && hedge::ArlSplitCount(*arl) == 1u);
    hedge::ArchiveGroup group;
    auto split = modded->ReadFile(*split_row);
    CHECK(split && hedge::ReadSplit(*split, group));
    CHECK(group.files.size() == 2);
    if (group.files.size() == 2) {
      CHECK(group.files[0].name == "mat_qte_en_001.dds" && group.files[0].data.size() == 5000);
      CHECK(group.files[1].name == "added_by_mod.dds" && group.files[1].data.size() == 333);
    }
  }

  // Every other file is served unchanged.
  size_t compared = 0;
  for (size_t row = 0; row < cpk->file_count(); ++row) {
    const auto path = cpk->PathOf(row);
    const auto upper = mods::UpperPath(path);
    if (upper == "#APPLICATION.ARL" || upper == "LANGUAGES/JAPANESE/SONIC.ARL" ||
        upper == "LANGUAGES/JAPANESE/SONIC.AR.00") {
      continue;
    }
    auto other = modded->Find(path);
    CHECK(other.has_value());
    if (!other) continue;
    const bool same = cpk->ReadFile(row) == modded->ReadFile(*other);
    CHECK(same);
    if (!same) std::fprintf(stderr, "  changed: %s\n", path.c_str());
    ++compared;
  }
  std::printf("mods ok: %zu untouched files identical, %zu replacements\n", compared,
              replacements->size());

  // The merge is cached: a second resolve reuses it without rewriting.
  const auto merged = scratch / "cache" / "bb3" / "Languages" / "Japanese" / "Sonic.ar.00";
  const auto before = fs::last_write_time(merged);
  auto again = mods::ResolvePlan(*cpk, plans["bb3"], scratch / "cache" / "bb3", &warnings);
  CHECK(again.has_value() && fs::last_write_time(merged) == before);
}

// 4. The database.
// Archives holding PC-only data (Direct3D 9 shaders, little-endian Havok
// packfiles) are dropped; Xbox 360 builds of the same files are kept.
static void TestPcOnlyData(const fs::path& scratch) {
  const auto mod = scratch / "pc-data";
  WriteText(mod / "mod.ini", "[Main]\nIncludeDir0=\".\"\nIncludeDirCount=1\n[Desc]\nTitle=\"PC\"\n");
  auto pack = [](std::string name, std::vector<uint8_t> data) {
    hedge::ArchiveGroup group;
    group.files.push_back({"Bloom.pixelshader", 0, {1, 2, 3, 4}});
    group.files.push_back({std::move(name), 0, std::move(data)});
    return hedge::Pack(group, 1u << 20);
  };
  auto havok = [](uint8_t little_endian) {
    std::vector<uint8_t> data = {0x57, 0xE0, 0xE0, 0x57, 0x10, 0xC0, 0xC0, 0x10, 0, 0, 0, 0,
                                 0, 0, 0, 8, 4, little_endian, 0, 1, 0, 0, 0, 0};
    return data;
  };
  auto write = [&](const std::string& base, const hedge::PackedGroup& packed) {
    WriteFile(mod / ("disk/bb3/" + base + ".ar.00"), packed.splits[0]);
    WriteFile(mod / ("disk/bb3/" + base + ".arl"), packed.arl);
  };
  write("shader_r", pack("Bloom.wpu", {0x00, 0x03, 0xFF, 0xFF, 0x42, 0x00}));   // ps_3_0
  write("shader_d", pack("Bloom.wpu", {0x10, 0x2A, 0x11, 0x00, 0x00, 0x00}));   // Xenos
  write("SonicPc", pack("chr_Sonic_HD.skl.hkx", havok(1)));
  write("SonicXbox", pack("chr_Sonic_HD.skl.hkx", havok(0)));
  WriteFile(mod / "disk/bb3/Loose.wvu", {0x00, 0x03, 0xFE, 0xFF});  // vs_3_0
  WriteFile(mod / "disk/bb3/Loose.anm.hkx", havok(1));
  WriteFile(mod / "disk/bb3/Kept.anm.hkx", havok(0));
  write("+Append", pack("sc_walk.anm.hkx", havok(1)));
  std::vector<std::string> warnings;
  auto plans = mods::PlanMods({mod / "mod.ini"}, &warnings);
  const auto& plan = plans["bb3"];
  const auto& files = plan.files;
  CHECK(!files.count("SHADER_R.AR.00") && !files.count("SHADER_R.ARL") && !files.count("LOOSE.WVU"));
  CHECK(!files.count("SONICPC.AR.00") && !files.count("SONICPC.ARL") && !files.count("LOOSE.ANM.HKX"));
  CHECK(files.count("SHADER_D.AR.00") && files.count("SHADER_D.ARL"));
  CHECK(files.count("SONICXBOX.AR.00") && files.count("SONICXBOX.ARL") && files.count("KEPT.ANM.HKX"));
  CHECK(plan.appends.empty());
  CHECK(warnings.size() == 5);
  // The same answers come back from the scan cache.
  const auto cache = scratch / "pc-data-scan.txt";
  std::vector<std::string> first, second;
  mods::PlanMods({mod / "mod.ini"}, &first, cache);
  CHECK(fs::exists(cache));
  auto cached = mods::PlanMods({mod / "mod.ini"}, &second, cache);
  CHECK(first == warnings && second == warnings && cached["bb3"].files.size() == files.size());
  std::printf("pc-only data ok\n");
}

static void TestDatabase(const fs::path& scratch) {
  const auto root = scratch / "db";
  fs::create_directories(root);
  const auto source = scratch / "src";
  WriteText(source / "Good/mod.ini",
            "[Main]\nIncludeDir0=\".\"\nIncludeDirCount=1\nID=\"good\"\n[Desc]\nTitle=\"Good\"\nVersion=\"1.2\"\n");
  WriteFile(source / "Good/disk/bb/Sonic.arl", {1});
  WriteText(source / "Dll/mod.ini",
            "[Main]\nIncludeDir0=\".\"\nIncludeDirCount=1\nDLLFile=\"mod.dll\"\n[Desc]\nTitle=\"Dll\"\n");
  WriteFile(source / "Dll/disk/bb/Sonic.arl", {1});
  WriteText(source / "Needs/mod.ini",
            "[Main]\nIncludeDir0=\".\"\nIncludeDirCount=1\nDepends0=\"good|Good|url|1.3\"\nDependsCount=1\n"
            "[Desc]\nTitle=\"Needs\"\n");
  WriteFile(source / "Needs/disk/bb2/x.bin", {1});
  WriteText(source / "Empty/mod.ini", "[Main]\nIncludeDir0=\".\"\nIncludeDirCount=1\n");
  WriteText(source / "Escape/mod.ini", "[Main]\nIncludeDir0=\"..\\..\"\nIncludeDirCount=1\n");

  std::string error;
  for (const char* name : {"Good", "Dll", "Needs", "Empty", "Escape"}) {
    CHECK(mods::Import(root, source / name, error));
  }
  CHECK(!mods::Import(root, source / "Good", error));  // duplicate folder name
  auto entries = mods::Scan(root);
  CHECK(entries.size() == 5);
  for (auto& e : entries) {
    const auto name = e.ini_path.parent_path().filename().string();
    if (name == "Dll") CHECK(e.issue.empty() && e.note.find("DLL") != std::string::npos);
    if (name != "Dll") CHECK(e.note.empty());
    if (name == "Empty") CHECK(!e.issue.empty());
    if (name == "Escape") CHECK(!e.issue.empty());
    if (name == "Good" || name == "Needs" || name == "Dll") CHECK(e.issue.empty());
    e.enabled = e.issue.empty();
  }
  // Needs asks for Good 1.3; Good is 1.2.
  CHECK(!mods::Save(root, entries, error) && error.find("Good") != std::string::npos);
  for (auto& e : entries) {
    if (e.ini_path.parent_path().filename() == "Needs") e.enabled = false;
  }
  // Put Good last, then check the order and selection survive a rescan.
  std::stable_partition(entries.begin(), entries.end(),
                        [](const mods::ModEntry& e) { return e.ini_path.parent_path().filename() != "Good"; });
  CHECK(mods::Save(root, entries, error));
  auto again = mods::Scan(root);
  CHECK(again.size() == 5 && again.back().ini_path.parent_path().filename() == "Good" && again.back().enabled);
  auto enabled = mods::EnabledMods(root);
  // Dll loads as data only; Good was moved last.
  CHECK(enabled.size() == 2 && enabled[0].parent_path().filename() == "Dll" &&
        enabled[1].parent_path().filename() == "Good");
  std::printf("database ok\n");
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <game disk folder> [scratch folder]\n", argv[0]);
    return 2;
  }
  const fs::path disk = argv[1];
  const fs::path scratch = argc > 2 ? fs::path(argv[2]) : fs::temp_directory_path() / "sg-mod-tests";
  fs::remove_all(scratch);
  fs::create_directories(scratch);
  for (const char* name : {"bb3.cpk", "bb.cpk", "bb2.cpk"}) {
    if (fs::exists(disk / name)) TestRoundTrip(disk / name);
  }
  TestMods(disk / "bb3.cpk", scratch);
  TestPcOnlyData(scratch);
  TestDatabase(scratch);
  if (!failures) fs::remove_all(scratch);
  std::printf(failures ? "%d FAILURES\n" : "all passed\n", failures);
  return failures ? 1 : 0;
}
