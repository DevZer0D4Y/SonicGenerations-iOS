// sonicgenerations - CRI @UTF tables (the CPK header, TOC and ETOC)
//
// A @UTF table is a big-endian column store: a schema of typed columns, each
// either constant, zero or stored per row, a fixed-width row array, a string
// pool and a data pool. Tables are parsed into values and written back with
// the layout CRI's own packer uses, so an unmodified table round-trips to the
// same bytes (tests/mod_loader_tests.cpp checks this on the game's archives).

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sg::cri {

enum class Storage : uint8_t { kZero = 0x10, kConstant = 0x30, kPerRow = 0x50, kConstant2 = 0x70 };

enum class Type : uint8_t {
  kU8 = 0, kS8 = 1, kU16 = 2, kS16 = 3, kU32 = 4, kS32 = 5, kU64 = 6, kS64 = 7,
  kFloat = 8, kDouble = 9, kString = 0xA, kData = 0xB,
};

struct Value {
  uint64_t number = 0;  // integers and the raw bits of floats
  std::string string;
  std::vector<uint8_t> data;
};

struct Column {
  std::string name;
  Storage storage = Storage::kPerRow;
  Type type = Type::kU32;
  Value constant;  // kConstant / kConstant2

  bool per_row() const { return storage == Storage::kPerRow; }
  bool has_constant() const {
    return storage == Storage::kConstant || storage == Storage::kConstant2;
  }
};

class Table {
 public:
  // Parses a table starting with "@UTF". Encrypted tables (CRI's XOR mask) are
  // decoded first. Returns nullopt for anything malformed.
  static std::optional<Table> Parse(std::span<const uint8_t> bytes);

  std::vector<uint8_t> Serialize() const;

  int ColumnIndex(std::string_view name) const;
  // The value of a column in a row: the row's own value, the constant, or 0.
  const Value& Get(size_t row, int column) const;
  uint64_t GetNumber(size_t row, std::string_view column) const;
  std::string GetString(size_t row, std::string_view column) const;
  // Sets a per-row column. Returns false when the column is not stored per row.
  bool SetNumber(size_t row, std::string_view column, uint64_t value);
  bool SetString(size_t row, std::string_view column, std::string value);

  std::string name;
  uint16_t version = 0;
  std::vector<Column> columns;
  std::vector<std::vector<Value>> rows;  // one Value per column
};

// A CPK section: magic ("CPK ", "TOC ", "ETOC"), 0xFF, a little-endian size,
// then a @UTF table.
std::vector<uint8_t> MakeChunk(const char magic[4], std::span<const uint8_t> table);
constexpr size_t kChunkHeaderSize = 16;

}  // namespace sg::cri
