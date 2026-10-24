#include "server/replication.h"

#include <algorithm>
#include <format>
#include <random>

#include "server/resp.h"
#include "storage/rocks_storage.h"
#include "util/clock.h"

namespace datadam {

namespace {

std::string random_id() {
  std::random_device rd;
  std::string id;
  for (int i = 0; i < 40; ++i) id.push_back("0123456789abcdef"[rd() % 16]);
  return id;
}

}  // namespace

std::string ReplicationState::own_id(RocksStorage& storage) {
  std::lock_guard lock(mu_);
  if (own_id_.empty()) {
    std::string stored;
    if (storage.get(Cf::System, kReplOwnIdKey, &stored) && stored.size() == 40) {
      own_id_ = stored;
    } else {
      own_id_ = random_id();
      storage.put(Cf::System, kReplOwnIdKey, own_id_);
    }
  }
  return own_id_;
}

std::string ReplicationState::new_own_id(RocksStorage& storage) {
  std::lock_guard lock(mu_);
  own_id_ = random_id();
  storage.put(Cf::System, kReplOwnIdKey, own_id_);
  return own_id_;
}

void ReplicationState::set_primary(std::string host, uint16_t port) {
  std::lock_guard lock(mu_);
  primary_host_ = std::move(host);
  primary_port_ = port;
  link_ = "down";
  syncing_ = false;
}

bool ReplicationState::is_replica() const {
  std::lock_guard lock(mu_);
  return !primary_host_.empty();
}

void ReplicationState::set_link(std::string state) {
  std::lock_guard lock(mu_);
  bool was_up = link_ == "connected";
  link_ = std::move(state);
  if (link_ == "connected") link_down_since_ms_ = 0;
  else if (was_up || link_down_since_ms_ == 0) link_down_since_ms_ = steady_time_ms();
}

bool ReplicationState::link_up() const {
  std::lock_guard lock(mu_);
  return link_ == "connected";
}

void ReplicationState::set_syncing(bool on) { syncing_.store(on, std::memory_order_relaxed); }

bool ReplicationState::syncing() const { return syncing_.load(std::memory_order_relaxed); }

void ReplicationState::set_offset(uint64_t offset, std::string primary_id) {
  std::lock_guard lock(mu_);
  offset_ = offset;
  primary_id_ = std::move(primary_id);
}

void ReplicationState::touched_primary() {
  std::lock_guard lock(mu_);
  last_io_ms_ = steady_time_ms();
}

void ReplicationState::count_sync(bool full) {
  std::lock_guard lock(mu_);
  ++(full ? full_syncs_done_ : partial_syncs_done_);
}

uint64_t ReplicationState::add_replica(std::string ip, uint16_t listening_port, bool full) {
  std::lock_guard lock(mu_);
  ++(full ? full_syncs_served_ : partial_syncs_served_);
  uint64_t id = next_replica_++;
  replicas_.push_back(Replica{id, std::move(ip), listening_port, full ? "sync" : "online", 0, steady_time_ms()});
  return id;
}

void ReplicationState::replica_state(uint64_t id, std::string state) {
  std::lock_guard lock(mu_);
  for (auto& r : replicas_) {
    if (r.id == id) r.state = std::move(state);
  }
}

void ReplicationState::replica_ack(uint64_t id, uint64_t offset) {
  std::vector<std::shared_ptr<KeyWatch::Waiter>> wake;
  {
    std::lock_guard lock(mu_);
    for (auto& r : replicas_) {
      if (r.id == id) {
        r.ack_offset = offset;
        r.last_ack_ms = steady_time_ms();
      }
    }
    wake.swap(ack_waiters_);
  }
  for (const auto& w : wake) w->fire();
}

std::vector<ReplicationState::Replica> ReplicationState::replicas() const {
  std::lock_guard lock(mu_);
  return replicas_;
}

size_t ReplicationState::acked(uint64_t offset) const {
  std::lock_guard lock(mu_);
  return static_cast<size_t>(std::count_if(replicas_.begin(), replicas_.end(), [&](const Replica& r) {
    return r.state == "online" && r.ack_offset >= offset;
  }));
}

void ReplicationState::wait_ack(std::shared_ptr<KeyWatch::Waiter> w) {
  std::lock_guard lock(mu_);
  ack_waiters_.push_back(std::move(w));
}

void ReplicationState::forget_waiter(const KeyWatch::Waiter* w) {
  std::lock_guard lock(mu_);
  std::erase_if(ack_waiters_, [&](const auto& p) { return p.get() == w; });
}

void ReplicationState::drop_waiters() {
  std::lock_guard lock(mu_);
  ack_waiters_.clear();
}

void ReplicationState::request_failover(uint64_t replica, uint64_t at, bool force) {
  std::lock_guard lock(mu_);
  failover_taken_by_ = 0;
  for (auto& r : replicas_) {
    if (r.id == replica) {
      r.failover_at = at;
      r.failover_force = force;
      r.failover_sent = false;
      r.failover_ok = false;
    }
  }
}

std::optional<uint64_t> ReplicationState::take_failover(uint64_t replica, uint64_t next) {
  std::lock_guard lock(mu_);
  for (auto& r : replicas_) {
    if (r.id == replica && r.failover_at != 0 && !r.failover_sent && next >= r.failover_at) {
      r.failover_sent = true;
      return r.failover_at;
    }
  }
  return std::nullopt;
}

void ReplicationState::failover_ack(uint64_t replica) {
  std::lock_guard lock(mu_);
  failover_taken_by_ = replica;
  for (auto& r : replicas_) {
    if (r.id == replica) r.failover_ok = true;
  }
}

bool ReplicationState::failover_forced(uint64_t replica) const {
  std::lock_guard lock(mu_);
  for (const auto& r : replicas_) {
    if (r.id == replica) return r.failover_force;
  }
  return false;
}

bool ReplicationState::failover_acked(uint64_t replica) const {
  std::lock_guard lock(mu_);
  return failover_taken_by_ == replica;
}

void ReplicationState::set_failover_state(std::string state) {
  std::lock_guard lock(mu_);
  failover_state_ = std::move(state);
}

std::string ReplicationState::failover_state() const {
  std::lock_guard lock(mu_);
  return failover_state_;
}

void ReplicationState::remove_replica(uint64_t id) {
  std::lock_guard lock(mu_);
  std::erase_if(replicas_, [id](const Replica& r) { return r.id == id; });
}

std::string ReplicationState::info(RocksStorage& storage, unsigned replica_priority) {
  std::string id = own_id(storage);
  uint64_t latest = storage.latest_sequence();
  std::lock_guard lock(mu_);
  int64_t now = steady_time_ms();
  std::string out = "# Replication\r\n";
  if (!primary_host_.empty()) {
    out += std::format(
        "role:slave\r\nmaster_host:{}\r\nmaster_port:{}\r\nmaster_link_status:{}\r\nmaster_last_io_seconds_ago:{}\r\n"
        "master_sync_in_progress:{}\r\nslave_read_repl_offset:{}\r\nslave_repl_offset:{}\r\nslave_priority:{}\r\n"
        "slave_read_only:1\r\nreplica_announced:1\r\nmaster_replid:{}\r\nfull_syncs:{}\r\npartial_syncs:{}\r\n",
        primary_host_, primary_port_, link_ == "connected" ? "up" : "down",
        last_io_ms_ ? (now - last_io_ms_) / 1000 : -1, syncing() ? 1 : 0, offset_, offset_, replica_priority,
        primary_id_, full_syncs_done_, partial_syncs_done_);
    if (link_ != "connected") {
      // Redis Sentinel reads this to judge how stale a replica is.
      out += std::format("master_link_down_since_seconds:{}\r\n",
                         link_down_since_ms_ ? (now - link_down_since_ms_) / 1000 : -1);
    }
  } else {
    out += "role:master\r\n";
  }
  out += std::format("connected_slaves:{}\r\n", replicas_.size());
  for (size_t i = 0; i < replicas_.size(); ++i) {
    const Replica& r = replicas_[i];
    out += std::format("slave{}:ip={},port={},state={},offset={},lag={}\r\n", i, r.ip, r.listening_port, r.state,
                       r.ack_offset, (now - r.last_ack_ms) / 1000);
  }
  out += std::format("master_failover_state:{}\r\n", failover_state_);
  if (primary_host_.empty()) out += std::format("master_replid:{}\r\nmaster_repl_offset:{}\r\n", id, latest + 1);
  out += std::format("own_replid:{}\r\nrepl_offset:{}\r\nfull_syncs_served:{}\r\npartial_syncs_served:{}\r\n\r\n", id,
                     latest + 1, full_syncs_served_, partial_syncs_served_);
  return out;
}

void ReplicationState::role(Reply& r, RocksStorage& storage) {
  uint64_t latest = storage.latest_sequence();
  std::lock_guard lock(mu_);
  if (!primary_host_.empty()) {
    r.array(5);
    r.bulk("slave");
    r.bulk(primary_host_);
    r.integer(primary_port_);
    r.bulk(link_ == "down" ? "connect" : link_);
    r.integer(static_cast<int64_t>(offset_));
    return;
  }
  r.array(3);
  r.bulk("master");
  r.integer(static_cast<int64_t>(latest + 1));
  r.array(replicas_.size());
  for (const Replica& rep : replicas_) {
    r.array(3);
    r.bulk(rep.ip);
    r.bulk(std::to_string(rep.listening_port));
    r.bulk(std::to_string(rep.ack_offset));
  }
}

}  // namespace datadam
