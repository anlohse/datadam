#pragma once

#include <asio.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "server/read_pool.h"
#include "server/replica_link.h"
#include "server/redis_commands.h"

namespace datadam {

// TCP front end: io-threads threads, each running an io_context of its own; every
// connection is a coroutine. The thread that calls run() accepts the
// connections. Listens for RESP, RESP over TLS (tls-port) and memcache, each
// optional.
struct OutputPump;

class Server {
 public:
  Server(ServerContext& ctx);
  ~Server();

  // Makes this server a replica of host:port, or a primary with an empty
  // host (REPLICAOF). Returns an error message or "".
  std::string follow(const std::string& host, uint16_t port);

  // Binds the listeners; throws on failure. With ephemeral_ports every
  // enabled listener gets a free port from the OS instead (tests).
  void start(bool ephemeral_ports = false);
  // Runs until stop() or SIGINT/SIGTERM.
  void run();
  void stop();

  // The ports actually bound (0: not listening).
  uint16_t port() const { return ports_[0]; }
  uint16_t tls_port() const { return ports_[1]; }
  uint16_t memcache_port() const { return ports_[2]; }
  uint16_t metrics_port() const { return ports_[3]; }

 private:
  enum class Protocol { Redis = 0, RedisTls = 1, Memcache = 2, Metrics = 3 };

  asio::awaitable<void> accept_loop(asio::ip::tcp::acceptor acceptor, Protocol proto);
  // The io_context of the next connection (accept loops only).
  asio::io_context& connection_context();
  asio::awaitable<void> redis_session(asio::ip::tcp::socket socket);
  asio::awaitable<void> redis_tls_session(asio::ip::tcp::socket socket);
  asio::awaitable<void> memcache_session(asio::ip::tcp::socket socket);
  // metrics-port: GET /metrics, /health and /ready over HTTP/1.x, one request
  // per connection, on the control context.
  asio::awaitable<void> metrics_loop(asio::ip::tcp::acceptor acceptor);
  asio::awaitable<void> http_session(asio::ip::tcp::socket socket);
  // Every second: the instantaneous_* rates of INFO stats.
  asio::awaitable<void> sample_rates();
  // The request loop of a Redis connection, over TCP or TLS.
  template <class Stream>
  asio::awaitable<void> serve_redis(Stream& stream, ClientState& client);
  // Once a connection subscribed (Pub/Sub), its output is sent by a writer of
  // its own (pump), so published messages go out while it waits for requests.
  template <class Stream>
  asio::awaitable<void> serve_requests(Stream& stream, ClientState& client, std::shared_ptr<OutputPump>& pump);
  // A blocked command (BLPOP...) waits for a write to its keys or its
  // timeout; false if the client went away meanwhile. Requests it sends
  // meanwhile are kept in `in`.
  template <class Stream>
  asio::awaitable<bool> wait_for_keys(Stream& stream, std::string& in, std::shared_ptr<asio::steady_timer>& timer,
                                      ClientState& client);

  // FAILOVER: checks and starts the hand-over (run_failover); "" or an error.
  std::string failover(const std::string& host, uint16_t port, int64_t timeout_ms, bool force, bool abort);
  std::string start_failover(const ReplicationState::Replica& target, int64_t timeout_ms, bool force);
  // promotion-handoff, on the primary: a replica being promoted asks for the hand-over.
  void hand_over(uint64_t replica);
  // follow() with follow_mu_ held.
  std::string follow_locked(const std::string& host, uint16_t port);
  asio::awaitable<void> run_failover(uint64_t replica, std::string ip, uint16_t port, int64_t timeout_ms, bool force);

  ServerContext& ctx_;
  std::atomic<bool> failover_running_{false};
  std::atomic<bool> failover_abort_{false};
  // One io_context per io-thread, each run by its own thread. A single one
  // shared by all the threads made every completion go through one scheduler
  // lock and one epoll: 16 threads did barely more than 8, with a fifth of the
  // CPU idle and twice the system time. Connections go to them round-robin,
  // past the one running a script: a connection there waits for its end, so
  // SCRIPT KILL could not come through it.
  // contexts_[0] serves no connection: the listeners, signals, replication and
  // FAILOVER run there, on the thread that calls run(), and a long command
  // cannot hold them up.
  std::vector<std::unique_ptr<asio::io_context>> contexts_;
  std::vector<asio::executor_work_guard<asio::io_context::executor_type>> work_;
  std::vector<std::thread::id> threads_;  // of contexts_[i + 1], set by run()
  asio::io_context& io_;                  // contexts_[0]
  size_t next_context_ = 0;               // only the accept loops (on io_) use it
  asio::signal_set signals_;
  // After io_: destroyed first, finishing its loads while their completions
  // can still be posted.
  std::unique_ptr<ReadPool> read_pool_;
  std::mutex follow_mu_;
  std::unique_ptr<ReplicaLink> replica_link_;  // when this is a replica
  // promotion-handoff, on a replica: REPLICAOF NO ONE asked the primary to
  // hand over; the answer, or handoff_timer_, promotes this server. Under
  // follow_mu_, as follow_gen_, which every follow() moves on.
  bool handoff_pending_ = false;
  uint64_t follow_gen_ = 0;
  asio::steady_timer handoff_timer_;
  uint16_t ports_[4] = {0, 0, 0, 0};
};

}  // namespace datadam
