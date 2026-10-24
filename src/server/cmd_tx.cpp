// Transactions: MULTI / EXEC / DISCARD / WATCH / UNWATCH.
//
// After MULTI, execute_redis_command checks each command (that it exists,
// its arity, the ACL) and queues it, replying QUEUED; one that fails the
// check makes EXEC answer EXECABORT. EXEC runs the queue as one block
// (Database::ExecScope): the stripes of every key the commands name are
// locked first (all stripes if one of them reads or writes keys it does not
// name, like KEYS or FLUSHALL), so no other client writes those keys or
// reads them from disk while the block runs, and its writes are committed
// together. As in Redis, a command that fails inside the block does not undo
// the others. WATCH makes EXEC reply nil if a watched key was written
// (by anyone) after the WATCH.

#include <format>

#include "server/command.h"

namespace datadam {

namespace {

void unwatch(ClientState& client) {
  if (client.watching) client.watching->clear();
}

void cmd_multi(Cmd& c) {
  if (c.client.in_multi) return c.r.error("ERR MULTI calls can not be nested");
  c.client.in_multi = true;
  c.client.multi_error = false;
  c.client.queued.clear();
  c.r.ok();
}

void cmd_discard(Cmd& c) {
  if (!c.client.in_multi) return c.r.error("ERR DISCARD without MULTI");
  c.client.in_multi = false;
  c.client.multi_error = false;
  c.client.queued.clear();
  unwatch(c.client);
  c.r.ok();
}

void cmd_exec(Cmd& c) {
  ClientState& client = c.client;
  if (!client.in_multi) return c.r.error("ERR EXEC without MULTI");
  std::vector<std::vector<std::string>> queued = std::move(client.queued);
  bool refused = client.multi_error;
  client.in_multi = false;
  client.multi_error = false;
  client.queued.clear();
  if (refused) {
    unwatch(client);
    return c.r.error("EXECABORT Transaction discarded because of previous errors.");
  }

  // The keys to lock, from each command's key positions.
  std::vector<std::string> keys;
  bool all = false;
  for (const auto& args : queued) {
    std::vector<std::string_view> views(args.begin(), args.end());
    const CommandSpec* spec = find_command(to_lower(args[0]));
    if (!spec) continue;
    size_t named = 0;
    for_each_key(*spec, views, [&](std::string_view key, uint8_t) {
      keys.emplace_back(key);
      ++named;
      return true;
    });
    if (named == 0 && (spec->categories & (kCatRead | kCatWrite))) all = true;
    if (spec->categories & kCatScripting) all = true;  // a script runs with the whole keyspace
  }
  std::vector<std::string_view> key_views(keys.begin(), keys.end());
  Database::ExecScope block(c.db(), key_views, all);

  // Every watched key is locked by now (they were written before, or wait).
  bool changed = client.watching && client.watching->dirty();
  unwatch(client);
  if (changed) return c.r.null_array();

  // Inside the block nothing waits: reads go to disk here, and blocking
  // commands answer at once.
  bool defer = client.defer_blocked_reads, can_block = client.can_block;
  client.defer_blocked_reads = false;
  client.can_block = false;
  c.r.array(queued.size());
  for (const auto& args : queued) {
    std::vector<std::string_view> views(args.begin(), args.end());
    execute_redis_command(c.ctx, client, views, c.r);
    client.retry_later = false;
  }
  client.defer_blocked_reads = defer;
  client.can_block = can_block;
  block.commit();
}

void cmd_watch(Cmd& c) {
  if (c.client.in_multi) return c.r.error("ERR WATCH inside MULTI is not allowed");
  if (!c.client.watching) c.client.watching = std::make_shared<WatchedKeys>(c.db().key_watch_ref());
  for (size_t i = 1; i < c.args.size(); ++i) {
    if (!c.client.watching->has(c.args[i])) c.db().watch_key(c.args[i], *c.client.watching);
  }
  c.r.ok();
}

void cmd_unwatch(Cmd& c) {
  unwatch(c.client);
  c.r.ok();
}

}  // namespace

bool runs_in_multi(std::string_view name) {
  return name == "exec" || name == "discard" || name == "multi" || name == "watch" || name == "quit" ||
         name == "reset";
}

CommandList transaction_commands() {
  return {
      {"multi", {cmd_multi, 1}},     {"exec", {cmd_exec, 1}},       {"discard", {cmd_discard, 1}},
      {"watch", {cmd_watch, -2}},    {"unwatch", {cmd_unwatch, 1}},
  };
}

}  // namespace datadam
