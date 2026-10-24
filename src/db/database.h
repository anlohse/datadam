#pragma once

#include <atomic>
#include <bitset>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cache/object_cache.h"
#include "db/element_set.h"
#include "db/key_locks.h"
#include "db/key_watch.h"
#include "json/json.h"
#include "index/index_query.h"
#include "index/index_registry.h"
#include "index/search_query.h"
#include "storage/record.h"
#include "storage/rocks_storage.h"
#include "util/hash.h"
#include "util/latency_histogram.h"

namespace datadam {

struct WriteBatchStats {
  uint64_t commits = 0;    // RocksDB writes made by write batches
  uint64_t writes = 0;     // puts and deletes they carried
  uint64_t conflicts = 0;  // early commits because another batch held a needed lock
};

// The keyspace: RocksDB is the source of truth, the object cache sits in
// front of it (write-through). All methods are thread-safe.
//
// Strings live entirely in their meta record. Collections (hash, set, zset,
// list) keep a CollectionMeta there and one data entry per element, accessed
// through a KeyTxn (see transact()). Hashes can have secondary indexes, kept
// in the same atomic writes as the hashes themselves.
class Database {
  struct PendingBatch;
  class TxnRun;

 public:
  // What an update() callback wants done with the record it was given.
  enum class Op {
    Keep,        // no change
    Put,         // store *rec with a new cas
    PutKeepCas,  // store *rec, keep its cas (TTL-only changes such as memcache touch)
    Delete,      // remove the key
  };

  enum class RenameResult { Ok, NoSuchKey, TargetExists };

  // resume_backfills: continue the index backfills a restart interrupted (a
  // replica passes false: the primary builds its indexes).
  Database(RocksStorage& storage, ObjectCache& cache, bool cache_on_write = true, bool resume_backfills = true);
  ~Database();

  // ---- replication

  // A replica: nothing is written to RocksDB except what apply_replicated
  // brings. Writes that reads make on their own (removing an expired key)
  // are dropped; commands that write are refused before they run.
  void set_read_only(bool on) { read_only_.store(on, std::memory_order_relaxed); }
  bool read_only() const { return read_only_.load(std::memory_order_relaxed); }
  // Applies a batch from the primary, whose column family ids are `ids`, with
  // `system_puts` (this replica's own state, e.g. its offset) in the same
  // write. The keys it touches are locked meanwhile and leave the cache.
  // Entries of the primary's own replication state ("repl:" in System) are
  // skipped; changed index definitions are reloaded.
  void apply_replicated(std::string_view data, const RocksStorage::CfIds& ids,
                        const std::vector<std::pair<std::string, std::string>>& system_puts);
  // Before a full sync: removes everything, the System column family included.
  void clear_for_full_sync();
  // The end of a full sync: index definitions are all there again.
  void finish_full_sync();
  // A replica that becomes a primary: reloads the version mark and the index
  // definitions, resumes interrupted backfills, and accepts writes again.
  void promote();

  // Groups the writes made on this thread while it is alive into one RocksDB
  // write (group commit for a pipeline). The stripe lock of every key touched
  // stays held until the batch is committed, so no other client can read the
  // old value from disk; this thread's own reads see its pending writes. A
  // batch never waits for a lock while holding others: when a lock is busy it
  // commits first, which keeps it deadlock-free. The scope must not span a
  // coroutine suspension. Commands that lock several keys at once (MSET,
  // RENAME, FLUSHALL, multi-key collection commands) commit the pending writes
  // first.
  class WriteBatchScope {
   public:
    // With enabled == false the scope does nothing. Inside another scope of
    // the same thread it does nothing either (the outer one commits).
    WriteBatchScope(Database& db, bool enabled = true);
    ~WriteBatchScope();  // commits; on failure the writes are dropped and logged
    WriteBatchScope(const WriteBatchScope&) = delete;
    WriteBatchScope& operator=(const WriteBatchScope&) = delete;

