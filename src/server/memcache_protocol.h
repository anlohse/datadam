#pragma once

#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "server/redis_commands.h"

namespace datadam {

// memcache text protocol on top of the same keyspace as RESP: a value written
// with memcache `set` is a Redis string, and vice versa.
class MemcacheSession {
 public:
  enum class Status { Done, Incomplete, Close };

  static constexpr size_t kMaxLine = 64 * 1024;
  static constexpr size_t kMaxKey = 250;
  static constexpr uint64_t kMaxValue = 512ULL << 20;

  explicit MemcacheSession(ServerContext& ctx) : ctx_(ctx), user_(ctx.acl.initial_user()) {}

  // Processes one request from buf[pos]; appends the reply to `out`.
  Status process(std::string_view buf, size_t& pos, std::string& out);

 private:
  // "" if the user may run the Redis command equivalent to this memcache one
  // on these keys, else the error to send.
  std::string permission_error(std::string_view redis_command, std::span<const std::string_view> keys,
                               uint8_t access);

  ServerContext& ctx_;
  // Who is logged in. When the default user needs a password, the connection
  // must first send "auth [user] <password>" (the memcache text protocol has
  // no authentication of its own). Permissions are those of the user.
  std::shared_ptr<const AclUser> user_;
  uint64_t acl_version_ = 0;
};

}  // namespace datadam
