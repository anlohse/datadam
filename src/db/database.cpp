#include "db/database.h"

#include <algorithm>
#include <array>
#include <format>

#include "db/change_feed.h"
#include "storage/data_keys.h"
#include "util/clock.h"
#include "util/log.h"
#include "util/spin.h"
#include "util/strings.h"

namespace datadam {

namespace {
// The write batch of the current thread. Scopes never span a coroutine
// suspension, so a thread runs at most one at a time.
thread_local void* tl_batch = nullptr;

constexpr std::string_view kVersionMarkKey = "collection-version";
}  // namespace

Database::Database(RocksStorage& storage, ObjectCache& cache, bool cache_on_write, bool resume_backfills)
    : storage_(storage),
      cache_(cache),
      cache_on_write_(cache_on_write),
      change_notes_(storage.wal_retained()),
      // Microsecond start point keeps cas values increasing across restarts.
      cas_(static_cast<uint64_t>(unix_time_ms()) * 1000) {
  std::string mark;
  if (storage_.get(Cf::System, kVersionMarkKey, &mark) && mark.size() == 8) {
    version_ = read_be64(mark);
    version_limit_ = version_.load();
  }
  indexes_ = std::make_unique<IndexRegistry>(storage_);
  storage_.set_index_resolver(indexes_.get());
  if (!resume_backfills) return;
  for (auto& index : indexes_->all()) {
    if (index->state == IndexState::Building) start_backfill(index);  // interrupted by a restart
  }
}

// ---- replication

namespace {

// Rebuilds a batch from the primary with this database's column families, and
// notes which keys it touches.
class ReplicaBatch : public WalVisitor {
 public:
  explicit ReplicaBatch(RocksStorage::Batch& out) : out_(out) {}

  bool begin(uint64_t, uint64_t) override { return true; }
  void end() override {}
  void put(Cf cf, std::string_view key, std::string_view value) override {
    if (skip(cf, key)) return;
    out_.put(cf, key, value);
    touched(cf, key);
  }
  void del(Cf cf, std::string_view key) override {
    if (skip(cf, key)) return;
    out_.del(cf, key);
    touched(cf, key);
  }
  void delete_range(Cf cf, std::string_view begin, std::string_view end) override {
    out_.delete_range(cf, begin, end);
    if (cf == Cf::Meta) everything = true;  // FLUSHALL
    touched(cf, begin);                     // a collection version: its key leads both bounds
  }
  void log_data(std::string_view blob) override { out_.put_log_data(blob); }

  std::vector<std::string> keys;
  bool everything = false;
  bool index_defs = false;

 private:
  static bool skip(Cf cf, std::string_view key) { return cf == Cf::System && key.starts_with("repl:"); }

  void touched(Cf cf, std::string_view key) {
    switch (cf) {
      case Cf::Meta: keys.emplace_back(key); break;
      case Cf::Data: {
        std::string_view user_key, sub;
        uint64_t version = 0;
        if (decode_data_key(key, &user_key, &version, &sub)) keys.emplace_back(user_key);
        break;
      }
      case Cf::System: index_defs = index_defs || key.starts_with("index:"); break;
      case Cf::Index: break;
    }
  }

