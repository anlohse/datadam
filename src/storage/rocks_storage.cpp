#include "storage/rocks_storage.h"

#include <rocksdb/advanced_cache.h>
#include <rocksdb/cache.h>
#include <rocksdb/compaction_filter.h>
#include <rocksdb/db.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/options.h>
#include <rocksdb/table.h>
#include <rocksdb/transaction_log.h>
#include <rocksdb/utilities/checkpoint.h>
#include <rocksdb/write_batch.h>

#include <algorithm>
#include <filesystem>
#include <format>

#include "storage/data_keys.h"
#include "storage/record.h"
#include "util/clock.h"
#include "util/log.h"

namespace datadam {

namespace {

void check(const rocksdb::Status& s, const char* what) {
  if (!s.ok()) throw StorageError(std::string(what) + ": " + s.ToString());
}

rocksdb::Slice slice(std::string_view v) { return {v.data(), v.size()}; }

// Drops expired records while RocksDB compacts, so keys that are never read
// again still free their disk space. The removal is written as a tombstone,
// so older versions in lower levels cannot resurface.
class TtlCompactionFilter : public rocksdb::CompactionFilter {
 public:
  bool Filter(int, const rocksdb::Slice&, const rocksdb::Slice& value, std::string*, bool*) const override {
    RecordView r;
    if (!decode_record({value.data(), value.size()}, &r)) return false;
    return r.expired(unix_time_ms());
  }
  const char* Name() const override { return "datadam.TtlCompactionFilter"; }
};

}  // namespace

// Drops data entries whose collection no longer exists: the meta record is
// gone, expired, of another type, or carries another version. This reclaims
// the elements of collections that expired or were overwritten without an
// explicit range delete.
class RocksStorage::DataGcFilterFactory : public rocksdb::CompactionFilterFactory {
 public:
  explicit DataGcFilterFactory(const RocksStorage* storage) : storage_(storage) {}

  std::unique_ptr<rocksdb::CompactionFilter> CreateCompactionFilter(
      const rocksdb::CompactionFilter::Context&) override {
    return std::make_unique<GcFilter>(storage_);
  }
  const char* Name() const override { return "datadam.DataGcFilterFactory"; }

 private:
  // One per compaction, used by one thread: remembers the last key looked up.
  class GcFilter : public rocksdb::CompactionFilter {
   public:
    explicit GcFilter(const RocksStorage* storage) : storage_(storage) {}

    bool Filter(int, const rocksdb::Slice& k, const rocksdb::Slice&, std::string*, bool*) const override {
      std::string_view key, sub;
      uint64_t version;
      if (!decode_data_key({k.data(), k.size()}, &key, &version, &sub)) return false;
      if (!looked_up_ || key != last_key_) {
        rocksdb::DB* db = storage_->gc_db_.load(std::memory_order_acquire);
        if (!db) return false;
        looked_up_ = false;
        last_key_.assign(key);
        rocksdb::PinnableSlice raw;
        auto st = db->Get(rocksdb::ReadOptions(), storage_->meta_, slice(key), &raw);
        if (st.IsNotFound()) {
          live_version_ = 0;
        } else if (!st.ok()) {
          return false;  // keep when in doubt; a later compaction retries
        } else {
          RecordView r;
          CollectionMeta m;
          bool live = decode_record({raw.data(), raw.size()}, &r) && is_collection(r.type) &&
                      !r.expired(unix_time_ms()) && decode_collection_meta(r.value, &m);
          live_version_ = live ? m.version : 0;
        }
        looked_up_ = true;
      }
      return version != live_version_;
    }
    const char* Name() const override { return "datadam.DataGcFilter"; }

   private:
    const RocksStorage* storage_;
    mutable std::string last_key_;
    mutable uint64_t live_version_ = 0;
    mutable bool looked_up_ = false;
  };

  const RocksStorage* storage_;
};

// Drops index entries whose index was dropped or whose owner is no longer
// the hash version the entry was written for.
class RocksStorage::IndexGcFilterFactory : public rocksdb::CompactionFilterFactory {
 public:
  explicit IndexGcFilterFactory(const RocksStorage* storage) : storage_(storage) {}

