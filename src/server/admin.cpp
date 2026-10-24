#include "server/admin.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <format>

#include "server/pubsub.h"
#include "util/clock.h"
#include "util/log.h"
#include "util/strings.h"

namespace datadam {

namespace fs = std::filesystem;

namespace {

// yyyymmdd-hhmmss, UTC.
std::string timestamp_name(int64_t unix_s) {
  std::time_t t = static_cast<std::time_t>(unix_s);
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  return std::format("{:04}{:02}{:02}-{:02}{:02}{:02}", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                     tm.tm_min, tm.tm_sec);
}

// A directory name timestamp_name() made (possibly with a -n suffix).
bool is_backup_name(const std::string& n) {
  if (n.size() < 15 || n[8] != '-') return false;
  for (size_t i = 0; i < n.size(); ++i) {
    if (i == 8) continue;
    if (i == 15 && n[i] == '-') continue;
    if (!std::isdigit(static_cast<unsigned char>(n[i]))) return false;
  }
  return true;
}

}  // namespace

// ---- Backups

Backups::Backups(RocksStorage& storage, int64_t start_time_s) : storage_(storage) {
  status_.last_save_s = start_time_s;  // as Redis: LASTSAVE starts at the start time
}

Backups::~Backups() {
  if (worker_.joinable()) worker_.join();
}

std::string Backups::backup(const std::string& path) { return checkpoint(path, false); }

std::string Backups::checkpoint(const std::string& path, bool background) {
  std::error_code ec;
  if (path.empty()) return "ERR the backup needs a path";
  if (fs::exists(path, ec)) {
    if (background) finish(false, path, "target exists", 0, true);
    return std::format("ERR backup target '{}' already exists", path);
  }
  fs::path parent = fs::path(path).parent_path();
  if (!parent.empty()) fs::create_directories(parent, ec);
  auto start = std::chrono::steady_clock::now();
  try {
    storage_.checkpoint(path);
  } catch (const std::exception& e) {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    finish(false, path, e.what(), ms.count(), background);
    return std::format("ERR backup failed: {}", e.what());
  }
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
  finish(true, path, {}, ms.count(), false);
  log_info("backup written to {} in {} ms", path, ms.count());
  return {};
}

bool Backups::start(const std::string& dir, unsigned keep) {
  std::lock_guard lock(mu_);
  if (status_.in_progress) return false;
  if (worker_.joinable()) worker_.join();  // the previous one is done
  status_.in_progress = true;
  worker_ = std::thread([this, dir, keep] {
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::string base = timestamp_name(unix_time_ms() / 1000), name = base;
    for (int n = 2; fs::exists(fs::path(dir) / name, ec); ++n) name = std::format("{}-{}", base, n);
    std::string error = checkpoint((fs::path(dir) / name).string(), true);
    if (!error.empty()) {
      log_error("BGSAVE: {}", error);
      return;  // finish() recorded it, and that it is over
    }
    auto done = [this] {
      std::lock_guard lock(mu_);
      status_.in_progress = false;
    };
    if (keep == 0) return done();
    // The oldest go first: the names sort by time.
    std::vector<std::string> names;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
      std::string n = e.path().filename().string();
      if (e.is_directory(ec) && is_backup_name(n)) names.push_back(n);
    }
    std::sort(names.begin(), names.end());
    for (size_t i = 0; i + keep < names.size(); ++i) {
      fs::remove_all(fs::path(dir) / names[i], ec);
      if (ec) log_warn("BGSAVE: could not remove the old backup {}: {}", names[i], ec.message());
    }
    done();
  });
  return true;
}

void Backups::saved() {
  std::lock_guard lock(mu_);
  status_.last_save_s = unix_time_ms() / 1000;
  status_.last_ok = true;
}

void Backups::finish(bool ok, std::string path, std::string error, int64_t duration_ms, bool background) {
  std::lock_guard lock(mu_);
  if (background) status_.in_progress = false;  // a failed BGSAVE is over
  status_.last_ok = ok;
  status_.last_duration_ms = duration_ms;
  status_.last_error = std::move(error);
  if (ok) {
    status_.last_save_s = unix_time_ms() / 1000;
    status_.last_path = std::move(path);
  }
}

Backups::Status Backups::status() const {
  std::lock_guard lock(mu_);
  return status_;
}

// ---- SlowLog

std::vector<std::string> loggable_args(std::span<const std::string_view> args) {
  constexpr size_t kMaxArgs = 32, kMaxArgLen = 128;
  std::vector<std::string> out;
  if (args.empty()) return out;
  std::string name = to_lower(args[0]);
  // Commands that carry passwords show their name (and subcommand) only.
  size_t shown = args.size();
  if (name == "auth" || name == "hello" || name == "migrate") shown = 1;
  else if (name == "acl" && args.size() > 1 && iequals(args[1], "setuser")) shown = std::min<size_t>(args.size(), 3);
  else if (name == "config" && args.size() > 1 && iequals(args[1], "set")) shown = 2;
  for (size_t i = 0; i < shown && i < args.size(); ++i) {
    if (out.size() + 1 == kMaxArgs && args.size() > kMaxArgs) {
      out.push_back(std::format("... ({} more arguments)", args.size() - out.size()));
      return out;
    }
    std::string_view a = args[i];
    if (a.size() > kMaxArgLen) {
      out.push_back(std::format("{}... ({} more bytes)", a.substr(0, kMaxArgLen), a.size() - kMaxArgLen));
    } else {
      out.emplace_back(a);
    }
  }
  if (shown < args.size()) out.emplace_back("(redacted)");
  return out;
}

