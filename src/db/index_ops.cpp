// Secondary indexes on the Database side: creation and backfill, queries, and
// the helpers that maintain entries outside of KeyTxn.

#include <format>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include "db/database.h"
#include "storage/data_keys.h"
#include "util/clock.h"
#include "util/log.h"
#include "util/strings.h"

namespace datadam {

namespace {

constexpr size_t kBackfillChunk = 256;
constexpr size_t kMaxRepairsPerQuery = 64;

using Cond = IndexCondition;

bool is_range(Cond::Op op) { return op != Cond::Op::Eq; }

// A condition evaluated on a value of a hash or document found through the index.
bool condition_holds(const Cond& c, std::string_view value, std::string_view wanted, bool numeric) {
  int cmp;
  if (numeric) {
    auto a = parse_double(value), b = parse_double(wanted);
    if (!a || !b) return false;
    cmp = *a < *b ? -1 : (*a > *b ? 1 : 0);
  } else {
    int r = value.compare(wanted);
    cmp = r < 0 ? -1 : (r > 0 ? 1 : 0);
  }
  switch (c.op) {
    case Cond::Op::Eq: return cmp == 0;
    case Cond::Op::Gt: return cmp > 0;
    case Cond::Op::Ge: return cmp >= 0;
    case Cond::Op::Lt: return cmp < 0;
    case Cond::Op::Le: return cmp <= 0;
  }
  return false;
}

// The condition holds for one of the values (an index field may have several).
bool condition_holds(const IndexDef& def, const Cond& c, const std::vector<std::string>& values) {
  int fi = def.field_index(c.field);
  bool numeric = fi >= 0 ? def.fields[static_cast<size_t>(fi)].type == IndexFieldType::Numeric : is_range(c.op);
  std::string wanted = fi >= 0 ? def.normalize(static_cast<size_t>(fi), c.value) : c.value;
  for (const auto& v : values) {
    if (condition_holds(c, v, wanted, numeric)) return true;
  }
  return false;
}

// The slice of the index a query reads, and what is left to filter.
struct Plan {
  std::string lower, upper;
  std::vector<const Cond*> filters;
  bool empty = false;
};

std::string plan_query(const IndexDef& def, const std::vector<Cond>& where, Plan* plan) {
  std::vector<bool> used(where.size(), false);
  std::string prefix = def.id_prefix();
  size_t k = 0;
  // Equality on the leading fields.
  for (; k < def.fields.size(); ++k) {
    size_t found = where.size();
    for (size_t i = 0; i < where.size(); ++i) {
      if (!used[i] && where[i].op == Cond::Op::Eq && where[i].field == def.fields[k].name) {
        found = i;
        break;
      }
    }
    if (found == where.size()) break;
    if (!encode_index_value(def.fields[k].type, def.normalize(k, where[found].value), &prefix)) {
      return std::format("ERR '{}' is not a number (field '{}' is NUMERIC)", where[found].value, def.fields[k].name);
    }
    used[found] = true;
  }
  plan->lower = prefix;
  plan->upper = prefix_end(prefix);
  // Ranges on the next one.
  if (k < def.fields.size()) {
    const auto& field = def.fields[k];
    for (size_t i = 0; i < where.size(); ++i) {
      const Cond& c = where[i];
      if (used[i] || !is_range(c.op) || c.field != field.name) continue;
      std::string bound = prefix;
      bool lower = c.op == Cond::Op::Gt || c.op == Cond::Op::Ge;
      if (field.type == IndexFieldType::Numeric) {
        auto v = parse_double(c.value);
        if (!v) return std::format("ERR '{}' is not a number (field '{}' is NUMERIC)", c.value, field.name);
        uint64_t s = sortable_score(*v);
        bool after = c.op == Cond::Op::Gt || c.op == Cond::Op::Le;  // bound just past the value
        append_be64(bound, s + (after ? 1 : 0));
      } else {
        bound += tag_bound(def.normalize(k, c.value), c.op == Cond::Op::Gt || c.op == Cond::Op::Le);
      }
      if (lower) plan->lower = std::max(plan->lower, bound);
      else plan->upper = std::min(plan->upper, bound);
      used[i] = true;
    }
  }
  for (size_t i = 0; i < where.size(); ++i) {
    if (!used[i]) plan->filters.push_back(&where[i]);
  }
  plan->empty = !plan->upper.empty() && plan->lower >= plan->upper;
  return {};
}

}  // namespace

// ---- helpers

void Database::write_index(std::string key, std::optional<std::string> value) {
  if (PendingBatch* b = active_batch()) {
    stage_index(*b, std::move(key), std::move(value));
    return;
  }
  auto batch = storage_.batch();
  if (value) batch.put(Cf::Index, key, *value);
  else batch.del(Cf::Index, key);
  storage_.write(batch);
}

std::optional<std::string> Database::read_index(std::string_view key) {
  if (PendingBatch* b = active_batch()) {
    if (auto it = b->index.find(key); it != b->index.end()) return it->second;
  }
  std::string value;
  if (!storage_.get(Cf::Index, key, &value)) return std::nullopt;
  return value;
}

std::optional<CollectionMeta> Database::stored_hash_meta(std::string_view key, const RocksStorage::Snapshot* snapshot) {
  std::string raw;
  if (const std::optional<std::string>* p = snapshot ? nullptr : pending(key)) {
    if (!*p) return std::nullopt;
    raw = **p;
  } else if (!storage_.get(Cf::Meta, key, &raw, snapshot)) {
    return std::nullopt;
  }
  RecordView v;
  CollectionMeta m;
  if (!decode_record(raw, &v) || v.type != ValueType::Hash || v.expired(unix_time_ms()) ||
      !decode_collection_meta(v.value, &m) || m.size == 0) {
    return std::nullopt;
  }
  return m;
}

IndexDef::Tuples Database::stored_tuples(const IndexDef& def, std::string_view key, const CollectionMeta& m,
                                         const RocksStorage::Snapshot* snapshot) {
  if (def.json()) return {};
  std::string prefix = data_prefix(key, m.version);
  PendingBatch* b = snapshot ? nullptr : active_batch();
  return def.tuples([&](std::string_view field) -> std::optional<std::string> {
    std::string k = prefix;
    k.append(field);
    if (b) {
      if (auto it = b->data.find(k); it != b->data.end()) return it->second;
    }
    std::string v;
    if (!storage_.get(Cf::Data, k, &v, snapshot)) return std::nullopt;
    return v;
  });
}

std::optional<std::string> Database::live_unique_owner(const IndexDef& def, std::string_view value) {
  if (value.size() < 16) return std::nullopt;
  std::string owner(value.substr(16));
  if (def.json()) {
    // A document's entries are rewritten by its every write: one that still holds a document is live.
    std::string raw;
    if (const std::optional<std::string>* p = pending(owner)) {
      if (!*p) return std::nullopt;
      raw = **p;
    } else if (!storage_.get(Cf::Meta, owner, &raw)) {
      return std::nullopt;
    }
    RecordView v;
    if (!decode_record(raw, &v) || v.type != ValueType::Json || v.expired(unix_time_ms())) return std::nullopt;
    return owner;
  }
  auto m = stored_hash_meta(owner, nullptr);
  if (!m || m->version != read_be64(value)) return std::nullopt;
  return owner;
}

namespace {
// The owner of a UNIQUE entry's value (no liveness check).
std::string_view unique_value_owner(std::string_view value) { return value.size() >= 16 ? value.substr(16) : ""; }
}  // namespace

void Database::drop_index_entries(std::string_view key, const CollectionMeta& m, RocksStorage::Batch* batch) {
  for (const auto& index : indexes_->matching(key)) {
    for (const auto& t : stored_tuples(index->def, key, m)) {
      std::string entry = index->def.entry_key(t, key);
      if (index->def.unique) {
        auto value = read_index(entry);
        if (!value || unique_value_owner(*value) != key) continue;
      }
      if (batch) batch->del(Cf::Index, entry);
      else write_index(std::move(entry), std::nullopt);
    }
  }
}

void Database::refresh_index_entries(std::string_view key, const CollectionMeta& m, int64_t expire_at_ms) {
  for (const auto& index : indexes_->matching(key)) {
    for (const auto& t : stored_tuples(index->def, key, m)) {
      std::string entry = index->def.entry_key(t, key);
      if (index->def.unique) {
        auto value = read_index(entry);
        if (!value || unique_value_owner(*value) != key) continue;
      }
      write_index(std::move(entry), index->def.entry_value(m.version, expire_at_ms, key));
    }
  }
}

// ---- JSON indexes

Database::JsonEntries Database::json_entries(std::string_view key, const std::optional<Record>& rec) {
  JsonEntries out;
  for (auto& index : indexes_->matching(key)) {
    if (index->def.json()) out.indexes.push_back(std::move(index));
  }
  if (out.indexes.empty() || !rec || rec->type != ValueType::Json || rec->expired(unix_time_ms())) return out;
  std::string error;
  auto doc = parse_json(rec->value, &error);
  if (!doc) return out;
  out.expire_at_ms = rec->expire_at_ms;
  for (const auto& index : out.indexes) out.tuples.push_back(index->def.json_tuples(*doc));
  return out;
}

std::vector<std::pair<std::string, std::optional<std::string>>> Database::json_entry_changes(
    std::string_view key, const JsonEntries& before, const JsonEntries& after, std::string_view also_mine) {
  std::vector<std::pair<std::string, std::optional<std::string>>> changes;
  static const IndexDef::Tuples kNone;
  auto tuples_at = [](const JsonEntries& e, size_t i) -> const IndexDef::Tuples& {
    return i < e.tuples.size() ? e.tuples[i] : kNone;
  };
  bool same_ttl = before.expire_at_ms == after.expire_at_ms;
  for (size_t i = 0; i < before.indexes.size(); ++i) {
    const IndexDef& def = before.indexes[i]->def;
    const IndexDef::Tuples &old = tuples_at(before, i), &now = tuples_at(after, i);
    if (old == now && same_ttl) continue;
    for (const auto& t : old) {
      if (contains_tuple(now, t)) continue;
      std::string entry = def.entry_key(t, key);
      bool mine = true;
      if (def.unique) {  // a UNIQUE entry is only ours to delete if it points to this key
        auto value = read_index(entry);
        IndexDef::Entry e;
        mine = value && def.split_entry(entry, *value, &e) && e.owner == key;
      }
      if (mine) changes.emplace_back(std::move(entry), std::nullopt);
    }
    for (const auto& t : now) {
      if (same_ttl && contains_tuple(old, t)) continue;
      std::string put_key = def.entry_key(t, key);
      if (def.unique) {
        if (auto value = read_index(put_key)) {
          auto owner = live_unique_owner(def, *value);
          if (owner && *owner != key && *owner != also_mine) {
            throw IndexConstraintError(
                std::format("ERR UNIQUE index '{}' already has this value, for key '{}'", def.name, *owner));
          }
        }
      }
      changes.emplace_back(std::move(put_key), def.entry_value(0, after.expire_at_ms, key));
    }
  }
  return changes;
}

std::vector<std::unique_lock<std::mutex>> Database::lock_unique(const std::vector<IndexRegistry::Ptr>& indexes) {
  // In id order, like TxnRun::finish, so writers never wait for each other in a circle.
  std::vector<IndexRegistry::Ptr> unique;
  for (const auto& index : indexes) {
    if (index->def.unique) unique.push_back(index);
  }
  std::sort(unique.begin(), unique.end(), [](const auto& a, const auto& b) { return a->def.id < b->def.id; });
  std::vector<std::unique_lock<std::mutex>> locks;
  for (auto& index : unique) locks.emplace_back(index->unique_mu);
  return locks;
}

std::optional<Json> Database::stored_json(std::string_view key, const RocksStorage::Snapshot* snapshot) {
  std::string raw;
  if (!storage_.get(Cf::Meta, key, &raw, snapshot)) return std::nullopt;
  RecordView v;
  if (!decode_record(raw, &v) || v.type != ValueType::Json || v.expired(unix_time_ms())) return std::nullopt;
  std::string error;
  return parse_json(v.value, &error);
}

namespace {

// The values a query sees in a document: an index field's by its name, or
// what a path finds ($.a.b, .a.b, or a top-level member by its name), a
// string as it is and anything else as JSON text.
std::vector<std::string> json_query_values(const IndexDef& def, const Json& doc, std::string_view name) {
  if (int i = def.field_index(name); i >= 0) return def.json_values(static_cast<size_t>(i), doc);
  std::vector<std::string> out;
  std::string text = name.starts_with("$") || name.starts_with(".") ? std::string(name) : "$." + std::string(name);
  std::string error;
  auto path = JsonPath::parse(text, &error);
  if (!path) return out;
  for (const Json* v : path->select(doc)) out.push_back(v->is_string() ? v->get<std::string>() : dump_json(*v));
  return out;
}

}  // namespace

// ---- create / drop / backfill

std::string Database::create_index(IndexDef def, bool scan) {
  IndexRegistry::Ptr index;
  std::string error = indexes_->create(std::move(def), &index);
  if (!error.empty()) return error;
  if (scan) start_backfill(index);
  else indexes_->set_state(index, IndexState::Ready);
  return error;
}

bool Database::drop_index(std::string_view name) { return indexes_->drop(name) != nullptr; }

void Database::start_backfill(IndexRegistry::Ptr index) {
  std::lock_guard lock(backfill_mu_);
  backfills_.emplace_back([this, index] { run_backfill(index); });
}

// Visits every key under the index prefixes and writes its entry. Writes made
// meanwhile maintain the index themselves (it is registered before this
// starts), so the result is complete once the scan ends.
void Database::run_backfill(IndexRegistry::Ptr index) {
  auto stop = [&] { return stopping_.load() || index->dropped.load(); };
  try {
    std::vector<std::string> prefixes = index->def.prefixes;
    if (prefixes.empty()) prefixes.emplace_back();
    for (const auto& p : prefixes) {
      std::string from = p, end = prefix_end(p);
      for (;;) {
        std::vector<std::string> keys;
        {
          auto c = storage_.cursor(Cf::Meta, from, end, false);
          for (c->seek_first(); c->valid() && keys.size() < kBackfillChunk; c->next()) keys.emplace_back(c->key());
          c->check_status();
        }
        if (keys.empty()) break;
        {
          WriteBatchScope batch(*this);
          for (const auto& key : keys) {
            if (stop()) return;
            transact(key, [&](KeyTxn& t) { t.reindex(index); });
            index->scanned.fetch_add(1, std::memory_order_relaxed);
          }
          batch.commit();
        }
        from = keys.back();
        from.push_back('\0');
      }
    }
    if (stop()) return;
    indexes_->set_state(index, IndexState::Ready);
    log_info("index '{}' is ready ({} keys scanned)", index->def.name, index->scanned.load());
  } catch (const IndexConstraintError& e) {
    log_warn("index '{}' failed: {}", index->def.name, e.what());
    indexes_->set_state(index, IndexState::Failed, e.what());
  } catch (const std::exception& e) {
    log_error("index '{}' backfill failed: {}", index->def.name, e.what());
    indexes_->set_state(index, IndexState::Failed, e.what());
  }
}

// ---- queries

IndexQueryResult Database::query_index(const IndexQuery& q) {
  IndexQueryResult result;
  IndexRegistry::Ptr index = indexes_->find(q.index);
  if (!index) {
    result.error = std::format("ERR no such index '{}'", q.index);
    return result;
  }
  if (index->def.search()) {
    result.error = std::format("ERR '{}' is a search index; query it with FT.SEARCH", q.index);
    return result;
  }
  switch (index->state.load()) {
    case IndexState::Ready: break;
    case IndexState::Building:
      result.error = std::format("ERR index '{}' is still being built ({} keys scanned)", q.index,
                                 index->scanned.load());
      return result;
    case IndexState::Failed:
      result.error = std::format("ERR index '{}' failed: {}", q.index, indexes_->error_of(index));
      return result;
  }
  const IndexDef& def = index->def;
  Plan plan;
  result.error = plan_query(def, q.where, &plan);
  if (!result.error.empty() || plan.empty) return result;
  if (!q.count_only && q.limit == 0) return result;  // LIMIT offset 0: an empty page

  // An entry is trusted as it is (its value carries the hash version and
  // TTL), so counting and NOCONTENT read nothing but the index. Hashes are
  // only read for filters and for what is returned; then the entry is also
  // checked against the fields, which costs little more.
  bool all_fields = !q.count_only && !q.no_content && q.return_fields.empty();
  bool read_hashes = all_fields || !plan.filters.empty() || !q.return_fields.empty();
  std::vector<std::string> wanted;
  auto want = [&](std::string_view f) {
    if (std::find(wanted.begin(), wanted.end(), f) == wanted.end()) wanted.emplace_back(f);
  };
  // The hash field behind a name (an index field may be an alias).
  auto hash_field = [&](std::string_view name) {
    int fi = def.field_index(name);
    return fi >= 0 ? def.hash_field(static_cast<size_t>(fi)) : name;
  };
  for (size_t i = 0; i < def.fields.size(); ++i) want(def.hash_field(i));
  for (const Cond* c : plan.filters) want(hash_field(c->field));
  for (const auto& f : q.return_fields) want(hash_field(f));
  int64_t now = unix_time_ms();
  // A key with several tuples may be met more than once in a scan.
  std::optional<std::unordered_set<std::string>> seen;
  if (def.multi_valued()) seen.emplace();

  commit_active_batch();  // this client's own writes must be visible
  std::vector<std::pair<std::string, std::string>> stale;  // owner, entry
  {
    auto snapshot = storage_.snapshot();
    auto cursor = storage_.cursor(Cf::Index, plan.lower, plan.upper, true, snapshot.get());
    q.desc ? cursor->seek_last() : cursor->seek_first();
    int64_t skip = q.offset;
    std::unordered_map<std::string, std::string> values;
    // Counts a key that passed; false when the page is full.
    auto take = [&](IndexHit hit) {
      if (q.count_only) {
        ++result.count;
      } else if (skip > 0) {
        --skip;
      } else {
        result.hits.push_back(std::move(hit));
        if (q.limit >= 0 && static_cast<int64_t>(result.hits.size()) >= q.limit) return false;
      }
      return true;
    };
    for (; cursor->valid(); q.desc ? cursor->prev() : cursor->next()) {
      IndexDef::Entry e;
      if (!def.split_entry(cursor->key(), cursor->value(), &e) || e.expired(now)) continue;
      if (q.key_filter && !q.key_filter(e.owner)) continue;
      if (seen && seen->contains(std::string(e.owner))) continue;
      std::string owner(e.owner);
      if (!read_hashes) {
        if (seen) seen->insert(owner);
        if (!take({std::move(owner), {}})) break;
        continue;
      }
      if (def.json()) {
        auto doc = stored_json(owner, snapshot.get());
        if (!doc || !contains_tuple(def.json_tuples(*doc), e.tuple)) {
          stale.emplace_back(owner, std::string(cursor->key()));
          continue;
        }
        if (seen) seen->insert(owner);
        bool pass = true;
        for (const Cond* c : plan.filters) {
          if (!condition_holds(def, *c, json_query_values(def, *doc, c->field))) {
            pass = false;
            break;
          }
        }
        if (!pass) continue;
        IndexHit hit;
        hit.key = owner;
        if (all_fields) {
          hit.fields.emplace_back("$", dump_json(*doc));
        } else if (!q.no_content && !q.count_only) {
          for (const auto& f : q.return_fields) {
            auto vs = json_query_values(def, *doc, f);
            if (!vs.empty()) hit.fields.emplace_back(f, vs.front());
          }
        }
        if (!take(std::move(hit))) break;
        continue;
      }
      auto m = stored_hash_meta(owner, snapshot.get());
      if (!m || m->version != e.version) {
        stale.emplace_back(owner, std::string(cursor->key()));
        continue;
      }
      values.clear();
      std::vector<std::pair<std::string, std::string>> fields;
      std::string prefix = data_prefix(owner, m->version);
      if (all_fields) {
        auto dc = storage_.cursor(Cf::Data, prefix, data_prefix(owner, m->version + 1), true, snapshot.get());
        for (dc->seek_first(); dc->valid(); dc->next()) {
          fields.emplace_back(dc->key().substr(prefix.size()), dc->value());
          values.emplace(fields.back().first, fields.back().second);
        }
        dc->check_status();
      } else {
        for (const auto& f : wanted) {
          std::string v;
          if (storage_.get(Cf::Data, prefix + f, &v, snapshot.get())) values.emplace(f, std::move(v));
        }
      }
      auto value_of = [&](std::string_view f) -> std::optional<std::string> {
        auto it = values.find(std::string(f));
        return it == values.end() ? std::nullopt : std::optional(it->second);
      };
      if (!contains_tuple(def.tuples(value_of), e.tuple)) {
        stale.emplace_back(owner, std::string(cursor->key()));  // should not happen; repaired anyway
        continue;
      }
      if (seen) seen->insert(owner);
      bool pass = true;
      for (const Cond* c : plan.filters) {
        std::vector<std::string> vs;
        int fi = def.field_index(c->field);
        if (auto v = value_of(hash_field(c->field))) {
          if (fi >= 0) def.split_values(static_cast<size_t>(fi), *v, &vs);
          else vs.push_back(std::move(*v));
        }
        if (!condition_holds(def, *c, vs)) {
          pass = false;
          break;
        }
      }
      if (!pass) continue;
      IndexHit hit;
      hit.key = owner;
      if (all_fields) {
        hit.fields = std::move(fields);
      } else if (!q.no_content && !q.count_only) {
        for (const auto& f : q.return_fields) {
          if (auto v = value_of(hash_field(f))) hit.fields.emplace_back(f, *v);
        }
      }
      if (!take(std::move(hit))) break;
    }
    cursor->check_status();
  }

  // Entries that no longer match their hash are fixed under the key's lock.
  if (stale.size() > kMaxRepairsPerQuery) stale.resize(kMaxRepairsPerQuery);
  for (auto& [owner, entry] : stale) {
    try {
      transact(owner, [&](KeyTxn& t) { t.repair(index, entry); });
    } catch (const IndexConstraintError&) {
      // The hash now conflicts with another key; leave it for its next write.
    }
  }
  return result;
}

}  // namespace datadam
