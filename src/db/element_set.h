#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace datadam {

// All elements of a small collection, sorted by sub-key: the encoded form kept
// in the object cache, read in place, plus the changes made since it was
// loaded. Loading costs one pass over the bytes and no allocation per
// element, which matters because a hot collection is loaded by every command.
//
// Encoding: u32 count, then per element u32 length + sub-key + u32 length + value.
class ElementSet {
 public:
  // Takes a buffer holding an encoding at `start`; false (and an empty set)
  // if it is malformed.
  bool load(std::string buffer, size_t start = 0);
  void clear();

  // Appends an element greater than all the others (to build a set in order).
  void append(std::string_view sub, std::string_view value);

  size_t size() const { return count_; }
  std::optional<std::string_view> get(std::string_view sub) const;
  void put(std::string_view sub, std::string_view value);
  void erase(std::string_view sub);

  // Visits [from, to) (empty `to`: no bound), in order or reversed, until fn
  // returns false. The set must not change meanwhile.
  template <class Fn>
  void for_range(std::string_view from, std::string_view to, bool reverse, Fn&& fn) const;

  std::string encode() const;

 private:
  struct Entry {
    std::string_view sub, value;
  };
  Entry entry(size_t i) const;
  // First base entry with sub >= s.
  size_t lower_bound(std::string_view s) const;
  bool base_has(std::string_view sub) const;

  std::string base_;
  size_t start_ = 0;               // where the encoding starts in base_
  std::vector<uint32_t> offsets_;  // start of each base entry
  // Changes since load: a value, or nullopt for a removal.
  std::map<std::string, std::optional<std::string>, std::less<>> changes_;
  size_t count_ = 0;
};

template <class Fn>
void ElementSet::for_range(std::string_view from, std::string_view to, bool reverse, Fn&& fn) const {
  size_t b_first = lower_bound(from), b_last = to.empty() ? offsets_.size() : lower_bound(to);
  auto c_first = changes_.lower_bound(from);
  auto c_last = to.empty() ? changes_.end() : changes_.lower_bound(to);
  if (b_first > b_last) b_last = b_first;
  // Merge the base with the changes; a change hides the base entry of the same sub-key.
  if (!reverse) {
    size_t b = b_first;
    auto c = c_first;
    for (;;) {
      bool has_b = b < b_last, has_c = c != c_last;
      if (!has_b && !has_c) return;
      if (has_c && (!has_b || std::string_view(c->first) <= entry(b).sub)) {
        if (has_b && std::string_view(c->first) == entry(b).sub) ++b;
        if (c->second && !fn(std::string_view(c->first), std::string_view(*c->second))) return;
        ++c;
      } else {
        Entry e = entry(b++);
        if (!fn(e.sub, e.value)) return;
      }
    }
  } else {
    size_t b = b_last;
    auto c = c_last;
    for (;;) {
      bool has_b = b > b_first, has_c = c != c_first;
      if (!has_b && !has_c) return;
      auto pc = has_c ? std::prev(c) : c;
      if (has_c && (!has_b || std::string_view(pc->first) >= entry(b - 1).sub)) {
        if (has_b && std::string_view(pc->first) == entry(b - 1).sub) --b;
        if (pc->second && !fn(std::string_view(pc->first), std::string_view(*pc->second))) return;
        c = pc;
      } else {
        Entry e = entry(--b);
        if (!fn(e.sub, e.value)) return;
      }
    }
  }
}

}  // namespace datadam
