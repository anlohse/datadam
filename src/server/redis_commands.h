#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "config/config.h"
#include "db/database.h"
#include "server/acl.h"
#include "server/admin.h"
#include "server/pubsub.h"
#include "server/replication.h"
#include "server/resp.h"
#include "server/scan_cursors.h"
#include "server/scripting.h"
#include "server/tls.h"

namespace datadam {

struct ServerStats {
  std::atomic<uint64_t> connected_clients{0};
  std::atomic<uint64_t> total_connections{0};
  std::atomic<uint64_t> total_commands{0};
  std::atomic<uint64_t> next_client_id{0};
  std::atomic<uint64_t> auth_failures{0};
  std::atomic<uint64_t> acl_denied{0};
  std::atomic<uint64_t> tls_connections{0};
  std::atomic<uint64_t> tls_handshake_failures{0};
};

// What the protocol handlers share. The settings CONFIG SET can change live
// in `config` under config_mu (and in the atomics below, for the hot path);
// the others (port, dir, ...) never change and can be read directly.
class ReadPool;

class ServerContext {
 public:
  ServerContext(Database& db, Config& config, ServerStats& stats, int64_t start_time_ms);

  Database& db;
  const Config& config;
  ServerStats& stats;
  int64_t start_time_ms;
  std::string run_id;  // random, per process (INFO server; Redis Sentinel tells restarts by it)
  ScanCursors cursors;
  Acl acl;
  AclLog acl_log;

  std::atomic<bool> write_batching;
  // FAILOVER: commands that write are refused (READONLY) while it hands over.
  std::atomic<bool> writes_paused{false};
  // A new connection must log in (the default user has a password or is off).
  std::atomic<bool> auth_required;
  // protected-mode: see protected_mode_refuses.
  std::atomic<bool> protected_mode;
  // Loads keys for reads deferred off the I/O threads; set by the Server
  // while it runs (null: reads go to disk on the I/O thread).
  ReadPool* read_pool = nullptr;

  // A copy of the current settings.
  Config settings() const;
  // CONFIG SET: validates every pair, then applies them all; returns an
  // error message or "".
  std::string set_config(const std::vector<std::pair<std::string_view, std::string_view>>& pairs);
  // CONFIG REWRITE: writes the current settings and ACL users into the
  // config file the server started with; returns an error message or "".
  std::string rewrite_config();
  // Call after changing users.
  void acl_changed();
  // The TLS context for new connections (null without tls-port).
  std::shared_ptr<TlsContext> tls() const;
  // REPLICAOF: records the primary followed ("host port", or "" for none),
  // for CONFIG GET and CONFIG REWRITE.
  void set_replicaof(std::string host_port);

  ReplicationState repl;
  PubSub pubsub;
  Backups backups;
  SlowLog slowlog;
  ClientRegistry clients;
  Monitors monitors;
  ScriptEngine scripts;
  // Set by the Server: stops it (SHUTDOWN); null when there is no server.
  std::function<void()> shutdown;
  // Set by the Server: FAILOVER (host "" = any replica; timeout_ms 0 = none;
  // abort: cancel the one waiting). Returns an error message or "".
  std::function<std::string(const std::string& host, uint16_t port, int64_t timeout_ms, bool force, bool abort)>
      failover;
  // Set by the Server: makes this a replica of host:port, or a primary with
  // an empty host. Returns an error message or "".
  std::function<std::string(const std::string& host, uint16_t port)> replicaof;

