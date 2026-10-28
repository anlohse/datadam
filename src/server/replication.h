#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "db/key_watch.h"

namespace datadam {

class RocksStorage;
class Reply;

// Replication over the Redis protocol. A replica connects to its primary and
// sends REPLSYNC <replid> <offset>; the primary answers
//   +CONTINUE <replid> <next> <cf ids>   it still keeps the WAL from offset, or
//   +FULLSYNC <replid> <next> <cf ids>   followed by ["snap", batch]... ["snapend"],
// then streams ["wal", next, batch] for every write batch, and ["ping"] when
// idle. Batches are raw RocksDB write batches; "next" is the primary's
// sequence number after them, which the replica keeps (with the batch, in the
// same write) as its offset. The replica sends REPLCONF ACK <offset> as soon
// as it applied what arrived, and at least every second.
//
// FAILOVER: once the replica has everything, the primary sends
// ["failover", next]; a replica at exactly that offset answers
// REPLCONF FAILOVER-ACK <offset> and becomes a primary, and the old primary
// then follows it.
//
// After a failover the history goes on: a promoted replica keeps which
// primary it followed and how far (ReplPrevious). The other replicas of that
// primary, and the old primary itself, ask for REPLSYNC <old id> <offset>; if
// they are not past the promotion point, the batches they miss are in the
// promoted replica's own WAL (each with the old primary's offset after it, in
// repl:offset), so it answers +CONTINUE at the matching place of its own WAL
// instead of a full sync.

// System column family keys of the replication state; a replica never takes
// the primary's own ones.
inline constexpr std::string_view kReplOwnIdKey = "repl:id";             // this server's replication id
inline constexpr std::string_view kReplPrimaryIdKey = "repl:primary-id";  // replica: the id it follows
inline constexpr std::string_view kReplOffsetKey = "repl:offset";         // replica: next sequence to fetch
inline constexpr std::string_view kReplSegmentKey = "repl:segment";       // replica: where its copy began
inline constexpr std::string_view kReplPreviousKey = "repl:previous";     // promoted: the history it continues

// Where a replica's copy of its primary's history began: the primary's offset
// `offset` holds this server's data up to its own sequence `own` (the next it
// would write). Written with the batch that starts the copy: the end of a full
// sync, or the switch to a promoted replica.
struct ReplSegment {
  uint64_t offset = 0;
  uint64_t own = 0;
};

// What a promoted replica keeps of the history it followed.
struct ReplPrevious {
  std::string id;       // the primary it followed
  uint64_t offset = 0;  // that primary's offset it had reached
  uint64_t own = 0;     // its own next sequence then: the old history ends there
  ReplSegment segment;  // where its copy of that history began
};

std::string encode_segment(const ReplSegment& segment);
std::optional<ReplSegment> read_segment(RocksStorage& storage);
std::optional<ReplPrevious> read_previous(RocksStorage& storage);
// A replica becomes a primary: drops its replica state, keeping what it
// followed as ReplPrevious (if it had a complete copy).
void keep_history_at_promotion(RocksStorage& storage);
// Where in this server's own WAL a replica that followed `id` up to `offset`
// goes on, or nullopt (a full sync): `id` must be the history this server
// continues, and `offset` within this server's copy of it.
std::optional<uint64_t> continue_previous(RocksStorage& storage, std::string_view id, uint64_t offset);

// Snapshot and WAL batches sent to a replica are about this large.
inline constexpr size_t kReplBatchBytes = 1 << 20;

// What INFO replication and ROLE show. Thread-safe.
class ReplicationState {
 public:
  struct Replica {
    uint64_t id = 0;
    std::string ip;
    uint16_t listening_port = 0;
    std::string state;  // "sync" (full sync under way) or "online"
    uint64_t ack_offset = 0;
    int64_t last_ack_ms = 0;
    uint64_t failover_at = 0;  // FAILOVER: send ["failover", failover_at] once the stream reaches it
    bool failover_sent = false;
    bool failover_ok = false;  // the replica answered FAILOVER-ACK
    bool failover_force = false;  // FAILOVER FORCE: it takes over even if it is behind
    uint64_t needs = 0;           // the oldest WAL sequence it may still ask for (0: none yet)
  };

  // This server's replication id (generated and stored on first use).
  std::string own_id(RocksStorage& storage);
  // A new id: after a promotion, the history is not the old primary's.
  std::string new_own_id(RocksStorage& storage);

  // ---- replica side
  void set_primary(std::string host, uint16_t port);  // host "": this is a primary
  bool is_replica() const;
  void set_link(std::string state);  // "connecting", "sync", "connected", "down"
  bool link_up() const;
  void set_syncing(bool on);
  bool syncing() const;
  void set_offset(uint64_t offset, std::string primary_id);
  void touched_primary();  // a message came from the primary
  void count_sync(bool full);

  // ---- primary side
  uint64_t add_replica(std::string ip, uint16_t listening_port, bool full);
  void replica_state(uint64_t id, std::string state);
  void replica_ack(uint64_t id, uint64_t offset, RocksStorage& storage);
  // A replica will read the WAL from `seq` (the end of its full sync's snapshot).
  void replica_needs(uint64_t id, uint64_t seq, RocksStorage& storage);
  void remove_replica(uint64_t id, RocksStorage& storage);
  std::vector<Replica> replicas() const;
  // How many replicas acknowledged everything before `offset` (WAIT).
  size_t acked(uint64_t offset) const;
  // WAIT: w fires at the next acknowledgment; forget it when done waiting.
  void wait_ack(std::shared_ptr<KeyWatch::Waiter> w);
  void forget_waiter(const KeyWatch::Waiter* w);
  void drop_waiters();  // the server goes away
  // FAILOVER, primary side.
  void request_failover(uint64_t replica, uint64_t at, bool force);
  bool failover_forced(uint64_t replica) const;
  // For the replica's stream: the offset to send ["failover", at] at, once (nullopt: none).
  std::optional<uint64_t> take_failover(uint64_t replica, uint64_t next);
  void failover_ack(uint64_t replica);
  bool failover_acked(uint64_t replica) const;
  void set_failover_state(std::string state);  // INFO master_failover_state
  std::string failover_state() const;

  // replica_priority: what Redis Sentinel reads as slave_priority.
  std::string info(RocksStorage& storage, unsigned replica_priority = 100);
  void role(Reply& r, RocksStorage& storage);

 private:
  mutable std::mutex mu_;
  std::string own_id_;
  // replica side
  std::string primary_host_;
  uint16_t primary_port_ = 0;
  std::string link_ = "down";
  int64_t link_down_since_ms_ = 0;
  std::atomic<bool> syncing_{false};  // read for every command
  uint64_t offset_ = 0;
  std::string primary_id_;
  int64_t last_io_ms_ = 0;
  uint64_t full_syncs_done_ = 0, partial_syncs_done_ = 0;
  // primary side
  uint64_t next_replica_ = 1;
  std::vector<Replica> replicas_;
  std::vector<std::shared_ptr<KeyWatch::Waiter>> ack_waiters_;
  std::string failover_state_ = "no-failover";
  // The replica that answered FAILOVER-ACK (kept after it disconnects to take over).
  uint64_t failover_taken_by_ = 0;
  uint64_t full_syncs_served_ = 0, partial_syncs_served_ = 0;

  // Tells the storage which WAL the replicas still need (under mu_).
  void hold_wal(RocksStorage& storage) const;
};

}  // namespace datadam
