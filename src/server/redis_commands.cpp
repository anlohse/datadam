#include "server/redis_commands.h"

#include <algorithm>
#include <bitset>
#include <chrono>
#include <cmath>
#include <format>
#include <iterator>
#include <limits>
#include <optional>
#include <unordered_map>

#ifdef _WIN32
#include <process.h>
#define DATADAM_GETPID _getpid
#else
#include <unistd.h>
#define DATADAM_GETPID getpid
#endif

#include "server/command.h"
#include "server/read_pool.h"
#include "util/clock.h"
#include "util/process.h"
#include "util/strings.h"

namespace datadam {

namespace {

using Op = Database::Op;

constexpr size_t kMaxStringSize = 512ULL << 20;
// Reported to clients that gate features on the Redis version.
constexpr std::string_view kRedisCompatVersion = "7.2.0";
// A script running longer than this makes other commands answer BUSY (Redis's busy-reply-threshold).
constexpr int64_t kScriptBusyMs = 5000;
// A command for the keyspace that comes in while a script has been running
// this long waits for it off the I/O thread (shorter: it waits for the lock).
constexpr int64_t kScriptParkUs = 1000;

bool is_string(const RecordView& v) { return v.type == ValueType::String; }
bool is_string(const Record& v) { return v.type == ValueType::String; }

// Converts the value of an EX/PX/EXAT/PXAT option to absolute ms. Rejects <= 0.
bool expire_option(std::string_view opt, std::string_view value, int64_t now, int64_t* out) {
  auto v = parse_int64(value);
  if (!v || *v <= 0) return false;
  int64_t n = *v;
  if (opt == "ex" || opt == "exat") {
    if (n > INT64_MAX / 1000) return false;
    n *= 1000;
  }
  if (opt == "ex" || opt == "px") {
    if (n > INT64_MAX - now) return false;
    n += now;
  }
  *out = n;
  return true;
}

// ---- connection / server -------------------------------------------------

void cmd_ping(Cmd& c) {
  if (c.args.size() > 2) return c.r.error("ERR wrong number of arguments for 'ping' command");
  if (c.client.subscriber && c.client.subscriber->count() > 0 && c.r.proto() < 3) {
    // A subscribed RESP2 connection gets it like a message: ["pong", argument].
    c.r.array(2);
    c.r.bulk("pong");
    return c.r.bulk(c.args.size() == 2 ? c.args[1] : "");
  }
  if (c.args.size() == 2) c.r.bulk(c.args[1]);
  else c.r.simple("PONG");
}

void cmd_echo(Cmd& c) { c.r.bulk(c.args[1]); }

void cmd_quit(Cmd& c) {
  c.r.ok();
  c.client.close_after_reply = true;
}

void cmd_select(Cmd& c) {
  auto v = parse_int64(c.args[1]);
  if (!v) return c.r.error(kNotInteger);
  if (*v != 0) return c.r.error("ERR DB index is out of range");
  c.r.ok();
}

constexpr std::string_view kWrongPass = "WRONGPASS invalid username-password pair or user is disabled.";

// Checks AUTH-style credentials (the only user is "default"); replies with
// the error and returns false when they are not accepted.
bool authenticate(Cmd& c, std::string_view user, std::string_view password) {
  auto u = c.ctx.acl.authenticate(user, password);
  if (!u) {
    c.ctx.stats.auth_failures.fetch_add(1, std::memory_order_relaxed);
    c.ctx.acl_log.add("auth", "AUTH", std::string(user), client_info(c.client));
    c.r.error(kWrongPass);
    return false;
  }
  c.client.user = std::move(u);
  c.client.acl_version = c.ctx.acl.version();
  c.client.authenticated = true;
  return true;
}

// AUTH password | AUTH username password
void cmd_auth(Cmd& c) {
  if (c.args.size() > 3) return c.r.error(kSyntax);
  if (c.args.size() == 2 && !c.ctx.auth_required) {
    return c.r.error(
        "ERR AUTH <password> called without any password configured for the default user. "
        "Are you sure your configuration is correct?");
  }
  std::string_view user = c.args.size() == 3 ? c.args[1] : "default";
  if (authenticate(c, user, c.args.back())) c.r.ok();
}

void cmd_hello(Cmd& c) {
  int proto = c.client.proto;
  size_t i = 1;
  if (c.args.size() > 1) {
    auto v = parse_int64(c.args[1]);
    if (!v) return c.r.error("ERR Protocol version is not an integer or out of range");
    if (*v < 2 || *v > 3) return c.r.error("NOPROTO unsupported protocol version");
    proto = static_cast<int>(*v);
    i = 2;
  }
  std::optional<std::string> name;
  std::optional<std::pair<std::string_view, std::string_view>> credentials;
  for (; i < c.args.size(); ++i) {
    std::string opt = to_lower(c.args[i]);
    if (opt == "auth" && i + 2 < c.args.size()) {
      credentials.emplace(c.args[i + 1], c.args[i + 2]);
      i += 2;
    } else if (opt == "setname" && i + 1 < c.args.size()) {
      name = std::string(c.args[++i]);
    } else {
      return c.r.error(kSyntax);
    }
  }
  if (credentials && !authenticate(c, credentials->first, credentials->second)) return;
  if (!c.client.authenticated) {
    return c.r.error(
        "NOAUTH HELLO must be called with the client already authenticated, otherwise the HELLO <proto> AUTH "
        "<user> <pass> option can be used to authenticate the client and select the RESP protocol version at the "
        "same time");
  }
  if (name) c.client.name = *name;
  c.client.proto = proto;
  c.r.set_proto(proto);
  if (c.client.subscriber) c.client.subscriber->set_proto(proto);  // how messages are sent from now on
  c.r.map(7);
  c.r.bulk("server");
  c.r.bulk("datadam");
  c.r.bulk("version");
  c.r.bulk(kRedisCompatVersion);
  c.r.bulk("proto");
  c.r.integer(proto);
  c.r.bulk("id");
  c.r.integer(static_cast<int64_t>(c.client.id));
  c.r.bulk("mode");
  c.r.bulk("standalone");
  c.r.bulk("role");
  c.r.bulk("master");
  c.r.bulk("modules");
  c.r.array(0);
}

std::string client_info_line(const ClientState& cl) { return client_info(cl) + "\n"; }

// What kind of client a connection is, for CLIENT LIST / KILL TYPE.
bool client_of_type(const ClientEntry& e, std::string_view type) {
  if (type == "pubsub") return e.pubsub.load();
  if (type == "replica" || type == "slave") return e.replica.load();
  if (type == "normal") return !e.pubsub.load() && !e.replica.load() && !e.monitor.load();
  return false;  // master: this server's link to its primary is not a client
}

// CLIENT LIST [TYPE normal|pubsub|replica|master] [ID id [id ...]]
void client_list(Cmd& c) {
  std::optional<std::string> type;
  std::vector<uint64_t> ids;
  for (size_t i = 2; i < c.args.size(); ++i) {
    if (iequals(c.args[i], "type") && i + 1 < c.args.size()) {
      type = to_lower(c.args[++i]);
      if (*type != "normal" && *type != "pubsub" && *type != "replica" && *type != "slave" && *type != "master") {
        return c.r.error(std::format("ERR Unknown client type '{}'", c.args[i]));
      }
    } else if (iequals(c.args[i], "id") && i + 1 < c.args.size()) {
      for (++i; i < c.args.size(); ++i) {
        auto id = parse_uint64(c.args[i]);
        if (!id) return c.r.error("ERR Invalid client ID");
        ids.push_back(*id);
      }
    } else {
      return c.r.error(kSyntax);
    }
  }
  if (!c.client.entry) return c.r.bulk(client_info_line(c.client));  // not on a server: this one only
  std::string out;
  for (const auto& e : c.ctx.clients.all()) {
    if (type && !client_of_type(*e, *type)) continue;
    if (!ids.empty() && std::find(ids.begin(), ids.end(), e->id) == ids.end()) continue;
    out += e->describe();
    out += '\n';
  }
  c.r.bulk(out);
}

// CLIENT KILL ip:port | CLIENT KILL [ID id] [ADDR ip:port] [USER name] [TYPE type] [SKIPME yes|no] [MAXAGE s]
void client_kill(Cmd& c) {
  bool old_form = c.args.size() == 3;
  std::optional<uint64_t> id;
  std::optional<std::string> addr, user, type;
  std::optional<int64_t> max_age;
  bool skip_me = true;
  if (old_form) {
    addr = std::string(c.args[2]);
  } else {
    if (c.args.size() % 2 != 0) return c.r.error(kSyntax);
    for (size_t i = 2; i + 1 < c.args.size(); i += 2) {
      std::string opt = to_lower(c.args[i]);
      std::string_view v = c.args[i + 1];
      if (opt == "id") {
        id = parse_uint64(v);
        if (!id) return c.r.error("ERR client-id should be greater than 0");
      } else if (opt == "addr") {
        addr = std::string(v);
      } else if (opt == "user") {
        user = std::string(v);
      } else if (opt == "type") {
        type = to_lower(v);
      } else if (opt == "skipme") {
        if (iequals(v, "yes")) skip_me = true;
        else if (iequals(v, "no")) skip_me = false;
        else return c.r.error(kSyntax);
      } else if (opt == "maxage") {
        max_age = parse_int64(v);
        if (!max_age) return c.r.error(kNotInteger);
      } else {
        return c.r.error(kSyntax);
      }
    }
  }
  int64_t killed = 0, now = steady_time_ms();
  for (const auto& e : c.ctx.clients.all()) {
    if (id && e->id != *id) continue;
    if (type && !client_of_type(*e, *type)) continue;
    if (max_age && (now - e->created_ms) / 1000 < *max_age) continue;
    {
      std::lock_guard lock(e->mu);
      if (addr && e->addr != *addr) continue;
      if (user && e->user != *user) continue;
    }
    bool me = e->id == c.client.id;
    if (me && skip_me && !old_form) continue;
    if (me) c.client.close_after_reply = true;  // after this reply
    else if (e->kill) e->kill();
    ++killed;
  }
  if (old_form) return killed ? c.r.ok() : c.r.error("ERR No such client");
  c.r.integer(killed);
}

void cmd_client(Cmd& c) {
  std::string sub = to_lower(c.args[1]);
  if (sub == "setname" && c.args.size() == 3) {
    if (c.args[2].find_first_of(" \n") != std::string_view::npos) {
      return c.r.error("ERR Client names cannot contain spaces, newlines or special characters.");
    }
    c.client.name = std::string(c.args[2]);
    return c.r.ok();
  }
  if (sub == "getname" && c.args.size() == 2) {
    if (c.client.name.empty()) return c.r.null();
    return c.r.bulk(c.client.name);
  }
  if (sub == "id" && c.args.size() == 2) return c.r.integer(static_cast<int64_t>(c.client.id));
  if (sub == "setinfo" && c.args.size() == 4) {
    std::string attr = to_lower(c.args[2]);
    if (attr == "lib-name") c.client.lib_name = std::string(c.args[3]);
    else if (attr == "lib-ver") c.client.lib_ver = std::string(c.args[3]);
    else return c.r.error(std::format("ERR Unrecognized option '{}'", c.args[2]));
    return c.r.ok();
  }
  if (sub == "info" && c.args.size() == 2) return c.r.bulk(client_info_line(c.client));
  if (sub == "list") return client_list(c);
  if (sub == "kill" && c.args.size() >= 3) return client_kill(c);
  if ((sub == "no-evict" || sub == "no-touch" || sub == "reply") && c.args.size() == 3) return c.r.ok();
  c.r.error(std::format("ERR unknown subcommand '{}'. Try CLIENT HELP.", c.args[1]));
}

const std::unordered_map<std::string, CommandSpec>& command_table();

void cmd_command(Cmd& c) {
  if (c.args.size() == 1) return c.r.array(0);
  std::string sub = to_lower(c.args[1]);
  if (sub == "count") return c.r.integer(static_cast<int64_t>(command_table().size()));
  if (sub == "docs") return c.r.map(0);
  if (sub == "list") {
    c.r.array(command_table().size());
    for (const auto& [name, spec] : command_table()) c.r.bulk(name);
    return;
  }
  if (sub == "info") {
    c.r.array(c.args.size() - 2);
    for (size_t i = 2; i < c.args.size(); ++i) c.r.null_array();
    return;
  }
  c.r.error(std::format("ERR unknown subcommand '{}'. Try COMMAND HELP.", c.args[1]));
}

void cmd_config(Cmd& c) {
  std::string sub = to_lower(c.args[1]);
  if (sub == "get" && c.args.size() >= 3) {
    auto params = c.ctx.settings().dump();
    // Parameters Redis tooling commonly asks for.
    params.emplace_back("databases", "1");
    params.emplace_back("save", "");
    params.emplace_back("appendonly", "no");
    params.emplace_back("maxmemory", std::to_string(c.ctx.config.cache_max_memory));
    std::vector<std::pair<std::string, std::string>> matched;
    for (const auto& p : params) {
      for (size_t i = 2; i < c.args.size(); ++i) {
        if (glob_match(c.args[i], p.first, true)) {
          matched.push_back(p);
          break;
        }
      }
    }
    c.r.map(matched.size());
    for (const auto& [k, v] : matched) {
      c.r.bulk(k);
      c.r.bulk(v);
    }
    return;
  }
  if (sub == "resetstat" && c.args.size() == 2) {
    c.ctx.db.reset_latency();
    if (c.ctx.read_pool) c.ctx.read_pool->reset_latency();
    return c.r.ok();
  }
  if (sub == "rewrite" && c.args.size() == 2) {
    std::string error = c.ctx.rewrite_config();
    if (!error.empty()) return c.r.error(error);
    return c.r.ok();
  }
  if (sub == "set") {
    // CONFIG SET parameter value [parameter value ...]
    if (c.args.size() < 4 || c.args.size() % 2 != 0) {
      return c.r.error("ERR wrong number of arguments for 'config|set' command");
    }
    std::vector<std::pair<std::string_view, std::string_view>> pairs;
    for (size_t i = 2; i < c.args.size(); i += 2) pairs.emplace_back(c.args[i], c.args[i + 1]);
    std::string error = c.ctx.set_config(pairs);
    if (!error.empty()) return c.r.error(error);
    return c.r.ok();
  }
  c.r.error(std::format("ERR unknown subcommand '{}'. Try CONFIG HELP.", c.args[1]));
}

void cmd_info(Cmd& c) {
  std::string section = c.args.size() > 1 ? to_lower(c.args[1]) : "";
  c.r.bulk(build_info(c.ctx, section));
}

void cmd_time(Cmd& c) {
  auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
  c.r.array(2);
  c.r.bulk(std::to_string(us / 1'000'000));
  c.r.bulk(std::to_string(us % 1'000'000));
}

void cmd_dbsize(Cmd& c) { c.r.integer(static_cast<int64_t>(c.db().approx_size())); }

void cmd_flushall(Cmd& c) {
  if (c.args.size() > 2) return c.r.error(kSyntax);
  if (c.args.size() == 2) {
    std::string mode = to_lower(c.args[1]);
    if (mode != "async" && mode != "sync") return c.r.error(kSyntax);
  }
  c.db().flush_all();
  c.r.ok();
}

// CACHE STATS | CACHE CLASSES | CACHE CLEAR
void cmd_cache(Cmd& c) {
  std::string sub = to_lower(c.args[1]);
  ObjectCache& cache = c.db().cache();
  if (sub == "stats") {
    CacheStats s = cache.stats();
    CacheOptions o = cache.options();
    std::pair<const char*, int64_t> fields[] = {
        {"max_memory", static_cast<int64_t>(o.max_memory)},
        {"min_memory", static_cast<int64_t>(o.min_memory)},
        {"max_object_size", static_cast<int64_t>(o.max_object_size)},
        {"max_idle_ms", o.max_idle_ms},
        {"max_age_ms", o.max_age_ms},
        {"page_size", static_cast<int64_t>(s.page_size)},
        {"pages_in_use", static_cast<int64_t>(s.pages_in_use)},
        {"pages_committed", static_cast<int64_t>(s.pages_committed)},
        {"used_memory", static_cast<int64_t>(s.pages_in_use * s.page_size)},
        {"chunk_bytes", static_cast<int64_t>(s.chunk_bytes)},
        {"payload_bytes", static_cast<int64_t>(s.payload_bytes)},
        {"items", static_cast<int64_t>(s.items)},
        {"hits", static_cast<int64_t>(s.hits)},
        {"misses", static_cast<int64_t>(s.misses)},
        {"inserts", static_cast<int64_t>(s.inserts)},
        {"rejected_too_large", static_cast<int64_t>(s.rejected_too_large)},
        {"rejected_admission", static_cast<int64_t>(s.rejected_admission)},
        {"alloc_failures", static_cast<int64_t>(s.alloc_failures)},
        {"evicted_lru", static_cast<int64_t>(s.evicted_lru)},
        {"evicted_idle", static_cast<int64_t>(s.evicted_idle)},
        {"evicted_age", static_cast<int64_t>(s.evicted_age)},
        {"evicted_ttl", static_cast<int64_t>(s.evicted_ttl)},
        {"page_steals", static_cast<int64_t>(s.page_steals)},
    };
    c.r.map(std::size(fields));
    for (const auto& [k, v] : fields) {
      c.r.bulk(k);
      c.r.integer(v);
    }
    return;
  }
  if (sub == "classes") {
    auto classes = cache.class_stats();
    c.r.array(classes.size());
    for (const auto& cl : classes) {
      c.r.map(3);
      c.r.bulk("chunk_size");
      c.r.integer(static_cast<int64_t>(cl.chunk_size));
      c.r.bulk("pages");
      c.r.integer(static_cast<int64_t>(cl.pages));
      c.r.bulk("items");
      c.r.integer(static_cast<int64_t>(cl.items));
    }
    return;
  }
  if (sub == "clear") {
    cache.clear();
    return c.r.ok();
  }
  c.r.error(std::format("ERR unknown subcommand '{}'. Try CACHE STATS, CACHE CLASSES or CACHE CLEAR.", c.args[1]));
}

// ---- strings ---------------------------------------------------------------

void cmd_get(Cmd& c) {
  bool found = c.db().read(c.args[1], [&](const RecordView& v) {
    if (!is_string(v)) c.r.error(kWrongType);
    else c.r.bulk(v.value);
  });
  if (!found) c.r.null();
}

// SET key value [NX|XX] [GET] [EX s|PX ms|EXAT ts|PXAT ts-ms|KEEPTTL]
void cmd_set(Cmd& c) {
  std::string_view key = c.args[1], value = c.args[2];
  bool nx = false, xx = false, get = false, keepttl = false, has_expire = false;
  int64_t expire_at = 0;
  int64_t now = unix_time_ms();
  for (size_t i = 3; i < c.args.size(); ++i) {
    std::string opt = to_lower(c.args[i]);
    if (opt == "nx" && !xx) {
      nx = true;
    } else if (opt == "xx" && !nx) {
      xx = true;
    } else if (opt == "get") {
      get = true;
    } else if (opt == "keepttl" && !has_expire) {
      keepttl = true;
    } else if ((opt == "ex" || opt == "px" || opt == "exat" || opt == "pxat") && !keepttl && !has_expire &&
               i + 1 < c.args.size()) {
      if (!expire_option(opt, c.args[++i], now, &expire_at)) {
        return c.r.error("ERR invalid expire time in 'set' command");
      }
      has_expire = true;
    } else {
      return c.r.error(kSyntax);
    }
  }

  if (!nx && !xx && !get && !keepttl) {
    Record rec;
    rec.value = std::string(value);
    rec.expire_at_ms = expire_at;
    c.db().put(key, std::move(rec));
    return c.r.ok();
  }

  bool wrongtype = false, written = false;
  std::optional<std::string> old;
  c.db().update(key, [&](std::optional<Record>& rec) {
    if (rec && get) {
      if (!is_string(*rec)) {
        wrongtype = true;
        return Op::Keep;
      }
      old = rec->value;
    }
    if ((nx && rec) || (xx && !rec)) return Op::Keep;
    int64_t exp = (keepttl && rec) ? rec->expire_at_ms : expire_at;
    rec.emplace();
    rec->value = std::string(value);
    rec->expire_at_ms = exp;
    written = true;
    return Op::Put;
  });
  if (wrongtype) return c.r.error(kWrongType);
  if (get) return old ? c.r.bulk(*old) : c.r.null();
  if (written) c.r.ok();
  else c.r.null();
}

void cmd_setnx(Cmd& c) {
  bool written = false;
  c.db().update(c.args[1], [&](std::optional<Record>& rec) {
    if (rec) return Op::Keep;
    rec.emplace();
    rec->value = std::string(c.args[2]);
    written = true;
    return Op::Put;
  });
  c.r.integer(written ? 1 : 0);
}

void setex_generic(Cmd& c, const char* unit, const char* name) {
  int64_t expire_at;
  if (!expire_option(unit, c.args[2], unix_time_ms(), &expire_at)) {
    return c.r.error(std::format("ERR invalid expire time in '{}' command", name));
  }
  Record rec;
  rec.value = std::string(c.args[3]);
  rec.expire_at_ms = expire_at;
  c.db().put(c.args[1], std::move(rec));
  c.r.ok();
}

void cmd_setex(Cmd& c) { setex_generic(c, "ex", "setex"); }
void cmd_psetex(Cmd& c) { setex_generic(c, "px", "psetex"); }

void cmd_getset(Cmd& c) {
  bool wrongtype = false;
  std::optional<std::string> old;
  c.db().update(c.args[1], [&](std::optional<Record>& rec) {
    if (rec) {
      if (!is_string(*rec)) {
        wrongtype = true;
        return Op::Keep;
      }
      old = std::move(rec->value);
    }
    rec.emplace();
    rec->value = std::string(c.args[2]);
    return Op::Put;
  });
  if (wrongtype) return c.r.error(kWrongType);
  old ? c.r.bulk(*old) : c.r.null();
}

void cmd_getdel(Cmd& c) {
  bool wrongtype = false;
  std::optional<std::string> old;
  c.db().update(c.args[1], [&](std::optional<Record>& rec) {
    if (!rec) return Op::Keep;
    if (!is_string(*rec)) {
      wrongtype = true;
      return Op::Keep;
    }
    old = std::move(rec->value);
    return Op::Delete;
  });
  if (wrongtype) return c.r.error(kWrongType);
  old ? c.r.bulk(*old) : c.r.null();
}

// GETEX key [EX s|PX ms|EXAT ts|PXAT ts-ms|PERSIST]
void cmd_getex(Cmd& c) {
  int64_t now = unix_time_ms();
  std::optional<int64_t> new_expire;  // 0 = persist
  if (c.args.size() > 2) {
    std::string opt = to_lower(c.args[2]);
    if (opt == "persist" && c.args.size() == 3) {
      new_expire = 0;
    } else if ((opt == "ex" || opt == "px" || opt == "exat" || opt == "pxat") && c.args.size() == 4) {
      int64_t at;
      if (!expire_option(opt, c.args[3], now, &at)) return c.r.error("ERR invalid expire time in 'getex' command");
      new_expire = at;
    } else {
      return c.r.error(kSyntax);
    }
  }
  bool wrongtype = false;
  std::optional<std::string> value;
  c.db().update(c.args[1], [&](std::optional<Record>& rec) {
    if (!rec) return Op::Keep;
    if (!is_string(*rec)) {
      wrongtype = true;
      return Op::Keep;
    }
    value = rec->value;
    if (!new_expire) return Op::Keep;
    rec->expire_at_ms = *new_expire;
    return Op::PutKeepCas;
  });
  if (wrongtype) return c.r.error(kWrongType);
  value ? c.r.bulk(*value) : c.r.null();
}

void cmd_mget(Cmd& c) {
  c.r.array(c.args.size() - 1);
  for (size_t i = 1; i < c.args.size(); ++i) {
    bool found = c.db().read(c.args[i], [&](const RecordView& v) {
      if (is_string(v)) c.r.bulk(v.value);
      else c.r.null();
    });
    if (!found) c.r.null();
  }
}

bool collect_pairs(Cmd& c, std::vector<std::pair<std::string_view, std::string_view>>* kvs, const char* name) {
  if ((c.args.size() - 1) % 2 != 0) {
    c.r.error(std::format("ERR wrong number of arguments for '{}' command", name));
    return false;
  }
  for (size_t i = 1; i < c.args.size(); i += 2) kvs->emplace_back(c.args[i], c.args[i + 1]);
  return true;
}

void cmd_mset(Cmd& c) {
  std::vector<std::pair<std::string_view, std::string_view>> kvs;
  if (!collect_pairs(c, &kvs, "mset")) return;
  c.db().mset(kvs, false);
  c.r.ok();
}

void cmd_msetnx(Cmd& c) {
  std::vector<std::pair<std::string_view, std::string_view>> kvs;
  if (!collect_pairs(c, &kvs, "msetnx")) return;
  c.r.integer(c.db().mset(kvs, true) ? 1 : 0);
}

void cmd_append(Cmd& c) {
  std::string_view add = c.args[2];
  std::optional<std::string_view> error;
  size_t len = 0;
  c.db().update(c.args[1], [&](std::optional<Record>& rec) {
    if (!rec) {
      rec.emplace();
    } else if (!is_string(*rec)) {
      error = kWrongType;
      return Op::Keep;
    } else if (rec->value.size() + add.size() > kMaxStringSize) {
      error = "ERR string exceeds maximum allowed size (proto-max-bulk-len)";
      return Op::Keep;
    }
    rec->value.append(add);
    len = rec->value.size();
    return Op::Put;
  });
  if (error) return c.r.error(*error);
  c.r.integer(static_cast<int64_t>(len));
}

void cmd_strlen(Cmd& c) {
  bool found = c.db().read(c.args[1], [&](const RecordView& v) {
    if (!is_string(v)) c.r.error(kWrongType);
    else c.r.integer(static_cast<int64_t>(v.value.size()));
  });
  if (!found) c.r.integer(0);
}

void cmd_getrange(Cmd& c) {
  auto start = parse_int64(c.args[2]);
  auto end = parse_int64(c.args[3]);
  if (!start || !end) return c.r.error(kNotInteger);
  bool found = c.db().read(c.args[1], [&](const RecordView& v) {
    if (!is_string(v)) return c.r.error(kWrongType);
    int64_t len = static_cast<int64_t>(v.value.size());
    int64_t s = *start, e = *end;
    if ((s < 0 && e < 0 && s > e) || len == 0) return c.r.bulk("");
    if (s < 0) s = std::max<int64_t>(len + s, 0);
    if (e < 0) e = std::max<int64_t>(len + e, 0);
    if (e >= len) e = len - 1;
    if (s > e) return c.r.bulk("");
    c.r.bulk(v.value.substr(static_cast<size_t>(s), static_cast<size_t>(e - s + 1)));
  });
  if (!found) c.r.bulk("");
}

void cmd_setrange(Cmd& c) {
  auto offset = parse_int64(c.args[2]);
  if (!offset) return c.r.error(kNotInteger);
  if (*offset < 0) return c.r.error("ERR offset is out of range");
  std::string_view value = c.args[3];
  if (static_cast<uint64_t>(*offset) + value.size() > kMaxStringSize) {
    return c.r.error("ERR string exceeds maximum allowed size (proto-max-bulk-len)");
  }
  bool wrongtype = false;
  size_t len = 0;
  c.db().update(c.args[1], [&](std::optional<Record>& rec) {
    if (rec && !is_string(*rec)) {
      wrongtype = true;
      return Op::Keep;
    }
    if (value.empty()) {
      len = rec ? rec->value.size() : 0;
      return Op::Keep;
    }
    if (!rec) rec.emplace();
    size_t off = static_cast<size_t>(*offset);
    if (rec->value.size() < off + value.size()) rec->value.resize(off + value.size(), '\0');
    rec->value.replace(off, value.size(), value);
    len = rec->value.size();
    return Op::Put;
  });
  if (wrongtype) return c.r.error(kWrongType);
  c.r.integer(static_cast<int64_t>(len));
}

void incr_by(Cmd& c, int64_t delta) {
  std::optional<std::string_view> error;
  int64_t result = 0;
  c.db().update(c.args[1], [&](std::optional<Record>& rec) {
    int64_t cur = 0;
    if (rec) {
      if (!is_string(*rec)) {
        error = kWrongType;
        return Op::Keep;
      }
      auto v = parse_int64(rec->value);
      if (!v) {
        error = kNotInteger;
        return Op::Keep;
      }
      cur = *v;
    }
    if ((delta > 0 && cur > INT64_MAX - delta) || (delta < 0 && cur < INT64_MIN - delta)) {
      error = "ERR increment or decrement would overflow";
      return Op::Keep;
    }
    result = cur + delta;
    if (!rec) rec.emplace();
    rec->value = std::to_string(result);
    return Op::Put;
  });
  if (error) return c.r.error(*error);
  c.r.integer(result);
}

void cmd_incr(Cmd& c) { incr_by(c, 1); }
void cmd_decr(Cmd& c) { incr_by(c, -1); }

void cmd_incrby(Cmd& c) {
  auto d = parse_int64(c.args[2]);
  if (!d) return c.r.error(kNotInteger);
  incr_by(c, *d);
}

void cmd_decrby(Cmd& c) {
  auto d = parse_int64(c.args[2]);
  if (!d) return c.r.error(kNotInteger);
  if (*d == INT64_MIN) return c.r.error("ERR decrement would overflow");
  incr_by(c, -*d);
}

void cmd_incrbyfloat(Cmd& c) {
  auto incr = parse_long_double(c.args[2]);
  if (!incr) return c.r.error(kNotFloat);
  std::optional<std::string_view> error;
  std::string result;
  c.db().update(c.args[1], [&](std::optional<Record>& rec) {
    long double cur = 0;
    if (rec) {
      if (!is_string(*rec)) {
        error = kWrongType;
        return Op::Keep;
      }
      auto v = parse_long_double(rec->value);
      if (!v) {
        error = kNotFloat;
        return Op::Keep;
      }
      cur = *v;
    }
    long double sum = cur + *incr;
    if (std::isnan(sum) || std::isinf(sum)) {
      error = "ERR increment would produce NaN or Infinity";
      return Op::Keep;
    }
    result = format_long_double(sum);
    if (!rec) rec.emplace();
    rec->value = result;
    return Op::Put;
  });
  if (error) return c.r.error(*error);
  c.r.bulk(result);
}

// ---- keyspace --------------------------------------------------------------

void cmd_del(Cmd& c) {
  int64_t n = 0;
  for (size_t i = 1; i < c.args.size(); ++i) n += c.db().del(c.args[i]) ? 1 : 0;
  c.r.integer(n);
}

void cmd_exists(Cmd& c) {
  int64_t n = 0;
  for (size_t i = 1; i < c.args.size(); ++i) n += c.db().exists(c.args[i]) ? 1 : 0;
  c.r.integer(n);
}

void cmd_type(Cmd& c) {
  std::string_view name = "none";
  c.db().read(c.args[1], [&](const RecordView& v) { name = value_type_name(v.type); });
  c.r.simple(name);
}

// EXPIRE / PEXPIRE / EXPIREAT / PEXPIREAT key time [NX|XX|GT|LT]
void expire_generic(Cmd& c, int64_t unit_ms, bool absolute, const char* name) {
  auto v = parse_int64(c.args[2]);
  if (!v) return c.r.error(kNotInteger);
  bool nx = false, xx = false, gt = false, lt = false;
  for (size_t i = 3; i < c.args.size(); ++i) {
    std::string opt = to_lower(c.args[i]);
    if (opt == "nx") nx = true;
    else if (opt == "xx") xx = true;
    else if (opt == "gt") gt = true;
    else if (opt == "lt") lt = true;
    else return c.r.error(std::format("ERR Unsupported option {}", c.args[i]));
  }
  if (nx && (xx || gt || lt)) {
    return c.r.error("ERR NX and XX, GT or LT options at the same time are not compatible");
  }
  if (gt && lt) return c.r.error("ERR GT and LT options at the same time are not compatible");

  int64_t now = unix_time_ms();
  int64_t when = *v;
  if (when > INT64_MAX / unit_ms || when < INT64_MIN / unit_ms) {
    return c.r.error(std::format("ERR invalid expire time in '{}' command", name));
  }
  when *= unit_ms;
  if (!absolute) {
    if (when > INT64_MAX - now) return c.r.error(std::format("ERR invalid expire time in '{}' command", name));
    when += now;
  }

  int64_t result = 0;
  c.db().update(c.args[1], [&](std::optional<Record>& rec) {
    if (!rec) return Op::Keep;
    int64_t cur = rec->expire_at_ms;  // 0 = no TTL, i.e. infinite
    if (nx && cur != 0) return Op::Keep;
    if (xx && cur == 0) return Op::Keep;
    if (gt && (cur == 0 || when <= cur)) return Op::Keep;
    if (lt && cur != 0 && when >= cur) return Op::Keep;
    result = 1;
    if (when <= now) return Op::Delete;
    rec->expire_at_ms = when;
    return Op::PutKeepCas;
  });
  c.r.integer(result);
}

void cmd_expire(Cmd& c) { expire_generic(c, 1000, false, "expire"); }
void cmd_pexpire(Cmd& c) { expire_generic(c, 1, false, "pexpire"); }
void cmd_expireat(Cmd& c) { expire_generic(c, 1000, true, "expireat"); }
void cmd_pexpireat(Cmd& c) { expire_generic(c, 1, true, "pexpireat"); }

enum class TtlKind { Ttl, Pttl, ExpireTime, PExpireTime };

void ttl_generic(Cmd& c, TtlKind kind) {
  int64_t result = -2;
  int64_t now = unix_time_ms();
  c.db().read(c.args[1], [&](const RecordView& v) {
    if (v.expire_at_ms == 0) {
      result = -1;
      return;
    }
    int64_t remaining = std::max<int64_t>(v.expire_at_ms - now, 0);
    switch (kind) {
      case TtlKind::Ttl: result = (remaining + 500) / 1000; break;
      case TtlKind::Pttl: result = remaining; break;
      case TtlKind::ExpireTime: result = v.expire_at_ms / 1000; break;
      case TtlKind::PExpireTime: result = v.expire_at_ms; break;
    }
  });
  c.r.integer(result);
}

void cmd_ttl(Cmd& c) { ttl_generic(c, TtlKind::Ttl); }
void cmd_pttl(Cmd& c) { ttl_generic(c, TtlKind::Pttl); }
void cmd_expiretime(Cmd& c) { ttl_generic(c, TtlKind::ExpireTime); }
void cmd_pexpiretime(Cmd& c) { ttl_generic(c, TtlKind::PExpireTime); }

void cmd_persist(Cmd& c) {
  int64_t result = 0;
  c.db().update(c.args[1], [&](std::optional<Record>& rec) {
    if (!rec || rec->expire_at_ms == 0) return Op::Keep;
    rec->expire_at_ms = 0;
    result = 1;
    return Op::PutKeepCas;
  });
  c.r.integer(result);
}

void cmd_rename(Cmd& c) {
  switch (c.db().rename(c.args[1], c.args[2], false)) {
    case Database::RenameResult::NoSuchKey: return c.r.error("ERR no such key");
    default: return c.r.ok();
  }
}

void cmd_renamenx(Cmd& c) {
  switch (c.db().rename(c.args[1], c.args[2], true)) {
    case Database::RenameResult::NoSuchKey: return c.r.error("ERR no such key");
    case Database::RenameResult::TargetExists: return c.r.integer(0);
    default: return c.r.integer(1);
  }
}

void cmd_keys(Cmd& c) {
  auto keys = c.db().keys(c.args[1]);
  if (!reads_all_keys(c)) std::erase_if(keys, [&](const std::string& k) { return !may_read_key(c, k); });
  c.r.array(keys.size());
  for (const auto& k : keys) c.r.bulk(k);
}

std::unordered_map<std::string, CommandSpec> build_command_table() {
  std::unordered_map<std::string, CommandSpec> table = {
      // connection / server
      {"ping", {cmd_ping, -1}},
      {"echo", {cmd_echo, 2}},
      {"quit", {cmd_quit, -1}},
      {"select", {cmd_select, 2}},
      {"auth", {cmd_auth, -2}},
      {"hello", {cmd_hello, -1}},
      {"client", {cmd_client, -2}},
      {"command", {cmd_command, -1}},
      {"config", {cmd_config, -2}},
      {"info", {cmd_info, -1}},
      {"time", {cmd_time, 1}},
      {"dbsize", {cmd_dbsize, 1}},
      {"flushdb", {cmd_flushall, -1}},
      {"flushall", {cmd_flushall, -1}},
      {"cache", {cmd_cache, 2}},
      // strings
      {"get", {cmd_get, 2}},
      {"set", {cmd_set, -3}},
      {"setnx", {cmd_setnx, 3}},
      {"setex", {cmd_setex, 4}},
      {"psetex", {cmd_psetex, 4}},
      {"getset", {cmd_getset, 3}},
      {"getdel", {cmd_getdel, 2}},
      {"getex", {cmd_getex, -2}},
      {"mget", {cmd_mget, -2}},
      {"mset", {cmd_mset, -3}},
      {"msetnx", {cmd_msetnx, -3}},
      {"append", {cmd_append, 3}},
      {"strlen", {cmd_strlen, 2}},
      {"getrange", {cmd_getrange, 4}},
      {"substr", {cmd_getrange, 4}},
      {"setrange", {cmd_setrange, 4}},
      {"incr", {cmd_incr, 2}},
      {"decr", {cmd_decr, 2}},
      {"incrby", {cmd_incrby, 3}},
      {"decrby", {cmd_decrby, 3}},
      {"incrbyfloat", {cmd_incrbyfloat, 3}},
      // keyspace
      {"del", {cmd_del, -2}},
      {"unlink", {cmd_del, -2}},
      {"exists", {cmd_exists, -2}},
      {"type", {cmd_type, 2}},
      {"expire", {cmd_expire, -3}},
      {"pexpire", {cmd_pexpire, -3}},
      {"expireat", {cmd_expireat, -3}},
      {"pexpireat", {cmd_pexpireat, -3}},
      {"ttl", {cmd_ttl, 2}},
      {"pttl", {cmd_pttl, 2}},
      {"expiretime", {cmd_expiretime, 2}},
      {"pexpiretime", {cmd_pexpiretime, 2}},
      {"persist", {cmd_persist, 2}},
      {"rename", {cmd_rename, 3}},
      {"renamenx", {cmd_renamenx, 3}},
      {"keys", {cmd_keys, 2}},
  };
  for (const auto& list : {hash_commands(), set_commands(), zset_commands(), list_commands(), index_commands(),
                            scan_commands(), acl_commands(), cdc_commands(), replication_commands(),
                            json_commands(), search_commands(), transaction_commands(), pubsub_commands(), script_commands(),
                            admin_commands()}) {
    table.insert(list.begin(), list.end());
  }
  // ACL metadata and dense ids (in name order, so ids are stable for a build).
  std::vector<std::string> names;
  for (const auto& [name, spec] : table) names.push_back(name);
  std::sort(names.begin(), names.end());
  uint16_t id = 0;
  for (const auto& name : names) {
    CommandSpec& spec = table[name];
    spec.id = id++;
    if (const CommandMeta* meta = command_meta(name)) {
      spec.categories = meta->categories;
      spec.keys[0] = meta->keys[0];
      spec.keys[1] = meta->keys[1];
    } else {
      spec.categories = kCatAdmin | kCatDangerous;  // unknown to ACL: only full-access users
    }
  }
  return table;
}

const std::unordered_map<std::string, CommandSpec>& command_table() {
  static const std::unordered_map<std::string, CommandSpec> table = build_command_table();
  return table;
}

}  // namespace

std::string client_info(const ClientState& cl) {
  return std::format("id={} addr={} name={} db=0 resp={} user={} tls={} lib-name={} lib-ver={}", cl.id, cl.addr,
                     cl.name, cl.proto, cl.user ? cl.user->name : "default", cl.tls ? "yes" : "no", cl.lib_name,
                     cl.lib_ver);
}

const std::unordered_map<std::string, CommandSpec>& all_commands() { return command_table(); }

const CommandSpec* find_command(std::string_view lower_name) {
  const auto& table = command_table();
  auto it = table.find(std::string(lower_name));
  return it == table.end() ? nullptr : &it->second;
}

namespace {

// Commands that only read keys through Database::read/get/exists and have no
// other effect, so running one again from the start is harmless.
bool rerunnable(const CommandSpec& spec) {
  static const std::bitset<1024> ids = [] {
    std::bitset<1024> b;
    for (const char* name : {"get", "mget", "exists", "strlen", "getrange", "substr", "type", "ttl", "pttl",
                             "expiretime", "pexpiretime", "json.get", "json.mget", "json.type", "json.strlen",
                             "json.arrlen", "json.arrindex", "json.objkeys", "json.objlen"}) {
      if (const CommandSpec* c = find_command(name)) b.set(c->id);
    }
    return b;
  }();
  return ids.test(spec.id);
}

// Counts one command into this thread's ThreadStats when it returns: run
// (calls, time, failed), refused (rejected) or neither (queued in MULTI, or to
// run again later), and its error replies. A command EXEC runs writes into
// the same Reply: its errors are its own, not EXEC's.
class CommandAccounting {
 public:
  CommandAccounting(ServerStats& stats, ClientState& client, Reply& reply)
      : stats_(stats), client_(client), reply_(reply), errors_before_(reply.errors()), parent_(current_) {
    current_ = this;
  }
  ~CommandAccounting() {
    current_ = parent_;
    if (client_.retry_later) return;  // counted when it runs again
    uint64_t errors = reply_.errors() - errors_before_;
    if (parent_ && &parent_->reply_ == &reply_) parent_->nested_errors_ += errors;
    errors -= std::min(errors, nested_errors_);
    ThreadStats& t = stats_.threads.local();
    if (spec_ && ran_) {
      ThreadStats::Command& c = t.by_command[spec_->id];
      bump(t.commands);
      bump(c.calls);
      bump(c.nsec, static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             std::chrono::steady_clock::now() - started_)
                                             .count()));
      if (errors > 0) bump(c.failed);
    } else if (spec_ && errors > 0) {
      bump(t.by_command[spec_->id].rejected);
    }
    if (errors > 0) {
      bump(t.error_replies, errors);
      std::lock_guard lock(t.errors_mu);
      auto it = t.errors.find(reply_.last_error_code());
      if (it == t.errors.end()) it = t.errors.emplace(std::string(reply_.last_error_code()), 0).first;
      it->second += errors;
    }
  }
  CommandAccounting(const CommandAccounting&) = delete;
  CommandAccounting& operator=(const CommandAccounting&) = delete;

