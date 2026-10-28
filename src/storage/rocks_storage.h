#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "config/config.h"
#include "util/latency_histogram.h"

namespace rocksdb {
class Cache;
class DB;
class ColumnFamilyHandle;
class Iterator;
class Snapshot;
class TransactionLogIterator;
class WriteBatch;
}  // namespace rocksdb

namespace datadam {

class StorageError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// RocksDB-backed persistence. Column families:
//   meta  - one record per user key (type, flags, TTL, cas, inline value)
//   data  - elements of collections (hash fields, set members, ...), see data_keys.h
//   index - secondary index entries
// The default column family (System) holds internal state such as the
// collection version mark. Errors are reported as StorageError.

enum class Cf : uint8_t { Meta, Data, Index, System };

struct StorageOptions {
  std::string dir = "./data";
  FsyncPolicy fsync = FsyncPolicy::EverySec;
  WritePath write_path = WritePath::Wal;
  uint64_t block_cache = 64ULL << 20;
  uint64_t write_buffer = 64ULL << 20;
  int background_jobs = 4;
  // Keep WAL files this long after they are no longer needed for recovery
  // (0: delete them at once), and at most this many bytes of them (0: no
  // limit). They are what read_wal reads: the change feed (CDC).
  uint64_t wal_retention_s = 0;
  uint64_t wal_retention_bytes = 0;
  // How much more than wal_retention_bytes is kept while a replica still
  // needs it (hold_wal).
  uint64_t wal_hold_bytes = 0;
};

// What read_wal hands out: the entries of each write batch, in order.
class WalVisitor {
 public:
  virtual ~WalVisitor() = default;
  // Before a batch of `count` operations whose first sequence number is
  // `seq`; false stops the reading there (the batch is not visited).
  virtual bool begin(uint64_t seq, uint64_t count) = 0;
  virtual void put(Cf cf, std::string_view key, std::string_view value) = 0;
  virtual void del(Cf cf, std::string_view key) = 0;
  virtual void delete_range(Cf cf, std::string_view begin, std::string_view end) = 0;
  // Data that only the WAL carries (Batch::put_log_data).
  virtual void log_data(std::string_view blob) = 0;
  virtual void end() = 0;
};

// Tells the index compaction filter which key owns an index entry.
class IndexOwnerResolver {
 public:
  virtual ~IndexOwnerResolver() = default;
  // 1: *owner, *json (the owner is a JSON document, not a hash) and *version
  // (of the owner hash) are set; 0: keep the entry as is; -1: drop it (its
  // index is gone).
  virtual int owner_of(std::string_view key, std::string_view value, std::string* owner, bool* json,
                       uint64_t* version) const = 0;
};

class RocksStorage {
 public:
  class Batch {
   public:
    Batch();
    ~Batch();
    Batch(Batch&&) noexcept;
    Batch& operator=(Batch&&) noexcept;
    void put(std::string_view key, std::string_view value) { put(Cf::Meta, key, value); }
    void del(std::string_view key) { del(Cf::Meta, key); }
    void put(Cf cf, std::string_view key, std::string_view value);
    void del(Cf cf, std::string_view key);
    // Deletes [begin, end).
    void delete_range(Cf cf, std::string_view begin, std::string_view end);
    // A blob written to the WAL with the batch but never stored (for read_wal).
    void put_log_data(std::string_view blob);
    // The encoded batch, and its size in bytes.
    std::string_view data() const;
    size_t size() const;
    void clear();
    bool empty() const;

   private:
    friend class RocksStorage;
    std::unique_ptr<rocksdb::WriteBatch> wb_;
    const RocksStorage* owner_ = nullptr;
  };

  // A consistent point-in-time view for reads.
  class Snapshot {
   public:
    ~Snapshot();
    // The sequence number of the last write the snapshot sees.
    uint64_t sequence() const;
    Snapshot(const Snapshot&) = delete;
    Snapshot& operator=(const Snapshot&) = delete;

   private:
    friend class RocksStorage;
    Snapshot() = default;
    rocksdb::DB* db_ = nullptr;
    const rocksdb::Snapshot* snap_ = nullptr;
  };

  // Iterates the keys of one column family within [lower, upper); an empty
  // upper bound means no bound.
  class Cursor {
   public:
    ~Cursor();
    Cursor(const Cursor&) = delete;
    Cursor& operator=(const Cursor&) = delete;

    void seek_first();
    void seek_last();
    bool valid() const;
    void next();
    void prev();
    std::string_view key() const;
    std::string_view value() const;
    // Throws StorageError if iteration stopped because of an error.
    void check_status() const;

   private:
    friend class RocksStorage;
    struct Bounds;
    Cursor() = default;
    std::unique_ptr<Bounds> bounds_;  // must outlive the iterator
    std::unique_ptr<rocksdb::Iterator> it_;
  };

