#include "server/memcache_protocol.h"

#include <format>
#include <optional>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define DATADAM_GETPID _getpid
#else
#include <unistd.h>
#define DATADAM_GETPID getpid
#endif

#include "server/command.h"
#include "util/clock.h"
#include "util/strings.h"

namespace datadam {

namespace {

using Op = Database::Op;

constexpr int64_t kRelativeExpiryLimit = 60 * 60 * 24 * 30;  // memcache: <= 30 days is relative
constexpr int64_t kAlreadyExpired = 1;                       // any past timestamp

// memcache exptime -> absolute ms (0 = never). Negative or past values expire at once.
int64_t expire_at_from(int64_t exptime, int64_t now_ms) {
  if (exptime == 0) return 0;
  if (exptime < 0) return kAlreadyExpired;
  if (exptime <= kRelativeExpiryLimit) return now_ms + exptime * 1000;
  int64_t at = exptime * 1000;
  return at <= now_ms ? kAlreadyExpired : at;
}

bool valid_key(std::string_view key) {
  if (key.empty() || key.size() > MemcacheSession::kMaxKey) return false;
  for (char c : key) {
    if (static_cast<unsigned char>(c) <= 32 || c == 127) return false;
  }
  return true;
}

void append_value(std::string& out, std::string_view key, const RecordView& v, bool with_cas) {
  out += "VALUE ";
  out += key;
  if (with_cas) out += std::format(" {} {} {}\r\n", v.flags, v.value.size(), v.cas);
  else out += std::format(" {} {}\r\n", v.flags, v.value.size());
  out += v.value;
  out += "\r\n";
}

}  // namespace

MemcacheSession::Status MemcacheSession::process(std::string_view buf, size_t& pos, std::string& out) {
  size_t nl = buf.find('\n', pos);
  if (nl == std::string_view::npos) {
    if (buf.size() - pos > kMaxLine) {
      out += "CLIENT_ERROR line too long\r\n";
      return Status::Close;
    }
    return Status::Incomplete;
  }
  std::string_view line = buf.substr(pos, nl - pos);
  if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
  std::vector<std::string_view> tok = split_ws(line);
  if (tok.empty()) {
    pos = nl + 1;
    out += "ERROR\r\n";
    return Status::Done;
  }
  ctx_.stats.total_commands.fetch_add(1, std::memory_order_relaxed);
  std::string cmd = to_lower(tok[0]);
  if (cmd == "auth") {
    pos = nl + 1;
    if (tok.size() != 2 && tok.size() != 3) {
      out += "CLIENT_ERROR usage: auth [user] <password>\r\n";
    } else if (auto u = ctx_.acl.authenticate(tok.size() == 3 ? tok[1] : "default", tok.back())) {
      user_ = std::move(u);
      out += "OK\r\n";
    } else {
      ctx_.stats.auth_failures.fetch_add(1, std::memory_order_relaxed);
      ctx_.acl_log.add("auth", "AUTH", std::string(tok.size() == 3 ? tok[1] : "default"), "protocol=memcache");
      out += "CLIENT_ERROR authentication failed\r\n";
    }
    return Status::Done;
  }
  if (user_ && ctx_.acl.version() != acl_version_) {  // users changed: reload ours
    acl_version_ = ctx_.acl.version();
    user_ = ctx_.acl.find(user_->name);
    if (!user_) {
      out += "SERVER_ERROR the user of this connection was deleted\r\n";
      return Status::Close;
    }
  }
  if (!user_ && cmd != "quit" && cmd != "version") {
    out += "CLIENT_ERROR authentication required (send: auth <password>)\r\n";
    return Status::Close;  // a storage command's data block would otherwise be read as commands
  }
  Database& db = ctx_.db;
  int64_t now = unix_time_ms();

  try {
    // ---- storage commands: <cmd> <key> <flags> <exptime> <bytes> [cas] [noreply]\r\n<data>\r\n
    if (cmd == "set" || cmd == "add" || cmd == "replace" || cmd == "append" || cmd == "prepend" || cmd == "cas") {
      bool is_cas = cmd == "cas";
      size_t base = is_cas ? 6 : 5;
      auto field = [&](size_t i, auto parse, auto* value) {
        if (i >= tok.size()) return false;
        auto v = parse(tok[i]);
        if (v) *value = *v;
        return v.has_value();
      };
      uint64_t flags = 0, bytes = 0, cas_unique = 0;
      int64_t exptime = 0;
      if (!field(4, parse_uint64, &bytes)) {
        out += "CLIENT_ERROR bad command line format\r\n";
        pos = nl + 1;
        return Status::Done;
      }
      bool noreply = tok.size() == base + 1 && tok[base] == "noreply";
      bool malformed = !field(2, parse_uint64, &flags) || flags > UINT32_MAX || !field(3, parse_int64, &exptime) ||
                       (is_cas && !field(5, parse_uint64, &cas_unique)) || (tok.size() != base && !noreply) ||
                       !valid_key(tok[1]);
      if (bytes > kMaxValue) {
        out += "SERVER_ERROR object too large for cache\r\n";
        return Status::Close;
      }
      size_t data_start = nl + 1;
      size_t end = data_start + static_cast<size_t>(bytes) + 2;
      if (buf.size() < end) return Status::Incomplete;
      if (buf[end - 2] != '\r' || buf[end - 1] != '\n') {
        out += "CLIENT_ERROR bad data chunk\r\n";
        return Status::Close;
      }
      pos = end;  // the data block is consumed even when the header is unusable
      if (malformed) {
        out += "CLIENT_ERROR bad command line format\r\n";
        return Status::Done;
      }

      std::string_view key = tok[1];
      std::string_view data = buf.substr(data_start, static_cast<size_t>(bytes));
      std::string_view as_redis = cmd == "add" ? "setnx" : (cmd == "append" || cmd == "prepend") ? "append" : "set";
      if (auto err = permission_error(as_redis, std::span(&key, 1), kKeyWrite); !err.empty()) {
        out += err;
        return Status::Done;
      }
      int64_t expire_at = expire_at_from(exptime, now);
      uint32_t fl = static_cast<uint32_t>(flags);
      std::string_view reply;

      if (cmd == "set") {
        db.put(key, Record{ValueType::String, fl, expire_at, 0, std::string(data)});
        reply = "STORED";
      } else {
        db.update(key, [&](std::optional<Record>& rec) {
          bool usable = rec && rec->type == ValueType::String;
          if (cmd == "add") {
            if (usable) {
              reply = "NOT_STORED";
              return Op::Keep;
            }
          } else if (cmd == "replace" || cmd == "append" || cmd == "prepend") {
            if (!usable) {
              reply = "NOT_STORED";
              return Op::Keep;
            }
          } else {  // cas
            if (!usable) {
              reply = "NOT_FOUND";
              return Op::Keep;
            }
            if (rec->cas != cas_unique) {
              reply = "EXISTS";
              return Op::Keep;
            }
          }
          if (cmd == "append") {
            rec->value.append(data);
          } else if (cmd == "prepend") {
            rec->value.insert(0, data);
          } else {
            rec = Record{ValueType::String, fl, expire_at, 0, std::string(data)};
          }
          reply = "STORED";
          return Op::Put;
        });
      }
      if (!noreply) {
        out += reply;
        out += "\r\n";
      }
      return Status::Done;
    }

    pos = nl + 1;
    bool noreply = tok.back() == "noreply";
    size_t argc = noreply ? tok.size() - 1 : tok.size();
    auto respond = [&](std::string_view s) {
      if (!noreply) {
        out += s;
        out += "\r\n";
      }
    };

    // Every other command needs the permissions of its Redis equivalent.
    {
      std::string_view as_redis;
      size_t first_key = 1, last_key = 1;
      uint8_t access = kKeyWrite;
      if (cmd == "get" || cmd == "gets") as_redis = "mget", last_key = tok.size() - 1, access = kKeyRead;
      else if (cmd == "gat" || cmd == "gats") as_redis = "getex", first_key = 2, last_key = tok.size() - 1;
      else if (cmd == "delete") as_redis = "del";
      else if (cmd == "incr" || cmd == "decr") as_redis = "incrby";
      else if (cmd == "touch") as_redis = "expire";
      else if (cmd == "flush_all") as_redis = "flushall", last_key = 0;
      else if (cmd == "stats") as_redis = "info", last_key = 0;
      if (!as_redis.empty()) {
        std::vector<std::string_view> keys;
        for (size_t i = first_key; i <= last_key && i < argc; ++i) keys.push_back(tok[i]);
        if (auto err = permission_error(as_redis, keys, access); !err.empty()) {
          out += err;
          return Status::Done;
        }
      }
    }

    if (cmd == "get" || cmd == "gets") {
      if (tok.size() < 2) {
        out += "ERROR\r\n";
        return Status::Done;
      }
      bool with_cas = cmd == "gets";
      for (size_t i = 1; i < tok.size(); ++i) {
        db.read(tok[i], [&](const RecordView& v) {
          if (v.type == ValueType::String) append_value(out, tok[i], v, with_cas);
        });
      }
      out += "END\r\n";
      return Status::Done;
    }

    if (cmd == "gat" || cmd == "gats") {
      auto exptime = tok.size() >= 3 ? parse_int64(tok[1]) : std::nullopt;
      if (!exptime) {
        out += "CLIENT_ERROR bad command line format\r\n";
        return Status::Done;
      }
      int64_t expire_at = expire_at_from(exptime.value_or(0), now);
      bool with_cas = cmd == "gats";
      for (size_t i = 2; i < tok.size(); ++i) {
        db.update(tok[i], [&](std::optional<Record>& rec) {
          if (!rec || rec->type != ValueType::String) return Op::Keep;
          rec->expire_at_ms = expire_at;
          if (rec->expired(now)) return Op::Delete;
          append_value(out, tok[i], rec->view(), with_cas);
          return Op::PutKeepCas;
        });
      }
      out += "END\r\n";
      return Status::Done;
    }

    if (cmd == "delete") {
      // "delete <key> [0] [noreply]"; the legacy time argument must be 0.
      if (argc < 2 || argc > 3 || (argc == 3 && tok[2] != "0")) {
        out += "CLIENT_ERROR bad command line format.  Usage: delete <key> [noreply]\r\n";
        return Status::Done;
      }
      respond(db.del(tok[1]) ? "DELETED" : "NOT_FOUND");
      return Status::Done;
    }

    if (cmd == "incr" || cmd == "decr") {
      if (argc != 3) {
        out += "ERROR\r\n";
        return Status::Done;
      }
      auto delta = parse_uint64(tok[2]);
      if (!delta) {
        out += "CLIENT_ERROR invalid numeric delta argument\r\n";
        return Status::Done;
      }
      std::string reply = "NOT_FOUND";
      bool incr = cmd == "incr";
      db.update(tok[1], [&](std::optional<Record>& rec) {
        if (!rec || rec->type != ValueType::String) return Op::Keep;
        auto cur = parse_uint64(rec->value);
        if (!cur) {
          reply = "CLIENT_ERROR cannot increment or decrement non-numeric value";
          return Op::Keep;
        }
        uint64_t v = incr ? *cur + *delta : (*delta > *cur ? 0 : *cur - *delta);  // incr wraps, decr floors at 0
        reply = std::to_string(v);
        rec->value = reply;
        return Op::Put;
      });
      respond(reply);
      return Status::Done;
    }

    if (cmd == "touch") {
      auto exptime = argc == 3 ? parse_int64(tok[2]) : std::nullopt;
      if (!exptime) {
        out += "CLIENT_ERROR bad command line format\r\n";
        return Status::Done;
      }
      int64_t expire_at = expire_at_from(exptime.value_or(0), now);
      bool touched = false;
      db.update(tok[1], [&](std::optional<Record>& rec) {
        if (!rec) return Op::Keep;
        touched = true;
        rec->expire_at_ms = expire_at;
        return Op::PutKeepCas;
      });
      respond(touched ? "TOUCHED" : "NOT_FOUND");
      return Status::Done;
    }

    if (cmd == "flush_all") {
      if (argc > 2 || (argc == 2 && !parse_int64(tok[1]))) {
        out += "CLIENT_ERROR bad command line format\r\n";
        return Status::Done;
      }
      db.flush_all();
      respond("OK");
      return Status::Done;
    }

    if (cmd == "version") {
      out += "VERSION " DATADAM_VERSION "\r\n";
      return Status::Done;
    }

    if (cmd == "verbosity") {
      respond("OK");
      return Status::Done;
    }

    if (cmd == "stats") {
      CacheStats s = db.cache().stats();
      int64_t uptime = (now - ctx_.start_time_ms) / 1000;
      out += std::format(
          "STAT pid {}\r\nSTAT uptime {}\r\nSTAT time {}\r\nSTAT version {}\r\nSTAT threads {}\r\n"
          "STAT curr_connections {}\r\nSTAT total_connections {}\r\nSTAT curr_items {}\r\n"
          "STAT limit_maxbytes {}\r\nSTAT bytes {}\r\nSTAT cache_items {}\r\nSTAT get_hits {}\r\n"
          "STAT get_misses {}\r\nSTAT evictions {}\r\nEND\r\n",
          DATADAM_GETPID(), uptime, now / 1000, DATADAM_VERSION, ctx_.config.io_threads,
          ctx_.stats.connected_clients.load(), ctx_.stats.total_connections.load(), db.approx_size(),
          db.cache().options().max_memory, s.payload_bytes, s.items, s.hits, s.misses,
          s.evicted_lru + s.evicted_idle + s.evicted_age);
      return Status::Done;
    }

    if (cmd == "quit") return Status::Close;

    out += "ERROR\r\n";
    return Status::Done;
  } catch (const std::exception& e) {
    out += std::format("SERVER_ERROR {}\r\n", e.what());
    if (pos <= nl) pos = nl + 1;
    return Status::Done;
  }
}

std::string MemcacheSession::permission_error(std::string_view redis_command, std::span<const std::string_view> keys,
                                              uint8_t access) {
  if (ctx_.db.read_only()) {  // a replica: its primary does the writing
    const CommandSpec* spec = find_command(redis_command);
    if (spec && (spec->categories & kCatWrite)) return "SERVER_ERROR read only replica\r\n";
  }
  if (!user_ || user_->unrestricted) return {};
  const CommandSpec* spec = find_command(redis_command);
  if (!spec || !user_->may_run(redis_command, *spec, "")) {
    ctx_.stats.acl_denied.fetch_add(1, std::memory_order_relaxed);
    ctx_.acl_log.add("command", std::string(redis_command), user_->name, "protocol=memcache");
    return std::format("CLIENT_ERROR NOPERM user {} may not run '{}'\r\n", user_->name, redis_command);
  }
  for (auto key : keys) {
    if (!user_->may_access(key, access)) {
      ctx_.stats.acl_denied.fetch_add(1, std::memory_order_relaxed);
      ctx_.acl_log.add("key", std::string(key), user_->name, "protocol=memcache");
      return "CLIENT_ERROR NOPERM no permissions to access a key\r\n";
    }
  }
  return {};
}

}  // namespace datadam