    // Commits what is pending. Throws StorageError if this or any earlier
    // commit of the scope failed; replies produced for those writes must then
    // be discarded (the connection is closed).
    void commit();

   private:
    Database& db_;
    std::unique_ptr<PendingBatch> batch_;
  };

  // One key inside transact(): its record plus, for collections, element
  // access. Writes become visible to this thread at once and reach RocksDB
  // atomically with the rest of the transaction.
  class KeyTxn {
   public:
    using ScanFn = std::function<bool(std::string_view sub, std::string_view value)>;

    std::string_view key() const { return key_; }
    bool exists() const { return rec_.has_value(); }
    // True when the key exists and holds another type than t.
    bool wrong_type(ValueType t) const { return rec_ && rec_->type != t; }
    const std::optional<Record>& record() const { return rec_; }

    // Collection metadata; size and head may be changed by the caller.
    CollectionMeta& meta() { return meta_; }
    uint64_t size() const { return rec_ ? meta_.size : 0; }

    // Makes the key an empty collection of type t unless it already is one.
    // Precondition: !wrong_type(t).
    void create(ValueType t);
    // Replaces whatever the key holds with an empty collection of type t.
    void reset(ValueType t);
    // Deletes the key (and all its elements).
    void remove();

    // Elements of the collection, by sub-key.
    std::optional<std::string> get(std::string_view sub);
    void put(std::string_view sub, std::string_view value);
    void del(std::string_view sub);
    // Visits elements with sub-key in [from, to) in order (or reverse order)
    // until fn returns false. An empty `to` means no upper bound. The
    // collection must not be modified during the scan.
    void scan(std::string_view from, std::string_view to, bool reverse, const ScanFn& fn);
    // True when the elements are all in memory (small collection).
    bool in_memory() const { return complete_; }

   private:
    friend class Database;
    friend class Database::TxnRun;

    // An index entry this transaction deletes and/or writes.
    struct IndexChange {
      std::optional<std::string> del;
      std::optional<std::string> put;
      std::string value;
    };

    KeyTxn(Database& db, PendingBatch& batch, std::string key);
    // Read-only, from a cached record; no lock, no batch.
    KeyTxn(Database& db, std::string key, Record cached);
    void init_collection(bool decode_elements = true);
    PendingBatch& batch() const;
    std::string data_key(std::string_view sub) const;
    // Cache tail of a collection whose elements are not all in memory.
    std::string partial_cache_tail() const;
    void drop_elements(const CollectionMeta& m);
    // Remembers the index tuples before the first change.
    void capture();
    void note_type();  // records the element type for the change feed
    IndexDef::Tuples current_tuples(const IndexDef& def);
    // Makes sure the key's entry in this index exists (backfill).
    void reindex(const IndexRegistry::Ptr& index) { reindex_.push_back(index); }
    // Also removes this entry if it is not the key's current one (query repair).
    void repair(const IndexRegistry::Ptr& index, std::string entry) { stale_.emplace_back(index, std::move(entry)); }
    // The UNIQUE indexes this transaction may write to.
    void unique_indexes(std::vector<IndexRegistry::Ptr>* out) const;
    // Computes the index changes and checks UNIQUE constraints (throws
    // IndexConstraintError); nothing is written yet.
    void prepare_indexes();
    void apply_indexes();
    void finish();

    Database& db_;
    PendingBatch* batch_;  // null for a read-only peek
    std::string key_;
    std::optional<Record> rec_;
    CollectionMeta meta_;
    std::optional<CollectionMeta> stored_meta_;  // what is on disk / pending now
    bool had_key_ = false;
    bool removed_ = false;
    std::string prefix_;  // data key prefix of meta_.version

