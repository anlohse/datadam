// Search indexes (FT.*) on the Database side: a query is worked out as set
// operations on the keys that scans of its fields find, all from the index;
// documents are only read for the page that is returned.

#include <algorithm>
#include <cmath>
#include <unordered_set>

#include "db/database.h"
#include "storage/data_keys.h"
#include "util/clock.h"

namespace datadam {

namespace {

using Keys = std::vector<std::string>;  // sorted, without duplicates

Keys unite(const Keys& a, const Keys& b) {
  Keys out;
  std::set_union(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(out));
  return out;
}

Keys subtract(const Keys& a, const Keys& b) {
  Keys out;
  std::set_difference(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(out));
  return out;
}

void sort_unique(Keys* keys) {
  std::sort(keys->begin(), keys->end());
  keys->erase(std::unique(keys->begin(), keys->end()), keys->end());
}

// Scans of one search index at one snapshot.
class SearchScanner {
 public:
  SearchScanner(RocksStorage& storage, const RocksStorage::Snapshot* snapshot, const IndexDef& def)
      : storage_(storage), snapshot_(snapshot), def_(def), now_(unix_time_ms()) {}

  // Visits the live entries in [lower, upper) until fn returns false.
  template <class F>
  void scan(const std::string& lower, const std::string& upper, bool reverse, F&& fn) {
    if (!upper.empty() && lower >= upper) return;
    auto c = storage_.cursor(Cf::Index, lower, upper, true, snapshot_);
    for (reverse ? c->seek_last() : c->seek_first(); c->valid(); reverse ? c->prev() : c->next()) {
      IndexDef::Entry e;
      if (!def_.split_entry(c->key(), c->value(), &e) || e.expired(now_)) continue;
      if (!fn(e)) break;
    }
    c->check_status();
  }

  // The owners of the entries in [lower, upper); only those in `within`, if given.
  void owners(const std::string& lower, const std::string& upper, Keys* out, const Keys* within = nullptr) {
    scan(lower, upper, false, [&](const IndexDef::Entry& e) {
      if (!within || std::binary_search(within->begin(), within->end(), e.owner, std::less<>())) {
        out->emplace_back(e.owner);
      }
      return true;
    });
  }

  Keys all() {
    // Presence entries are id | kPresence | owner: already in key order.
    Keys out;
    std::string p = def_.presence_prefix();
    owners(p, prefix_end(p), &out);
    return out;
  }

  // The keys that match n; with `within`, only those among them.
  Keys eval(const SearchNode& n, const Keys* within = nullptr) {
    using Kind = SearchNode::Kind;
    Keys out;
    switch (n.kind) {
      case Kind::All: return within ? *within : all();
      case Kind::Tag: {
        std::string fp = def_.field_prefix(n.field);
        for (size_t i = 0; i < n.values.size(); ++i) {
          std::string p = fp;
          if (n.prefix[i]) p += tag_prefix(n.values[i]);
          else encode_index_value(IndexFieldType::Tag, n.values[i], &p);
          owners(p, prefix_end(p), &out, within);
        }
        sort_unique(&out);
        return out;
      }
      case Kind::Range: {
        std::string fp = def_.field_prefix(n.field), lower = fp, upper;
        if (n.lo != -INFINITY || n.lo_open) append_be64(lower, sortable_score(n.lo) + (n.lo_open ? 1 : 0));
        if (n.hi == INFINITY && !n.hi_open) {
          upper = prefix_end(fp);
        } else {
          upper = fp;
          append_be64(upper, sortable_score(n.hi) + (n.hi_open ? 0 : 1));
        }
        owners(lower, upper, &out, within);
        sort_unique(&out);
        return out;
      }
      case Kind::Or:
        for (const auto& c : n.children) out = unite(out, eval(c, within));
        return out;
      case Kind::Not: {
        Keys base = within ? *within : all();
        return subtract(base, eval(n.children.front(), &base));
      }
      case Kind::And: {
        // The positive clauses, TAG first (usually the most selective): each
        // one after the first only scans for keys still in the result. Then
        // the negated ones are taken away.
        std::vector<const SearchNode*> positive;
        for (const auto& c : n.children) {
          if (c.kind != Kind::Not) positive.push_back(&c);
        }
        std::stable_sort(positive.begin(), positive.end(), [](const SearchNode* a, const SearchNode* b) {
          return (a->kind == Kind::Tag) > (b->kind == Kind::Tag);
        });
        bool first = true;
        for (const SearchNode* c : positive) {
          out = first ? eval(*c, within) : eval(*c, &out);
          first = false;
          if (out.empty()) return out;
        }
        if (first) out = within ? *within : all();
        for (const auto& c : n.children) {
          if (c.kind == Kind::Not && !out.empty()) out = subtract(out, eval(c.children.front(), &out));
        }
        return out;
      }
    }
    return out;
  }

