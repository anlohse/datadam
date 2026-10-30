// Set commands. Each member is one data entry: sub-key = member, empty value.

#include <algorithm>
#include <array>
#include <format>
#include <iterator>
#include <optional>
#include <random>

#include "server/command.h"
#include "util/strings.h"

namespace datadam {

namespace {

using Txn = Database::KeyTxn;
constexpr ValueType kSet = ValueType::Set;

using Members = std::vector<std::string>;

// Sorted, since the data entries are.
Members all_members(Txn& t) {
  Members out;
  out.reserve(t.size());
  t.scan("", "", false, [&](std::string_view m, std::string_view) {
    out.emplace_back(m);
    return true;
  });
  return out;
}

bool has_member(Txn& t, std::string_view m) { return t.exists() && t.get(m).has_value(); }

void reply_set(Cmd& c, const Members& members) {
  c.r.set(members.size());
  for (const auto& m : members) c.r.bulk(m);
}

std::mt19937_64& rng() {
  thread_local std::mt19937_64 r{std::random_device{}()};
  return r;
}

void cmd_sadd(Cmd& c) {
  int64_t added = 0;
  if (!with_collection(c, c.args[1], kSet, [&](Txn& t) {
        t.create(kSet);
        for (size_t i = 2; i < c.args.size(); ++i) {
          if (t.get(c.args[i])) continue;
          t.put(c.args[i], "");
          ++t.meta().size;
          ++added;
        }
      })) {
    return;
  }
  c.r.integer(added);
}

void cmd_srem(Cmd& c) {
  int64_t removed = 0;
  if (!with_collection(c, c.args[1], kSet, [&](Txn& t) {
        for (size_t i = 2; i < c.args.size(); ++i) {
          if (!has_member(t, c.args[i])) continue;
          t.del(c.args[i]);
          --t.meta().size;
          ++removed;
        }
      })) {
    return;
  }
  c.r.integer(removed);
}

void cmd_sismember(Cmd& c) {
  bool found = false;
  if (!read_collection(c, c.args[1], kSet, true, [&](Txn& t) { found = has_member(t, c.args[2]); })) return;
  c.r.integer(found ? 1 : 0);
}

void cmd_smismember(Cmd& c) {
  std::vector<bool> found(c.args.size() - 2);
  if (!read_collection(c, c.args[1], kSet, true, [&](Txn& t) {
        for (size_t i = 2; i < c.args.size(); ++i) found[i - 2] = has_member(t, c.args[i]);
      })) {
    return;
  }
  c.r.array(found.size());
  for (bool f : found) c.r.integer(f ? 1 : 0);
}

void cmd_scard(Cmd& c) {
  uint64_t n = 0;
  if (!read_collection(c, c.args[1], kSet, false, [&](Txn& t) { n = t.size(); })) return;
  c.r.integer(static_cast<int64_t>(n));
}

void cmd_smembers(Cmd& c) {
  Members members;
  if (!read_collection(c, c.args[1], kSet, true, [&](Txn& t) { members = all_members(t); })) return;
  reply_set(c, members);
}

// SPOP key [count]
void cmd_spop(Cmd& c) {
  std::optional<int64_t> count;
  if (c.args.size() > 3) return c.r.error(kSyntax);
  if (c.args.size() == 3) {
    count = parse_int64(c.args[2]);
    if (!count || *count < 0) return c.r.error(kMustBePositive);
  }
  Members popped;
  if (!with_collection(c, c.args[1], kSet, [&](Txn& t) {
        if (!t.exists()) return;
        Members all = all_members(t);
        size_t n = count ? std::min<size_t>(all.size(), static_cast<size_t>(*count)) : 1;
        std::sample(all.begin(), all.end(), std::back_inserter(popped), n, rng());
        for (const auto& m : popped) t.del(m);
        t.meta().size -= popped.size();
      })) {
    return;
  }
  if (!count) return popped.empty() ? c.r.null() : c.r.bulk(popped[0]);
  std::shuffle(popped.begin(), popped.end(), rng());
  reply_set(c, popped);
}

// SRANDMEMBER key [count]
void cmd_srandmember(Cmd& c) {
  std::optional<int64_t> count;
  if (c.args.size() > 3) return c.r.error(kSyntax);
  if (c.args.size() == 3) {
    count = parse_int64(c.args[2]);
    if (!count) return c.r.error(kNotInteger);
  }
  Members all;
  if (!read_collection(c, c.args[1], kSet, true, [&](Txn& t) { all = all_members(t); })) return;
  if (!count) return all.empty() ? c.r.null() : c.r.bulk(all[rng()() % all.size()]);
  if (*count < 0) {
    // Members may repeat: written as they are picked, not copied first.
    uint64_t n = *count == INT64_MIN ? static_cast<uint64_t>(INT64_MAX) : static_cast<uint64_t>(-*count);
    if (n > 1'000'000'000) return c.r.error(kMustBePositive);
    if (all.empty()) n = 0;
    c.r.array(n);
    for (uint64_t i = 0; i < n; ++i) c.r.bulk(all[rng()() % all.size()]);
    return;
  }
  Members picked;
  std::sample(all.begin(), all.end(), std::back_inserter(picked), static_cast<size_t>(*count), rng());
  std::shuffle(picked.begin(), picked.end(), rng());
  c.r.array(picked.size());
  for (const auto& m : picked) c.r.bulk(m);
}

void cmd_smove(Cmd& c) {
  std::string_view src = c.args[1], dst = c.args[2], member = c.args[3];
  std::array<std::string_view, 2> keys{src, dst};
  bool wrong = false, moved = false;
  c.db().transact_many(keys, [&](Database::KeyTxns& txns) {
    Txn &s = txns[0], &d = txns[1];
    if (s.wrong_type(kSet) || d.wrong_type(kSet)) {
      wrong = true;
      return;
    }
    if (!has_member(s, member)) return;
    moved = true;
    if (src == dst) return;
    s.del(member);
    --s.meta().size;
    d.create(kSet);
    if (!d.get(member)) {
      d.put(member, "");
      ++d.meta().size;
    }
  });
  if (wrong) return c.r.error(kWrongType);
  c.r.integer(moved ? 1 : 0);
}

enum class SetOp { Inter, Union, Diff };

// Computes op over the sets in keys[first..]; false (WRONGTYPE) if one is not a set.
bool combine(Database::KeyTxns& txns, size_t first, SetOp op, Members* out) {
  for (size_t i = first; i < txns.size(); ++i) {
    if (txns[i].wrong_type(kSet)) return false;
  }
  for (size_t i = first; i < txns.size(); ++i) {
    Members m = all_members(txns[i]);
    if (i == first) {
      *out = std::move(m);
      continue;
    }
    Members r;
    switch (op) {
      case SetOp::Inter: std::set_intersection(out->begin(), out->end(), m.begin(), m.end(), std::back_inserter(r)); break;
      case SetOp::Union: std::set_union(out->begin(), out->end(), m.begin(), m.end(), std::back_inserter(r)); break;
      case SetOp::Diff: std::set_difference(out->begin(), out->end(), m.begin(), m.end(), std::back_inserter(r)); break;
    }
    *out = std::move(r);
    if (op != SetOp::Union && out->empty()) break;
  }
  return true;
}

// SINTER / SUNION / SDIFF key [key ...]
void set_op(Cmd& c, SetOp op) {
  std::vector<std::string_view> keys(c.args.begin() + 1, c.args.end());
  Members result;
  bool ok = true;
  c.db().transact_many(keys, [&](Database::KeyTxns& txns) { ok = combine(txns, 0, op, &result); });
  if (!ok) return c.r.error(kWrongType);
  reply_set(c, result);
}

void cmd_sinter(Cmd& c) { set_op(c, SetOp::Inter); }
void cmd_sunion(Cmd& c) { set_op(c, SetOp::Union); }
void cmd_sdiff(Cmd& c) { set_op(c, SetOp::Diff); }

// SINTERSTORE / SUNIONSTORE / SDIFFSTORE destination key [key ...]
void set_op_store(Cmd& c, SetOp op) {
  std::vector<std::string_view> keys(c.args.begin() + 1, c.args.end());
  size_t stored = 0;
  bool ok = true;
  c.db().transact_many(keys, [&](Database::KeyTxns& txns) {
    Members result;
    ok = combine(txns, 1, op, &result);
    if (!ok) return;
    Txn& dst = txns[0];
    dst.reset(kSet);  // overwrites any type; an empty result deletes the key
    for (const auto& m : result) dst.put(m, "");
    dst.meta().size = result.size();
    stored = result.size();
  });
  if (!ok) return c.r.error(kWrongType);
  c.r.integer(static_cast<int64_t>(stored));
}

void cmd_sinterstore(Cmd& c) { set_op_store(c, SetOp::Inter); }
void cmd_sunionstore(Cmd& c) { set_op_store(c, SetOp::Union); }
void cmd_sdiffstore(Cmd& c) { set_op_store(c, SetOp::Diff); }

// SINTERCARD numkeys key [key ...] [LIMIT limit]
void cmd_sintercard(Cmd& c) {
  auto numkeys = parse_int64(c.args[1]);
  if (!numkeys || *numkeys <= 0) return c.r.error("ERR numkeys should be greater than 0");
  if (static_cast<size_t>(*numkeys) > c.args.size() - 2) {
    return c.r.error("ERR Number of keys can't be greater than number of args");
  }
  size_t end = 2 + static_cast<size_t>(*numkeys);
  int64_t limit = 0;
  if (end < c.args.size()) {
    if (end + 2 != c.args.size() || !iequals(c.args[end], "limit")) return c.r.error(kSyntax);
    auto l = parse_int64(c.args[end + 1]);
    if (!l || *l < 0) return c.r.error("ERR LIMIT can't be negative");
    limit = *l;
  }
  std::vector<std::string_view> keys(c.args.begin() + 2, c.args.begin() + static_cast<ptrdiff_t>(end));
  Members result;
  bool ok = true;
  c.db().transact_many(keys, [&](Database::KeyTxns& txns) { ok = combine(txns, 0, SetOp::Inter, &result); });
  if (!ok) return c.r.error(kWrongType);
  size_t n = result.size();
  if (limit > 0) n = std::min<size_t>(n, static_cast<size_t>(limit));
  c.r.integer(static_cast<int64_t>(n));
}

}  // namespace

CommandList set_commands() {
  return {
      {"sadd", {cmd_sadd, -3}},
      {"srem", {cmd_srem, -3}},
      {"sismember", {cmd_sismember, 3}},
      {"smismember", {cmd_smismember, -3}},
      {"scard", {cmd_scard, 2}},
      {"smembers", {cmd_smembers, 2}},
      {"spop", {cmd_spop, -2}},
      {"srandmember", {cmd_srandmember, -2}},
      {"smove", {cmd_smove, 4}},
      {"sinter", {cmd_sinter, -2}},
      {"sunion", {cmd_sunion, -2}},
      {"sdiff", {cmd_sdiff, -2}},
      {"sinterstore", {cmd_sinterstore, -3}},
      {"sunionstore", {cmd_sunionstore, -3}},
      {"sdiffstore", {cmd_sdiffstore, -3}},
      {"sintercard", {cmd_sintercard, -3}},
  };
}

}  // namespace datadam