    bool complete_ = false;  // elems_ holds every element (sorted by sub-key)
    bool too_big_ = false;   // known not to fit in the cache
    bool elements_changed_ = false;
    bool populate_ = false;    // elems_ were read from RocksDB: put them in the cache
    bool from_batch_ = false;  // elems_ were taken from the batch (must go back)
    ElementSet elems_;

    std::vector<IndexRegistry::Ptr> indexes_;  // indexes covering this key
    bool captured_ = false;
    std::vector<IndexDef::Tuples> old_tuples_;
    uint64_t old_version_ = 0;
    int64_t old_expire_ = 0;
    uint64_t noted_version_ = 0;  // last version recorded in the batch's element_types
    std::vector<IndexRegistry::Ptr> reindex_;
    std::vector<std::pair<IndexRegistry::Ptr, std::string>> stale_;
    std::vector<IndexChange> changes_;
  };

  // The KeyTxns of transact_many(), indexed like its keys. Repeated keys share
  // one KeyTxn.
  class KeyTxns {
   public:
    KeyTxn& operator[](size_t i) { return *txns_[index_[i]]; }
    size_t size() const { return index_.size(); }

   private:
    friend class Database;
    std::vector<std::unique_ptr<KeyTxn>> txns_;
    std::vector<size_t> index_;
  };

  // Runs f(KeyTxn&) with the key locked. Everything it writes is applied
  // atomically: in the thread's write batch if there is one, otherwise in a
  // batch of its own. If f throws, its writes are discarded.
  template <class F>
  decltype(auto) transact(std::string_view key, F&& f) {
    WriteBatchScope local(*this);
    TxnRun run(*this, std::span<const std::string_view>(&key, 1), false);
    return run.execute(local, [&] { return f(run.txns()[0]); });
  }

  // Same, for several keys locked together: f(KeyTxns&).
  template <class F>
  decltype(auto) transact_many(std::span<const std::string_view> keys, F&& f) {
    WriteBatchScope local(*this);
    TxnRun run(*this, keys, true);
    return run.execute(local, [&] { return f(run.txns()); });
  }

  // Runs f(KeyTxn&) on a collection straight from the cache, without the key
  // lock, if the cache has its record (and, with need_elements, all its
  // elements); otherwise returns false without calling f. f may only read.
  // The record may also be of another type (check wrong_type()).
  template <class F>
  bool peek(std::string_view key, bool need_elements, F&& f) {
    std::optional<Record> rec;
    cache_.lookup(key, [&](const RecordView& v) {
      if (is_collection(v.type) && need_elements &&
          (v.value.size() < kCollectionMetaSize ||
           element_cache_state(v.value.substr(kCollectionMetaSize)) != ElementCache::Complete)) {
        return;
      }
      rec = Record::from(v);
    });
    if (!rec) return false;
    KeyTxn t(*this, std::string(key), std::move(*rec));
    f(t);
    return true;
  }

  // Calls f(const RecordView&) if the key exists. On a cache hit f runs under
  // the cache shard lock, straight from pooled memory (no copy).
  template <class F>
  bool read(std::string_view key, F&& f) {
    if (cache_.lookup(key, f)) return true;
    KeyGuard lock = lock_for_read(key);
    auto rec = load_locked(key);
    if (!rec) return false;
    f(rec->view());
    return true;
  }

  std::optional<Record> get(std::string_view key);
  bool exists(std::string_view key);

