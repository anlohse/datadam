#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "json/json_path.h"

namespace datadam {

// A secondary index over hash fields, or over values in JSON documents. An
// entry in the index column family is
//   u32 index id | tuple | owner key            -> version | expire_at
//   u32 index id | tuple            (unique)    -> version | expire_at | owner key
// version is the owner hash's collection version and expire_at its TTL, so an
// entry can be trusted without reading the hash: every change of the fields
// within one version rewrites the entry, and a new version means a new hash.
// A JSON document has no version (it is 0): every write of the document
// rewrites its entries, and a query that reads it checks them against it.
//
// A key has a set of tuples in an index (one, several or none):
//  - compound (IDX.CREATE): the encoded value of each field, in order. A field
//    with several values (a TAG split by its separator, a JSON array, a path
//    that matches more than once) gives one tuple per combination. Encodings
//    preserve order, so equality on the first fields plus a range on the next
//    one is a single contiguous scan (like a B-tree compound index).
//  - search (FT.CREATE): u8 field position | encoded value, one per value of
//    each field, plus kPresence alone, so that every document of the index is
//    listed even if it has none of the fields. Fields are independent: a
//    query combines scans of several fields.
enum class IndexFieldType : uint8_t { Tag = 0, Numeric = 1 };
enum class IndexState : uint8_t { Building = 0, Ready = 1, Failed = 2 };
enum class IndexSource : uint8_t { Hash = 0, Json = 1 };
enum class IndexKind : uint8_t { Compound = 0, Search = 1 };

std::string_view index_field_type_name(IndexFieldType t);
std::string_view index_state_name(IndexState s);

// A write would give two keys the same value in a UNIQUE index.
class IndexConstraintError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

struct IndexDef {
  struct Field {
    std::string name;  // the name queries use: a hash field, or its alias (AS)
    IndexFieldType type = IndexFieldType::Tag;
    // JSON: where the value is (JSONPath or legacy path). Hash: the field,
    // when the name is an alias (empty: the name).
    std::string path;
    char separator = 0;          // TAG: splits a value into several (0: none)
    bool case_sensitive = true;  // TAG: false folds ASCII letters to lower case
  };
  using Tuples = std::vector<std::string>;  // sorted, without duplicates; empty: not indexed

  static constexpr size_t kMaxFields = 8;          // compound
  static constexpr size_t kMaxSearchFields = 128;  // search
  // Per key and index. More combinations: a compound index leaves the key out;
  // a search index keeps the first ones (the presence entry among them).
  static constexpr size_t kMaxTuples = 1024;
  static constexpr char kPresence = '\xff';

  uint32_t id = 0;
  std::string name;
  IndexSource source = IndexSource::Hash;
  IndexKind kind = IndexKind::Compound;
  std::vector<std::string> prefixes;  // empty: every key
  std::vector<Field> fields;
  bool unique = false;
  IndexState state = IndexState::Building;
  std::string error;  // why the index failed

  bool json() const { return source == IndexSource::Json; }
  bool search() const { return kind == IndexKind::Search; }
  // A key may have several tuples (scans must skip repeated owners).
  bool multi_valued() const;
  std::string_view hash_field(size_t field) const {
    return fields[field].path.empty() ? std::string_view(fields[field].name) : std::string_view(fields[field].path);
  }
  bool matches(std::string_view key) const;
  // Parses the JSON paths of the fields (decode does it too); an error message or "".
  std::string compile();
  // Position of a field in `fields`, or -1.
  int field_index(std::string_view field) const;

  std::string encode() const;
  static bool decode(std::string_view raw, IndexDef* out);

  // A TAG value as stored and queried (case folded unless case_sensitive).
  std::string normalize(size_t field, std::string_view value) const;
  // The values of a field given its raw text (a hash field): a TAG split by
  // its separator, trimmed and normalized, empty pieces dropped.
  void split_values(size_t field, std::string_view raw, std::vector<std::string>* out) const;
  // JSON: the values of a field in a document: every match of its path, an
  // array's elements one by one; a string as it is, a number or boolean as
  // JSON text, anything else skipped. Then like split_values.
  std::vector<std::string> json_values(size_t field, const Json& doc) const;

  // The tuples of a hash, its fields read through value_of(hash field), and
  // of a document. A compound index needs every field (one missing or not
  // valid for its type: the key is not indexed); a search index takes what
  // there is.
  Tuples tuples(const std::function<std::optional<std::string>(std::string_view)>& value_of) const;
  Tuples json_tuples(const Json& doc) const;
  // From the values of each field (in field order).
  Tuples make_tuples(const std::vector<std::vector<std::string>>& values) const;

  std::string id_prefix() const;
  // Search: the first bytes of every entry of a field, and of the presence entries.
  std::string field_prefix(size_t field) const;
  std::string presence_prefix() const;
  std::string entry_key(std::string_view tuple, std::string_view owner) const;
  std::string entry_value(uint64_t version, int64_t expire_at_ms, std::string_view owner) const;

  struct Entry {
    std::string_view tuple, owner;
    int field = -1;  // search: the field of the entry (-1: presence)
    uint64_t version = 0;
    int64_t expire_at_ms = 0;
    bool expired(int64_t now_ms) const { return expire_at_ms != 0 && now_ms >= expire_at_ms; }
  };
  // Splits an entry; false if malformed.
  bool split_entry(std::string_view key, std::string_view value, Entry* out) const;

 private:
  std::vector<JsonPath> paths_;  // JSON: the compiled Field::path of each field
};

bool contains_tuple(const IndexDef::Tuples& tuples, std::string_view tuple);

// Order-preserving, self-delimiting encoding of one field value; false if the
// value is not valid for the type (a NUMERIC field that is not a number).
bool encode_index_value(IndexFieldType type, std::string_view value, std::string* out);
// The bytes a TAG value starts with, without its terminator: every value
// with this prefix is encoded as this followed by more.
std::string tag_prefix(std::string_view value);
// Decodes the TAG value at the start of s; false if malformed.
bool decode_tag(std::string_view s, std::string* out);
// The bytes of a TAG value followed by its terminator, for range bounds:
// tag_bound(v, false) sorts before every encoding that starts with v's
// encoding, tag_bound(v, true) after all of them.
std::string tag_bound(std::string_view value, bool after);
// Smallest string greater than every string with this prefix ("" = no bound).
std::string prefix_end(std::string_view prefix);
// u32 big endian, the first 4 bytes of an entry.
std::string encode_index_id(uint32_t id);
uint32_t decode_index_id(std::string_view key);

}  // namespace datadam
