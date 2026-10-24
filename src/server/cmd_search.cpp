// RediSearch-compatible commands over search indexes (TAG and NUMERIC fields).
//
//   FT.CREATE index [ON HASH|JSON] [PREFIX count prefix ...] [SKIPINITIALSCAN] SCHEMA
//       field [AS alias] TAG [SEPARATOR c] [CASESENSITIVE] | NUMERIC   [SORTABLE] ...
//   FT.SEARCH index query [NOCONTENT] [WITHSCORES] [FILTER field min max] [RETURN count field [AS name] ...]
//       [SORTBY field [ASC|DESC]] [LIMIT offset count] [PARAMS count name value ...] [DIALECT n]
//   FT.DROPINDEX index [DD]
//   FT.INFO index
//   FT._LIST
//   FT.TAGVALS index field
//
// Unlike IDX.* indexes, every field is indexed on its own (a document missing
// a field is still in the index), and a query combines any of them. A TAG on
// a hash is split by its separator (',' by default), a JSON array gives one
// value per element, and TAG values are case insensitive unless
// CASESENSITIVE. Options about full-text search are accepted and ignored;
// TEXT, GEO and VECTOR fields are not supported.

#include <format>
#include <set>

#include "server/command.h"
#include "util/strings.h"

namespace datadam {

namespace {

// The search index by that name, or null (replied).
IndexRegistry::Ptr find_search_index(Cmd& c, std::string_view name) {
  auto index = c.db().indexes().find(name);
  if (!index || !index->def.search()) {
    c.r.error(std::format("ERR {}: no such index", name));
    return nullptr;
  }
  return index;
}

bool one_of(std::string_view s, std::initializer_list<std::string_view> words) {
  for (auto w : words) {
    if (s == w) return true;
  }
  return false;
}

void cmd_ft_create(Cmd& c) {
  IndexDef def;
  def.kind = IndexKind::Search;
  def.name = std::string(c.args[1]);
  bool scan = true;
  size_t n = c.args.size(), i = 2;
  // A count followed by that many arguments, at args[i + 1].
  auto counted = [&](size_t* count) {
    auto v = i + 1 < n ? parse_int64(c.args[i + 1]) : std::nullopt;
    if (!v || *v < 0 || i + 2 + static_cast<size_t>(*v) > n) return false;
    *count = static_cast<size_t>(*v);
    return true;
  };
  while (i < n && !iequals(c.args[i], "schema")) {
    std::string opt = to_lower(c.args[i]);
    size_t count = 0;
    if (opt == "on" && i + 1 < n) {
      if (iequals(c.args[i + 1], "json")) def.source = IndexSource::Json;
      else if (!iequals(c.args[i + 1], "hash")) return c.r.error("ERR ON takes HASH or JSON");
      i += 2;
    } else if (opt == "prefix") {
      if (!counted(&count)) return c.r.error("ERR bad PREFIX count");
      for (size_t k = 0; k < count; ++k) def.prefixes.emplace_back(c.args[i + 2 + k]);
      i += 2 + count;
    } else if (opt == "stopwords") {
      if (!counted(&count)) return c.r.error("ERR bad STOPWORDS count");
      i += 2 + count;
    } else if (one_of(opt, {"language", "language_field", "score", "score_field", "payload_field"}) && i + 1 < n) {
      i += 2;
    } else if (one_of(opt, {"maxtextfields", "nooffsets", "nohl", "nofields", "nofreqs"})) {
      ++i;
    } else if (opt == "skipinitialscan") {
      scan = false;
      ++i;
    } else if (opt == "filter" || opt == "temporary") {
      return c.r.error(std::format("ERR {} is not supported", c.args[i]));
    } else {
      return c.r.error(std::format("ERR unknown argument '{}'", c.args[i]));
    }
  }
  if (i >= n) return c.r.error("ERR expected SCHEMA");
  ++i;
  std::set<std::string> seen;
  while (i < n) {
    IndexDef::Field f;
    std::string source(c.args[i++]);
    f.name = source;
    if (i + 1 < n && iequals(c.args[i], "as")) {
      f.name = std::string(c.args[i + 1]);
      i += 2;
    }
    if (def.json() || f.name != source) f.path = source;
    if (i >= n) return c.r.error(std::format("ERR field '{}' has no type", f.name));
    std::string type = to_lower(c.args[i++]);
    if (type == "tag") {
      f.type = IndexFieldType::Tag;
      f.separator = def.json() ? 0 : ',';
      f.case_sensitive = false;
    } else if (type == "numeric") {
      f.type = IndexFieldType::Numeric;
    } else if (one_of(type, {"text", "geo", "vector", "geoshape"})) {
      return c.r.error(std::format("ERR {} fields are not supported (only TAG and NUMERIC)", c.args[i - 1]));
    } else {
      return c.r.error(std::format("ERR unknown field type '{}'", c.args[i - 1]));
    }
    while (i < n) {
      std::string o = to_lower(c.args[i]);
      if (f.type == IndexFieldType::Tag && o == "separator" && i + 1 < n) {
        if (c.args[i + 1].size() != 1) return c.r.error("ERR SEPARATOR takes a single character");
        f.separator = c.args[i + 1][0];
        i += 2;
      } else if (f.type == IndexFieldType::Tag && o == "casesensitive") {
        f.case_sensitive = true;
        ++i;
      } else if (one_of(o, {"sortable", "unf", "noindex", "withsuffixtrie", "indexempty", "indexmissing"})) {
        ++i;  // every field can be sorted on already
      } else {
        break;
      }
    }
    if (!seen.insert(f.name).second) return c.r.error(std::format("ERR duplicate field '{}'", f.name));
    def.fields.push_back(std::move(f));
  }
  if (def.fields.empty()) return c.r.error("ERR SCHEMA needs at least one field");
  if (def.fields.size() > IndexDef::kMaxSearchFields) {
    return c.r.error(std::format("ERR an index has at most {} fields", IndexDef::kMaxSearchFields));
  }
  if (std::string error = def.compile(); !error.empty()) return c.r.error(error);
  std::string error = c.db().create_index(std::move(def), scan);
  if (!error.empty()) return c.r.error(error);
  c.r.ok();
}

void cmd_ft_search(Cmd& c) {
  auto index = find_search_index(c, c.args[1]);
  if (!index) return;
  const IndexDef& def = index->def;
  if (index->state.load() == IndexState::Failed) {
    return c.r.error(std::format("ERR index '{}' failed: {}", def.name, c.db().indexes().error_of(index)));
  }
  SearchOptions opts;
  SearchParams params;
  bool with_scores = false;
  std::vector<SearchNode> filters;  // FILTER field min max
  size_t n = c.args.size();
  for (size_t i = 3; i < n;) {
    std::string opt = to_lower(c.args[i]);
    auto count_at = [&](size_t at) -> std::optional<size_t> {
      auto v = at < n ? parse_int64(c.args[at]) : std::nullopt;
      if (!v || *v < 0 || at + 1 + static_cast<size_t>(*v) > n) return std::nullopt;
      return static_cast<size_t>(*v);
    };
    if (opt == "nocontent") {
      opts.no_content = true;
      ++i;
    } else if (opt == "withscores") {
      with_scores = true;
      ++i;
    } else if (one_of(opt, {"verbatim", "nostopwords", "inorder", "explainscore"})) {
      ++i;
    } else if (one_of(opt, {"dialect", "timeout", "slop", "language", "scorer", "expander"}) && i + 1 < n) {
      i += 2;
    } else if (opt == "limit" && i + 2 < n) {
      auto off = parse_int64(c.args[i + 1]), cnt = parse_int64(c.args[i + 2]);
      if (!off || !cnt || *off < 0 || *cnt < 0) return c.r.error("ERR bad LIMIT");
      opts.offset = *off;
      opts.limit = *cnt;
      i += 3;
    } else if (opt == "return") {
      auto count = count_at(i + 1);
      if (!count) return c.r.error("ERR bad RETURN count");
      size_t end = i + 2 + *count;
      for (size_t k = i + 2; k < end;) {
        std::string name(c.args[k]);
        if (k + 1 < end && iequals(c.args[k + 1], "as")) {
          if (k + 2 >= end) return c.r.error("ERR bad RETURN: AS needs a name");
          opts.return_fields.emplace_back(name, std::string(c.args[k + 2]));
          k += 3;
        } else {
          opts.return_fields.emplace_back(name, name);
          ++k;
        }
      }
      // RETURN 0 means no content.
      if (*count == 0) opts.no_content = true;
      i = end;
    } else if (opt == "sortby" && i + 1 < n) {
      std::string_view field = c.args[i + 1];
      if (field.starts_with("@")) field.remove_prefix(1);
      int fi = def.field_index(field);
      if (fi < 0) return c.r.error(std::format("ERR Property '{}' not loaded nor in schema", field));
      opts.sort_field = static_cast<size_t>(fi);
      i += 2;
      if (i < n && (iequals(c.args[i], "asc") || iequals(c.args[i], "desc"))) {
        opts.sort_desc = iequals(c.args[i], "desc");
        ++i;
      }
      if (i < n && iequals(c.args[i], "withcount")) ++i;
    } else if (opt == "params") {
      auto count = count_at(i + 1);
      if (!count || *count % 2 != 0) return c.r.error("ERR bad PARAMS count");
      for (size_t k = i + 2; k < i + 2 + *count; k += 2) params[std::string(c.args[k])] = std::string(c.args[k + 1]);
      i += 2 + *count;
    } else if (opt == "filter" && i + 3 < n) {
      // The old form of @field:[min max].
      std::string q = std::format("@{}:[{} {}]", c.args[i + 1], c.args[i + 2], c.args[i + 3]);
      std::string error;
      auto f = parse_search_query(q, def, {}, &error);
      if (!f) return c.r.error(error);
      filters.push_back(std::move(*f));
      i += 4;
    } else {
      return c.r.error(std::format("ERR unknown or unsupported argument '{}'", c.args[i]));
    }
  }
  std::string error;
  auto query = parse_search_query(c.args[2], def, params, &error);
  if (!query) return c.r.error(error);
  if (!filters.empty()) {
    SearchNode all;
    all.kind = SearchNode::Kind::And;
    all.children.push_back(std::move(*query));
    for (auto& f : filters) all.children.push_back(std::move(f));
    query = std::move(all);
  }
  if (!reads_all_keys(c)) opts.key_filter = [&c](std::string_view key) { return may_read_key(c, key); };
  SearchResult res = c.db().search(index, *query, opts);
  size_t per_hit = 1 + (with_scores ? 1 : 0) + (opts.no_content ? 0 : 1);
  c.r.array(1 + res.hits.size() * per_hit);
  c.r.integer(static_cast<int64_t>(res.total));
  for (const auto& h : res.hits) {
    c.r.bulk(h.key);
    if (with_scores) c.r.bulk("1");
    if (opts.no_content) continue;
    c.r.array(h.fields.size() * 2);
    for (const auto& [f, v] : h.fields) {
      c.r.bulk(f);
      c.r.bulk(v);
    }
  }
}

void cmd_ft_dropindex(Cmd& c) {
  auto index = find_search_index(c, c.args[1]);
  if (!index) return;
  bool delete_docs = false;
  if (c.args.size() == 3) {
    if (!iequals(c.args[2], "dd")) return c.r.error(kSyntax);
    delete_docs = true;
  } else if (c.args.size() > 3) {
    return c.r.error(kSyntax);
  }
  std::vector<std::string> keys;
  if (delete_docs) keys = c.db().search_keys(index);
  c.db().drop_index(index->def.name);
  for (const auto& key : keys) c.db().del(key);
  c.r.ok();
}

void cmd_ft_info(Cmd& c) {
  auto index = find_search_index(c, c.args[1]);
  if (!index) return;
  const IndexDef& def = index->def;
  IndexState state = index->state.load();
  uint64_t docs = 0, records = 0;
  c.db().count_search_entries(index, &docs, &records);
  c.r.map(10);
  c.r.bulk("index_name");
  c.r.bulk(def.name);
  c.r.bulk("index_options");
  c.r.array(0);
  c.r.bulk("index_definition");
  c.r.array(6);
  c.r.bulk("key_type");
  c.r.bulk(def.json() ? "JSON" : "HASH");
  c.r.bulk("prefixes");
  c.r.array(def.prefixes.empty() ? 1 : def.prefixes.size());
  if (def.prefixes.empty()) c.r.bulk("");
  for (const auto& p : def.prefixes) c.r.bulk(p);
  c.r.bulk("default_score");
  c.r.bulk("1");
  c.r.bulk("attributes");
  c.r.array(def.fields.size());
  for (size_t i = 0; i < def.fields.size(); ++i) {
    const auto& f = def.fields[i];
    bool tag = f.type == IndexFieldType::Tag;
    c.r.array(6 + (tag ? 2 : 0) + (tag && f.case_sensitive ? 1 : 0));
    c.r.bulk("identifier");
    c.r.bulk(def.json() ? std::string_view(f.path) : def.hash_field(i));
    c.r.bulk("attribute");
    c.r.bulk(f.name);
    c.r.bulk("type");
    c.r.bulk(index_field_type_name(f.type));
    if (tag) {
      c.r.bulk("SEPARATOR");
      c.r.bulk(f.separator ? std::string(1, f.separator) : std::string());
      if (f.case_sensitive) c.r.bulk("CASESENSITIVE");
    }
  }
  c.r.bulk("num_docs");
  c.r.integer(static_cast<int64_t>(docs));
  c.r.bulk("num_records");
  c.r.integer(static_cast<int64_t>(records));
  c.r.bulk("indexing");
  c.r.integer(state == IndexState::Building ? 1 : 0);
  c.r.bulk("percent_indexed");
  c.r.bulk(state == IndexState::Building ? "0" : "1");
  c.r.bulk("hash_indexing_failures");
  c.r.integer(0);
  c.r.bulk("state");
  c.r.bulk(index_state_name(state));
}

void cmd_ft_list(Cmd& c) {
  std::vector<std::string> names;
  for (const auto& index : c.db().indexes().all()) {
    if (index->def.search()) names.push_back(index->def.name);
  }
  c.r.array(names.size());
  for (const auto& name : names) c.r.bulk(name);
}

void cmd_ft_tagvals(Cmd& c) {
  auto index = find_search_index(c, c.args[1]);
  if (!index) return;
  int fi = index->def.field_index(c.args[2]);
  if (fi < 0 || index->def.fields[static_cast<size_t>(fi)].type != IndexFieldType::Tag) {
    return c.r.error(std::format("ERR no TAG field '{}' in index '{}'", c.args[2], index->def.name));
  }
  auto values = c.db().tag_values(index, static_cast<size_t>(fi));
  c.r.array(values.size());
  for (const auto& v : values) c.r.bulk(v);
}

}  // namespace

CommandList search_commands() {
  return {
      {"ft.create", {cmd_ft_create, -5}},   {"ft.search", {cmd_ft_search, -3}},  {"ft.dropindex", {cmd_ft_dropindex, -2}},
      {"ft.info", {cmd_ft_info, 2}},        {"ft._list", {cmd_ft_list, 1}},     {"ft.tagvals", {cmd_ft_tagvals, 3}},
  };
}

}  // namespace datadam