  static std::unique_ptr<RocksStorage> open(const StorageOptions& opts);
  ~RocksStorage();

  bool get(std::string_view key, std::string* value) { return get(Cf::Meta, key, value); }
  bool get(Cf cf, std::string_view key, std::string* value, const Snapshot* snapshot = nullptr);
  // A point read that only looks in memory (memtables, the block cache, and
  // the filters there): whether the key is found, missing, or would need a
  // file read to tell.
  enum class Probe { Found, Missing, NeedsIo };
  Probe probe(Cf cf, std::string_view key);
  // Point reads of several keys in one RocksDB MultiGet, which shares the
  // index and filter work and reads keys of the same block once.
  // (*values)[i] is nullopt if keys[i] is missing.
  void multi_get(Cf cf, std::span<const std::string_view> keys, std::vector<std::optional<std::string>>* values);
  void put(std::string_view key, std::string_view value) { put(Cf::Meta, key, value); }
  void put(Cf cf, std::string_view key, std::string_view value);
  void del(std::string_view key);
  Batch batch();
  void write(Batch& b);
  std::unique_ptr<Cursor> cursor(Cf cf, std::string lower, std::string upper, bool fill_cache = true,
                                 const Snapshot* snapshot = nullptr);
  std::unique_ptr<Snapshot> snapshot();
  // Used by the index compaction filter; null disables it. Must outlive its use.
  void set_index_resolver(const IndexOwnerResolver* resolver) { index_resolver_.store(resolver); }

  // Visits every meta record in key order until fn returns false.
  void scan(const std::function<bool(std::string_view key, std::string_view value)>& fn);
  // Removes everything (all column families but the default one) in one
  // write; log_data, if any, goes to the WAL with it.
  void delete_all(std::string_view log_data = {});

  uint64_t estimate_keys();
  std::string property(const std::string& name);
  // Write stalls (RocksDB slowing or stopping writes because flushes or
  // compactions fall behind), counted since the database was opened, summed
  // over the column families; and what is behind right now.
  struct WriteStalls {
    uint64_t delays = 0, stops = 0;  // every cause
    uint64_t memtable_delays = 0, memtable_stops = 0;  // too many memtables waiting for flush
    uint64_t l0_delays = 0, l0_stops = 0;  // too many L0 files
    uint64_t pending_compaction_delays = 0, pending_compaction_stops = 0;
    uint64_t immutable_memtables = 0;      // waiting for flush now
    uint64_t pending_compaction_bytes = 0;
    uint64_t running_flushes = 0, running_compactions = 0;
    uint64_t delayed_write_rate = 0;  // bytes/s while delaying; 0 when not
    bool write_stopped = false;
  };
  WriteStalls write_stalls();
  // Makes buffered WAL data durable according to the fsync policy.
  void sync_wal();
  void flush();
  // A consistent copy of the whole database in `dir`, which must not exist:
  // the memtables are flushed, then the SST files are hard-linked (copied
  // when `dir` is on another file system). The copy opens as a database.
  void checkpoint(const std::string& dir);
  // Compacts every column family (runs the compaction filters over all data).
  void compact();

  FsyncPolicy fsync_policy() const { return fsync_.load(std::memory_order_relaxed); }
  // Runtime changes (CONFIG SET).
  void set_fsync_policy(FsyncPolicy p) { fsync_.store(p, std::memory_order_relaxed); }
  void set_block_cache_size(uint64_t bytes);
  void set_write_buffer_size(uint64_t bytes);

  // ---- change feed (the retained WAL)