  RocksStorage::Batch& out_;
};

}  // namespace

void Database::apply_replicated(std::string_view data, const RocksStorage::CfIds& ids,
                                const std::vector<std::pair<std::string, std::string>>& system_puts) {
  auto batch = storage_.batch();
  ReplicaBatch rebuilt(batch);
  storage_.decode_batch(data, ids, rebuilt);
  for (const auto& [k, v] : system_puts) batch.put(Cf::System, k, v);
  {
    // Readers that miss the cache hold the key lock while they load it, so
    // none can put a value older than this write back into the cache.
    auto guard = rebuilt.everything ? locks_.lock_all() : locks_.lock_keys(rebuilt.keys);
    if (rebuilt.everything) key_watch_->touched_all();
    for (const auto& k : rebuilt.keys) key_watch_->touched(k);
    storage_.write(batch);
    if (rebuilt.everything) cache_.clear();
    for (const auto& k : rebuilt.keys) cache_.erase(k);
  }
  if (rebuilt.index_defs) indexes_->reload();
}

void Database::clear_for_full_sync() {
  auto guard = locks_.lock_all();
  key_watch_->touched_all();
  storage_.delete_all();
  auto batch = storage_.batch();
  {
    auto c = storage_.cursor(Cf::System, "", std::string(64, '\xff'), false);
    for (c->seek_first(); c->valid(); c->next()) batch.del(Cf::System, c->key());
    c->check_status();
  }
  if (!batch.empty()) storage_.write(batch);
  cache_.clear();
  indexes_->pause_gc(true);  // until finish_full_sync: definitions come last
  indexes_->reload();
}

void Database::finish_full_sync() {
  indexes_->reload();
  indexes_->pause_gc(false);
}

void Database::promote() {
  std::string mark;
  uint64_t v = storage_.get(Cf::System, kVersionMarkKey, &mark) && mark.size() == 8 ? read_be64(mark) : 0;
  {
    std::lock_guard lock(version_mu_);
    // The primary may have used versions up to its mark: start past it.
    version_ = std::max(version_.load(), v);
    version_limit_ = std::max(version_limit_.load(), v);
  }
  indexes_->reload();
  indexes_->pause_gc(false);  // a full sync that never finished
  set_read_only(false);
  for (auto& index : indexes_->all()) {
    if (index->state == IndexState::Building) start_backfill(index);
  }
}

Database::~Database() {
  stopping_ = true;
  {
    std::lock_guard lock(backfill_mu_);
    for (auto& t : backfills_) {
      if (t.joinable()) t.join();
    }
  }
  storage_.set_index_resolver(nullptr);
}

// ---- write batches

Database::WriteBatchScope::WriteBatchScope(Database& db, bool enabled) : db_(db) {
  if (!enabled || tl_batch) return;
  batch_ = std::make_unique<PendingBatch>();
  batch_->db = &db;
  batch_->batch = db.storage_.batch();
  tl_batch = batch_.get();
}

Database::WriteBatchScope::~WriteBatchScope() {
  if (!batch_) return;
  try {
    db_.commit_batch(*batch_);
  } catch (const std::exception& e) {
    log_error("write batch dropped: {}", e.what());
  }
  tl_batch = nullptr;
}

void Database::WriteBatchScope::commit() {
  if (!batch_) return;
  db_.commit_batch(*batch_);
  if (batch_->failed) {
    batch_->failed = false;
    throw StorageError("write batch failed; replies discarded");
  }
}

Database::PendingBatch* Database::active_batch() const {
  auto* b = static_cast<PendingBatch*>(tl_batch);
  return b && b->db == this ? b : nullptr;
}

void Database::wait_lock(size_t stripe, bool read) {
  LatencyHistogram::Timer t(read ? read_lock_waits_ : lock_waits_);
  locks_.lock(stripe);
}

void Database::note_held(PendingBatch& b, size_t s) {
  if (b.held.empty()) b.held_since = std::chrono::steady_clock::now();
  b.held_mask.set(s);
  b.held.push_back(s);
}

Database::KeyGuard Database::lock_key(std::string_view key, bool reads) {
  if (locks_.world_owned()) return {};  // a script: every key is this thread's
  size_t s = locks_.stripe_of(key);
  PendingBatch* b = active_batch();
  if (!b) {
    if (!locks_.spin_try_lock(s)) wait_lock(s);
    return KeyGuard(&locks_, s);
  }
  if (b->held_mask.test(s)) return {};
  bool to_disk = reads && !b->held.empty() && !b->in_txn && !cache_.lookup(key, [](const RecordView&) {}, false);
  if (to_disk || b->batch_count >= kMaxBatchWrites || b->bytes >= kMaxBatchBytes) commit_batch(*b);
  if (!locks_.spin_try_lock(s)) {
    // Never wait while holding stripes: commit (releasing them), then wait.
    if (!b->held.empty()) {
      batch_conflicts_.fetch_add(1, std::memory_order_relaxed);
      commit_batch(*b);
    }
    wait_lock(s);
  }
  note_held(*b, s);
  return {};
}

namespace {
thread_local bool tl_nonblocking_reads = false;
}  // namespace

Database::NonBlockingReads::NonBlockingReads() : previous_(tl_nonblocking_reads) { tl_nonblocking_reads = true; }
Database::NonBlockingReads::~NonBlockingReads() { tl_nonblocking_reads = previous_; }

Database::KeyGuard Database::lock_for_read(std::string_view key) {
  if (locks_.world_owned()) return {};  // a script: every key is this thread's
  size_t s = locks_.stripe_of(key);
  PendingBatch* b = active_batch();
  if (b && b->held_mask.test(s)) return {};  // the batch's own pending write is read
  if (tl_nonblocking_reads && !cache_.pending(key)) {
    // Not cached. If RocksDB can answer from memory (a memtable, the block
    // cache, or a filter that says the key is not there), the read goes on
    // here; otherwise the key is loaded by another thread and the command
    // rerun. (Pending: another client is committing it right now; wait for
    // its lock.)
    if (storage_.probe(Cf::Meta, key) == RocksStorage::Probe::NeedsIo) {
      cache_.defer_miss(key);
      deferred_reads_.fetch_add(1, std::memory_order_relaxed);
      throw WouldBlock{std::string(key)};
    }
    memory_reads_.fetch_add(1, std::memory_order_relaxed);
  }
  bool holding = b && !b->held.empty();
  if (holding && b->in_txn) return lock_key(key);
  if (!locks_.spin_try_lock(s)) {
    if (holding) commit_batch(*b);  // never wait while holding stripes
    holding = false;
    wait_lock(s, true);
  }
  KeyGuard guard(&locks_, s);
  if (holding) commit_batch(*b);  // so the batch's keys are free during the disk read
  return guard;
}

void Database::hold_stripe(PendingBatch& b, size_t s) {
  if (b.held_mask.test(s) || locks_.world_owned()) return;
  if (!locks_.spin_try_lock(s)) wait_lock(s);
  note_held(b, s);
}

void Database::release_locks(PendingBatch& b) {
  if (!b.held.empty()) {
    auto d = std::chrono::steady_clock::now() - b.held_since;
    batch_hold_.record(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(d).count()));
  }
  for (auto it = b.held.rbegin(); it != b.held.rend(); ++it) locks_.unlock(*it);
  b.held.clear();
  b.held_mask.reset();
}

void Database::commit_batch(PendingBatch& b) {
  if (b.batch_count > 0 && read_only()) {
    // A replica: only reads ran (refused commands never get here), and what
    // they staged (an expired key's removal) is the primary's to write.
    for (const auto& w : b.writes) {
      if (w.second) cache_.erase(w.first);
    }
    b.batch.clear();
    b.writes.clear();
    b.data.clear();
    b.index.clear();
    b.range_deletes.clear();
    b.element_types.clear();
    b.batch_count = 0;
    b.bytes = 0;
  }
  if (b.batch_count > 0) {
    try {
      // Only the last write of each key reaches RocksDB: a pipeline of INCRs
      // on one counter becomes a single put.
      b.batch.clear();
      if (change_notes_) {
        std::string note = change_note(unix_time_ms());
        for (const auto& [prefix, type] : b.element_types) add_change_note_type(note, prefix, type);
        b.batch.put_log_data(note);
      }
      for (const auto& [key, value] : b.writes) {
        if (value) b.batch.put(Cf::Meta, key, *value);
        else b.batch.del(Cf::Meta, key);
      }
      for (const auto& [key, value] : b.data) {
        if (value) b.batch.put(Cf::Data, key, *value);
        else b.batch.del(Cf::Data, key);
      }
      for (const auto& [key, value] : b.index) {
        if (value) b.batch.put(Cf::Index, key, *value);
        else b.batch.del(Cf::Index, key);
      }
      for (const auto& [begin, end] : b.range_deletes) b.batch.delete_range(Cf::Data, begin, end);
      storage_.write(b.batch);
      // Committed: lock-free readers may see the cached records now (a block
      // only lets them when it ends).
      for (const auto& [key, value] : b.writes) {
        if (!value) continue;
        if (b.exec) b.exec_written.push_back(key);
        else cache_.confirm(key);
      }
    } catch (...) {
      // The cache already holds these writes (pending); drop them while the
      // stripes are still held, so nobody reads them.
      for (const auto& w : b.writes) cache_.erase(w.first);
      for (const auto& e : b.elements) cache_.erase(e.first);
      for (const auto& k : b.exec_written) cache_.erase(k);
      b.exec_written.clear();
      b.elements.clear();
      b.failed = true;
      b.batch.clear();
      b.writes.clear();
      b.data.clear();
      b.index.clear();
      b.range_deletes.clear();
      b.element_types.clear();
      b.batch_count = 0;
      b.bytes = 0;
      release_locks(b);
      throw;
    }
    batch_commits_.fetch_add(1, std::memory_order_relaxed);
    batch_writes_.fetch_add(b.batch_count, std::memory_order_relaxed);
    b.batch.clear();
    b.writes.clear();
    b.data.clear();
    b.index.clear();
    b.range_deletes.clear();
    b.element_types.clear();
    b.batch_count = 0;
    b.bytes = 0;
  }
  publish_elements(b);  // while the keys are still locked
  if (b.exec) return;    // a MULTI/EXEC block keeps its locks until it ends
  for (const auto& k : b.exec_written) cache_.confirm(k);
  b.exec_written.clear();
  release_locks(b);
}

