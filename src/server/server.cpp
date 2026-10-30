#include "server/server.h"

#include <asio/experimental/awaitable_operators.hpp>
#include <asio/ssl.hpp>
#include <csignal>
#include <format>
#include <limits>
#include <optional>

#include "server/asio_util.h"
#include "server/memcache_protocol.h"
#include "server/metrics.h"
#include "server/read_pool.h"
#include "server/resp.h"
#include "util/clock.h"
#include "util/log.h"
#include "util/strings.h"

namespace datadam {

namespace {

using asio::ip::tcp;
// Pipelined replies are flushed once this much output is pending.
constexpr size_t kMaxPendingOutput = 1 << 20;
// Largest incomplete request a client may send before it authenticates.
constexpr size_t kMaxUnauthenticatedRequest = 16 * 1024;
// A TLS client that has not finished its handshake by then is dropped.
constexpr auto kTlsHandshakeTimeout = std::chrono::seconds(10);
// When a read waits for its key to be loaded, the keys of up to this many of
// the requests after it are loaded at the same time.
constexpr int kMaxPrefetchCommands = 256;
// While a command is blocked (BLPOP...), requests the client sends are kept
// up to this much; past it the connection is no longer watched for them.
constexpr size_t kMaxInputWhileBlocked = 1 << 20;

// Counts a connection for INFO / stats for as long as the session lives.
class ConnectionScope {
 public:
  explicit ConnectionScope(ServerStats& s) : stats_(s) {
    stats_.connected_clients.fetch_add(1, std::memory_order_relaxed);
    stats_.total_connections.fetch_add(1, std::memory_order_relaxed);
  }
  ~ConnectionScope() { stats_.connected_clients.fetch_sub(1, std::memory_order_relaxed); }

