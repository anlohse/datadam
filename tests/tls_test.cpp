// TLS end to end: a real server on ephemeral ports, real TLS clients, and
// certificates made here with the OpenSSL API. Also ACL LOG.

#include <gtest/gtest.h>

#include <asio.hpp>
#include <asio/ssl.hpp>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <random>
#include <thread>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include "cache/object_cache.h"
#include "db/database.h"
#include "server/server.h"
#include "storage/rocks_storage.h"

using namespace datadam;
namespace fs = std::filesystem;
using asio::ip::tcp;

namespace {

struct KeyPair {
  EVP_PKEY* key = nullptr;
  X509* cert = nullptr;
  ~KeyPair() {
    X509_free(cert);
    EVP_PKEY_free(key);
  }
};

// A certificate for `cn`, signed by `issuer` (or self-signed when null).
std::unique_ptr<KeyPair> make_cert(const std::string& cn, const KeyPair* issuer, bool ca) {
  auto kp = std::make_unique<KeyPair>();
  kp->key = EVP_EC_gen("P-256");
  kp->cert = X509_new();
  X509_set_version(kp->cert, 2);
  static long serial = 1;
  ASN1_INTEGER_set(X509_get_serialNumber(kp->cert), serial++);
  X509_gmtime_adj(X509_getm_notBefore(kp->cert), -60);
  X509_gmtime_adj(X509_getm_notAfter(kp->cert), 3600);
  X509_set_pubkey(kp->cert, kp->key);
  X509_NAME* name = X509_get_subject_name(kp->cert);
  X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>(cn.c_str()), -1, -1, 0);
  X509_set_issuer_name(kp->cert, issuer ? X509_get_subject_name(issuer->cert) : name);
  // The extensions a real CA puts in (OpenSSL checks them when it builds the chain).
  X509V3_CTX v3;
  X509V3_set_ctx(&v3, issuer ? issuer->cert : kp->cert, kp->cert, nullptr, nullptr, 0);
  auto add = [&](int nid, const char* value) {
    X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &v3, nid, value);
    X509_add_ext(kp->cert, ext, -1);
    X509_EXTENSION_free(ext);
  };
  add(NID_basic_constraints, ca ? "critical,CA:TRUE" : "CA:FALSE");
  add(NID_subject_key_identifier, "hash");
  add(NID_authority_key_identifier, "keyid:always");
  add(NID_key_usage, ca ? "critical,keyCertSign,cRLSign" : "critical,digitalSignature,keyEncipherment");
  if (!ca) {
    add(NID_ext_key_usage, "serverAuth,clientAuth");
    add(NID_subject_alt_name, ("DNS:" + cn).c_str());
  }
  X509_sign(kp->cert, issuer ? issuer->key : kp->key, EVP_sha256());
  return kp;
}

// Through BIOs: OpenSSL's FILE* functions need the same C runtime as OpenSSL on Windows.
void write_pem(const KeyPair& kp, const fs::path& cert_file, const fs::path& key_file) {
  BIO* b = BIO_new_file(cert_file.string().c_str(), "wb");
  PEM_write_bio_X509(b, kp.cert);
  BIO_free(b);
  b = BIO_new_file(key_file.string().c_str(), "wb");
  PEM_write_bio_PrivateKey(b, kp.key, nullptr, nullptr, 0, nullptr, nullptr);
  BIO_free(b);
}

class TlsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() / ("datadam_tls_" + std::to_string(std::random_device{}()));
    fs::create_directories(dir_);
    auto ca = make_cert("test-ca", nullptr, true);
    auto server = make_cert("localhost", ca.get(), false);
    auto alice = make_cert("alice", ca.get(), false);
    auto other_ca = make_cert("other-ca", nullptr, true);
    auto mallory = make_cert("alice", other_ca.get(), false);  // right name, wrong CA
    write_pem(*ca, dir_ / "ca.crt", dir_ / "ca.key");
    write_pem(*server, dir_ / "server.crt", dir_ / "server.key");
    write_pem(*alice, dir_ / "alice.crt", dir_ / "alice.key");
    write_pem(*mallory, dir_ / "mallory.crt", dir_ / "mallory.key");

    StorageOptions so;
    so.dir = (dir_ / "data").string();
    so.fsync = FsyncPolicy::No;
    so.wal_retention_s = 3600;  // so a replica can follow it
    storage_ = RocksStorage::open(so);
    CacheOptions co;
    co.max_memory = 4 << 20;
    co.min_memory = 0;
    co.shards = 2;
    cache_ = std::make_unique<ObjectCache>(co);
    db_ = std::make_unique<Database>(*storage_, *cache_);

    config_.bind = "127.0.0.1";
    config_.io_threads = 2;
    config_.memcache_port = 0;
    config_.tls_port = 1;  // any non-zero: the ports are ephemeral
    config_.tls_cert_file = (dir_ / "server.crt").string();
    config_.tls_key_file = (dir_ / "server.key").string();
    config_.tls_ca_cert_file = (dir_ / "ca.crt").string();
    config_.tls_auth_clients = "optional";
    config_.tls_auth_clients_user = "cn";
    ASSERT_EQ(config_.validate(), "");
    ctx_ = std::make_unique<ServerContext>(*db_, config_, stats_, 0);
    server_ = std::make_unique<Server>(*ctx_);
    server_->start(true);
    thread_ = std::thread([this] { server_->run(); });
    if (std::string issuer = presented_issuer(); issuer != "/CN=test-ca") {
      GTEST_SKIP() << "TLS to 127.0.0.1 is intercepted here (certificate issued by '" << issuer
                   << "', an antivirus?): the TLS tests run in Docker";
    }
  }

  // Who signed the certificate the server presents, as the client sees it.
  std::string presented_issuer() {
    asio::io_context io;
    asio::ssl::context ssl(asio::ssl::context::tls_client);
    ssl.set_verify_mode(asio::ssl::verify_none);
    asio::ssl::stream<tcp::socket> s(io, ssl);
    s.lowest_layer().connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), server_->tls_port()));
    s.handshake(asio::ssl::stream_base::client);
    X509* cert = SSL_get1_peer_certificate(s.native_handle());
    char issuer[512] = {};
    if (cert) X509_NAME_oneline(X509_get_issuer_name(cert), issuer, sizeof(issuer));
    X509_free(cert);
    return issuer;
  }

  void TearDown() override {
    server_->stop();
    thread_.join();
    server_.reset();
    ctx_.reset();
    db_.reset();
    cache_.reset();
    storage_.reset();
    fs::remove_all(dir_);
  }

  // A TLS client; with a certificate if `cert` is given. Throws if the handshake fails.
  struct Client {
    asio::io_context io;
    asio::ssl::context ssl{asio::ssl::context::tls_client};
    std::unique_ptr<asio::ssl::stream<tcp::socket>> stream;

    std::string command(std::string_view resp) {
      asio::write(*stream, asio::buffer(resp));
      std::string out;
      char buf[4096];
      // Replies here are a single line or one bulk string: read until it is complete.
      for (;;) {
        size_t n = stream->read_some(asio::buffer(buf));
        out.append(buf, n);
        if (out.size() >= 2 && out.ends_with("\r\n")) {
          if (out[0] != '$' || out.starts_with("$-1")) break;
          size_t eol = out.find("\r\n");
          size_t len = std::stoul(out.substr(1, eol - 1));
          if (out.size() >= eol + 2 + len + 2) break;
        }
      }
      return out;
    }
  };

  std::unique_ptr<Client> connect(const std::string& cert = "") {
    auto c = std::make_unique<Client>();
    c->ssl.load_verify_file((dir_ / "ca.crt").string());  // the client checks the server too
    c->ssl.set_verify_mode(asio::ssl::verify_peer);
    if (!cert.empty()) {
      c->ssl.use_certificate_chain_file((dir_ / (cert + ".crt")).string());
      c->ssl.use_private_key_file((dir_ / (cert + ".key")).string(), asio::ssl::context::pem);
    }
    c->stream = std::make_unique<asio::ssl::stream<tcp::socket>>(c->io, c->ssl);
    c->stream->lowest_layer().connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), server_->tls_port()));
    c->stream->handshake(asio::ssl::stream_base::client);
    return c;
  }

  // A plain TCP command.
  std::string plain(std::string_view resp, uint16_t port) {
    asio::io_context io;
    tcp::socket s(io);
    s.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
    asio::write(s, asio::buffer(resp));
    char buf[4096];
    size_t n = s.read_some(asio::buffer(buf));
    return std::string(buf, n);
  }

  fs::path dir_;
  Config config_;
  ServerStats stats_;
  std::unique_ptr<RocksStorage> storage_;
  std::unique_ptr<ObjectCache> cache_;
  std::unique_ptr<Database> db_;
  std::unique_ptr<ServerContext> ctx_;
  std::unique_ptr<Server> server_;
  std::thread thread_;
};

}  // namespace

