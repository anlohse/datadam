#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "index/index_def.h"
#include "storage/rocks_storage.h"

namespace datadam {

// The secondary indexes that exist. Definitions are kept in the System column
// family; ids are never reused, so the entries of a dropped index can never
// be mistaken for those of a new one. Thread-safe.
class IndexRegistry : public IndexOwnerResolver {
 public:
  struct Index {
    IndexDef def;  // fixed after creation, except state and error (registry lock)
    std::atomic<IndexState> state{IndexState::Building};
    std::atomic<uint64_t> scanned{0};  // keys looked at by the backfill
    std::atomic<bool> dropped{false};
    std::mutex unique_mu;  // writers of a UNIQUE index check and commit under it
  };
  using Ptr = std::shared_ptr<Index>;

  explicit IndexRegistry(RocksStorage& storage);

  // Reads the definitions again (a replica, after the primary's changes
  // arrived): new indexes appear, removed ones are marked dropped, and states
  // and errors follow what is stored.
  void reload();

  bool any() const { return count_.load(std::memory_order_acquire) > 0; }

  // Registers and persists a new index; returns an error message or "".
  std::string create(IndexDef def, Ptr* out);
  // Removes an index and all its entries; null if there is none by that name.
  Ptr drop(std::string_view name);
  Ptr find(std::string_view name) const;
  std::vector<Ptr> all() const;
  // The indexes (building or ready) that a write to `key` must maintain.
  std::vector<Ptr> matching(std::string_view key) const;
  bool covers(std::string_view key) const;

  void set_state(const Ptr& index, IndexState state, std::string error = {});
  std::string error_of(const Ptr& index) const;

  // While paused, owner_of keeps every entry. A replica pauses it for a full
  // sync: the entries arrive before the definitions that explain them.
  void pause_gc(bool paused) { gc_paused_.store(paused, std::memory_order_release); }

  int owner_of(std::string_view key, std::string_view value, std::string* owner, bool* json,
               uint64_t* version) const override;

 private:
  void persist_locked(const Index& index);
  // The stored definitions, and the next id.
  std::vector<IndexDef> load(uint32_t* next_id) const;

  RocksStorage& storage_;
  mutable std::shared_mutex mu_;
  std::map<std::string, Ptr, std::less<>> by_name_;
  std::unordered_map<uint32_t, Ptr> by_id_;
  uint32_t next_id_ = 1;
  std::atomic<size_t> count_{0};
  std::atomic<bool> gc_paused_{false};
};

}  // namespace datadam