std::string Database::element_tail(std::string_view key, const ElementSet& elems) const {
  if (elems.size() <= kMaxCachedElements) {
    std::string tail(1, static_cast<char>(ElementCache::Complete));
    tail += elems.encode();
    if (cache_.cacheable(key.size(), kCollectionMetaSize + tail.size())) return tail;
  }
  return std::string(1, static_cast<char>(ElementCache::TooBig));
}

void Database::publish_elements(PendingBatch& b) {
  for (auto& [key, cached] : b.elements) {
    if (!cached) {
      cache_.erase(key);
      continue;
    }
    // The cached record has the latest meta (an EXPIRE may have changed it);
    // the elements go with it if it is still the same collection.
    std::optional<Record> rec;
    cache_.lookup(key, [&](RecordView v) {
      v.value = v.value.substr(0, kCollectionMetaSize);  // the old elements are not needed
      rec = Record::from(v);
    }, false, true);
    CollectionMeta m;
    if (!rec || !is_collection(rec->type) || !decode_collection_meta(rec->value, &m) || m.version != cached->version) {
      continue;
    }
    rec->value += element_tail(key, cached->elems);
    cache_.insert(key, rec->view(), false, b.exec);
    if (b.exec) b.exec_written.push_back(key);
  }
  b.elements.clear();
}

void Database::commit_active_batch() {
  if (PendingBatch* b = active_batch()) commit_batch(*b);
}

const std::optional<std::string>* Database::pending(std::string_view key) const {
  PendingBatch* b = active_batch();
  if (!b || b->writes.empty()) return nullptr;
  auto it = b->writes.find(key);
  return it == b->writes.end() ? nullptr : &it->second;
}

void Database::stage(PendingBatch& b, std::string_view key, std::optional<std::string> encoded) {
  b.batch_count++;
  size_t size = encoded ? encoded->size() : 0;
  auto it = b.writes.find(key);
  if (it == b.writes.end()) {
    b.bytes += key.size() + size;
    b.writes.emplace(std::string(key), std::move(encoded));
  } else {
    b.bytes = b.bytes - (it->second ? it->second->size() : 0) + size;
    it->second = std::move(encoded);
  }
}

void Database::stage_data(PendingBatch& b, std::string key, std::optional<std::string> value) {
  b.batch_count++;
  b.bytes += key.size() + (value ? value->size() : 0);
  auto it = b.data.find(key);
  if (b.in_txn) {
    b.undo.push_back({false, key, it == b.data.end() ? std::nullopt : std::optional(it->second)});
  }
  if (it == b.data.end()) b.data.emplace(std::move(key), std::move(value));
  else it->second = std::move(value);
}

void Database::stage_index(PendingBatch& b, std::string key, std::optional<std::string> value) {
  b.batch_count++;
  b.bytes += key.size() + (value ? value->size() : 0);
  auto it = b.index.find(key);
  if (b.in_txn) {
    b.undo.push_back({true, key, it == b.index.end() ? std::nullopt : std::optional(it->second)});
  }
  if (it == b.index.end()) b.index.emplace(std::move(key), std::move(value));
  else it->second = std::move(value);
}

void Database::write_storage(std::string_view key, std::string encoded) {
  key_watch_->touched(key);
  if (PendingBatch* b = active_batch()) return stage(*b, key, std::move(encoded));
  if (read_only()) return;
  if (!change_notes_) return storage_.put(key, encoded);
  auto batch = storage_.batch();  // a write of its own, with its change note
  batch.put(key, encoded);
  batch.put_log_data(change_note(unix_time_ms()));
  storage_.write(batch);
}

void Database::delete_storage(std::string_view key) {
  key_watch_->touched(key);
  if (PendingBatch* b = active_batch()) return stage(*b, key, std::nullopt);
  if (read_only()) return;
  if (!change_notes_) return storage_.del(key);
  auto batch = storage_.batch();
  batch.del(key);
  batch.put_log_data(change_note(unix_time_ms()));
  storage_.write(batch);
}

void Database::drop_elements(std::string_view key, const CollectionMeta& m) {
  std::string begin = data_prefix(key, m.version), end = data_prefix(key, m.version + 1);
  if (PendingBatch* b = active_batch()) {
    b->batch_count++;
    b->range_deletes.emplace_back(std::move(begin), std::move(end));
    return;
  }
  if (read_only()) return;
  auto batch = storage_.batch();
  batch.delete_range(Cf::Data, begin, end);
  storage_.write(batch);
}

std::optional<CollectionMeta> Database::collection_meta(const std::optional<Record>& rec) {
  CollectionMeta m;
  if (!rec || !is_collection(rec->type) || !decode_collection_meta(rec->value, &m)) return std::nullopt;
  return m;
}

uint64_t Database::next_version() {
  uint64_t v = version_.fetch_add(1, std::memory_order_relaxed) + 1;
  if (v > version_limit_.load(std::memory_order_acquire)) {
    std::lock_guard lock(version_mu_);
    uint64_t limit = version_limit_.load(std::memory_order_relaxed);
    if (v > limit) {
      // Reached RocksDB before any data entry that uses these versions (the WAL keeps write order).
      uint64_t mark = std::max(limit, v) + kVersionBlock;
      storage_.put(Cf::System, kVersionMarkKey, encode_u64(mark));
      version_limit_.store(mark, std::memory_order_release);
    }
  }
  return v;
}

WriteBatchStats Database::write_batch_stats() const {
  return {batch_commits_.load(std::memory_order_relaxed), batch_writes_.load(std::memory_order_relaxed),
          batch_conflicts_.load(std::memory_order_relaxed)};
}

// ---- MULTI/EXEC and WATCH

void Database::note_keys(std::span<const std::string_view> keys) {
  PendingBatch* b = active_batch();
  if (!b || !b->track_keys) return;
  for (auto k : keys) {
    size_t s = locks_.stripe_of(k);
    locks_.wait_committed(s);
    b->tracked.push_back(s);
  }
}

void Database::note_any_key() {
  PendingBatch* b = active_batch();
  if (!b || !b->track_keys) return;
  b->track_keys = false;
  locks_.wait_all_committed();
}

bool Database::exec_holds_locks() const {
  PendingBatch* b = active_batch();
  return (b && b->exec) || locks_.world_owned();
}

