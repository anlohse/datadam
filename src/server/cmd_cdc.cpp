// CDC.INFO and CDC.READ: the change feed, decoded from the retained RocksDB
// WAL (see db/change_feed.h). Needs wal-retention > 0.
//
//   CDC.READ <sequence|$> [COUNT n] [MATCH pattern] [BLOCK ms]
//   -> [next sequence, [[batch sequence, commit time ms, [[op, key, args...], ...]], ...]]
//
// "$" means "what is written from now on". COUNT (default 100) is reached
// with whole batches, so a batch is never split. BLOCK waits up to ms (0:
// forever) for a write when there is nothing to return, without holding a
// thread (ClientState::can_block).

#include <cstdint>
#include <format>
#include <limits>

#include "db/change_feed.h"
#include "server/command.h"
#include "util/clock.h"
#include "util/strings.h"

namespace datadam {

namespace {

constexpr size_t kDefaultCount = 100;
// A read looks at no more batches than this (a MATCH may skip most of them);
// the next sequence it returns continues the scan.
constexpr size_t kMaxBatchesPerRead = 10000;

bool feed_on(Cmd& c) {
  if (c.db().storage().wal_retained()) return true;
  c.r.error("ERR the change feed is off: set wal-retention (it also needs experimental-write-path wal or wal-buffered)");
  return false;
}

void cmd_cdc_info(Cmd& c) {
  RocksStorage& st = c.db().storage();
  bool on = st.wal_retained();
  Config cfg = c.ctx.settings();
  c.r.map(6);
  c.r.bulk("enabled");
  c.r.bulk(on ? "yes" : "no");
  c.r.bulk("wal_retention_seconds");
  c.r.integer(cfg.wal_retention.count() / 1000);
  c.r.bulk("wal_retention_size");
  c.r.integer(static_cast<int64_t>(cfg.wal_retention_size));
  c.r.bulk("first_sequence");
  c.r.integer(on ? static_cast<int64_t>(st.oldest_sequence()) : 0);
  c.r.bulk("last_sequence");
  c.r.integer(static_cast<int64_t>(st.latest_sequence()));
  c.r.bulk("wal_bytes");
  c.r.integer(static_cast<int64_t>(st.wal_bytes()));
}

void reply_changes(Cmd& c, const ChangeRead& r) {
  c.r.array(2);
  c.r.integer(static_cast<int64_t>(r.next));
  c.r.array(r.batches.size());
  for (const ChangeBatch& b : r.batches) {
    c.r.array(3);
    c.r.integer(static_cast<int64_t>(b.seq));
    c.r.integer(b.time_ms);
    c.r.array(b.changes.size());
    for (const Change& ch : b.changes) {
      c.r.array(2 + ch.args.size());
      c.r.bulk(ch.op);
      c.r.bulk(ch.key);
      for (const std::string& a : ch.args) c.r.bulk(a);
    }
  }
}

void cmd_cdc_read(Cmd& c) {
  if (!feed_on(c)) return;
  RocksStorage& st = c.db().storage();
  ClientState& client = c.client;
  bool resuming = client.block_deadline_ms != 0;  // run again after waiting
  uint64_t from = 0;
  if (c.args[1] == "$") {
    from = st.latest_sequence() + 1;
  } else if (auto v = parse_uint64(c.args[1])) {
    from = *v;
  } else {
    return c.r.error("ERR invalid sequence: a number, or $ for new changes only");
  }
  size_t count = kDefaultCount;
  std::string_view pattern;
  std::optional<int64_t> block_ms;
  for (size_t i = 2; i < c.args.size(); ++i) {
    std::string opt = to_lower(c.args[i]);
    if (i + 1 >= c.args.size()) return c.r.error(kSyntax);
    if (opt == "count") {
      auto n = parse_int64(c.args[++i]);
      if (!n || *n < 1) return c.r.error("ERR COUNT must be a positive integer");
      count = static_cast<size_t>(*n);
    } else if (opt == "match") {
      pattern = c.args[++i];
    } else if (opt == "block") {
      auto n = parse_int64(c.args[++i]);
      if (!n || *n < 0) return c.r.error("ERR timeout is not an integer or out of range");
      block_ms = *n;
    } else {
      return c.r.error(kSyntax);
    }
  }
  if (resuming) from = client.block_from;
  auto keep = [&](std::string_view key) {
    return (pattern.empty() || glob_match(pattern, key)) && may_read_key(c, key);
  };
  ChangeRead r;
  try {
    r = read_changes(st, from, count, kMaxBatchesPerRead, keep);
  } catch (const StorageError& e) {
    client.block_deadline_ms = 0;
    return c.r.error(std::format("ERR {}", e.what()));
  }
  if (r.batches.empty() && block_ms && client.can_block) {
    int64_t now = steady_time_ms();
    if (!resuming) {
      client.block_deadline_ms = *block_ms == 0 ? std::numeric_limits<int64_t>::max() : deadline_after(now, *block_ms);
    }
    if (now < client.block_deadline_ms) {
      client.block_from = r.next;
      client.wait_after_seq = r.next - 1;
      client.wait_for_writes = true;
      client.retry_later = true;
      return;
    }
  }
  client.block_deadline_ms = 0;
  reply_changes(c, r);
}

}  // namespace

CommandList cdc_commands() {
  return {
      {"cdc.info", {cmd_cdc_info, 1}},
      {"cdc.read", {cmd_cdc_read, -2}},
  };
}

}  // namespace datadam
