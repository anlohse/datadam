#include "server/pubsub.h"

#include <algorithm>

#include "server/resp.h"
#include "util/strings.h"

namespace datadam {

void Subscriber::push(std::string_view data) {
  if (data.empty()) return;
  std::function<void()> notify;
  {
    std::lock_guard lock(mu_);
    if (closed_) return;
    bool was_idle = pending_.empty();
    if (pending_.size() + data.size() > kMaxPending) {
      // Too far behind: what is waiting is dropped and the connection closed.
      pending_.clear();
      overflowed_ = closed_ = true;
      notify = notify_;
    } else {
      pending_.append(data);
      if (was_idle) notify = notify_;
    }
  }
  if (notify) notify();
}

bool Subscriber::take(std::string* out) {
  std::lock_guard lock(mu_);
  if (pending_.empty()) return false;
  out->swap(pending_);
  pending_.clear();
  return true;
}

void Subscriber::close() {
  std::function<void()> notify;
  {
    std::lock_guard lock(mu_);
    if (closed_) return;
    closed_ = true;
    notify = notify_;
  }
  if (notify) notify();
}

bool Subscriber::closed() const {
  std::lock_guard lock(mu_);
  return closed_;
}

bool Subscriber::overflowed() const {
  std::lock_guard lock(mu_);
  return overflowed_;
}

void Subscriber::set_notify(std::function<void()> notify) {
  bool pending;
  {
    std::lock_guard lock(mu_);
    notify_ = notify;
    pending = !pending_.empty() || closed_;
  }
  if (pending && notify) notify();
}

void PubSub::subscribe(const std::shared_ptr<Subscriber>& s, std::string_view name, Kind kind) {
  std::unique_lock lock(mu_);
  Map& m = map_of(kind);
  auto it = m.find(name);
  if (it == m.end()) it = m.emplace(std::string(name), Subs{}).first;
  it->second.push_back(s);
}

void PubSub::unsubscribe(const Subscriber* s, std::string_view name, Kind kind) {
  std::unique_lock lock(mu_);
  Map& m = map_of(kind);
  auto it = m.find(name);
  if (it == m.end()) return;
  std::erase_if(it->second, [&](const std::weak_ptr<Subscriber>& w) {
    auto p = w.lock();
    return !p || p.get() == s;
  });
  if (it->second.empty()) m.erase(it);
}

void PubSub::unsubscribe_all(Subscriber& s) {
  for (const auto& c : s.channels) unsubscribe(&s, c, Kind::Channel);
  for (const auto& p : s.patterns) unsubscribe(&s, p, Kind::Pattern);
  for (const auto& c : s.shard_channels) unsubscribe(&s, c, Kind::Shard);
  s.channels.clear();
  s.patterns.clear();
  s.shard_channels.clear();
}

namespace {

// A message as one protocol sees it: ["message", channel, payload],
// ["pmessage", pattern, channel, payload] or ["smessage", channel, payload].
std::string encode_message(int proto, std::string_view kind, std::string_view pattern, std::string_view channel,
                           std::string_view message) {
  std::string out;
  Reply r(out, proto);
  r.push(pattern.empty() ? 3 : 4);
  r.bulk(kind);
  if (!pattern.empty()) r.bulk(pattern);
  r.bulk(channel);
  r.bulk(message);
  return out;
}

}  // namespace

size_t PubSub::publish(std::string_view channel, std::string_view message, bool shard) {
  size_t receivers = 0;
  bool stale = false;
  {
    std::shared_lock lock(mu_);
    // Encoded once per protocol, when someone needs it.
    std::string encoded[2];
    auto deliver = [&](const Subs& subs, std::string_view kind, std::string_view pattern) {
      for (const auto& w : subs) {
        auto s = w.lock();
        if (!s) {
          stale = true;
          continue;
        }
        int p = s->proto() >= 3 ? 1 : 0;
        if (encoded[p].empty()) encoded[p] = encode_message(p ? 3 : 2, kind, pattern, channel, message);
        s->push(encoded[p]);
        ++receivers;
      }
      encoded[0].clear();  // the next list may need another kind or pattern
      encoded[1].clear();
    };
    const Map& direct = shard ? shard_ : channels_;
    if (auto it = direct.find(channel); it != direct.end()) {
      deliver(it->second, shard ? "smessage" : "message", {});
    }
    if (!shard) {
      for (const auto& [pattern, subs] : patterns_) {
        if (glob_match(pattern, channel)) deliver(subs, "pmessage", pattern);
      }
    }
  }
  if (stale) prune();
  return receivers;
}

void PubSub::prune() {
  std::unique_lock lock(mu_);
  for (Map* m : {&channels_, &patterns_, &shard_}) {
    for (auto it = m->begin(); it != m->end();) {
      std::erase_if(it->second, [](const std::weak_ptr<Subscriber>& w) { return w.expired(); });
      it = it->second.empty() ? m->erase(it) : std::next(it);
    }
  }
}

std::vector<std::string> PubSub::channels(std::optional<std::string_view> pattern, bool shard) const {
  std::shared_lock lock(mu_);
  std::vector<std::string> out;
  for (const auto& [name, subs] : shard ? shard_ : channels_) {
    bool live = std::any_of(subs.begin(), subs.end(), [](const auto& w) { return !w.expired(); });
    if (live && (!pattern || glob_match(*pattern, name))) out.push_back(name);
  }
  return out;
}

size_t PubSub::subscribers(std::string_view channel, bool shard) const {
  std::shared_lock lock(mu_);
  const Map& m = shard ? shard_ : channels_;
  auto it = m.find(channel);
  if (it == m.end()) return 0;
  return static_cast<size_t>(
      std::count_if(it->second.begin(), it->second.end(), [](const auto& w) { return !w.expired(); }));
}

size_t PubSub::patterns() const {
  std::shared_lock lock(mu_);
  size_t n = 0;
  for (const auto& [name, subs] : patterns_) {
    if (std::any_of(subs.begin(), subs.end(), [](const auto& w) { return !w.expired(); })) ++n;
  }
  return n;
}

}  // namespace datadam