  // Whether the WAL is written and kept (wal_retention_s > 0).
  bool wal_retained() const;
  // Sequence number of the last write (0: none yet).
  uint64_t latest_sequence() const;
  // Lowest sequence number read_wal can still start from (latest + 1 when no WAL is kept).
  uint64_t oldest_sequence() const;
  // Bytes of WAL on disk (live and kept).
  uint64_t wal_bytes() const;
  // Bytes of the live WAL (what a restart replays).
  uint64_t live_wal_bytes() const;
  // Deletes the oldest kept WAL files while they exceed wal_retention_bytes
  // (RocksDB would check only every 10 minutes, which heavy writes can
  // overshoot by far: the server calls this every second). Files holding
  // sequences from the held one on are kept up to wal_hold_bytes longer.
  // Returns the number of files deleted.
  size_t trim_wal();
  // The oldest sequence a replica still needs (0: none): a replica in a long
  // full sync under heavy writes would otherwise lose the WAL it continues
  // from, and sync again, and again.
  void hold_wal(uint64_t seq) { wal_hold_.store(seq, std::memory_order_relaxed); }
  // Visits the write batches from the one holding sequence `from` (0: the
  // oldest kept) to the newest. Throws StorageError if `from` is older than
  // what is kept.
  void read_wal(uint64_t from, WalVisitor& visitor) const;
  // The same, undecoded (for replication): whole batches from `from` until
  // about max_bytes were read. Returns the number of batches added.
  struct RawBatch {
    uint64_t seq = 0;
    uint64_t count = 0;  // operations: the next batch starts at seq + count
    std::string data;    // a rocksdb::WriteBatch, see decode_batch
  };
  size_t read_wal_raw(uint64_t from, size_t max_bytes, std::vector<RawBatch>* out) const;
  // read_wal_raw that keeps its place (a replica's stream): finding a sequence
  // reads the WAL file it is in from the start, so each read goes on from the
  // last instead, at the tail of the live WAL file too, and seeks again only
  // when RocksDB moves on to a new file. One reader per stream, one thread at a time.
  class WalReader {
   public:
    WalReader(const RocksStorage& storage, uint64_t from);
    ~WalReader();
    // The batches after the last read, until about max_bytes were read; throws
    // StorageError if they are no longer kept. Returns the number added.
    size_t read(size_t max_bytes, std::vector<RawBatch>* out);

   private:
    const RocksStorage& storage_;
    uint64_t next_;
    std::unique_ptr<rocksdb::TransactionLogIterator> it_;  // null: seek next_ at the next read
  };
  // Column family ids as they appear in this database's batches, by Cf.
  using CfIds = std::array<uint32_t, 4>;  // meta, data, index, system
  CfIds cf_ids() const;
  // Visits a batch written by a database whose column family ids are `ids`
  // (begin and end are not called).
  void decode_batch(std::string_view data, const CfIds& ids, WalVisitor& visitor) const;
  // Calls fn once, from the writing thread, after the next write that takes
  // the sequence past after_seq; returns an id for cancel_write_wait, or 0 if
  // that already happened (fn is then not called).
  uint64_t on_next_write(uint64_t after_seq, std::function<void()> fn);
  void cancel_write_wait(uint64_t id);
  void cancel_all_write_waits();
  // Runs every on_next_write callback now, as if a write had happened (a
  // replication stream that has something other than WAL to send).
  void wake_write_waiters() { wrote(); }

  // Time spent in point reads, multi-key reads and batch writes (INFO latency).
  const LatencyHistogram& get_latency() const { return get_latency_; }
  const LatencyHistogram& multi_get_latency() const { return multi_get_latency_; }
  const LatencyHistogram& write_latency() const { return write_latency_; }
  void reset_latency() {
    get_latency_.reset();
    multi_get_latency_.reset();
    write_latency_.reset();
  }

 private:
  class DataGcFilterFactory;
  class IndexGcFilterFactory;

  bool skip_writes() const { return opts_.write_path == WritePath::Memory; }
  void wrote();  // after every write: wakes on_next_write waiters
  // An iterator over the WAL from the batch holding `from` (0: the oldest
  // kept), or null if nothing was written since; throws StorageError if `from`
  // is older than the oldest kept file. A first batch past `from` (a gap) is
  // for the reader to check: GetBatch moves the batch out, so it is read once.
  std::unique_ptr<rocksdb::TransactionLogIterator> wal_from(uint64_t from) const;
  // Calls fn for every batch from `from` on (see read_wal), until it returns false.
  void for_each_wal_batch(uint64_t from, const std::function<bool(uint64_t seq, rocksdb::WriteBatch& batch)>& fn) const;
  Cf cf_of(uint32_t column_family_id) const;
  rocksdb::ColumnFamilyHandle* handle(Cf cf) const;

  explicit RocksStorage(const StorageOptions& opts);

  StorageOptions opts_;
  std::atomic<FsyncPolicy> fsync_;
  std::atomic<uint64_t> wal_hold_{0};
  std::shared_ptr<rocksdb::Cache> block_cache_;
  std::unique_ptr<rocksdb::DB> db_;
  std::vector<rocksdb::ColumnFamilyHandle*> handles_;
  rocksdb::ColumnFamilyHandle* meta_ = nullptr;
  // What the data compaction filter reads meta records from; null while the
  // database is opening or closing (it then keeps everything).
  std::atomic<rocksdb::DB*> gc_db_{nullptr};
  std::atomic<const IndexOwnerResolver*> index_resolver_{nullptr};
  LatencyHistogram get_latency_;
  std::mutex write_waits_mu_;
  std::atomic<bool> has_write_waits_{false};
  uint64_t next_write_wait_ = 1;
  std::vector<std::pair<uint64_t, std::function<void()>>> write_waits_;
  LatencyHistogram multi_get_latency_;
  LatencyHistogram write_latency_;
};

}  // namespace datadam