void Database::watch_key(std::string_view key, WatchedKeys& w) {
  KeyGuard lock = lock_key(key);
  key_watch_->watch(key, w.watcher());
  w.add(std::string(key));
}

Database::ExecScope::ExecScope(Database& db, std::span<const std::string_view> keys, bool all, bool track_keys)
    : db_(db), local_(db), batch_(db.active_batch()) {
  // Like a multi-key transaction: holding nothing, take every stripe in
  // ascending order.
  db.commit_batch(*batch_);
  if (all) {
    db.locks_.lock_world();  // holding no stripe now: waits for every other holder
    world_ = true;
    batch_->track_keys = track_keys;
    batch_->tracked.clear();
    if (!track_keys) db.locks_.wait_all_committed();  // it may touch any key
  } else {
    std::vector<size_t> stripes;
    for (auto k : keys) stripes.push_back(db.locks_.stripe_of(k));
    std::sort(stripes.begin(), stripes.end());
    for (size_t s : stripes) db.hold_stripe(*batch_, s);
  }
  batch_->exec = true;
}

void Database::ExecScope::commit() {
  if (done_) return;
  done_ = true;
  batch_->exec = false;
  if (world_ && batch_->track_keys) {
    // Keep the keys the block used, and let the others in during the write.
    std::vector<size_t>& s = batch_->tracked;
    std::sort(s.begin(), s.end());
    s.erase(std::unique(s.begin(), s.end()), s.end());
    db_.locks_.downgrade_world(s);
    for (size_t stripe : s) db_.note_held(*batch_, stripe);
    batch_->track_keys = false;
    s.clear();
    world_ = false;
  }
  struct Release {  // the world lock goes after the commit, also if it throws
    Database& db;
    bool world;
    ~Release() {
      if (world) db.locks_.unlock_world();
    }
  } release{db_, world_};
  db_.commit_batch(*batch_);
}

Database::ExecScope::~ExecScope() {
  if (done_) return;
  try {
    commit();
  } catch (const std::exception& e) {
    log_error("MULTI/EXEC block dropped: {}", e.what());
  }
}

// ---- transactions

Database::TxnRun::TxnRun(Database& db, std::span<const std::string_view> keys, bool multi)
    : db_(db), batch_(db.active_batch()) {
  if (!batch_) throw std::logic_error("transaction without a write batch");
  if (multi) {
    // Several stripes are taken in ascending order, blocking; a batch must
    // hold none at that point, or it could deadlock with another one.
    db.commit_batch(*batch_);
    std::vector<size_t> stripes;
    for (auto k : keys) stripes.push_back(db.locks_.stripe_of(k));
    std::sort(stripes.begin(), stripes.end());
    for (size_t s : stripes) db.hold_stripe(*batch_, s);
  } else {
    db.lock_key(keys[0], true);  // held by the batch until it commits
  }
  batch_->in_txn = true;
  batch_->undo.clear();
  batch_->range_mark = batch_->range_deletes.size();
  for (auto k : keys) {
    size_t found = txns_.txns_.size();
    for (size_t i = 0; i < txns_.txns_.size(); ++i) {
      if (txns_.txns_[i]->key() == k) found = i;
    }
    if (found == txns_.txns_.size()) {
      txns_.txns_.push_back(std::unique_ptr<KeyTxn>(new KeyTxn(db, *batch_, std::string(k))));
    }
    txns_.index_.push_back(found);
  }
}

Database::TxnRun::~TxnRun() {
  if (done_) return;
  // f threw: put the batch back as it was before the transaction.
  PendingBatch& b = *batch_;
  for (auto it = b.undo.rbegin(); it != b.undo.rend(); ++it) {
    auto& map = it->index ? b.index : b.data;
    if (it->before) map[it->key] = std::move(*it->before);
    else map.erase(it->key);
  }
  b.range_deletes.resize(b.range_mark);
  b.undo.clear();
  b.in_txn = false;
  // Elements these transactions held in memory may not match the batch any more.
  for (auto& t : txns_.txns_) b.elements[t->key_] = std::nullopt;
}

void Database::TxnRun::finish() {
  // Writers of a UNIQUE index take its lock (in id order, so two transactions
  // can never wait for each other) and commit before releasing it: the next
  // one then sees their entries in RocksDB. Nothing waits for a key lock while
  // holding one of these, so they cannot deadlock with the key locks either.
  std::vector<IndexRegistry::Ptr> unique;
  for (auto& t : txns_.txns_) t->unique_indexes(&unique);
  std::sort(unique.begin(), unique.end(), [](const auto& a, const auto& b) { return a->def.id < b->def.id; });
  unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
  std::vector<std::unique_lock<std::mutex>> locks;
  for (auto& index : unique) locks.emplace_back(index->unique_mu);

  for (auto& t : txns_.txns_) t->prepare_indexes();  // may throw: nothing is written yet
  for (auto& t : txns_.txns_) t->apply_indexes();
  for (auto& t : txns_.txns_) t->finish();
  batch_->in_txn = false;
  batch_->undo.clear();
  done_ = true;
  if (!locks.empty()) db_.commit_batch(*batch_);
}

Database::KeyTxn::KeyTxn(Database& db, PendingBatch& batch, std::string key)
    : db_(db), batch_(&batch), key_(std::move(key)) {
  // Elements an earlier transaction of this batch left are newer than the cache's.
  auto it = batch.elements.find(key_);
  bool in_batch = it != batch.elements.end();
  rec_ = db_.load_locked(key_, in_batch);
  had_key_ = rec_.has_value();
  indexes_ = db_.indexes_->matching(key_);
  init_collection(!in_batch);
  if (in_batch && rec_ && is_collection(rec_->type)) {
    if (it->second && it->second->version == meta_.version) {
      elems_ = std::move(it->second->elems);
      complete_ = true;
      from_batch_ = true;
    } else {
      elems_.clear();
      complete_ = false;
    }
    too_big_ = false;
  }
  // A small collection not cached whole yet: read it all once, so this and
  // the next commands work from memory.
  if (rec_ && is_collection(rec_->type) && !complete_ && !too_big_ && meta_.size <= kMaxCachedElements &&
      db_.cache_.cacheable(key_.size(), kCollectionMetaSize + 1)) {
    ElementSet all;
    scan("", "", false, [&](std::string_view sub, std::string_view value) {
      all.append(sub, value);
      return true;
    });
    elems_ = std::move(all);
    complete_ = true;
    populate_ = true;
  }
}

Database::KeyTxn::KeyTxn(Database& db, std::string key, Record cached)
    : db_(db), batch_(nullptr), key_(std::move(key)), rec_(std::move(cached)) {
  had_key_ = true;
  init_collection();
}

