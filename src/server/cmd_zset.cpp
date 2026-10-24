// Sorted set commands. Each member has two data entries:
//   'm' + member                     -> sortable score (8 bytes)   lookup by member, lex order
//   's' + sortable score + member    -> ""                         score order
// Rank-based operations walk the score entries (O(rank)).

#include <cmath>
#include <format>
#include <optional>

#include "server/command.h"
#include "storage/data_keys.h"
#include "util/strings.h"

namespace datadam {

namespace {

using Txn = Database::KeyTxn;
constexpr ValueType kZSet = ValueType::ZSet;

struct Entry {
  std::string member;
  double score;
};
using Entries = std::vector<Entry>;

std::string member_sub(std::string_view m) {
  std::string s = "m";
  s.append(m);
  return s;
}

std::string score_prefix(uint64_t sortable) {
  std::string s = "s";
  append_be64(s, sortable);
  return s;
}

std::string score_sub(double score, std::string_view m) {
  std::string s = score_prefix(sortable_score(score));
  s.append(m);
  return s;
}

// Upper bounds of the two sub-key spaces.
constexpr std::string_view kMemberEnd = "n";
constexpr std::string_view kScoreEnd = "t";

std::optional<double> score_of(Txn& t, std::string_view m) {
  if (!t.exists()) return std::nullopt;
  auto v = t.get(member_sub(m));
  if (!v || v->size() != 8) return std::nullopt;
  return score_from_sortable(read_be64(*v));
}

void set_score(Txn& t, std::string_view m, double score, std::optional<double> old) {
  if (old) t.del(score_sub(*old, m));
  else ++t.meta().size;
  t.put(member_sub(m), encode_u64(sortable_score(score)));
  t.put(score_sub(score, m), "");
}

void remove_member(Txn& t, std::string_view m, double score) {
  t.del(member_sub(m));
  t.del(score_sub(score, m));
  --t.meta().size;
}

Entry entry_from_score_sub(std::string_view sub) {
  return {std::string(sub.substr(9)), score_from_sortable(read_be64(sub.substr(1, 8)))};
}

void reply_score(Cmd& c, double s) { c.r.double_value(s); }

void reply_entries(Cmd& c, const Entries& entries, bool with_scores) {
  if (!with_scores) {
    c.r.array(entries.size());
    for (const auto& e : entries) c.r.bulk(e.member);
  } else if (c.r.proto() >= 3) {
    c.r.array(entries.size());
    for (const auto& e : entries) {
      c.r.array(2);
      c.r.bulk(e.member);
      reply_score(c, e.score);
    }
  } else {
    c.r.array(entries.size() * 2);
    for (const auto& e : entries) {
      c.r.bulk(e.member);
      reply_score(c, e.score);
    }
  }
}

// ---- ranges

// A score interval as sub-key bounds [lower, upper) of the score entries.
struct ScoreRange {
  std::string lower, upper;
  bool empty = false;
};

bool parse_score_bound(std::string_view s, double* v, bool* exclusive) {
  *exclusive = !s.empty() && s[0] == '(';
  if (*exclusive) s.remove_prefix(1);
  auto d = parse_double(s);
  if (!d) return false;
  *v = *d;
  return true;
}

std::optional<ScoreRange> parse_score_range(std::string_view min_s, std::string_view max_s) {
  double min, max;
  bool min_ex, max_ex;
  if (!parse_score_bound(min_s, &min, &min_ex) || !parse_score_bound(max_s, &max, &max_ex)) return std::nullopt;
  ScoreRange r;
  uint64_t lo = sortable_score(min) + (min_ex ? 1 : 0);
  uint64_t hi = sortable_score(max) + (max_ex ? 0 : 1);
  r.empty = min > max || lo >= hi;
  r.lower = score_prefix(lo);
  r.upper = score_prefix(hi);
  return r;
}

// A lex interval as sub-key bounds [lower, upper) of the member entries.
struct LexRange {
  std::string lower, upper;
};

bool parse_lex_bound(std::string_view s, bool is_max, std::string* out) {
  if (s == "-") {
    *out = "m";
    return true;
  }
  if (s == "+") {
    *out = std::string(kMemberEnd);
    return true;
  }
  if (s.empty() || (s[0] != '[' && s[0] != '(')) return false;
  bool inclusive = s[0] == '[';
  *out = member_sub(s.substr(1));
  // The first key after `m` is `m` followed by a zero byte.
  if (inclusive == is_max) out->push_back('\0');
  return true;
}

std::optional<LexRange> parse_lex_range(std::string_view min_s, std::string_view max_s) {
  LexRange r;
  if (!parse_lex_bound(min_s, false, &r.lower) || !parse_lex_bound(max_s, true, &r.upper)) return std::nullopt;
  if (min_s == "+" || max_s == "-") r.upper = r.lower;  // empty
  return r;
}

struct Limit {
  int64_t offset = 0;
  int64_t count = -1;  // < 0: all
};

Entries scan_scores(Txn& t, const ScoreRange& r, bool rev, Limit limit) {
  Entries out;
  if (r.empty || limit.offset < 0) return out;
  int64_t skip = limit.offset;
  t.scan(r.lower, r.upper, rev, [&](std::string_view sub, std::string_view) {
    if (skip > 0) {
      --skip;
      return true;
    }
    if (limit.count >= 0 && static_cast<int64_t>(out.size()) >= limit.count) return false;
    out.push_back(entry_from_score_sub(sub));
    return true;
  });
  return out;
}

Entries scan_lex(Txn& t, const LexRange& r, bool rev, Limit limit) {
  Entries out;
  if (limit.offset < 0 || r.lower >= r.upper) return out;
  int64_t skip = limit.offset;
  t.scan(r.lower, r.upper, rev, [&](std::string_view sub, std::string_view v) {
    if (skip > 0) {
      --skip;
      return true;
    }
    if (limit.count >= 0 && static_cast<int64_t>(out.size()) >= limit.count) return false;
    out.push_back({std::string(sub.substr(1)), score_from_sortable(read_be64(v))});
    return true;
  });
  return out;
}

// Entries with rank start..stop (inclusive, already normalized), from the
// lowest score or, with rev, from the highest.
Entries scan_ranks(Txn& t, int64_t start, int64_t stop, bool rev) {
  Entries out;
  int64_t rank = 0;
  t.scan("s", kScoreEnd, rev, [&](std::string_view sub, std::string_view) {
    if (rank > stop) return false;
    if (rank >= start) out.push_back(entry_from_score_sub(sub));
    ++rank;
    return true;
  });
  return out;
}

// ---- commands

// ZADD key [NX|XX] [GT|LT] [CH] [INCR] score member [score member ...]
void cmd_zadd(Cmd& c) {
  bool nx = false, xx = false, gt = false, lt = false, ch = false, incr = false;
  size_t i = 2;
  for (; i < c.args.size(); ++i) {
    std::string opt = to_lower(c.args[i]);
    if (opt == "nx") nx = true;
    else if (opt == "xx") xx = true;
    else if (opt == "gt") gt = true;
    else if (opt == "lt") lt = true;
    else if (opt == "ch") ch = true;
    else if (opt == "incr") incr = true;
    else break;
  }
  size_t pairs_start = i;
  if (pairs_start >= c.args.size() || (c.args.size() - pairs_start) % 2 != 0) return c.r.error(kSyntax);
  if (nx && xx) return c.r.error("ERR XX and NX options at the same time are not compatible");
  if ((gt && lt) || (nx && (gt || lt))) {
    return c.r.error("ERR GT, LT, and/or NX options at the same time are not compatible");
  }
  if (incr && c.args.size() - pairs_start != 2) {
    return c.r.error("ERR INCR option supports a single increment-element pair");
  }
  std::vector<double> scores;
  for (size_t j = pairs_start; j < c.args.size(); j += 2) {
    auto s = parse_double(c.args[j]);
    if (!s) return c.r.error(kNotFloat);
    scores.push_back(*s);
  }

  int64_t added = 0, changed = 0;
  std::optional<double> incr_result;
  std::optional<std::string_view> error;
  if (!with_collection(c, c.args[1], kZSet, [&](Txn& t) {
        for (size_t j = pairs_start, k = 0; j < c.args.size(); j += 2, ++k) {
          std::string_view member = c.args[j + 1];
          std::optional<double> old = score_of(t, member);
          double score = scores[k];
          if (incr && old) score += *old;
          if (std::isnan(score)) {
            error = "ERR resulting score is not a number (NaN)";
            return;
          }
          if ((nx && old) || (xx && !old)) continue;
          if (old && ((gt && score <= *old) || (lt && score >= *old))) continue;
          if (incr) incr_result = score;
          if (old && *old == score) continue;
          t.create(kZSet);
          set_score(t, member, score, old);
          ++changed;
          if (!old) ++added;
        }
      })) {
    return;
  }
  if (error) return c.r.error(*error);
  if (incr) return incr_result ? reply_score(c, *incr_result) : c.r.null();
  c.r.integer(ch ? changed : added);
}

void cmd_zincrby(Cmd& c) {
  auto delta = parse_double(c.args[2]);
  if (!delta) return c.r.error(kNotFloat);
  double result = 0;
  std::optional<std::string_view> error;
  if (!with_collection(c, c.args[1], kZSet, [&](Txn& t) {
        std::optional<double> old = score_of(t, c.args[3]);
        result = (old ? *old : 0) + *delta;
        if (std::isnan(result)) {
          error = "ERR resulting score is not a number (NaN)";
          return;
        }
        t.create(kZSet);
        set_score(t, c.args[3], result, old);
      })) {
    return;
  }
  if (error) return c.r.error(*error);
  reply_score(c, result);
}

void cmd_zrem(Cmd& c) {
  int64_t removed = 0;
  if (!with_collection(c, c.args[1], kZSet, [&](Txn& t) {
        for (size_t i = 2; i < c.args.size(); ++i) {
          if (auto old = score_of(t, c.args[i])) {
            remove_member(t, c.args[i], *old);
            ++removed;
          }
        }
      })) {
    return;
  }
  c.r.integer(removed);
}

void cmd_zscore(Cmd& c) {
  std::optional<double> score;
  if (!read_collection(c, c.args[1], kZSet, true, [&](Txn& t) { score = score_of(t, c.args[2]); })) return;
  score ? reply_score(c, *score) : c.r.null();
}

void cmd_zmscore(Cmd& c) {
  std::vector<std::optional<double>> scores(c.args.size() - 2);
  if (!read_collection(c, c.args[1], kZSet, true, [&](Txn& t) {
        for (size_t i = 2; i < c.args.size(); ++i) scores[i - 2] = score_of(t, c.args[i]);
      })) {
    return;
  }
  c.r.array(scores.size());
  for (const auto& s : scores) s ? reply_score(c, *s) : c.r.null();
}

void cmd_zcard(Cmd& c) {
  uint64_t n = 0;
  if (!read_collection(c, c.args[1], kZSet, false, [&](Txn& t) { n = t.size(); })) return;
  c.r.integer(static_cast<int64_t>(n));
}

void cmd_zcount(Cmd& c) {
  auto range = parse_score_range(c.args[2], c.args[3]);
  if (!range) return c.r.error("ERR min or max is not a float");
  size_t n = 0;
  if (!read_collection(c, c.args[1], kZSet, true, [&](Txn& t) { n = scan_scores(t, *range, false, {}).size(); })) return;
  c.r.integer(static_cast<int64_t>(n));
}

void cmd_zlexcount(Cmd& c) {
  auto range = parse_lex_range(c.args[2], c.args[3]);
  if (!range) return c.r.error("ERR min or max not valid string range item");
  size_t n = 0;
  if (!read_collection(c, c.args[1], kZSet, true, [&](Txn& t) { n = scan_lex(t, *range, false, {}).size(); })) return;
  c.r.integer(static_cast<int64_t>(n));
}

enum class RangeBy { Rank, Score, Lex };

struct RangeQuery {
  RangeBy by = RangeBy::Rank;
  bool rev = false;
  bool with_scores = false;
  bool has_limit = false;
  Limit limit;
};

// Runs a range query; min/max are in the order the command takes them
// (for REV: max first).
void run_range(Cmd& c, std::string_view key, std::string_view a, std::string_view b, const RangeQuery& q) {
  if (q.has_limit && q.by == RangeBy::Rank) {
    return c.r.error("ERR syntax error, LIMIT is only supported in combination with either BYSCORE or BYLEX");
  }
  if (q.with_scores && q.by == RangeBy::Lex) {
    return c.r.error("ERR syntax error, WITHSCORES not supported in combination with BYLEX");
  }
  std::string_view min = q.rev ? b : a, max = q.rev ? a : b;
  Entries out;
  if (q.by == RangeBy::Rank) {
    auto start = parse_int64(a), stop = parse_int64(b);
    if (!start || !stop) return c.r.error(kNotInteger);
    if (!read_collection(c, key, kZSet, true, [&](Txn& t) {
          int64_t s, e;
          if (normalize_range(*start, *stop, static_cast<int64_t>(t.size()), &s, &e)) out = scan_ranks(t, s, e, q.rev);
        })) {
      return;
    }
  } else if (q.by == RangeBy::Score) {
    auto range = parse_score_range(min, max);
    if (!range) return c.r.error("ERR min or max is not a float");
    if (!read_collection(c, key, kZSet, true, [&](Txn& t) { out = scan_scores(t, *range, q.rev, q.limit); })) return;
  } else {
    auto range = parse_lex_range(min, max);
    if (!range) return c.r.error("ERR min or max not valid string range item");
    if (!read_collection(c, key, kZSet, true, [&](Txn& t) { out = scan_lex(t, *range, q.rev, q.limit); })) return;
  }
  reply_entries(c, out, q.with_scores);
}

// Parses [WITHSCORES] [LIMIT offset count] from args[i..]; false on error (replied).
bool parse_range_options(Cmd& c, size_t i, RangeQuery* q, bool allow_by) {
  for (; i < c.args.size(); ++i) {
    std::string opt = to_lower(c.args[i]);
    if (opt == "withscores") {
      q->with_scores = true;
    } else if (opt == "limit" && i + 2 < c.args.size()) {
      auto off = parse_int64(c.args[i + 1]), cnt = parse_int64(c.args[i + 2]);
      if (!off || !cnt) {
        c.r.error(kNotInteger);
        return false;
      }
      q->has_limit = true;
      q->limit = {*off, *cnt};
      i += 2;
    } else if (allow_by && opt == "byscore") {
      q->by = RangeBy::Score;
    } else if (allow_by && opt == "bylex") {
      q->by = RangeBy::Lex;
    } else if (allow_by && opt == "rev") {
      q->rev = true;
    } else {
      c.r.error(kSyntax);
      return false;
    }
  }
  return true;
}

// ZRANGE key start stop [BYSCORE|BYLEX] [REV] [LIMIT offset count] [WITHSCORES]
void cmd_zrange(Cmd& c) {
  RangeQuery q;
  if (!parse_range_options(c, 4, &q, true)) return;
  run_range(c, c.args[1], c.args[2], c.args[3], q);
}

void range_variant(Cmd& c, RangeBy by, bool rev) {
  RangeQuery q;
  if (!parse_range_options(c, 4, &q, false)) return;
  if (by == RangeBy::Lex && q.with_scores) return c.r.error(kSyntax);
  if (by == RangeBy::Rank && q.has_limit) return c.r.error(kSyntax);
  q.by = by;
  q.rev = rev;
  run_range(c, c.args[1], c.args[2], c.args[3], q);
}

void cmd_zrangebyscore(Cmd& c) { range_variant(c, RangeBy::Score, false); }
void cmd_zrevrangebyscore(Cmd& c) { range_variant(c, RangeBy::Score, true); }
void cmd_zrevrange(Cmd& c) { range_variant(c, RangeBy::Rank, true); }
void cmd_zrangebylex(Cmd& c) { range_variant(c, RangeBy::Lex, false); }
void cmd_zrevrangebylex(Cmd& c) { range_variant(c, RangeBy::Lex, true); }

// ZRANK / ZREVRANK key member [WITHSCORE]
void rank_generic(Cmd& c, bool rev) {
  bool with_score = false;
  if (c.args.size() == 4) {
    if (!iequals(c.args[3], "withscore")) return c.r.error(kSyntax);
    with_score = true;
  } else if (c.args.size() > 4) {
    return c.r.error(kSyntax);
  }
  std::optional<double> score;
  int64_t rank = 0;
  if (!read_collection(c, c.args[1], kZSet, true, [&](Txn& t) {
        score = score_of(t, c.args[2]);
        if (!score) return;
        t.scan("s", score_sub(*score, c.args[2]), false, [&](std::string_view, std::string_view) {
          ++rank;
          return true;
        });
        if (rev) rank = static_cast<int64_t>(t.size()) - 1 - rank;
      })) {
    return;
  }
  if (!score) return with_score ? c.r.null_array() : c.r.null();
  if (!with_score) return c.r.integer(rank);
  c.r.array(2);
  c.r.integer(rank);
  reply_score(c, *score);
}

void cmd_zrank(Cmd& c) { rank_generic(c, false); }
void cmd_zrevrank(Cmd& c) { rank_generic(c, true); }

template <class Select>
void remove_range(Cmd& c, Select&& select) {
  int64_t removed = 0;
  if (!with_collection(c, c.args[1], kZSet, [&](Txn& t) {
        Entries victims = select(t);
        for (const auto& e : victims) remove_member(t, e.member, e.score);
        removed = static_cast<int64_t>(victims.size());
      })) {
    return;
  }
  c.r.integer(removed);
}

void cmd_zremrangebyscore(Cmd& c) {
  auto range = parse_score_range(c.args[2], c.args[3]);
  if (!range) return c.r.error("ERR min or max is not a float");
  remove_range(c, [&](Txn& t) { return scan_scores(t, *range, false, {}); });
}

void cmd_zremrangebylex(Cmd& c) {
  auto range = parse_lex_range(c.args[2], c.args[3]);
  if (!range) return c.r.error("ERR min or max not valid string range item");
  remove_range(c, [&](Txn& t) { return scan_lex(t, *range, false, {}); });
}

void cmd_zremrangebyrank(Cmd& c) {
  auto start = parse_int64(c.args[2]), stop = parse_int64(c.args[3]);
  if (!start || !stop) return c.r.error(kNotInteger);
  remove_range(c, [&](Txn& t) {
    int64_t s, e;
    if (!normalize_range(*start, *stop, static_cast<int64_t>(t.size()), &s, &e)) return Entries{};
    return scan_ranks(t, s, e, false);
  });
}

// ZPOPMIN / ZPOPMAX key [count]
void pop_generic(Cmd& c, bool max) {
  std::optional<int64_t> count;
  if (c.args.size() > 3) return c.r.error(kSyntax);
  if (c.args.size() == 3) {
    count = parse_int64(c.args[2]);
    if (!count || *count < 0) return c.r.error(kMustBePositive);
  }
  Entries popped;
  if (!with_collection(c, c.args[1], kZSet, [&](Txn& t) {
        int64_t n = count.value_or(1);
        if (n == 0 || t.size() == 0) return;
        popped = scan_ranks(t, 0, n - 1, max);
        for (const auto& e : popped) remove_member(t, e.member, e.score);
      })) {
    return;
  }
  if (count && c.r.proto() >= 3) return reply_entries(c, popped, true);
  c.r.array(popped.size() * 2);
  for (const auto& e : popped) {
    c.r.bulk(e.member);
    reply_score(c, e.score);
  }
}

void cmd_zpopmin(Cmd& c) { pop_generic(c, false); }
void cmd_zpopmax(Cmd& c) { pop_generic(c, true); }

// Pops up to `count` members, lowest (or highest) score first, from the
// first of `keys` that has any; with a timeout, waits when none has.
struct Popped {
  bool wrong = false;
  bool blocked = false;
  std::optional<std::string_view> from;
  Entries entries;
};

Popped pop_first(Cmd& c, const std::vector<std::string_view>& keys, bool max, int64_t count,
                 std::optional<double> timeout) {
  Popped out;
  c.db().transact_many(keys, [&](Database::KeyTxns& txns) {
    for (size_t i = 0; i < keys.size(); ++i) {
      Txn& t = txns[i];
      if (t.wrong_type(kZSet)) {
        out.wrong = true;
        return;
      }
      if (t.size() == 0) continue;
      out.entries = scan_ranks(t, 0, count - 1, max);
      for (const auto& e : out.entries) remove_member(t, e.member, e.score);
      out.from = keys[i];
      return;
    }
    if (timeout) out.blocked = block_for_keys(c, keys, *timeout);
  });
  return out;
}

// BZPOPMIN / BZPOPMAX key [key ...] timeout: [key, member, score].
void bpop_generic(Cmd& c, bool max) {
  auto timeout = parse_block_timeout(c, c.args.back());
  if (!timeout) return;
  std::vector<std::string_view> keys(c.args.begin() + 1, c.args.end() - 1);
  Popped p = pop_first(c, keys, max, 1, timeout);
  if (p.wrong) return c.r.error(kWrongType);
  if (p.blocked) return;
  if (!p.from) return c.r.null_array();
  c.r.array(3);
  c.r.bulk(*p.from);
  c.r.bulk(p.entries[0].member);
  reply_score(c, p.entries[0].score);
}

void cmd_bzpopmin(Cmd& c) { bpop_generic(c, false); }
void cmd_bzpopmax(Cmd& c) { bpop_generic(c, true); }

// ZMPOP numkeys key [key ...] MIN|MAX [COUNT count], and BZMPOP with a
// timeout first: [key, [[member, score] ...]].
void mpop_generic(Cmd& c, size_t at, std::optional<double> timeout) {
  auto n = parse_int64(c.args[at]);
  if (!n || *n <= 0) return c.r.error("ERR numkeys should be greater than 0");
  size_t side_at = at + 1 + static_cast<size_t>(*n);
  if (side_at >= c.args.size()) return c.r.error(kSyntax);
  bool max;
  if (iequals(c.args[side_at], "min")) max = false;
  else if (iequals(c.args[side_at], "max")) max = true;
  else return c.r.error(kSyntax);
  int64_t count = 1;
  if (side_at + 1 < c.args.size()) {
    if (side_at + 3 != c.args.size() || !iequals(c.args[side_at + 1], "count")) return c.r.error(kSyntax);
    auto v = parse_int64(c.args[side_at + 2]);
    if (!v || *v <= 0) return c.r.error("ERR count should be greater than 0");
    count = *v;
  }
  std::vector<std::string_view> keys(c.args.begin() + static_cast<std::ptrdiff_t>(at + 1),
                                     c.args.begin() + static_cast<std::ptrdiff_t>(side_at));
  Popped p = pop_first(c, keys, max, count, timeout);
  if (p.wrong) return c.r.error(kWrongType);
  if (p.blocked) return;
  if (!p.from) return c.r.null_array();
  c.r.array(2);
  c.r.bulk(*p.from);
  c.r.array(p.entries.size());
  for (const auto& e : p.entries) {
    c.r.array(2);
    c.r.bulk(e.member);
    reply_score(c, e.score);
  }
}

void cmd_zmpop(Cmd& c) { mpop_generic(c, 1, std::nullopt); }

void cmd_bzmpop(Cmd& c) {
  auto timeout = parse_block_timeout(c, c.args[1]);
  if (!timeout) return;
  mpop_generic(c, 2, timeout);
}

}  // namespace

CommandList zset_commands() {
  return {
      {"zadd", {cmd_zadd, -4}},
      {"zincrby", {cmd_zincrby, 4}},
      {"zrem", {cmd_zrem, -3}},
      {"zscore", {cmd_zscore, 3}},
      {"zmscore", {cmd_zmscore, -3}},
      {"zcard", {cmd_zcard, 2}},
      {"zcount", {cmd_zcount, 4}},
      {"zlexcount", {cmd_zlexcount, 4}},
      {"zrange", {cmd_zrange, -4}},
      {"zrangebyscore", {cmd_zrangebyscore, -4}},
      {"zrevrangebyscore", {cmd_zrevrangebyscore, -4}},
      {"zrevrange", {cmd_zrevrange, -4}},
      {"zrangebylex", {cmd_zrangebylex, -4}},
      {"zrevrangebylex", {cmd_zrevrangebylex, -4}},
      {"zrank", {cmd_zrank, -3}},
      {"zrevrank", {cmd_zrevrank, -3}},
      {"zremrangebyscore", {cmd_zremrangebyscore, 4}},
      {"zremrangebyrank", {cmd_zremrangebyrank, 4}},
      {"zremrangebylex", {cmd_zremrangebylex, 4}},
      {"zpopmin", {cmd_zpopmin, -2}},
      {"zpopmax", {cmd_zpopmax, -2}},
      {"bzpopmin", {cmd_bzpopmin, -3}},
      {"bzpopmax", {cmd_bzpopmax, -3}},
      {"zmpop", {cmd_zmpop, -4}},
      {"bzmpop", {cmd_bzmpop, -5}},
  };
}

}  // namespace datadam
