// sonicgenerations - CRI @UTF tables

#include "cri_utf.h"

#include <cstring>
#include <unordered_map>

namespace sg::cri {
namespace {

constexpr size_t kHeaderSize = 32;  // "@UTF" through the row count
constexpr size_t kBase = 8;         // offsets count from after "@UTF" + size

size_t TypeSize(Type type) {
  switch (type) {
    case Type::kU8: case Type::kS8: return 1;
    case Type::kU16: case Type::kS16: return 2;
    case Type::kU32: case Type::kS32: case Type::kFloat: case Type::kString: return 4;
    case Type::kU64: case Type::kS64: case Type::kDouble: case Type::kData: return 8;
  }
  return 0;
}

uint64_t LoadBE(const uint8_t* p, size_t n) {
  uint64_t v = 0;
  for (size_t i = 0; i < n; ++i) v = (v << 8) | p[i];
  return v;
}

void StoreBE(std::vector<uint8_t>& out, uint64_t v, size_t n) {
  for (size_t i = 0; i < n; ++i) out.push_back(uint8_t(v >> (8 * (n - 1 - i))));
}

void PatchBE(std::vector<uint8_t>& out, size_t at, uint64_t v, size_t n) {
  for (size_t i = 0; i < n; ++i) out[at + i] = uint8_t(v >> (8 * (n - 1 - i)));
}

// CRI masks some tables with a multiplicative XOR stream.
std::vector<uint8_t> Unmask(std::span<const uint8_t> bytes) {
  std::vector<uint8_t> out(bytes.begin(), bytes.end());
  uint32_t m = 0x655F;
  for (auto& b : out) {
    b ^= uint8_t(m);
    m *= 0x4115;
  }
  return out;
}

struct Reader {
  std::span<const uint8_t> bytes;
  size_t strings = 0, strings_end = 0, data = 0, data_end = 0;
  bool ok = true;