void Database::KeyTxn::init_collection(bool decode_elements) {
  auto m = collection_meta(rec_);
  if (!m) return;
  meta_ = *m;
  stored_meta_ = *m;
  prefix_ = data_prefix(key_, meta_.version);
  std::string_view tail = std::string_view(rec_->value).substr(kCollectionMetaSize);
  ElementCache state = decode_elements ? element_cache_state(tail) : ElementCache::Unknown;
  too_big_ = state == ElementCache::TooBig;
  if (state == ElementCache::Complete) {
    // The set reads the elements in place, in the buffer the record came in.
    std::string buffer = std::move(rec_->value);
    rec_->value = buffer.substr(0, kCollectionMetaSize);
    complete_ = elems_.load(std::move(buffer), kCollectionMetaSize + 1);
  }
  rec_->value.resize(kCollectionMetaSize);
}

Database::PendingBatch& Database::KeyTxn::batch() const {
  if (!batch_) throw std::logic_error("write or uncached read in a read-only collection peek");
  return *batch_;
}

std::string Database::KeyTxn::partial_cache_tail() const {
  if (too_big_ && meta_.size > kMaxCachedElements / 2) return std::string(1, static_cast<char>(ElementCache::TooBig));
  return {};  // unknown: a later command reads it all again if it is small
}

std::string Database::KeyTxn::data_key(std::string_view sub) const {
  std::string k;
  k.reserve(prefix_.size() + sub.size());
  k.append(prefix_);
  k.append(sub);
  return k;
}

void Database::KeyTxn::drop_elements(const CollectionMeta& m) {
  batch().batch_count++;
  batch().range_deletes.emplace_back(data_prefix(key_, m.version), data_prefix(key_, m.version + 1));
}

void Database::KeyTxn::create(ValueType t) {
  if (rec_ && rec_->type == t) return;
  reset(t);
}

void Database::KeyTxn::reset(ValueType t) {
  capture();
  if (auto m = collection_meta(rec_)) drop_elements(*m);
  rec_.emplace();
  rec_->type = t;
  meta_ = CollectionMeta{};
  meta_.version = db_.next_version();
  prefix_ = data_prefix(key_, meta_.version);
  removed_ = false;
  elems_.clear();
  complete_ = true;  // a new collection is empty: fully known
  too_big_ = false;
  elements_changed_ = true;
}

void Database::KeyTxn::remove() {
  capture();
  if (auto m = collection_meta(rec_)) drop_elements(*m);
  rec_.reset();
  meta_ = CollectionMeta{};
  removed_ = true;
  elems_.clear();
  complete_ = false;
}

std::optional<std::string> Database::KeyTxn::get(std::string_view sub) {
  if (!rec_ || !is_collection(rec_->type)) return std::nullopt;
  if (complete_) {
    auto v = elems_.get(sub);
    if (!v) return std::nullopt;
    return std::string(*v);
  }
  std::string k = data_key(sub);
  if (auto it = batch().data.find(k); it != batch().data.end()) return it->second;
  std::string value;
  if (!db_.storage_.get(Cf::Data, k, &value)) return std::nullopt;
  return value;
}

void Database::KeyTxn::note_type() {
  if (!db_.change_notes_ || meta_.version == noted_version_) return;
  noted_version_ = meta_.version;
  batch().element_types.try_emplace(data_prefix(key_, meta_.version), rec_->type);
}

void Database::KeyTxn::put(std::string_view sub, std::string_view value) {
  db_.key_watch_->touched(key_);
  capture();
  note_type();
  db_.stage_data(batch(), data_key(sub), std::string(value));
  if (complete_) {
    elems_.put(sub, value);
    elements_changed_ = true;
  }
}

void Database::KeyTxn::del(std::string_view sub) {
  db_.key_watch_->touched(key_);
  capture();
  note_type();
  db_.stage_data(batch(), data_key(sub), std::nullopt);
  if (complete_) {
    elems_.erase(sub);
    elements_changed_ = true;
  }
}

void Database::KeyTxn::capture() {
  if (captured_ || indexes_.empty()) return;
  captured_ = true;
  for (const auto& index : indexes_) old_tuples_.push_back(current_tuples(index->def));
  old_version_ = meta_.version;
  old_expire_ = rec_ ? rec_->expire_at_ms : 0;
}

IndexDef::Tuples Database::KeyTxn::current_tuples(const IndexDef& def) {
  if (def.json()) {
    if (!rec_ || rec_->type != ValueType::Json || rec_->expired(unix_time_ms())) return {};
    std::string error;
    auto doc = parse_json(rec_->value, &error);
    return doc ? def.json_tuples(*doc) : IndexDef::Tuples{};
  }
  if (!rec_ || rec_->type != ValueType::Hash || meta_.size == 0) return {};
  return def.tuples([&](std::string_view field) { return get(field); });
}

void Database::KeyTxn::unique_indexes(std::vector<IndexRegistry::Ptr>* out) const {
  if (captured_) {
    for (const auto& index : indexes_) {
      if (index->def.unique) out->push_back(index);
    }
  }
  for (const auto& index : reindex_) {
    if (index->def.unique) out->push_back(index);
  }
  for (const auto& [index, entry] : stale_) {
    if (index->def.unique) out->push_back(index);
  }
}

void Database::KeyTxn::prepare_indexes() {
  changes_.clear();
  static const IndexDef::Tuples kNone;
  auto plan = [&](const IndexRegistry::Ptr& index, const IndexDef::Tuples& old, bool force,
                  const std::string* stale) {
    const IndexDef& def = index->def;
    IndexDef::Tuples now = current_tuples(def);
    int64_t expire = rec_ ? rec_->expire_at_ms : 0;
    // The entry value carries the hash version and TTL, so it changes with them too.
    bool same_value = old_version_ == meta_.version && old_expire_ == expire;
    if (same_value && old == now && !force) return;
    auto unique_owner = [&](const std::string& entry) -> std::optional<std::string> {
      auto value = db_.read_index(entry);
      IndexDef::Entry e;
      if (!value || !def.split_entry(entry, *value, &e)) return std::nullopt;
      return std::string(e.owner);
    };
    auto own_delete = [&](std::string entry) {
      // A UNIQUE entry is only ours to delete if it points to this key.
      if (def.unique && unique_owner(entry) != std::optional<std::string>(key_)) return;
      changes_.push_back({std::move(entry), std::nullopt, {}});
    };
    for (const auto& t : old) {
      if (!contains_tuple(now, t)) own_delete(def.entry_key(t, key_));
    }
    if (stale) {
      IndexDef::Entry e;
      auto value = db_.read_index(*stale);
      bool current = value && def.split_entry(*stale, *value, &e) && e.owner == key_ && contains_tuple(now, e.tuple);
      if (!current) own_delete(*stale);
    }
    for (const auto& t : now) {
      if (same_value && !force && contains_tuple(old, t)) continue;
      std::string put_key = def.entry_key(t, key_);
      if (def.unique) {
        auto value = db_.read_index(put_key);
        if (value) {
          // Taken, unless that entry is stale (its owner is gone or newer).
          auto owner = db_.live_unique_owner(def, *value);
          if (owner && *owner != key_) {
            throw IndexConstraintError(std::format("ERR UNIQUE index '{}' already has this value, for key '{}'",
                                                   def.name, *owner));
          }
        }
      }
      changes_.push_back({std::nullopt, std::move(put_key), def.entry_value(meta_.version, expire, key_)});
    }
  };
  if (captured_) {
    for (size_t i = 0; i < indexes_.size(); ++i) plan(indexes_[i], old_tuples_[i], false, nullptr);
  }
  for (const auto& index : reindex_) plan(index, kNone, true, nullptr);
  for (const auto& [index, entry] : stale_) plan(index, kNone, true, &entry);
}

