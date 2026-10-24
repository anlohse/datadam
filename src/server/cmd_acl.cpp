// ACL SETUSER / GETUSER / DELUSER / LIST / USERS / WHOAMI / CAT.
//
// Users and rules follow Redis (see AclUser). Changes are in memory;
// CONFIG REWRITE writes them to the config file as "user" lines.

#include <algorithm>
#include <format>

#include "server/command.h"
#include "util/clock.h"
#include "util/strings.h"

namespace datadam {

namespace {

void acl_setuser(Cmd& c) {
  if (c.args.size() < 3) return c.r.error("ERR wrong number of arguments for 'acl|setuser' command");
  std::vector<std::string> rules(c.args.begin() + 3, c.args.end());
  std::string error = c.ctx.acl.set_user(c.args[2], rules);
  if (!error.empty()) return c.r.error(error);
  c.ctx.acl_changed();
  c.r.ok();
}

void acl_getuser(Cmd& c) {
  if (c.args.size() != 3) return c.r.error("ERR wrong number of arguments for 'acl|getuser' command");
  auto u = c.ctx.acl.find(c.args[2]);
  if (!u) return c.r.null();
  // The same fields Redis 7 returns.
  std::vector<std::string_view> flags = {u->enabled ? "on" : "off"};
  if (u->nopass) flags.push_back("nopass");
  std::string keys, channels, commands;
  std::string described = u->describe();
  for (std::string_view token : split_ws(described)) {
    if (token.starts_with("~") || token.starts_with("%")) keys += (keys.empty() ? "" : " ") + std::string(token);
    else if (token.starts_with("&")) channels += (channels.empty() ? "" : " ") + std::string(token);
    else if (token.starts_with("+") || token.starts_with("-")) {
      commands += (commands.empty() ? "" : " ") + std::string(token);
    }
  }
  c.r.map(6);
  c.r.bulk("flags");
  c.r.array(flags.size());
  for (auto f : flags) c.r.bulk(f);
  c.r.bulk("passwords");
  c.r.array(u->passwords.size());
  for (const auto& p : u->passwords) c.r.bulk(p);
  c.r.bulk("commands");
  c.r.bulk(commands);
  c.r.bulk("keys");
  c.r.bulk(keys);
  c.r.bulk("channels");
  c.r.bulk(channels);
  c.r.bulk("selectors");
  c.r.array(0);
}

void acl_deluser(Cmd& c) {
  if (c.args.size() < 3) return c.r.error("ERR wrong number of arguments for 'acl|deluser' command");
  std::vector<std::string_view> names(c.args.begin() + 2, c.args.end());
  std::string error;
  auto removed = c.ctx.acl.del_users(names, &error);
  if (!removed) return c.r.error(error);
  c.ctx.acl_changed();
  c.r.integer(*removed);
}

void acl_list(Cmd& c) {
  auto users = c.ctx.acl.users();
  c.r.array(users.size());
  for (const auto& u : users) c.r.bulk(std::format("user {} {}", u->name, u->describe()));
}

void acl_users(Cmd& c) {
  auto users = c.ctx.acl.users();
  c.r.array(users.size());
  for (const auto& u : users) c.r.bulk(u->name);
}

void acl_whoami(Cmd& c) { c.r.bulk(c.client.user ? c.client.user->name : "default"); }

void acl_cat(Cmd& c) {
  const auto& cats = acl_categories();
  if (c.args.size() == 2) {
    c.r.array(cats.size() - 1);
    for (const auto& [name, bits] : cats) {
      if (name != "all") c.r.bulk(name);
    }
    return;
  }
  std::string want = to_lower(c.args[2]);
  auto cat = std::find_if(cats.begin(), cats.end(), [&](const auto& e) { return e.first == want; });
  if (cat == cats.end()) return c.r.error(std::format("ERR Unknown category '{}'", c.args[2]));
  std::vector<std::string_view> names;
  for (const auto& [name, spec] : all_commands()) {
    if (spec.categories & cat->second) names.push_back(name);
  }
  std::sort(names.begin(), names.end());
  c.r.array(names.size());
  for (auto n : names) c.r.bulk(n);
}

// ACL LOG [count | RESET]
void acl_log(Cmd& c) {
  size_t count = 10;
  if (c.args.size() == 3) {
    if (iequals(c.args[2], "reset")) {
      c.ctx.acl_log.reset();
      return c.r.ok();
    }
    auto n = parse_int64(c.args[2]);
    if (!n || *n < 0) return c.r.error("ERR value is out of range, must be positive");
    count = static_cast<size_t>(*n);
  } else if (c.args.size() > 3) {
    return c.r.error(kSyntax);
  }
  int64_t now = unix_time_ms();
  auto entries = c.ctx.acl_log.entries(count);
  c.r.array(entries.size());
  for (const auto& e : entries) {
    c.r.map(10);
    c.r.bulk("count");
    c.r.integer(static_cast<int64_t>(e.count));
    c.r.bulk("reason");
    c.r.bulk(e.reason);
    c.r.bulk("context");
    c.r.bulk(e.context);
    c.r.bulk("object");
    c.r.bulk(e.object);
    c.r.bulk("username");
    c.r.bulk(e.username);
    c.r.bulk("age-seconds");
    c.r.double_value(static_cast<double>(now - e.created_ms) / 1000.0);
    c.r.bulk("client-info");
    c.r.bulk(e.client_info);
    c.r.bulk("entry-id");
    c.r.integer(static_cast<int64_t>(e.id));
    c.r.bulk("timestamp-created");
    c.r.integer(e.created_ms);
    c.r.bulk("timestamp-last-updated");
    c.r.integer(e.updated_ms);
  }
}

void cmd_acl(Cmd& c) {
  std::string sub = to_lower(c.args[1]);
  if (sub == "log") return acl_log(c);
  if (sub == "setuser") return acl_setuser(c);
  if (sub == "getuser") return acl_getuser(c);
  if (sub == "deluser") return acl_deluser(c);
  if (sub == "list" && c.args.size() == 2) return acl_list(c);
  if (sub == "users" && c.args.size() == 2) return acl_users(c);
  if (sub == "whoami" && c.args.size() == 2) return acl_whoami(c);
  if (sub == "cat" && c.args.size() <= 3) return acl_cat(c);
  c.r.error(std::format("ERR unknown subcommand or wrong number of arguments for '{}'. Try ACL HELP.", c.args[1]));
}

}  // namespace

CommandList acl_commands() { return {{"acl", {cmd_acl, -2}}}; }

}  // namespace datadam