 private:
  ServerStats& stats_;
};

std::string peer_address(const tcp::socket& socket) {
  asio::error_code ec;
  auto ep = socket.remote_endpoint(ec);
  if (ec) return "?";
  return std::format("{}:{}", ep.address().to_string(), ep.port());
}

std::string peer_ip(const tcp::socket& socket) {
  asio::error_code ec;
  auto ep = socket.remote_endpoint(ec);
  return ec ? std::string() : ep.address().to_string();
}

// The control context, then one per io-thread.
std::vector<std::unique_ptr<asio::io_context>> make_contexts(unsigned io_threads) {
  std::vector<std::unique_ptr<asio::io_context>> out;
  for (unsigned i = 0; i <= std::max(io_threads, 1u); ++i) out.push_back(std::make_unique<asio::io_context>(1));
  return out;
}

// Counts client traffic into the thread's INFO total_net_input_bytes and
// total_net_output_bytes (read_more, flush).
struct NetInput {
  ServerStats* stats;
  void operator()(size_t n) const { bump(stats->threads.local().net_input_bytes, n); }
};
struct NetOutput {
  ServerStats* stats;
  void operator()(size_t n) const { bump(stats->threads.local().net_output_bytes, n); }
};

// Reads more bytes into `in`; returns false when the peer is gone.
// Waits, without holding the thread, for the read pool to load `key`;
// completes with whether it is now cached. `then` runs once the key is
// queued, to queue more behind it.
template <class Then, class Token>
auto async_load(ReadPool& pool, std::string_view key, Then& then, Token&& token) {
  return asio::async_initiate<Token, void(bool)>(
      [&pool, &then](auto handler, std::string_view key) {
        auto ex = asio::get_associated_executor(handler);
        auto h = std::make_shared<decltype(handler)>(std::move(handler));
        pool.load(key, [h, ex](bool cached) { asio::post(ex, [h, cached] { std::move(*h)(cached); }); });
        then();
      },
      token, key);
}


}  // namespace

Server::Server(ServerContext& ctx)
    : ctx_(ctx), contexts_(make_contexts(ctx.config.io_thread_count())), io_(*contexts_[0]), signals_(io_, SIGINT, SIGTERM),
      handoff_timer_(io_) {
  // The others have no work until a connection comes: keep their threads.
  for (auto& c : contexts_) work_.push_back(asio::make_work_guard(*c));
  if (ctx.config.read_threads > 0) {
    read_pool_ = std::make_unique<ReadPool>(ctx.db, ctx.config.read_threads, ctx.config.read_batch);
    ctx_.read_pool = read_pool_.get();
  }
  ctx_.replicaof = [this](const std::string& host, uint16_t port) { return follow(host, port); };
  ctx_.shutdown = [this] { stop(); };
  ctx_.failover = [this](const std::string& host, uint16_t port, int64_t timeout_ms, bool force, bool abort) {
    return failover(host, port, timeout_ms, force, abort);
  };
  ctx_.handoff = [this](uint64_t replica) { hand_over(replica); };
}

Server::~Server() {
  ctx_.replicaof = nullptr;
  ctx_.shutdown = nullptr;
  ctx_.failover = nullptr;
  ctx_.handoff = nullptr;
  // Before io_ goes: these callbacks hold its executors.
  ctx_.clients.clear();
  ctx_.db.key_watch().drop_waiters();
  ctx_.repl.drop_waiters();
  {
    std::lock_guard lock(follow_mu_);
    handoff_timer_.cancel();
    replica_link_.reset();
  }
  ctx_.read_pool = nullptr;
  // Waiting connections hold timers of io_: release them while it exists.
  ctx_.db.storage().cancel_all_write_waits();
}

void Server::start(bool ephemeral_ports) {
  auto address = asio::ip::make_address(ctx_.config.bind);
  auto listen = [&](uint16_t port, Protocol proto) {
    tcp::endpoint ep(address, ephemeral_ports ? 0 : port);
    tcp::acceptor acceptor(io_);
    acceptor.open(ep.protocol());
#ifndef _WIN32
    // On Windows SO_REUSEADDR would let a second process steal the port.
    acceptor.set_option(tcp::acceptor::reuse_address(true));
#endif
    acceptor.bind(ep);
    acceptor.listen(asio::socket_base::max_listen_connections);
    ports_[static_cast<int>(proto)] = acceptor.local_endpoint().port();
    if (proto == Protocol::Metrics) {
      asio::co_spawn(io_, metrics_loop(std::move(acceptor)), asio::detached);
    } else {
      asio::co_spawn(io_, accept_loop(std::move(acceptor), proto), asio::detached);
    }
  };
  if (ctx_.config.port != 0) listen(ctx_.config.port, Protocol::Redis);
  if (ctx_.config.tls_port != 0) listen(ctx_.config.tls_port, Protocol::RedisTls);
  if (ctx_.config.memcache_port != 0) listen(ctx_.config.memcache_port, Protocol::Memcache);
  if (ctx_.config.metrics_port != 0) listen(ctx_.config.metrics_port, Protocol::Metrics);
  asio::co_spawn(io_, sample_rates(), asio::detached);

  if (!ctx_.config.replicaof.empty()) {
    auto parts = split_ws(ctx_.config.replicaof);
    follow(std::string(parts[0]), static_cast<uint16_t>(*parse_int64(parts[1])));
  }

  signals_.async_wait([this](const asio::error_code& ec, int sig) {
    if (ec) return;
    log_info("signal {} received, shutting down", sig);
    stop();
  });
}

void Server::run() {
  std::vector<std::thread> threads;
  for (size_t i = 1; i < contexts_.size(); ++i) {
    threads.emplace_back([c = contexts_[i].get()] { c->run(); });
    threads_.push_back(threads.back().get_id());
  }
  io_.run();  // the accept loops read threads_ from here on
  for (auto& t : threads) t.join();
}

void Server::stop() {
  for (auto& c : contexts_) c->stop();
}

std::string Server::failover(const std::string& host, uint16_t port, int64_t timeout_ms, bool force, bool abort) {
  if (abort) {
    if (!failover_running_) return "ERR No failover in progress.";
    failover_abort_ = true;
    return {};
  }
  if (ctx_.repl.is_replica()) return "ERR FAILOVER is not valid when server is a replica.";
  if (force && (timeout_ms == 0 || host.empty())) {
    return "ERR FAILOVER with force option requires both a timeout and target HOST and IP.";
  }
  std::optional<ReplicationState::Replica> target;
  for (const auto& r : ctx_.repl.replicas()) {
    if (r.state != "online") continue;
    if (!host.empty()) {
      if (r.ip == host && r.listening_port == port) target = r;
    } else if (!target || r.ack_offset > target->ack_offset) {
      target = r;  // the one that is furthest along
    }
  }
  if (!target) {
    return host.empty() ? "ERR FAILOVER requires connected replicas."
                        : "ERR FAILOVER target HOST and PORT is not a replica.";
  }
  return start_failover(*target, timeout_ms, force);
}

std::string Server::start_failover(const ReplicationState::Replica& target, int64_t timeout_ms, bool force) {
  if (failover_running_.exchange(true)) return "ERR FAILOVER already in progress.";
  failover_abort_ = false;
  ctx_.writes_paused = true;  // from here on writes are refused; those under way finish
  ctx_.repl.set_failover_state("waiting-for-sync");
  log_info("FAILOVER: handing over to {}:{}", target.ip, target.listening_port);
  asio::co_spawn(io_, run_failover(target.id, target.ip, target.listening_port, timeout_ms, force), asio::detached);
  return {};
}

void Server::hand_over(uint64_t replica) {
  // Less than the replica waits for the answer (kHandoffWait): a primary that
  // gives up takes writes again before the replica promotes itself anyway.
  constexpr int64_t kCatchUpMs = 1000;
  if (ctx_.repl.is_replica()) return;
  for (const auto& r : ctx_.repl.replicas()) {
    if (r.id != replica) continue;
    log_info("replication: {}:{} is being promoted and asks for the hand-over", r.ip, r.listening_port);
    std::string error = r.state == "online" ? start_failover(r, kCatchUpMs, false) : "it is still syncing";
    if (!error.empty()) log_warn("replication: no hand-over to {}:{}: {}", r.ip, r.listening_port, error);
    return;
  }
}

asio::awaitable<void> Server::run_failover(uint64_t replica, std::string ip, uint16_t port, int64_t timeout_ms,
                                           bool force) {
  constexpr int64_t kTakeOverMs = 5000;  // for the replica to answer the hand-over
  RocksStorage& st = ctx_.db.storage();
  asio::steady_timer pause(co_await asio::this_coro::executor);
  auto sleep = [&]() -> asio::awaitable<void> {
    pause.expires_after(std::chrono::milliseconds(10));
    co_await pause.async_wait(kUseAwaitable);
  };
  auto give_up = [&](std::string_view why) {
    log_warn("FAILOVER aborted: {}", why);
    ctx_.writes_paused = false;
    ctx_.repl.set_failover_state("no-failover");
    failover_running_ = false;
  };
  auto acked = [&](uint64_t offset) {
    for (const auto& r : ctx_.repl.replicas()) {
      if (r.id == replica) return r.ack_offset >= offset;
    }
    return false;
  };
  co_await sleep();  // writes that were under way commit meanwhile
  int64_t deadline = timeout_ms ? steady_time_ms() + timeout_ms : std::numeric_limits<int64_t>::max();
  uint64_t at = 0;
  for (;;) {
    if (failover_abort_) {
      give_up("FAILOVER ABORT");
      co_return;
    }
    uint64_t now_at = st.latest_sequence() + 1;
    if (acked(now_at)) {
      at = now_at;
      break;
    }
    if (steady_time_ms() >= deadline) {
      if (!force) {
        give_up("the replica did not catch up in time");
        co_return;
      }
      at = now_at;
      break;
    }
    co_await sleep();
  }
  // The replica has everything (or FORCE): hand over in its stream.
  ctx_.repl.set_failover_state("failover-in-progress");
  ctx_.repl.request_failover(replica, at, force);
  st.wake_write_waiters();
  int64_t answer_by = steady_time_ms() + kTakeOverMs;
  while (!ctx_.repl.failover_acked(replica)) {
    if (steady_time_ms() >= answer_by) {
      give_up("the replica did not take over");
      co_return;
    }
    co_await sleep();
  }
  // It is the primary now: follow it.
  std::string error = follow(ip, port);
  if (error.empty()) ctx_.set_replicaof(std::format("{} {}", ip, port));
  ctx_.writes_paused = false;  // writes now get the replica's READONLY
  ctx_.repl.set_failover_state("no-failover");
  failover_running_ = false;
  log_info("FAILOVER done: following {}:{}", ip, port);
}

std::string Server::follow(const std::string& host, uint16_t port) {
  std::lock_guard lock(follow_mu_);
  return follow_locked(host, port);
}

std::string Server::follow_locked(const std::string& host, uint16_t port) {
  // For the primary to hand over: it pauses its writes, waits for this
  // replica to have them all, then sends FAILOVER's ["failover", next].
  constexpr auto kHandoffWait = std::chrono::seconds(5);
  ++follow_gen_;
  bool handed_over = std::exchange(handoff_pending_, false);
  handoff_timer_.cancel();
  if (host.empty() && !handed_over && ctx_.db.read_only() && replica_link_ && !ctx_.repl.syncing() &&
      ctx_.settings().promotion_handoff && replica_link_->request_handoff()) {
    // promotion-handoff: the primary would otherwise go on taking writes,
    // acknowledged by its other replicas, that this server never gets. Its
    // answer comes back here (the link calls replicaof("", 0)); without one,
    // this server becomes a primary all the same.
    handoff_pending_ = true;
    uint64_t gen = follow_gen_;
    handoff_timer_.expires_after(kHandoffWait);
    handoff_timer_.async_wait([this, gen](const asio::error_code& ec) {
      if (ec) return;
      std::lock_guard lock(follow_mu_);
      if (gen != follow_gen_ || !handoff_pending_) return;
      log_warn("replication: the primary did not hand over: becoming a primary without it");
      follow_locked("", 0);
    });
    log_info("replication: asked the primary to hand over (promotion-handoff)");
    return {};
  }
  if (replica_link_) {
    replica_link_->stop();
    replica_link_.reset();
  }
  RocksStorage& st = ctx_.db.storage();
  if (host.empty()) {
    if (!ctx_.db.read_only()) return {};  // already a primary
    ctx_.repl.set_primary("", 0);
    ctx_.repl.set_syncing(false);
    // A new history, which goes on from the one followed: replicas of the old
    // primary (and the old primary) that are not past this point continue
    // here. Kept before any write of its own can come in.
    keep_history_at_promotion(st);
    ctx_.repl.new_own_id(st);
    ctx_.db.promote();
    log_info("replication: this server is a primary now");
    return {};
  }
  ctx_.db.set_read_only(true);
  ctx_.repl.set_primary(host, port);
  // A replica serves no replicas (cmd_replsync): what it would stream is its
  // own WAL, whose sequence numbers no other server shares, so a replica of
  // it would go past the point where the others could continue (as an old
  // primary's replicas would, re-pointed after a failover), and sync in full.
  for (const auto& e : ctx_.clients.all()) {
    if (e->replica && e->kill) e->kill();
  }
  replica_link_ = std::make_unique<ReplicaLink>(ctx_, io_, host, port, ports_[0] ? ports_[0] : ctx_.config.port);
  replica_link_->start();
  log_info("replication: following {}:{}", host, port);
  return {};
}

asio::io_context& Server::connection_context() {
  size_t n = contexts_.size() - 1;
  size_t pick = next_context_++ % n;
  if (n > 1 && pick < threads_.size() && ctx_.scripts.running() && threads_[pick] == ctx_.scripts.runner()) {
    pick = next_context_++ % n;
  }
  return *contexts_[pick + 1];
}

asio::awaitable<void> Server::accept_loop(tcp::acceptor acceptor, Protocol proto) {
  // The socket of a connection is made on its context: a socket completes its
  // operations on the context it belongs to, not on the handler's.
#ifdef _WIN32
  // Asio waits for a Windows socket to be readable with a zero-byte receive,
  // which fails on a listener: the context is chosen before the connection
  // comes, so a script may have started on it meanwhile.
  for (;;) {
    asio::io_context& io = connection_context();
    auto [ec, accepted] = co_await acceptor.async_accept(io, kUseAwaitable);
    if (ec == asio::error::operation_aborted) co_return;
    if (ec) {
      log_warn("accept failed: {}", ec.message());
      continue;
    }
    tcp::socket socket(std::move(accepted));
#else
  // The context is chosen once the connection is there.
  acceptor.non_blocking(true);
  for (;;) {
    auto [ec] = co_await acceptor.async_wait(tcp::acceptor::wait_read, kUseAwaitable);
    if (ec == asio::error::operation_aborted) co_return;
    asio::io_context& io = connection_context();
    tcp::socket socket(io);
    if (!ec) acceptor.accept(socket, ec);
    if (ec == asio::error::would_block || ec == asio::error::try_again) continue;
    if (ec) {
      log_warn("accept failed: {}", ec.message());
      continue;
    }
#endif
    asio::error_code ignored;
    socket.set_option(tcp::no_delay(true), ignored);
    // Each connection runs on its own strand: its operations never run
    // concurrently (others post to it: wake-ups, kills).
    auto strand = asio::make_strand(io);
    switch (proto) {
      case Protocol::Redis: asio::co_spawn(strand, redis_session(std::move(socket)), asio::detached); break;
      case Protocol::RedisTls: asio::co_spawn(strand, redis_tls_session(std::move(socket)), asio::detached); break;
      case Protocol::Memcache: asio::co_spawn(strand, memcache_session(std::move(socket)), asio::detached); break;
      case Protocol::Metrics: break;  // metrics_loop
    }
  }
}

asio::awaitable<void> Server::redis_session(tcp::socket socket) {
  ConnectionScope scope(ctx_.stats);
  ClientState client;
  client.id = ctx_.stats.next_client_id.fetch_add(1, std::memory_order_relaxed) + 1;
  start_session(ctx_, client);
  client.addr = peer_address(socket);
  if (protected_mode_refuses(ctx_, peer_ip(socket))) {
    log_debug("redis client {} from {} refused: protected mode", client.id, client.addr);
    std::string reply = std::format("-{}\r\n", kProtectedModeError);
    co_await asio::async_write(socket, asio::buffer(reply), kUseAwaitable);
    co_return;
  }
  log_debug("redis client {} connected from {}", client.id, client.addr);
  co_await serve_redis(socket, client);
  log_debug("redis client {} disconnected", client.id);
}

asio::awaitable<void> Server::redis_tls_session(tcp::socket socket) {
  using namespace asio::experimental::awaitable_operators;
  ConnectionScope scope(ctx_.stats);
  std::shared_ptr<TlsContext> tls = ctx_.tls();  // kept alive for the whole connection
  if (!tls) co_return;
  std::string addr = peer_address(socket);
  std::string ip = peer_ip(socket);
  asio::ssl::stream<tcp::socket> stream(std::move(socket), tls->ssl());
  asio::steady_timer timer(co_await asio::this_coro::executor, kTlsHandshakeTimeout);
  // `||` waits for the first operation that *succeeds*, so the handshake reports
  // its error as a value: a failed handshake must not wait for the timer.
  auto first = co_await (stream.async_handshake(asio::ssl::stream_base::server, kUseAwaitable) ||
                         timer.async_wait(kUseAwaitable));
  if (first.index() != 0) {
    ctx_.stats.tls_handshake_failures.fetch_add(1, std::memory_order_relaxed);
    log_debug("TLS client {}: handshake timed out", addr);
    co_return;
  }
  if (auto [ec] = std::get<0>(first); ec) {
    ctx_.stats.tls_handshake_failures.fetch_add(1, std::memory_order_relaxed);
    log_debug("TLS client {}: handshake failed: {}", addr, ec.message());
    co_return;
  }
  ctx_.stats.tls_connections.fetch_add(1, std::memory_order_relaxed);
  if (protected_mode_refuses(ctx_, ip, !verified_peer_cn(stream.native_handle()).empty())) {
    log_debug("TLS client {} refused: protected mode", addr);
    std::string reply = std::format("-{}\r\n", kProtectedModeError);
    co_await asio::async_write(stream, asio::buffer(reply), kUseAwaitable);
    co_return;
  }

  ClientState client;
  client.id = ctx_.stats.next_client_id.fetch_add(1, std::memory_order_relaxed) + 1;
  client.addr = addr;
  client.tls = true;
  start_session(ctx_, client);
  // tls-auth-clients-user cn: a verified certificate logs in as the user of its CN.
  if (tls->cn_logs_in()) {
    std::string cn = verified_peer_cn(stream.native_handle());
    if (auto user = cn.empty() ? nullptr : ctx_.acl.find(cn); user && user->enabled) {
      client.user = std::move(user);
      client.authenticated = true;
      log_debug("TLS client {} logged in as '{}' by certificate", addr, cn);
    }
  }
  log_debug("TLS redis client {} connected from {}", client.id, client.addr);
  co_await serve_redis(stream, client);
  log_debug("TLS redis client {} disconnected", client.id);
}

struct OutputPump {
  std::shared_ptr<asio::steady_timer> signal;  // cancelled when output arrives
  std::shared_ptr<asio::steady_timer> done;    // cancelled when the pump ends
  bool finished = false;
};

namespace {

// Sends what the subscriber holds, in order, until it is closed and empty or
// the peer is gone; then closes the socket, so the request loop ends too.
template <class Stream>
asio::awaitable<void> pump_output(Stream& socket, std::shared_ptr<Subscriber> sub,
                                  std::shared_ptr<OutputPump> pump, ServerStats& stats) {
  std::string buf;
  for (;;) {
    buf.clear();
    if (!sub->take(&buf)) {
      if (sub->closed()) break;
      // A notification posted meanwhile runs after this wait starts (same strand).
      pump->signal->expires_at(asio::steady_timer::time_point::max());
      co_await pump->signal->async_wait(kUseAwaitable);
      continue;
    }
    auto [ec, n] = co_await asio::async_write(socket, asio::buffer(buf), kUseAwaitable);
    NetOutput{&stats}(n);
    if (ec) {
      sub->close();
      break;
    }
  }
  if (sub->overflowed()) log_info("a Pub/Sub client fell more than {} bytes behind: disconnected", Subscriber::kMaxPending);
  asio::error_code ignored;
  socket.lowest_layer().close(ignored);
  pump->finished = true;
  pump->done->cancel();
}

}  // namespace

template <class Stream>
asio::awaitable<bool> Server::wait_for_keys(Stream& socket, std::string& in, std::shared_ptr<asio::steady_timer>& timer,
                                            ClientState& client) {
  using namespace asio::experimental::awaitable_operators;
  auto ex = co_await asio::this_coro::executor;
  if (!timer) timer = std::make_shared<asio::steady_timer>(ex);
  if (client.block_deadline_ms == std::numeric_limits<int64_t>::max()) {
    timer->expires_at(asio::steady_timer::time_point::max());
  } else {
    timer->expires_at(std::chrono::steady_clock::time_point(std::chrono::milliseconds(client.block_deadline_ms)));
  }
  std::shared_ptr<KeyWatch::Waiter> waiter = client.key_waiter;
  // Posted to this connection's strand: it cannot run before the wait starts.
  waiter->set_wake([t = timer, ex] { asio::post(ex, [t] { t->cancel(); }); });
  auto& sock = socket.lowest_layer();
  bool alive = true, watch_socket = true;
  while (!waiter->fired() && steady_time_ms() < client.block_deadline_ms) {
    if (!watch_socket) {
      co_await timer->async_wait(kUseAwaitable);
      break;
    }
    // The socket is watched too: a client that goes away while blocked must
    // not take an element it will never receive.
    auto first = co_await (timer->async_wait(kUseAwaitable) || sock.async_wait(tcp::socket::wait_read, kUseAwaitable));
    if (first.index() == 0) break;  // woken, or the deadline
    auto [wait_ec] = std::get<1>(first);
    asio::error_code ec;
    if (wait_ec || sock.available(ec) == 0 || ec) {
      alive = false;  // closed (readable with nothing to read)
      break;
    }
    if (!co_await read_more(socket, in, NetInput{&ctx_.stats})) {
      alive = false;
      break;
    }
    if (in.size() > kMaxInputWhileBlocked) watch_socket = false;
  }
  for (const auto& k : client.wait_keys) ctx_.db.key_watch().unwait(k, waiter.get());
  ctx_.repl.forget_waiter(waiter.get());  // WAIT waits on replication acknowledgments
  client.wait_keys.clear();
  client.key_waiter.reset();
  co_return alive;
}

template <class Stream>
asio::awaitable<void> Server::serve_redis(Stream& socket, ClientState& client) {
  // CLIENT LIST shows the connection; CLIENT KILL closes it, on its own
  // strand, and only while it is alive (the closure holds it weakly).
  auto ex = co_await asio::this_coro::executor;
  client.entry = ctx_.clients.add(client.id);
  publish_identity(client);
  auto* sock = &socket.lowest_layer();
  auto alive = std::make_shared<int>(0);
  client.entry->kill = [ex, sock, w = std::weak_ptr<int>(alive)] {
    asio::post(ex, [sock, w] {
      if (w.lock()) {
        asio::error_code ignored;
        sock->close(ignored);
      }
    });
  };
  std::shared_ptr<OutputPump> pump;
  co_await serve_requests(socket, client, pump);
  ctx_.clients.remove(client.id);  // a CLIENT KILL still running finds `alive` gone
  if (client.subscriber) {
    ctx_.pubsub.unsubscribe_all(*client.subscriber);
    client.subscriber->close();  // the pump sends what is left, then stops
    if (pump && !pump->finished) {
      pump->done->expires_at(asio::steady_timer::time_point::max());
      co_await pump->done->async_wait(kUseAwaitable);
    }
  }
}

template <class Stream>
asio::awaitable<void> Server::serve_requests(Stream& socket, ClientState& client, std::shared_ptr<OutputPump>& pump) {
  try {
    std::string in, out;
    std::vector<std::string_view> args;
    std::string error;
    Reply reply(out, client.proto);
    std::vector<std::string_view> ahead;
    std::shared_ptr<asio::steady_timer> write_timer;  // for commands that wait for writes
    client.can_block = true;
    ReadPool* pool = ctx_.read_pool;
    bool defer = pool != nullptr;  // for the next command
    while (!client.close_after_reply && co_await read_more(socket, in, NetInput{&ctx_.stats})) {
      size_t pos = 0;
      bool more = true;
      while (more) {
        more = false;
        bool blocked = false;
        bool replicate = false;
        {
          // The writes of the requests parsed here reach RocksDB as one batch,
          // committed before any reply is sent.
          Database::WriteBatchScope batch(ctx_.db, ctx_.write_batching);
          for (;;) {
            size_t start = pos;
            auto st = RespParser::parse(in, pos, args, error);
            if (st == RespParser::Status::Incomplete) {
              // Before AUTH only small requests are accepted (as Redis does).
              if (!client.authenticated && in.size() - pos > kMaxUnauthenticatedRequest) {
                reply.error("ERR Protocol error: unauthenticated request too big");
                client.close_after_reply = true;
              }
              break;
            }
            if (st == RespParser::Status::Error) {
              reply.error(error);
              client.close_after_reply = true;
              break;
            }
            if (args.empty()) continue;
            // A plain read whose key is not cached gives the thread back
            // instead of reading the disk on it (retry_later, below).
            client.defer_blocked_reads = defer;
            execute_redis_command(ctx_, client, args, reply);
            if (client.start_replication) {  // REPLSYNC: this connection is a replica's stream now
              if (client.entry) client.entry->replica = true;
              replicate = true;
              break;
            }
            if (client.retry_later) {
              client.retry_later = false;
              pos = start;
              blocked = more = true;
              break;
            }
            defer = pool != nullptr;
            if (client.close_after_reply) break;
            if (out.size() >= kMaxPendingOutput) {
              more = true;
              break;
            }
          }
          batch.commit();
        }
        // `args` point into `in`, which is not touched while flushing.
        if (client.subscriber) {
          // Subscribed: the replies go out through the pump, in order with the messages.
          if (!pump) {
            auto ex = co_await asio::this_coro::executor;
            pump = std::make_shared<OutputPump>();
            pump->signal = std::make_shared<asio::steady_timer>(ex);
            pump->done = std::make_shared<asio::steady_timer>(ex);
            client.subscriber->set_notify([s = pump->signal, ex] { asio::post(ex, [s] { s->cancel(); }); });
            asio::co_spawn(ex, pump_output(socket, client.subscriber, pump, ctx_.stats), asio::detached);
          }
          client.subscriber->push(out);
          out.clear();
          if (pump->finished) co_return;
        } else if (!co_await flush(socket, out, NetOutput{&ctx_.stats})) {
          co_return;
        }
        if (replicate) {
          co_await serve_replica(ctx_, socket, client, in.substr(pos));
          co_return;
        }
        if (blocked && client.wait_for_script) {
          // A long script has the keyspace: wait for its end, then run the command.
          client.wait_for_script = false;
          int64_t deadline = std::exchange(client.block_deadline_ms, std::numeric_limits<int64_t>::max());
          bool alive = co_await wait_for_keys(socket, in, write_timer, client);
          client.block_deadline_ms = deadline;
          if (!alive) co_return;
        } else if (blocked && client.wait_for_keys) {
          client.wait_for_keys = false;
          if (client.entry) client.entry->blocked = true;
          bool alive = co_await wait_for_keys(socket, in, write_timer, client);
          if (client.entry) client.entry->blocked = false;
          if (!alive) co_return;
        } else if (blocked && client.wait_for_writes) {
          client.wait_for_writes = false;
          if (client.entry) client.entry->blocked = true;
          co_await wait_for_write(ctx_.db.storage(), write_timer, client.wait_after_seq, client.block_deadline_ms);
          if (client.entry) client.entry->blocked = false;
        } else if (blocked) {
          // The earlier replies are out. Wait for this one's key while the
          // thread serves others; behind it, start loading the other keys of
          // this command and of the plain reads further down the pipeline,
          // so their disk reads overlap.
          auto prefetch = [&] {
            size_t p = pos;
            for (int n = 0; n < kMaxPrefetchCommands; ++n) {
              if (RespParser::parse(in, p, ahead, error) != RespParser::Status::Ok) break;
              uncached_read_keys(ctx_, client, ahead, [&](std::string_view key) {
                if (key != client.load_key) pool->load(key, nullptr);
              });
            }
          };
          auto [cached] = co_await async_load(*pool, client.load_key, prefetch, kUseAwaitable);
          // A key that is still not cached (missing, too large) is read on this thread.
          defer = cached;
        }
      }
      consume(in, pos);
    }
  } catch (const std::exception& e) {
    log_debug("redis client {} error: {}", client.id, e.what());
  }
}

asio::awaitable<void> Server::memcache_session(tcp::socket socket) {
  ConnectionScope scope(ctx_.stats);
  if (protected_mode_refuses(ctx_, peer_ip(socket))) {
    log_debug("memcache client {} refused: protected mode", peer_address(socket));
    std::string reply = std::format("SERVER_ERROR {}\r\n", kProtectedModeError.substr(7));  // without "DENIED "
    co_await asio::async_write(socket, asio::buffer(reply), kUseAwaitable);
    co_return;
  }
  try {
    MemcacheSession session(ctx_);
    std::string in, out;
    bool close = false;
    while (!close && co_await read_more(socket, in, NetInput{&ctx_.stats})) {
      size_t pos = 0;
      bool more = true;
      while (more) {
        more = false;
        {
          Database::WriteBatchScope batch(ctx_.db, ctx_.write_batching);
          for (;;) {
            auto st = session.process(in, pos, out);
            if (st == MemcacheSession::Status::Incomplete) break;
            if (st == MemcacheSession::Status::Close) {
              close = true;
              break;
            }
            if (out.size() >= kMaxPendingOutput) {
              more = true;
              break;
            }
          }
          batch.commit();
        }
        if (!co_await flush(socket, out, NetOutput{&ctx_.stats})) co_return;
      }
      consume(in, pos);
    }
  } catch (const std::exception& e) {
    log_debug("memcache client error: {}", e.what());
  }
}

asio::awaitable<void> Server::sample_rates() {
  asio::steady_timer timer(io_);
  ServerStats& s = ctx_.stats;
  uint64_t ops = s.total_commands(), in = s.total_net_input_bytes(), out = s.total_net_output_bytes();
  auto last = std::chrono::steady_clock::now();
  for (;;) {
    timer.expires_after(std::chrono::seconds(1));
    auto [ec] = co_await timer.async_wait(kUseAwaitable);
    if (ec) co_return;
    auto now = std::chrono::steady_clock::now();
    double seconds = std::chrono::duration<double>(now - last).count();
    last = now;
    auto rate = [&](uint64_t& prev, uint64_t total) {
      uint64_t delta = total - std::exchange(prev, total);
      return static_cast<uint64_t>(static_cast<double>(delta) / std::max(seconds, 0.001));
    };
    s.instantaneous_ops.store(rate(ops, s.total_commands()), std::memory_order_relaxed);
    s.instantaneous_input_bps.store(rate(in, s.total_net_input_bytes()), std::memory_order_relaxed);
    s.instantaneous_output_bps.store(rate(out, s.total_net_output_bytes()), std::memory_order_relaxed);
  }
}

asio::awaitable<void> Server::metrics_loop(tcp::acceptor acceptor) {
  for (;;) {
    auto [ec, socket] = co_await acceptor.async_accept(kUseAwaitable);
    if (ec == asio::error::operation_aborted) co_return;
    if (ec) {
      log_warn("metrics: accept failed: {}", ec.message());
      continue;
    }
    asio::co_spawn(io_, http_session(std::move(socket)), asio::detached);
  }
}

asio::awaitable<void> Server::http_session(tcp::socket socket) {
  using namespace asio::experimental::awaitable_operators;
  // A request line and headers, in this long, in this much.
  constexpr auto kTimeout = std::chrono::seconds(5);
  constexpr size_t kMaxRequest = 8 * 1024;
  std::string in;
  asio::steady_timer timer(io_, kTimeout);
  while (in.find("\r\n\r\n") == std::string::npos && in.find("\n\n") == std::string::npos) {
    if (in.size() > kMaxRequest) co_return;
    auto got = co_await (read_more(socket, in) || timer.async_wait(kUseAwaitable));
    if (got.index() != 0 || !std::get<0>(got)) co_return;  // timed out, or gone
  }
  HttpResponse r = metrics_http(ctx_, std::string_view(in).substr(0, in.find_first_of("\r\n")));
  std::string reply = std::format(
      "HTTP/1.1 {}\r\nContent-Type: {}\r\nContent-Length: {}\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n",
      r.status, r.content_type, r.body.size());
  if (!r.head_only) reply += r.body;
  co_await asio::async_write(socket, asio::buffer(reply), kUseAwaitable);
  asio::error_code ignored;
  socket.shutdown(tcp::socket::shutdown_both, ignored);
}

}  // namespace datadam
