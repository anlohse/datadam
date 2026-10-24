#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace datadam {

struct IndexCondition {
  enum class Op { Eq, Gt, Ge, Lt, Le };
  std::string field;
  Op op = Op::Eq;
  std::string value;
};

// IDX.QUERY / IDX.COUNT / IDX.GET. Equality conditions on the first index
// fields plus range conditions on the next one select a contiguous slice of
// the index; every other condition filters the hashes found there.
struct IndexQuery {
  std::string index;
  std::vector<IndexCondition> where;
  int64_t offset = 0;
  int64_t limit = 10;  // < 0: no limit
  bool desc = false;
  bool count_only = false;
  bool no_content = false;
  std::vector<std::string> return_fields;  // empty: all fields
  // If set, only hashes whose key passes it count (ACL key permissions).
  std::function<bool(std::string_view key)> key_filter;
};

struct IndexHit {
  std::string key;
  std::vector<std::pair<std::string, std::string>> fields;
};

struct IndexQueryResult {
  std::string error;  // non-empty: the query was rejected
  std::vector<IndexHit> hits;
  uint64_t count = 0;  // count_only
};

}  // namespace datadam
