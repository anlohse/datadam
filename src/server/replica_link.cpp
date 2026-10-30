#include "server/replica_link.h"

#include <asio/experimental/awaitable_operators.hpp>
#include <asio/ssl.hpp>

#include <atomic>
#include <format>
#include <functional>
#include <stdexcept>

#include "server/acl.h"
#include "server/asio_util.h"
#include "server/command.h"
#include "server/resp.h"
#include "server/tls.h"
#include "storage/data_keys.h"
#include "util/clock.h"
#include "util/log.h"
#include "util/strings.h"

namespace datadam {

using asio::ip::tcp;
using namespace asio::experimental::awaitable_operators;

namespace {

// A replica gives up on a primary that sent nothing (not even a ping) for this long.
constexpr auto kPrimaryTimeout = std::chrono::seconds(10);
constexpr auto kConnectTimeout = std::chrono::seconds(5);
constexpr auto kReconnectDelay = std::chrono::seconds(1);
constexpr auto kAckInterval = std::chrono::seconds(1);
constexpr int64_t kPingIntervalMs = 1000;

// A command or a stream message: a RESP array of bulk strings.
void append_array(std::string& out, std::initializer_list<std::string_view> items) {
  out += std::format("*{}\r\n", items.size());
  for (std::string_view s : items) {
    out += std::format("${}\r\n", s.size());
    out.append(s);
    out += "\r\n";
  }
}

std::string ids_text(const RocksStorage::CfIds& ids) {
  return std::format("{} {} {} {}", ids[0], ids[1], ids[2], ids[3]);
}

// ---- replica side

// One line of a simple reply ("+OK", "-ERR ..."), without its CRLF.
template <class Stream>
asio::awaitable<std::string> read_line(Stream& stream, std::string& in) {
  for (;;) {
    if (size_t eol = in.find("\r\n"); eol != std::string::npos) {
      std::string line = in.substr(0, eol);
      in.erase(0, eol + 2);
      co_return line;
    }
    if (!co_await read_more(stream, in)) throw std::runtime_error("the primary closed the connection");
  }
}

template <class Stream>
asio::awaitable<std::string> call(Stream& stream, std::string& in, std::initializer_list<std::string_view> args) {
  std::string out;
  append_array(out, args);
  if (!co_await flush(stream, out)) throw std::runtime_error("cannot write to the primary");
  co_return co_await read_line(stream, in);
}

}  // namespace

struct ReplicaLink::Shared {
  ServerContext& ctx;
  std::string host;
  uint16_t port;
  uint16_t own_port;
  asio::strand<asio::io_context::executor_type> strand;
  std::atomic<bool> stopped{false};
  std::atomic<bool> streaming{false};  // the stream from the primary runs
  std::atomic<bool> handoff{false};    // request_handoff: send REPLCONF HANDOFF
  // On the strand only: what stop() must interrupt, and the acknowledging
  // side's timer (request_handoff wakes it).
  tcp::socket* socket = nullptr;
  asio::steady_timer* timer = nullptr;
  asio::steady_timer* ack_timer = nullptr;

