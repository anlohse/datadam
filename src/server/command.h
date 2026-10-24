#pragma once

// Shared by the files that implement Redis commands.

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <string_view>
#include <utility>
#include <vector>

#include "server/redis_commands.h"
#include "util/strings.h"

namespace datadam {

inline constexpr std::string_view kWrongType = "WRONGTYPE Operation against a key holding the wrong kind of value";
inline constexpr std::string_view kNotInteger = "ERR value is not an integer or out of range";
inline constexpr std::string_view kNotFloat = "ERR value is not a valid float";
inline constexpr std::string_view kSyntax = "ERR syntax error";
inline constexpr std::string_view kMustBePositive = "ERR value is out of range, must be positive";

struct Cmd {
  ServerContext& ctx;
  ClientState& client;
  Args args;
  Reply& r;
  Database& db() { return ctx.db; }
};

using Handler = void (*)(Cmd&);

// ACL categories (ACL rules say +@read, -@dangerous, ...).
enum AclCategory : uint32_t {
  kCatRead = 1u << 0,
  kCatWrite = 1u << 1,
  kCatKeyspace = 1u << 2,
  kCatString = 1u << 3,
  kCatHash = 1u << 4,
  kCatSet = 1u << 5,
  kCatSortedSet = 1u << 6,
  kCatList = 1u << 7,
  kCatAdmin = 1u << 8,
  kCatDangerous = 1u << 9,
  kCatConnection = 1u << 10,
  kCatFast = 1u << 11,
  kCatSlow = 1u << 12,
  kCatIndex = 1u << 13,
  kCatJson = 1u << 14,
  kCatPubSub = 1u << 15,
  kCatTransaction = 1u << 16,
  kCatBlocking = 1u << 17,
  kCatScripting = 1u << 18,
};

enum KeyAccess : uint8_t { kKeyRead = 1, kKeyWrite = 2 };

// Which arguments are keys: args[first..last] every `step` (last < 0 counts
// from the end); or, with numkeys_at, the args[numkeys_at] keys that follow it.
struct KeyRange {
  int first = 0;  // 0: none
  int last = 0;
  int step = 1;
  uint8_t access = 0;
  int numkeys_at = 0;
};

struct CommandSpec {
  Handler fn;
  int arity;  // > 0: exact argument count (including the name); < 0: at least -arity
  // Filled in from the command metadata table (command_meta.cpp):
  uint32_t categories = 0;
  KeyRange keys[2] = {};
  uint16_t id = 0;  // dense, for per-user command bitmaps
};

// Calls f(key, access) for every key argument of a command, in order, until f
// returns false. A bad key count (numkeys) is skipped: the command reports it.
template <class F>
void for_each_key(const CommandSpec& spec, std::span<const std::string_view> args, F&& f) {
  int argc = static_cast<int>(args.size());
  for (const KeyRange& r : spec.keys) {
    if (r.first == 0) continue;
    int last = r.last < 0 ? argc + r.last : r.last;
    if (r.numkeys_at > 0) {
      auto n = r.numkeys_at < argc ? parse_int64(args[static_cast<size_t>(r.numkeys_at)]) : std::nullopt;
      if (!n || *n < 0) continue;
      last = r.first + static_cast<int>(std::min<int64_t>(*n, argc)) - 1;
    }
    last = std::min(last, argc - 1);
    for (int i = r.first; i <= last; i += r.step) {
      if (!f(args[static_cast<size_t>(i)], r.access)) return;
    }
  }
}

// The ACL metadata of a command: categories and key positions.
struct CommandMeta {
  uint32_t categories;
  KeyRange keys[2];
};
const CommandMeta* command_meta(std::string_view name);

// All commands, by lower-case name.
const std::unordered_map<std::string, CommandSpec>& all_commands();
const CommandSpec* find_command(std::string_view lower_name);

using CommandList = std::vector<std::pair<std::string, CommandSpec>>;

CommandList hash_commands();
CommandList set_commands();
CommandList zset_commands();
CommandList list_commands();
CommandList index_commands();
CommandList search_commands();
CommandList scan_commands();
CommandList acl_commands();
CommandList cdc_commands();
CommandList replication_commands();
CommandList json_commands();
CommandList transaction_commands();
CommandList pubsub_commands();
CommandList script_commands();
// While a script has run too long, only these get through (else BUSY).
bool allowed_while_script_busy(std::string_view name, Args args);
CommandList admin_commands();

// Commands that run, instead of being queued, between MULTI and EXEC.
bool runs_in_multi(std::string_view name);

// Blocking commands (BLPOP, BLMOVE, BZPOPMIN...). The timeout argument, in
// seconds (fractions allowed, 0 = forever); replies an error and returns
// nullopt if it is not valid.
std::optional<double> parse_block_timeout(Cmd& c, std::string_view arg);
// For a blocking command that found nothing in its keys, called while they
// are still locked (inside the transaction that looked). True: the command
// waits; it replies nothing, and the connection runs it again after a write
// to one of the keys or once the timeout passed. False: it cannot wait
// (inside EXEC, or not on a connection) or the timeout passed; it replies
// that nothing came.
bool block_for_keys(Cmd& c, std::span<const std::string_view> keys, double timeout_s);

// For commands that return keys found by other means (KEYS, SCAN, IDX.*):
// whether the logged-in user may read this key.
inline bool may_read_key(const Cmd& c, std::string_view key) {
  const AclUser* u = c.client.user.get();
  return !u || u->unrestricted || u->may_access(key, kKeyRead);
}
// Whether the user may read every key (no filtering needed).
inline bool reads_all_keys(const Cmd& c) {
  const AclUser* u = c.client.user.get();
  return !u || u->unrestricted || u->all_keys(kKeyRead);
}

// Runs f(KeyTxn&) on a key that must be missing or of the given type;
// otherwise replies WRONGTYPE and returns false. f must not write replies (it
// may be rolled back), only collect what the reply needs.
template <class F>
bool with_collection(Cmd& c, std::string_view key, ValueType type, F&& f) {
  bool wrong = false;
  c.db().transact(key, [&](Database::KeyTxn& t) {
    if (t.wrong_type(type)) {
      wrong = true;
      return;
    }
    f(t);
  });
  if (wrong) c.r.error(kWrongType);
  return !wrong;
}

// Same for commands that only read: served from the cache without the key
// lock when the collection is cached (whole, if need_elements), like GET.
template <class F>
bool read_collection(Cmd& c, std::string_view key, ValueType type, bool need_elements, F&& f) {
  bool wrong = false;
  bool served = c.db().peek(key, need_elements, [&](Database::KeyTxn& t) {
    if (t.wrong_type(type)) wrong = true;
    else f(t);
  });
  if (!served) return with_collection(c, key, type, std::forward<F>(f));
  if (wrong) c.r.error(kWrongType);
  return !wrong;
}

// Normalizes a Redis index range (negative = from the end) over `len` items
// into [*start, *stop]; false when it is empty.
inline bool normalize_range(int64_t start, int64_t stop, int64_t len, int64_t* s, int64_t* e) {
  if (start < 0) start = std::max<int64_t>(len + start, 0);
  if (stop < 0) stop += len;
  if (stop >= len) stop = len - 1;
  if (start > stop || start >= len) return false;
  *s = start;
  *e = stop;
  return true;
}

}  // namespace datadam