  std::unique_ptr<rocksdb::CompactionFilter> CreateCompactionFilter(
      const rocksdb::CompactionFilter::Context&) override {
    return std::make_unique<GcFilter>(storage_);
  }
  const char* Name() const override { return "datadam.IndexGcFilterFactory"; }

 private:
  class GcFilter : public rocksdb::CompactionFilter {
   public:
    explicit GcFilter(const RocksStorage* storage) : storage_(storage) {}

    bool Filter(int, const rocksdb::Slice& k, const rocksdb::Slice& v, std::string*, bool*) const override {
      rocksdb::DB* db = storage_->gc_db_.load(std::memory_order_acquire);
      const IndexOwnerResolver* resolver = storage_->index_resolver_.load(std::memory_order_acquire);
      if (!db || !resolver) return false;
      std::string owner;
      bool json = false;
      uint64_t version = 0;
      int r = resolver->owner_of({k.data(), k.size()}, {v.data(), v.size()}, &owner, &json, &version);
      if (r <= 0) return r < 0;
      rocksdb::PinnableSlice raw;
      auto st = db->Get(rocksdb::ReadOptions(), storage_->meta_, owner, &raw);
      if (st.IsNotFound()) return true;
      if (!st.ok()) return false;
      RecordView rec;
      if (!decode_record({raw.data(), raw.size()}, &rec) || rec.expired(unix_time_ms())) return true;
      // A document's entries are rewritten by its every write; a hash's carry its version.
      if (json) return rec.type != datadam::ValueType::Json;
      CollectionMeta m;
      return rec.type != datadam::ValueType::Hash || !decode_collection_meta(rec.value, &m) || m.version != version;
    }
    const char* Name() const override { return "datadam.IndexGcFilter"; }

   private:
    const RocksStorage* storage_;
  };

  const RocksStorage* storage_;
};

namespace {

rocksdb::ColumnFamilyOptions column_family_options(const StorageOptions& opts,
                                                   const std::shared_ptr<rocksdb::Cache>& cache) {
  rocksdb::ColumnFamilyOptions cf;
  cf.write_buffer_size = opts.write_buffer;
  cf.max_write_buffer_number = 4;
  cf.level_compaction_dynamic_level_bytes = true;
  cf.compression = rocksdb::kLZ4Compression;
  cf.bottommost_compression = rocksdb::kZSTD;

  rocksdb::BlockBasedTableOptions table;
  table.block_cache = cache;
  table.block_size = 16 * 1024;
  table.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10));
  // Index and filter blocks live in the block cache (bounded memory), split
  // into small partitions, in its high-priority pool: a lookup of a missing
  // element must never wait for a whole filter to be read back after a scan
  // pushed it out of the cache.
  table.cache_index_and_filter_blocks = true;
  table.cache_index_and_filter_blocks_with_high_priority = true;
  table.pin_l0_filter_and_index_blocks_in_cache = true;
  table.index_type = rocksdb::BlockBasedTableOptions::kTwoLevelIndexSearch;
  table.partition_filters = true;
  table.metadata_block_size = 4096;
  table.pin_top_level_index_and_filter = true;
  cf.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table));
  return cf;
}

}  // namespace

RocksStorage::Batch::Batch() : wb_(std::make_unique<rocksdb::WriteBatch>()) {}
RocksStorage::Batch::~Batch() = default;
RocksStorage::Batch::Batch(Batch&&) noexcept = default;
RocksStorage::Batch& RocksStorage::Batch::operator=(Batch&&) noexcept = default;

void RocksStorage::Batch::put(Cf cf, std::string_view key, std::string_view value) {
  check(wb_->Put(owner_->handle(cf), slice(key), slice(value)), "batch put");
}

void RocksStorage::Batch::del(Cf cf, std::string_view key) {
  check(wb_->Delete(owner_->handle(cf), slice(key)), "batch delete");
}

void RocksStorage::Batch::delete_range(Cf cf, std::string_view begin, std::string_view end) {
  check(wb_->DeleteRange(owner_->handle(cf), slice(begin), slice(end)), "batch delete range");
}