void SlowLog::configure(int64_t slower_than_us, size_t max_len) {
  slower_than_.store(slower_than_us, std::memory_order_relaxed);
  max_len_.store(max_len, std::memory_order_relaxed);
  std::lock_guard lock(mu_);
  while (entries_.size() > max_len) entries_.pop_back();
}

void SlowLog::add(std::span<const std::string_view> args, int64_t us, std::string addr, std::string name) {
  Entry e{0, unix_time_ms() / 1000, us, loggable_args(args), std::move(addr), std::move(name)};
  std::lock_guard lock(mu_);
  size_t max = max_len_.load(std::memory_order_relaxed);
  if (max == 0) return;
  e.id = next_id_++;
  entries_.push_front(std::move(e));
  while (entries_.size() > max) entries_.pop_back();
}

std::vector<SlowLog::Entry> SlowLog::get(size_t count) const {
  std::lock_guard lock(mu_);
  std::vector<Entry> out(entries_.begin(), entries_.begin() + static_cast<std::ptrdiff_t>(std::min(count, entries_.size())));
  return out;
}

size_t SlowLog::len() const {
  std::lock_guard lock(mu_);
  return entries_.size();
}

void SlowLog::reset() {
  std::lock_guard lock(mu_);
  entries_.clear();
}

// ---- clients

std::string ClientEntry::describe() const {
  int64_t now = steady_time_ms();
  std::string flags;
  if (replica.load()) flags += 'S';
  if (monitor.load()) flags += 'O';
  if (pubsub.load()) flags += 'P';
  if (blocked.load()) flags += 'b';
  if (flags.empty()) flags = "N";
  const std::string* cmd = last_cmd.load();
  std::lock_guard lock(mu);
  return std::format(
      "id={} addr={} name={} age={} idle={} flags={} db=0 sub={} psub={} ssub={} multi=-1 cmd={} user={} resp={} "
      "lib-name={} lib-ver={} tls={}",
      id, addr, name, (now - created_ms) / 1000, (now - last_ms.load()) / 1000, flags, subscriptions.load(),
      patterns.load(), shard_subscriptions.load(), cmd ? *cmd : std::string("NULL"), user, proto, lib_name, lib_ver,
      tls ? "yes" : "no");
}

std::shared_ptr<ClientEntry> ClientRegistry::add(uint64_t id) {
  auto e = std::make_shared<ClientEntry>();
  e->id = id;
  e->created_ms = e->last_ms = steady_time_ms();
  std::lock_guard lock(mu_);
  entries_[id] = e;
  return e;
}

void ClientRegistry::remove(uint64_t id) {
  std::lock_guard lock(mu_);
  entries_.erase(id);
}

std::vector<std::shared_ptr<ClientEntry>> ClientRegistry::all() const {
  std::lock_guard lock(mu_);
  std::vector<std::shared_ptr<ClientEntry>> out;
  for (const auto& [id, e] : entries_) out.push_back(e);
  return out;
}

void ClientRegistry::clear() {
  std::lock_guard lock(mu_);
  for (auto& [id, e] : entries_) e->kill = nullptr;
  entries_.clear();
}

size_t ClientRegistry::size() const {
  std::lock_guard lock(mu_);
  return entries_.size();
}

// ---- MONITOR

void Monitors::add(uint64_t client_id, std::shared_ptr<Subscriber> s) {
  std::lock_guard lock(mu_);
  watchers_.push_back({client_id, std::move(s)});
  count_.store(watchers_.size(), std::memory_order_relaxed);
}

namespace {

// An argument as MONITOR shows it: quoted, with escapes for quotes,
// backslashes and unprintable bytes.
void append_quoted(std::string& out, std::string_view a) {
  out.push_back('"');
  for (unsigned char c : a) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20 || c >= 0x7f) out += std::format("\\x{:02x}", c);
        else out.push_back(static_cast<char>(c));
    }
  }
  out.push_back('"');
}

}  // namespace

void Monitors::feed(uint64_t from_id, std::string_view addr, std::span<const std::string_view> args) {
  int64_t us = std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
                   .count();
  std::string line = std::format("+{}.{:06} [0 {}]", us / 1000000, us % 1000000, addr);
  for (const auto& a : loggable_args(args)) {
    line.push_back(' ');
    append_quoted(line, a);
  }
  line += "\r\n";
  std::lock_guard lock(mu_);
  std::erase_if(watchers_, [](const Watcher& w) { return w.sub.expired(); });
  count_.store(watchers_.size(), std::memory_order_relaxed);
  for (const auto& w : watchers_) {
    if (w.id == from_id) continue;
    if (auto s = w.sub.lock()) s->push(line);
  }
}

}  // namespace datadam