void Database::KeyTxn::apply_indexes() {
  for (auto& c : changes_) {
    if (c.del) db_.stage_index(batch(), std::move(*c.del), std::nullopt);
    if (c.put) db_.stage_index(batch(), std::move(*c.put), std::move(c.value));
  }
  changes_.clear();
}

void Database::KeyTxn::scan(std::string_view from, std::string_view to, bool reverse, const ScanFn& fn) {
  if (!rec_ || !is_collection(rec_->type)) return;
  if (complete_) {
    if (to.empty() || from < to) elems_.for_range(from, to, reverse, fn);
    return;
  }
  std::string lower = data_key(from);
  std::string upper = to.empty() ? data_prefix(key_, meta_.version + 1) : data_key(to);
  if (lower >= upper) return;

  // Merge RocksDB with this thread's pending writes; pending entries win.
  auto cursor = db_.storage_.cursor(Cf::Data, lower, upper);
  auto& data = batch().data;
  auto p_begin = data.lower_bound(lower), p_end = data.lower_bound(upper);
  size_t skip = prefix_.size();
  auto emit = [&](std::string_view k, const std::optional<std::string>* pending, std::string_view v) {
    if (pending) return !*pending || fn(k.substr(skip), **pending);
    return fn(k.substr(skip), v);
  };

  if (!reverse) {
    cursor->seek_first();
    auto p = p_begin;
    for (;;) {
      bool has_c = cursor->valid(), has_p = p != p_end;
      if (!has_c && !has_p) break;
      if (has_p && (!has_c || std::string_view(p->first) <= cursor->key())) {
        if (has_c && std::string_view(p->first) == cursor->key()) cursor->next();
        if (!emit(p->first, &p->second, {})) return;
        ++p;
      } else {
        if (!emit(cursor->key(), nullptr, cursor->value())) return;
        cursor->next();
      }
    }
  } else {
    cursor->seek_last();
    auto p = p_end;
    for (;;) {
      bool has_c = cursor->valid(), has_p = p != p_begin;
      if (!has_c && !has_p) break;
      auto prev = has_p ? std::prev(p) : p;
      if (has_p && (!has_c || std::string_view(prev->first) >= cursor->key())) {
        if (has_c && std::string_view(prev->first) == cursor->key()) cursor->prev();
        if (!emit(prev->first, &prev->second, {})) return;
        p = prev;
      } else {
        if (!emit(cursor->key(), nullptr, cursor->value())) return;
        cursor->prev();
      }
    }
  }
  cursor->check_status();
}

void Database::KeyTxn::finish() {
  if (removed_ || (rec_ && is_collection(rec_->type) && meta_.size == 0)) {
    // An empty collection does not exist.
    if (rec_ && is_collection(rec_->type) && stored_meta_ && stored_meta_->version == meta_.version) {
      drop_elements(meta_);
    }
    if (had_key_) db_.erase_locked(key_);
    batch().elements.erase(key_);
    return;
  }
  if (!rec_ || !is_collection(rec_->type)) return;
  if (complete_ && (from_batch_ || elements_changed_ || populate_)) {
    batch().elements[key_] = PendingBatch::CachedElements{meta_.version, std::move(elems_)};
  }
  if (!(stored_meta_ && *stored_meta_ == meta_)) {
    // The cache gets the meta now and the elements when the batch commits.
    rec_->value = encode_collection_meta(meta_);
    if (!complete_) rec_->value += partial_cache_tail();
    db_.store_locked(key_, *rec_, true);
    rec_->value.resize(kCollectionMetaSize);
  }
}

// ---- keyspace

bool Database::load_into_cache(std::string_view key) {
  auto cached = [&] { return cache_.lookup(key, [](const RecordView&) {}, false); };
  if (cached()) return true;
  KeyGuard lock = lock_for_read(key);
  if (cached()) return true;  // loaded by someone else while this waited
  std::string raw;
  if (!storage_.get(key, &raw)) return false;
  RecordView v;
  if (!decode_record(raw, &v)) throw StorageError("corrupt record");
  if (v.expired(unix_time_ms())) return false;  // the rerun read expires it
  return cache_.insert(key, v, true);
}

std::vector<bool> Database::load_many_into_cache(std::span<const std::string> keys) {
  auto cached = [&](std::string_view key) { return cache_.lookup(key, [](const RecordView&) {}, false); };
  std::vector<bool> result(keys.size(), false);
  std::vector<size_t> locked, busy;  // indexes into keys
  std::vector<size_t> held;          // stripes
  std::vector<KeyGuard> guards;
  guards.reserve(keys.size());
  for (size_t i = 0; i < keys.size(); ++i) {
    if (cached(keys[i])) {
      result[i] = true;
      continue;
    }
    // try_lock never waits, so the order the stripes are taken in does not matter.
    size_t s = locks_.stripe_of(keys[i]);
    if (std::find(held.begin(), held.end(), s) != held.end()) {
      locked.push_back(i);
    } else if (locks_.try_lock(s)) {
      guards.emplace_back(&locks_, s);
      held.push_back(s);
      locked.push_back(i);
    } else {
      busy.push_back(i);
    }
  }
  std::vector<size_t> missing;
  std::vector<std::string_view> names;
  for (size_t i : locked) {
    if (cached(keys[i])) {  // loaded by someone else before the lock was taken
      result[i] = true;
    } else {
      missing.push_back(i);
      names.push_back(keys[i]);
    }
  }
  if (!names.empty()) {
    std::vector<std::optional<std::string>> raw;
    storage_.multi_get(Cf::Meta, names, &raw);
    int64_t now = unix_time_ms();
    for (size_t j = 0; j < missing.size(); ++j) {
      if (!raw[j]) continue;
      RecordView v;
      if (!decode_record(*raw[j], &v)) throw StorageError("corrupt record");
      if (v.expired(now)) continue;  // the rerun read expires it
      result[missing[j]] = cache_.insert(keys[missing[j]], v, true);
    }
  }
  guards.clear();  // before waiting for the busy ones
  for (size_t i : busy) result[i] = load_into_cache(keys[i]);
  return result;
}

