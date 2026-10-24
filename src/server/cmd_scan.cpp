// SCAN, HSCAN, SSCAN and ZSCAN.
//
// Keys and elements are stored in order, so an iteration simply resumes after
// the last key (or element) it returned: everything present for the whole
// iteration is returned exactly once, and nothing is ever returned twice. The
// numeric cursors clients expect map to those positions (ScanCursors).

#include <format>
#include <optional>

#include "server/command.h"
#include "storage/data_keys.h"
#include "util/strings.h"

namespace datadam {

namespace {

using Txn = Database::KeyTxn;

struct ScanOptions {
  std::optional<std::string_view> match;
  size_t count = 10;
  std::optional<std::string> type;  // SCAN only
  bool no_values = false;           // HSCAN only
};

// Parses [MATCH pattern] [COUNT count] (+ TYPE / NOVALUES where allowed) from args[i..].
bool parse_options(Cmd& c, size_t i, bool allow_type, bool allow_novalues, ScanOptions* o) {
  for (; i < c.args.size(); ++i) {
    std::string opt = to_lower(c.args[i]);
    if (opt == "match" && i + 1 < c.args.size()) {
      o->match = c.args[++i];
    } else if (opt == "count" && i + 1 < c.args.size()) {
      auto n = parse_int64(c.args[++i]);
      if (!n) {
        c.r.error(kNotInteger);
        return false;
      }
      if (*n < 1) {
        c.r.error(kSyntax);
        return false;
      }
      o->count = static_cast<size_t>(std::min<int64_t>(*n, 1'000'000));
    } else if (allow_type && opt == "type" && i + 1 < c.args.size()) {
      o->type = to_lower(c.args[++i]);
    } else if (allow_novalues && opt == "novalues") {
      o->no_values = true;
    } else {
      c.r.error(kSyntax);
      return false;
    }
  }
  return true;
}

// Resolves a cursor argument to the position to resume after ("" = start);
// replies with an error and returns nullopt if it is not valid.
std::optional<std::string> resume_position(Cmd& c, std::string_view cursor, std::string_view scope) {
  auto id = parse_uint64(cursor);
  if (!id) {
    c.r.error("ERR invalid cursor");
    return std::nullopt;
  }
  if (*id == 0) return std::string();
  auto pos = c.ctx.cursors.load(*id, scope);
  if (!pos) c.r.error("ERR invalid cursor (unknown or expired; start again from 0)");
  return pos;
}

void reply_cursor(Cmd& c, std::string_view scope, std::optional<std::string> next) {
  c.r.array(2);
  c.r.bulk(next ? std::to_string(c.ctx.cursors.save(std::string(scope), std::move(*next))) : "0");
}

// The literal start of a glob pattern: keys outside it can be skipped.
std::string_view literal_prefix(std::string_view pattern) {
  size_t n = pattern.find_first_of("*?[\\");
  return n == std::string_view::npos ? pattern : pattern.substr(0, n);
}

// SCAN cursor [MATCH pattern] [COUNT count] [TYPE type]
void cmd_scan(Cmd& c) {
  ScanOptions o;
  if (!parse_options(c, 2, true, false, &o)) return;
  auto after = resume_position(c, c.args[1], "keys");
  if (!after) return;
  std::string_view prefix = o.match ? literal_prefix(*o.match) : std::string_view();
  bool filter_acl = !reads_all_keys(c);
  auto result = c.db().scan_keys(*after, prefix, o.count, [&](std::string_view key, const RecordView& rec) {
    if (o.type && to_lower(value_type_name(rec.type)) != *o.type) return false;  // TYPE ignores case (ReJSON-RL)
    if (filter_acl && !may_read_key(c, key)) return false;  // only keys this user may read
    return !o.match || glob_match(*o.match, key);
  });
  reply_cursor(c, "keys", std::move(result.next));
  c.r.array(result.keys.size());
  for (const auto& k : result.keys) c.r.bulk(k);
}

// HSCAN / SSCAN / ZSCAN key cursor [MATCH pattern] [COUNT count] [NOVALUES]
// Elements are walked by sub-key; `space` is the sub-key prefix of what is
// scanned ("m" for zset members), `space_end` its end ("" = no end).
void scan_collection(Cmd& c, ValueType type, std::string_view name, std::string_view space,
                     std::string_view space_end) {
  ScanOptions o;
  if (!parse_options(c, 3, false, type == ValueType::Hash, &o)) return;
  std::string scope = std::format("{}:{}", name, c.args[1]);
  auto after = resume_position(c, c.args[2], scope);
  if (!after) return;

  std::vector<std::pair<std::string, std::string>> found;
  std::optional<std::string> next;
  bool ok = read_collection(c, c.args[1], type, true, [&](Txn& t) {
    std::string from(space);
    if (!after->empty()) {
      from.append(*after);
      from.push_back('\0');  // just after the last element returned
    }
    size_t seen = 0;
    t.scan(from, space_end, false, [&](std::string_view sub, std::string_view value) {
      if (seen == o.count) return false;  // `next` holds the last element looked at
      ++seen;
      std::string_view element = sub.substr(space.size());
      next = std::string(element);  // the last element looked at
      if (!o.match || glob_match(*o.match, element)) found.emplace_back(element, value);
      return true;
    });
    if (seen < o.count) next.reset();  // walked to the end
  });
  if (!ok) return;
  reply_cursor(c, scope, std::move(next));
  bool with_values = type != ValueType::Set && !o.no_values;
  c.r.array(found.size() * (with_values ? 2 : 1));
  for (const auto& [element, value] : found) {
    c.r.bulk(element);
    if (!with_values) continue;
    if (type == ValueType::ZSet) c.r.bulk(format_double(score_from_sortable(read_be64(value))));
    else c.r.bulk(value);
  }
}

void cmd_hscan(Cmd& c) { scan_collection(c, ValueType::Hash, "hscan", "", ""); }
void cmd_sscan(Cmd& c) { scan_collection(c, ValueType::Set, "sscan", "", ""); }
void cmd_zscan(Cmd& c) { scan_collection(c, ValueType::ZSet, "zscan", "m", "n"); }

}  // namespace

CommandList scan_commands() {
  return {
      {"scan", {cmd_scan, -2}},
      {"hscan", {cmd_hscan, -3}},
      {"sscan", {cmd_sscan, -3}},
      {"zscan", {cmd_zscan, -3}},
  };
}

}  // namespace datadam