  const uint8_t* At(size_t offset, size_t length) {
    if (offset > bytes.size() || bytes.size() - offset < length) {
      ok = false;
      return nullptr;
    }
    return bytes.data() + offset;
  }
  std::string String(uint32_t offset) {
    const size_t start = strings + offset;
    if (start >= strings_end) {
      ok = false;
      return {};
    }
    const auto* p = bytes.data() + start;
    const auto* end = static_cast<const uint8_t*>(std::memchr(p, 0, strings_end - start));
    if (!end) {
      ok = false;
      return {};
    }
    return std::string(reinterpret_cast<const char*>(p), end - p);
  }
  bool ReadValue(size_t offset, Type type, Value& value) {
    const auto* p = At(offset, TypeSize(type));
    if (!p) return false;
    if (type == Type::kString) {
      value.string = String(uint32_t(LoadBE(p, 4)));
    } else if (type == Type::kData) {
      const uint32_t at = uint32_t(LoadBE(p, 4)), size = uint32_t(LoadBE(p + 4, 4));
      if (data + at > data_end || data_end - (data + at) < size) {
        ok = false;
        return false;
      }
      value.data.assign(bytes.begin() + data + at, bytes.begin() + data + at + size);
    } else {
      value.number = LoadBE(p, TypeSize(type));
    }
    return ok;
  }
};

}  // namespace

std::optional<Table> Table::Parse(std::span<const uint8_t> bytes) {
  std::vector<uint8_t> unmasked;
  if (bytes.size() < kHeaderSize) return std::nullopt;
  if (std::memcmp(bytes.data(), "@UTF", 4) != 0) {
    unmasked = Unmask(bytes);
    if (std::memcmp(unmasked.data(), "@UTF", 4) != 0) return std::nullopt;
    bytes = unmasked;
  }
  const auto* h = bytes.data();
  const uint64_t table_size = LoadBE(h + 4, 4);
  if (table_size + kBase > bytes.size()) return std::nullopt;
  bytes = bytes.first(size_t(table_size + kBase));

  Table table;
  table.version = uint16_t(LoadBE(h + 8, 2));
  const size_t rows_at = kBase + LoadBE(h + 10, 2);
  Reader r{bytes};
  r.strings = kBase + LoadBE(h + 12, 4);
  r.data = kBase + LoadBE(h + 16, 4);
  r.strings_end = r.data;
  r.data_end = bytes.size();
  const uint32_t name_at = uint32_t(LoadBE(h + 20, 4));
  const size_t column_count = LoadBE(h + 24, 2);
  const size_t row_length = LoadBE(h + 26, 2);
  const size_t row_count = LoadBE(h + 28, 4);
  if (r.strings > r.data || r.data > bytes.size() || rows_at > r.strings) return std::nullopt;
  table.name = r.String(name_at);

  size_t p = kHeaderSize, per_row_length = 0;
  for (size_t i = 0; i < column_count && r.ok; ++i) {
    const auto* c = r.At(p, 5);
    if (!c) break;
    Column column;
    column.storage = Storage(c[0] & 0xF0);
    column.type = Type(c[0] & 0x0F);
    column.name = r.String(uint32_t(LoadBE(c + 1, 4)));
    p += 5;
    if (TypeSize(column.type) == 0) return std::nullopt;
    switch (column.storage) {
      case Storage::kZero: break;
      case Storage::kPerRow: per_row_length += TypeSize(column.type); break;
      case Storage::kConstant:
      case Storage::kConstant2:
        r.ReadValue(p, column.type, column.constant);
        p += TypeSize(column.type);
        break;
      default: return std::nullopt;
    }
    table.columns.push_back(std::move(column));
  }
  if (!r.ok || per_row_length != row_length || p > rows_at ||
      row_count > (r.strings - rows_at) / std::max<size_t>(row_length, 1)) {
    return std::nullopt;
  }
  table.rows.resize(row_count);
  for (size_t row = 0; row < row_count && r.ok; ++row) {
    size_t q = rows_at + row * row_length;
    auto& values = table.rows[row];
    values.resize(table.columns.size());
    for (size_t i = 0; i < table.columns.size(); ++i) {
      const auto& column = table.columns[i];
      if (column.per_row()) {
        r.ReadValue(q, column.type, values[i]);
        q += TypeSize(column.type);
      } else if (column.has_constant()) {
        values[i] = column.constant;
      }
    }
  }
  if (!r.ok) return std::nullopt;
  return table;
}

std::vector<uint8_t> Table::Serialize() const {
  // CRI's layout: header, schema (with constants), rows, the string pool
  // ("<NULL>", table name, column names, then values in first-use order),
  // padding to 8, the data pool, padding to 8.
  std::vector<uint8_t> strings;
  std::unordered_map<std::string, uint32_t> string_at;
  auto intern = [&](const std::string& s) -> uint32_t {
    auto [it, inserted] = string_at.try_emplace(s, uint32_t(strings.size()));
    if (inserted) {
      strings.insert(strings.end(), s.begin(), s.end());
      strings.push_back(0);
    }
    return it->second;
  };
  std::vector<uint8_t> data;
  auto put_value = [&](std::vector<uint8_t>& out, Type type, const Value& value) {
    if (type == Type::kString) {
      StoreBE(out, intern(value.string), 4);
    } else if (type == Type::kData) {
      StoreBE(out, data.size(), 4);
      StoreBE(out, value.data.size(), 4);
      data.insert(data.end(), value.data.begin(), value.data.end());
    } else {
      StoreBE(out, value.number, TypeSize(type));
    }
  };

  intern("<NULL>");
  const uint32_t name_at = intern(name);
  std::vector<uint8_t> schema;
  size_t row_length = 0;
  for (const auto& column : columns) {
    schema.push_back(uint8_t(uint8_t(column.storage) | uint8_t(column.type)));
    StoreBE(schema, intern(column.name), 4);
    if (column.has_constant()) put_value(schema, column.type, column.constant);
    if (column.per_row()) row_length += TypeSize(column.type);
  }
  std::vector<uint8_t> row_bytes;
  row_bytes.reserve(row_length * rows.size());
  for (const auto& values : rows) {
    for (size_t i = 0; i < columns.size(); ++i) {
      if (columns[i].per_row()) put_value(row_bytes, columns[i].type, values[i]);
    }
  }

  std::vector<uint8_t> out;
  out.reserve(kHeaderSize + schema.size() + row_bytes.size() + strings.size() + data.size() + 16);
  out.insert(out.end(), {'@', 'U', 'T', 'F'});
  out.resize(kHeaderSize);
  out.insert(out.end(), schema.begin(), schema.end());
  const size_t rows_at = out.size();
  out.insert(out.end(), row_bytes.begin(), row_bytes.end());
  const size_t strings_at = out.size();
  out.insert(out.end(), strings.begin(), strings.end());
  out.resize((out.size() + 7) & ~size_t(7), 0);
  const size_t data_at = out.size();
  out.insert(out.end(), data.begin(), data.end());
  out.resize((out.size() + 7) & ~size_t(7), 0);

  PatchBE(out, 4, out.size() - kBase, 4);
  PatchBE(out, 8, version, 2);
  PatchBE(out, 10, rows_at - kBase, 2);
  PatchBE(out, 12, strings_at - kBase, 4);
  PatchBE(out, 16, data_at - kBase, 4);
  PatchBE(out, 20, name_at, 4);
  PatchBE(out, 24, columns.size(), 2);
  PatchBE(out, 26, row_length, 2);
  PatchBE(out, 28, rows.size(), 4);
  return out;
}

int Table::ColumnIndex(std::string_view column) const {
  for (size_t i = 0; i < columns.size(); ++i) {
    if (columns[i].name == column) return int(i);
  }
  return -1;
}

const Value& Table::Get(size_t row, int column) const {
  static const Value kEmpty;
  if (column < 0 || row >= rows.size()) return kEmpty;
  return rows[row][size_t(column)];
}

uint64_t Table::GetNumber(size_t row, std::string_view column) const {
  return Get(row, ColumnIndex(column)).number;
}

std::string Table::GetString(size_t row, std::string_view column) const {
  return Get(row, ColumnIndex(column)).string;
}

bool Table::SetNumber(size_t row, std::string_view column, uint64_t value) {
  const int i = ColumnIndex(column);
  if (i < 0 || row >= rows.size() || !columns[size_t(i)].per_row()) return false;
  rows[row][size_t(i)].number = value;
  return true;
}

bool Table::SetString(size_t row, std::string_view column, std::string value) {
  const int i = ColumnIndex(column);
  if (i < 0 || row >= rows.size() || !columns[size_t(i)].per_row()) return false;
  rows[row][size_t(i)].string = std::move(value);
  return true;
}

std::vector<uint8_t> MakeChunk(const char magic[4], std::span<const uint8_t> table) {
  std::vector<uint8_t> out(kChunkHeaderSize, 0);
  std::memcpy(out.data(), magic, 4);
  out[4] = 0xFF;
  const uint64_t size = table.size();
  for (int i = 0; i < 8; ++i) out[8 + i] = uint8_t(size >> (8 * i));
  out.insert(out.end(), table.begin(), table.end());
  return out;
}

}  // namespace sg::cri
