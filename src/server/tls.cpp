#include "server/tls.h"

#include <asio/ssl.hpp>
#include <format>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "util/strings.h"

namespace datadam {

namespace {

// The most recent OpenSSL error, for messages.
std::string openssl_error() {
  unsigned long e = ERR_get_error();
  if (e == 0) return "unknown error";
  char buf[256];
  ERR_error_string_n(e, buf, sizeof(buf));
  ERR_clear_error();
  return buf;
}

}  // namespace

TlsContext::TlsContext() = default;
TlsContext::~TlsContext() = default;

asio::ssl::context& TlsContext::ssl() { return *ctx_; }

std::shared_ptr<TlsContext> TlsContext::create(const Config& cfg, std::string* error) {
  std::shared_ptr<TlsContext> t(new TlsContext());
  t->ctx_ = std::make_unique<asio::ssl::context>(asio::ssl::context::tls_server);
  asio::ssl::context& ctx = *t->ctx_;
  SSL_CTX* native = ctx.native_handle();
  ctx.set_options(asio::ssl::context::default_workarounds | asio::ssl::context::single_dh_use);
  // As Redis: a client may not renegotiate (each one costs a handshake's CPU).
  // OpenSSL 3 refuses it by default; this keeps it so with 1.1.
  SSL_CTX_set_options(native, SSL_OP_NO_RENEGOTIATION);

  // Allowed versions: the lowest and highest of tls-protocols.
  bool v12 = cfg.tls_protocols.find("TLSv1.2") != std::string::npos;
  bool v13 = cfg.tls_protocols.find("TLSv1.3") != std::string::npos;
  SSL_CTX_set_min_proto_version(native, v12 ? TLS1_2_VERSION : TLS1_3_VERSION);
  SSL_CTX_set_max_proto_version(native, v13 ? TLS1_3_VERSION : TLS1_2_VERSION);

  auto fail = [&](std::string_view what, const std::string& file) {
    *error = std::format("{} '{}': {}", what, file, openssl_error());
    return nullptr;
  };
  asio::error_code ec;
  ctx.use_certificate_chain_file(cfg.tls_cert_file, ec);
  if (ec) return fail("cannot load tls-cert-file", cfg.tls_cert_file);
  ctx.use_private_key_file(cfg.tls_key_file, asio::ssl::context::pem, ec);
  if (ec) return fail("cannot load tls-key-file", cfg.tls_key_file);
  if (SSL_CTX_check_private_key(native) != 1) return fail("tls-key-file does not match the certificate", cfg.tls_key_file);
  if (!cfg.tls_ca_cert_file.empty()) {
    ctx.load_verify_file(cfg.tls_ca_cert_file, ec);
    if (ec) return fail("cannot load tls-ca-cert-file", cfg.tls_ca_cert_file);
  }
  if (!cfg.tls_ciphers.empty() && SSL_CTX_set_cipher_list(native, cfg.tls_ciphers.c_str()) != 1) {
    return fail("invalid tls-ciphers", cfg.tls_ciphers);
  }
  if (!cfg.tls_ciphersuites.empty() && SSL_CTX_set_ciphersuites(native, cfg.tls_ciphersuites.c_str()) != 1) {
    return fail("invalid tls-ciphersuites", cfg.tls_ciphersuites);
  }

  if (cfg.tls_auth_clients == "yes") {
    ctx.set_verify_mode(asio::ssl::verify_peer | asio::ssl::verify_fail_if_no_peer_cert);
  } else if (cfg.tls_auth_clients == "optional") {
    ctx.set_verify_mode(asio::ssl::verify_peer);
  } else {
    ctx.set_verify_mode(asio::ssl::verify_none);
  }
  t->cn_logs_in_ = cfg.tls_auth_clients_user == "cn";
  return t;
}

std::shared_ptr<asio::ssl::context> replication_client_tls(const Config& cfg, std::string* error) {
  auto ctx = std::make_shared<asio::ssl::context>(asio::ssl::context::tls_client);
  SSL_CTX* native = ctx->native_handle();
  bool v12 = cfg.tls_protocols.find("TLSv1.2") != std::string::npos;
  bool v13 = cfg.tls_protocols.find("TLSv1.3") != std::string::npos;
  SSL_CTX_set_min_proto_version(native, v12 ? TLS1_2_VERSION : TLS1_3_VERSION);
  SSL_CTX_set_max_proto_version(native, v13 ? TLS1_3_VERSION : TLS1_2_VERSION);
  auto fail = [&](std::string_view what, const std::string& file) {
    *error = std::format("{} '{}': {}", what, file, openssl_error());
    return nullptr;
  };
  asio::error_code ec;
  if (cfg.tls_ca_cert_file.empty()) {
    ctx->set_default_verify_paths(ec);
  } else {
    ctx->load_verify_file(cfg.tls_ca_cert_file, ec);
    if (ec) return fail("cannot load tls-ca-cert-file", cfg.tls_ca_cert_file);
  }
  ctx->set_verify_mode(asio::ssl::verify_peer);
  const std::string& cert = cfg.tls_client_cert_file.empty() ? cfg.tls_cert_file : cfg.tls_client_cert_file;
  const std::string& key = cfg.tls_client_cert_file.empty() ? cfg.tls_key_file : cfg.tls_client_key_file;
  if (!cert.empty()) {
    ctx->use_certificate_chain_file(cert, ec);
    if (ec) return fail("cannot load the replication client certificate", cert);
    ctx->use_private_key_file(key, asio::ssl::context::pem, ec);
    if (ec) return fail("cannot load the replication client key", key);
    if (SSL_CTX_check_private_key(native) != 1) return fail("the client key does not match its certificate", key);
  }
  return ctx;
}

std::string verified_peer_cn(void* ssl_handle) {
  SSL* ssl = static_cast<SSL*>(ssl_handle);
  if (SSL_get_verify_result(ssl) != X509_V_OK) return {};
  X509* cert = SSL_get1_peer_certificate(ssl);
  if (!cert) return {};
  char cn[256] = {};
  int n = X509_NAME_get_text_by_NID(X509_get_subject_name(cert), NID_commonName, cn, sizeof(cn));
  X509_free(cert);
  return n > 0 ? std::string(cn, static_cast<size_t>(n)) : std::string();
}

}  // namespace datadam