void RocksStorage::Batch::put_log_data(std::string_view blob) { check(wb_->PutLogData(slice(blob)), "batch log data"); }

std::string_view RocksStorage::Batch::data() const { return wb_->Data(); }

size_t RocksStorage::Batch::size() const { return wb_->GetDataSize(); }

void RocksStorage::Batch::clear() { wb_->Clear(); }

bool RocksStorage::Batch::empty() const { return wb_->Count() == 0; }

RocksStorage::RocksStorage(const StorageOptions& opts) : opts_(opts), fsync_(opts.fsync) {}

std::unique_ptr<RocksStorage> RocksStorage::open(const StorageOptions& opts) {
  std::filesystem::create_directories(opts.dir);
  std::unique_ptr<RocksStorage> s(new RocksStorage(opts));

  rocksdb::DBOptions db_opts;
  db_opts.create_if_missing = true;
  db_opts.create_missing_column_families = true;
  db_opts.max_background_jobs = opts.background_jobs;
  db_opts.bytes_per_sync = 1 << 20;
  db_opts.keep_log_file_num = 5;
  db_opts.manual_wal_flush = opts.write_path == WritePath::WalBuffered;
  db_opts.WAL_ttl_seconds = opts.wal_retention_s;
  db_opts.WAL_size_limit_MB = opts.wal_retention_s > 0 ? (opts.wal_retention_bytes + (1 << 20) - 1) >> 20 : 0;

  rocksdb::LRUCacheOptions cache_opts;
  cache_opts.capacity = opts.block_cache;
  cache_opts.high_pri_pool_ratio = 0.5;
  auto cache = rocksdb::NewLRUCache(cache_opts);
  s->block_cache_ = cache;
  static TtlCompactionFilter ttl_filter;
  rocksdb::ColumnFamilyOptions plain = column_family_options(opts, cache);
  rocksdb::ColumnFamilyOptions meta = plain;
  meta.compaction_filter = &ttl_filter;
  rocksdb::ColumnFamilyOptions data = plain;
  data.compaction_filter_factory = std::make_shared<DataGcFilterFactory>(s.get());
  rocksdb::ColumnFamilyOptions index = plain;
  index.compaction_filter_factory = std::make_shared<IndexGcFilterFactory>(s.get());

  std::vector<rocksdb::ColumnFamilyDescriptor> families = {
      {rocksdb::kDefaultColumnFamilyName, plain},
      {"meta", meta},
      {"data", data},
      {"index", index},
  };
  check(rocksdb::DB::Open(db_opts, opts.dir, families, &s->handles_, &s->db_), "open");
  s->meta_ = s->handles_[1];
  s->gc_db_.store(s->db_.get(), std::memory_order_release);
  return s;
}

RocksStorage::~RocksStorage() {
  if (!db_) return;
  gc_db_.store(nullptr, std::memory_order_release);
  if (fsync_policy() != FsyncPolicy::Always) {
    try {
      sync_wal();
    } catch (const StorageError& e) {
      log_warn("WAL sync on shutdown failed: {}", e.what());
    }
  }
  for (auto* h : handles_) db_->DestroyColumnFamilyHandle(h);
  auto st = db_->Close();
  if (!st.ok()) log_warn("RocksDB close failed: {}", st.ToString());
}

namespace {
rocksdb::WriteOptions write_options(const StorageOptions& o, FsyncPolicy fsync) {
  rocksdb::WriteOptions w;
  w.sync = fsync == FsyncPolicy::Always;
  w.disableWAL = o.write_path == WritePath::NoWal;
  return w;
}
}  // namespace

rocksdb::ColumnFamilyHandle* RocksStorage::handle(Cf cf) const {
  switch (cf) {
    case Cf::Meta: return handles_[1];
    case Cf::Data: return handles_[2];
    case Cf::Index: return handles_[3];
    case Cf::System: return handles_[0];
  }
  return handles_[0];
}

