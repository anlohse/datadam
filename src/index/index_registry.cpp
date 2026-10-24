#include "index/index_registry.h"

#include <algorithm>
#include <format>

#include "storage/data_keys.h"
#include "util/log.h"

namespace datadam {

namespace {
constexpr std::string_view kDefPrefix = "index:def:";
constexpr std::string_view kNextIdKey = "index:next-id";

std::string def_key(std::string_view name) {
  std::string k(kDefPrefix);
  k.append(name);
  return k;
}
}  // namespace

IndexRegistry::IndexRegistry(RocksStorage& storage) : storage_(storage) { reload(); }

std::vector<IndexDef> IndexRegistry::load(uint32_t* next_id) const {
  std::vector<IndexDef> defs;
  *next_id = 1;
  std::string raw;
  if (storage_.get(Cf::System, kNextIdKey, &raw) && raw.size() == 4) *next_id = decode_index_id(raw);
  auto c = storage_.cursor(Cf::System, std::string(kDefPrefix), prefix_end(kDefPrefix));
  for (c->seek_first(); c->valid(); c->next()) {
    IndexDef def;
    if (!IndexDef::decode(c->value(), &def)) {
      log_warn("ignoring unreadable index definition '{}'", c->key());
      continue;
    }
    *next_id = std::max(*next_id, def.id + 1);
    defs.push_back(std::move(def));
  }
  c->check_status();
  return defs;
}

void IndexRegistry::reload() {
  uint32_t next_id = 1;
  std::vector<IndexDef> defs = load(&next_id);
  std::unique_lock lock(mu_);
  std::unordered_map<uint32_t, Ptr> kept;
  for (IndexDef& def : defs) {
    Ptr index;
    if (auto it = by_id_.find(def.id); it != by_id_.end()) {
      index = it->second;
      index->def.state = def.state;
      index->def.error = std::move(def.error);
    } else {
      index = std::make_shared<Index>();
      index->def = std::move(def);
    }
    index->state = index->def.state;
    kept[index->def.id] = index;
  }
  for (auto& [id, index] : by_id_) {
    if (!kept.contains(id)) index->dropped = true;
  }
  by_id_ = std::move(kept);
  by_name_.clear();
  for (auto& [id, index] : by_id_) by_name_[index->def.name] = index;
  next_id_ = std::max(next_id_, next_id);
  count_ = by_name_.size();
}

std::string IndexRegistry::create(IndexDef def, Ptr* out) {
  std::unique_lock lock(mu_);
  if (by_name_.contains(def.name)) return "ERR Index already exists";
  def.id = next_id_++;
  def.state = IndexState::Building;
  auto index = std::make_shared<Index>();
  index->def = std::move(def);
  index->state = IndexState::Building;
  storage_.put(Cf::System, kNextIdKey, encode_index_id(next_id_));
  persist_locked(*index);
  by_id_[index->def.id] = index;
  by_name_[index->def.name] = index;
  count_ = by_name_.size();
  *out = index;
  return {};
}

IndexRegistry::Ptr IndexRegistry::drop(std::string_view name) {
  std::unique_lock lock(mu_);
  auto it = by_name_.find(name);
  if (it == by_name_.end()) return nullptr;
  Ptr index = it->second;
  index->dropped = true;
  by_name_.erase(it);
  by_id_.erase(index->def.id);
  count_ = by_name_.size();
  auto batch = storage_.batch();
  batch.del(Cf::System, def_key(index->def.name));
  std::string begin = index->def.id_prefix();
  batch.delete_range(Cf::Index, begin, prefix_end(begin));
  storage_.write(batch);
  return index;
}

IndexRegistry::Ptr IndexRegistry::find(std::string_view name) const {
  std::shared_lock lock(mu_);
  auto it = by_name_.find(name);
  return it == by_name_.end() ? nullptr : it->second;
}

std::vector<IndexRegistry::Ptr> IndexRegistry::all() const {
  std::shared_lock lock(mu_);
  std::vector<Ptr> out;
  for (const auto& [name, index] : by_name_) out.push_back(index);
  return out;
}

std::vector<IndexRegistry::Ptr> IndexRegistry::matching(std::string_view key) const {
  std::vector<Ptr> out;
  if (!any()) return out;
  std::shared_lock lock(mu_);
  for (const auto& [name, index] : by_name_) {
    if (index->state.load(std::memory_order_acquire) != IndexState::Failed && index->def.matches(key)) {
      out.push_back(index);
    }
  }
  // Id order: UNIQUE indexes are locked in this order.
  std::sort(out.begin(), out.end(), [](const Ptr& a, const Ptr& b) { return a->def.id < b->def.id; });
  return out;
}

bool IndexRegistry::covers(std::string_view key) const {
  if (!any()) return false;
  std::shared_lock lock(mu_);
  for (const auto& [name, index] : by_name_) {
    if (index->state.load(std::memory_order_acquire) != IndexState::Failed && index->def.matches(key)) return true;
  }
  return false;
}

void IndexRegistry::set_state(const Ptr& index, IndexState state, std::string error) {
  std::unique_lock lock(mu_);
  if (index->dropped) return;
  index->state = state;
  index->def.state = state;
  index->def.error = std::move(error);
  persist_locked(*index);
}

std::string IndexRegistry::error_of(const Ptr& index) const {
  std::shared_lock lock(mu_);
  return index->def.error;
}

int IndexRegistry::owner_of(std::string_view key, std::string_view value, std::string* owner, bool* json,
                            uint64_t* version) const {
  if (key.size() < 4 || gc_paused_.load(std::memory_order_acquire)) return 0;
  std::shared_lock lock(mu_);
  auto it = by_id_.find(decode_index_id(key));
  if (it == by_id_.end()) return -1;
  IndexDef::Entry e;
  if (!it->second->def.split_entry(key, value, &e)) return 0;
  owner->assign(e.owner);
  *json = it->second->def.json();
  *version = e.version;
  return 1;
}

void IndexRegistry::persist_locked(const Index& index) {
  storage_.put(Cf::System, def_key(index.def.name), index.def.encode());
}

}  // namespace datadam