  // Atomic read-modify-write of one key. f(std::optional<Record>& rec) -> Op.
  template <class F>
  void update(std::string_view key, F&& f) {
    // With indexes, index entries and the record must reach RocksDB together.
    std::optional<WriteBatchScope> local;
    if (indexes_->any()) local.emplace(*this);
    KeyGuard lock = lock_key(key, true);
    std::optional<Record> rec = load_locked(key);
    std::optional<CollectionMeta> old = collection_meta(rec);
    bool old_hash = rec && rec->type == ValueType::Hash;
    int64_t old_expire = rec ? rec->expire_at_ms : 0;
    JsonEntries json_before;
    if (indexes_->any()) json_before = json_entries(key, rec);
    Op op = f(rec);
    std::vector<std::unique_lock<std::mutex>> unique_locks;
    if (op != Op::Keep && !json_before.indexes.empty()) {
      // A JSON document's entries: worked out (and a UNIQUE clash thrown)
      // before the record is written.
      JsonEntries after = op == Op::Delete ? json_before.gone() : json_entries(key, rec);
      unique_locks = lock_unique(json_before.indexes);
      for (auto& [k, v] : json_entry_changes(key, json_before, after)) write_index(std::move(k), std::move(v));
    }
    switch (op) {
      case Op::Keep: break;
      case Op::Put: store_locked(key, *rec, true); break;
      case Op::PutKeepCas: store_locked(key, *rec, false); break;
      case Op::Delete: erase_locked(key); break;
    }
    if (op != Op::Keep && old) {
      // The elements of a collection that was deleted or replaced are dropped
      // (after the meta record: leftover elements are harmless, see compaction).
      std::optional<CollectionMeta> now = op == Op::Delete ? std::nullopt : collection_meta(rec);
      if (!now || now->version != old->version) {
        if (old_hash && indexes_->any()) drop_index_entries(key, *old);
        drop_elements(key, *old);
      } else if (old_hash && indexes_->any() && rec->expire_at_ms != old_expire) {
        refresh_index_entries(key, *now, rec->expire_at_ms);  // entries carry the TTL
      }
    }
    if (!unique_locks.empty()) commit_active_batch();  // a UNIQUE value is taken once it is written
    if (local) local->commit();
  }

  // Blind write: does not read the previous value. The elements of a
  // collection it overwrites are reclaimed by compaction.
  void put(std::string_view key, Record rec);
  bool del(std::string_view key);

  // Writes every pair in one atomic batch. With only_if_none_exist, writes
  // nothing (and returns false) when any of the keys exists.
  bool mset(const std::vector<std::pair<std::string_view, std::string_view>>& kvs, bool only_if_none_exist);
  RenameResult rename(std::string_view src, std::string_view dst, bool only_if_target_missing);

  void flush_all();
  std::vector<std::string> keys(std::string_view pattern);

  struct KeyScan {
    std::vector<std::string> keys;
    std::optional<std::string> next;  // where to resume (the last key looked at); nullopt: done
  };
  // SCAN: walks the keys starting with `prefix` in key order, from just after
  // `after` ("" = the beginning), looking at no more than `count` of them;
  // keep(key, record) decides which are returned. Expired keys are skipped.
  KeyScan scan_keys(std::string_view after, std::string_view prefix, size_t count,
                    const std::function<bool(std::string_view key, const RecordView& rec)>& keep);
  uint64_t approx_size();

  RocksStorage& storage() { return storage_; }
  ObjectCache& cache() { return cache_; }
  void set_cache_on_write(bool on) { cache_on_write_.store(on, std::memory_order_relaxed); }
  WriteBatchStats write_batch_stats() const;
  // Thrown, inside a NonBlockingReads scope, by a read that missed the cache
  // and would have to go to disk. The caller has `key` loaded into the cache
  // elsewhere (load_into_cache) and runs the command again. Deliberately not a
  // std::exception, so generic error handlers let it through.
  struct WouldBlock {
    std::string key;
  };
  // While alive on a thread, such reads throw WouldBlock instead of reading
  // the disk. Only for commands without side effects, which can be rerun.
  class NonBlockingReads {
   public:
    NonBlockingReads();
    ~NonBlockingReads();
    NonBlockingReads(const NonBlockingReads&) = delete;
    NonBlockingReads& operator=(const NonBlockingReads&) = delete;