  void known(const CommandSpec& spec) { spec_ = &spec; }
  void running() {
    ran_ = true;
    started_ = std::chrono::steady_clock::now();
  }
  std::chrono::steady_clock::time_point started() const { return started_; }

 private:
  ServerStats& stats_;
  ClientState& client_;
  Reply& reply_;
  uint64_t errors_before_;
  uint64_t nested_errors_ = 0;
  CommandAccounting* parent_;
  const CommandSpec* spec_ = nullptr;
  bool ran_ = false;
  std::chrono::steady_clock::time_point started_;
  static thread_local CommandAccounting* current_;
};

thread_local CommandAccounting* CommandAccounting::current_ = nullptr;

}  // namespace

void execute_redis_command(ServerContext& ctx, ClientState& client, Args args, Reply& reply) {
  CommandAccounting accounting(ctx.stats, client, reply);
  std::string name = to_lower(args[0]);
  const auto& table = command_table();
  auto it = table.find(name);
  // Users may have changed since this connection last looked: reload its own.
  if (client.user && client.acl_version != ctx.acl.version()) {
    client.acl_version = ctx.acl.version();
    client.user = ctx.acl.find(client.user->name);
    if (!client.user) {  // deleted: the connection goes, as in Redis
      client.authenticated = false;
      client.close_after_reply = true;
      return reply.error("ERR the user of this connection was deleted");
    }
  }
  if (!client.authenticated && name != "auth" && name != "hello" && name != "quit") {
    return reply.error("NOAUTH Authentication required.");
  }
  if (it == table.end()) {
    if (client.in_multi) client.multi_error = true;
    std::string msg = std::format("ERR unknown command '{}', with args beginning with: ", args[0]);
    for (size_t i = 1; i < args.size() && i < 4; ++i) msg += std::format("'{}' ", args[i].substr(0, 64));
    return reply.error(msg);
  }
  const CommandSpec& spec = it->second;
  accounting.known(spec);
  int argc = static_cast<int>(args.size());
  if ((spec.arity > 0 && argc != spec.arity) || (spec.arity < 0 && argc < -spec.arity)) {
    if (client.in_multi) client.multi_error = true;
    return reply.error(std::format("ERR wrong number of arguments for '{}' command", name));
  }
  if (client.user && !client.user->unrestricted) {
    if (AclDenial denied = acl_check(*client.user, name, spec, args)) {
      ctx.stats.acl_denied.fetch_add(1, std::memory_order_relaxed);
      ctx.acl_log.add(denied.reason, denied.object, client.user->name, client_info(client));
      if (client.in_multi) client.multi_error = true;
      return reply.error(denied.message);
    }
  }
  // RESP2 has no way to tell messages from replies: a subscribed connection
  // may only (un)subscribe, PING or leave.
  if (client.subscriber && client.subscriber->count() > 0 && reply.proto() < 3 &&
      !((spec.categories & kCatPubSub) && name.find("subscribe") != std::string::npos) && name != "ping" &&
      name != "quit" && name != "reset") {
    return reply.error(std::format(
        "ERR Can't execute '{}': only (P|S)SUBSCRIBE / (P|S)UNSUBSCRIBE / PING / QUIT / RESET are allowed in "
        "this context",
        name));
  }
  // A script that runs too long: everyone else waits for it, or kills it.
  if (!client.in_script && ctx.scripts.running_ms() > kScriptBusyMs && !allowed_while_script_busy(name, args)) {
    if (client.in_multi) client.multi_error = true;
    return reply.error(
        "BUSY Redis is busy running a script. You can only call SCRIPT KILL, FUNCTION KILL or SHUTDOWN NOSAVE.");
  }
  if (client.in_multi && !runs_in_multi(name)) {
    client.queued.emplace_back(args.begin(), args.end());
    return reply.simple("QUEUED");
  }
  if (!client.in_script && client.can_block &&
      (spec.categories & (kCatRead | kCatWrite | kCatScripting | kCatTransaction)) &&
      ctx.scripts.running_us() >= kScriptParkUs && !allowed_while_script_busy(name, args)) {
    if (auto done = ctx.scripts.wait_until_done()) {
      client.retry_later = true;  // counted when it runs
      client.wait_for_script = true;
      client.key_waiter = std::move(done);
      return;
    }
  }
  if ((spec.categories & kCatWrite) && ctx.db.read_only()) {
    return reply.error("READONLY You can't write against a read only replica.");
  }
  if ((spec.categories & kCatWrite) && ctx.writes_paused.load(std::memory_order_relaxed)) {
    return reply.error("READONLY datadam is handing over to a replica (FAILOVER)");
  }
  if ((spec.categories & (kCatRead | kCatWrite)) && ctx.repl.syncing()) {
    return reply.error("LOADING datadam is loading the dataset from its primary");
  }
  if (client.entry) {
    client.entry->last_ms.store(steady_time_ms(), std::memory_order_relaxed);
    client.entry->last_cmd.store(&it->first, std::memory_order_relaxed);
  }
  accounting.running();
  Cmd cmd{ctx, client, args, reply};
  size_t mark = reply.mark();
  try {
    if (client.defer_blocked_reads && rerunnable(spec)) {
      Database::NonBlockingReads nonblocking;
      spec.fn(cmd);
    } else {
      spec.fn(cmd);
    }
  } catch (Database::WouldBlock& e) {
    // The key is on disk: drop the partial reply; the connection has the key
    // loaded off this thread and runs the command again.
    reply.rewind(mark);
    client.retry_later = true;
    client.load_key = std::move(e.key);
  } catch (const IndexConstraintError& e) {
    reply.error(e.what());
  } catch (const StorageError& e) {
    reply.error(std::format("ERR storage error: {}", e.what()));
  } catch (const std::exception& e) {
    reply.error(std::format("ERR internal error: {}", e.what()));
  }
  // A command that runs again later is counted when it really runs.
  if (client.retry_later) return;
  client.block_deadline_ms = 0;  // a blocking command that answered is not blocked any more
  if (ctx.slowlog.enabled()) {
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                                    accounting.started());
    if (ctx.slowlog.wants(us.count())) ctx.slowlog.add(args, us.count(), client.addr, client.name);
  }
  if (ctx.monitors.any()) ctx.monitors.feed(client.id, client.addr, args);
  if (client.entry && (name == "client" || name == "auth" || name == "hello")) publish_identity(client);
}

