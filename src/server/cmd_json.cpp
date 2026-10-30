// JSON.* commands (RedisJSON compatible). A JSON key holds one document, kept
// as compact text in its record (ValueType::Json): every write parses it,
// changes it and stores it whole, under the key's lock.
//
// Paths are JSONPath ($...) or legacy (.a.b): with $, a command acts on every
// match and answers with one result per match (nil where the value has the
// wrong type); with a legacy path, it acts on the first match and answers
// with that result, or an error. A missing path means the legacy root ".".

#include <cmath>
#include <format>
#include <limits>
#include <optional>
#include <utility>

#include "json/json.h"
#include "json/json_path.h"
#include "server/command.h"
#include "util/strings.h"

namespace datadam {

namespace {

constexpr size_t kMaxStringSize = 512ULL << 20;  // as APPEND's
constexpr std::string_view kNoKey = "ERR could not perform this operation on a key that doesn't exist";

std::optional<JsonPath> path_arg(Cmd& c, std::string_view text) {
  std::string error;
  auto p = JsonPath::parse(text, &error);
  if (!p) c.r.error(error);
  return p;
}

std::optional<Json> value_arg(Cmd& c, std::string_view text) {
  std::string error;
  auto v = parse_json(text, &error);
  if (!v) c.r.error(error);
  return v;
}

std::string no_path(const JsonPath& p) { return std::format("ERR Path '{}' does not exist", p.text()); }

enum class Doc { Missing, WrongType, Ok };

// The key's document, for reading.
Doc read_doc(Cmd& c, std::string_view key, std::optional<Json>* doc) {
  Doc d = Doc::Missing;
  std::string text;
  c.db().read(key, [&](const RecordView& v) {
    d = v.type == ValueType::Json ? Doc::Ok : Doc::WrongType;
    if (d == Doc::Ok) text = std::string(v.value);  // parsed outside the cache lock
  });
  if (d != Doc::Ok) return d;
  std::string error;
  *doc = parse_json(text, &error);
  return *doc ? Doc::Ok : Doc::WrongType;
}

// What a writing command did to the document.
enum class Change { None, Store, Remove };

// Read-modify-write of the key's document under its lock. f(std::optional<Json>&)
// gets nullopt for a missing key (setting it creates the key) and says what
// to do. Returns Missing only if the key did not exist and f created nothing.
template <class F>
Doc update_doc(Cmd& c, std::string_view key, F&& f, bool* too_deep = nullptr) {
  Doc d = Doc::Ok;
  c.db().update(key, [&](std::optional<Record>& rec) {
    std::optional<Json> doc;
    if (rec) {
      std::string error;
      if (rec->type == ValueType::Json) doc = parse_json(rec->value, &error);
      if (!doc) {
        d = Doc::WrongType;
        return Database::Op::Keep;
      }
    }
    Change change = f(doc);
    if (!rec && !doc) d = Doc::Missing;
    if (change == Change::Remove) return rec ? Database::Op::Delete : Database::Op::Keep;
    if (change == Change::None || !doc) return Database::Op::Keep;
    if (json_depth(*doc) > kMaxJsonDepth) {
      if (too_deep) *too_deep = true;
      return Database::Op::Keep;
    }
    if (!rec) {
      rec.emplace();
      rec->type = ValueType::Json;
    }
    rec->value = dump_json(*doc);
    return Database::Op::Put;
  });
  return d;
}

bool wrong_or_missing(Cmd& c, Doc d, bool missing_is_error = true) {
  if (d == Doc::WrongType) {
    c.r.error(kWrongType);
    return true;
  }
  if (d == Doc::Missing && missing_is_error) {
    c.r.error(kNoKey);
    return true;
  }
  return false;
}

// One integer per match ($), or the first one (legacy).
void reply_ints(Cmd& c, const JsonPath& p, const std::vector<std::optional<int64_t>>& results, std::string_view wrong) {
  if (!p.legacy()) {
    c.r.array(results.size());
    for (const auto& r : results) r ? c.r.integer(*r) : c.r.null();
    return;
  }
  if (results.empty()) return c.r.error(no_path(p));
  if (!results[0]) return c.r.error(wrong);
  c.r.integer(*results[0]);
}

// One JSON value per match ($: as a JSON array in a bulk string), or the first one.
void reply_json_values(Cmd& c, const JsonPath& p, const std::vector<std::optional<Json>>& results,
                       std::string_view wrong) {
  if (!p.legacy()) {
    Json all = Json::array();
    for (const auto& r : results) all.push_back(r ? *r : Json(nullptr));
    return c.r.bulk(dump_json(all));
  }
  if (results.empty()) return c.r.error(no_path(p));
  if (!results[0]) return c.r.error(wrong);
  c.r.bulk(dump_json(*results[0]));
}

// ---- JSON.SET key path value [NX | XX]

void cmd_json_set(Cmd& c) {
  auto path = path_arg(c, c.args[2]);
  if (!path) return;
  auto value = value_arg(c, c.args[3]);
  if (!value) return;
  bool nx = false, xx = false;
  for (size_t i = 4; i < c.args.size(); ++i) {
    if (iequals(c.args[i], "nx") && !xx) nx = true;
    else if (iequals(c.args[i], "xx") && !nx) xx = true;
    else return c.r.error(kSyntax);
  }
  bool done = false, not_root = false, too_deep = false;
  Doc d = update_doc(
      c, c.args[1],
      [&](std::optional<Json>& doc) {
        if (!doc) {
          if (!path->root()) {
            not_root = true;
            return Change::None;
          }
          if (xx) return Change::None;
          doc = *value;
          done = true;
          return Change::Store;
        }
        if (path->root()) {
          if (nx) return Change::None;
          *doc = *value;
          done = true;
          return Change::Store;
        }
        auto existing = path->select(*doc);
        if ((nx && !existing.empty()) || (xx && existing.empty())) return Change::None;
        std::vector<JsonPath::Match> targets;
        if (xx) {
          targets = std::move(existing);
        } else {
          size_t created = 0;
          targets = path->select_or_create(*doc, &created);
          if (nx && created == 0) return Change::None;
        }
        if (targets.empty()) return Change::None;
        // Later and deeper first, so a replaced value never holds one still to replace.
        for (auto it = targets.rbegin(); it != targets.rend(); ++it) *it->value = *value;
        done = true;
        return Change::Store;
      },
      &too_deep);
  if (d == Doc::WrongType) return c.r.error(kWrongType);
  if (not_root) return c.r.error("ERR new objects must be created at the root");
  if (too_deep) return c.r.error(std::format("ERR JSON nests deeper than {} levels", kMaxJsonDepth));
  done ? c.r.ok() : c.r.null();
}

// ---- JSON.GET key [INDENT s] [NEWLINE s] [SPACE s] [path ...]

void cmd_json_get(Cmd& c) {
  JsonFormat format;
  size_t i = 2;
  for (; i + 1 < c.args.size(); i += 2) {
    std::string opt = to_lower(c.args[i]);
    if (opt == "indent") format.indent = std::string(c.args[i + 1]);
    else if (opt == "newline") format.newline = std::string(c.args[i + 1]);
    else if (opt == "space") format.space = std::string(c.args[i + 1]);
    else break;
  }
  if (i < c.args.size() && iequals(c.args[i], "noescape")) ++i;
  std::vector<JsonPath> paths;
  for (; i < c.args.size(); ++i) {
    auto p = path_arg(c, c.args[i]);
    if (!p) return;
    paths.push_back(std::move(*p));
  }
  if (paths.empty() && format.plain()) {
    // The whole document as stored: compact already, no need to parse it.
    bool wrong = false, found = c.db().read(c.args[1], [&](const RecordView& v) {
      if (v.type == ValueType::Json) c.r.bulk(v.value);
      else wrong = true;
    });
    if (wrong) return c.r.error(kWrongType);
    if (!found) c.r.null();
    return;
  }
  std::optional<Json> doc;
  Doc d = read_doc(c, c.args[1], &doc);
  if (d == Doc::WrongType) return c.r.error(kWrongType);
  if (d == Doc::Missing) return c.r.null();
  auto one = [&](const JsonPath& p, Json* out) {
    auto found = p.select(std::as_const(*doc));
    if (p.legacy()) {
      if (found.empty()) return false;
      *out = *found[0];
      return true;
    }
    *out = Json::array();
    for (const Json* v : found) out->push_back(*v);
    return true;
  };
  if (paths.empty()) return c.r.bulk(dump_json(*doc, format));
  if (paths.size() == 1) {
    Json v;
    if (!one(paths[0], &v)) return c.r.error(no_path(paths[0]));
    return c.r.bulk(dump_json(v, format));
  }
  Json all = Json::object();
  for (const auto& p : paths) {
    Json v;
    if (!one(p, &v)) return c.r.error(no_path(p));
    all[p.text()] = std::move(v);
  }
  c.r.bulk(dump_json(all, format));
}

// ---- JSON.MGET key [key ...] path

void cmd_json_mget(Cmd& c) {
  auto path = path_arg(c, c.args.back());
  if (!path) return;
  c.r.array(c.args.size() - 2);
  for (size_t i = 1; i + 1 < c.args.size(); ++i) {
    std::optional<Json> doc;
    if (read_doc(c, c.args[i], &doc) != Doc::Ok) {
      c.r.null();
      continue;
    }
    auto found = path->select(std::as_const(*doc));
    if (path->legacy()) {
      found.empty() ? c.r.null() : c.r.bulk(dump_json(*found[0]));
    } else {
      Json all = Json::array();
      for (const Json* v : found) all.push_back(*v);
      c.r.bulk(dump_json(all));
    }
  }
}

// ---- JSON.MSET key path value [key path value ...]

void cmd_json_mset(Cmd& c) {
  if ((c.args.size() - 1) % 3 != 0) return c.r.error("ERR wrong number of arguments for 'json.mset' command");
  struct Item {
    std::string_view key;
    JsonPath path;
    Json value;
  };
  std::vector<Item> items;
  for (size_t i = 1; i < c.args.size(); i += 3) {
    auto p = path_arg(c, c.args[i + 1]);
    if (!p) return;
    auto v = value_arg(c, c.args[i + 2]);
    if (!v) return;
    items.push_back({c.args[i], std::move(*p), std::move(*v)});
  }
  for (const auto& item : items) {
    bool not_root = false;
    Doc d = update_doc(c, item.key, [&](std::optional<Json>& doc) {
      if (!doc) {
        if (!item.path.root()) {
          not_root = true;
          return Change::None;
        }
        doc = item.value;
        return Change::Store;
      }
      if (item.path.root()) {
        *doc = item.value;
        return Change::Store;
      }
      size_t created = 0;
      auto targets = item.path.select_or_create(*doc, &created);
      for (auto it = targets.rbegin(); it != targets.rend(); ++it) *it->value = item.value;
      return targets.empty() ? Change::None : Change::Store;
    });
    if (d == Doc::WrongType) return c.r.error(kWrongType);
    if (not_root) return c.r.error("ERR new objects must be created at the root");
  }
  c.r.ok();
}

// ---- JSON.DEL / JSON.FORGET key [path]

void cmd_json_del(Cmd& c) {
  auto path = path_arg(c, c.args.size() > 2 ? c.args[2] : ".");
  if (!path) return;
  size_t removed = 0;
  Doc d = update_doc(c, c.args[1], [&](std::optional<Json>& doc) {
    if (!doc) return Change::None;
    if (path->root()) {
      removed = 1;
      return Change::Remove;
    }
    removed = path->erase(*doc);
    return removed ? Change::Store : Change::None;
  });
  if (d == Doc::WrongType) return c.r.error(kWrongType);
  c.r.integer(static_cast<int64_t>(removed));
}

// ---- JSON.TYPE key [path]

void cmd_json_type(Cmd& c) {
  auto path = path_arg(c, c.args.size() > 2 ? c.args[2] : ".");
  if (!path) return;
  std::optional<Json> doc;
  Doc d = read_doc(c, c.args[1], &doc);
  if (d == Doc::WrongType) return c.r.error(kWrongType);
  if (d == Doc::Missing) return path->legacy() ? c.r.null() : c.r.array(0);
  auto found = path->select(std::as_const(*doc));
  if (path->legacy()) return found.empty() ? c.r.null() : c.r.simple(json_type_name(*found[0]));
  c.r.array(found.size());
  for (const Json* v : found) c.r.simple(json_type_name(*v));
}

// ---- JSON.NUMINCRBY / JSON.NUMMULTBY key path number

std::optional<Json> arithmetic(const Json& v, const Json& n, bool multiply) {
  if (!v.is_number()) return std::nullopt;
  if (v.is_number_integer() && n.is_number_integer()) {
    int64_t a = v.get<int64_t>(), b = n.get<int64_t>(), r = 0;
    bool overflow = false;
#if defined(_MSC_VER) && !defined(__clang__)
    long double exact = multiply ? static_cast<long double>(a) * b : static_cast<long double>(a) + b;
    overflow = exact > static_cast<long double>(std::numeric_limits<int64_t>::max()) ||
               exact < static_cast<long double>(std::numeric_limits<int64_t>::min());
    if (!overflow) r = multiply ? a * b : a + b;
#else
    overflow = multiply ? __builtin_mul_overflow(a, b, &r) : __builtin_add_overflow(a, b, &r);
#endif
    if (!overflow) return Json(r);
  }
  double r = multiply ? v.get<double>() * n.get<double>() : v.get<double>() + n.get<double>();
  if (!std::isfinite(r)) return std::nullopt;
  return Json(r);
}

void numeric(Cmd& c, bool multiply) {
  auto path = path_arg(c, c.args[2]);
  if (!path) return;
  auto n = value_arg(c, c.args[3]);
  if (!n) return;
  if (!n->is_number()) return c.r.error("ERR the increment is not a number");
  std::vector<std::optional<Json>> results;
  bool failed = false;
  Doc d = update_doc(c, c.args[1], [&](std::optional<Json>& doc) {
    if (!doc) return Change::None;
    auto found = path->select(*doc);
    for (auto& m : found) {
      auto r = arithmetic(*m.value, *n, multiply);
      if (!r && m.value->is_number()) failed = true;  // the result is not a finite number
      if (r) *m.value = *r;
      results.push_back(r);
    }
    return failed ? Change::None : Change::Store;
  });
  if (wrong_or_missing(c, d)) return;
  if (failed) return c.r.error("ERR the result is not a finite number");
  reply_json_values(c, *path, results, "ERR the value at the path is not a number");
}

void cmd_json_numincrby(Cmd& c) { numeric(c, false); }
void cmd_json_nummultby(Cmd& c) { numeric(c, true); }

// ---- strings

// JSON.STRAPPEND key [path] "json string"
void cmd_json_strappend(Cmd& c) {
  bool with_path = c.args.size() > 3;
  auto path = path_arg(c, with_path ? c.args[2] : ".");
  if (!path) return;
  auto value = value_arg(c, c.args[with_path ? 3 : 2]);
  if (!value) return;
  if (!value->is_string()) return c.r.error("ERR the value to append must be a JSON string");
  std::vector<std::optional<int64_t>> lengths;
  bool too_big = false;
  Doc d = update_doc(c, c.args[1], [&](std::optional<Json>& doc) {
    if (!doc) return Change::None;
    for (auto& m : path->select(*doc)) {
      if (!m.value->is_string()) {
        lengths.emplace_back();
        continue;
      }
      auto& s = m.value->get_ref<std::string&>();
      // As APPEND: a string grows to 512 MB at most.
      if (s.size() + value->get_ref<const std::string&>().size() > kMaxStringSize) {
        too_big = true;
        return Change::None;
      }
      s += value->get_ref<const std::string&>();
      lengths.push_back(static_cast<int64_t>(s.size()));
    }
    return Change::Store;
  });
  if (too_big) return c.r.error("ERR string exceeds maximum allowed size (proto-max-bulk-len)");
  if (wrong_or_missing(c, d)) return;
  reply_ints(c, *path, lengths, "ERR the value at the path is not a string");
}

// Applies f(const Json&) -> optional<int64_t> to each match, for the length commands.
template <class F>
void read_ints(Cmd& c, std::string_view wrong, F&& f) {
  auto path = path_arg(c, c.args.size() > 2 ? c.args[2] : ".");
  if (!path) return;
  std::optional<Json> doc;
  Doc d = read_doc(c, c.args[1], &doc);
  if (d == Doc::WrongType) return c.r.error(kWrongType);
  if (d == Doc::Missing) return path->legacy() ? c.r.null() : c.r.array(0);
  std::vector<std::optional<int64_t>> results;
  for (const Json* v : path->select(std::as_const(*doc))) results.push_back(f(*v));
  reply_ints(c, *path, results, wrong);
}

void cmd_json_strlen(Cmd& c) {
  read_ints(c, "ERR the value at the path is not a string", [](const Json& v) -> std::optional<int64_t> {
    if (!v.is_string()) return std::nullopt;
    return static_cast<int64_t>(v.get_ref<const std::string&>().size());
  });
}

// ---- arrays

std::optional<std::vector<Json>> values_from(Cmd& c, size_t first) {
  std::vector<Json> values;
  for (size_t i = first; i < c.args.size(); ++i) {
    auto v = value_arg(c, c.args[i]);
    if (!v) return std::nullopt;
    values.push_back(std::move(*v));
  }
  return values;
}

// JSON.ARRAPPEND key path value [value ...]
void cmd_json_arrappend(Cmd& c) {
  auto path = path_arg(c, c.args[2]);
  if (!path) return;
  auto values = values_from(c, 3);
  if (!values) return;
  std::vector<std::optional<int64_t>> sizes;
  bool too_deep = false;
  Doc d = update_doc(
      c, c.args[1],
      [&](std::optional<Json>& doc) {
        if (!doc) return Change::None;
        auto found = path->select(*doc);
        sizes.resize(found.size());
        // Later and deeper first: growing an array moves the values inside it.
        for (size_t i = found.size(); i-- > 0;) {
          if (!found[i].value->is_array()) continue;
          for (const auto& v : *values) found[i].value->push_back(v);
          sizes[i] = static_cast<int64_t>(found[i].value->size());
        }
        return Change::Store;
      },
      &too_deep);
  if (wrong_or_missing(c, d)) return;
  if (too_deep) return c.r.error(std::format("ERR JSON nests deeper than {} levels", kMaxJsonDepth));
  reply_ints(c, *path, sizes, "ERR the value at the path is not an array");
}

// JSON.ARRINSERT key path index value [value ...]
void cmd_json_arrinsert(Cmd& c) {
  auto path = path_arg(c, c.args[2]);
  if (!path) return;
  auto index = parse_int64(c.args[3]);
  if (!index) return c.r.error(kNotInteger);
  auto values = values_from(c, 4);
  if (!values) return;
  std::vector<std::optional<int64_t>> sizes;
  bool out_of_range = false, too_deep = false;
  Doc d = update_doc(
      c, c.args[1],
      [&](std::optional<Json>& doc) {
        if (!doc) return Change::None;
        auto found = path->select(*doc);
        sizes.resize(found.size());
        for (size_t i = found.size(); i-- > 0;) {
          Json& a = *found[i].value;
          if (!a.is_array()) continue;
          int64_t n = static_cast<int64_t>(a.size()), at = *index < 0 ? *index + n : *index;
          if (at < 0 || at > n) {
            out_of_range = true;
            continue;
          }
          for (size_t k = 0; k < values->size(); ++k) a.insert(a.begin() + at + static_cast<int64_t>(k), (*values)[k]);
          sizes[i] = static_cast<int64_t>(a.size());
        }
        return out_of_range ? Change::None : Change::Store;
      },
      &too_deep);
  if (wrong_or_missing(c, d)) return;
  if (out_of_range) return c.r.error("ERR index out of bounds");
  if (too_deep) return c.r.error(std::format("ERR JSON nests deeper than {} levels", kMaxJsonDepth));
  reply_ints(c, *path, sizes, "ERR the value at the path is not an array");
}

void cmd_json_arrlen(Cmd& c) {
  read_ints(c, "ERR the value at the path is not an array", [](const Json& v) -> std::optional<int64_t> {
    if (!v.is_array()) return std::nullopt;
    return static_cast<int64_t>(v.size());
  });
}

// JSON.ARRPOP key [path [index]]
void cmd_json_arrpop(Cmd& c) {
  auto path = path_arg(c, c.args.size() > 2 ? c.args[2] : ".");
  if (!path) return;
  int64_t index = -1;
  if (c.args.size() > 3) {
    auto i = parse_int64(c.args[3]);
    if (!i) return c.r.error(kNotInteger);
    index = *i;
  }
  if (c.args.size() > 4) return c.r.error(kSyntax);
  std::vector<std::optional<Json>> popped;
  Doc d = update_doc(c, c.args[1], [&](std::optional<Json>& doc) {
    if (!doc) return Change::None;
    auto found = path->select(*doc);
    popped.resize(found.size());
    for (size_t i = found.size(); i-- > 0;) {
      Json& a = *found[i].value;
      if (!a.is_array() || a.empty()) continue;
      int64_t n = static_cast<int64_t>(a.size());
      int64_t at = std::clamp<int64_t>(index < 0 ? index + n : index, 0, n - 1);  // out of range: the nearest end
      popped[i] = a[static_cast<size_t>(at)];
      a.erase(static_cast<size_t>(at));
    }
    return Change::Store;
  });
  if (wrong_or_missing(c, d)) return;
  if (!path->legacy()) {
    c.r.array(popped.size());
    for (const auto& p : popped) p ? c.r.bulk(dump_json(*p)) : c.r.null();
    return;
  }
  if (popped.empty()) return c.r.error(no_path(*path));
  popped[0] ? c.r.bulk(dump_json(*popped[0])) : c.r.null();
}

// JSON.ARRTRIM key path start stop (inclusive, negative from the end, like LTRIM)
void cmd_json_arrtrim(Cmd& c) {
  auto path = path_arg(c, c.args[2]);
  if (!path) return;
  auto start = parse_int64(c.args[3]), stop = parse_int64(c.args[4]);
  if (!start || !stop) return c.r.error(kNotInteger);
  std::vector<std::optional<int64_t>> sizes;
  Doc d = update_doc(c, c.args[1], [&](std::optional<Json>& doc) {
    if (!doc) return Change::None;
    auto found = path->select(*doc);
    sizes.resize(found.size());
    for (size_t i = found.size(); i-- > 0;) {
      Json& a = *found[i].value;
      if (!a.is_array()) continue;
      int64_t n = static_cast<int64_t>(a.size());
      int64_t lo = *start < 0 ? std::max<int64_t>(*start + n, 0) : *start;
      int64_t hi = *stop < 0 ? *stop + n : std::min<int64_t>(*stop, n - 1);
      Json kept = Json::array();
      for (int64_t k = lo; k <= hi && k < n; ++k) kept.push_back(std::move(a[static_cast<size_t>(k)]));
      a = std::move(kept);
      sizes[i] = static_cast<int64_t>(a.size());
    }
    return Change::Store;
  });
  if (wrong_or_missing(c, d)) return;
  reply_ints(c, *path, sizes, "ERR the value at the path is not an array");
}

// JSON.ARRINDEX key path value [start [stop]]   (stop exclusive; 0: the end)
void cmd_json_arrindex(Cmd& c) {
  auto path = path_arg(c, c.args[2]);
  if (!path) return;
  auto value = value_arg(c, c.args[3]);
  if (!value) return;
  int64_t start = 0, stop = 0;
  if (c.args.size() > 4) {
    auto s = parse_int64(c.args[4]);
    if (!s) return c.r.error(kNotInteger);
    start = *s;
  }
  if (c.args.size() > 5) {
    auto s = parse_int64(c.args[5]);
    if (!s) return c.r.error(kNotInteger);
    stop = *s;
  }
  std::optional<Json> doc;
  Doc d = read_doc(c, c.args[1], &doc);
  if (wrong_or_missing(c, d)) return;
  std::vector<std::optional<int64_t>> results;
  for (const Json* v : path->select(std::as_const(*doc))) {
    if (!v->is_array()) {
      results.emplace_back();
      continue;
    }
    int64_t n = static_cast<int64_t>(v->size());
    int64_t lo = std::clamp<int64_t>(start < 0 ? start + n : start, 0, n);
    int64_t hi = stop == 0 ? n : std::clamp<int64_t>(stop < 0 ? stop + n : stop, 0, n);
    int64_t at = -1;
    for (int64_t k = lo; k < hi && at < 0; ++k) {
      if ((*v)[static_cast<size_t>(k)] == *value) at = k;
    }
    results.push_back(at);
  }
  reply_ints(c, *path, results, "ERR the value at the path is not an array");
}

// ---- objects

void cmd_json_objkeys(Cmd& c) {
  auto path = path_arg(c, c.args.size() > 2 ? c.args[2] : ".");
  if (!path) return;
  std::optional<Json> doc;
  Doc d = read_doc(c, c.args[1], &doc);
  if (d == Doc::WrongType) return c.r.error(kWrongType);
  if (d == Doc::Missing) return path->legacy() ? c.r.null() : c.r.array(0);
  auto found = path->select(std::as_const(*doc));
  auto keys = [&](const Json& v) {
    c.r.array(v.size());
    for (auto it = v.begin(); it != v.end(); ++it) c.r.bulk(it.key());
  };
  if (path->legacy()) {
    if (found.empty()) return c.r.error(no_path(*path));
    if (!found[0]->is_object()) return c.r.error("ERR the value at the path is not an object");
    return keys(*found[0]);
  }
  c.r.array(found.size());
  for (const Json* v : found) v->is_object() ? keys(*v) : c.r.null();
}

void cmd_json_objlen(Cmd& c) {
  read_ints(c, "ERR the value at the path is not an object", [](const Json& v) -> std::optional<int64_t> {
    if (!v.is_object()) return std::nullopt;
    return static_cast<int64_t>(v.size());
  });
}

// ---- others

// JSON.TOGGLE key path
void cmd_json_toggle(Cmd& c) {
  auto path = path_arg(c, c.args[2]);
  if (!path) return;
  std::vector<std::optional<int64_t>> results;
  Doc d = update_doc(c, c.args[1], [&](std::optional<Json>& doc) {
    if (!doc) return Change::None;
    for (auto& m : path->select(*doc)) {
      if (!m.value->is_boolean()) {
        results.emplace_back();
        continue;
      }
      bool now = !m.value->get<bool>();
      *m.value = now;
      results.push_back(now ? 1 : 0);
    }
    return Change::Store;
  });
  if (wrong_or_missing(c, d)) return;
  if (!path->legacy()) return reply_ints(c, *path, results, {});
  if (results.empty()) return c.r.error(no_path(*path));
  if (!results[0]) return c.r.error("ERR the value at the path is not a boolean");
  c.r.bulk(*results[0] ? "true" : "false");
}

// JSON.CLEAR key [path]: containers become empty and numbers 0.
void cmd_json_clear(Cmd& c) {
  auto path = path_arg(c, c.args.size() > 2 ? c.args[2] : ".");
  if (!path) return;
  int64_t cleared = 0;
  Doc d = update_doc(c, c.args[1], [&](std::optional<Json>& doc) {
    if (!doc) return Change::None;
    auto found = path->select(*doc);
    for (auto it = found.rbegin(); it != found.rend(); ++it) {
      Json& v = *it->value;
      if (v.is_structured()) {
        if (!v.empty()) ++cleared;
        v = v.is_object() ? Json::object() : Json::array();
      } else if (v.is_number()) {
        if (v != Json(0)) ++cleared;
        v = 0;
      }
    }
    return cleared ? Change::Store : Change::None;
  });
  if (d == Doc::WrongType) return c.r.error(kWrongType);
  c.r.integer(cleared);
}

// JSON.MERGE key path value (RFC 7386 merge patch: null removes a member)
void cmd_json_merge(Cmd& c) {
  auto path = path_arg(c, c.args[2]);
  if (!path) return;
  auto patch = value_arg(c, c.args[3]);
  if (!patch) return;
  bool not_root = false, too_deep = false, found_any = false;
  Doc d = update_doc(
      c, c.args[1],
      [&](std::optional<Json>& doc) {
        if (!doc) {
          if (!path->root()) {
            not_root = true;
            return Change::None;
          }
          doc = Json::object();
          doc->merge_patch(*patch);
          found_any = true;
          return Change::Store;
        }
        auto found = path->select(*doc);
        for (auto it = found.rbegin(); it != found.rend(); ++it) it->value->merge_patch(*patch);
        found_any = !found.empty();
        return found_any ? Change::Store : Change::None;
      },
      &too_deep);
  if (d == Doc::WrongType) return c.r.error(kWrongType);
  if (not_root) return c.r.error("ERR new objects must be created at the root");
  if (too_deep) return c.r.error(std::format("ERR JSON nests deeper than {} levels", kMaxJsonDepth));
  if (!found_any && path->legacy()) return c.r.error(no_path(*path));
  c.r.ok();
}

}  // namespace

CommandList json_commands() {
  return {
      {"json.set", {cmd_json_set, -4}},
      {"json.get", {cmd_json_get, -2}},
      {"json.mget", {cmd_json_mget, -3}},
      {"json.mset", {cmd_json_mset, -4}},
      {"json.del", {cmd_json_del, -2}},
      {"json.forget", {cmd_json_del, -2}},
      {"json.type", {cmd_json_type, -2}},
      {"json.numincrby", {cmd_json_numincrby, 4}},
      {"json.nummultby", {cmd_json_nummultby, 4}},
      {"json.strappend", {cmd_json_strappend, -3}},
      {"json.strlen", {cmd_json_strlen, -2}},
      {"json.arrappend", {cmd_json_arrappend, -4}},
      {"json.arrinsert", {cmd_json_arrinsert, -5}},
      {"json.arrlen", {cmd_json_arrlen, -2}},
      {"json.arrpop", {cmd_json_arrpop, -2}},
      {"json.arrtrim", {cmd_json_arrtrim, 5}},
      {"json.arrindex", {cmd_json_arrindex, -4}},
      {"json.objkeys", {cmd_json_objkeys, -2}},
      {"json.objlen", {cmd_json_objlen, -2}},
      {"json.toggle", {cmd_json_toggle, 3}},
      {"json.clear", {cmd_json_clear, -2}},
      {"json.merge", {cmd_json_merge, 4}},
  };
}

}  // namespace datadam