TEST_F(TlsTest, CommandsWorkOverTlsAndPlainSideBySide) {
  auto c = connect();
  EXPECT_EQ(c->command("PING\r\n"), "+PONG\r\n");
  EXPECT_EQ(c->command("*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$3\r\ntls\r\n"), "+OK\r\n");
  EXPECT_EQ(plain("*2\r\n$3\r\nGET\r\n$1\r\nk\r\n", server_->port()), "$3\r\ntls\r\n");
  EXPECT_NE(c->command("CLIENT INFO\r\n").find("tls=yes"), std::string::npos);
  std::string stats = c->command("INFO stats\r\n");
  EXPECT_NE(stats.find("tls_connections:"), std::string::npos);
  EXPECT_EQ(stats.find("tls_connections:0"), std::string::npos);
}

TEST_F(TlsTest, ClientCertificateLogsInByCommonName) {
  plain("*7\r\n$3\r\nACL\r\n$7\r\nSETUSER\r\n$5\r\nalice\r\n$2\r\non\r\n$6\r\n~app:*\r\n$5\r\n+@all\r\n$9\r\nresetpass\r\n",
        server_->port());
  plain("*3\r\n$6\r\nCONFIG\r\n$3\r\nSET\r\n$11\r\nrequirepass\r\n", server_->port());  // (arity error: ignored)
  plain("*4\r\n$6\r\nCONFIG\r\n$3\r\nSET\r\n$11\r\nrequirepass\r\n$2\r\npw\r\n", server_->port());
  auto anonymous = connect();
  EXPECT_EQ(anonymous->command("PING\r\n"), "-NOAUTH Authentication required.\r\n");
  auto alice = connect("alice");  // no password: the certificate is the login
  EXPECT_EQ(alice->command("ACL WHOAMI\r\n"), "$5\r\nalice\r\n");
  EXPECT_EQ(alice->command("GET other\r\n"), "-NOPERM No permissions to access a key\r\n");
  EXPECT_EQ(alice->command("GET app:1\r\n"), "$-1\r\n");
}

