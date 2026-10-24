#pragma once

#include <asio.hpp>

#include <cstdint>
#include <memory>
#include <string>

#include "server/redis_commands.h"

namespace datadam {

// The replica side of replication (see server/replication.h): connects to
// the primary, syncs, applies its stream to the database, and reconnects a
// second after the link drops, until stop().
class ReplicaLink {
 public:
  ReplicaLink(ServerContext& ctx, asio::io_context& io, std::string host, uint16_t port, uint16_t own_port);
  ~ReplicaLink();  // stop()
  ReplicaLink(const ReplicaLink&) = delete;
  ReplicaLink& operator=(const ReplicaLink&) = delete;

  void start();
  // Ends the link and closes its connection; callable from any thread.
  void stop();

  struct Shared;  // what the link's coroutine shares with stop()

 private:
  std::shared_ptr<Shared> shared_;
};

// The primary side: streams to the replica that sent REPLSYNC on this
// connection (client.repl_id and client.repl_offset). `in` is what the
// replica sent after REPLSYNC. Returns when the replica goes away or falls
// behind the kept WAL.
template <class Stream>
asio::awaitable<void> serve_replica(ServerContext& ctx, Stream& stream, ClientState& client, std::string in);

}  // namespace datadam
