// ACL metadata of every command: its categories and which arguments are keys
// (and whether the command reads or writes them). Close to what Redis
// declares, so ACL rules written for Redis mean the same here.

#include <unordered_map>

#include "server/command.h"

namespace datadam {

namespace {

constexpr uint32_t R = kCatRead, W = kCatWrite, KS = kCatKeyspace, STR = kCatString, HASH = kCatHash,
                   SET = kCatSet, ZSET = kCatSortedSet, LIST = kCatList, ADMIN = kCatAdmin, DANGER = kCatDangerous,
                   CONN = kCatConnection, FAST = kCatFast, SLOW = kCatSlow, IDX = kCatIndex, JSON = kCatJson,
                   PUBSUB = kCatPubSub, TX = kCatTransaction, BLOCK = kCatBlocking,
                   SCRIPT = kCatScripting;

constexpr KeyRange kNone{};
constexpr KeyRange kRead1{1, 1, 1, kKeyRead};
constexpr KeyRange kWrite1{1, 1, 1, kKeyWrite};
constexpr KeyRange kReadAll{1, -1, 1, kKeyRead};
constexpr KeyRange kWriteAll{1, -1, 1, kKeyWrite};
constexpr KeyRange kWrite1to2{1, 2, 1, kKeyWrite};
constexpr KeyRange kWritePairs{1, -1, 2, kKeyWrite};
constexpr KeyRange kRead2toEnd{2, -1, 1, kKeyRead};
constexpr KeyRange kRead2{2, 2, 1, kKeyRead};  // OBJECT ENCODING key, MEMORY USAGE key
constexpr KeyRange kReadNumkeys{2, 0, 1, kKeyRead, 1};
constexpr KeyRange kWriteNumkeys1{2, 0, 1, kKeyWrite, 1};   // numkeys at 1, keys from 2 (LMPOP)
constexpr KeyRange kWriteNumkeys2{3, 0, 1, kKeyWrite, 2};   // numkeys at 2, keys from 3 (BLMPOP)
constexpr KeyRange kWriteBeforeLast{1, -2, 1, kKeyWrite};   // key ... timeout (BLPOP)
constexpr KeyRange kScriptKeys{3, 0, 1, kKeyRead | kKeyWrite, 2};  // EVAL script numkeys key...
constexpr KeyRange kScriptReadKeys{3, 0, 1, kKeyRead, 2};         // EVAL_RO

struct Entry {
  const char* name;
  CommandMeta meta;
};

const Entry kEntries[] = {
    // connection / server
    {"ping", {CONN | FAST, {kNone, kNone}}},
    {"echo", {CONN | FAST, {kNone, kNone}}},
    {"quit", {CONN | FAST, {kNone, kNone}}},
    {"select", {CONN | FAST, {kNone, kNone}}},
    {"auth", {CONN | FAST, {kNone, kNone}}},
    {"hello", {CONN | FAST, {kNone, kNone}}},
    {"client", {CONN | SLOW, {kNone, kNone}}},
    {"command", {CONN | SLOW, {kNone, kNone}}},
    {"config", {ADMIN | SLOW | DANGER, {kNone, kNone}}},
    {"acl", {ADMIN | SLOW | DANGER, {kNone, kNone}}},
    {"info", {SLOW | DANGER, {kNone, kNone}}},
    {"time", {FAST, {kNone, kNone}}},
    {"dbsize", {KS | R | FAST, {kNone, kNone}}},
    {"flushdb", {KS | W | SLOW | DANGER, {kNone, kNone}}},
    {"flushall", {KS | W | SLOW | DANGER, {kNone, kNone}}},
    {"save", {ADMIN | SLOW | DANGER, {kNone, kNone}}},
    {"bgsave", {ADMIN | SLOW | DANGER, {kNone, kNone}}},
    {"backup", {ADMIN | SLOW | DANGER, {kNone, kNone}}},
    {"wait", {SLOW | BLOCK, {kNone, kNone}}},
    {"failover", {ADMIN | SLOW | DANGER, {kNone, kNone}}},
    {"lastsave", {ADMIN | FAST | DANGER, {kNone, kNone}}},
    {"shutdown", {ADMIN | SLOW | DANGER, {kNone, kNone}}},
    {"slowlog", {ADMIN | SLOW | DANGER, {kNone, kNone}}},
    {"monitor", {ADMIN | SLOW | DANGER, {kNone, kNone}}},
    {"object", {KS | R | SLOW, {kRead2, kNone}}},
    {"touch", {KS | R | FAST, {kReadAll, kNone}}},
    {"memory", {R | SLOW, {kRead2, kNone}}},
    {"cache", {ADMIN | SLOW, {kNone, kNone}}},
    {"keys", {KS | R | SLOW | DANGER, {kNone, kNone}}},
    {"scan", {KS | R | SLOW, {kNone, kNone}}},
    // transactions
    {"multi", {FAST | TX, {kNone, kNone}}},
    {"exec", {SLOW | TX, {kNone, kNone}}},
    // scripting: the commands a script calls are checked one by one
    {"eval", {SCRIPT | SLOW, {kScriptKeys, kNone}}},
    {"evalsha", {SCRIPT | SLOW, {kScriptKeys, kNone}}},
    {"eval_ro", {SCRIPT | SLOW, {kScriptReadKeys, kNone}}},
    {"evalsha_ro", {SCRIPT | SLOW, {kScriptReadKeys, kNone}}},
    {"fcall", {SCRIPT | SLOW, {kScriptKeys, kNone}}},
    {"fcall_ro", {SCRIPT | SLOW, {kScriptReadKeys, kNone}}},
    {"script", {SCRIPT | SLOW, {kNone, kNone}}},
    {"function", {SCRIPT | SLOW | DANGER, {kNone, kNone}}},
    {"discard", {FAST | TX, {kNone, kNone}}},
    {"watch", {FAST | TX, {kReadAll, kNone}}},
    {"unwatch", {FAST | TX, {kNone, kNone}}},
    // pub/sub
    {"subscribe", {PUBSUB | SLOW, {kNone, kNone}}},
    {"unsubscribe", {PUBSUB | SLOW, {kNone, kNone}}},
    {"psubscribe", {PUBSUB | SLOW, {kNone, kNone}}},
    {"punsubscribe", {PUBSUB | SLOW, {kNone, kNone}}},
    {"ssubscribe", {PUBSUB | SLOW, {kNone, kNone}}},
    {"sunsubscribe", {PUBSUB | SLOW, {kNone, kNone}}},
    {"publish", {PUBSUB | FAST, {kNone, kNone}}},
    {"spublish", {PUBSUB | FAST, {kNone, kNone}}},
    {"pubsub", {PUBSUB | SLOW, {kNone, kNone}}},
    // strings
    {"get", {R | STR | FAST, {kRead1, kNone}}},
    {"set", {W | STR | SLOW, {kWrite1, kNone}}},
    {"setnx", {W | STR | FAST, {kWrite1, kNone}}},
    {"setex", {W | STR | SLOW, {kWrite1, kNone}}},
    {"psetex", {W | STR | SLOW, {kWrite1, kNone}}},
    {"getset", {W | STR | FAST, {kWrite1, kNone}}},
    {"getdel", {W | STR | FAST, {kWrite1, kNone}}},
    {"getex", {W | STR | FAST, {kWrite1, kNone}}},
    {"mget", {R | STR | FAST, {kReadAll, kNone}}},
    {"mset", {W | STR | SLOW, {kWritePairs, kNone}}},
    {"msetnx", {W | STR | SLOW, {kWritePairs, kNone}}},
    {"append", {W | STR | FAST, {kWrite1, kNone}}},
    {"strlen", {R | STR | FAST, {kRead1, kNone}}},
    {"getrange", {R | STR | SLOW, {kRead1, kNone}}},
    {"substr", {R | STR | SLOW, {kRead1, kNone}}},
    {"setrange", {W | STR | SLOW, {kWrite1, kNone}}},
    {"incr", {W | STR | FAST, {kWrite1, kNone}}},
    {"decr", {W | STR | FAST, {kWrite1, kNone}}},
    {"incrby", {W | STR | FAST, {kWrite1, kNone}}},
    {"decrby", {W | STR | FAST, {kWrite1, kNone}}},
    {"incrbyfloat", {W | STR | FAST, {kWrite1, kNone}}},
    // keyspace
    {"del", {KS | W | SLOW, {kWriteAll, kNone}}},
    {"unlink", {KS | W | FAST, {kWriteAll, kNone}}},
    {"exists", {KS | R | FAST, {kReadAll, kNone}}},
    {"type", {KS | R | FAST, {kRead1, kNone}}},
    {"expire", {KS | W | FAST, {kWrite1, kNone}}},
    {"pexpire", {KS | W | FAST, {kWrite1, kNone}}},
    {"expireat", {KS | W | FAST, {kWrite1, kNone}}},
    {"pexpireat", {KS | W | FAST, {kWrite1, kNone}}},
    {"ttl", {KS | R | FAST, {kRead1, kNone}}},
    {"pttl", {KS | R | FAST, {kRead1, kNone}}},
    {"expiretime", {KS | R | FAST, {kRead1, kNone}}},
    {"pexpiretime", {KS | R | FAST, {kRead1, kNone}}},
    {"persist", {KS | W | FAST, {kWrite1, kNone}}},
    {"rename", {KS | W | SLOW, {kWrite1to2, kNone}}},
    {"renamenx", {KS | W | FAST, {kWrite1to2, kNone}}},
    // hash
    {"hset", {W | HASH | FAST, {kWrite1, kNone}}},
    {"hmset", {W | HASH | FAST, {kWrite1, kNone}}},
    {"hsetnx", {W | HASH | FAST, {kWrite1, kNone}}},
    {"hdel", {W | HASH | FAST, {kWrite1, kNone}}},
    {"hincrby", {W | HASH | FAST, {kWrite1, kNone}}},
    {"hincrbyfloat", {W | HASH | FAST, {kWrite1, kNone}}},
    {"hget", {R | HASH | FAST, {kRead1, kNone}}},
    {"hmget", {R | HASH | FAST, {kRead1, kNone}}},
    {"hexists", {R | HASH | FAST, {kRead1, kNone}}},
    {"hlen", {R | HASH | FAST, {kRead1, kNone}}},
    {"hstrlen", {R | HASH | FAST, {kRead1, kNone}}},
    {"hgetall", {R | HASH | SLOW, {kRead1, kNone}}},
    {"hkeys", {R | HASH | SLOW, {kRead1, kNone}}},
    {"hvals", {R | HASH | SLOW, {kRead1, kNone}}},
    {"hrandfield", {R | HASH | SLOW, {kRead1, kNone}}},
    {"hscan", {R | HASH | SLOW, {kRead1, kNone}}},
    // set
    {"sadd", {W | SET | FAST, {kWrite1, kNone}}},
    {"srem", {W | SET | FAST, {kWrite1, kNone}}},
    {"spop", {W | SET | FAST, {kWrite1, kNone}}},
    {"smove", {W | SET | FAST, {kWrite1to2, kNone}}},
    {"sinterstore", {W | SET | SLOW, {kWrite1, kRead2toEnd}}},
    {"sunionstore", {W | SET | SLOW, {kWrite1, kRead2toEnd}}},
    {"sdiffstore", {W | SET | SLOW, {kWrite1, kRead2toEnd}}},
    {"sismember", {R | SET | FAST, {kRead1, kNone}}},
    {"smismember", {R | SET | FAST, {kRead1, kNone}}},
    {"scard", {R | SET | FAST, {kRead1, kNone}}},
    {"smembers", {R | SET | SLOW, {kRead1, kNone}}},
    {"srandmember", {R | SET | SLOW, {kRead1, kNone}}},
    {"sscan", {R | SET | SLOW, {kRead1, kNone}}},
    {"sinter", {R | SET | SLOW, {kReadAll, kNone}}},
    {"sunion", {R | SET | SLOW, {kReadAll, kNone}}},
    {"sdiff", {R | SET | SLOW, {kReadAll, kNone}}},
    {"sintercard", {R | SET | SLOW, {kReadNumkeys, kNone}}},
    // sorted set
    {"zadd", {W | ZSET | FAST, {kWrite1, kNone}}},
    {"zincrby", {W | ZSET | FAST, {kWrite1, kNone}}},
    {"zrem", {W | ZSET | FAST, {kWrite1, kNone}}},
    {"zpopmin", {W | ZSET | FAST, {kWrite1, kNone}}},
    {"bzpopmin", {W | ZSET | FAST | BLOCK, {kWriteBeforeLast, kNone}}},
    {"bzpopmax", {W | ZSET | FAST | BLOCK, {kWriteBeforeLast, kNone}}},
    {"zmpop", {W | ZSET | SLOW, {kWriteNumkeys1, kNone}}},
    {"bzmpop", {W | ZSET | SLOW | BLOCK, {kWriteNumkeys2, kNone}}},
    {"zpopmax", {W | ZSET | FAST, {kWrite1, kNone}}},
    {"zremrangebyscore", {W | ZSET | SLOW, {kWrite1, kNone}}},
    {"zremrangebyrank", {W | ZSET | SLOW, {kWrite1, kNone}}},
    {"zremrangebylex", {W | ZSET | SLOW, {kWrite1, kNone}}},
    {"zscore", {R | ZSET | FAST, {kRead1, kNone}}},
    {"zmscore", {R | ZSET | FAST, {kRead1, kNone}}},
    {"zcard", {R | ZSET | FAST, {kRead1, kNone}}},
    {"zrank", {R | ZSET | FAST, {kRead1, kNone}}},
    {"zrevrank", {R | ZSET | FAST, {kRead1, kNone}}},
    {"zcount", {R | ZSET | FAST, {kRead1, kNone}}},
    {"zlexcount", {R | ZSET | FAST, {kRead1, kNone}}},
    {"zrange", {R | ZSET | SLOW, {kRead1, kNone}}},
    {"zrangebyscore", {R | ZSET | SLOW, {kRead1, kNone}}},
    {"zrevrangebyscore", {R | ZSET | SLOW, {kRead1, kNone}}},
    {"zrevrange", {R | ZSET | SLOW, {kRead1, kNone}}},
    {"zrangebylex", {R | ZSET | SLOW, {kRead1, kNone}}},
    {"zrevrangebylex", {R | ZSET | SLOW, {kRead1, kNone}}},
    {"zscan", {R | ZSET | SLOW, {kRead1, kNone}}},
    // list
    {"lpush", {W | LIST | FAST, {kWrite1, kNone}}},
    {"rpush", {W | LIST | FAST, {kWrite1, kNone}}},
    {"lpushx", {W | LIST | FAST, {kWrite1, kNone}}},
    {"rpushx", {W | LIST | FAST, {kWrite1, kNone}}},
    {"lpop", {W | LIST | FAST, {kWrite1, kNone}}},
    {"rpop", {W | LIST | FAST, {kWrite1, kNone}}},
    {"lset", {W | LIST | SLOW, {kWrite1, kNone}}},
    {"ltrim", {W | LIST | SLOW, {kWrite1, kNone}}},
    {"lrem", {W | LIST | SLOW, {kWrite1, kNone}}},
    {"linsert", {W | LIST | SLOW, {kWrite1, kNone}}},
    {"lmove", {W | LIST | SLOW, {kWrite1to2, kNone}}},
    {"blpop", {W | LIST | SLOW | BLOCK, {kWriteBeforeLast, kNone}}},
    {"brpop", {W | LIST | SLOW | BLOCK, {kWriteBeforeLast, kNone}}},
    {"blmove", {W | LIST | SLOW | BLOCK, {kWrite1to2, kNone}}},
    {"brpoplpush", {W | LIST | SLOW | BLOCK, {kWrite1to2, kNone}}},
    {"lmpop", {W | LIST | SLOW, {kWriteNumkeys1, kNone}}},
    {"blmpop", {W | LIST | SLOW | BLOCK, {kWriteNumkeys2, kNone}}},
    {"rpoplpush", {W | LIST | SLOW, {kWrite1to2, kNone}}},
    {"llen", {R | LIST | FAST, {kRead1, kNone}}},
    {"lindex", {R | LIST | SLOW, {kRead1, kNone}}},
    {"lrange", {R | LIST | SLOW, {kRead1, kNone}}},
    {"lpos", {R | LIST | SLOW, {kRead1, kNone}}},
    // secondary indexes: queries return only the hashes the user may read
    {"idx.create", {W | IDX | ADMIN | SLOW | DANGER, {kNone, kNone}}},
    {"idx.drop", {W | IDX | ADMIN | SLOW | DANGER, {kNone, kNone}}},
    {"idx.list", {R | IDX | SLOW, {kNone, kNone}}},
    {"idx.info", {R | IDX | SLOW, {kNone, kNone}}},
    {"idx.query", {R | IDX | SLOW, {kNone, kNone}}},
    {"idx.get", {R | IDX | SLOW, {kNone, kNone}}},
    {"idx.count", {R | IDX | SLOW, {kNone, kNone}}},
    {"ft.create", {W | IDX | ADMIN | SLOW | DANGER, {kNone, kNone}}},
    {"ft.dropindex", {W | IDX | ADMIN | SLOW | DANGER, {kNone, kNone}}},
    {"ft.search", {R | IDX | SLOW, {kNone, kNone}}},
    {"ft.info", {R | IDX | SLOW, {kNone, kNone}}},
    {"ft._list", {R | IDX | SLOW, {kNone, kNone}}},
    {"ft.tagvals", {R | IDX | SLOW, {kNone, kNone}}},
    // change feed: every key's changes, filtered by the keys the user may read
    {"cdc.info", {ADMIN | SLOW, {kNone, kNone}}},
    {"cdc.read", {ADMIN | SLOW | DANGER, {kNone, kNone}}},
    // JSON documents
    {"json.set", {W | JSON | SLOW, {kWrite1, kNone}}},
    {"json.get", {R | JSON | SLOW, {kRead1, kNone}}},
    {"json.mget", {R | JSON | SLOW, {{1, -2, 1, kKeyRead}, kNone}}},
    {"json.mset", {W | JSON | SLOW, {{1, -3, 3, kKeyWrite}, kNone}}},
    {"json.del", {W | JSON | SLOW, {kWrite1, kNone}}},
    {"json.forget", {W | JSON | SLOW, {kWrite1, kNone}}},
    {"json.type", {R | JSON | SLOW, {kRead1, kNone}}},
    {"json.numincrby", {W | JSON | SLOW, {kWrite1, kNone}}},
    {"json.nummultby", {W | JSON | SLOW, {kWrite1, kNone}}},
    {"json.strappend", {W | JSON | SLOW, {kWrite1, kNone}}},
    {"json.strlen", {R | JSON | SLOW, {kRead1, kNone}}},
    {"json.arrappend", {W | JSON | SLOW, {kWrite1, kNone}}},
    {"json.arrinsert", {W | JSON | SLOW, {kWrite1, kNone}}},
    {"json.arrlen", {R | JSON | SLOW, {kRead1, kNone}}},
    {"json.arrpop", {W | JSON | SLOW, {kWrite1, kNone}}},
    {"json.arrtrim", {W | JSON | SLOW, {kWrite1, kNone}}},
    {"json.arrindex", {R | JSON | SLOW, {kRead1, kNone}}},
    {"json.objkeys", {R | JSON | SLOW, {kRead1, kNone}}},
    {"json.objlen", {R | JSON | SLOW, {kRead1, kNone}}},
    {"json.toggle", {W | JSON | SLOW, {kWrite1, kNone}}},
    {"json.clear", {W | JSON | SLOW, {kWrite1, kNone}}},
    {"json.merge", {W | JSON | SLOW, {kWrite1, kNone}}},
    // replication
    {"replicaof", {ADMIN | SLOW | DANGER, {kNone, kNone}}},
    {"slaveof", {ADMIN | SLOW | DANGER, {kNone, kNone}}},
    {"role", {ADMIN | FAST | DANGER, {kNone, kNone}}},
    {"replsync", {ADMIN | SLOW | DANGER, {kNone, kNone}}},
    {"replconf", {ADMIN | SLOW | DANGER, {kNone, kNone}}},
};

}  // namespace

const CommandMeta* command_meta(std::string_view name) {
  static const std::unordered_map<std::string_view, const CommandMeta*> by_name = [] {
    std::unordered_map<std::string_view, const CommandMeta*> m;
    for (const auto& e : kEntries) m.emplace(e.name, &e.meta);
    return m;
  }();
  auto it = by_name.find(name);
  return it == by_name.end() ? nullptr : it->second;
}

}  // namespace datadam