   private:
    bool previous_;
  };
  // Reads that threw WouldBlock.
  uint64_t deferred_reads() const { return deferred_reads_.load(std::memory_order_relaxed); }
  // Reads that missed the object cache but RocksDB answered from memory, on the I/O thread.
  uint64_t memory_reads() const { return memory_reads_.load(std::memory_order_relaxed); }
  // Puts a key that missed the cache into it, for a deferred read; blocks for
  // the key lock and the disk. The cache takes it even if its admission
  // filter has not seen the key before: a client is waiting for it. False if
  // it is not cached afterwards (missing, expired, too large for the cache).
  bool load_into_cache(std::string_view key);
  // The same for several keys, read with one RocksDB MultiGet; the result
  // says, per key, whether it is cached afterwards. The locks are taken
  // without waiting: keys whose lock is busy are loaded one by one after.
  std::vector<bool> load_many_into_cache(std::span<const std::string> keys);

  // Time a command waited for a key lock held by another connection: to write
  // the key, or to read it after a cache miss.
  const LatencyHistogram& lock_waits() const { return lock_waits_; }
  const LatencyHistogram& read_lock_waits() const { return read_lock_waits_; }
  // Time a write batch held its key locks (from the first lock to the commit).
  const LatencyHistogram& batch_hold() const { return batch_hold_; }
  void reset_latency() {
    lock_waits_.reset();
    read_lock_waits_.reset();
    batch_hold_.reset();
    storage_.reset_latency();
  }

  // ---- secondary indexes (see index_ops.cpp)
  IndexRegistry& indexes() { return *indexes_; }
  // Creates an index and starts filling it from the existing keys in the
  // background (scan = false: only later writes are indexed); returns an
  // error message or "".
  std::string create_index(IndexDef def, bool scan = true);
  bool drop_index(std::string_view name);
  IndexQueryResult query_index(const IndexQuery& q);

  // ---- search indexes, FT.* (see search_ops.cpp)
  SearchResult search(const IndexRegistry::Ptr& index, const SearchNode& query, const SearchOptions& opts);
  // The documents and entries of a search index (FT.INFO).
  void count_search_entries(const IndexRegistry::Ptr& index, uint64_t* docs, uint64_t* records);
  // The distinct values of a TAG field of a search index (FT.TAGVALS).
  std::vector<std::string> tag_values(const IndexRegistry::Ptr& index, size_t field);
  // Every key in a search index (FT.DROPINDEX DD).
  std::vector<std::string> search_keys(const IndexRegistry::Ptr& index);
  // Commits this thread's write batch, if any (before reading RocksDB directly).
  void commit_active_batch();

  // ---- WATCH and blocking commands
  // Told about every write to a key (WATCH, BLPOP...).
  KeyWatch& key_watch() { return *key_watch_; }
  std::weak_ptr<KeyWatch> key_watch_ref() const { return key_watch_; }
  // WATCH: adds key to w under the key's lock, so a write to it either came
  // before (and is not seen) or comes after and marks w.
  void watch_key(std::string_view key, WatchedKeys& w);

  // A MULTI/EXEC block or a Lua script: the stripes of `keys` are held by
  // this thread's write batch (one of its own if there is none) from here to
  // commit(); with `all`, the whole keyspace is (KeyLocks' world lock), and
  // the block may touch any key. No other client writes those keys, or reads
  // them from disk, in between. The block's writes reach RocksDB in one batch,
  // unless a command in it commits early (MSET, RENAME, FLUSHALL, KEYS,
  // SCAN, index queries...): that writes what is pending but keeps the locks.
  class ExecScope {
   public:
    // track_keys (with all): a Lua script, which names the keys of each of
    // its commands (note_keys), so the commit can keep only those locked.
    ExecScope(Database& db, std::span<const std::string_view> keys, bool all, bool track_keys = false);
    ~ExecScope();  // commits if commit() was not called; failures are logged
    ExecScope(const ExecScope&) = delete;
    ExecScope& operator=(const ExecScope&) = delete;
    // Commits the block and releases its locks; throws StorageError on failure.
    void commit();