bool RocksStorage::get(Cf cf, std::string_view key, std::string* value, const Snapshot* snapshot) {
  rocksdb::PinnableSlice out;
  rocksdb::ReadOptions ro;
  if (snapshot) ro.snapshot = snapshot->snap_;
  rocksdb::Status st;
  {
    LatencyHistogram::Timer t(get_latency_);
    st = db_->Get(ro, handle(cf), slice(key), &out);
  }
  if (st.IsNotFound()) return false;
  check(st, "get");
  value->assign(out.data(), out.size());
  return true;
}

RocksStorage::Probe RocksStorage::probe(Cf cf, std::string_view key) {
  rocksdb::PinnableSlice out;
  rocksdb::ReadOptions ro;
  ro.read_tier = rocksdb::kBlockCacheTier;  // Incomplete instead of reading a file
  rocksdb::Status st = db_->Get(ro, handle(cf), slice(key), &out);
  if (st.ok()) return Probe::Found;
  if (st.IsNotFound()) return Probe::Missing;
  return Probe::NeedsIo;  // Incomplete, or an error the real read will report
}

void RocksStorage::multi_get(Cf cf, std::span<const std::string_view> keys,
                             std::vector<std::optional<std::string>>* values) {
  size_t n = keys.size();
  std::vector<rocksdb::Slice> slices;
  slices.reserve(n);
  for (std::string_view k : keys) slices.push_back(slice(k));
  std::vector<rocksdb::PinnableSlice> out(n);
  std::vector<rocksdb::Status> st(n);
  {
    LatencyHistogram::Timer t(multi_get_latency_);
    db_->MultiGet(rocksdb::ReadOptions(), handle(cf), n, slices.data(), out.data(), st.data());
  }
  values->assign(n, std::nullopt);
  for (size_t i = 0; i < n; ++i) {
    if (st[i].IsNotFound()) continue;
    check(st[i], "multiget");
    (*values)[i].emplace(out[i].data(), out[i].size());
  }
}

void RocksStorage::put(Cf cf, std::string_view key, std::string_view value) {
  if (skip_writes()) return;
  check(db_->Put(write_options(opts_, fsync_policy()), handle(cf), slice(key), slice(value)), "put");
  wrote();
}

void RocksStorage::del(std::string_view key) {
  if (skip_writes()) return;
  check(db_->Delete(write_options(opts_, fsync_policy()), meta_, slice(key)), "delete");
  wrote();
}

RocksStorage::Batch RocksStorage::batch() {
  Batch b;
  b.owner_ = this;
  return b;
}

struct RocksStorage::Cursor::Bounds {
  std::string lower, upper;
  rocksdb::Slice lower_slice, upper_slice;
};

RocksStorage::Cursor::~Cursor() = default;

void RocksStorage::Cursor::seek_first() { it_->SeekToFirst(); }
void RocksStorage::Cursor::seek_last() { it_->SeekToLast(); }
bool RocksStorage::Cursor::valid() const { return it_->Valid(); }
void RocksStorage::Cursor::next() { it_->Next(); }
void RocksStorage::Cursor::prev() { it_->Prev(); }

std::string_view RocksStorage::Cursor::key() const {
  auto k = it_->key();
  return {k.data(), k.size()};
}

std::string_view RocksStorage::Cursor::value() const {
  auto v = it_->value();
  return {v.data(), v.size()};
}

void RocksStorage::Cursor::check_status() const { check(it_->status(), "iterate"); }

RocksStorage::Snapshot::~Snapshot() {
  if (db_ && snap_) db_->ReleaseSnapshot(snap_);
}

uint64_t RocksStorage::Snapshot::sequence() const { return snap_->GetSequenceNumber(); }

std::unique_ptr<RocksStorage::Snapshot> RocksStorage::snapshot() {
  std::unique_ptr<Snapshot> s(new Snapshot());
  s->db_ = db_.get();
  s->snap_ = db_->GetSnapshot();
  return s;
}

