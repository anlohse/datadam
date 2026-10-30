// Administration: SAVE / BGSAVE / BACKUP / LASTSAVE, SHUTDOWN, SLOWLOG,
// MONITOR, and the key introspection commands OBJECT / TOUCH / MEMORY.

#include <format>

#include "server/command.h"
#include "util/clock.h"
#include "util/log.h"

namespace datadam {

namespace {

// ---- saving

void cmd_save(Cmd& c) {
  c.db().commit_active_batch();
  c.db().storage().flush();
  c.ctx.backups.saved();
  c.r.ok();
}

// BGSAVE [SCHEDULE]: a checkpoint in backup-dir when it is set, else a flush.
void cmd_bgsave(Cmd& c) {
  if (c.args.size() > 2 || (c.args.size() == 2 && !iequals(c.args[1], "schedule"))) return c.r.error(kSyntax);
  Config cfg = c.ctx.settings();
  c.db().commit_active_batch();
  if (cfg.backup_dir.empty()) {
    c.db().storage().flush();
    c.ctx.backups.saved();
    return c.r.simple("Background saving started");
  }
  if (!c.ctx.backups.start(cfg.backup_dir, cfg.backup_keep)) {
    if (c.args.size() == 2) return c.r.simple("Background saving scheduled");
    return c.r.error("ERR Background save already in progress");
  }
  c.r.simple("Background saving started");
}

// BACKUP path: datadam's own; a consistent copy of the database at path.
// Unless enable-protected-configs lets this client choose any path, it is a
// directory inside backup-dir (a relative path is taken from there).
void cmd_backup(Cmd& c) {
  std::string path(c.args[1]);
  if (!protected_configs_allowed(c.ctx, c.client)) {
    std::string dir = c.ctx.settings().backup_dir;
    if (dir.empty()) {
      return c.r.error("ERR BACKUP writes inside backup-dir, which is not set (or set enable-protected-configs)");
    }
    auto inside = backup_path_inside(dir, path);
    if (!inside) return c.r.error(std::format("ERR BACKUP path '{}' is not inside backup-dir", path));
    path = *inside;
  }
  c.db().commit_active_batch();
  std::string error = c.ctx.backups.backup(path);
  if (!error.empty()) return c.r.error(error);
  c.r.ok();
}

void cmd_lastsave(Cmd& c) { c.r.integer(c.ctx.backups.status().last_save_s); }

// SHUTDOWN [NOSAVE | SAVE] [NOW] [FORCE] [ABORT]
void cmd_shutdown(Cmd& c) {
  bool save = false;
  for (size_t i = 1; i < c.args.size(); ++i) {
    std::string opt = to_lower(c.args[i]);
    if (opt == "save") save = true;
    else if (opt == "nosave") save = false;
    else if (opt == "abort") return c.r.error("ERR No shutdown in progress.");
    else if (opt != "now" && opt != "force") return c.r.error(kSyntax);
  }
  if (!c.ctx.shutdown) return c.r.error("ERR SHUTDOWN is not available here");
  c.db().commit_active_batch();
  if (save) c.db().storage().flush();
  log_info("SHUTDOWN requested by client {} ({})", c.client.id, c.client.addr);
  // As in Redis there is no reply: the connection closes as the server stops.
  c.client.close_after_reply = true;
  c.ctx.shutdown();
}

// ---- SLOWLOG GET [count] | LEN | RESET | HELP

void cmd_slowlog(Cmd& c) {
  std::string sub = to_lower(c.args[1]);
  if (sub == "get" && c.args.size() <= 3) {
    size_t count = 10;
    if (c.args.size() == 3) {
      auto n = parse_int64(c.args[2]);
      if (!n || *n < -1) return c.r.error("ERR count should be greater than or equal to -1");
      count = *n == -1 ? SIZE_MAX : static_cast<size_t>(*n);
    }
    auto entries = c.ctx.slowlog.get(count);
    c.r.array(entries.size());
    for (const auto& e : entries) {
      c.r.array(6);
      c.r.integer(static_cast<int64_t>(e.id));
      c.r.integer(e.time_s);
      c.r.integer(e.duration_us);
      c.r.array(e.args.size());
      for (const auto& a : e.args) c.r.bulk(a);
      c.r.bulk(e.addr);
      c.r.bulk(e.name);
    }
    return;
  }
  if (sub == "len" && c.args.size() == 2) return c.r.integer(static_cast<int64_t>(c.ctx.slowlog.len()));
  if (sub == "reset" && c.args.size() == 2) {
    c.ctx.slowlog.reset();
    return c.r.ok();
  }
  if (sub == "help" && c.args.size() == 2) {
    static const char* lines[] = {
        "SLOWLOG <subcommand> [<arg> [value] [opt] ...]. Subcommands are:",
        "GET [<count>]", "    Return top <count> entries from the slowlog (default: 10, -1 mean all).",
        "LEN", "    Return the length of the slowlog.",
        "RESET", "    Reset the slowlog.",
    };
    c.r.array(std::size(lines));
    for (const char* l : lines) c.r.simple(l);
    return;
  }
  c.r.error(std::format("ERR unknown subcommand or wrong number of arguments for '{}'. Try SLOWLOG HELP.",
                        c.args[1]));
}

// ---- MONITOR

void cmd_monitor(Cmd& c) {
  if (c.client.in_multi) return c.r.error("ERR MONITOR isn't allowed in MULTI");
  if (!c.client.subscriber) c.client.subscriber = std::make_shared<Subscriber>(c.r.proto());
  if (!c.client.monitor) {
    c.client.monitor = true;
    if (c.client.entry) c.client.entry->monitor = true;
    c.r.ok();
    // The OK goes out first; then every command the server runs.
    c.client.subscriber->push(c.r.buffer());
    c.r.buffer().clear();
    c.ctx.monitors.add(c.client.id, c.client.subscriber);
    return;
  }
  c.r.ok();
}

// ---- keys

// OBJECT ENCODING | IDLETIME | REFCOUNT | FREQ | HELP key
void cmd_object(Cmd& c) {
  std::string sub = to_lower(c.args[1]);
  if (sub == "help" && c.args.size() == 2) {
    static const char* lines[] = {
        "OBJECT <subcommand> [<arg> [value] [opt] ...]. Subcommands are:",
        "ENCODING <key>", "    Return the kind of internal representation used in order to store the value",
        "    associated with a <key>.",
        "FREQ <key>", "    Return the access frequency index of the <key>.",
        "IDLETIME <key>", "    Return the idle time of the <key>, that is the approximated number of",
        "    seconds elapsed since the last access to the key.",
        "REFCOUNT <key>", "    Return the number of references of the value associated with the specified",
        "    <key>.",
    };
    c.r.array(std::size(lines));
    for (const char* l : lines) c.r.simple(l);
    return;
  }
  if (c.args.size() != 3) {
    return c.r.error(std::format("ERR unknown subcommand or wrong number of arguments for '{}'. Try OBJECT HELP.",
                                 c.args[1]));
  }
  std::string_view key = c.args[2];
  auto rec = c.db().get(key);
  if (!rec) return c.r.null();
  if (sub == "encoding") {
    // The names Redis would use for a value of this kind and size.
    CollectionMeta m;
    bool small = is_collection(rec->type) && decode_collection_meta(rec->value, &m) && m.size <= 128;
    switch (rec->type) {
      case ValueType::String: {
        bool integer = rec->value.size() <= 20 && parse_int64(rec->value).has_value();
        return c.r.bulk(integer ? "int" : rec->value.size() <= 44 ? "embstr" : "raw");
      }
      case ValueType::Hash: return c.r.bulk(small ? "listpack" : "hashtable");
      case ValueType::Set: return c.r.bulk(small ? "listpack" : "hashtable");
      case ValueType::ZSet: return c.r.bulk(small ? "listpack" : "skiplist");
      case ValueType::List: return c.r.bulk(small ? "listpack" : "quicklist");
      default: return c.r.bulk("raw");
    }
  }
  if (sub == "refcount") return c.r.integer(1);
  if (sub == "idletime") {
    auto idle = c.db().cache().idle_ms(key);
    return c.r.integer(idle ? *idle / 1000 : 0);
  }
  if (sub == "freq") {
    return c.r.error(
        "ERR An LFU maxmemory policy is not selected, access frequency not tracked. Please note that when "
        "switching between policies at runtime LRU and LFU data will take some time to adjust.");
  }
  c.r.error(std::format("ERR unknown subcommand or wrong number of arguments for '{}'. Try OBJECT HELP.",
                        c.args[1]));
}

// TOUCH key [key ...]: how many exist (their access time is updated).
void cmd_touch(Cmd& c) {
  int64_t n = 0;
  for (size_t i = 1; i < c.args.size(); ++i) n += c.db().exists(c.args[i]) ? 1 : 0;
  c.r.integer(n);
}

// MEMORY USAGE key [SAMPLES count] | MEMORY HELP: an estimate of the bytes a
// key takes, from its record and (for collections) a sample of its elements.
void cmd_memory(Cmd& c) {
  std::string sub = to_lower(c.args[1]);
  if (sub == "help" && c.args.size() == 2) {
    static const char* lines[] = {
        "MEMORY <subcommand> [<arg> [value] [opt] ...]. Subcommands are:",
        "USAGE <key> [SAMPLES <count>]",
        "    Return memory in bytes used by <key> and its value. Nested values are",
        "    sampled up to <count> times (default: 5, 0 means sample all).",
    };
    c.r.array(std::size(lines));
    for (const char* l : lines) c.r.simple(l);
    return;
  }
  if (sub != "usage" || (c.args.size() != 3 && c.args.size() != 5)) {
    return c.r.error(std::format("ERR unknown subcommand or wrong number of arguments for '{}'. Try MEMORY HELP.",
                                 c.args[1]));
  }
  int64_t samples = 5;
  if (c.args.size() == 5) {
    auto n = parse_int64(c.args[4]);
    if (!iequals(c.args[3], "samples") || !n || *n < 0) return c.r.error(kSyntax);
    samples = *n;
  }
  std::string_view key = c.args[2];
  auto rec = c.db().get(key);
  if (!rec) return c.r.null();
  constexpr int64_t kRecordOverhead = 72;  // a cached object's header, about what a record costs around it
  int64_t total = kRecordOverhead + static_cast<int64_t>(key.size());
  CollectionMeta m;
  if (!is_collection(rec->type) || !decode_collection_meta(rec->value, &m)) {
    return c.r.integer(total + static_cast<int64_t>(rec->value.size()));
  }
  total += static_cast<int64_t>(kCollectionMetaSize);
  // Each element is a data entry: the key, its version, the sub-key and the value.
  int64_t seen = 0, bytes = 0;
  int64_t per_entry = static_cast<int64_t>(key.size()) + 12;
  bool ok = read_collection(c, key, rec->type, true, [&](Database::KeyTxn& t) {
    t.scan("", "", false, [&](std::string_view sub_key, std::string_view value) {
      bytes += per_entry + static_cast<int64_t>(sub_key.size() + value.size());
      ++seen;
      return samples == 0 || seen < samples;
    });
  });
  if (!ok) return;  // replied
  if (seen > 0) {
    // A sorted set keeps two entries per member.
    int64_t entries = static_cast<int64_t>(m.size) * (rec->type == ValueType::ZSet ? 2 : 1);
    total += bytes * entries / seen;
  }
  c.r.integer(total);
}

}  // namespace

CommandList admin_commands() {
  return {
      {"save", {cmd_save, 1}},        {"bgsave", {cmd_bgsave, -1}},     {"backup", {cmd_backup, 2}},
      {"lastsave", {cmd_lastsave, 1}}, {"shutdown", {cmd_shutdown, -1}}, {"slowlog", {cmd_slowlog, -2}},
      {"monitor", {cmd_monitor, 1}},  {"object", {cmd_object, -2}},     {"touch", {cmd_touch, -2}},
      {"memory", {cmd_memory, -2}},
  };
}

}  // namespace datadam