   private:
    Database& db_;
    WriteBatchScope local_;
    PendingBatch* batch_;
    bool world_ = false;
    bool done_ = false;
  };
  // This thread has the whole keyspace (an ExecScope with `all`: a script, or EXEC).
  bool owns_all_keys() const { return locks_.world_owned(); }
  // A Lua script in an ExecScope with `track_keys` names the keys each of its
  // commands reads or writes (note_keys), or that one may touch any key
  // (note_any_key), before the command runs. At commit the scope then keeps
  // only those keys locked and lets other clients in while RocksDB writes;
  // after note_any_key it holds the whole keyspace until the write is done.
  // Both first wait for earlier scripts still committing those keys (any key).
  void note_keys(std::span<const std::string_view> keys);
  void note_any_key();

 private:
  // Unlocks a stripe on destruction; empty when a write batch keeps the stripe.
  class KeyGuard {
   public:
    KeyGuard() = default;
    KeyGuard(KeyLocks* locks, size_t stripe) : locks_(locks), stripe_(stripe) {}
    KeyGuard(KeyGuard&& o) noexcept : locks_(std::exchange(o.locks_, nullptr)), stripe_(o.stripe_) {}
    KeyGuard& operator=(KeyGuard&&) = delete;
    ~KeyGuard() {
      if (locks_) locks_->unlock(stripe_);
    }

   private:
    KeyLocks* locks_ = nullptr;
    size_t stripe_ = 0;
  };

  struct StringHash {
    using is_transparent = void;
    size_t operator()(std::string_view s) const { return hash_bytes(s); }
  };

  struct PendingBatch {
    Database* db = nullptr;
    RocksStorage::Batch batch;  // built at commit
    // Last write per meta key: the encoded record, or nullopt for a delete.
    std::unordered_map<std::string, std::optional<std::string>, StringHash, std::equal_to<>> writes;
    // Same for data keys, ordered so collection scans can merge it with RocksDB.
    std::map<std::string, std::optional<std::string>, std::less<>> data;
    // Same for index entries.
    std::map<std::string, std::optional<std::string>, std::less<>> index;
    // Applied after the writes above; they only ever cover dead versions.
    std::vector<std::pair<std::string, std::string>> range_deletes;
    std::bitset<KeyLocks::kStripes> held_mask;
    std::vector<size_t> held;
    std::chrono::steady_clock::time_point held_since;  // when `held` stopped being empty
    // Decoded elements of small collections touched by this batch, handed
    // from one transaction to the next and put in the object cache when the
    // batch commits (so lock-free readers only see committed elements).
    // nullopt: the cached elements are older than the pending writes.
    struct CachedElements {
      uint64_t version;
      ElementSet elems;
    };
    std::unordered_map<std::string, std::optional<CachedElements>, StringHash, std::equal_to<>> elements;
    // With the change feed on: the type of each collection version whose
    // elements the batch writes, by data prefix (for the WAL note).
    std::unordered_map<std::string, ValueType, StringHash, std::equal_to<>> element_types;
    size_t batch_count = 0;  // writes staged, before coalescing
    size_t bytes = 0;        // approximate size of the staged writes
    bool failed = false;
    // Inside an ExecScope: commits keep the stripes (they are the block's),
    // and the cache items of what they wrote stay pending until it ends.
    bool exec = false;
    std::vector<std::string> exec_written;
    // An ExecScope with `all` that tracks the keys it uses (see track_keys):
    // their stripes, to keep while it commits.
    bool track_keys = false;
    std::vector<size_t> tracked;

    // Undo log of the transaction in progress (data entries only).
    struct Undo {
      bool index;  // which map: index or data
      std::string key;
      std::optional<std::optional<std::string>> before;  // nullopt: absent
    };
    bool in_txn = false;
    std::vector<Undo> undo;
    size_t range_mark = 0;
  };

