#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace datadam {

// The output side of a connection that subscribed to something. Publishers
// (any thread) and the connection itself (its own replies, in order) append
// to it; the connection's writer takes what is waiting and sends it.
class Subscriber {
 public:
  // A subscriber that falls this far behind is cut off (Redis's
  // client-output-buffer-limit for pubsub clients is 32 MB).
  static constexpr size_t kMaxPending = 32 << 20;

  explicit Subscriber(int proto) : proto_(proto) {}

  int proto() const { return proto_.load(std::memory_order_relaxed); }
  void set_proto(int p) { proto_.store(p, std::memory_order_relaxed); }

  void push(std::string_view data);
  // Moves what is waiting into *out; false if nothing is.
  bool take(std::string* out);
  // No more output: set when the connection ends, or when it fell too far behind.
  void close();
  bool closed() const;
  bool overflowed() const;
  // Called (outside the lock) when output arrives for an idle writer.
  void set_notify(std::function<void()> notify);

  // The subscriptions, kept by the connection's own thread.
  std::set<std::string, std::less<>> channels, patterns, shard_channels;
  size_t count() const { return channels.size() + patterns.size() + shard_channels.size(); }

 private:
  std::atomic<int> proto_;
  mutable std::mutex mu_;
  std::string pending_;
  bool closed_ = false;
  bool overflowed_ = false;
  std::function<void()> notify_;
};

// Who listens to what. Subscribers are held weakly: one whose connection is
// gone is skipped and pruned.
class PubSub {
 public:
  enum class Kind { Channel, Pattern, Shard };

  void subscribe(const std::shared_ptr<Subscriber>& s, std::string_view name, Kind kind);
  void unsubscribe(const Subscriber* s, std::string_view name, Kind kind);
  // Every subscription of s (its connection ends).
  void unsubscribe_all(Subscriber& s);
  // PUBLISH / SPUBLISH: delivers to the channel's subscribers (and, unless
  // shard, to matching patterns); returns how many received it.
  size_t publish(std::string_view channel, std::string_view message, bool shard);

  // PUBSUB CHANNELS / SHARDCHANNELS: channels with subscribers, optionally matching a pattern.
  std::vector<std::string> channels(std::optional<std::string_view> pattern, bool shard) const;
  // PUBSUB NUMSUB / SHARDNUMSUB.
  size_t subscribers(std::string_view channel, bool shard) const;
  // PUBSUB NUMPAT: patterns with subscribers.
  size_t patterns() const;

 private:
  using Subs = std::vector<std::weak_ptr<Subscriber>>;
  using Map = std::map<std::string, Subs, std::less<>>;
  Map& map_of(Kind kind) { return kind == Kind::Pattern ? patterns_ : kind == Kind::Shard ? shard_ : channels_; }
  void prune();

  mutable std::shared_mutex mu_;
  Map channels_, patterns_, shard_;
};

}  // namespace datadam