void publish_identity(ClientState& client) {
  if (!client.entry) return;
  ClientEntry& e = *client.entry;
  std::lock_guard lock(e.mu);
  e.addr = client.addr;
  e.name = client.name;
  e.user = client.user ? client.user->name : "default";
  e.lib_name = client.lib_name;
  e.lib_ver = client.lib_ver;
  e.proto = client.proto;
  e.tls = client.tls;
}

std::optional<double> parse_block_timeout(Cmd& c, std::string_view arg) {
  auto v = parse_double(arg);
  if (!v || !std::isfinite(*v)) {
    c.r.error("ERR timeout is not a float or out of range");
    return std::nullopt;
  }
  if (*v < 0) {
    c.r.error("ERR timeout is negative");
    return std::nullopt;
  }
  return v;
}

bool block_for_keys(Cmd& c, std::span<const std::string_view> keys, double timeout_s) {
  ClientState& client = c.client;
  if (!client.can_block) return false;
  int64_t now = steady_time_ms();
  if (client.block_deadline_ms == 0) {  // the first try; later ones keep the deadline
    client.block_deadline_ms = timeout_s == 0 ? std::numeric_limits<int64_t>::max()
                                              : now + std::max<int64_t>(1, std::llround(timeout_s * 1000));
  }
  if (now >= client.block_deadline_ms) return false;
  auto waiter = std::make_shared<KeyWatch::Waiter>();
  client.wait_keys.clear();
  for (auto k : keys) {
    c.db().key_watch().wait(k, waiter);
    client.wait_keys.emplace_back(k);
  }
  client.key_waiter = std::move(waiter);
  client.wait_for_keys = true;
  client.retry_later = true;
  return true;
}