  // Locks the keys of a transaction and runs it; rolls back if it throws.
  class TxnRun {
   public:
    TxnRun(Database& db, std::span<const std::string_view> keys, bool multi);
    ~TxnRun();
    KeyTxns& txns() { return txns_; }

    template <class F>
    decltype(auto) execute(WriteBatchScope& local, F&& f) {
      if constexpr (std::is_void_v<decltype(f())>) {
        f();
        finish();
        local.commit();
      } else {
        auto result = f();
        finish();
        local.commit();
        return result;
      }
    }

   private:
    void finish();

    Database& db_;
    PendingBatch* batch_;
    KeyTxns txns_;
    bool done_ = false;
  };

  static constexpr size_t kMaxBatchWrites = 1024;
  static constexpr size_t kMaxBatchBytes = 4 << 20;
  static constexpr uint64_t kVersionBlock = 1 << 20;
  // Collections up to this many elements are cached whole (if they fit in a cache object).
  static constexpr size_t kMaxCachedElements = 128;

  PendingBatch* active_batch() const;
  // Locks a key for a command that writes it. Inside a batch the lock is held
  // until the batch commits. `reads` says the command reads the key first: if
  // it is not cached, the batch commits before taking the lock, so the keys of
  // earlier commands are not held while the disk is read.
  KeyGuard lock_key(std::string_view key, bool reads = false);
  // Locks a key only while a read that missed the cache loads it. It does not
  // join the batch's locks, and commits the batch first, for the same reason.
  KeyGuard lock_for_read(std::string_view key);
  // Locks a stripe for the thread's batch (blocking), unless it already holds it.
  void hold_stripe(PendingBatch& b, size_t stripe);
  // An ExecScope of this thread holds the locks a multi-key command needs.
  bool exec_holds_locks() const;
  void commit_batch(PendingBatch& b);
  void publish_elements(PendingBatch& b);
  // Cache tail holding these elements, or TooBig.
  std::string element_tail(std::string_view key, const ElementSet& elems) const;
  void release_locks(PendingBatch& b);
  void wait_lock(size_t stripe, bool read = false);  // blocks for the stripe, timing the wait
  void note_held(PendingBatch& b, size_t stripe);
  void stage(PendingBatch& b, std::string_view key, std::optional<std::string> encoded);
  void stage_data(PendingBatch& b, std::string key, std::optional<std::string> value);
  void stage_index(PendingBatch& b, std::string key, std::optional<std::string> value);
  // Writes an index entry through the thread's batch, or directly.
  void write_index(std::string key, std::optional<std::string> value);
  // Reads an index entry, including this thread's pending writes.
  std::optional<std::string> read_index(std::string_view key);
  // The tuples of `key` (a hash with meta m) in an index, read from RocksDB
  // plus this thread's pending writes, or from a snapshot.
  IndexDef::Tuples stored_tuples(const IndexDef& def, std::string_view key, const CollectionMeta& m,
                                 const RocksStorage::Snapshot* snapshot = nullptr);
  // The meta of a live hash, from RocksDB (pending-aware) or a snapshot.
  std::optional<CollectionMeta> stored_hash_meta(std::string_view key, const RocksStorage::Snapshot* snapshot);
  // Deletes the index entries of a hash that is being deleted or replaced
  // (into `batch` if given, else through the thread's batch or directly).
  void drop_index_entries(std::string_view key, const CollectionMeta& m, RocksStorage::Batch* batch = nullptr);
  // Rewrites the entries of a hash whose TTL changed.
  void refresh_index_entries(std::string_view key, const CollectionMeta& m, int64_t expire_at_ms);
  // The owner hash of a UNIQUE entry value, if that entry is still current.
  std::optional<std::string> live_unique_owner(const IndexDef& def, std::string_view value);

