#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <stdexcept>

#include <asio/ip/address.hpp>

#include "server/redis_commands.h"
#include "util/log.h"
#include "util/strings.h"

namespace datadam {

namespace {

// Settings CONFIG SET can change; every other one is fixed at startup.
const std::set<std::string, std::less<>>& runtime_settings() {
  static const std::set<std::string, std::less<>> keys = {
      "log-level",           "write-batching",       "fsync",          "cache-min-memory",
      "cache-max-idle",      "cache-max-age",        "cache-on-write", "rocksdb-block-cache", "cache-admission",
      "rocksdb-write-buffer", "requirepass",         "acllog-max-len", "tls-cert-file",
      "tls-key-file",        "tls-ca-cert-file",     "tls-auth-clients", "tls-auth-clients-user",
      "tls-protocols",       "tls-ciphers",          "tls-ciphersuites", "masteruser",
      "masterauth",          "backup-dir",           "backup-keep",    "slowlog-log-slower-than",
      "slowlog-max-len",     "tls-replication",      "tls-client-cert-file", "tls-client-key-file",
      "replica-priority",    "protected-mode",       "promotion-handoff", "maxclients",
      "client-query-buffer-limit", "tcp-keepalive",
  };
  return keys;
}

bool tls_settings_differ(const Config& a, const Config& b) {
  return a.tls_cert_file != b.tls_cert_file || a.tls_key_file != b.tls_key_file ||
         a.tls_ca_cert_file != b.tls_ca_cert_file || a.tls_auth_clients != b.tls_auth_clients ||
         a.tls_auth_clients_user != b.tls_auth_clients_user || a.tls_protocols != b.tls_protocols ||
         a.tls_ciphers != b.tls_ciphers || a.tls_ciphersuites != b.tls_ciphersuites;
}

}  // namespace

ServerContext::ServerContext(Database& db_, Config& config_, ServerStats& stats_, int64_t start_time_ms_)
    : db(db_),
      config(config_),
      stats(stats_),
      start_time_ms(start_time_ms_),
      write_batching(config_.write_batching),
      auth_required(false),
      protected_mode(config_.protected_mode),
      maxclients(config_.maxclients),
      query_buffer_limit(config_.client_query_buffer_limit),
      tcp_keepalive(config_.tcp_keepalive),
      backups(db_.storage(), start_time_ms_ / 1000),
      scripts(db_.storage()),
      mutable_config_(config_) {
  {
    std::random_device rd;
    for (int i = 0; i < 40; ++i) run_id.push_back("0123456789abcdef"[rd() % 16]);
  }
  // requirepass is the default user's password; "user" lines then define users from scratch.
  if (!config_.requirepass.empty()) {
    acl.set_user("default", std::vector<std::string>{"resetpass", ">" + config_.requirepass});
  }
  for (const auto& line : config_.users) {
    auto tokens = split_ws(line);
    std::vector<std::string> rules(tokens.begin() + 1, tokens.end());
    if (auto err = acl.define_user(tokens[0], rules); !err.empty()) {
      throw std::runtime_error(std::format("config: user {}: {}", tokens[0], err));
    }
  }
  acl_changed();
  acl_log.set_max_len(config_.acllog_max_len);
  slowlog.configure(config_.slowlog_log_slower_than, config_.slowlog_max_len);
  if (config_.tls_port != 0) {
    std::string error;
    tls_ = TlsContext::create(config_, &error);
    if (!tls_) throw std::runtime_error("TLS: " + error);
  }
}

std::shared_ptr<TlsContext> ServerContext::tls() const {
  std::lock_guard lock(config_mu_);
  return tls_;
}

void ServerContext::acl_changed() { auth_required = acl.initial_user() == nullptr; }

void start_session(ServerContext& ctx, ClientState& client) {
  client.acl_version = ctx.acl.version();
  client.user = ctx.acl.initial_user();
  client.authenticated = client.user != nullptr;
}

namespace {

bool is_loopback(std::string_view ip) {
  asio::error_code ec;
  asio::ip::address addr = asio::ip::make_address(std::string(ip), ec);
  if (ec) return false;
  if (addr.is_v6() && addr.to_v6().is_v4_mapped()) {
    addr = asio::ip::make_address_v4(asio::ip::v4_mapped, addr.to_v6());
  }
  return addr.is_loopback();
}

}  // namespace

bool protected_mode_refuses(const ServerContext& ctx, std::string_view peer, bool verified_certificate) {
  if (!ctx.protected_mode.load(std::memory_order_relaxed) || ctx.auth_required.load(std::memory_order_relaxed)) {
    return false;
  }
  if (verified_certificate) return false;
  return !is_loopback(peer);
}

bool protected_configs_allowed(const ServerContext& ctx, const ClientState& client) {
  // Fixed at startup: read without the settings' lock.
  const std::string& setting = ctx.config.enable_protected_configs;
  if (setting == "yes") return true;
  if (setting != "local") return false;
  std::string_view ip = std::string_view(client.addr).substr(0, client.addr.rfind(':'));  // "ip:port"
  return is_loopback(ip);
}

Config ServerContext::settings() const {
  std::lock_guard lock(config_mu_);
  return mutable_config_;
}

void ServerContext::set_replicaof(std::string host_port) {
  std::lock_guard lock(config_mu_);
  mutable_config_.replicaof = std::move(host_port);
}

std::string ServerContext::set_config(const std::vector<std::pair<std::string_view, std::string_view>>& pairs) {
  std::lock_guard lock(config_mu_);
  Config next = mutable_config_;
  std::set<std::string, std::less<>> seen;
  bool tls_named = false;  // a tls-* option is set, even to its current value
  for (const auto& [k, v] : pairs) {
    std::string key = to_lower(k);
    if (key.starts_with("tls-")) tls_named = true;
    if (!runtime_settings().contains(key)) {
      Config probe;
      if (!probe.set(key, v).starts_with("unknown config option")) {
        return std::format("ERR CONFIG SET failed (possibly related to argument '{}') - can't set immutable config",
                           key);
      }
      return std::format("ERR Unknown option or number of arguments for CONFIG SET - '{}'", key);
    }
    if (!seen.insert(key).second) {
      return std::format("ERR CONFIG SET failed (possibly related to argument '{}') - duplicate parameter", key);
    }
    if (auto err = next.set(key, v); !err.empty()) {
      return std::format("ERR CONFIG SET failed (possibly related to argument '{}') - {}", key, err);
    }
  }
  if (auto err = next.validate(); !err.empty()) return std::format("ERR CONFIG SET failed - {}", err);

  // The changes that can fail go first, so a failure leaves everything as it was.
  Config& cur = mutable_config_;
  std::shared_ptr<TlsContext> new_tls;
  if (next.tls_port != 0 && (tls_named || tls_settings_differ(next, cur))) {
    // The certificate files are read again, also when a path is set to its
    // current value: that is how certificates are rotated in place.
    std::string error;
    new_tls = TlsContext::create(next, &error);
    if (!new_tls) return std::format("ERR CONFIG SET failed - TLS: {}", error);
  }
  try {
    if (next.rocksdb_write_buffer != cur.rocksdb_write_buffer) db.storage().set_write_buffer_size(next.rocksdb_write_buffer);
  } catch (const std::exception& e) {
    return std::format("ERR CONFIG SET failed (possibly related to argument 'rocksdb-write-buffer') - {}", e.what());
  }
  if (next.rocksdb_block_cache != cur.rocksdb_block_cache) db.storage().set_block_cache_size(next.rocksdb_block_cache);
  if (next.fsync != cur.fsync) db.storage().set_fsync_policy(next.fsync);
  db.cache().set_eviction(next.cache_min_memory, next.cache_max_idle.count(), next.cache_max_age.count());
  db.set_cache_on_write(next.cache_on_write);
  db.cache().set_admission(next.cache_admission);
  write_batching = next.write_batching;
  protected_mode = next.protected_mode;
  maxclients = next.maxclients;
  query_buffer_limit = next.client_query_buffer_limit;
  tcp_keepalive = next.tcp_keepalive;
  if (next.requirepass != cur.requirepass) {
    std::string rule = next.requirepass.empty() ? "nopass" : ">" + next.requirepass;
    acl.set_user("default", std::vector<std::string>{"resetpass", rule});
    acl_changed();
  }
  if (next.log_level != cur.log_level) {
    LogLevel level;
    if (parse_log_level(next.log_level, &level)) set_log_level(level);
  }
  if (new_tls) tls_ = std::move(new_tls);
  acl_log.set_max_len(next.acllog_max_len);
  slowlog.configure(next.slowlog_log_slower_than, next.slowlog_max_len);

  // Only the runtime fields are written: the fixed ones are read without the lock.
  cur.log_level = next.log_level;
  cur.write_batching = next.write_batching;
  cur.fsync = next.fsync;
  cur.cache_min_memory = next.cache_min_memory;
  cur.cache_max_idle = next.cache_max_idle;
  cur.cache_max_age = next.cache_max_age;
  cur.cache_on_write = next.cache_on_write;
  cur.cache_admission = next.cache_admission;
  cur.rocksdb_block_cache = next.rocksdb_block_cache;
  cur.rocksdb_write_buffer = next.rocksdb_write_buffer;
  cur.requirepass = next.requirepass;
  cur.acllog_max_len = next.acllog_max_len;
  cur.tls_cert_file = next.tls_cert_file;
  cur.tls_key_file = next.tls_key_file;
  cur.tls_ca_cert_file = next.tls_ca_cert_file;
  cur.tls_auth_clients = next.tls_auth_clients;
  cur.tls_auth_clients_user = next.tls_auth_clients_user;
  cur.tls_protocols = next.tls_protocols;
  cur.tls_ciphers = next.tls_ciphers;
  cur.tls_ciphersuites = next.tls_ciphersuites;
  cur.masteruser = next.masteruser;
  cur.masterauth = next.masterauth;
  cur.backup_dir = next.backup_dir;
  cur.backup_keep = next.backup_keep;
  cur.slowlog_log_slower_than = next.slowlog_log_slower_than;
  cur.slowlog_max_len = next.slowlog_max_len;
  cur.tls_replication = next.tls_replication;
  cur.protected_mode = next.protected_mode;
  cur.tls_client_cert_file = next.tls_client_cert_file;
  cur.tls_client_key_file = next.tls_client_key_file;
  cur.replica_priority = next.replica_priority;
  cur.promotion_handoff = next.promotion_handoff;
  cur.maxclients = next.maxclients;
  cur.client_query_buffer_limit = next.client_query_buffer_limit;
  cur.tcp_keepalive = next.tcp_keepalive;
  for (const auto& [k, v] : pairs) log_info("CONFIG SET {}", to_lower(k));
  return {};
}

namespace {

// replicaof's "host port" is written as Redis writes it; other values quoted
// when they need it.
std::string config_line(const std::string& key, const std::string& value) {
  return key + " " + (key == "replicaof" ? value : config_file_value(value));
}

}  // namespace

std::string ServerContext::rewrite_config() {
  std::lock_guard lock(config_mu_);
  const std::string& path = mutable_config_.config_file;
  if (path.empty()) return "ERR The server is running without a config file";
  std::ifstream in(path);
  if (!in) return std::format("ERR Rewriting config file: cannot read '{}'", path);
  std::vector<std::string> lines;
  for (std::string line; std::getline(in, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines.push_back(std::move(line));
  }
  in.close();

  std::map<std::string, std::string> current, defaults;
  for (auto& [k, v] : mutable_config_.dump()) current[k] = v;
  for (auto& [k, v] : Config().dump()) defaults[k] = v;
  std::vector<std::string> user_lines;
  for (const auto& u : acl.users()) {
    std::string rules = u->describe();
    if (u->name == "default" && rules == "on nopass ~* &* +@all") continue;  // how it starts anyway
    user_lines.push_back(std::format("user {} {}", u->name, rules));
  }

  // Lines that set an option get its current value, in place; comments and
  // everything else stay as they are. What is left goes at the end.
  std::vector<std::string> out;
  std::set<std::string> done;
  bool users_written = false;
  for (auto& line : lines) {
    auto tokens = split_ws(line);
    if (tokens.empty() || tokens[0].front() == '#') {
      out.push_back(std::move(line));
      continue;
    }
    std::string key = to_lower(tokens[0]);
    if (key == "slaveof") key = "replicaof";  // the old name of the same option
    if (key == "user") {
      if (!users_written) out.insert(out.end(), user_lines.begin(), user_lines.end());
      users_written = true;
      continue;
    }
    auto it = current.find(key);
    if (it == current.end()) {
      out.push_back(std::move(line));
      continue;
    }
    if (done.insert(key).second && !it->second.empty()) out.push_back(config_line(key, it->second));
  }
  std::vector<std::string> added;
  for (const auto& [key, value] : mutable_config_.dump()) {
    if (!done.contains(key) && !value.empty() && value != defaults[key]) {
      added.push_back(config_line(key, value));
    }
  }
  if (!users_written) added.insert(added.end(), user_lines.begin(), user_lines.end());
  if (!added.empty()) {
    out.push_back("");
    out.push_back("# Generated by CONFIG REWRITE");
    out.insert(out.end(), added.begin(), added.end());
  }

  // Written next to it and renamed over it, so a crash never leaves half a file.
  std::string tmp = path + ".rewrite.tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return std::format("ERR Rewriting config file: cannot write '{}'", tmp);
    for (const auto& line : out) f << line << '\n';
    f.flush();
    if (!f) return "ERR Rewriting config file: write failed";
  }
  // With the file's own permissions: it may hold passwords (requirepass, masterauth).
  std::error_code ec;
  auto perms = std::filesystem::status(path, ec).permissions();
  if (!ec) std::filesystem::permissions(tmp, perms, ec);
  std::filesystem::rename(tmp, path, ec);
  if (ec) return std::format("ERR Rewriting config file: {}", ec.message());
  log_info("CONFIG REWRITE: {}", path);
  return {};
}

}  // namespace datadam
