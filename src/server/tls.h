#pragma once

#include <memory>
#include <string>

#include "config/config.h"

namespace asio::ssl {
class context;
}  // namespace asio::ssl

namespace datadam {

// The tls-* settings turned into an OpenSSL context for new connections.
// CONFIG SET builds a new one and swaps it in; connections already open keep
// the one they started with.
class TlsContext {
 public:
  // Returns null and sets *error if a file cannot be loaded or a setting is invalid.
  static std::shared_ptr<TlsContext> create(const Config& cfg, std::string* error);
  ~TlsContext();
  TlsContext(const TlsContext&) = delete;
  TlsContext& operator=(const TlsContext&) = delete;

  asio::ssl::context& ssl();
  // A verified client certificate logs in as the ACL user named by its CN.
  bool cn_logs_in() const { return cn_logs_in_; }

 private:
  TlsContext();
  std::unique_ptr<asio::ssl::context> ctx_;
  bool cn_logs_in_ = false;
};

// The context a replica connects to its primary with (tls-replication): it
// verifies the primary's certificate with tls-ca-cert-file (the system's CAs
// when empty) and presents tls-client-cert-file, or tls-cert-file, if set.
// Null with *error if a file cannot be loaded.
std::shared_ptr<asio::ssl::context> replication_client_tls(const Config& cfg, std::string* error);

// The common name of a connection's client certificate, if it sent one and
// it was verified; "" otherwise. `ssl` is the connection's SSL*.
std::string verified_peer_cn(void* ssl);

}  // namespace datadam