void uncached_read_keys(ServerContext& ctx, const ClientState& client, Args args,
                        const std::function<void(std::string_view)>& load) {
  if (args.empty() || !client.authenticated) return;
  const CommandSpec* spec = find_command(to_lower(args[0]));
  if (!spec || !rerunnable(*spec)) return;
  const AclUser* user = client.user.get();
  for_each_key(*spec, args, [&](std::string_view key, uint8_t access) {
    if (user && !user->unrestricted && !user->may_access(key, access)) return true;
    if (!ctx.db.cache().lookup(key, [](const RecordView&) {}, false)) load(key);
    return true;
  });
}

std::vector<CommandTotals> command_totals(const ServerStats& stats) {
  std::vector<CommandTotals> by_id(kMaxAclCommands);
  stats.threads.for_each([&](const ThreadStats& t) {
    for (size_t i = 0; i < kMaxAclCommands; ++i) {
      const ThreadStats::Command& c = t.by_command[i];
      by_id[i].calls += c.calls.load(std::memory_order_relaxed);
      by_id[i].nsec += c.nsec.load(std::memory_order_relaxed);
      by_id[i].failed += c.failed.load(std::memory_order_relaxed);
      by_id[i].rejected += c.rejected.load(std::memory_order_relaxed);
    }
  });
  std::vector<CommandTotals> out;
  for (const auto& [name, spec] : command_table()) {
    CommandTotals c = by_id[spec.id];
    if (c.calls == 0 && c.rejected == 0) continue;
    c.name = name;
    out.push_back(c);
  }
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
  return out;
}

