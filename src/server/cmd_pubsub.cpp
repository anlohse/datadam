// Pub/Sub: SUBSCRIBE / UNSUBSCRIBE / PSUBSCRIBE / PUNSUBSCRIBE / SSUBSCRIBE /
// SUNSUBSCRIBE / PUBLISH / SPUBLISH / PUBSUB.
//
// The first SUBSCRIBE gives the connection a Subscriber, and from then on
// all its output goes through it: what the connection replies and the
// messages published to it are sent in the order they were added. A
// subscription command hands over what was replied so far before it starts
// listening, so its confirmation always comes before the first message.
// Messages stay on this server: they are not sent to replicas. Shard
// channels (SSUBSCRIBE, SPUBLISH) behave like plain ones on a single server.

#include <format>

#include "server/command.h"

namespace datadam {

namespace {

using Kind = PubSub::Kind;

const std::shared_ptr<Subscriber>& subscriber_of(Cmd& c) {
  if (!c.client.subscriber) c.client.subscriber = std::make_shared<Subscriber>(c.r.proto());
  return c.client.subscriber;
}

// What has been replied so far goes out ahead of anything published later.
// Also shows the subscriptions in CLIENT LIST.
void hand_over(Cmd& c) {
  if (!c.client.subscriber) return;
  if (ClientEntry* e = c.client.entry.get()) {
    const Subscriber& s = *c.client.subscriber;
    e->subscriptions = static_cast<uint32_t>(s.channels.size());
    e->patterns = static_cast<uint32_t>(s.patterns.size());
    e->shard_subscriptions = static_cast<uint32_t>(s.shard_channels.size());
    e->pubsub = s.count() > 0;
  }
  c.client.subscriber->push(c.r.buffer());
  c.r.buffer().clear();
}

bool channel_allowed(Cmd& c, std::string_view channel, bool is_pattern) {
  const AclUser* u = c.client.user.get();
  if (!u || u->may_use_channel(channel, is_pattern)) return true;
  c.ctx.stats.acl_denied.fetch_add(1, std::memory_order_relaxed);
  c.ctx.acl_log.add("channel", std::string(channel), u->name, client_info(c.client));
  return false;
}

std::set<std::string, std::less<>>& set_of(Subscriber& s, Kind kind) {
  return kind == Kind::Pattern ? s.patterns : kind == Kind::Shard ? s.shard_channels : s.channels;
}

// What a confirmation counts: shard subscriptions apart from the others, as in Redis.
size_t counted(const Subscriber& s, Kind kind) {
  return kind == Kind::Shard ? s.shard_channels.size() : s.channels.size() + s.patterns.size();
}

void subscribe_generic(Cmd& c, Kind kind, std::string_view word) {
  for (size_t i = 1; i < c.args.size(); ++i) {
    if (!channel_allowed(c, c.args[i], kind == Kind::Pattern)) {
      return c.r.error("NOPERM No permissions to access a channel");
    }
  }
  const auto& sub = subscriber_of(c);
  auto& set = set_of(*sub, kind);
  std::vector<std::string_view> added;
  for (size_t i = 1; i < c.args.size(); ++i) {
    if (set.emplace(c.args[i]).second) added.push_back(c.args[i]);
    c.r.push(3);
    c.r.bulk(word);
    c.r.bulk(c.args[i]);
    c.r.integer(static_cast<int64_t>(counted(*sub, kind)));
  }
  hand_over(c);
  for (auto name : added) c.ctx.pubsub.subscribe(sub, name, kind);
}

void unsubscribe_generic(Cmd& c, Kind kind, std::string_view word) {
  Subscriber* sub = c.client.subscriber.get();
  std::vector<std::string> names;
  if (c.args.size() > 1) {
    names.assign(c.args.begin() + 1, c.args.end());
  } else if (sub) {
    names.assign(set_of(*sub, kind).begin(), set_of(*sub, kind).end());
  }
  if (names.empty()) {  // nothing to leave: one confirmation anyway
    c.r.push(3);
    c.r.bulk(word);
    c.r.null();
    c.r.integer(sub ? static_cast<int64_t>(counted(*sub, kind)) : 0);
    return hand_over(c);
  }
  for (const auto& name : names) {
    if (sub && set_of(*sub, kind).erase(name)) c.ctx.pubsub.unsubscribe(sub, name, kind);
    c.r.push(3);
    c.r.bulk(word);
    c.r.bulk(name);
    c.r.integer(sub ? static_cast<int64_t>(counted(*sub, kind)) : 0);
  }
  hand_over(c);
}

void cmd_subscribe(Cmd& c) { subscribe_generic(c, Kind::Channel, "subscribe"); }
void cmd_psubscribe(Cmd& c) { subscribe_generic(c, Kind::Pattern, "psubscribe"); }
void cmd_ssubscribe(Cmd& c) { subscribe_generic(c, Kind::Shard, "ssubscribe"); }
void cmd_unsubscribe(Cmd& c) { unsubscribe_generic(c, Kind::Channel, "unsubscribe"); }
void cmd_punsubscribe(Cmd& c) { unsubscribe_generic(c, Kind::Pattern, "punsubscribe"); }
void cmd_sunsubscribe(Cmd& c) { unsubscribe_generic(c, Kind::Shard, "sunsubscribe"); }

void publish_generic(Cmd& c, bool shard) {
  if (!channel_allowed(c, c.args[1], false)) return c.r.error("NOPERM No permissions to access a channel");
  c.r.integer(static_cast<int64_t>(c.ctx.pubsub.publish(c.args[1], c.args[2], shard)));
}

void cmd_publish(Cmd& c) { publish_generic(c, false); }
void cmd_spublish(Cmd& c) { publish_generic(c, true); }

// PUBSUB CHANNELS [pattern] | NUMSUB [channel ...] | NUMPAT | SHARDCHANNELS [pattern] | SHARDNUMSUB [channel ...]
void cmd_pubsub(Cmd& c) {
  std::string sub = to_lower(c.args[1]);
  bool shard = sub.starts_with("shard");
  if (shard) sub.erase(0, 5);
  if (sub == "channels" && c.args.size() <= 3) {
    std::optional<std::string_view> pattern;
    if (c.args.size() == 3) pattern = c.args[2];
    auto names = c.ctx.pubsub.channels(pattern, shard);
    c.r.array(names.size());
    for (const auto& n : names) c.r.bulk(n);
  } else if (sub == "numsub") {
    c.r.map(c.args.size() - 2);
    for (size_t i = 2; i < c.args.size(); ++i) {
      c.r.bulk(c.args[i]);
      c.r.integer(static_cast<int64_t>(c.ctx.pubsub.subscribers(c.args[i], shard)));
    }
  } else if (sub == "numpat" && !shard && c.args.size() == 2) {
    c.r.integer(static_cast<int64_t>(c.ctx.pubsub.patterns()));
  } else if (sub == "help" && !shard) {
    static const char* lines[] = {
        "PUBSUB <subcommand> [<arg> [value] [opt] ...]. Subcommands are:",
        "CHANNELS [<pattern>]", "    Return the currently active channels matching a <pattern> (default: '*').",
        "NUMPAT", "    Return number of subscriptions to patterns.",
        "NUMSUB [<channel> ...]", "    Return the number of subscribers for the specified channels.",
        "SHARDCHANNELS [<pattern>]", "    Return the currently active shard level channels matching a <pattern>.",
        "SHARDNUMSUB [<shardchannel> ...]", "    Return the number of subscribers for the specified shard channels.",
    };
    c.r.array(std::size(lines));
    for (const char* l : lines) c.r.simple(l);
  } else {
    c.r.error(std::format("ERR unknown subcommand or wrong number of arguments for '{}'. Try PUBSUB HELP.",
                          c.args[1]));
  }
}

}  // namespace

CommandList pubsub_commands() {
  return {
      {"subscribe", {cmd_subscribe, -2}},     {"psubscribe", {cmd_psubscribe, -2}},
      {"ssubscribe", {cmd_ssubscribe, -2}},   {"unsubscribe", {cmd_unsubscribe, -1}},
      {"punsubscribe", {cmd_punsubscribe, -1}}, {"sunsubscribe", {cmd_sunsubscribe, -1}},
      {"publish", {cmd_publish, 3}},          {"spublish", {cmd_spublish, 3}},
      {"pubsub", {cmd_pubsub, -2}},
  };
}

}  // namespace datadam