std::unique_ptr<RocksStorage::Cursor> RocksStorage::cursor(Cf cf, std::string lower, std::string upper,
                                                           bool fill_cache, const Snapshot* snapshot) {
  std::unique_ptr<Cursor> c(new Cursor());
  c->bounds_ = std::make_unique<Cursor::Bounds>();
  Cursor::Bounds& b = *c->bounds_;
  b.lower = std::move(lower);
  b.upper = std::move(upper);
  b.lower_slice = rocksdb::Slice(b.lower);
  b.upper_slice = rocksdb::Slice(b.upper);
  rocksdb::ReadOptions ro;
  ro.fill_cache = fill_cache;
  if (snapshot) ro.snapshot = snapshot->snap_;
  ro.iterate_lower_bound = &b.lower_slice;
  if (!b.upper.empty()) ro.iterate_upper_bound = &b.upper_slice;
  c->it_.reset(db_->NewIterator(ro, handle(cf)));
  return c;
}

void RocksStorage::write(Batch& b) {
  if (skip_writes()) return;
  LatencyHistogram::Timer t(write_latency_);
  check(db_->Write(write_options(opts_, fsync_policy()), b.wb_.get()), "write");
  wrote();
}

void RocksStorage::scan(const std::function<bool(std::string_view, std::string_view)>& fn) {
  rocksdb::ReadOptions ro;
  ro.fill_cache = false;
  std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(ro, meta_));
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    auto k = it->key();
    auto v = it->value();
    if (!fn({k.data(), k.size()}, {v.data(), v.size()})) break;
  }
  check(it->status(), "scan");
}

void RocksStorage::delete_all(std::string_view log_data) {
  rocksdb::WriteBatch wb;
  for (size_t i = 1; i < handles_.size(); ++i) {
    std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(rocksdb::ReadOptions(), handles_[i]));
    it->SeekToFirst();
    if (!it->Valid()) continue;
    std::string first = it->key().ToString();
    it->SeekToLast();
    std::string last = it->key().ToString();
    it.reset();
    // DeleteRange's end is exclusive, so the last key is deleted separately.
    check(wb.DeleteRange(handles_[i], first, last), "delete range");
    check(wb.Delete(handles_[i], last), "delete");
  }
  if (wb.Count() == 0) return;
  if (!log_data.empty()) check(wb.PutLogData(slice(log_data)), "batch log data");
  check(db_->Write(write_options(opts_, fsync_policy()), &wb), "delete all");
  wrote();
}

// ---- change feed

bool RocksStorage::wal_retained() const {
  return opts_.wal_retention_s > 0 && (opts_.write_path == WritePath::Wal || opts_.write_path == WritePath::WalBuffered);
}

uint64_t RocksStorage::latest_sequence() const { return db_->GetLatestSequenceNumber(); }

uint64_t RocksStorage::oldest_sequence() const {
  rocksdb::VectorWalPtr files;
  if (!db_->GetSortedWalFiles(files).ok() || files.empty()) return latest_sequence() + 1;
  return files.front()->StartSequence();
}

uint64_t RocksStorage::wal_bytes() const {
  rocksdb::VectorWalPtr files;
  if (!db_->GetSortedWalFiles(files).ok()) return 0;
  uint64_t total = 0;
  for (const auto& f : files) total += f->SizeFileBytes();
  return total;
}

size_t RocksStorage::trim_wal() {
  if (!wal_retained() || opts_.wal_retention_bytes == 0) return 0;
  rocksdb::VectorWalPtr files;
  if (!db_->GetSortedWalFiles(files).ok()) return 0;
  uint64_t kept = 0;
  for (const auto& f : files) {
    if (f->Type() == rocksdb::kArchivedLogFile) kept += f->SizeFileBytes();
  }
  size_t deleted = 0;
  for (const auto& f : files) {  // oldest first
    if (kept <= opts_.wal_retention_bytes) break;
    if (f->Type() != rocksdb::kArchivedLogFile) continue;
    // Archived WAL files are not needed for recovery; a reader still on this
    // one gets the "no longer kept" error from read_wal.
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path(opts_.dir) / f->PathName().substr(1), ec);
    if (ec) {
      log_warn("deleting kept WAL file {} failed: {}", f->PathName(), ec.message());
      break;
    }
    kept -= f->SizeFileBytes();
    ++deleted;
  }
  return deleted;
}

Cf RocksStorage::cf_of(uint32_t column_family_id) const {
  for (Cf cf : {Cf::Meta, Cf::Data, Cf::Index}) {
    if (handle(cf)->GetID() == column_family_id) return cf;
  }
  return Cf::System;
}