std::vector<std::pair<std::string, uint64_t>> error_totals(const ServerStats& stats) {
  std::map<std::string, uint64_t, std::less<>> sum;
  stats.threads.for_each([&](const ThreadStats& t) {
    std::lock_guard lock(t.errors_mu);
    for (const auto& [code, n] : t.errors) sum[code] += n;
  });
  return {sum.begin(), sum.end()};
}

std::vector<std::pair<std::string_view, const LatencyHistogram*>> latency_histograms(ServerContext& ctx) {
  RocksStorage& st = ctx.db.storage();
  std::vector<std::pair<std::string_view, const LatencyHistogram*>> out = {
      {"storage_get_usec", &st.get_latency()},       {"storage_multiget_usec", &st.multi_get_latency()},
      {"storage_write_usec", &st.write_latency()},   {"key_lock_wait_usec", &ctx.db.lock_waits()},
      {"key_read_lock_wait_usec", &ctx.db.read_lock_waits()}, {"write_batch_hold_usec", &ctx.db.batch_hold()}};
  if (ctx.read_pool) out.emplace_back("read_pool_load_usec", &ctx.read_pool->latency());
  return out;
}

std::string build_info(ServerContext& ctx, std::string_view section) {
  bool all = section.empty() || section == "all" || section == "everything" || section == "default";
  auto want = [&](std::string_view s) { return all || section == s; };
  // As in Redis, only on request: one line per command used.
  auto want_extra = [&](std::string_view s) { return section == "all" || section == "everything" || section == s; };
  std::string out;
  int64_t uptime_s = (unix_time_ms() - ctx.start_time_ms) / 1000;

  if (want("server")) {
    out += std::format(
        "# Server\r\ndatadam_version:{}\r\nredis_version:{}\r\nredis_mode:standalone\r\nprocess_id:{}\r\nrun_id:{}\r\n"
        "tcp_port:{}\r\nmemcache_port:{}\r\nio_threads:{}\r\nuptime_in_seconds:{}\r\nuptime_in_days:{}\r\n\r\n",
        DATADAM_VERSION, kRedisCompatVersion, DATADAM_GETPID(), ctx.run_id, ctx.config.port, ctx.config.memcache_port,
        ctx.config.io_thread_count(), uptime_s, uptime_s / 86400);
  }
  if (want("clients")) {
    size_t blocked = 0, pubsub = 0;
    for (const auto& e : ctx.clients.all()) {
      blocked += e->blocked.load() ? 1 : 0;
      pubsub += e->pubsub.load() ? 1 : 0;
    }
    out += std::format("# Clients\r\nconnected_clients:{}\r\nblocked_clients:{}\r\npubsub_clients:{}\r\n\r\n",
                       ctx.stats.connected_clients.load(), blocked, pubsub);
  }
  if (want("stats")) {
    out += std::format(
        "# Stats\r\ntotal_connections_received:{}\r\ntotal_commands_processed:{}\r\ninstantaneous_ops_per_sec:{}\r\n"
        "total_net_input_bytes:{}\r\ntotal_net_output_bytes:{}\r\ninstantaneous_input_kbps:{:.2f}\r\n"
        "instantaneous_output_kbps:{:.2f}\r\nkeyspace_hits:{}\r\nkeyspace_misses:{}\r\nevicted_keys:0\r\n"
        "total_error_replies:{}\r\nauth_failures:{}\r\n"
        "acl_access_denied:{}\r\ntls_connections:{}\r\ntls_handshake_failures:{}\r\ndeferred_reads:{}\r\nmemory_reads:{}\r\n"
        "read_pool_loads:{}\r\nread_pool_batches:{}\r\nread_pool_joined:{}\r\nread_pool_queued:{}\r\n\r\n",
        ctx.stats.total_connections.load(), ctx.stats.total_commands(), ctx.stats.instantaneous_ops.load(),
        ctx.stats.total_net_input_bytes(), ctx.stats.total_net_output_bytes(),
        static_cast<double>(ctx.stats.instantaneous_input_bps.load()) / 1024,
        static_cast<double>(ctx.stats.instantaneous_output_bps.load()) / 1024, ctx.db.keyspace_hits(),
        ctx.db.keyspace_misses(), ctx.stats.total_error_replies(), ctx.stats.auth_failures.load(),
        ctx.stats.acl_denied.load(), ctx.stats.tls_connections.load(), ctx.stats.tls_handshake_failures.load(),
        ctx.db.deferred_reads(), ctx.db.memory_reads(), ctx.read_pool ? ctx.read_pool->loads() : 0,
        ctx.read_pool ? ctx.read_pool->batches() : 0, ctx.read_pool ? ctx.read_pool->joined() : 0,
        ctx.read_pool ? ctx.read_pool->queued() : 0);
  }
  if (want("memory")) {
    // used_memory: what holds data (object cache, memtables, block cache);
    // used_memory_rss: the whole process.
    RocksStorage& st = ctx.db.storage();
    CacheStats cs = ctx.db.cache().stats();
    auto number = [&](const char* name) { return parse_uint64(st.property(name)).value_or(0); };
    uint64_t used = cs.pages_in_use * cs.page_size + number("rocksdb.cur-size-all-mem-tables") +
                    number("rocksdb.block-cache-usage");
    out += std::format("# Memory\r\nused_memory:{}\r\nused_memory_rss:{}\r\n\r\n", used, process_rss_bytes());
  }
  if (want("cache") || want("memory")) {
    ObjectCache& cache = ctx.db.cache();
    CacheStats s = cache.stats();
    const CacheOptions& o = cache.options();
    uint64_t lookups = s.hits + s.misses;
    out += std::format(
        "# Cache\r\ncache_max_memory:{}\r\ncache_min_memory:{}\r\ncache_max_object_size:{}\r\n"
        "cache_max_idle_ms:{}\r\ncache_max_age_ms:{}\r\ncache_page_size:{}\r\ncache_pages_in_use:{}\r\n"
        "cache_pages_committed:{}\r\ncache_used_memory:{}\r\ncache_chunk_bytes:{}\r\ncache_payload_bytes:{}\r\n"
        "cache_items:{}\r\ncache_hits:{}\r\ncache_misses:{}\r\ncache_hit_ratio:{:.4f}\r\n"
        "cache_rejected_too_large:{}\r\ncache_rejected_admission:{}\r\ncache_alloc_failures:{}\r\ncache_evicted_lru:{}\r\n"
        "cache_evicted_idle:{}\r\ncache_evicted_age:{}\r\ncache_evicted_ttl:{}\r\ncache_page_steals:{}\r\n\r\n",
        o.max_memory, o.min_memory, o.max_object_size, o.max_idle_ms, o.max_age_ms, s.page_size, s.pages_in_use,
        s.pages_committed, s.pages_in_use * s.page_size, s.chunk_bytes, s.payload_bytes, s.items, s.hits,
        s.misses, lookups ? static_cast<double>(s.hits) / static_cast<double>(lookups) : 0.0,
        s.rejected_too_large, s.rejected_admission, s.alloc_failures, s.evicted_lru, s.evicted_idle, s.evicted_age, s.evicted_ttl,
        s.page_steals);
  }
  if (want("persistence")) {
    RocksStorage& st = ctx.db.storage();
    WriteBatchStats wb = ctx.db.write_batch_stats();
    Backups::Status backup = ctx.backups.status();
    out += std::format(
        "# Persistence\r\nengine:rocksdb\r\ndir:{}\r\nfsync:{}\r\nwrite_batching:{}\r\nwrite_batches:{}\r\n"
        "write_batch_ops:{}\r\nwrite_batch_conflicts:{}\r\nrocksdb_estimate_num_keys:{}\r\n"
        "rocksdb_live_sst_bytes:{}\r\nrocksdb_live_wal_bytes:{}\r\nrocksdb_memtable_bytes:{}\r\n"
        "rocksdb_block_cache_usage:{}\r\n"
        "rdb_bgsave_in_progress:{}\r\nrdb_last_save_time:{}\r\nrdb_last_bgsave_status:{}\r\n"
        "last_backup_path:{}\r\nlast_backup_duration_ms:{}\r\n",
        ctx.config.dir, fsync_policy_name(st.fsync_policy()), ctx.write_batching ? "yes" : "no", wb.commits,
        wb.writes, wb.conflicts, st.estimate_keys(), st.property("rocksdb.live-sst-files-size"),
        st.live_wal_bytes(), st.property("rocksdb.cur-size-all-mem-tables"), st.property("rocksdb.block-cache-usage"),
        backup.in_progress ? 1 : 0, backup.last_save_s, backup.last_ok ? "ok" : "err", backup.last_path,
        backup.last_duration_ms);
    RocksStorage::WriteStalls ws = st.write_stalls();
    out += std::format(
        "rocksdb_write_stall_delays:{}\r\nrocksdb_write_stall_stops:{}\r\nrocksdb_stall_memtable_delays:{}\r\n"
        "rocksdb_stall_memtable_stops:{}\r\nrocksdb_stall_l0_delays:{}\r\nrocksdb_stall_l0_stops:{}\r\n"
        "rocksdb_stall_pending_compaction_delays:{}\r\nrocksdb_stall_pending_compaction_stops:{}\r\n"
        "rocksdb_write_stopped:{}\r\nrocksdb_delayed_write_rate:{}\r\nrocksdb_immutable_memtables:{}\r\n"
        "rocksdb_pending_compaction_bytes:{}\r\nrocksdb_running_flushes:{}\r\nrocksdb_running_compactions:{}\r\n\r\n",
        ws.delays, ws.stops, ws.memtable_delays, ws.memtable_stops, ws.l0_delays, ws.l0_stops,
        ws.pending_compaction_delays, ws.pending_compaction_stops, ws.write_stopped ? 1 : 0, ws.delayed_write_rate,
        ws.immutable_memtables, ws.pending_compaction_bytes, ws.running_flushes, ws.running_compactions);
  }
  if (want("latency")) {
    // Microseconds; a percentile is the upper bound of its histogram bucket (within 25%).
    auto line = [](std::string_view name, const LatencyHistogram& h) {
      return std::format("{}:calls={},p50={},p99={},p99.9={},p99.99={},max={}\r\n", name, h.count(),
                         h.percentile(0.5), h.percentile(0.99), h.percentile(0.999), h.percentile(0.9999), h.max());
    };
    out += "# Latency\r\n";
    for (const auto& [name, h] : latency_histograms(ctx)) out += line(name, *h);
    out += "\r\n";
  }
  if (want_extra("commandstats")) {
    out += "# Commandstats\r\n";
    for (const CommandTotals& c : command_totals(ctx.stats)) {
      out += std::format("cmdstat_{}:calls={},usec={},usec_per_call={:.2f},rejected_calls={},failed_calls={}\r\n", c.name,
                         c.calls, c.nsec / 1000,
                         c.calls ? static_cast<double>(c.nsec) / 1000 / static_cast<double>(c.calls) : 0.0,
                         c.rejected, c.failed);
    }
    out += "\r\n";
  }
  if (want("errorstats")) {
    out += "# Errorstats\r\n";
    for (const auto& [code, n] : error_totals(ctx.stats)) out += std::format("errorstat_{}:count={}\r\n", code, n);
    out += "\r\n";
  }
  if (want("replication")) out += ctx.repl.info(ctx.db.storage(), ctx.settings().replica_priority);
  if (want("keyspace")) {
    out += std::format("# Keyspace\r\ndb0:keys={},expires=0,avg_ttl=0\r\n", ctx.db.approx_size());
  }
  return out;
}

}  // namespace datadam