std::optional<Record> Database::load_locked(std::string_view key, bool meta_only) {
  std::optional<Record> out;
  // With the key locked, a pending item can only be this thread's own write.
  if (cache_.lookup(key, [&](RecordView v) {
        if (meta_only && is_collection(v.type)) v.value = v.value.substr(0, kCollectionMetaSize);
        out = Record::from(v);
      }, false, true)) {
    return out;
  }

  RecordView v;
  if (auto* p = pending(key)) {  // this thread's uncommitted write
    if (!*p) return std::nullopt;
    if (!decode_record(**p, &v)) throw StorageError("corrupt record");
    if (v.expired(unix_time_ms())) return std::nullopt;
    return Record::from(v);
  }
  std::string raw;
  if (!storage_.get(key, &raw)) return std::nullopt;
  if (!decode_record(raw, &v)) throw StorageError("corrupt record");
  if (v.expired(unix_time_ms())) {
    // Lazy expiration.
    delete_storage(key);
    CollectionMeta m;
    if (is_collection(v.type) && decode_collection_meta(v.value, &m)) drop_elements(key, m);
    return std::nullopt;
  }
  cache_.insert(key, v);
  return Record::from(v);
}

bool Database::exists_locked(std::string_view key) {
  if (cache_.lookup(key, [](const RecordView&) {}, false, true)) return true;
  std::string raw;
  if (auto* p = pending(key)) {
    if (!*p) return false;
    raw = **p;
  } else if (!storage_.get(key, &raw)) {
    return false;
  }
  RecordView v;
  if (!decode_record(raw, &v)) throw StorageError("corrupt record");
  return !v.expired(unix_time_ms());
}

void Database::cache_store(std::string_view key, const RecordView& rec, bool staged) {
  if (!cache_on_write_.load(std::memory_order_relaxed)) {
    cache_.erase(key);
    return;
  }
  PendingBatch* b = active_batch();
  bool pending = b && (staged || b->exec);
  cache_.insert(key, rec, false, pending);
  // A write of its own (MSET, RENAME) inside a block: visible when the block ends.
  if (pending && !staged) b->exec_written.emplace_back(key);
}

void Database::store_locked(std::string_view key, Record& rec, bool new_cas) {
  if (rec.expired(unix_time_ms())) {
    erase_locked(key);
    return;
  }
  if (new_cas || rec.cas == 0) rec.cas = next_cas();
  RecordView stored = rec.view();
  // A collection's cached elements stay in the cache; RocksDB keeps the meta.
  if (is_collection(rec.type) && stored.value.size() > kCollectionMetaSize) {
    stored.value = stored.value.substr(0, kCollectionMetaSize);
  }
  write_storage(key, encode_record(stored));
  cache_store(key, rec.view(), active_batch() != nullptr);
}

void Database::erase_locked(std::string_view key) {
  delete_storage(key);
  cache_.erase(key);
}

std::optional<Record> Database::get(std::string_view key) {
  std::optional<Record> out;
  if (cache_.lookup(key, [&](const RecordView& v) { out = Record::from(v); })) {
    count_lookup(true);
    return out;
  }
  KeyGuard lock = lock_for_read(key);
  out = load_locked(key);
  count_lookup(out.has_value());
  return out;
}

bool Database::exists(std::string_view key) {
  if (cache_.lookup(key, [](const RecordView&) {}, false)) return count_lookup(true);
  KeyGuard lock = lock_for_read(key);
  return count_lookup(exists_locked(key));
}

void Database::put(std::string_view key, Record rec) {
  if (indexes_->covers(key)) {
    // A hash overwritten blindly loses its index entries in the same batch.
    WriteBatchScope local(*this);
    KeyGuard lock = lock_key(key, true);
    auto old = load_locked(key);
    if (old && old->type == ValueType::Hash) {
      if (auto m = collection_meta(old)) drop_index_entries(key, *m);
    }
    JsonEntries before = json_entries(key, old);
    std::vector<std::unique_lock<std::mutex>> unique_locks;
    if (!before.indexes.empty()) {
      JsonEntries after = json_entries(key, std::optional<Record>(rec));
      unique_locks = lock_unique(before.indexes);
      for (auto& [k, v] : json_entry_changes(key, before, after)) write_index(std::move(k), std::move(v));
    }
    store_locked(key, rec, true);
    if (!unique_locks.empty()) commit_active_batch();
    local.commit();
    return;
  }
  KeyGuard lock = lock_key(key);
  store_locked(key, rec, true);
}

bool Database::del(std::string_view key) {
  bool found = false;
  update(key, [&](std::optional<Record>& rec) {
    found = rec.has_value();
    return found ? Op::Delete : Op::Keep;
  });
  return found;
}

bool Database::mset(const std::vector<std::pair<std::string_view, std::string_view>>& kvs, bool only_if_none_exist) {
  std::vector<std::string_view> keys;
  keys.reserve(kvs.size());
  for (const auto& kv : kvs) keys.push_back(kv.first);
  commit_active_batch();  // multi-key commands run unbatched
  auto guard = exec_holds_locks() ? KeyLocks::Multi(locks_, {}) : locks_.lock_keys(keys);
  for (auto k : keys) key_watch_->touched(k);

  if (only_if_none_exist) {
    for (auto k : keys) {
      if (exists_locked(k)) return false;
    }
  }
  std::vector<Record> recs;
  recs.reserve(kvs.size());
  auto batch = storage_.batch();
  if (indexes_->any()) {
    for (auto k : keys) {
      if (!indexes_->covers(k)) continue;
      auto old = load_locked(k);
      if (old && old->type == ValueType::Hash) {
        if (auto m = collection_meta(old)) drop_index_entries(k, *m, &batch);
      }
      if (old && old->type == ValueType::Json) {
        JsonEntries before = json_entries(k, old);
        for (auto& [entry, value] : json_entry_changes(k, before, before.gone())) batch.del(Cf::Index, entry);
      }
    }
  }
  for (const auto& [k, v] : kvs) {
    Record& r = recs.emplace_back();
    r.value = std::string(v);
    r.cas = next_cas();
    batch.put(k, encode_record(r.view()));
  }
  if (change_notes_) batch.put_log_data(change_note(unix_time_ms()));
  storage_.write(batch);
  for (size_t i = 0; i < kvs.size(); ++i) cache_store(kvs[i].first, recs[i].view(), false);
  return true;
}