namespace {

// Hands the entries of one rocksdb::WriteBatch to a WalVisitor.
class BatchDispatcher : public rocksdb::WriteBatch::Handler {
 public:
  BatchDispatcher(WalVisitor& v, std::function<Cf(uint32_t)> cf_of) : v_(v), cf_of_(std::move(cf_of)) {}

  rocksdb::Status PutCF(uint32_t cf, const rocksdb::Slice& key, const rocksdb::Slice& value) override {
    v_.put(cf_of_(cf), view(key), view(value));
    return rocksdb::Status::OK();
  }
  rocksdb::Status DeleteCF(uint32_t cf, const rocksdb::Slice& key) override {
    v_.del(cf_of_(cf), view(key));
    return rocksdb::Status::OK();
  }
  rocksdb::Status SingleDeleteCF(uint32_t cf, const rocksdb::Slice& key) override { return DeleteCF(cf, key); }
  rocksdb::Status DeleteRangeCF(uint32_t cf, const rocksdb::Slice& begin, const rocksdb::Slice& end) override {
    v_.delete_range(cf_of_(cf), view(begin), view(end));
    return rocksdb::Status::OK();
  }
  rocksdb::Status MergeCF(uint32_t, const rocksdb::Slice&, const rocksdb::Slice&) override {
    return rocksdb::Status::OK();  // never written
  }
  void LogData(const rocksdb::Slice& blob) override { v_.log_data(view(blob)); }

 private:
  static std::string_view view(const rocksdb::Slice& s) { return {s.data(), s.size()}; }

  WalVisitor& v_;
  std::function<Cf(uint32_t)> cf_of_;
};

}  // namespace

void RocksStorage::for_each_wal_batch(uint64_t from,
                                      const std::function<bool(uint64_t, rocksdb::WriteBatch&)>& fn) const {
  if (!wal_retained()) throw StorageError("the WAL is not kept (wal-retention 0)");
  uint64_t oldest = oldest_sequence();
  if (from != 0 && from < oldest) {
    throw StorageError(std::format("sequence {} is no longer in the kept WAL (the oldest is {})", from, oldest));
  }
  uint64_t start = from == 0 ? std::max<uint64_t>(oldest, 1) : from;
  if (start > latest_sequence()) return;
  std::unique_ptr<rocksdb::TransactionLogIterator> it;
  check(db_->GetUpdatesSince(start, &it), "read wal");
  bool first = true;
  for (; it->Valid(); it->Next()) {
    rocksdb::BatchResult r = it->GetBatch();
    if (first && from != 0 && r.sequence > from) {
      throw StorageError(std::format("sequence {} is no longer in the kept WAL (the oldest is {})", from, r.sequence));
    }
    first = false;
    if (!fn(r.sequence, *r.writeBatchPtr)) return;
  }
  // The end of the WAL reads as "not found" or "sequence not written yet": just no more batches.
  if (auto st = it->status(); !st.ok() && !st.IsNotFound() && !st.IsTryAgain()) check(st, "read wal");
}

void RocksStorage::read_wal(uint64_t from, WalVisitor& visitor) const {
  BatchDispatcher dispatch(visitor, [this](uint32_t id) { return cf_of(id); });
  for_each_wal_batch(from, [&](uint64_t seq, rocksdb::WriteBatch& b) {
    if (!visitor.begin(seq, static_cast<uint64_t>(b.Count()))) return false;
    check(b.Iterate(&dispatch), "decode wal batch");
    visitor.end();
    return true;
  });
}

size_t RocksStorage::read_wal_raw(uint64_t from, size_t max_bytes, std::vector<RawBatch>* out) const {
  size_t n = 0, bytes = 0;
  for_each_wal_batch(from, [&](uint64_t seq, rocksdb::WriteBatch& b) {
    if (n > 0 && bytes >= max_bytes) return false;
    out->push_back(RawBatch{seq, static_cast<uint64_t>(b.Count()), b.Data()});
    bytes += b.GetDataSize();
    ++n;
    return true;
  });
  return n;
}