 private:
  Config& mutable_config_;
  mutable std::mutex config_mu_;
  std::shared_ptr<TlsContext> tls_;  // under config_mu_
};

struct ClientState {
  uint64_t id = 0;
  int proto = 2;
  std::string name;
  std::string lib_name;
  std::string lib_ver;
  std::string addr;
  bool authenticated = true;
  // Who is logged in (null: nobody yet, or a test client with no ACL) and
  // the ACL version it was read at.
  std::shared_ptr<const AclUser> user;
  uint64_t acl_version = 0;
  bool tls = false;
  bool close_after_reply = false;
  // Set by the connection: a side-effect-free read whose key is not cached
  // does not read the disk on this thread; it sets retry_later and load_key
  // instead, and the connection runs it again once the key is loaded.
  bool defer_blocked_reads = false;
  bool retry_later = false;
  std::string load_key;  // with retry_later: the key to load before the rerun
  // Set by the connection: a command may wait for writes (CDC.READ BLOCK). It
  // sets retry_later and wait_for_writes, and is run again after the next
  // write past wait_after_seq, or once block_deadline_ms (steady clock) passed.
  bool can_block = false;
  bool wait_for_writes = false;
  uint64_t wait_after_seq = 0;
  int64_t block_deadline_ms = 0;  // 0: not blocking
  uint64_t block_from = 0;        // where the blocked read resumes
  // Blocking commands (BLPOP...): set with retry_later. The connection waits
  // until key_waiter fires (a write to one of wait_keys) or block_deadline_ms
  // passes, then runs the command again.
  bool wait_for_keys = false;
  std::vector<std::string> wait_keys;
  std::shared_ptr<KeyWatch::Waiter> key_waiter;
  uint64_t wait_offset = 0;  // WAIT: the replication offset waited for
  // MULTI: the commands queued for EXEC; multi_error: one was refused, so
  // EXEC aborts. WATCH: the keys whose change makes EXEC fail.
  bool in_multi = false;
  bool multi_error = false;
  std::vector<std::vector<std::string>> queued;
  std::shared_ptr<WatchedKeys> watching;
  // Pub/Sub: created by the first SUBSCRIBE; from then on the connection's
  // output goes through it (published messages are interleaved there).
  // MONITOR uses it too.
  std::shared_ptr<Subscriber> subscriber;
  bool monitor = false;
  bool in_script = false;  // the client a Lua script's redis.call uses
  // Set with retry_later: a script has the keyspace; the connection waits
  // for key_waiter (the script's end) off the thread, then runs the command.
  bool wait_for_script = false;
  // What CLIENT LIST shows of this connection (null outside a server).
  std::shared_ptr<ClientEntry> entry;
  // REPLSYNC: the connection becomes a replica's stream (Server::serve_replica).
  bool start_replication = false;
  std::string repl_id;
  uint64_t repl_offset = 0;
  uint16_t listening_port = 0;  // REPLCONF listening-port
};

// Logs a fresh connection in as the default user when it needs no password.
void start_session(ServerContext& ctx, ClientState& client);
// protected-mode: a connection from `peer` (an IP address) is refused while
// the default user needs no password, unless it comes from the loopback
// interface. A TLS connection with a verified client certificate passes too.
bool protected_mode_refuses(const ServerContext& ctx, std::string_view peer, bool verified_certificate = false);
inline constexpr std::string_view kProtectedModeError =
    "DENIED datadam is running in protected mode because protected mode is enabled and no password is set for "
    "the default user. In this mode connections are only accepted from the loopback interface. To accept "
    "connections from other computers, do one of these: 1) set a password for the default user (requirepass, "
    "or ACL SETUSER default); 2) disable protected mode with 'CONFIG SET protected-mode no' from the loopback "
    "interface (and CONFIG REWRITE to keep it), making sure datadam is not reachable from the internet; 3) "
    "start the server with '--protected-mode no', or set 'protected-mode no' in its configuration file.";
// CLIENT LIST style description of a connection (also used by ACL LOG).
std::string client_info(const ClientState& client);
// Copies what CLIENT LIST shows of a connection (name, user...) to its entry.
void publish_identity(ClientState& client);

using Args = std::span<const std::string_view>;

// Executes one Redis command and appends its reply (none if it sets
// client.retry_later: then nothing happened and it must be run again).
void execute_redis_command(ServerContext& ctx, ClientState& client, Args args, Reply& reply);

// If args is a read that defer_blocked_reads applies to, calls load(key) for
// each of its keys that is not cached and the client may read (to prefetch them).
void uncached_read_keys(ServerContext& ctx, const ClientState& client, Args args,
                        const std::function<void(std::string_view)>& load);

// Text of the INFO command; section is lower-case ("" = all).
std::string build_info(ServerContext& ctx, std::string_view section);

}  // namespace datadam
