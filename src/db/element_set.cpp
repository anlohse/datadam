#include "db/element_set.h"

namespace datadam {

namespace {

uint32_t read_u32(const char* p) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(static_cast<uint8_t>(p[i])) << (8 * i);
  return v;
}

void append_u32(std::string& out, uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

void append_entry(std::string& out, std::string_view sub, std::string_view value) {
  append_u32(out, static_cast<uint32_t>(sub.size()));
  out.append(sub);
  append_u32(out, static_cast<uint32_t>(value.size()));
  out.append(value);
}

}  // namespace

bool ElementSet::load(std::string buffer, size_t start) {
  clear();
  if (buffer.size() < start + 4) return buffer.size() == start;  // nothing: an empty set
  base_ = std::move(buffer);
  start_ = start;
  uint32_t n = read_u32(base_.data() + start_);
  offsets_.reserve(n);
  size_t pos = start_ + 4;
  for (uint32_t i = 0; i < n; ++i) {
    offsets_.push_back(static_cast<uint32_t>(pos));
    for (int part = 0; part < 2; ++part) {
      if (pos + 4 > base_.size()) {
        clear();
        return false;
      }
      pos += 4 + read_u32(base_.data() + pos);
    }
    if (pos > base_.size()) {
      clear();
      return false;
    }
  }
  count_ = n;
  return true;
}

void ElementSet::clear() {
  base_.clear();
  start_ = 0;
  offsets_.clear();
  changes_.clear();
  count_ = 0;
}

void ElementSet::append(std::string_view sub, std::string_view value) {
  if (base_.empty()) base_.assign(start_ + 4, '\0');
  offsets_.push_back(static_cast<uint32_t>(base_.size()));
  append_entry(base_, sub, value);
  ++count_;
  // Keep the count prefix in step, so base_ is always a valid encoding.
  uint32_t n = static_cast<uint32_t>(offsets_.size());
  for (size_t i = 0; i < 4; ++i) base_[start_ + i] = static_cast<char>((n >> (8 * i)) & 0xFF);
}

ElementSet::Entry ElementSet::entry(size_t i) const {
  const char* p = base_.data() + offsets_[i];
  uint32_t klen = read_u32(p);
  std::string_view sub(p + 4, klen);
  uint32_t vlen = read_u32(p + 4 + klen);
  return {sub, std::string_view(p + 8 + klen, vlen)};
}

size_t ElementSet::lower_bound(std::string_view s) const {
  size_t lo = 0, hi = offsets_.size();
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (entry(mid).sub < s) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}

bool ElementSet::base_has(std::string_view sub) const {
  size_t i = lower_bound(sub);
  return i < offsets_.size() && entry(i).sub == sub;
}

std::optional<std::string_view> ElementSet::get(std::string_view sub) const {
  if (auto it = changes_.find(sub); it != changes_.end()) {
    if (!it->second) return std::nullopt;
    return std::string_view(*it->second);
  }
  size_t i = lower_bound(sub);
  if (i < offsets_.size()) {
    Entry e = entry(i);
    if (e.sub == sub) return e.value;
  }
  return std::nullopt;
}

void ElementSet::put(std::string_view sub, std::string_view value) {
  if (!get(sub)) ++count_;
  auto it = changes_.find(sub);
  if (it == changes_.end()) changes_.emplace(std::string(sub), std::string(value));
  else it->second = std::string(value);
}

void ElementSet::erase(std::string_view sub) {
  if (!get(sub)) return;
  --count_;
  if (!base_has(sub)) {
    changes_.erase(changes_.find(sub));
    return;
  }
  auto it = changes_.find(sub);
  if (it == changes_.end()) changes_.emplace(std::string(sub), std::nullopt);
  else it->second.reset();
}

std::string ElementSet::encode() const {
  if (changes_.empty() && !base_.empty()) return base_.substr(start_);
  std::string out;
  out.reserve(base_.size() + 64);
  append_u32(out, static_cast<uint32_t>(count_));
  for_range("", "", false, [&](std::string_view sub, std::string_view value) {
    append_entry(out, sub, value);
    return true;
  });
  return out;
}

}  // namespace datadam
