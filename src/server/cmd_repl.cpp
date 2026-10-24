// REPLICAOF / SLAVEOF, ROLE, WAIT, FAILOVER, and the commands a replica sends
// its primary: REPLCONF and REPLSYNC (see server/replication.h for the protocol).

#include <format>
#include <limits>

#include "server/command.h"
#include "util/clock.h"
#include "util/strings.h"

namespace datadam {

namespace {

// REPLICAOF host port | REPLICAOF NO ONE
void cmd_replicaof(Cmd& c) {
  std::string host;
  uint16_t port = 0;
  if (!(iequals(c.args[1], "no") && iequals(c.args[2], "one"))) {
    auto p = parse_int64(c.args[2]);
    if (!p || *p < 1 || *p > 65535) return c.r.error("ERR Invalid master port");
    host = std::string(c.args[1]);
    port = static_cast<uint16_t>(*p);
  }
  if (!c.ctx.replicaof) return c.r.error("ERR replication is not available in this process");
  if (std::string error = c.ctx.replicaof(host, port); !error.empty()) return c.r.error(error);
  c.ctx.set_replicaof(host.empty() ? std::string() : std::format("{} {}", host, port));
  c.r.ok();
}

void cmd_role(Cmd& c) { c.ctx.repl.role(c.r, c.db().storage()); }

// REPLCONF listening-port <port> | ACK <offset> | <anything else> <value>
void cmd_replconf(Cmd& c) {
  if ((c.args.size() - 1) % 2 != 0) return c.r.error(kSyntax);
  for (size_t i = 1; i + 1 < c.args.size(); i += 2) {
    std::string opt = to_lower(c.args[i]);
    if (opt == "ack") return;  // only meaningful on a replica's stream, and never answered
    if (opt == "listening-port") {
      auto p = parse_int64(c.args[i + 1]);
      if (!p || *p < 0 || *p > 65535) return c.r.error("ERR invalid listening port");
      c.client.listening_port = static_cast<uint16_t>(*p);
    }
  }
  c.r.ok();
}

// REPLSYNC <replid|?> <offset>: the connection becomes this replica's stream.
void cmd_replsync(Cmd& c) {
  if (!c.db().storage().wal_retained()) {
    return c.r.error("ERR replication needs the WAL kept on the primary: set wal-retention");
  }
  auto offset = parse_uint64(c.args[2]);
  if (!offset) return c.r.error("ERR invalid offset");
  c.client.repl_id = std::string(c.args[1]);
  c.client.repl_offset = *offset;
  c.client.start_replication = true;
}

// WAIT numreplicas timeout: until that many replicas acknowledged every
// write committed before it (this connection's included), or the timeout
// (milliseconds, 0 = none). Replies how many did.
void cmd_wait(Cmd& c) {
  if (c.ctx.repl.is_replica()) {
    return c.r.error(
        "ERR WAIT cannot be used with replica instances. Please also note that since Redis 4.0 if a replica is "
        "configured to be writable (which is not the default) writes to replicas are just local and are not "
        "propagated.");
  }
  auto wanted = parse_int64(c.args[1]);
  if (!wanted) return c.r.error(kNotInteger);
  auto timeout = parse_int64(c.args[2]);
  if (!timeout) return c.r.error("ERR timeout is not an integer or out of range");
  if (*timeout < 0) return c.r.error("ERR timeout is negative");
  ClientState& client = c.client;
  bool resuming = client.block_deadline_ms != 0;
  if (!resuming) {
    c.db().commit_active_batch();  // this connection's writes are in the WAL now
    client.wait_offset = c.db().storage().latest_sequence() + 1;
  }
  // Registered before counting, so an acknowledgment in between is not missed.
  auto waiter = std::make_shared<KeyWatch::Waiter>();
  c.ctx.repl.wait_ack(waiter);
  auto acked = static_cast<int64_t>(c.ctx.repl.acked(client.wait_offset));
  int64_t now = steady_time_ms();
  if (!resuming && client.can_block) {
    client.block_deadline_ms = *timeout == 0 ? std::numeric_limits<int64_t>::max() : now + *timeout;
  }
  if (acked >= *wanted || !client.can_block || now >= client.block_deadline_ms) {
    c.ctx.repl.forget_waiter(waiter.get());
    return c.r.integer(acked);
  }
  client.key_waiter = std::move(waiter);
  client.wait_keys.clear();
  client.wait_for_keys = true;
  client.retry_later = true;
}

// FAILOVER [TO host port [FORCE]] [ABORT] [TIMEOUT milliseconds]
void cmd_failover(Cmd& c) {
  std::string host;
  uint16_t port = 0;
  int64_t timeout = 0;
  bool force = false, abort = false;
  for (size_t i = 1; i < c.args.size(); ++i) {
    std::string opt = to_lower(c.args[i]);
    if (opt == "to" && i + 2 < c.args.size()) {
      host = std::string(c.args[i + 1]);
      auto p = parse_int64(c.args[i + 2]);
      if (!p || *p < 1 || *p > 65535) return c.r.error("ERR Invalid port");
      port = static_cast<uint16_t>(*p);
      i += 2;
    } else if (opt == "force") {
      force = true;
    } else if (opt == "abort") {
      abort = true;
    } else if (opt == "timeout" && i + 1 < c.args.size()) {
      auto t = parse_int64(c.args[++i]);
      if (!t || *t <= 0) return c.r.error("ERR FAILOVER timeout must be greater than 0");
      timeout = *t;
    } else {
      return c.r.error(kSyntax);
    }
  }
  if (abort && (force || !host.empty() || timeout)) return c.r.error(kSyntax);
  if (!c.ctx.failover) return c.r.error("ERR FAILOVER is not available in this process");
  if (std::string error = c.ctx.failover(host, port, timeout, force, abort); !error.empty()) return c.r.error(error);
  c.r.ok();
}

}  // namespace

CommandList replication_commands() {
  return {
      {"replicaof", {cmd_replicaof, 3}},
      {"slaveof", {cmd_replicaof, 3}},
      {"role", {cmd_role, 1}},
      {"replconf", {cmd_replconf, -3}},
      {"replsync", {cmd_replsync, 3}},
      {"wait", {cmd_wait, 3}},
      {"failover", {cmd_failover, -1}},
  };
}

}  // namespace datadam
