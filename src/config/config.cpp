#include "config/config.h"

#include <algorithm>
#include <fstream>
#include <thread>

#include "util/log.h"
#include "util/strings.h"

namespace datadam {

namespace {

std::string bad_value(std::string_view key, std::string_view value) {
  return std::format("invalid value '{}' for '{}'", value, key);
}

std::string format_duration(std::chrono::milliseconds d) {
  auto ms = d.count();
  if (ms == 0) return "0";
  // The largest unit that represents it exactly, as a config file would say it.
  for (auto [unit, len] : {std::pair{"d", 86'400'000LL}, {"h", 3'600'000LL}, {"m", 60'000LL}, {"s", 1'000LL}}) {
    if (ms % len == 0) return std::to_string(ms / len) + unit;
  }
  return std::to_string(ms) + "ms";
}

std::optional<bool> parse_bool(std::string_view v) {
  std::string l = to_lower(v);
  if (l == "yes" || l == "true" || l == "on" || l == "1") return true;
  if (l == "no" || l == "false" || l == "off" || l == "0") return false;
  return std::nullopt;
}

template <class T>
std::string set_number(std::string_view key, std::string_view value, T* out, T min, T max) {
  auto v = parse_int64(value);
  if (!v || *v < static_cast<int64_t>(min) || *v > static_cast<int64_t>(max)) return bad_value(key, value);
  *out = static_cast<T>(*v);
  return {};
}

std::string set_size(std::string_view key, std::string_view value, uint64_t* out) {
  auto v = parse_size(value);
  if (!v) return bad_value(key, value);
  *out = *v;
  return {};
}

std::string set_duration(std::string_view key, std::string_view value, std::chrono::milliseconds* out) {
  auto v = parse_duration(value);
  if (!v) return bad_value(key, value);
  *out = *v;
  return {};
}

}  // namespace

unsigned auto_io_threads(unsigned cpus) {
  if (cpus == 0) cpus = 4;  // unknown
  return cpus <= 8 ? cpus : std::max(8u, cpus / 2);
}

unsigned Config::io_thread_count() const {
  return io_threads != 0 ? io_threads : auto_io_threads(std::thread::hardware_concurrency());
}

std::string_view fsync_policy_name(FsyncPolicy p) {
  switch (p) {
    case FsyncPolicy::Always: return "always";
    case FsyncPolicy::EverySec: return "everysec";
    case FsyncPolicy::No: return "no";
  }
  return "?";
}

std::string_view write_path_name(WritePath p) {
  switch (p) {
    case WritePath::Wal: return "wal";
    case WritePath::WalBuffered: return "wal-buffered";
    case WritePath::NoWal: return "no-wal";
    case WritePath::Memory: return "memory";
  }
  return "?";
}

std::string Config::set(std::string_view key_in, std::string_view value) {
  std::string key = to_lower(key_in);
  if (key == "bind") {
    bind = std::string(value);
    return {};
  }
  if (key == "port") return set_number<uint16_t>(key, value, &port, 0, 65535);
  if (key == "tls-port") return set_number<uint16_t>(key, value, &tls_port, 0, 65535);
  if (key == "acllog-max-len") return set_number<unsigned>(key, value, &acllog_max_len, 0, 100000);
  if (key == "backup-keep") return set_number<unsigned>(key, value, &backup_keep, 0, 100000);
  if (key == "slowlog-log-slower-than") {
    return set_number<int64_t>(key, value, &slowlog_log_slower_than, -1, INT64_MAX);
  }
  if (key == "slowlog-max-len") return set_number<unsigned>(key, value, &slowlog_max_len, 0, 1000000);
  auto set_text = [&](std::string* field) {
    *field = std::string(value);
    return std::string();
  };
  if (key == "tls-cert-file") return set_text(&tls_cert_file);
  if (key == "backup-dir") return set_text(&backup_dir);
  if (key == "tls-client-cert-file") return set_text(&tls_client_cert_file);
  if (key == "tls-client-key-file") return set_text(&tls_client_key_file);
  if (key == "tls-key-file") return set_text(&tls_key_file);
  if (key == "tls-ca-cert-file") return set_text(&tls_ca_cert_file);
  if (key == "tls-ciphers") return set_text(&tls_ciphers);
  if (key == "tls-ciphersuites") return set_text(&tls_ciphersuites);
  if (key == "tls-protocols") {  // "TLSv1.2,TLSv1.3" (commas or spaces)
    std::string list;
    std::string v(value);
    std::replace(v.begin(), v.end(), ',', ' ');
    for (std::string_view p : split_ws(v)) {
      if (p != "TLSv1.2" && p != "TLSv1.3") return bad_value(key, value);
      list += (list.empty() ? "" : ",") + std::string(p);
    }
    if (list.empty()) return bad_value(key, value);
    tls_protocols = list;
    return {};
  }
  if (key == "tls-auth-clients") {
    std::string v = to_lower(value);
    if (v != "yes" && v != "no" && v != "optional") return bad_value(key, value);
    tls_auth_clients = v;
    return {};
  }
  if (key == "tls-auth-clients-user") {
    std::string v = to_lower(value);
    if (v != "off" && v != "cn") return bad_value(key, value);
    tls_auth_clients_user = v;
    return {};
  }
  if (key == "memcache-port") return set_number<uint16_t>(key, value, &memcache_port, 0, 65535);
  if (key == "io-threads") {
    if (to_lower(value) == "auto") {
      io_threads = 0;
      return {};
    }
    return set_number<unsigned>(key, value, &io_threads, 1, 1024);
  }
  if (key == "read-threads") return set_number<unsigned>(key, value, &read_threads, 0, 1024);
  if (key == "read-batch") return set_number<unsigned>(key, value, &read_batch, 1, 1024);
  if (key == "dir") {
    dir = std::string(value);
    return {};
  }
  if (key == "fsync") {
    std::string v = to_lower(value);
    if (v == "always") fsync = FsyncPolicy::Always;
    else if (v == "everysec") fsync = FsyncPolicy::EverySec;
    else if (v == "no") fsync = FsyncPolicy::No;
    else return bad_value(key, value);
    return {};
  }
  if (key == "experimental-write-path") {
    std::string v = to_lower(value);
    if (v == "wal") write_path = WritePath::Wal;
    else if (v == "wal-buffered") write_path = WritePath::WalBuffered;
    else if (v == "no-wal") write_path = WritePath::NoWal;
    else if (v == "memory") write_path = WritePath::Memory;
    else return bad_value(key, value);
    return {};
  }
  if (key == "write-batching") {
    auto b = parse_bool(value);
    if (!b) return bad_value(key, value);
    write_batching = *b;
    return {};
  }
  if (key == "tls-replication") {
    auto b = parse_bool(value);
    if (!b) return bad_value(key, value);
    tls_replication = *b;
    return {};
  }
  if (key == "protected-mode") {
    auto b = parse_bool(value);
    if (!b) return bad_value(key, value);
    protected_mode = *b;
    return {};
  }
  if (key == "replica-priority" || key == "slave-priority") {
    return set_number<unsigned>(key, value, &replica_priority, 0, 1000000);
  }
  if (key == "cache-max-memory") return set_size(key, value, &cache_max_memory);
  if (key == "cache-min-memory") return set_size(key, value, &cache_min_memory);
  if (key == "cache-max-object-size") return set_size(key, value, &cache_max_object_size);
  if (key == "cache-max-idle") return set_duration(key, value, &cache_max_idle);
  if (key == "cache-max-age") return set_duration(key, value, &cache_max_age);
  if (key == "cache-maintenance-interval") return set_duration(key, value, &cache_maintenance_interval);
  if (key == "cache-shards") return set_number<unsigned>(key, value, &cache_shards, 1, 1024);
  if (key == "cache-admission") {
    auto b = parse_bool(value);
    if (!b) return bad_value(key, value);
    cache_admission = *b;
    return {};
  }
  if (key == "cache-on-write") {
    auto b = parse_bool(value);
    if (!b) return bad_value(key, value);
    cache_on_write = *b;
    return {};
  }
  if (key == "rocksdb-block-cache") return set_size(key, value, &rocksdb_block_cache);
  if (key == "rocksdb-write-buffer") return set_size(key, value, &rocksdb_write_buffer);
  if (key == "rocksdb-background-jobs") return set_number<int>(key, value, &rocksdb_background_jobs, 1, 256);
  if (key == "wal-retention") return set_duration(key, value, &wal_retention);
  if (key == "wal-retention-size") return set_size(key, value, &wal_retention_size);
  if (key == "requirepass") {
    if (value.find_first_of(" \t\r\n") != std::string_view::npos) return "requirepass cannot contain spaces";
    requirepass = std::string(value);
    return {};
  }
  if (key == "replicaof" || key == "slaveof") {
    auto parts = split_ws(value);
    if (parts.empty() || (parts.size() == 2 && iequals(parts[0], "no") && iequals(parts[1], "one"))) {
      replicaof.clear();
      return {};
    }
    auto primary_port = parts.size() == 2 ? parse_int64(parts[1]) : std::nullopt;
    if (!primary_port || *primary_port < 1 || *primary_port > 65535) return "replicaof needs 'host port' (or 'no one')";
    replicaof = std::format("{} {}", parts[0], *primary_port);
    return {};
  }
  if (key == "masteruser") return set_text(&masteruser);
  if (key == "masterauth") {
    if (value.find_first_of(" \t\r\n") != std::string_view::npos) return "masterauth cannot contain spaces";
    masterauth = std::string(value);
    return {};
  }
  if (key == "user") {
    if (split_ws(value).size() < 1) return "user needs a name";
    users.emplace_back(value);
    return {};
  }
  if (key == "log-level") {
    LogLevel l;
    if (!parse_log_level(value, &l)) return bad_value(key, value);
    log_level = to_lower(value);
    return {};
  }
  return std::format("unknown config option '{}'", key_in);
}

std::string Config::load_file(const std::string& path) {
  std::ifstream in(path);
  if (!in) return std::format("cannot open config file '{}'", path);
  std::string line;
  int lineno = 0;
  while (std::getline(in, line)) {
    ++lineno;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    auto tokens = split_ws(line);
    if (tokens.empty() || tokens[0].front() == '#') continue;
    if ((to_lower(tokens[0]) == "replicaof" || to_lower(tokens[0]) == "slaveof") && tokens.size() == 3) {  // host port
      if (auto err = set(tokens[0], line.substr(static_cast<size_t>(tokens[1].data() - line.data()))); !err.empty()) {
        return std::format("{}:{}: {}", path, lineno, err);
      }
      continue;
    }
    if (to_lower(tokens[0]) == "user" && tokens.size() >= 2) {  // user <name> <rule> ...
      users.emplace_back(line.substr(static_cast<size_t>(tokens[1].data() - line.data())));
      continue;
    }
    if (tokens.size() != 2) return std::format("{}:{}: expected 'key value'", path, lineno);
    if (auto err = set(tokens[0], tokens[1]); !err.empty()) return std::format("{}:{}: {}", path, lineno, err);
  }
  return {};
}

std::string Config::parse_args(int argc, char** argv) {
  int i = 1;
  if (i < argc && std::string_view(argv[i]).substr(0, 2) != "--") {
    if (auto err = load_file(argv[i]); !err.empty()) return err;
    config_file = argv[i];
    ++i;
  }
  for (; i < argc; ++i) {
    std::string_view arg = argv[i];
    if (arg.substr(0, 2) != "--") return std::format("unexpected argument '{}'", arg);
    if (i + 1 >= argc) return std::format("missing value for '{}'", arg);
    if (auto err = set(arg.substr(2), argv[++i]); !err.empty()) return err;
  }
  return validate();
}

std::string Config::validate() {
  if (cache_max_object_size < 64 || cache_max_object_size > (1ULL << 20)) {
    return "cache-max-object-size must be between 64b and 1mb";
  }
  if (cache_min_memory > cache_max_memory) return "cache-min-memory must not exceed cache-max-memory";
  if (cache_maintenance_interval.count() < 10) return "cache-maintenance-interval must be at least 10ms";
  if (wal_retention.count() > 0 && wal_retention.count() < 1000) return "wal-retention must be 0 or at least 1s";
  if (fsync == FsyncPolicy::Always && write_path != WritePath::Wal) {
    return "fsync always requires experimental-write-path wal";
  }
  if (port == 0 && tls_port == 0) return "port 0 disables the plain port; set tls-port, or nothing listens";
  if (tls_port != 0) {
    if (tls_cert_file.empty() || tls_key_file.empty()) return "tls-port needs tls-cert-file and tls-key-file";
    if (tls_auth_clients != "no" && tls_ca_cert_file.empty()) {
      return "tls-auth-clients yes/optional needs tls-ca-cert-file (or set tls-auth-clients no)";
    }
    if (tls_auth_clients_user == "cn" && tls_auth_clients == "no") {
      return "tls-auth-clients-user cn needs client certificates (tls-auth-clients yes or optional)";
    }
  }
  return {};
}

std::vector<std::pair<std::string, std::string>> Config::dump() const {
  return {
      {"bind", bind},
      {"port", std::to_string(port)},
      {"tls-port", std::to_string(tls_port)},
      {"tls-cert-file", tls_cert_file},
      {"tls-key-file", tls_key_file},
      {"tls-ca-cert-file", tls_ca_cert_file},
      {"tls-auth-clients", tls_auth_clients},
      {"tls-auth-clients-user", tls_auth_clients_user},
      {"tls-protocols", tls_protocols},
      {"tls-ciphers", tls_ciphers},
      {"tls-ciphersuites", tls_ciphersuites},
      {"tls-replication", tls_replication ? "yes" : "no"},
      {"protected-mode", protected_mode ? "yes" : "no"},
      {"tls-client-cert-file", tls_client_cert_file},
      {"tls-client-key-file", tls_client_key_file},
      {"replica-priority", std::to_string(replica_priority)},
      {"memcache-port", std::to_string(memcache_port)},
      {"io-threads", io_threads == 0 ? "auto" : std::to_string(io_threads)},
      {"read-threads", std::to_string(read_threads)},
      {"read-batch", std::to_string(read_batch)},
      {"dir", dir},
      {"fsync", std::string(fsync_policy_name(fsync))},
      {"experimental-write-path", std::string(write_path_name(write_path))},
      {"write-batching", write_batching ? "yes" : "no"},
      {"cache-max-memory", format_size(cache_max_memory)},
      {"cache-min-memory", format_size(cache_min_memory)},
      {"cache-max-object-size", format_size(cache_max_object_size)},
      {"cache-max-idle", format_duration(cache_max_idle)},
      {"cache-max-age", format_duration(cache_max_age)},
      {"cache-maintenance-interval", format_duration(cache_maintenance_interval)},
      {"cache-shards", std::to_string(cache_shards)},
      {"cache-on-write", cache_on_write ? "yes" : "no"},
      {"cache-admission", cache_admission ? "yes" : "no"},
      {"rocksdb-block-cache", format_size(rocksdb_block_cache)},
      {"rocksdb-write-buffer", format_size(rocksdb_write_buffer)},
      {"rocksdb-background-jobs", std::to_string(rocksdb_background_jobs)},
      {"wal-retention", format_duration(wal_retention)},
      {"wal-retention-size", format_size(wal_retention_size)},
      {"log-level", log_level},
      {"requirepass", requirepass},
      {"replicaof", replicaof},
      {"masteruser", masteruser},
      {"masterauth", masterauth},
      {"acllog-max-len", std::to_string(acllog_max_len)},
      {"backup-dir", backup_dir},
      {"backup-keep", std::to_string(backup_keep)},
      {"slowlog-log-slower-than", std::to_string(slowlog_log_slower_than)},
      {"slowlog-max-len", std::to_string(slowlog_max_len)},
  };
}

}  // namespace datadam
