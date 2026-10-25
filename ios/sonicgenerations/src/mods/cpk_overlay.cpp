// sonicgenerations - a CPK with files replaced or added

#include "cpk_overlay.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>
#include <numeric>
#include <unordered_map>

#include "crilayla.h"

namespace sg::mods {
namespace {

constexpr uint64_t kSector = 2048;

uint64_t AlignUp(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

uint64_t Le64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

bool ReadExact(const ReadFn& read, uint64_t offset, std::span<uint8_t> buffer) {
  size_t got = 0;
  return read(offset, buffer, &got) && got == buffer.size();
}

// A CPK section: 16-byte chunk header, then its @UTF table.
std::optional<cri::Table> ReadChunk(const ReadFn& read, uint64_t offset, const char magic[4],
                                    uint64_t file_size, std::string* error) {
  uint8_t head[cri::kChunkHeaderSize];
  if (offset > file_size || !ReadExact(read, offset, head) || std::memcmp(head, magic, 4) != 0) {
    *error = std::string("missing ") + std::string(magic, 4) + " section";
    return std::nullopt;
  }
  const uint64_t size = Le64(head + 8);
  if (size > file_size - offset || size > (64u << 20)) {
    *error = std::string("bad ") + std::string(magic, 4) + " size";
    return std::nullopt;
  }
  std::vector<uint8_t> table(size);
  if (!ReadExact(read, offset + cri::kChunkHeaderSize, table)) {
    *error = "read error";
    return std::nullopt;
  }
  auto parsed = cri::Table::Parse(table);
  if (!parsed) *error = std::string("unreadable ") + std::string(magic, 4) + " table";
  return parsed;
}

// The guest-visible file: sorted, non-overlapping segments; gaps read as zero.
class CpkOverlay final : public rex::filesystem::FileOverlay {
 public:
  enum class Kind { kMemory, kOriginal, kHost };
  struct Segment {
    uint64_t begin = 0, end = 0;
    Kind kind = Kind::kMemory;
    uint64_t source = 0;  // kOriginal: offset in the original; kMemory/kHost: index
  };

  CpkOverlay(ReadFn original, uint64_t size) : original_(std::move(original)), size_(size) {}
  ~CpkOverlay() override {
    for (int fd : fds_) {
      if (fd >= 0) ::close(fd);
    }
  }

  size_t AddMemory(std::vector<uint8_t> bytes) {
    memory_.push_back(std::move(bytes));
    return memory_.size() - 1;
  }
  size_t AddHost(std::filesystem::path path) {
    hosts_.push_back(std::move(path));
    fds_.push_back(-1);
    return hosts_.size() - 1;
  }
  void AddSegment(Segment segment) { segments_.push_back(segment); }
  void Finish(uint64_t size) {
    size_ = size;
    std::sort(segments_.begin(), segments_.end(),
              [](const Segment& a, const Segment& b) { return a.begin < b.begin; });
  }

  uint64_t size() const override { return size_; }

  bool Read(uint64_t offset, std::span<uint8_t> buffer, size_t* out_bytes_read) override {
    *out_bytes_read = 0;
    if (offset >= size_) return true;
    const uint64_t end = std::min<uint64_t>(size_, offset + buffer.size());
    auto it = std::upper_bound(segments_.begin(), segments_.end(), offset,
                               [](uint64_t v, const Segment& s) { return v < s.begin; });
    if (it != segments_.begin()) --it;
    uint64_t pos = offset;
    while (pos < end) {
      while (it != segments_.end() && it->end <= pos) ++it;
      uint8_t* out = buffer.data() + (pos - offset);
      if (it == segments_.end() || it->begin > pos) {
        const uint64_t gap_end = it == segments_.end() ? end : std::min(end, it->begin);
        std::memset(out, 0, gap_end - pos);
        pos = gap_end;
        continue;
      }
      const uint64_t length = std::min(end, it->end) - pos;
      const uint64_t within = pos - it->begin;
      if (!ReadSegment(*it, within, std::span<uint8_t>(out, length))) return false;
      pos += length;
    }
    *out_bytes_read = size_t(end - offset);
    return true;
  }

 private:
  bool ReadSegment(const Segment& s, uint64_t within, std::span<uint8_t> out) {
    switch (s.kind) {
      case Kind::kMemory: {
        const auto& bytes = memory_[s.source];
        std::memcpy(out.data(), bytes.data() + within, out.size());
        return true;
      }
      case Kind::kOriginal:
        return ReadExact(original_, s.source + within, out);
      case Kind::kHost: {
        const int fd = HostFd(s.source);
        if (fd < 0) return false;
        size_t done = 0;
        while (done < out.size()) {
          const ssize_t n = ::pread(fd, out.data() + done, out.size() - done, off_t(within + done));
          if (n <= 0) return false;
          done += size_t(n);
        }
        return true;
      }
    }
    return false;
  }

  int HostFd(size_t index) {
    std::lock_guard<std::mutex> lock(fd_mutex_);
    if (fds_[index] < 0) fds_[index] = ::open(hosts_[index].c_str(), O_RDONLY | O_CLOEXEC);
    return fds_[index];
  }

  ReadFn original_;
  uint64_t size_;
  std::vector<Segment> segments_;
  std::vector<std::vector<uint8_t>> memory_;
  std::vector<std::filesystem::path> hosts_;
  std::mutex fd_mutex_;
  std::vector<int> fds_;
};

}  // namespace

std::string UpperPath(std::string_view path) {
  std::string out(path);
  for (auto& c : out) {
    c = c == '\\' ? '/' : char(std::toupper(static_cast<unsigned char>(c)));
  }
  return out;
}

std::optional<CpkView> CpkView::Open(ReadFn read, uint64_t size, std::string* error) {
  CpkView cpk;
  cpk.read_ = std::move(read);
  cpk.size_ = size;
  auto header = ReadChunk(cpk.read_, 0, "CPK ", size, error);
  if (!header || header->rows.size() != 1) {
    if (header) *error = "unexpected CPK header";
    return std::nullopt;
  }
  cpk.header_ = std::move(*header);
  if (cpk.header_.GetNumber(0, "ItocOffset") || cpk.header_.GetNumber(0, "GtocOffset")) {
    *error = "CPKs with ID or group tables are not supported";
    return std::nullopt;
  }
  cpk.toc_offset_ = cpk.header_.GetNumber(0, "TocOffset");
  cpk.content_offset_ = cpk.header_.GetNumber(0, "ContentOffset");
  cpk.content_size_ = cpk.header_.GetNumber(0, "ContentSize");
  cpk.align_ = std::max<uint64_t>(1, cpk.header_.GetNumber(0, "Align"));
  // The layout this rebuilds: header sector, TOC, then content.
  if (cpk.toc_offset_ < cri::kChunkHeaderSize || cpk.toc_offset_ > kSector ||
      cpk.content_offset_ <= cpk.toc_offset_ || cpk.content_offset_ > size ||
      cpk.content_size_ > size - cpk.content_offset_) {
    *error = "unsupported CPK layout";
    return std::nullopt;
  }
  auto toc = ReadChunk(cpk.read_, cpk.toc_offset_, "TOC ", size, error);
  if (!toc) return std::nullopt;
  cpk.toc_ = std::move(*toc);
  for (const char* column : {"DirName", "FileName", "FileSize", "ExtractSize", "FileOffset", "ID"}) {
    const int i = cpk.toc_.ColumnIndex(column);
    if (i < 0 || !cpk.toc_.columns[size_t(i)].per_row()) {
      *error = std::string("TOC has no per-file ") + column;
      return std::nullopt;
    }
  }
  if (const uint64_t etoc = cpk.header_.GetNumber(0, "EtocOffset")) {
    auto table = ReadChunk(cpk.read_, etoc, "ETOC", size, error);
    if (!table) return std::nullopt;
    cpk.etoc_ = std::move(*table);
    cpk.has_etoc_ = true;
  }
  cpk.first_sector_.resize(cpk.toc_offset_);
  if (!ReadExact(cpk.read_, 0, cpk.first_sector_)) {
    *error = "read error";
    return std::nullopt;
  }
  return cpk;
}

std::string CpkView::PathOf(size_t row) const {
  const auto dir = toc_.GetString(row, "DirName");
  const auto file = toc_.GetString(row, "FileName");
  return dir.empty() || dir == "<NULL>" ? file : dir + "/" + file;
}

std::optional<size_t> CpkView::Find(std::string_view path) const {
  const auto want = UpperPath(path);
  for (size_t row = 0; row < toc_.rows.size(); ++row) {
    if (UpperPath(PathOf(row)) == want) return row;
  }
  return std::nullopt;
}

std::optional<std::vector<uint8_t>> CpkView::ReadFile(size_t row) const {
  const uint64_t stored = toc_.GetNumber(row, "FileSize");
  const uint64_t extracted = toc_.GetNumber(row, "ExtractSize");
  const uint64_t offset = toc_offset_ + toc_.GetNumber(row, "FileOffset");
  if (offset > size_ || stored > size_ - offset) return std::nullopt;
  std::vector<uint8_t> bytes(stored);
  if (!ReadExact(read_, offset, bytes)) return std::nullopt;
  if (stored == extracted) return bytes;
  auto out = cri::DecompressLayla(bytes);
  if (!out || out->size() != extracted) return std::nullopt;
  return out;
}

class CpkBuilder {
 public:
  static std::shared_ptr<rex::filesystem::FileOverlay> Build(
      const CpkView& cpk, const std::vector<Replacement>& replacements, std::string* error) {
    cri::Table toc = cpk.toc_;
    const int dir_column = toc.ColumnIndex("DirName");
    const int name_column = toc.ColumnIndex("FileName");

    std::unordered_map<std::string, size_t> rows;
    uint64_t next_id = 0;
    for (size_t row = 0; row < toc.rows.size(); ++row) {
      rows.emplace(UpperPath(cpk.PathOf(row)), row);
      next_id = std::max(next_id, toc.GetNumber(row, "ID") + 1);
    }

    // Each row's data: where it comes from in the new layout.
    struct Placement {
      bool from_mod = false;
      size_t replacement = 0;
    };
    std::vector<Placement> placement(toc.rows.size());
    int64_t packed_delta = 0, data_delta = 0;
    std::vector<std::pair<std::string, std::string>> added_dirs;  // ETOC LocalDir per new row
    for (size_t i = 0; i < replacements.size(); ++i) {
      const auto& r = replacements[i];
      if (r.size > 0xFFFFFFFFull) {
        *error = r.path + " is larger than 4 GB";
        return nullptr;
      }
      const auto key = UpperPath(r.path);
      auto found = rows.find(key);
      size_t row;
      if (found != rows.end()) {
        row = found->second;
        packed_delta -= int64_t(toc.GetNumber(row, "FileSize"));
        data_delta -= int64_t(toc.GetNumber(row, "ExtractSize"));
      } else {
        // A new file: a row like the others, under the mod's own spelling.
        row = toc.rows.size();
        toc.rows.push_back(toc.rows.empty() ? std::vector<cri::Value>(toc.columns.size())
                                            : toc.rows.front());
        const auto slash = r.path.rfind('/');
        toc.rows[row][size_t(dir_column)].string =
            slash == std::string::npos ? std::string() : r.path.substr(0, slash);
        toc.rows[row][size_t(name_column)].string =
            slash == std::string::npos ? r.path : r.path.substr(slash + 1);
        toc.SetNumber(row, "ID", next_id++);
        rows.emplace(key, row);
        placement.emplace_back();
        added_dirs.emplace_back(key, toc.rows[row][size_t(dir_column)].string);
      }
      toc.SetNumber(row, "FileSize", r.size);
      toc.SetNumber(row, "ExtractSize", r.size);
      packed_delta += int64_t(r.size);
      data_delta += int64_t(r.size);
      placement[row] = {true, i};
    }

    // Sort as CRI does (the file system binary-searches it), keeping
    // placements alongside.
    std::vector<size_t> order(toc.rows.size());
    std::iota(order.begin(), order.end(), size_t(0));
    std::vector<std::string> keys(toc.rows.size());
    for (size_t row = 0; row < toc.rows.size(); ++row) {
      const auto& dir = toc.rows[row][size_t(dir_column)].string;
      const auto& file = toc.rows[row][size_t(name_column)].string;
      keys[row] = UpperPath(dir.empty() || dir == "<NULL>" ? file : dir + "/" + file);
    }
    std::stable_sort(order.begin(), order.end(),
                     [&](size_t a, size_t b) { return keys[a] < keys[b]; });
    {
      std::vector<std::vector<cri::Value>> sorted_rows;
      std::vector<Placement> sorted_placement;
      for (size_t row : order) {
        sorted_rows.push_back(std::move(toc.rows[row]));
        sorted_placement.push_back(placement[row]);
      }
      toc.rows = std::move(sorted_rows);
      placement = std::move(sorted_placement);
    }

    // The TOC's size does not depend on the offsets in it (fixed-width rows).
    const uint64_t toc_chunk = cri::kChunkHeaderSize + toc.Serialize().size();
    const uint64_t toc_end = cpk.toc_offset_ + toc_chunk;
    const uint64_t shift =
        toc_end > cpk.content_offset_ ? AlignUp(toc_end - cpk.content_offset_, kSector) : 0;
    const uint64_t content = cpk.content_offset_ + shift;

    auto overlay = std::make_shared<CpkOverlay>(cpk.read_, 0);
    overlay->AddSegment({content, content + cpk.content_size_, CpkOverlay::Kind::kOriginal,
                         cpk.content_offset_});
    uint64_t cursor = content + cpk.content_size_;
    std::vector<size_t> host_of(replacements.size(), SIZE_MAX);
    for (size_t row = 0; row < toc.rows.size(); ++row) {
      if (!placement[row].from_mod) {
        toc.SetNumber(row, "FileOffset", toc.GetNumber(row, "FileOffset") + shift);
        continue;
      }
      const auto& r = replacements[placement[row].replacement];
      cursor = AlignUp(cursor, cpk.align_);
      toc.SetNumber(row, "FileOffset", cursor - cpk.toc_offset_);
      if (r.size) {
        overlay->AddSegment({cursor, cursor + r.size, CpkOverlay::Kind::kHost,
                             overlay->AddHost(r.host)});
      }
      cursor += r.size;
    }

    auto toc_bytes = cri::MakeChunk("TOC ", toc.Serialize());
    toc_bytes.resize(content - cpk.toc_offset_, 0);
    overlay->AddSegment({cpk.toc_offset_, content, CpkOverlay::Kind::kMemory,
                         overlay->AddMemory(std::move(toc_bytes))});

    // ETOC: one row per file plus a terminator. New files get a copy of the
    // first row's stamp and their own directory, before the terminator.
    uint64_t etoc_offset = 0, etoc_size = 0;
    if (cpk.has_etoc_) {
      cri::Table etoc = cpk.etoc_;
      const int local_dir = etoc.ColumnIndex("LocalDir");
      for (const auto& [key, dir] : added_dirs) {
        auto row = etoc.rows.empty() ? std::vector<cri::Value>(etoc.columns.size())
                                     : etoc.rows.front();
        if (local_dir >= 0) row[size_t(local_dir)].string = dir;
        etoc.rows.insert(etoc.rows.empty() ? etoc.rows.end() : etoc.rows.end() - 1, row);
      }
      cursor = AlignUp(cursor, cpk.align_);
      auto etoc_bytes = cri::MakeChunk("ETOC", etoc.Serialize());
      etoc_offset = cursor;
      etoc_size = etoc_bytes.size();
      cursor += etoc_size;
      overlay->AddSegment({etoc_offset, cursor, CpkOverlay::Kind::kMemory,
                           overlay->AddMemory(std::move(etoc_bytes))});
    }

    cri::Table header = cpk.header_;
    const bool ok =
        header.SetNumber(0, "ContentOffset", content) &&
        header.SetNumber(0, "ContentSize", (etoc_offset ? etoc_offset : cursor) - content) &&
        header.SetNumber(0, "TocSize", toc_chunk) &&
        header.SetNumber(0, "Files", toc.rows.size()) &&
        (!cpk.has_etoc_ || (header.SetNumber(0, "EtocOffset", etoc_offset) &&
                            header.SetNumber(0, "EtocSize", etoc_size)));
    if (!ok) {
      *error = "CPK header fields are not stored per archive";
      return nullptr;
    }
    // Totals CRI keeps for its own statistics; adjusted where present.
    header.SetNumber(0, "EnabledPackedSize",
                     uint64_t(int64_t(header.GetNumber(0, "EnabledPackedSize")) + packed_delta));
    header.SetNumber(0, "EnabledDataSize",
                     uint64_t(int64_t(header.GetNumber(0, "EnabledDataSize")) + data_delta));
    auto header_chunk = cri::MakeChunk("CPK ", header.Serialize());
    auto sector = cpk.first_sector_;
    if (header_chunk.size() > sector.size()) {
      *error = "CPK header grew";
      return nullptr;
    }
    std::copy(header_chunk.begin(), header_chunk.end(), sector.begin());
    overlay->AddSegment({0, sector.size(), CpkOverlay::Kind::kMemory,
                         overlay->AddMemory(std::move(sector))});

    overlay->Finish(cursor);
    return overlay;
  }
};

std::shared_ptr<rex::filesystem::FileOverlay> BuildCpkOverlay(
    const CpkView& cpk, const std::vector<Replacement>& replacements, std::string* error) {
  return CpkBuilder::Build(cpk, replacements, error);
}

}  // namespace sg::mods
