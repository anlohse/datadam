#include "index/index_def.h"

#include <algorithm>

#include "storage/data_keys.h"
#include "util/strings.h"

namespace datadam {

namespace {

// 1: hash indexes; 2 adds the source and, per field, a JSON path; 3 adds the
// kind and, per field, the TAG separator and flags.
constexpr uint8_t kDefFormat = 3;
constexpr uint8_t kFieldCaseFolded = 1;

void put_u32(std::string& out, uint32_t v) {
  for (int i = 3; i >= 0; --i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

bool get_u32(std::string_view& in, uint32_t* v) {
  if (in.size() < 4) return false;
  *v = 0;
  for (size_t i = 0; i < 4; ++i) *v = (*v << 8) | static_cast<uint8_t>(in[i]);
  in.remove_prefix(4);
  return true;
}

void put_str(std::string& out, std::string_view s) {
  put_u32(out, static_cast<uint32_t>(s.size()));
  out.append(s);
}

bool get_str(std::string_view& in, std::string* s) {
  uint32_t n;
  if (!get_u32(in, &n) || in.size() < n) return false;
  s->assign(in.substr(0, n));
  in.remove_prefix(n);
  return true;
}

bool get_u8(std::string_view& in, uint8_t* v) {
  if (in.empty()) return false;
  *v = static_cast<uint8_t>(in[0]);
  in.remove_prefix(1);
  return true;
}

// Length of the encoded value at the start of s, or 0 if malformed.
size_t encoded_length(IndexFieldType type, std::string_view s) {
  if (type == IndexFieldType::Numeric) return s.size() >= 8 ? 8 : 0;
  for (size_t i = 0; i + 1 < s.size(); ++i) {
    if (s[i] != '\0') continue;
    if (s[i + 1] == '\x01') return i + 2;  // terminator
    ++i;                                   // escaped zero byte
  }
  return 0;
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

void add_unique(std::vector<std::string>* out, std::string v) {
  for (const auto& x : *out) {
    if (x == v) return;
  }
  out->push_back(std::move(v));
}

}  // namespace

std::string_view index_field_type_name(IndexFieldType t) { return t == IndexFieldType::Numeric ? "NUMERIC" : "TAG"; }

std::string_view index_state_name(IndexState s) {
  switch (s) {
    case IndexState::Building: return "building";
    case IndexState::Ready: return "ready";
    case IndexState::Failed: return "failed";
  }
  return "?";
}

bool IndexDef::matches(std::string_view key) const {
  if (prefixes.empty()) return true;
  for (const auto& p : prefixes) {
    if (key.starts_with(p)) return true;
  }
  return false;
}

bool IndexDef::multi_valued() const {
  if (json()) return true;
  for (const auto& f : fields) {
    if (f.separator != 0) return true;
  }
  return false;
}

int IndexDef::field_index(std::string_view field) const {
  for (size_t i = 0; i < fields.size(); ++i) {
    if (fields[i].name == field) return static_cast<int>(i);
  }
  return -1;
}

std::string IndexDef::compile() {
  paths_.clear();
  if (!json()) return {};
  for (const auto& f : fields) {
    std::string why;
    auto p = JsonPath::parse(f.path, &why);
    if (!p) return why;
    paths_.push_back(std::move(*p));
  }
  return {};
}

std::string IndexDef::normalize(size_t field, std::string_view value) const {
  if (field >= fields.size() || fields[field].type != IndexFieldType::Tag || fields[field].case_sensitive) {
    return std::string(value);
  }
  return to_lower(value);
}

void IndexDef::split_values(size_t field, std::string_view raw, std::vector<std::string>* out) const {
  const Field& f = fields[field];
  if (f.type == IndexFieldType::Numeric) {
    add_unique(out, std::string(trim(raw)));
    return;
  }
  if (f.separator == 0) {
    add_unique(out, normalize(field, raw));
    return;
  }
  for (;;) {
    size_t at = raw.find(f.separator);
    std::string_view piece = trim(raw.substr(0, at));
    if (!piece.empty()) add_unique(out, normalize(field, piece));
    if (at == std::string_view::npos) break;
    raw.remove_prefix(at + 1);
  }
}

std::vector<std::string> IndexDef::json_values(size_t field, const Json& doc) const {
  std::vector<std::string> out;
  if (field >= paths_.size()) return out;
  auto add = [&](const Json& v) {
    if (v.is_string()) split_values(field, v.get_ref<const std::string&>(), &out);
    else if (v.is_number() || v.is_boolean()) split_values(field, dump_json(v), &out);
  };
  for (const Json* v : paths_[field].select(doc)) {
    if (v->is_array()) {
      for (const auto& e : *v) add(e);
    } else {
      add(*v);
    }
  }
  return out;
}

IndexDef::Tuples IndexDef::make_tuples(const std::vector<std::vector<std::string>>& values) const {
  Tuples out;
  if (search()) {
    out.emplace_back(1, kPresence);
    for (size_t i = 0; i < fields.size() && i < values.size(); ++i) {
      for (const auto& v : values[i]) {
        std::string t(1, static_cast<char>(i));
        if (encode_index_value(fields[i].type, v, &t)) out.push_back(std::move(t));
      }
    }
    if (out.size() > kMaxTuples) out.resize(kMaxTuples);
  } else {
    out.emplace_back();
    for (size_t i = 0; i < fields.size(); ++i) {
      std::vector<std::string> encoded;
      if (i < values.size()) {
        for (const auto& v : values[i]) {
          std::string e;
          if (encode_index_value(fields[i].type, v, &e)) encoded.push_back(std::move(e));
        }
      }
      if (encoded.empty() || out.size() * encoded.size() > kMaxTuples) return {};
      Tuples next;
      next.reserve(out.size() * encoded.size());
      for (const auto& head : out) {
        for (const auto& e : encoded) next.push_back(head + e);
      }
      out = std::move(next);
    }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

IndexDef::Tuples IndexDef::tuples(
    const std::function<std::optional<std::string>(std::string_view)>& value_of) const {
  std::vector<std::vector<std::string>> values(fields.size());
  for (size_t i = 0; i < fields.size(); ++i) {
    auto v = value_of(hash_field(i));
    if (v) split_values(i, *v, &values[i]);
    else if (!search()) return {};
  }
  return make_tuples(values);
}

IndexDef::Tuples IndexDef::json_tuples(const Json& doc) const {
  std::vector<std::vector<std::string>> values(fields.size());
  for (size_t i = 0; i < fields.size(); ++i) {
    values[i] = json_values(i, doc);
    if (values[i].empty() && !search()) return {};
  }
  return make_tuples(values);
}

bool contains_tuple(const IndexDef::Tuples& tuples, std::string_view tuple) {
  return std::binary_search(tuples.begin(), tuples.end(), tuple, std::less<>());
}

std::string IndexDef::encode() const {
  std::string out;
  out.push_back(static_cast<char>(kDefFormat));
  out.push_back(static_cast<char>(source));
  out.push_back(static_cast<char>(kind));
  put_u32(out, id);
  put_str(out, name);
  put_u32(out, static_cast<uint32_t>(prefixes.size()));
  for (const auto& p : prefixes) put_str(out, p);
  put_u32(out, static_cast<uint32_t>(fields.size()));
  for (const auto& f : fields) {
    put_str(out, f.name);
    out.push_back(static_cast<char>(f.type));
    put_str(out, f.path);
    out.push_back(f.separator);
    out.push_back(static_cast<char>(f.case_sensitive ? 0 : kFieldCaseFolded));
  }
  out.push_back(unique ? 1 : 0);
  out.push_back(static_cast<char>(state));
  put_str(out, error);
  return out;
}

bool IndexDef::decode(std::string_view in, IndexDef* d) {
  uint8_t format, u8;
  uint32_t n;
  if (!get_u8(in, &format) || format < 1 || format > kDefFormat) return false;
  d->source = IndexSource::Hash;
  if (format >= 2) {
    if (!get_u8(in, &u8) || u8 > 1) return false;
    d->source = static_cast<IndexSource>(u8);
  }
  d->kind = IndexKind::Compound;
  if (format >= 3) {
    if (!get_u8(in, &u8) || u8 > 1) return false;
    d->kind = static_cast<IndexKind>(u8);
  }
  if (!get_u32(in, &d->id) || !get_str(in, &d->name) || !get_u32(in, &n)) return false;
  d->prefixes.assign(n, {});
  for (auto& p : d->prefixes) {
    if (!get_str(in, &p)) return false;
  }
  if (!get_u32(in, &n) || n > kMaxSearchFields) return false;
  d->fields.assign(n, {});
  for (auto& f : d->fields) {
    if (!get_str(in, &f.name) || !get_u8(in, &u8) || u8 > 1) return false;
    f.type = static_cast<IndexFieldType>(u8);
    if (format >= 2 && !get_str(in, &f.path)) return false;
    if (format >= 3) {
      uint8_t sep, flags;
      if (!get_u8(in, &sep) || !get_u8(in, &flags)) return false;
      f.separator = static_cast<char>(sep);
      f.case_sensitive = (flags & kFieldCaseFolded) == 0;
    }
  }
  if (!get_u8(in, &u8)) return false;
  d->unique = u8 != 0;
  if (!get_u8(in, &u8) || u8 > 2) return false;
  d->state = static_cast<IndexState>(u8);
  return get_str(in, &d->error) && in.empty() && d->compile().empty();
}

std::string IndexDef::id_prefix() const { return encode_index_id(id); }

std::string IndexDef::field_prefix(size_t field) const {
  std::string k = id_prefix();
  k.push_back(static_cast<char>(field));
  return k;
}

std::string IndexDef::presence_prefix() const {
  std::string k = id_prefix();
  k.push_back(kPresence);
  return k;
}

std::string IndexDef::entry_key(std::string_view tuple, std::string_view owner) const {
  std::string k = id_prefix();
  k.append(tuple);
  if (!unique) k.append(owner);
  return k;
}

std::string IndexDef::entry_value(uint64_t version, int64_t expire_at_ms, std::string_view owner) const {
  std::string v;
  append_be64(v, version);
  append_be64(v, static_cast<uint64_t>(expire_at_ms));
  if (unique) v.append(owner);
  return v;
}

bool IndexDef::split_entry(std::string_view key, std::string_view value, Entry* out) const {
  if (key.size() < 4 || value.size() < 16) return false;
  std::string_view rest = key.substr(4);
  size_t len = 0;
  out->field = -1;
  if (search()) {
    if (rest.empty()) return false;
    auto b = static_cast<uint8_t>(rest[0]);
    len = 1;
    if (b != static_cast<uint8_t>(kPresence)) {
      if (b >= fields.size()) return false;
      size_t n = encoded_length(fields[b].type, rest.substr(1));
      if (n == 0) return false;
      len += n;
      out->field = b;
    }
  } else {
    for (const auto& f : fields) {
      size_t n = encoded_length(f.type, rest.substr(len));
      if (n == 0) return false;
      len += n;
    }
  }
  out->tuple = rest.substr(0, len);
  out->version = read_be64(value);
  out->expire_at_ms = static_cast<int64_t>(read_be64(value.substr(8)));
  out->owner = unique ? value.substr(16) : rest.substr(len);
  return true;
}

bool encode_index_value(IndexFieldType type, std::string_view value, std::string* out) {
  if (type == IndexFieldType::Numeric) {
    auto d = parse_double(value);
    if (!d) return false;
    append_be64(*out, sortable_score(*d));
    return true;
  }
  for (char c : value) {
    out->push_back(c);
    if (c == '\0') out->push_back('\xff');
  }
  out->push_back('\0');
  out->push_back('\x01');
  return true;
}

std::string tag_prefix(std::string_view value) {
  std::string out;
  encode_index_value(IndexFieldType::Tag, value, &out);
  out.resize(out.size() - 2);
  return out;
}

bool decode_tag(std::string_view s, std::string* out) {
  out->clear();
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '\0') {
      out->push_back(s[i]);
      continue;
    }
    if (i + 1 >= s.size()) return false;
    if (s[i + 1] == '\x01') return true;
    out->push_back('\0');
    ++i;
  }
  return false;
}

std::string tag_bound(std::string_view value, bool after) {
  std::string out;
  encode_index_value(IndexFieldType::Tag, value, &out);
  if (after) out.back() = '\x02';
  return out;
}

std::string prefix_end(std::string_view prefix) {
  std::string s(prefix);
  while (!s.empty() && static_cast<uint8_t>(s.back()) == 0xFF) s.pop_back();
  if (s.empty()) return s;
  s.back() = static_cast<char>(static_cast<uint8_t>(s.back()) + 1);
  return s;
}

std::string encode_index_id(uint32_t id) {
  std::string out;
  put_u32(out, id);
  return out;
}

uint32_t decode_index_id(std::string_view key) {
  uint32_t v = 0;
  get_u32(key, &v);
  return v;
}

}  // namespace datadam
