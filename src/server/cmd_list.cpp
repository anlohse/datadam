// List commands. Element i (0-based) is the data entry with sub-key
// big-endian u64 (meta.head + i). Pushing at the head decrements head, so both
// ends are O(1); inserting or removing in the middle rewrites the tail.

#include <algorithm>
#include <array>
#include <format>
#include <optional>

#include "server/command.h"
#include "storage/data_keys.h"
#include "util/strings.h"

namespace datadam {

namespace {

using Txn = Database::KeyTxn;
constexpr ValueType kList = ValueType::List;

using Elements = std::vector<std::string>;

std::string pos_sub(Txn& t, uint64_t i) { return encode_u64(t.meta().head + i); }

// Elements i in [from, to).
Elements read_range(Txn& t, uint64_t from, uint64_t to) {
  Elements out;
  if (from >= to) return out;
  out.reserve(to - from);
  t.scan(pos_sub(t, from), pos_sub(t, to), false, [&](std::string_view, std::string_view v) {
    out.emplace_back(v);
    return true;
  });
  return out;
}

Elements read_all(Txn& t) { return read_range(t, 0, t.size()); }

// Replaces the elements from position `from` on with `tail`.
void rewrite_tail(Txn& t, uint64_t from, const Elements& tail) {
  uint64_t old_size = t.size();
  for (uint64_t i = 0; i < tail.size(); ++i) t.put(pos_sub(t, from + i), tail[i]);
  for (uint64_t i = from + tail.size(); i < old_size; ++i) t.del(pos_sub(t, i));
  t.meta().size = from + tail.size();
}

void push_left(Txn& t, std::string_view v) {
  --t.meta().head;
  ++t.meta().size;
  t.put(pos_sub(t, 0), v);
}

void push_right(Txn& t, std::string_view v) {
  t.put(pos_sub(t, t.size()), v);
  ++t.meta().size;
}

std::string pop_left(Txn& t) {
  std::string sub = pos_sub(t, 0);
  std::string v = t.get(sub).value_or("");
  t.del(sub);
  ++t.meta().head;
  --t.meta().size;
  return v;
}

std::string pop_right(Txn& t) {
  std::string sub = pos_sub(t, t.size() - 1);
  std::string v = t.get(sub).value_or("");
  t.del(sub);
  --t.meta().size;
  return v;
}

// LPUSH / RPUSH / LPUSHX / RPUSHX key element [element ...]
void push_generic(Cmd& c, bool left, bool only_if_exists) {
  uint64_t len = 0;
  if (!with_collection(c, c.args[1], kList, [&](Txn& t) {
        if (only_if_exists && !t.exists()) return;
        t.create(kList);
        for (size_t i = 2; i < c.args.size(); ++i) left ? push_left(t, c.args[i]) : push_right(t, c.args[i]);
        len = t.size();
      })) {
    return;
  }
  c.r.integer(static_cast<int64_t>(len));
}

void cmd_lpush(Cmd& c) { push_generic(c, true, false); }
void cmd_rpush(Cmd& c) { push_generic(c, false, false); }
void cmd_lpushx(Cmd& c) { push_generic(c, true, true); }
void cmd_rpushx(Cmd& c) { push_generic(c, false, true); }

// LPOP / RPOP key [count]
void pop_generic(Cmd& c, bool left) {
  std::optional<int64_t> count;
  if (c.args.size() > 3) return c.r.error(kSyntax);
  if (c.args.size() == 3) {
    count = parse_int64(c.args[2]);
    if (!count || *count < 0) return c.r.error(kMustBePositive);
  }
  bool exists = false;
  Elements popped;
  if (!with_collection(c, c.args[1], kList, [&](Txn& t) {
        exists = t.exists();
        uint64_t n = std::min<uint64_t>(t.size(), count ? static_cast<uint64_t>(*count) : 1);
        for (uint64_t i = 0; i < n; ++i) popped.push_back(left ? pop_left(t) : pop_right(t));
      })) {
    return;
  }
  if (!count) return popped.empty() ? c.r.null() : c.r.bulk(popped[0]);
  if (!exists) return c.r.null_array();
  c.r.array(popped.size());
  for (const auto& v : popped) c.r.bulk(v);
}

void cmd_lpop(Cmd& c) { pop_generic(c, true); }
void cmd_rpop(Cmd& c) { pop_generic(c, false); }

void cmd_llen(Cmd& c) {
  uint64_t n = 0;
  if (!read_collection(c, c.args[1], kList, false, [&](Txn& t) { n = t.size(); })) return;
  c.r.integer(static_cast<int64_t>(n));
}

// Resolves a possibly negative index; false if out of range.
bool resolve_index(int64_t index, uint64_t size, uint64_t* out) {
  int64_t len = static_cast<int64_t>(size);
  if (index < 0) index += len;
  if (index < 0 || index >= len) return false;
  *out = static_cast<uint64_t>(index);
  return true;
}

void cmd_lindex(Cmd& c) {
  auto index = parse_int64(c.args[2]);
  if (!index) return c.r.error(kNotInteger);
  std::optional<std::string> v;
  if (!read_collection(c, c.args[1], kList, true, [&](Txn& t) {
        uint64_t i;
        if (resolve_index(*index, t.size(), &i)) v = t.get(pos_sub(t, i));
      })) {
    return;
  }
  v ? c.r.bulk(*v) : c.r.null();
}

void cmd_lset(Cmd& c) {
  auto index = parse_int64(c.args[2]);
  if (!index) return c.r.error(kNotInteger);
  std::optional<std::string_view> error;
  if (!with_collection(c, c.args[1], kList, [&](Txn& t) {
        uint64_t i;
        if (!t.exists()) error = "ERR no such key";
        else if (!resolve_index(*index, t.size(), &i)) error = "ERR index out of range";
        else t.put(pos_sub(t, i), c.args[3]);
      })) {
    return;
  }
  if (error) return c.r.error(*error);
  c.r.ok();
}

void cmd_lrange(Cmd& c) {
  auto start = parse_int64(c.args[2]), stop = parse_int64(c.args[3]);
  if (!start || !stop) return c.r.error(kNotInteger);
  Elements out;
  if (!read_collection(c, c.args[1], kList, true, [&](Txn& t) {
        int64_t s, e;
        if (normalize_range(*start, *stop, static_cast<int64_t>(t.size()), &s, &e)) {
          out = read_range(t, static_cast<uint64_t>(s), static_cast<uint64_t>(e) + 1);
        }
      })) {
    return;
  }
  c.r.array(out.size());
  for (const auto& v : out) c.r.bulk(v);
}

void cmd_ltrim(Cmd& c) {
  auto start = parse_int64(c.args[2]), stop = parse_int64(c.args[3]);
  if (!start || !stop) return c.r.error(kNotInteger);
  if (!with_collection(c, c.args[1], kList, [&](Txn& t) {
        if (!t.exists()) return;
        uint64_t size = t.size();
        int64_t s, e;
        if (!normalize_range(*start, *stop, static_cast<int64_t>(size), &s, &e)) {
          for (uint64_t i = 0; i < size; ++i) t.del(pos_sub(t, i));
          t.meta().size = 0;
          return;
        }
        for (uint64_t i = static_cast<uint64_t>(e) + 1; i < size; ++i) t.del(pos_sub(t, i));
        for (uint64_t i = 0; i < static_cast<uint64_t>(s); ++i) t.del(pos_sub(t, i));
        t.meta().head += static_cast<uint64_t>(s);
        t.meta().size = static_cast<uint64_t>(e - s + 1);
      })) {
    return;
  }
  c.r.ok();
}

// LREM key count element
void cmd_lrem(Cmd& c) {
  auto count = parse_int64(c.args[2]);
  if (!count) return c.r.error(kNotInteger);
  std::string_view element = c.args[3];
  int64_t removed = 0;
  if (!with_collection(c, c.args[1], kList, [&](Txn& t) {
        if (!t.exists()) return;
        Elements all = read_all(t);
        std::vector<bool> drop(all.size(), false);
        int64_t limit = *count == 0 || *count == INT64_MIN ? INT64_MAX : (*count > 0 ? *count : -*count);
        if (*count >= 0) {
          for (size_t i = 0; i < all.size() && removed < limit; ++i) {
            if (all[i] == element) drop[i] = true, ++removed;
          }
        } else {
          for (size_t i = all.size(); i-- > 0 && removed < limit;) {
            if (all[i] == element) drop[i] = true, ++removed;
          }
        }
        if (removed == 0) return;
        size_t first = 0;
        while (!drop[first]) ++first;
        Elements tail;
        for (size_t i = first; i < all.size(); ++i) {
          if (!drop[i]) tail.push_back(std::move(all[i]));
        }
        rewrite_tail(t, first, tail);
      })) {
    return;
  }
  c.r.integer(removed);
}

// LINSERT key BEFORE|AFTER pivot element
void cmd_linsert(Cmd& c) {
  std::string where = to_lower(c.args[2]);
  if (where != "before" && where != "after") return c.r.error(kSyntax);
  int64_t result = 0;
  if (!with_collection(c, c.args[1], kList, [&](Txn& t) {
        if (!t.exists()) return;
        Elements all = read_all(t);
        auto it = std::find(all.begin(), all.end(), c.args[3]);
        if (it == all.end()) {
          result = -1;
          return;
        }
        size_t at = static_cast<size_t>(it - all.begin()) + (where == "after" ? 1 : 0);
        if (at == 0) {
          push_left(t, c.args[4]);
        } else {
          Elements tail(all.begin() + static_cast<ptrdiff_t>(at), all.end());
          tail.insert(tail.begin(), std::string(c.args[4]));
          rewrite_tail(t, at, tail);
        }
        result = static_cast<int64_t>(t.size());
      })) {
    return;
  }
  c.r.integer(result);
}

// LPOS key element [RANK rank] [COUNT num-matches] [MAXLEN len]
void cmd_lpos(Cmd& c) {
  int64_t rank = 1, maxlen = 0;
  std::optional<int64_t> count;
  for (size_t i = 3; i < c.args.size(); i += 2) {
    std::string opt = to_lower(c.args[i]);
    if (i + 1 >= c.args.size()) return c.r.error(kSyntax);
    auto v = parse_int64(c.args[i + 1]);
    if (!v) return c.r.error(kNotInteger);
    if (opt == "rank") {
      if (*v == 0 || *v == INT64_MIN) {
        return c.r.error(
            "ERR RANK can't be zero: use 1 to start from the first match, 2 from the second ... "
            "or use negative to start from the end of the list");
      }
      rank = *v;
    } else if (opt == "count") {
      if (*v < 0) return c.r.error("ERR COUNT can't be negative");
      count = *v;
    } else if (opt == "maxlen") {
      if (*v < 0) return c.r.error("ERR MAXLEN can't be negative");
      maxlen = *v;
    } else {
      return c.r.error(kSyntax);
    }
  }
  std::vector<int64_t> found;
  if (!read_collection(c, c.args[1], kList, true, [&](Txn& t) {
        if (!t.exists()) return;
        bool rev = rank < 0;
        int64_t skip = (rev ? -rank : rank) - 1;
        int64_t want = count ? (*count == 0 ? INT64_MAX : *count) : 1;
        int64_t size = static_cast<int64_t>(t.size()), seen = 0;
        int64_t pos = rev ? size - 1 : 0;
        t.scan(pos_sub(t, 0), pos_sub(t, t.size()), rev, [&](std::string_view, std::string_view v) {
          if (maxlen > 0 && seen++ >= maxlen) return false;
          if (v == c.args[2]) {
            if (skip > 0) --skip;
            else found.push_back(pos);
          }
          pos += rev ? -1 : 1;
          return static_cast<int64_t>(found.size()) < want;
        });
      })) {
    return;
  }
  if (!count) return found.empty() ? c.r.null() : c.r.integer(found[0]);
  c.r.array(found.size());
  for (int64_t p : found) c.r.integer(p);
}

// Moves one element from src to dst; used by LMOVE and RPOPLPUSH, and with
// a timeout by BLMOVE and BRPOPLPUSH (which wait for src to get one).
void move_generic(Cmd& c, bool from_left, bool to_left, std::optional<double> timeout = std::nullopt) {
  std::array<std::string_view, 2> keys{c.args[1], c.args[2]};
  bool wrong = false, blocked = false;
  std::optional<std::string> moved;
  c.db().transact_many(keys, [&](Database::KeyTxns& txns) {
    Txn &src = txns[0], &dst = txns[1];
    if (src.wrong_type(kList) || dst.wrong_type(kList)) {
      wrong = true;
      return;
    }
    if (src.size() == 0) {
      if (timeout) blocked = block_for_keys(c, std::span(keys.data(), 1), *timeout);
      return;
    }
    moved = from_left ? pop_left(src) : pop_right(src);
    dst.create(kList);
    to_left ? push_left(dst, *moved) : push_right(dst, *moved);
  });
  if (wrong) return c.r.error(kWrongType);
  if (blocked) return;
  moved ? c.r.bulk(*moved) : c.r.null();
}

bool parse_side(std::string_view s, bool* left) {
  if (iequals(s, "left")) *left = true;
  else if (iequals(s, "right")) *left = false;
  else return false;
  return true;
}

void cmd_lmove(Cmd& c) {
  bool from_left, to_left;
  if (!parse_side(c.args[3], &from_left) || !parse_side(c.args[4], &to_left)) return c.r.error(kSyntax);
  move_generic(c, from_left, to_left);
}

void cmd_rpoplpush(Cmd& c) { move_generic(c, false, true); }

// BLMOVE source destination LEFT|RIGHT LEFT|RIGHT timeout
void cmd_blmove(Cmd& c) {
  bool from_left, to_left;
  if (!parse_side(c.args[3], &from_left) || !parse_side(c.args[4], &to_left)) return c.r.error(kSyntax);
  auto timeout = parse_block_timeout(c, c.args[5]);
  if (!timeout) return;
  move_generic(c, from_left, to_left, timeout);
}

// BRPOPLPUSH source destination timeout
void cmd_brpoplpush(Cmd& c) {
  auto timeout = parse_block_timeout(c, c.args[3]);
  if (!timeout) return;
  move_generic(c, false, true, timeout);
}

// BLPOP / BRPOP key [key ...] timeout: the first element of the first list
// that has one, as [key, element]; waits if none has.
void bpop_generic(Cmd& c, bool left) {
  auto timeout = parse_block_timeout(c, c.args.back());
  if (!timeout) return;
  std::vector<std::string_view> keys(c.args.begin() + 1, c.args.end() - 1);
  bool wrong = false, blocked = false;
  std::optional<std::pair<std::string_view, std::string>> got;
  c.db().transact_many(keys, [&](Database::KeyTxns& txns) {
    for (size_t i = 0; i < keys.size(); ++i) {
      Txn& t = txns[i];
      if (t.wrong_type(kList)) {
        wrong = true;
        return;
      }
      if (t.size() > 0) {
        got.emplace(keys[i], left ? pop_left(t) : pop_right(t));
        return;
      }
    }
    blocked = block_for_keys(c, keys, *timeout);
  });
  if (wrong) return c.r.error(kWrongType);
  if (blocked) return;
  if (!got) return c.r.null_array();
  c.r.array(2);
  c.r.bulk(got->first);
  c.r.bulk(got->second);
}

void cmd_blpop(Cmd& c) { bpop_generic(c, true); }
void cmd_brpop(Cmd& c) { bpop_generic(c, false); }

// LMPOP numkeys key [key ...] LEFT|RIGHT [COUNT count], and BLMPOP with a
// timeout first: up to count elements of the first list that has any, as
// [key, [element ...]].
void mpop_generic(Cmd& c, size_t at, std::optional<double> timeout) {
  auto n = parse_int64(c.args[at]);
  if (!n || *n <= 0) return c.r.error("ERR numkeys should be greater than 0");
  size_t side_at = at + 1 + static_cast<size_t>(*n);
  if (side_at >= c.args.size()) return c.r.error(kSyntax);
  bool left;
  if (!parse_side(c.args[side_at], &left)) return c.r.error(kSyntax);
  int64_t count = 1;
  if (side_at + 1 < c.args.size()) {
    if (side_at + 3 != c.args.size() || !iequals(c.args[side_at + 1], "count")) return c.r.error(kSyntax);
    auto v = parse_int64(c.args[side_at + 2]);
    if (!v || *v <= 0) return c.r.error("ERR count should be greater than 0");
    count = *v;
  }
  std::vector<std::string_view> keys(c.args.begin() + static_cast<std::ptrdiff_t>(at + 1),
                                     c.args.begin() + static_cast<std::ptrdiff_t>(side_at));
  bool wrong = false, blocked = false;
  std::optional<std::string_view> from;
  Elements popped;
  c.db().transact_many(keys, [&](Database::KeyTxns& txns) {
    for (size_t i = 0; i < keys.size(); ++i) {
      Txn& t = txns[i];
      if (t.wrong_type(kList)) {
        wrong = true;
        return;
      }
      if (t.size() == 0) continue;
      uint64_t k = std::min<uint64_t>(t.size(), static_cast<uint64_t>(count));
      for (uint64_t j = 0; j < k; ++j) popped.push_back(left ? pop_left(t) : pop_right(t));
      from = keys[i];
      return;
    }
    if (timeout) blocked = block_for_keys(c, keys, *timeout);
  });
  if (wrong) return c.r.error(kWrongType);
  if (blocked) return;
  if (!from) return c.r.null_array();
  c.r.array(2);
  c.r.bulk(*from);
  c.r.array(popped.size());
  for (const auto& v : popped) c.r.bulk(v);
}

void cmd_lmpop(Cmd& c) { mpop_generic(c, 1, std::nullopt); }

void cmd_blmpop(Cmd& c) {
  auto timeout = parse_block_timeout(c, c.args[1]);
  if (!timeout) return;
  mpop_generic(c, 2, timeout);
}

}  // namespace

CommandList list_commands() {
  return {
      {"lpush", {cmd_lpush, -3}},
      {"rpush", {cmd_rpush, -3}},
      {"lpushx", {cmd_lpushx, -3}},
      {"rpushx", {cmd_rpushx, -3}},
      {"lpop", {cmd_lpop, -2}},
      {"rpop", {cmd_rpop, -2}},
      {"llen", {cmd_llen, 2}},
      {"lindex", {cmd_lindex, 3}},
      {"lset", {cmd_lset, 4}},
      {"lrange", {cmd_lrange, 4}},
      {"ltrim", {cmd_ltrim, 4}},
      {"lrem", {cmd_lrem, 4}},
      {"linsert", {cmd_linsert, 5}},
      {"lpos", {cmd_lpos, -3}},
      {"lmove", {cmd_lmove, 5}},
      {"rpoplpush", {cmd_rpoplpush, 3}},
      {"blpop", {cmd_blpop, -3}},
      {"brpop", {cmd_brpop, -3}},
      {"blmove", {cmd_blmove, 6}},
      {"brpoplpush", {cmd_brpoplpush, 4}},
      {"lmpop", {cmd_lmpop, -4}},
      {"blmpop", {cmd_blmpop, -5}},
  };
}

}  // namespace datadam
