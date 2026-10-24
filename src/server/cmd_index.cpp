// Secondary index commands.
//
//   IDX.CREATE index [ON HASH|JSON] [PREFIX count prefix ...] SCHEMA field TAG [SEPARATOR c]|NUMERIC ... [UNIQUE]
//     with ON JSON a field is a path, optionally named: $.address.city AS city TAG
//     A TAG with a separator has several values ("a,b"), like a JSON array
//     or a path that matches more than once: a key is found by any of them.
//   IDX.DROP index
//   IDX.LIST
//   IDX.INFO index
//   IDX.QUERY index [field op value ...] [DESC] [LIMIT offset count] [NOCONTENT | RETURN count field ...]
//   IDX.COUNT index [field op value ...]
//   IDX.GET index value [value ...]          (one value per index field)
//
// op is one of = > >= < <=. A query reads the slice of the index given by
// equality on its first fields and a range on the next one; other conditions
// filter the hashes (or documents) found. Results come in index order (DESC
// reverses it). A JSON document comes back as its "$" (the whole document).

#include <format>
#include <optional>
#include <set>

#include "server/command.h"
#include "util/strings.h"

namespace datadam {

namespace {

std::optional<IndexCondition::Op> parse_op(std::string_view s) {
  using Op = IndexCondition::Op;
  if (s == "=" || s == "==") return Op::Eq;
  if (s == ">") return Op::Gt;
  if (s == ">=") return Op::Ge;
  if (s == "<") return Op::Lt;
  if (s == "<=") return Op::Le;
  return std::nullopt;
}

void reply_hits(Cmd& c, const IndexQueryResult& res, bool no_content) {
  if (no_content) {
    c.r.array(res.hits.size());
    for (const auto& h : res.hits) c.r.bulk(h.key);
    return;
  }
  c.r.array(res.hits.size() * 2);
  for (const auto& h : res.hits) {
    c.r.bulk(h.key);
    c.r.array(h.fields.size() * 2);
    for (const auto& [f, v] : h.fields) {
      c.r.bulk(f);
      c.r.bulk(v);
    }
  }
}

void cmd_idx_create(Cmd& c) {
  IndexDef def;
  def.name = std::string(c.args[1]);
  size_t i = 2;
  if (i + 1 < c.args.size() && iequals(c.args[i], "on")) {
    if (iequals(c.args[i + 1], "json")) def.source = IndexSource::Json;
    else if (!iequals(c.args[i + 1], "hash")) return c.r.error("ERR ON takes HASH or JSON");
    i += 2;
  }
  if (i < c.args.size() && iequals(c.args[i], "prefix")) {
    auto n = i + 1 < c.args.size() ? parse_int64(c.args[i + 1]) : std::nullopt;
    if (!n || *n < 0 || i + 2 + static_cast<size_t>(*n) > c.args.size()) return c.r.error("ERR bad PREFIX count");
    for (int64_t k = 0; k < *n; ++k) def.prefixes.emplace_back(c.args[i + 2 + static_cast<size_t>(k)]);
    i += 2 + static_cast<size_t>(*n);
  }
  if (i >= c.args.size() || !iequals(c.args[i], "schema")) return c.r.error("ERR expected SCHEMA");
  ++i;
  std::set<std::string> seen;
  while (i < c.args.size() && !iequals(c.args[i], "unique")) {
    if (i + 1 >= c.args.size()) return c.r.error(kSyntax);
    IndexDef::Field f;
    f.name = std::string(c.args[i]);
    if (def.json()) {
      f.path = f.name;
      if (i + 3 < c.args.size() && iequals(c.args[i + 1], "as")) {
        f.name = std::string(c.args[i + 2]);
        i += 2;
      }
    }
    if (iequals(c.args[i + 1], "tag")) f.type = IndexFieldType::Tag;
    else if (iequals(c.args[i + 1], "numeric")) f.type = IndexFieldType::Numeric;
    else return c.r.error(std::format("ERR unknown field type '{}' (use TAG or NUMERIC)", c.args[i + 1]));
    i += 2;
    if (f.type == IndexFieldType::Tag && i + 1 < c.args.size() && iequals(c.args[i], "separator")) {
      if (c.args[i + 1].size() != 1) return c.r.error("ERR SEPARATOR takes a single character");
      f.separator = c.args[i + 1][0];
      i += 2;
    }
    if (!seen.insert(f.name).second) return c.r.error(std::format("ERR duplicate field '{}'", f.name));
    def.fields.push_back(std::move(f));
  }
  if (i < c.args.size()) {  // UNIQUE
    if (i + 1 != c.args.size()) return c.r.error(kSyntax);
    def.unique = true;
  }
  if (def.fields.empty()) return c.r.error("ERR SCHEMA needs at least one field");
  if (def.fields.size() > IndexDef::kMaxFields) {
    return c.r.error(std::format("ERR an index has at most {} fields", IndexDef::kMaxFields));
  }
  if (std::string error = def.compile(); !error.empty()) return c.r.error(error);
  std::string error = c.db().create_index(std::move(def));
  if (!error.empty()) return c.r.error(error);
  c.r.ok();
}

void cmd_idx_drop(Cmd& c) {
  if (!c.db().drop_index(c.args[1])) return c.r.error(std::format("ERR no such index '{}'", c.args[1]));
  c.r.ok();
}

void cmd_idx_list(Cmd& c) {
  auto all = c.db().indexes().all();
  c.r.array(all.size());
  for (const auto& index : all) c.r.bulk(index->def.name);
}

void cmd_idx_info(Cmd& c) {
  auto index = c.db().indexes().find(c.args[1]);
  if (!index) return c.r.error(std::format("ERR no such index '{}'", c.args[1]));
  const IndexDef& def = index->def;
  IndexState state = index->state.load();
  c.r.map(state == IndexState::Failed ? 9 : 8);
  c.r.bulk("name");
  c.r.bulk(def.name);
  c.r.bulk("kind");
  c.r.bulk(def.search() ? "search" : "compound");
  c.r.bulk("on");
  c.r.bulk(def.json() ? "JSON" : "HASH");
  c.r.bulk("state");
  c.r.bulk(index_state_name(state));
  c.r.bulk("prefixes");
  c.r.array(def.prefixes.size());
  for (const auto& p : def.prefixes) c.r.bulk(p);
  c.r.bulk("fields");
  c.r.array(def.fields.size());
  for (const auto& f : def.fields) {
    c.r.array(2 + (f.path.empty() ? 0 : 1) + (f.separator ? 2 : 0));
    c.r.bulk(f.name);
    c.r.bulk(index_field_type_name(f.type));
    if (!f.path.empty()) c.r.bulk(f.path);
    if (f.separator) {
      c.r.bulk("SEPARATOR");
      c.r.bulk(std::string(1, f.separator));
    }
  }
  c.r.bulk("unique");
  c.r.integer(def.unique ? 1 : 0);
  c.r.bulk("keys_scanned");
  c.r.integer(static_cast<int64_t>(index->scanned.load()));
  if (state == IndexState::Failed) {
    c.r.bulk("error");
    c.r.bulk(c.db().indexes().error_of(index));
  }
}

// Parses conditions and options from args[2..]; false on error (replied).
bool parse_query(Cmd& c, IndexQuery* q, bool allow_options) {
  q->index = std::string(c.args[1]);
  for (size_t i = 2; i < c.args.size();) {
    if (i + 1 < c.args.size() && parse_op(c.args[i + 1])) {
      if (i + 2 >= c.args.size()) {
        c.r.error(kSyntax);
        return false;
      }
      q->where.push_back({std::string(c.args[i]), *parse_op(c.args[i + 1]), std::string(c.args[i + 2])});
      i += 3;
      continue;
    }
    std::string opt = to_lower(c.args[i]);
    if (allow_options && opt == "desc") {
      q->desc = true;
      ++i;
    } else if (allow_options && opt == "nocontent") {
      q->no_content = true;
      ++i;
    } else if (allow_options && opt == "limit" && i + 2 < c.args.size()) {
      auto off = parse_int64(c.args[i + 1]), cnt = parse_int64(c.args[i + 2]);
      if (!off || !cnt || *off < 0) {
        c.r.error(kNotInteger);
        return false;
      }
      q->offset = *off;
      q->limit = *cnt;
      i += 3;
    } else if (allow_options && opt == "return" && i + 1 < c.args.size()) {
      auto n = parse_int64(c.args[i + 1]);
      if (!n || *n <= 0 || i + 2 + static_cast<size_t>(*n) > c.args.size()) {
        c.r.error("ERR bad RETURN count");
        return false;
      }
      for (int64_t k = 0; k < *n; ++k) q->return_fields.emplace_back(c.args[i + 2 + static_cast<size_t>(k)]);
      i += 2 + static_cast<size_t>(*n);
    } else {
      c.r.error(std::format("ERR syntax error near '{}' (conditions are: field op value, op one of = > >= < <=)",
                            c.args[i]));
      return false;
    }
  }
  if (q->no_content && !q->return_fields.empty()) {
    c.r.error("ERR NOCONTENT and RETURN cannot be used together");
    return false;
  }
  return true;
}

void cmd_idx_query(Cmd& c) {
  IndexQuery q;
  if (!parse_query(c, &q, true)) return;
  if (!reads_all_keys(c)) q.key_filter = [&c](std::string_view key) { return may_read_key(c, key); };
  IndexQueryResult res = c.db().query_index(q);
  if (!res.error.empty()) return c.r.error(res.error);
  reply_hits(c, res, q.no_content);
}

void cmd_idx_count(Cmd& c) {
  IndexQuery q;
  if (!parse_query(c, &q, false)) return;
  q.count_only = true;
  if (!reads_all_keys(c)) q.key_filter = [&c](std::string_view key) { return may_read_key(c, key); };
  IndexQueryResult res = c.db().query_index(q);
  if (!res.error.empty()) return c.r.error(res.error);
  c.r.integer(static_cast<int64_t>(res.count));
}

void cmd_idx_get(Cmd& c) {
  auto index = c.db().indexes().find(c.args[1]);
  if (!index) return c.r.error(std::format("ERR no such index '{}'", c.args[1]));
  const IndexDef& def = index->def;
  if (c.args.size() - 2 != def.fields.size()) {
    return c.r.error(std::format("ERR index '{}' has {} fields; give one value for each", def.name,
                                 def.fields.size()));
  }
  IndexQuery q;
  q.index = def.name;
  q.limit = def.unique ? 1 : -1;
  for (size_t i = 0; i < def.fields.size(); ++i) {
    q.where.push_back({def.fields[i].name, IndexCondition::Op::Eq, std::string(c.args[i + 2])});
  }
  if (!reads_all_keys(c)) q.key_filter = [&c](std::string_view key) { return may_read_key(c, key); };
  IndexQueryResult res = c.db().query_index(q);
  if (!res.error.empty()) return c.r.error(res.error);
  if (!def.unique) return reply_hits(c, res, false);
  if (res.hits.empty()) return c.r.null();
  reply_hits(c, res, false);
}

}  // namespace

CommandList index_commands() {
  return {
      {"idx.create", {cmd_idx_create, -5}},
      {"idx.drop", {cmd_idx_drop, 2}},
      {"idx.list", {cmd_idx_list, 1}},
      {"idx.info", {cmd_idx_info, 2}},
      {"idx.query", {cmd_idx_query, -2}},
      {"idx.count", {cmd_idx_count, -2}},
      {"idx.get", {cmd_idx_get, -3}},
  };
}

}  // namespace datadam
