#include "server/acl.h"

#include <algorithm>
#include <format>

#include "server/command.h"
#include "util/clock.h"
#include "util/sha256.h"
#include "util/strings.h"

namespace datadam {

namespace {

bool is_hex_hash(std::string_view s) {
  return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

// Commands whose first argument is a subcommand (for +cmd|sub rules).
bool has_subcommands(std::string_view name) {
  return name == "config" || name == "client" || name == "acl" || name == "cache" || name == "command";
}

}  // namespace

const std::vector<std::pair<std::string_view, uint32_t>>& acl_categories() {
  static const std::vector<std::pair<std::string_view, uint32_t>> cats = {
      {"all", ~0u},
      {"read", kCatRead},
      {"write", kCatWrite},
      {"keyspace", kCatKeyspace},
      {"string", kCatString},
      {"hash", kCatHash},
      {"set", kCatSet},
      {"sortedset", kCatSortedSet},
      {"list", kCatList},
      {"admin", kCatAdmin},
      {"dangerous", kCatDangerous},
      {"connection", kCatConnection},
      {"fast", kCatFast},
      {"slow", kCatSlow},
      {"index", kCatIndex},
      {"search", kCatIndex},
      {"json", kCatJson},
      {"pubsub", kCatPubSub},
      {"transaction", kCatTransaction},
      {"blocking", kCatBlocking},
      // Redis categories of commands datadam does not have: accepted, so rule
      // sets written for Redis load, and they cover nothing.
      {"scripting", kCatScripting},
      {"geo", 0},
      {"hyperloglog", 0},
      {"stream", 0},
      {"bitmap", 0},
  };
  return cats;
}

// ---- AclUser

void AclUser::set_all_commands(bool allow) {
  if (allow) commands.set();
  else commands.reset();
  sub_allowed.clear();
  sub_denied.clear();
  commands_from_all = allow;
  command_rules.clear();
}

std::string AclUser::apply(std::string_view rule) {
  if (rule.empty()) return "Syntax error";
  // Rules are written back space-separated (ACL LIST, CONFIG REWRITE): only a
  // password (hashed when written) may hold spaces, and nothing control characters.
  for (char c : rule) {
    bool space = c == ' ';
    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f || (space && rule[0] != '>' && rule[0] != '<')) {
      return "Syntax error";
    }
  }
  std::string lower = to_lower(rule);
  if (lower == "on") {
    enabled = true;
  } else if (lower == "off") {
    enabled = false;
  } else if (lower == "nopass") {
    passwords.clear();
    nopass = true;
  } else if (lower == "resetpass") {
    passwords.clear();
    nopass = false;
  } else if (lower == "allkeys") {
    keys = {{"*", true, true}};
  } else if (lower == "resetkeys") {
    keys.clear();
  } else if (lower == "allchannels") {
    channels = {"*"};
  } else if (lower == "resetchannels") {
    channels.clear();
  } else if (lower == "allcommands") {
    set_all_commands(true);
  } else if (lower == "nocommands") {
    set_all_commands(false);
  } else if (lower == "reset") {
    enabled = false;
    nopass = false;
    passwords.clear();
    keys.clear();
    channels.clear();
    set_all_commands(false);
  } else if (rule[0] == '>' || rule[0] == '#') {
    std::string hash = rule[0] == '>' ? sha256_hex(rule.substr(1)) : std::string(rule.substr(1));
    if (rule[0] == '#' && !is_hex_hash(hash)) {
      return "The password hash must be exactly 64 characters and contain only lowercase hexadecimal characters";
    }
    if (std::find(passwords.begin(), passwords.end(), hash) == passwords.end()) passwords.push_back(hash);
    nopass = false;
  } else if (rule[0] == '<' || rule[0] == '!') {
    std::string hash = rule[0] == '<' ? sha256_hex(rule.substr(1)) : std::string(rule.substr(1));
    auto it = std::find(passwords.begin(), passwords.end(), hash);
    if (it == passwords.end()) return "The password you are trying to remove from the user does not exist";
    passwords.erase(it);
  } else if (rule[0] == '~') {
    keys.push_back({std::string(rule.substr(1)), true, true});
  } else if (rule[0] == '%') {
    size_t tilde = rule.find('~');
    if (tilde == std::string_view::npos || tilde == 1) return "Syntax error";
    KeyPattern p{std::string(rule.substr(tilde + 1)), false, false};
    for (char c : rule.substr(1, tilde - 1)) {
      if (c == 'R' || c == 'r') p.read = true;
      else if (c == 'W' || c == 'w') p.write = true;
      else return "Syntax error";
    }
    keys.push_back(std::move(p));
  } else if (rule[0] == '&') {
    channels.emplace_back(rule.substr(1));
  } else if (rule[0] == '+' || rule[0] == '-') {
    bool allow = rule[0] == '+';
    std::string what = to_lower(rule.substr(1));
    if (what.starts_with("@")) {
      const auto& cats = acl_categories();
      auto cat = std::find_if(cats.begin(), cats.end(), [&](const auto& c) { return c.first == what.substr(1); });
      if (cat == cats.end()) return "Unknown command or category name in ACL";
      if (cat->first == "all") {
        set_all_commands(allow);
        refresh();
        return {};
      }
      for (const auto& [command, spec] : all_commands()) {
        if ((spec.categories & cat->second) == 0) continue;
        commands.set(spec.id, allow);
        std::erase_if(sub_allowed, [&](const std::string& s) { return s.starts_with(command + "|"); });
        std::erase_if(sub_denied, [&](const std::string& s) { return s.starts_with(command + "|"); });
      }
      for (const auto& [sub, categories] : subcommand_categories()) {
        if ((categories & cat->second) == 0) continue;
        (allow ? sub_denied : sub_allowed).erase(std::string(sub));
        (allow ? sub_allowed : sub_denied).insert(std::string(sub));
      }
    } else if (size_t bar = what.find('|'); bar != std::string::npos) {
      std::string parent = what.substr(0, bar);
      if (!find_command(parent) || !has_subcommands(parent) || bar + 1 == what.size()) {
        return "Unknown command or category name in ACL";
      }
      if (allow) {
        sub_denied.erase(what);
        sub_allowed.insert(what);
      } else {
        sub_allowed.erase(what);
        sub_denied.insert(what);
      }
    } else {
      const CommandSpec* spec = find_command(what);
      if (!spec) return "Unknown command or category name in ACL";
      commands.set(spec->id, allow);
      std::erase_if(sub_allowed, [&](const std::string& s) { return s.starts_with(what + "|"); });
      std::erase_if(sub_denied, [&](const std::string& s) { return s.starts_with(what + "|"); });
    }
    command_rules.push_back(std::string(1, rule[0]) + what);
  } else {
    return "Syntax error";
  }
  refresh();
  return {};
}

void AclUser::refresh() {
  // Command ids are dense: 0 .. count - 1.
  static const std::bitset<kMaxAclCommands> every = [] {
    std::bitset<kMaxAclCommands> b;
    for (size_t i = 0; i < all_commands().size(); ++i) b.set(i);
    return b;
  }();
  bool every_command = (commands & every) == every && sub_denied.empty();
  unrestricted = every_command && all_keys(kKeyRead | kKeyWrite);
}

std::string AclUser::describe() const {
  std::string out = enabled ? "on" : "off";
  if (nopass) out += " nopass";
  for (const auto& h : passwords) out += " #" + h;
  if (keys.empty()) out += " resetkeys";
  for (const auto& k : keys) {
    if (k.read && k.write) out += " ~" + k.pattern;
    else out += std::format(" %{}{}~{}", k.read ? "R" : "", k.write ? "W" : "", k.pattern);
  }
  if (channels.empty()) out += " resetchannels";
  for (const auto& c : channels) out += " &" + c;
  out += commands_from_all ? " +@all" : " -@all";
  for (const auto& r : command_rules) out += " " + r;
  return out;
}

bool AclUser::password_ok(std::string_view password) const {
  if (nopass) return true;
  std::string hash = sha256_hex(password);
  // Every byte of every one: how long it takes says nothing of the hashes.
  bool ok = false;
  for (const auto& h : passwords) {
    unsigned char diff = h.size() == hash.size() ? 0 : 1;
    for (size_t i = 0; i < h.size() && i < hash.size(); ++i) {
      diff |= static_cast<unsigned char>(h[i] ^ hash[i]);
    }
    ok |= diff == 0;
  }
  return ok;
}

bool AclUser::may_run(std::string_view command, const CommandSpec& spec, std::string_view sub) const {
  if (!sub.empty() && (!sub_allowed.empty() || !sub_denied.empty())) {
    std::string full = std::format("{}|{}", command, sub);
    if (sub_denied.contains(full)) return false;
    if (sub_allowed.contains(full)) return true;
  }
  return commands.test(spec.id);
}

bool AclUser::may_access(std::string_view key, uint8_t access) const {
  for (const auto& k : keys) {
    if ((access & kKeyRead) && !k.read) continue;
    if ((access & kKeyWrite) && !k.write) continue;
    if (k.pattern == "*" || glob_match(k.pattern, key)) return true;
  }
  return false;
}

bool AclUser::may_use_channel(std::string_view channel, bool is_pattern) const {
  for (const auto& p : channels) {
    if (p == "*" || p == channel || (!is_pattern && glob_match(p, channel))) return true;
  }
  return false;
}

bool AclUser::all_keys(uint8_t access) const {
  for (const auto& k : keys) {
    if (k.pattern != "*") continue;
    if ((access & kKeyRead) && !k.read) continue;
    if ((access & kKeyWrite) && !k.write) continue;
    return true;
  }
  return false;
}

AclDenial acl_check(const AclUser& user, std::string_view name, const CommandSpec& spec,
                    std::span<const std::string_view> args) {
  if (user.unrestricted) return {};
  std::string sub = has_subcommands(name) && args.size() > 1 ? to_lower(args[1]) : std::string();
  // Anyone logged in may ask who they are and what the categories are.
  bool self_service = name == "acl" && (sub == "whoami" || sub == "cat");
  if (!self_service && !user.may_run(name, spec, sub)) {
    std::string object = sub.empty() ? std::string(name) : std::format("{}|{}", name, sub);
    std::string message = std::format("NOPERM User {} has no permissions to run the '{}' command", user.name, object);
    return {"command", std::move(object), std::move(message)};
  }
  AclDenial denied;
  for_each_key(spec, args, [&](std::string_view key, uint8_t access) {
    if (user.may_access(key, access)) return true;
    denied = {"key", std::string(key), "NOPERM No permissions to access a key"};
    return false;
  });
  return denied;
}

// ---- Acl

Acl::Acl() {
  AclUser d;
  d.name = "default";
  d.apply("on");
  d.apply("nopass");
  d.apply("allkeys");
  d.apply("allchannels");
  d.apply("+@all");
  users_.emplace("default", std::make_shared<const AclUser>(std::move(d)));
}

std::shared_ptr<const AclUser> Acl::find(std::string_view name) const {
  std::shared_lock lock(mu_);
  auto it = users_.find(name);
  return it == users_.end() ? nullptr : it->second;
}

std::vector<std::shared_ptr<const AclUser>> Acl::users() const {
  std::shared_lock lock(mu_);
  std::vector<std::shared_ptr<const AclUser>> out;
  for (const auto& [name, user] : users_) out.push_back(user);
  return out;
}

std::string Acl::store(std::string_view name, AclUser user, std::span<const std::string> rules) {
  if (name.empty() || name.find_first_of(" \t\r\n") != std::string_view::npos) {
    return "ERR Usernames can't contain spaces or be empty";
  }
  user.name = std::string(name);
  for (const auto& rule : rules) {
    if (auto err = user.apply(rule); !err.empty()) {
      return std::format("ERR Error in ACL SETUSER modifier '{}': {}", rule, err);
    }
  }
  std::unique_lock lock(mu_);
  users_[std::string(name)] = std::make_shared<const AclUser>(std::move(user));
  version_.fetch_add(1, std::memory_order_acq_rel);
  return {};
}

std::string Acl::set_user(std::string_view name, std::span<const std::string> rules) {
  AclUser user;
  if (auto existing = find(name)) {
    user = *existing;
  } else {
    user.apply("reset");
  }
  return store(name, std::move(user), rules);
}

std::string Acl::define_user(std::string_view name, std::span<const std::string> rules) {
  AclUser user;
  user.apply("reset");
  return store(name, std::move(user), rules);
}

std::optional<int> Acl::del_users(std::span<const std::string_view> names, std::string* error) {
  for (auto n : names) {
    if (n == "default") {
      *error = "ERR The 'default' user cannot be removed";
      return std::nullopt;
    }
  }
  std::unique_lock lock(mu_);
  int removed = 0;
  for (auto n : names) {
    if (auto it = users_.find(n); it != users_.end()) {
      users_.erase(it);
      ++removed;
    }
  }
  if (removed) version_.fetch_add(1, std::memory_order_acq_rel);
  return removed;
}

std::shared_ptr<const AclUser> Acl::authenticate(std::string_view user, std::string_view password) const {
  auto u = find(user);
  if (!u || !u->enabled || !u->password_ok(password)) return nullptr;
  return u;
}

std::shared_ptr<const AclUser> Acl::initial_user() const {
  auto d = find("default");
  return d && d->enabled && d->nopass ? d : nullptr;
}

// ---- AclLog

void AclLog::add(std::string reason, std::string object, std::string username, std::string client_info) {
  int64_t now = unix_time_ms();
  std::lock_guard lock(mu_);
  if (max_len_ == 0) return;
  for (size_t i = 0; i < entries_.size(); ++i) {
    Entry& e = entries_[i];
    if (e.reason == reason && e.object == object && e.username == username && now - e.updated_ms < 60'000) {
      ++e.count;
      e.updated_ms = now;
      e.client_info = std::move(client_info);
      std::rotate(entries_.begin(), entries_.begin() + static_cast<ptrdiff_t>(i),
                  entries_.begin() + static_cast<ptrdiff_t>(i) + 1);
      return;
    }
  }
  Entry e;
  e.id = next_id_++;
  e.count = 1;
  e.reason = std::move(reason);
  e.object = std::move(object);
  e.username = std::move(username);
  e.client_info = std::move(client_info);
  e.created_ms = e.updated_ms = now;
  entries_.insert(entries_.begin(), std::move(e));
  if (entries_.size() > max_len_) entries_.resize(max_len_);
}

std::vector<AclLog::Entry> AclLog::entries(size_t count) const {
  std::lock_guard lock(mu_);
  return {entries_.begin(), entries_.begin() + static_cast<ptrdiff_t>(std::min(count, entries_.size()))};
}

void AclLog::reset() {
  std::lock_guard lock(mu_);
  entries_.clear();
}

void AclLog::set_max_len(size_t n) {
  std::lock_guard lock(mu_);
  max_len_ = n;
  if (entries_.size() > n) entries_.resize(n);
}

}  // namespace datadam
