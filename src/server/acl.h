#pragma once

#include <atomic>
#include <bitset>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace datadam {

struct CommandSpec;

constexpr size_t kMaxAclCommands = 512;

// A user and what it may do, built from Redis-style ACL rules:
//   on / off                 enable or disable logging in as the user
//   >pass <pass #hash !hash  add / remove a password (kept as SHA-256)
//   nopass / resetpass       any password works / no password works
//   ~pattern                 keys it may read and write (glob)
//   %R~pattern %W~pattern    keys it may only read / only write
//   allkeys / resetkeys      ~* / no keys
//   +cmd -cmd +cmd|sub       allow / deny a command or a subcommand
//   +@category -@category    allow / deny a category (ACL CAT lists them)
//   allcommands / nocommands +@all / -@all
//   &pattern allchannels resetchannels   Pub/Sub channels it may use (glob)
//   reset                    back to: off, no passwords, no keys, no commands
struct AclUser {
  struct KeyPattern {
    std::string pattern;
    bool read = true;
    bool write = true;
  };

  std::string name;
  bool enabled = false;
  bool nopass = false;
  std::vector<std::string> passwords;  // SHA-256, lower-case hex
  std::bitset<kMaxAclCommands> commands;
  std::set<std::string, std::less<>> sub_allowed, sub_denied;  // "config|get"
  bool commands_from_all = false;          // the command rules start from +@all (else -@all)
  std::vector<std::string> command_rules;  // the rules since then, for display
  std::vector<KeyPattern> keys;
  std::vector<std::string> channels;
  bool unrestricted = false;  // every command, every key: nothing to check

  // Applies one rule; returns an error message or "".
  std::string apply(std::string_view rule);
  // The rules that define this user from scratch (ACL LIST, CONFIG REWRITE).
  std::string describe() const;
  bool password_ok(std::string_view password) const;
  bool may_run(std::string_view command, const CommandSpec& spec, std::string_view sub) const;
  bool may_access(std::string_view key, uint8_t access) const;
  // True when a "*" pattern grants this access (every key).
  bool all_keys(uint8_t access) const;
  // Pub/Sub: a channel it may publish or subscribe to; a pattern it may
  // PSUBSCRIBE to must be one of its channel patterns, literally (as in Redis).
  bool may_use_channel(std::string_view channel, bool is_pattern) const;

 private:
  void set_all_commands(bool allow);
  void refresh();
};

// Why a command was refused: reason "command" or "key", the command (or
// command|sub) or key refused, and the NOPERM error for the client.
struct AclDenial {
  std::string reason;
  std::string object;
  std::string message;
  explicit operator bool() const { return !message.empty(); }
};

// Empty if the user may run this command with these arguments.
AclDenial acl_check(const AclUser& user, std::string_view name, const CommandSpec& spec,
                    std::span<const std::string_view> args);

// ACL LOG: recent refusals and failed logins, newest first. Repeats of the
// same refusal (same reason, object and user) within a minute update one
// entry instead of adding more.
class AclLog {
 public:
  struct Entry {
    uint64_t id = 0;
    uint64_t count = 0;
    std::string reason;  // command, key or auth
    std::string context = "toplevel";
    std::string object;
    std::string username;
    std::string client_info;
    int64_t created_ms = 0;
    int64_t updated_ms = 0;
  };

  void add(std::string reason, std::string object, std::string username, std::string client_info);
  std::vector<Entry> entries(size_t count) const;
  void reset();
  void set_max_len(size_t n);

 private:
  mutable std::mutex mu_;
  std::vector<Entry> entries_;  // newest first
  uint64_t next_id_ = 0;
  size_t max_len_ = 128;
};

// Category names and bits ("all" included).
const std::vector<std::pair<std::string_view, uint32_t>>& acl_categories();

// The users. Starts with "default": on, nopass, every key and command.
class Acl {
 public:
  Acl();

  std::shared_ptr<const AclUser> find(std::string_view name) const;
  std::vector<std::shared_ptr<const AclUser>> users() const;
  // ACL SETUSER: applies the rules to the user (created with no permissions
  // if it does not exist); all of them or none. Returns an error or "".
  std::string set_user(std::string_view name, std::span<const std::string> rules);
  // A "user" line of the config file: defines the user from scratch.
  std::string define_user(std::string_view name, std::span<const std::string> rules);
  // ACL DELUSER: how many were removed; the default user cannot be.
  std::optional<int> del_users(std::span<const std::string_view> names, std::string* error);
  // The user these credentials log in as, or null.
  std::shared_ptr<const AclUser> authenticate(std::string_view user, std::string_view password) const;
  // What a new connection is logged in as: default, if it needs no password; else null.
  std::shared_ptr<const AclUser> initial_user() const;
  // Changes whenever a user changes, so connections know to reload theirs.
  uint64_t version() const { return version_.load(std::memory_order_acquire); }

 private:
  std::string store(std::string_view name, AclUser user, std::span<const std::string> rules);

  mutable std::shared_mutex mu_;
  std::map<std::string, std::shared_ptr<const AclUser>, std::less<>> users_;
  std::atomic<uint64_t> version_{1};
};

}  // namespace datadam