Database::RenameResult Database::rename(std::string_view src, std::string_view dst, bool only_if_target_missing) {
  commit_active_batch();
  auto guard = exec_holds_locks() ? KeyLocks::Multi(locks_, {})
                                  : locks_.lock_keys(std::array<std::string_view, 2>{src, dst});
  // Nothing is pending for this thread now, so RocksDB has the whole state.
  auto rec = load_locked(src);
  if (!rec) return RenameResult::NoSuchKey;
  if (src == dst) return only_if_target_missing ? RenameResult::TargetExists : RenameResult::Ok;
  auto old_dst = load_locked(dst);
  if (only_if_target_missing && old_dst) return RenameResult::TargetExists;

  auto batch = storage_.batch();

  // Index entries move with a hash: src's go away, dst gets the same values.
  std::vector<std::unique_lock<std::mutex>> unique_locks;
  std::optional<uint64_t> renamed_version;  // the version the moved hash gets
  if (indexes_->any()) {
    auto src_m = rec->type == ValueType::Hash ? collection_meta(rec) : std::nullopt;
    auto dst_m = old_dst && old_dst->type == ValueType::Hash ? collection_meta(old_dst) : std::nullopt;
    std::vector<IndexRegistry::Ptr> dst_indexes = indexes_->matching(dst);
    for (const auto& index : dst_indexes) {
      if (index->def.unique) unique_locks.emplace_back(index->unique_mu);
    }
    if (src_m) drop_index_entries(src, *src_m, &batch);
    if (dst_m) drop_index_entries(dst, *dst_m, &batch);
    // A document's entries move to the new name.
    JsonEntries src_json = json_entries(src, rec), dst_json = json_entries(dst, old_dst);
    auto apply = [&](std::vector<std::pair<std::string, std::optional<std::string>>> changes) {
      for (auto& [entry, value] : changes) value ? batch.put(Cf::Index, entry, *value) : batch.del(Cf::Index, entry);
    };
    if (!src_json.indexes.empty()) apply(json_entry_changes(src, src_json, src_json.gone()));
    if (!dst_json.indexes.empty()) apply(json_entry_changes(dst, dst_json, json_entries(dst, rec), src));
    if (src_m) {
      uint64_t moved_version = next_version();
      for (const auto& index : dst_indexes) {
        for (const auto& t : stored_tuples(index->def, src, *src_m)) {
          std::string entry = index->def.entry_key(t, dst);
          if (index->def.unique) {
            if (auto value = read_index(entry)) {
              auto owner = live_unique_owner(index->def, *value);
              if (owner && *owner != src && *owner != dst) {
                throw IndexConstraintError(std::format(
                    "ERR UNIQUE index '{}' already has this value, for key '{}'", index->def.name, *owner));
              }
            }
          }
          batch.put(Cf::Index, entry, index->def.entry_value(moved_version, rec->expire_at_ms, dst));
        }
      }
      renamed_version = moved_version;
    }
  }

  if (auto m = collection_meta(old_dst)) {
    batch.delete_range(Cf::Data, data_prefix(dst, m->version), data_prefix(dst, m->version + 1));
  }
  if (auto m = collection_meta(rec)) {
    // Element keys contain the user key, so they are copied under the new name.
    std::string from = data_prefix(src, m->version), to = data_prefix(src, m->version + 1);
    CollectionMeta moved = *m;
    moved.version = renamed_version ? *renamed_version : next_version();
    std::string prefix = data_prefix(dst, moved.version);
    auto cursor = storage_.cursor(Cf::Data, from, to, false);
    for (cursor->seek_first(); cursor->valid(); cursor->next()) {
      std::string k = prefix;
      k.append(cursor->key().substr(from.size()));
      batch.put(Cf::Data, k, cursor->value());
    }
    cursor->check_status();
    batch.delete_range(Cf::Data, from, to);
    rec->value = encode_collection_meta(moved);
  }
  rec->cas = next_cas();
  key_watch_->touched(src);
  key_watch_->touched(dst);
  batch.put(dst, encode_record(rec->view()));
  batch.del(src);
  if (change_notes_) {
    std::string note = change_note(unix_time_ms());
    if (auto m = collection_meta(rec)) add_change_note_type(note, data_prefix(dst, m->version), rec->type);
    batch.put_log_data(note);
  }
  storage_.write(batch);
  cache_.erase(src);
  cache_store(dst, rec->view(), false);
  return RenameResult::Ok;
}

void Database::flush_all() {
  commit_active_batch();
  auto guard = exec_holds_locks() ? KeyLocks::Multi(locks_, {}) : locks_.lock_all();
  key_watch_->touched_all();
  storage_.delete_all(change_notes_ ? change_note(unix_time_ms()) : std::string());
  cache_.clear();
}

std::vector<std::string> Database::keys(std::string_view pattern) {
  commit_active_batch();  // the scan reads RocksDB directly
  std::vector<std::string> out;
  int64_t now = unix_time_ms();
  bool match_all = pattern == "*";
  storage_.scan([&](std::string_view key, std::string_view raw) {
    RecordView v;
    if (decode_record(raw, &v) && !v.expired(now) && (match_all || glob_match(pattern, key))) {
      out.emplace_back(key);
    }
    return true;
  });
  return out;
}

Database::KeyScan Database::scan_keys(std::string_view after, std::string_view prefix, size_t count,
                                     const std::function<bool(std::string_view, const RecordView&)>& keep) {
  commit_active_batch();  // the scan reads RocksDB directly
  std::string lower(prefix);
  if (!after.empty()) {
    std::string next_after(after);
    next_after.push_back('\0');  // the first key after `after`
    lower = std::max(lower, next_after);
  }
  KeyScan out;
  int64_t now = unix_time_ms();
  auto cursor = storage_.cursor(Cf::Meta, lower, prefix_end(prefix), false);
  size_t seen = 0;
  for (cursor->seek_first(); cursor->valid(); cursor->next()) {
    if (seen == count) {
      out.next = std::move(lower);  // the last key looked at
      return out;
    }
    ++seen;
    lower.assign(cursor->key());
    RecordView v;
    if (decode_record(cursor->value(), &v) && !v.expired(now) && keep(lower, v)) out.keys.push_back(lower);
  }
  cursor->check_status();
  return out;
}

uint64_t Database::approx_size() {
  commit_active_batch();
  return storage_.estimate_keys();
}

}  // namespace datadam
