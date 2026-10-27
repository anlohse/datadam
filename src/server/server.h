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

 private:
  enum class Protocol { Redis = 0, RedisTls = 1, Memcache = 2 };

  asio::awaitable<void> accept_loop(asio::ip::tcp::acceptor acceptor, Protocol proto);
  asio::awaitable<void> redis_session(asio::ip::tcp::socket socket);
  asio::awaitable<void> redis_tls_session(asio::ip::tcp::socket socket);
  asio::awaitable<void> memcache_session(asio::ip::tcp::socket socket);
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
  uint16_t ports_[3] = {0, 0, 0};
};

}  // namespace datadam
