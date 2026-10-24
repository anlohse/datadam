#include "db/change_feed.h"

#include <optional>
#include <unordered_map>

#include "storage/data_keys.h"
#include "storage/rocks_storage.h"
#include "util/strings.h"

namespace datadam {

namespace {

// Note layout: 'C' | u8 format (1) | be64 time_ms | then, per collection
// version: be32 prefix length | data prefix | u8 type.
constexpr char kNoteTag = 'C';
constexpr char kNoteFormat = 1;

void append_be32(std::string& out, uint32_t v) {
  for (int i = 3; i >= 0; --i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

uint32_t read_be32(std::string_view s) {
  uint32_t v = 0;
  for (size_t i = 0; i < 4; ++i) v = (v << 8) | static_cast<uint8_t>(s[i]);
  return v;
}

// The data prefix of a data key: everything up to and including the version.
std::string_view prefix_of(std::string_view data_key, std::string_view key) {
  return data_key.substr(0, 4 + key.size() + 8);
}

class Decoder : public WalVisitor {
 public:
  Decoder(size_t max_changes, size_t max_batches, const std::function<bool(std::string_view)>& keep)
      : max_changes_(max_changes), max_batches_(max_batches), keep_(keep) {}

  bool begin(uint64_t seq, uint64_t count) override {
    if (collected_ >= max_changes_ || looked_ >= max_batches_) return false;
    ++looked_;
    cur_ = ChangeBatch{};
    cur_.seq = seq;
    next_ = seq + count;
    types_.clear();
    elements_.clear();
    dropped_.clear();
    flushall_ = false;
    return true;
  }

  void log_data(std::string_view blob) override {
    if (blob.size() < 10 || blob[0] != kNoteTag || blob[1] != kNoteFormat) return;
    cur_.time_ms = static_cast<int64_t>(read_be64(blob.substr(2)));
    std::string_view rest = blob.substr(10);
    while (rest.size() >= 4) {
      uint32_t len = read_be32(rest);
      if (rest.size() < 4 + static_cast<size_t>(len) + 1) break;
      types_[std::string(rest.substr(4, len))] = static_cast<ValueType>(static_cast<uint8_t>(rest[4 + len]));
      rest.remove_prefix(4 + len + 1);
    }
  }

  void put(Cf cf, std::string_view key, std::string_view value) override {
    if (cf == Cf::Meta) return meta_put(key, value);
    if (cf == Cf::Data) element(key, value, true);
  }

  void del(Cf cf, std::string_view key) override {
    if (cf == Cf::Meta) return add(Change{"del", std::string(key), {}});
    if (cf == Cf::Data) element(key, {}, false);
  }

  void delete_range(Cf cf, std::string_view begin, std::string_view) override {
    if (cf == Cf::Meta) flushall_ = true;  // FLUSHALL is the only range delete of records
    // A dead collection version: its element writes in this batch never show.
    if (cf == Cf::Data) dropped_.emplace_back(begin);
  }

  void end() override {
    if (flushall_) {  // whatever else the batch removes is part of it
      cur_.changes = {Change{"flushall", "", {}}};
      elements_.clear();
    }
    // Elements are decoded last: the note with their types may come anywhere in the batch.
    for (const Element& e : elements_) {
      bool dead = false;
      for (const auto& d : dropped_) dead = dead || d == e.prefix;
      if (dead) continue;
      if (auto c = decode(e)) add(std::move(*c));
    }
    if (!cur_.changes.empty()) {
      collected_ += cur_.changes.size();
      out_.batches.push_back(std::move(cur_));
    }
  }

  ChangeRead result(uint64_t from, uint64_t latest) {
    out_.next = looked_ > 0 ? next_ : (from == 0 ? latest + 1 : from);
    return std::move(out_);
  }

 private:
  void add(Change c) {
    if (c.op != "flushall" && !keep_(c.key)) return;
    cur_.changes.push_back(std::move(c));
  }

  void meta_put(std::string_view key, std::string_view raw) {
    RecordView v;
    if (!decode_record(raw, &v)) return;
    if (!is_collection(v.type)) {
      std::string op = v.type == ValueType::Json ? "json" : "set";
      return add(Change{op, std::string(key), {std::string(v.value), std::to_string(v.expire_at_ms)}});
    }
    CollectionMeta m;
    if (!decode_collection_meta(v.value, &m)) return;
    Change c{"meta", std::string(key),
             {std::string(value_type_name(v.type)), std::to_string(m.size), std::to_string(v.expire_at_ms),
              std::to_string(m.version)}};
    if (v.type == ValueType::List) c.args.push_back(std::to_string(position(m.head)));
    add(std::move(c));
  }

  static int64_t position(uint64_t list_index) {
    return static_cast<int64_t>(list_index - CollectionMeta::kListStart);
  }

  struct Element {
    std::string prefix, key;
    uint64_t version;
    std::string sub, value;
    bool put;
  };

  void element(std::string_view data_key, std::string_view value, bool put) {
    std::string_view key, sub;
    uint64_t version = 0;
    if (!decode_data_key(data_key, &key, &version, &sub)) return;
    elements_.push_back(Element{std::string(prefix_of(data_key, key)), std::string(key), version, std::string(sub),
                                std::string(value), put});
  }

  std::optional<Change> decode(const Element& e) const {
    std::string_view sub = e.sub, value = e.value;
    bool put = e.put;
    uint64_t version = e.version;
    auto it = types_.find(e.prefix);
    Change c{"", e.key, {}};
    if (it == types_.end()) {
      c.op = put ? "data" : "datadel";
      c.args = {std::to_string(version), std::string(sub)};
      if (put) c.args.emplace_back(value);
    } else {
      switch (it->second) {
        case ValueType::Hash:
          c.op = put ? "hset" : "hdel";
          c.args = {std::string(sub)};
          if (put) c.args.emplace_back(value);
          break;
        case ValueType::Set:
          c.op = put ? "sadd" : "srem";
          c.args = {std::string(sub)};
          break;
        case ValueType::ZSet:
          if (sub.empty() || sub[0] != 'm') return std::nullopt;  // the score-ordered twin entry
          if (put) {
            if (value.size() != 8) return std::nullopt;
            c.op = "zadd";
            c.args = {format_double(score_from_sortable(read_be64(value))), std::string(sub.substr(1))};
          } else {
            c.op = "zrem";
            c.args = {std::string(sub.substr(1))};
          }
          break;
        case ValueType::List:
          if (sub.size() != 8) return std::nullopt;
          c.op = put ? "lset" : "ldel";
          c.args = {std::to_string(position(read_be64(sub)))};
          if (put) c.args.emplace_back(value);
          break;
        case ValueType::String:
        case ValueType::Json: return std::nullopt;
      }
    }
    return c;
  }

  size_t max_changes_, max_batches_;
  const std::function<bool(std::string_view)>& keep_;
  size_t collected_ = 0;
  size_t looked_ = 0;
  uint64_t next_ = 0;
  ChangeBatch cur_;
  std::unordered_map<std::string, ValueType> types_;
  std::vector<Element> elements_;
  std::vector<std::string> dropped_;
  bool flushall_ = false;
  ChangeRead out_;
};

}  // namespace

ChangeRead read_changes(const RocksStorage& storage, uint64_t from, size_t max_changes, size_t max_batches,
                        const std::function<bool(std::string_view key)>& keep) {
  uint64_t latest = storage.latest_sequence();  // before reading: nothing written meanwhile is skipped
  Decoder d(max_changes, max_batches, keep);
  storage.read_wal(from, d);
  return d.result(from, latest);
}

std::string change_note(int64_t time_ms) {
  std::string note;
  note.push_back(kNoteTag);
  note.push_back(kNoteFormat);
  append_be64(note, static_cast<uint64_t>(time_ms));
  return note;
}

void add_change_note_type(std::string& note, std::string_view data_prefix, ValueType type) {
  append_be32(note, static_cast<uint32_t>(data_prefix.size()));
  note.append(data_prefix);
  note.push_back(static_cast<char>(type));
}

}  // namespace datadam