 private:
  RocksStorage& storage_;
  const RocksStorage::Snapshot* snapshot_;
  const IndexDef& def_;
  int64_t now_;
};

// What RETURN shows for a name in a document: an index field's path, or the
// name as a path; a string as it is, anything else as JSON text.
std::optional<std::string> json_return_value(const IndexDef& def, const Json& doc, const std::string& name) {
  int fi = def.field_index(name);
  std::string text = fi >= 0 ? def.fields[static_cast<size_t>(fi)].path
                     : name.starts_with("$") || name.starts_with(".") ? name
                                                                      : "$." + name;
  std::string error;
  auto path = JsonPath::parse(text, &error);
  if (!path) return std::nullopt;
  auto found = path->select(doc);
  if (found.empty()) return std::nullopt;
  return found.front()->is_string() ? found.front()->get<std::string>() : dump_json(*found.front());
}

}  // namespace

SearchResult Database::search(const IndexRegistry::Ptr& index, const SearchNode& query, const SearchOptions& opts) {
  const IndexDef& def = index->def;
  SearchResult result;
  size_t want = static_cast<size_t>(std::max<int64_t>(opts.offset, 0) + std::max<int64_t>(opts.limit, 0));

  commit_active_batch();  // this client's own writes must be visible
  auto snapshot = storage_.snapshot();
  SearchScanner scanner(storage_, snapshot.get(), def);

  Keys page;
  if (query.kind == SearchNode::Kind::All && !opts.sort_field) {
    // Every document: counted from the presence entries, nothing kept but the page.
    std::string p = def.presence_prefix();
    scanner.scan(p, prefix_end(p), false, [&](const IndexDef::Entry& e) {
      if (opts.key_filter && !opts.key_filter(e.owner)) return true;
      if (result.total++ >= static_cast<uint64_t>(opts.offset) && page.size() < static_cast<size_t>(opts.limit)) {
        page.emplace_back(e.owner);
      }
      return true;
    });
  } else {
    Keys keys = scanner.eval(query);
    if (opts.key_filter) std::erase_if(keys, [&](const std::string& k) { return !opts.key_filter(k); });
    result.total = keys.size();
    Keys ordered;
    if (opts.sort_field && want > 0) {
      // In the order of the sort field's entries; keys without the field last.
      std::unordered_set<std::string> taken;
      std::string fp = def.field_prefix(*opts.sort_field);
      scanner.scan(fp, prefix_end(fp), opts.sort_desc, [&](const IndexDef::Entry& e) {
        std::string owner(e.owner);
        if (std::binary_search(keys.begin(), keys.end(), owner) && taken.insert(owner).second) {
          ordered.push_back(std::move(owner));
        }
        return ordered.size() < want;
      });
      for (size_t i = 0; i < keys.size() && ordered.size() < want; ++i) {
        if (!taken.contains(keys[i])) ordered.push_back(keys[i]);
      }
    } else {
      if (keys.size() > want) keys.resize(want);
      ordered = std::move(keys);
    }
    for (size_t i = static_cast<size_t>(std::max<int64_t>(opts.offset, 0)); i < ordered.size(); ++i) {
      page.push_back(std::move(ordered[i]));
    }
  }

  for (auto& key : page) {
    IndexHit hit;
    hit.key = std::move(key);
    if (opts.no_content) {
      result.hits.push_back(std::move(hit));
      continue;
    }
    if (def.json()) {
      auto doc = stored_json(hit.key, snapshot.get());
      if (!doc) continue;  // gone since the scan
      if (opts.return_fields.empty()) {
        hit.fields.emplace_back("$", dump_json(*doc));
      } else {
        for (const auto& [name, as] : opts.return_fields) {
          if (auto v = json_return_value(def, *doc, name)) hit.fields.emplace_back(as, std::move(*v));
        }
      }
    } else {
      auto m = stored_hash_meta(hit.key, snapshot.get());
      if (!m) continue;
      std::string prefix = data_prefix(hit.key, m->version);
      if (opts.return_fields.empty()) {
        auto dc = storage_.cursor(Cf::Data, prefix, data_prefix(hit.key, m->version + 1), true, snapshot.get());
        for (dc->seek_first(); dc->valid(); dc->next()) {
          hit.fields.emplace_back(dc->key().substr(prefix.size()), dc->value());
        }
        dc->check_status();
      } else {
        for (const auto& [name, as] : opts.return_fields) {
          int fi = def.field_index(name);
          std::string field = fi >= 0 ? std::string(def.hash_field(static_cast<size_t>(fi))) : name;
          std::string v;
          if (storage_.get(Cf::Data, prefix + field, &v, snapshot.get())) hit.fields.emplace_back(as, std::move(v));
        }
      }
    }
    result.hits.push_back(std::move(hit));
  }
  return result;
}

void Database::count_search_entries(const IndexRegistry::Ptr& index, uint64_t* docs, uint64_t* records) {
  *docs = *records = 0;
  auto snapshot = storage_.snapshot();
  SearchScanner scanner(storage_, snapshot.get(), index->def);
  std::string p = index->def.id_prefix();
  scanner.scan(p, prefix_end(p), false, [&](const IndexDef::Entry& e) {
    ++*records;
    if (e.field < 0) ++*docs;
    return true;
  });
}

std::vector<std::string> Database::tag_values(const IndexRegistry::Ptr& index, size_t field) {
  std::vector<std::string> out;
  auto snapshot = storage_.snapshot();
  SearchScanner scanner(storage_, snapshot.get(), index->def);
  std::string p = index->def.field_prefix(field), value;
  scanner.scan(p, prefix_end(p), false, [&](const IndexDef::Entry& e) {
    if (decode_tag(e.tuple.substr(1), &value) && (out.empty() || out.back() != value)) out.push_back(value);
    return true;
  });
  return out;
}

std::vector<std::string> Database::search_keys(const IndexRegistry::Ptr& index) {
  commit_active_batch();
  auto snapshot = storage_.snapshot();
  return SearchScanner(storage_, snapshot.get(), index->def).all();
}

}  // namespace datadam
