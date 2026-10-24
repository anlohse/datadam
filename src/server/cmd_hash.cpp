// Hash commands. Each field is one data entry: sub-key = field, value = value.

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>
#include <random>

#include "server/command.h"
#include "util/strings.h"

namespace datadam {

namespace {

using Txn = Database::KeyTxn;
constexpr ValueType kHash = ValueType::Hash;

using Pairs = std::vector<std::pair<std::string, std::string>>;

Pairs all_fields(Txn& t) {
  Pairs out;
  out.reserve(t.size());
  t.scan("", "", false, [&](std::string_view f, std::string_view v) {
    out.emplace_back(f, v);
    return true;
  });
  return out;
}

void reply_pairs_as_map(Cmd& c, const Pairs& pairs) {
  c.r.map(pairs.size());
  for (const auto& [f, v] : pairs) {
    c.r.bulk(f);
    c.r.bulk(v);
  }
}

// HSET key field value [field value ...] (HMSET replies OK instead of a count)
void hset_generic(Cmd& c, bool hmset) {
  if (c.args.size() % 2 != 0) {
    return c.r.error(std::format("ERR wrong number of arguments for '{}' command", hmset ? "hmset" : "hset"));
  }
  int64_t added = 0;
  bool ok = with_collection(c, c.args[1], kHash, [&](Txn& t) {
    t.create(kHash);
    for (size_t i = 2; i < c.args.size(); i += 2) {
      if (!t.get(c.args[i])) {
        ++added;
        ++t.meta().size;
      }
      t.put(c.args[i], c.args[i + 1]);
    }
  });
  if (!ok) return;
  if (hmset) c.r.ok();
  else c.r.integer(added);
}

void cmd_hset(Cmd& c) { hset_generic(c, false); }
void cmd_hmset(Cmd& c) { hset_generic(c, true); }

void cmd_hsetnx(Cmd& c) {
  bool added = false;
  if (!with_collection(c, c.args[1], kHash, [&](Txn& t) {
        if (t.exists() && t.get(c.args[2])) return;
        t.create(kHash);
        t.put(c.args[2], c.args[3]);
        ++t.meta().size;
        added = true;
      })) {
    return;
  }
  c.r.integer(added ? 1 : 0);
}

void cmd_hget(Cmd& c) {
  std::optional<std::string> v;
  if (!read_collection(c, c.args[1], kHash, true, [&](Txn& t) {
        if (t.exists()) v = t.get(c.args[2]);
      })) {
    return;
  }
  v ? c.r.bulk(*v) : c.r.null();
}

void cmd_hmget(Cmd& c) {
  std::vector<std::optional<std::string>> values(c.args.size() - 2);
  if (!read_collection(c, c.args[1], kHash, true, [&](Txn& t) {
        if (!t.exists()) return;
        for (size_t i = 2; i < c.args.size(); ++i) values[i - 2] = t.get(c.args[i]);
      })) {
    return;
  }
  c.r.array(values.size());
  for (const auto& v : values) v ? c.r.bulk(*v) : c.r.null();
}

void cmd_hdel(Cmd& c) {
  int64_t removed = 0;
  if (!with_collection(c, c.args[1], kHash, [&](Txn& t) {
        if (!t.exists()) return;
        for (size_t i = 2; i < c.args.size(); ++i) {
          if (t.get(c.args[i])) {
            t.del(c.args[i]);
            --t.meta().size;
            ++removed;
          }
        }
      })) {
    return;
  }
  c.r.integer(removed);
}

void cmd_hexists(Cmd& c) {
  bool found = false;
  if (!read_collection(c, c.args[1], kHash, true, [&](Txn& t) { found = t.exists() && t.get(c.args[2]); })) return;
  c.r.integer(found ? 1 : 0);
}

void cmd_hlen(Cmd& c) {
  uint64_t n = 0;
  if (!read_collection(c, c.args[1], kHash, false, [&](Txn& t) { n = t.size(); })) return;
  c.r.integer(static_cast<int64_t>(n));
}

void cmd_hstrlen(Cmd& c) {
  size_t n = 0;
  if (!read_collection(c, c.args[1], kHash, true, [&](Txn& t) {
        if (!t.exists()) return;
        if (auto v = t.get(c.args[2])) n = v->size();
      })) {
    return;
  }
  c.r.integer(static_cast<int64_t>(n));
}

void cmd_hgetall(Cmd& c) {
  Pairs pairs;
  if (!read_collection(c, c.args[1], kHash, true, [&](Txn& t) { pairs = all_fields(t); })) return;
  reply_pairs_as_map(c, pairs);
}

void hkeys_or_vals(Cmd& c, bool keys) {
  Pairs pairs;
  if (!read_collection(c, c.args[1], kHash, true, [&](Txn& t) { pairs = all_fields(t); })) return;
  c.r.array(pairs.size());
  for (const auto& [f, v] : pairs) c.r.bulk(keys ? f : v);
}

void cmd_hkeys(Cmd& c) { hkeys_or_vals(c, true); }
void cmd_hvals(Cmd& c) { hkeys_or_vals(c, false); }

void cmd_hincrby(Cmd& c) {
  auto delta = parse_int64(c.args[3]);
  if (!delta) return c.r.error(kNotInteger);
  std::optional<std::string_view> error;
  int64_t result = 0;
  if (!with_collection(c, c.args[1], kHash, [&](Txn& t) {
        int64_t cur = 0;
        std::optional<std::string> old = t.exists() ? t.get(c.args[2]) : std::nullopt;
        if (old) {
          auto v = parse_int64(*old);
          if (!v) {
            error = "ERR hash value is not an integer";
            return;
          }
          cur = *v;
        }
        if ((*delta > 0 && cur > INT64_MAX - *delta) || (*delta < 0 && cur < INT64_MIN - *delta)) {
          error = "ERR increment or decrement would overflow";
          return;
        }
        result = cur + *delta;
        t.create(kHash);
        if (!old) ++t.meta().size;
        t.put(c.args[2], std::to_string(result));
      })) {
    return;
  }
  if (error) return c.r.error(*error);
  c.r.integer(result);
}

void cmd_hincrbyfloat(Cmd& c) {
  auto delta = parse_long_double(c.args[3]);
  if (!delta) return c.r.error(kNotFloat);
  std::optional<std::string_view> error;
  std::string result;
  if (!with_collection(c, c.args[1], kHash, [&](Txn& t) {
        long double cur = 0;
        std::optional<std::string> old = t.exists() ? t.get(c.args[2]) : std::nullopt;
        if (old) {
          auto v = parse_long_double(*old);
          if (!v) {
            error = "ERR hash value is not a float";
            return;
          }
          cur = *v;
        }
        long double sum = cur + *delta;
        if (std::isnan(sum) || std::isinf(sum)) {
          error = "ERR increment would produce NaN or Infinity";
          return;
        }
        result = format_long_double(sum);
        t.create(kHash);
        if (!old) ++t.meta().size;
        t.put(c.args[2], result);
      })) {
    return;
  }
  if (error) return c.r.error(*error);
  c.r.bulk(result);
}

// HRANDFIELD key [count [WITHVALUES]]
void cmd_hrandfield(Cmd& c) {
  std::optional<int64_t> count;
  bool with_values = false;
  if (c.args.size() >= 3) {
    count = parse_int64(c.args[2]);
    if (!count) return c.r.error(kNotInteger);
    if (c.args.size() == 4 && iequals(c.args[3], "withvalues")) with_values = true;
    else if (c.args.size() > 3) return c.r.error(kSyntax);
  }
  Pairs fields;
  if (!read_collection(c, c.args[1], kHash, true, [&](Txn& t) { fields = all_fields(t); })) return;

  thread_local std::mt19937_64 rng{std::random_device{}()};
  if (!count) {
    if (fields.empty()) return c.r.null();
    return c.r.bulk(fields[rng() % fields.size()].first);
  }
  Pairs picked;
  if (*count >= 0) {
    // Distinct fields, in random order.
    std::shuffle(fields.begin(), fields.end(), rng);
    fields.resize(std::min<size_t>(fields.size(), static_cast<size_t>(*count)));
    picked = std::move(fields);
  } else if (!fields.empty()) {
    uint64_t n = *count == INT64_MIN ? static_cast<uint64_t>(INT64_MAX) : static_cast<uint64_t>(-*count);
    if (n > 1'000'000'000) return c.r.error(kMustBePositive);
    for (uint64_t i = 0; i < n; ++i) picked.push_back(fields[rng() % fields.size()]);
  }
  if (!with_values) {
    c.r.array(picked.size());
    for (const auto& p : picked) c.r.bulk(p.first);
  } else if (c.r.proto() >= 3) {
    c.r.array(picked.size());
    for (const auto& [f, v] : picked) {
      c.r.array(2);
      c.r.bulk(f);
      c.r.bulk(v);
    }
  } else {
    c.r.array(picked.size() * 2);
    for (const auto& [f, v] : picked) {
      c.r.bulk(f);
      c.r.bulk(v);
    }
  }
}

}  // namespace

CommandList hash_commands() {
  return {
      {"hset", {cmd_hset, -4}},
      {"hmset", {cmd_hmset, -4}},
      {"hsetnx", {cmd_hsetnx, 4}},
      {"hget", {cmd_hget, 3}},
      {"hmget", {cmd_hmget, -3}},
      {"hdel", {cmd_hdel, -3}},
      {"hexists", {cmd_hexists, 3}},
      {"hlen", {cmd_hlen, 2}},
      {"hstrlen", {cmd_hstrlen, 3}},
      {"hgetall", {cmd_hgetall, 2}},
      {"hkeys", {cmd_hkeys, 2}},
      {"hvals", {cmd_hvals, 2}},
      {"hincrby", {cmd_hincrby, 4}},
      {"hincrbyfloat", {cmd_hincrbyfloat, 4}},
      {"hrandfield", {cmd_hrandfield, -2}},
  };
}

}  // namespace datadam