RocksStorage::CfIds RocksStorage::cf_ids() const {
  return {handle(Cf::Meta)->GetID(), handle(Cf::Data)->GetID(), handle(Cf::Index)->GetID(),
          handle(Cf::System)->GetID()};
}

void RocksStorage::decode_batch(std::string_view data, const CfIds& ids, WalVisitor& visitor) const {
  rocksdb::WriteBatch b{std::string(data)};
  BatchDispatcher dispatch(visitor, [&ids](uint32_t id) {
    for (Cf cf : {Cf::Meta, Cf::Data, Cf::Index}) {
      if (ids[static_cast<size_t>(cf)] == id) return cf;
    }
    return Cf::System;
  });
  check(b.Iterate(&dispatch), "decode batch");
}

uint64_t RocksStorage::on_next_write(uint64_t after_seq, std::function<void()> fn) {
  std::lock_guard lock(write_waits_mu_);
  if (latest_sequence() > after_seq) return 0;
  uint64_t id = next_write_wait_++;
  write_waits_.emplace_back(id, std::move(fn));
  has_write_waits_.store(true, std::memory_order_release);
  return id;
}

void RocksStorage::cancel_write_wait(uint64_t id) {
  std::lock_guard lock(write_waits_mu_);
  std::erase_if(write_waits_, [id](const auto& w) { return w.first == id; });
  if (write_waits_.empty()) has_write_waits_.store(false, std::memory_order_release);
}

void RocksStorage::cancel_all_write_waits() {
  std::vector<std::pair<uint64_t, std::function<void()>>> gone;
  {
    std::lock_guard lock(write_waits_mu_);
    gone.swap(write_waits_);
    has_write_waits_.store(false, std::memory_order_release);
  }
}

void RocksStorage::wrote() {
  if (!has_write_waits_.load(std::memory_order_acquire)) return;
  std::vector<std::pair<uint64_t, std::function<void()>>> ready;
  {
    std::lock_guard lock(write_waits_mu_);
    ready.swap(write_waits_);
    has_write_waits_.store(false, std::memory_order_release);
  }
  for (auto& [id, fn] : ready) fn();
}

uint64_t RocksStorage::estimate_keys() {
  uint64_t n = 0;
  db_->GetIntProperty(meta_, "rocksdb.estimate-num-keys", &n);
  return n;
}

std::string RocksStorage::property(const std::string& name) {
  std::string v;
  if (!db_->GetProperty(meta_, name, &v)) return {};
  return v;
}

void RocksStorage::sync_wal() {
  switch (opts_.write_path) {
    case WritePath::Wal: check(db_->SyncWAL(), "sync wal"); break;
    case WritePath::WalBuffered: check(db_->FlushWAL(fsync_policy() != FsyncPolicy::No), "flush wal"); break;
    case WritePath::NoWal:
    case WritePath::Memory: break;
  }
}

void RocksStorage::flush() {
  rocksdb::FlushOptions fo;
  check(db_->Flush(fo, handles_), "flush");
}

void RocksStorage::checkpoint(const std::string& dir) {
  if (opts_.write_path == WritePath::Memory) throw StorageError("checkpoint: nothing is on disk (write path memory)");
  rocksdb::Checkpoint* raw = nullptr;
  check(rocksdb::Checkpoint::Create(db_.get(), &raw), "checkpoint");
  std::unique_ptr<rocksdb::Checkpoint> cp(raw);
  check(cp->CreateCheckpoint(dir, 0), "checkpoint");
}

void RocksStorage::compact() {
  rocksdb::CompactRangeOptions co;
  co.bottommost_level_compaction = rocksdb::BottommostLevelCompaction::kForce;
  for (auto* h : handles_) check(db_->CompactRange(co, h, nullptr, nullptr), "compact");
}

void RocksStorage::set_block_cache_size(uint64_t bytes) { block_cache_->SetCapacity(bytes); }

void RocksStorage::set_write_buffer_size(uint64_t bytes) {
  for (auto* h : handles_) {
    check(db_->SetOptions(h, {{"write_buffer_size", std::to_string(bytes)}}), "set write buffer size");
  }
}

}  // namespace datadam