  // ---- JSON indexes (index_ops.cpp)
  // The JSON indexes over a key and, for a record, the tuples each wants
  // (none: the record is not indexed there) and its TTL. Without tuples:
  // nothing is indexed (the key is gone).
  struct JsonEntries {
    std::vector<IndexRegistry::Ptr> indexes;
    std::vector<IndexDef::Tuples> tuples;
    int64_t expire_at_ms = 0;
    JsonEntries gone() const { return JsonEntries{indexes, {}, 0}; }
  };
  JsonEntries json_entries(std::string_view key, const std::optional<Record>& rec);
  // The index writes that take `key` from `before` to `after` (the same
  // indexes); nullopt values are deletes. Throws IndexConstraintError when a
  // UNIQUE value belongs to a live key other than `key` and `also_mine`.
  // Callers hold the locks of the UNIQUE indexes (lock_unique).
  std::vector<std::pair<std::string, std::optional<std::string>>> json_entry_changes(
      std::string_view key, const JsonEntries& before, const JsonEntries& after, std::string_view also_mine = {});
  std::vector<std::unique_lock<std::mutex>> lock_unique(const std::vector<IndexRegistry::Ptr>& indexes);
  // The document a key holds at a snapshot (none: missing, expired, not JSON).
  std::optional<Json> stored_json(std::string_view key, const RocksStorage::Snapshot* snapshot);
  void run_backfill(IndexRegistry::Ptr index);
  void start_backfill(IndexRegistry::Ptr index);
  // Pending write of the thread's batch: nullptr if none, else the encoded
  // record or nullopt for a delete.
  const std::optional<std::string>* pending(std::string_view key) const;
  void write_storage(std::string_view key, std::string encoded);
  void delete_storage(std::string_view key);
  // Removes all elements of one collection version.
  void drop_elements(std::string_view key, const CollectionMeta& m);
  static std::optional<CollectionMeta> collection_meta(const std::optional<Record>& rec);
  uint64_t next_version();

  // With meta_only, a cached collection comes without its elements.
  std::optional<Record> load_locked(std::string_view key, bool meta_only = false);
  bool exists_locked(std::string_view key);
  void store_locked(std::string_view key, Record& rec, bool new_cas);
  void erase_locked(std::string_view key);
  // Puts a record just written in the cache. staged: the write waits in this
  // thread's batch, so the item stays pending (lock-free readers do not see
  // it) until the batch commits; inside an ExecScope every write does, until
  // the block ends.
  void cache_store(std::string_view key, const RecordView& rec, bool staged);
  uint64_t next_cas() { return cas_.fetch_add(1, std::memory_order_relaxed) + 1; }

  RocksStorage& storage_;
  ObjectCache& cache_;
  std::atomic<bool> cache_on_write_;
  std::atomic<bool> read_only_{false};
  const bool change_notes_;  // the WAL is kept: batches carry a change note
  KeyLocks locks_;
  std::atomic<uint64_t> cas_;
  // Collection versions are never reused, not even across restarts: a mark
  // persisted in the System column family is always ahead of the last one used.
  std::atomic<uint64_t> version_{0};
  std::atomic<uint64_t> version_limit_{0};
  std::mutex version_mu_;
  std::unique_ptr<IndexRegistry> indexes_;
  std::shared_ptr<KeyWatch> key_watch_ = std::make_shared<KeyWatch>();
  std::mutex backfill_mu_;
  std::vector<std::thread> backfills_;
  std::atomic<bool> stopping_{false};
  std::atomic<uint64_t> batch_commits_{0};
  std::atomic<uint64_t> batch_writes_{0};
  std::atomic<uint64_t> batch_conflicts_{0};
  LatencyHistogram lock_waits_;
  LatencyHistogram read_lock_waits_;
  std::atomic<uint64_t> deferred_reads_{0};
  std::atomic<uint64_t> memory_reads_{0};
  LatencyHistogram batch_hold_;
};

}  // namespace datadam