TEST_F(TlsTest, UntrustedOrMissingCertificatesAreRefused) {
  // With TLS 1.3 the client finishes its side of the handshake before the
  // server has checked its certificate: a refusal shows on the first read.
  auto refused = [&](const std::string& cert) {
    try {
      return connect(cert)->command("PING\r\n") != "+PONG\r\n";
    } catch (const std::exception&) {
      return true;
    }
  };
  EXPECT_TRUE(refused("mallory"));  // signed by a CA the server does not trust
  // Plain bytes on the TLS port are not a handshake.
  std::string garbage;
  try {
    garbage = plain("PING\r\n", server_->tls_port());
  } catch (const std::exception&) {
  }
  EXPECT_EQ(garbage.find("PONG"), std::string::npos);
  // tls-auth-clients yes: a certificate is required from now on.
  auto admin = connect("alice");
  ASSERT_EQ(admin->command("*4\r\n$6\r\nCONFIG\r\n$3\r\nSET\r\n$16\r\ntls-auth-clients\r\n$3\r\nyes\r\n"), "+OK\r\n");
  EXPECT_TRUE(refused(""));
  EXPECT_FALSE(refused("alice"));
  // Failed handshakes end at once (not after the 10 s handshake timeout).
  for (int i = 0; i < 100 && stats_.tls_handshake_failures.load() < 3; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_GE(stats_.tls_handshake_failures.load(), 3u);
}

TEST_F(TlsTest, ReplicaFollowsOverTls) {
  // A second server follows this one on its TLS port (tls-replication): it
  // checks the primary's certificate (CA and host name) and presents alice's.
  StorageOptions so;
  so.dir = (dir_ / "replica").string();
  so.fsync = FsyncPolicy::No;
  auto rstorage = RocksStorage::open(so);
  CacheOptions co;
  co.max_memory = 4 << 20;
  co.min_memory = 0;
  co.shards = 2;
  auto rcache = std::make_unique<ObjectCache>(co);
  auto rdb = std::make_unique<Database>(*rstorage, *rcache, true, false);
  rdb->set_read_only(true);
  Config rc;
  rc.bind = "127.0.0.1";
  rc.io_threads = 2;
  rc.memcache_port = 0;
  // The certificate names localhost, not 127.0.0.1: this one must be refused.
  rc.replicaof = std::format("127.0.0.1 {}", server_->tls_port());
  rc.tls_replication = true;
  rc.tls_ca_cert_file = (dir_ / "ca.crt").string();
  rc.tls_cert_file = (dir_ / "alice.crt").string();
  rc.tls_key_file = (dir_ / "alice.key").string();
  ServerStats rs;
  auto rctx = std::make_unique<ServerContext>(*rdb, rc, rs, 0);
  auto rserver = std::make_unique<Server>(*rctx);
  rserver->start(true);
  std::thread rthread([&] { rserver->run(); });
  uint16_t rport = rserver->port();
  auto info = [&] { return plain("*2\r\n$4\r\nINFO\r\n$11\r\nreplication\r\n", rport); };

  EXPECT_EQ(connect()->command("SET k v\r\n"), "+OK\r\n");
  std::this_thread::sleep_for(std::chrono::milliseconds(700));
  EXPECT_NE(info().find("master_link_status:down"), std::string::npos);  // host name mismatch
  std::string follow = std::format("*3\r\n$9\r\nREPLICAOF\r\n$9\r\nlocalhost\r\n${}\r\n{}\r\n",
                                   std::to_string(server_->tls_port()).size(), server_->tls_port());
  EXPECT_EQ(plain(follow, rport), "+OK\r\n");
  bool up = false;
  for (int i = 0; i < 300 && !up; ++i) {
    up = plain("*2\r\n$3\r\nGET\r\n$1\r\nk\r\n", rport) == "$1\r\nv\r\n";
    if (!up) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  EXPECT_TRUE(up) << info();
  EXPECT_NE(info().find("master_link_status:up"), std::string::npos);

  rserver->stop();
  rthread.join();
  rserver.reset();
  rctx.reset();
  rdb.reset();
  rcache.reset();
  rstorage.reset();
}

TEST_F(TlsTest, CertificatesReloadWithConfigSet) {
  auto c = connect();
  // A file that does not exist: refused, the old certificate stays.
  std::string bad = "*4\r\n$6\r\nCONFIG\r\n$3\r\nSET\r\n$13\r\ntls-cert-file\r\n$12\r\n/nonexistent\r\n";
  EXPECT_EQ(c->command(bad).substr(0, 4), "-ERR");
  EXPECT_EQ(connect()->command("PING\r\n"), "+PONG\r\n");
  // A new certificate from the same CA: new connections get it.
  std::string set = std::format("*4\r\n$6\r\nCONFIG\r\n$3\r\nSET\r\n$13\r\ntls-cert-file\r\n${}\r\n{}\r\n",
                                config_.tls_cert_file.size(), config_.tls_cert_file);
  EXPECT_EQ(c->command(set), "+OK\r\n");
  EXPECT_EQ(connect()->command("PING\r\n"), "+PONG\r\n");
  EXPECT_EQ(c->command("PING\r\n"), "+PONG\r\n");  // open connections are not affected
  // The same path is read again (rotation in place): a broken file there is refused.
  std::string good;
  {
    std::ifstream f(config_.tls_cert_file, std::ios::binary);
    good.assign(std::istreambuf_iterator<char>(f), {});
  }
  {
    std::ofstream f(config_.tls_cert_file, std::ios::binary | std::ios::trunc);
    f << "not a certificate";
  }
  EXPECT_EQ(c->command(set).substr(0, 4), "-ERR");
  {
    std::ofstream f(config_.tls_cert_file, std::ios::binary | std::ios::trunc);
    f << good;
  }
  EXPECT_EQ(c->command(set), "+OK\r\n");
  EXPECT_EQ(connect()->command("PING\r\n"), "+PONG\r\n");
}

TEST_F(TlsTest, AclLogRecordsRefusalsAndFailedLogins) {
  uint16_t p = server_->port();
  plain("*6\r\n$3\r\nACL\r\n$7\r\nSETUSER\r\n$3\r\nbob\r\n$2\r\non\r\n$4\r\n>pw1\r\n$4\r\n+get\r\n", p);
  auto c = connect();
  EXPECT_EQ(c->command("AUTH bob wrong\r\n").substr(0, 10), "-WRONGPASS");
  EXPECT_EQ(c->command("AUTH bob pw1\r\n"), "+OK\r\n");
  EXPECT_EQ(c->command("SET k v\r\n").substr(0, 7), "-NOPERM");
  EXPECT_EQ(c->command("GET secret\r\n").substr(0, 7), "-NOPERM");  // bob has no key patterns at all
  EXPECT_EQ(c->command("GET secret\r\n").substr(0, 7), "-NOPERM");  // a repeat: same entry, count 2

  std::string log = plain("ACL LOG\r\n", p);
  EXPECT_EQ(log.substr(0, 4), "*3\r\n") << log;
  size_t key = log.find("$3\r\nkey\r\n"), command = log.find("$7\r\ncommand\r\n"), auth = log.find("$4\r\nauth\r\n");
  ASSERT_NE(key, std::string::npos);
  ASSERT_NE(command, std::string::npos);
  ASSERT_NE(auth, std::string::npos);
  EXPECT_LT(key, command);  // newest first
  EXPECT_LT(command, auth);
  EXPECT_NE(log.find("$5\r\ncount\r\n:2\r\n"), std::string::npos);  // the two GET secret
  EXPECT_NE(log.find("$6\r\nsecret\r\n"), std::string::npos);
  EXPECT_NE(log.find("tls=yes"), std::string::npos);  // client-info
  EXPECT_EQ(plain("ACL LOG 1\r\n", p).substr(0, 4), "*1\r\n");
  EXPECT_EQ(plain("ACL LOG RESET\r\n", p), "+OK\r\n");
  EXPECT_EQ(plain("ACL LOG\r\n", p), "*0\r\n");
  plain("*4\r\n$6\r\nCONFIG\r\n$3\r\nSET\r\n$14\r\nacllog-max-len\r\n$1\r\n0\r\n", p);
  c->command("SET k v\r\n");
  EXPECT_EQ(plain("ACL LOG\r\n", p), "*0\r\n");
}