  Shared(ServerContext& c, asio::io_context& io, std::string h, uint16_t p, uint16_t own)
      : ctx(c), host(std::move(h)), port(p), own_port(own), strand(asio::make_strand(io)) {}
};

namespace {

// The state of one connection to the primary.
template <class Stream>
struct Session {
  ServerContext& ctx;
  std::shared_ptr<ReplicaLink::Shared> sh;
  Stream& stream;
  std::string in;
  RocksStorage::CfIds ids{};
  std::string primary_id;
  uint64_t offset = 0;  // next sequence to fetch
  // The acknowledging side sends as soon as something was applied.
  asio::steady_timer* ack_timer = nullptr;
  uint64_t acked = 0;
  bool failover = false;  // the primary handed over: acknowledge, then become a primary
};

template <class Stream>
void finish_full_sync(Session<Stream>& s, uint64_t next) {
  RocksStorage& st = s.ctx.db.storage();
  auto batch = st.batch();
  batch.put(Cf::System, kReplPrimaryIdKey, s.primary_id);
  batch.put(Cf::System, kReplOffsetKey, encode_u64(next));
  batch.put(Cf::System, kReplSegmentKey, encode_segment({next, st.latest_sequence() + 1}));
  batch.put(Cf::System, kReplOwnIdKey, s.ctx.repl.own_id(st));  // the sync removed it
  st.write(batch);
  s.ctx.db.finish_full_sync();
  s.offset = next;
  s.ctx.repl.set_syncing(false);
  s.ctx.repl.count_sync(true);
}

// Applies one message of the stream.
template <class Stream>
void handle(Session<Stream>& s, const std::vector<std::string_view>& msg, uint64_t full_sync_next) {
  if (msg.empty()) return;
  std::string_view kind = msg[0];
  if (kind == "wal" && msg.size() == 3) {
    auto next = parse_uint64(msg[1]);
    if (!next) throw std::runtime_error("bad wal message from the primary");
    s.ctx.db.apply_replicated(msg[2], s.ids, {{std::string(kReplOffsetKey), encode_u64(*next)}});
    s.offset = *next;
    s.ctx.repl.set_offset(s.offset, s.primary_id);
  } else if (kind == "snap" && msg.size() == 2) {
    s.ctx.db.apply_replicated(msg[1], s.ids, {});
  } else if (kind == "snapend") {
    finish_full_sync(s, full_sync_next);
    s.ctx.repl.set_offset(s.offset, s.primary_id);
    s.ctx.repl.set_link("connected");
    log_info("replication: full sync from the primary done, at offset {}", s.offset);
  } else if (kind == "failover" && (msg.size() == 2 || msg.size() == 3)) {
    auto at = parse_uint64(msg[1]);
    bool force = msg.size() == 3 && msg[2] == "force";
    if (at && (*at == s.offset || force)) {
      s.failover = true;
    } else {
      log_warn("replication: the primary asked for a failover at offset {}, but this replica is at {}: ignored",
               msg[1], s.offset);
    }
  } else if (kind != "ping") {
    throw std::runtime_error(std::format("unexpected message '{}' from the primary", kind));
  }
}

template <class Stream>
asio::awaitable<void> read_stream(Session<Stream>& s, uint64_t full_sync_next) {
  auto ex = co_await asio::this_coro::executor;
  asio::steady_timer idle(ex);
  std::vector<std::string_view> msg;
  std::string error;
  for (;;) {
    size_t pos = 0;
    for (;;) {
      auto st = RespParser::parse(s.in, pos, msg, error);
      if (st == RespParser::Status::Incomplete) break;
      if (st == RespParser::Status::Error) throw std::runtime_error("bad stream from the primary: " + error);
      handle(s, msg, full_sync_next);
    }
    consume(s.in, pos);
    // Applied something (or must hand over): acknowledge now, not at the next tick.
    if ((s.offset != s.acked || s.failover) && s.ack_timer) s.ack_timer->cancel();
    if (s.failover) {
      // The acknowledging side takes it from here; keep reading meanwhile.
      idle.expires_after(kPrimaryTimeout);
      co_await idle.async_wait(kUseAwaitable);
      co_return;
    }
    idle.expires_after(kPrimaryTimeout);
    auto got = co_await (read_more(s.stream, s.in) || idle.async_wait(kUseAwaitable));
    if (got.index() == 1) throw std::runtime_error("the primary sent nothing for 10 s");
    if (!std::get<0>(got)) throw std::runtime_error("the primary closed the connection");
    s.ctx.repl.touched_primary();
  }
}

// The only writer of the connection once the stream runs: acknowledgments,
// as soon as read_stream applied something and at least every second, and
// the answer to a failover.
template <class Stream>
asio::awaitable<void> send_acks(Session<Stream>& s) {
  auto ex = co_await asio::this_coro::executor;
  asio::steady_timer timer(ex);
  s.ack_timer = &timer;
  s.sh->ack_timer = &timer;
  struct Forget {
    asio::steady_timer*& t;
    asio::steady_timer*& shared;
    ~Forget() { t = shared = nullptr; }
  } forget{s.ack_timer, s.sh->ack_timer};
  for (;;) {
    if (!s.failover) {
      // A cancel() posted from read_stream (or request_handoff) while this
      // was writing is not lost: the comparisons below catch it.
      if (s.offset == s.acked && !s.sh->handoff) {
        timer.expires_after(kAckInterval);
        co_await timer.async_wait(kUseAwaitable);
      }
    }
    std::string out;
    uint64_t sending = s.offset;  // read_stream may move it on while this writes
    std::string offset = std::to_string(sending);
    append_array(out, {"REPLCONF", "ACK", offset});
    if (s.failover) append_array(out, {"REPLCONF", "FAILOVER-ACK", offset});
    if (s.sh->handoff.exchange(false)) append_array(out, {"REPLCONF", "HANDOFF", offset});
    if (!co_await flush(s.stream, out)) co_return;
    s.acked = sending;
    if (s.failover) {
      // Handed over: this server becomes a primary (on the server's own
      // strand, which stops this link), and the link does not reconnect.
      log_info("replication: the primary handed over at offset {}: becoming a primary", s.offset);
      s.sh->stopped = true;
      ServerContext& ctx = s.ctx;
      asio::post(s.sh->strand.get_inner_executor(), [&ctx] {
        if (ctx.replicaof && ctx.replicaof("", 0).empty()) ctx.set_replicaof("");
      });
      co_return;
    }
  }
}

template <class Stream>
asio::awaitable<void> sync_with(std::shared_ptr<ReplicaLink::Shared> sh, Stream& stream) {
  ServerContext& ctx = sh->ctx;
  RocksStorage& st = ctx.db.storage();
  Session<Stream> s{ctx, sh, stream, {}, {}, {}, 0};
  Config cfg = ctx.settings();
  if (!cfg.masterauth.empty()) {
    std::string reply = cfg.masteruser.empty() ? co_await call(stream, s.in, {"AUTH", cfg.masterauth})
                                               : co_await call(stream, s.in, {"AUTH", cfg.masteruser, cfg.masterauth});
    if (reply != "+OK") throw std::runtime_error("the primary refused AUTH: " + reply);
  }
  if (std::string reply = co_await call(stream, s.in, {"PING"}); reply != "+PONG") {
    throw std::runtime_error("the primary answered PING with " + reply);
  }
  std::string port_text = std::to_string(sh->own_port);
  if (std::string reply = co_await call(stream, s.in, {"REPLCONF", "listening-port", port_text}); reply != "+OK") {
    throw std::runtime_error("the primary refused REPLCONF: " + reply);
  }
  // Resume where the last stream stopped, if this replica has a complete copy.
  // A server that followed nobody (a primary until now) offers its own
  // history, which a replica it handed over to can continue.
  std::string followed, raw_offset;
  uint64_t offset = 0;
  bool following = st.get(Cf::System, kReplPrimaryIdKey, &followed);
  if (ctx.repl.syncing()) {
    followed = "?";
  } else if (following && st.get(Cf::System, kReplOffsetKey, &raw_offset) && raw_offset.size() == 8) {
    offset = read_be64(raw_offset);
  } else if (!following && st.get(Cf::System, kReplOwnIdKey, &followed) && followed.size() == 40) {
    offset = st.latest_sequence() + 1;
  } else {
    followed = "?";
  }
  std::string reply = co_await call(stream, s.in, {"REPLSYNC", followed, std::to_string(offset)});
  auto words = split_ws(reply);
  if (words.size() != 7 || (words[0] != "+FULLSYNC" && words[0] != "+CONTINUE")) {
    throw std::runtime_error("the primary refused REPLSYNC: " + reply);
  }
  s.primary_id = std::string(words[1]);
  auto next = parse_uint64(words[2]);
  for (size_t i = 0; i < 4; ++i) {
    auto id = parse_uint64(words[3 + i]);
    if (!id || !next) throw std::runtime_error("bad REPLSYNC reply: " + reply);
    s.ids[i] = static_cast<uint32_t>(*id);
  }
  uint64_t full_sync_next = 0;
  if (words[0] == "+FULLSYNC") {
    log_info("replication: full sync from {}:{} (primary {})", sh->host, sh->port, s.primary_id);
    ctx.repl.set_link("sync");
    ctx.repl.set_syncing(true);
    ctx.db.clear_for_full_sync();
    full_sync_next = *next;
  } else {
    if (s.primary_id != followed) {
      // The primary continues the history this server had: from here on, the
      // copy is of the primary's.
      auto batch = st.batch();
      batch.put(Cf::System, kReplPrimaryIdKey, s.primary_id);
      batch.put(Cf::System, kReplOffsetKey, encode_u64(*next));
      batch.put(Cf::System, kReplSegmentKey, encode_segment({*next, st.latest_sequence() + 1}));
      batch.del(Cf::System, kReplPreviousKey);
      st.write(batch);
      log_info("replication: continuing the history of {} from {}:{} (primary {}) at offset {}", followed, sh->host,
               sh->port, s.primary_id, *next);
    } else {
      log_info("replication: continuing from {}:{} at offset {}", sh->host, sh->port, *next);
    }
    s.offset = *next;
    s.acked = s.offset;
    ctx.repl.count_sync(false);
    ctx.repl.set_offset(s.offset, s.primary_id);
    ctx.repl.set_link("connected");
  }
  ctx.repl.touched_primary();
  sh->handoff = false;
  sh->streaming = true;
  struct NotStreaming {
    std::atomic<bool>& streaming;
    ~NotStreaming() { streaming = false; }
  } not_streaming{sh->streaming};
  co_await first_to_end(read_stream(s, full_sync_next), send_acks(s));
}

asio::awaitable<void> connect_and_sync(std::shared_ptr<ReplicaLink::Shared> sh) {
  ServerContext& ctx = sh->ctx;
  auto ex = co_await asio::this_coro::executor;
  tcp::resolver resolver(ex);
  auto [rec, endpoints] = co_await resolver.async_resolve(sh->host, std::to_string(sh->port), kUseAwaitable);
  if (rec) throw std::runtime_error(std::format("cannot resolve {}: {}", sh->host, rec.message()));
  tcp::socket socket(ex);
  sh->socket = &socket;
  struct Forget {
    ReplicaLink::Shared* s;
    ~Forget() { s->socket = nullptr; }
  } forget{sh.get()};
  if (sh->stopped) co_return;
  ctx.repl.set_link("connecting");
  asio::steady_timer deadline(ex);
  deadline.expires_after(kConnectTimeout);
  auto connected = co_await (asio::async_connect(socket, endpoints, kUseAwaitable) || deadline.async_wait(kUseAwaitable));
  if (connected.index() == 1) throw std::runtime_error(std::format("connecting to {}:{} timed out", sh->host, sh->port));
  if (auto [ec, ep] = std::get<0>(connected); ec) {
    throw std::runtime_error(std::format("cannot connect to {}:{}: {}", sh->host, sh->port, ec.message()));
  }
  asio::error_code ignored;
  socket.set_option(tcp::no_delay(true), ignored);

  Config cfg = ctx.settings();
  if (!cfg.tls_replication) co_return co_await sync_with(sh, socket);

  // TLS: the primary's certificate must be signed by a trusted CA and name the host.
  std::string error;
  auto tls = replication_client_tls(cfg, &error);
  if (!tls) throw std::runtime_error("tls-replication: " + error);
  asio::ssl::stream<tcp::socket&> stream(socket, *tls);
  if (!SSL_set_tlsext_host_name(stream.native_handle(), sh->host.c_str())) {
    throw std::runtime_error("tls-replication: cannot set the server name");
  }
  stream.set_verify_callback(asio::ssl::host_name_verification(sh->host));
  deadline.expires_after(kConnectTimeout);
  auto shaken = co_await (stream.async_handshake(asio::ssl::stream_base::client, kUseAwaitable) ||
                          deadline.async_wait(kUseAwaitable));
  if (shaken.index() == 1) throw std::runtime_error("the TLS handshake with the primary timed out");
  if (auto [ec] = std::get<0>(shaken); ec) {
    throw std::runtime_error(std::format("TLS handshake with {}:{} failed: {}", sh->host, sh->port, ec.message()));
  }
  co_await sync_with(sh, stream);
}

asio::awaitable<void> run(std::shared_ptr<ReplicaLink::Shared> sh) {
  auto ex = co_await asio::this_coro::executor;
  asio::steady_timer pause(ex);
  while (!sh->stopped) {
    try {
      co_await connect_and_sync(sh);
    } catch (const std::exception& e) {
      if (!sh->stopped) log_warn("replication: {}", e.what());
    }
    if (sh->stopped) break;  // its successor (if any) speaks for the link now
    sh->ctx.repl.set_link("down");
    pause.expires_after(kReconnectDelay);
    sh->timer = &pause;
    co_await pause.async_wait(kUseAwaitable);
    sh->timer = nullptr;
  }
}

}  // namespace

ReplicaLink::ReplicaLink(ServerContext& ctx, asio::io_context& io, std::string host, uint16_t port, uint16_t own_port)
    : shared_(std::make_shared<Shared>(ctx, io, std::move(host), port, own_port)) {}

ReplicaLink::~ReplicaLink() { stop(); }

void ReplicaLink::stop() {
  shared_->stopped = true;
  asio::post(shared_->strand, [s = shared_] {
    asio::error_code ignored;
    if (s->socket) s->socket->close(ignored);
    if (s->timer) s->timer->cancel();
  });
}

void ReplicaLink::start() { asio::co_spawn(shared_->strand, run(shared_), asio::detached); }

bool ReplicaLink::request_handoff() {
  auto sh = shared_;
  if (sh->stopped || !sh->streaming) return false;
  sh->handoff = true;
  asio::post(sh->strand, [sh] {
    if (sh->ack_timer) sh->ack_timer->cancel();
  });
  return true;
}

// ---- primary side

namespace {

template <class Stream>
asio::awaitable<void> stream_wal(ServerContext& ctx, Stream& stream, uint64_t replica, uint64_t next) {
  RocksStorage& st = ctx.db.storage();
  RocksStorage::WalReader wal(st, next);
  std::shared_ptr<asio::steady_timer> timer;
  int64_t last_sent = steady_time_ms();
  std::string out;
  for (;;) {
    std::vector<RocksStorage::RawBatch> raw;
    wal.read(kReplBatchBytes, &raw);  // throws if the replica fell behind the kept WAL
    if (raw.empty()) {
      // FAILOVER: everything up to the hand-over point is sent; tell the replica.
      if (auto at = ctx.repl.take_failover(replica, next)) {
        std::string at_text = std::to_string(*at);
        if (ctx.repl.failover_forced(replica)) append_array(out, {"failover", at_text, "force"});
        else append_array(out, {"failover", at_text});
        if (!co_await flush(stream, out)) co_return;
        last_sent = steady_time_ms();
        continue;
      }
      if (steady_time_ms() - last_sent >= kPingIntervalMs) {
        append_array(out, {"ping"});
        if (!co_await flush(stream, out)) co_return;
        last_sent = steady_time_ms();
      }
      co_await wait_for_write(st, timer, next - 1, steady_time_ms() + kPingIntervalMs);
      continue;
    }
    for (const auto& b : raw) {
      next = b.seq + b.count;
      std::string next_text = std::to_string(next);
      append_array(out, {"wal", next_text, b.data});
    }
    if (!co_await flush(stream, out)) co_return;
    ctx.repl.replica_sent(replica, next);
    last_sent = steady_time_ms();
  }
}

template <class Stream>
asio::awaitable<void> read_acks(ServerContext& ctx, Stream& stream, uint64_t replica, std::string in,
                                std::function<bool()> may_hand_over) {
  std::vector<std::string_view> cmd;
  std::string error;
  for (;;) {
    size_t pos = 0;
    for (;;) {
      auto st = RespParser::parse(in, pos, cmd, error);
      if (st != RespParser::Status::Ok) {
        if (st == RespParser::Status::Error) co_return;
        break;
      }
      if (cmd.size() == 3 && iequals(cmd[0], "replconf") && iequals(cmd[1], "ack")) {
        if (auto offset = parse_uint64(cmd[2])) ctx.repl.replica_ack(replica, *offset, ctx.db.storage());
      } else if (cmd.size() == 3 && iequals(cmd[0], "replconf") && iequals(cmd[1], "failover-ack")) {
        ctx.repl.failover_ack(replica);
      } else if (cmd.size() == 3 && iequals(cmd[0], "replconf") && iequals(cmd[1], "handoff")) {
        if (!may_hand_over()) {
          log_warn("replication: replica {} asked for the hand-over, but its user may not run FAILOVER: refused",
                   replica);
        } else if (ctx.handoff) {
          ctx.handoff(replica);
        }
      }
    }
    consume(in, pos);
    if (!co_await read_more(stream, in)) co_return;
  }
}

}  // namespace

template <class Stream>
asio::awaitable<void> serve_replica(ServerContext& ctx, Stream& stream, ClientState& client, std::string in) {
  RocksStorage& st = ctx.db.storage();
  std::string id = ctx.repl.own_id(st);
  RocksStorage::CfIds ids = st.cf_ids();
  uint64_t offset = client.repl_offset;
  bool partial = client.repl_id == id && offset > 0 && offset >= st.oldest_sequence() && offset <= st.latest_sequence() + 1;
  bool continued = false;  // a replica of the primary this server followed, going on here
  if (!partial && offset > 0) {
    auto at = continue_previous(st, client.repl_id, offset);
    if (at && *at >= st.oldest_sequence() && *at <= st.latest_sequence() + 1) {
      offset = *at;
      partial = continued = true;
    }
  }
  std::string ip = client.addr.substr(0, client.addr.rfind(':'));
  uint64_t replica = ctx.repl.add_replica(ip, client.listening_port, !partial);
  struct Gone {
    ServerContext& ctx;
    uint64_t replica;
    ~Gone() { ctx.repl.remove_replica(replica, ctx.db.storage()); }
  } gone{ctx, replica};
  try {
    std::string out;
    uint64_t next = offset;
    if (partial) {
      ctx.repl.replica_needs(replica, next, st);
      out = std::format("+CONTINUE {} {} {}\r\n", id, next, ids_text(ids));
      if (continued) {
        log_info("replication: replica {} continues the history of {} (at its offset {}) at offset {}", client.addr,
                 client.repl_id, client.repl_offset, next);
      } else {
        log_info("replication: replica {} continues at offset {}", client.addr, next);
      }
    } else {
      auto snap = st.snapshot();
      next = snap->sequence() + 1;
      ctx.repl.replica_needs(replica, next, st);  // the WAL it continues from, written while it syncs
      out = std::format("+FULLSYNC {} {} {}\r\n", id, next, ids_text(ids));
      log_info("replication: full sync to replica {} (up to offset {})", client.addr, next);
      for (Cf cf : {Cf::Meta, Cf::Data, Cf::Index, Cf::System}) {
        auto c = st.cursor(cf, "", "", false, snap.get());
        auto batch = st.batch();
        for (c->seek_first(); c->valid(); c->next()) {
          if (cf == Cf::System && c->key().starts_with("repl:")) continue;  // this server's own state
          batch.put(cf, c->key(), c->value());
          if (batch.size() >= kReplBatchBytes) {
            append_array(out, {"snap", batch.data()});
            batch.clear();
            if (!co_await flush(stream, out)) co_return;
          }
        }
        c->check_status();
        if (!batch.empty()) append_array(out, {"snap", batch.data()});
      }
      append_array(out, {"snapend"});
    }
    if (!co_await flush(stream, out)) co_return;
    ctx.repl.replica_sent(replica, next);
    ctx.repl.replica_state(replica, "online");
    // A hand-over makes this server follow the replica: the same as FAILOVER
    // TO it, so only for a user that may run FAILOVER (a replication-only user
    // may not), as it is when the replica asks.
    std::string user = client.user ? client.user->name : "";
    auto may_hand_over = [&ctx, user] {
      if (user.empty()) return true;
      auto u = ctx.acl.find(user);
      const CommandSpec* spec = find_command("failover");
      return u && spec && u->may_run("failover", *spec, "");
    };
    co_await first_to_end(stream_wal(ctx, stream, replica, next),
                          read_acks(ctx, stream, replica, std::move(in), may_hand_over));
  } catch (const std::exception& e) {
    log_warn("replication: replica {}: {}", client.addr, e.what());
  }
  log_info("replication: replica {} disconnected", client.addr);
}

template asio::awaitable<void> serve_replica(ServerContext&, tcp::socket&, ClientState&, std::string);
template asio::awaitable<void> serve_replica(ServerContext&, asio::ssl::stream<tcp::socket>&, ClientState&,
                                             std::string);

}  // namespace datadam
