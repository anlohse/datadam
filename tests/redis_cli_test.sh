#!/usr/bin/env bash
# Exercises every supported Redis command through the real redis-cli.
#
# usage: tests/redis_cli_test.sh [host] [port] [phase]
#   phase "commands" (default) - every command, edge cases and error replies
#   phase "write"              - stores a known data set
#   phase "verify"             - checks that data set (run after a restart)
set -u

H=${1:-127.0.0.1}
P=${2:-6379}
PHASE=${3:-commands}
# Extra redis-cli options for every connection, e.g. "--tls --cacert ca.crt ...".
CLI_OPTS=${CLI_OPTS:-}
pass=0
fail=0

cli() { redis-cli -h "$H" -p "$P" $CLI_OPTS "$@" 2>&1; }

report() {  # name, ok?, expected, got, command
  if [ "$2" = 1 ]; then
    pass=$((pass + 1))
    echo "  ok   $1"
  else
    fail=$((fail + 1))
    printf '  FAIL %s\n       cmd:      %s\n       expected: %q\n       got:      %q\n' "$1" "$5" "$3" "$4"
  fi
}

# t NAME EXPECTED CMD...    exact match of redis-cli's (raw) output
t() {
  local name=$1 expected=$2 got
  shift 2
  got=$(cli "$@")
  [ "$got" == "$expected" ] && report "$name" 1 || report "$name" 0 "$expected" "$got" "$*"
}

# tc NAME SUBSTRING CMD...  output contains SUBSTRING
tc() {
  local name=$1 expected=$2 got
  shift 2
  got=$(cli "$@")
  [[ "$got" == *"$expected"* ]] && report "$name" 1 || report "$name" 0 "*$expected*" "$got" "$*"
}

# tx NAME REGEX CMD...      output matches the extended regex
tx() {
  local name=$1 expected=$2 got
  shift 2
  got=$(cli "$@")
  [[ "$got" =~ $expected ]] && report "$name" 1 || report "$name" 0 "/$expected/" "$got" "$*"
}

# ts NAME EXPECTED CMD...   sorted output (for KEYS)
ts() {
  local name=$1 expected=$2 got
  shift 2
  got=$(cli "$@" | sort)
  [ "$got" == "$expected" ] && report "$name" 1 || report "$name" 0 "$expected" "$got" "$*"
}

# tp NAME EXPECTED <<< "commands, one per line, on a single connection"
tp() {
  local name=$1 expected=$2 input got
  input=$(cat)
  got=$(printf '%s\n' "$input" | redis-cli -h "$H" -p "$P" $CLI_OPTS 2>&1)
  [ "$got" == "$expected" ] && report "$name" 1 || report "$name" 0 "$expected" "$got" "$input"
}

# tpc NAME SUBSTRING <<< "commands on one connection"   output contains SUBSTRING
tpc() {
  local name=$1 expected=$2 input got
  input=$(cat)
  got=$(printf '%s\n' "$input" | redis-cli -h "$H" -p "$P" $CLI_OPTS 2>&1)
  [[ "$got" == *"$expected"* ]] && report "$name" 1 || report "$name" 0 "*$expected*" "$got" "$input"
}

# tu NAME EXPECTED USER PASSWORD CMD...   exact match, logged in as USER
tu() {
  local name=$1 expected=$2 user=$3 password=$4 got
  shift 4
  got=$(REDISCLI_AUTH="$password" redis-cli -h "$H" -p "$P" $CLI_OPTS --user "$user" "$@" 2>&1)
  [ "$got" == "$expected" ] && report "$name" 1 || report "$name" 0 "$expected" "$got" "--user $user $*"
}

section() { echo "== $1"; }

# Waits until an index has finished its backfill.
wait_index() {
  for _ in $(seq 100); do
    cli IDX.INFO "$1" | grep -qx ready && return
    sleep 0.05
  done
  echo "  index $1 did not become ready"
}

big_file=$(mktemp)
head -c 100000 /dev/zero | tr '\0' 'x' > "$big_file"

commands() {
  section "connection / server"
  t  "PING"                     "PONG" PING
  t  "PING message"             "hello" PING hello
  t  "ECHO"                     "hi there" ECHO "hi there"
  t  "SELECT 0"                 "OK" SELECT 0
  tc "SELECT 1 rejected"        "ERR DB index is out of range" SELECT 1
  tc "AUTH without password"    "ERR AUTH" AUTH secret
  tp "CLIENT SETNAME/GETNAME"   $'OK\ntester' <<< $'CLIENT SETNAME tester\nCLIENT GETNAME'
  tx "CLIENT ID"                '^[0-9]+$' CLIENT ID
  t  "CLIENT SETINFO"           "OK" CLIENT SETINFO lib-name bash-test
  tc "CLIENT INFO"              "id=" CLIENT INFO
  tc "CLIENT unknown"           "ERR unknown subcommand" CLIENT FOO
  tx "COMMAND COUNT"            '^[0-9]+$' COMMAND COUNT
  tc "COMMAND LIST"             "incrbyfloat" COMMAND LIST
  t  "CONFIG GET"               $'cache-max-object-size\n4kb' CONFIG GET cache-max-object-size
  tc "CONFIG GET glob"          "cache-max-idle" CONFIG GET "cache-*"
  tc "CONFIG SET not supported" "ERR CONFIG SET" CONFIG SET port 1
  tc "INFO server"              "datadam_version:" INFO server
  tc "INFO cache"               "cache_hit_ratio:" INFO cache
  tc "INFO persistence"         "engine:rocksdb" INFO persistence
  tc "INFO all"                 "# Keyspace" INFO
  tx "TIME"                     $'^[0-9]{10}\n[0-9]{1,6}$' TIME
  tc "HELLO 3"                  "datadam" HELLO 3
  tc "HELLO 4 rejected"         "NOPROTO" HELLO 4
  t  "RESP3 GET nil"            "" -3 GET no-such-key
  t  "QUIT"                     "OK" QUIT
  tc "unknown command"          "ERR unknown command 'NOSUCH'" NOSUCH a b
  tc "wrong arity"              "ERR wrong number of arguments for 'get' command" GET
  t  "FLUSHALL"                 "OK" FLUSHALL
  t  "FLUSHDB ASYNC"            "OK" FLUSHDB ASYNC
  tc "FLUSHALL bad option"      "ERR syntax error" FLUSHALL NOW
  t  "SAVE"                     "OK" SAVE
  t  "BGSAVE"                   "Background saving started" BGSAVE

  section "SET / GET"
  t  "SET"                      "OK" SET k v1
  t  "GET"                      "v1" GET k
  t  "GET missing"              "" GET missing
  t  "SET NX on existing"       "" SET k other NX
  t  "SET XX GET"               "v1" SET k v2 XX GET
  t  "SET XX on missing"        "" SET missing v XX
  t  "SET GET"                  "v2" SET k v3 GET
  t  "SET GET on missing"       "" SET fresh v GET
  t  "SET EX"                   "OK" SET ek v EX 100
  t  "TTL after EX"             "100" TTL ek
  t  "SET PX"                   "OK" SET ek v PX 100000
  tx "PTTL after PX"            '^(99[0-9]{3}|100000)$' PTTL ek
  t  "SET EXAT"                 "OK" SET ek v EXAT $(( $(date +%s) + 200 ))
  tx "TTL after EXAT"           '^(199|200)$' TTL ek
  t  "SET PXAT"                 "OK" SET ek v PXAT $(( $(date +%s) * 1000 + 300000 ))
  tx "TTL after PXAT"           '^(299|300)$' TTL ek
  t  "SET KEEPTTL"              "OK" SET ek v2 KEEPTTL
  tx "TTL kept"                 '^(299|300)$' TTL ek
  t  "SET without KEEPTTL"      "OK" SET ek v3
  t  "TTL cleared"              "-1" TTL ek
  tc "SET EX 0"                 "ERR invalid expire time in 'set' command" SET ek v EX 0
  tc "SET EX + PX"              "ERR syntax error" SET ek v EX 10 PX 10
  tc "SET NX + XX"              "ERR syntax error" SET ek v NX XX
  tc "SET bad option"           "ERR syntax error" SET ek v FOO
  t  "SET empty value"          "OK" SET empty ""
  t  "GET empty value"          "" GET empty
  t  "STRLEN empty value"       "0" STRLEN empty
  t  "SET binary/utf-8"         "OK" SET "chave com espaço" "ação ✓"
  t  "GET binary/utf-8"         "ação ✓" GET "chave com espaço"

  section "other string commands"
  t  "SETNX existing"           "0" SETNX k x
  t  "SETNX new"                "1" SETNX nx-key x
  t  "SETEX"                    "OK" SETEX sx 100 v
  t  "TTL after SETEX"          "100" TTL sx
  tc "SETEX 0"                  "ERR invalid expire time in 'setex' command" SETEX sx 0 v
  t  "PSETEX"                   "OK" PSETEX px 100000 v
  tx "PTTL after PSETEX"        '^(99[0-9]{3}|100000)$' PTTL px
  t  "GETSET"                   "v3" GETSET k v4
  t  "GETSET missing"           "" GETSET gs-new v
  t  "GETDEL"                   "v4" GETDEL k
  t  "GET after GETDEL"         "" GET k
  t  "GETDEL missing"           "" GETDEL k
  t  "SET for GETEX"            "OK" SET g v
  t  "GETEX EX"                 "v" GETEX g EX 100
  t  "TTL after GETEX"          "100" TTL g
  t  "GETEX PERSIST"            "v" GETEX g PERSIST
  t  "TTL after PERSIST"        "-1" TTL g
  t  "GETEX PX"                 "v" GETEX g PX 50000
  tx "PTTL after GETEX PX"      '^(49[0-9]{3}|50000)$' PTTL g
  t  "GETEX missing"            "" GETEX nope EX 10
  tc "GETEX bad option"         "ERR syntax error" GETEX g FOO
  t  "MSET"                     "OK" MSET a 1 b 2 c 3
  t  "MGET"                     $'1\n2\n\n3' MGET a b nope c
  tc "MSET odd args"            "ERR wrong number of arguments for 'mset' command" MSET a 1 b
  t  "MSETNX blocked"           "0" MSETNX a x d 4
  t  "MSETNX wrote nothing"     "0" EXISTS d
  t  "MSETNX"                   "1" MSETNX d 4 e 5
  t  "APPEND new"               "5" APPEND ap Hello
  t  "APPEND existing"          "11" APPEND ap " World"
  t  "GET after APPEND"         "Hello World" GET ap
  t  "STRLEN"                   "11" STRLEN ap
  t  "STRLEN missing"           "0" STRLEN nope
  t  "GETRANGE"                 "Hello" GETRANGE ap 0 4
  t  "GETRANGE negative"        "World" GETRANGE ap -5 -1
  t  "GETRANGE whole"           "Hello World" GETRANGE ap 0 -1
  t  "GETRANGE out of range"    "" GETRANGE ap 100 200
  t  "GETRANGE missing"         "" GETRANGE nope 0 -1
  t  "SUBSTR"                   "World" SUBSTR ap 6 10
  t  "SETRANGE"                 "11" SETRANGE ap 6 Redis
  t  "GET after SETRANGE"       "Hello Redis" GET ap
  t  "SETRANGE pads with zeros" "6" SETRANGE pad 5 x
  t  "STRLEN padded"            "6" STRLEN pad
  tc "SETRANGE negative"        "ERR offset is out of range" SETRANGE pad -1 x

  section "counters"
  t  "INCR new"                 "1" INCR n
  t  "INCRBY"                   "10" INCRBY n 9
  t  "DECR"                     "9" DECR n
  t  "DECRBY"                   "5" DECRBY n 4
  t  "INCRBY negative"          "-5" INCRBY n -10
  t  "DECR new"                 "-1" DECR n2
  tc "INCR non-integer"         "ERR value is not an integer or out of range" INCR ap
  tc "INCRBY bad delta"         "ERR value is not an integer or out of range" INCRBY n abc
  t  "SET max int64"            "OK" SET max 9223372036854775807
  tc "INCR overflow"            "ERR increment or decrement would overflow" INCR max
  t  "INCRBYFLOAT new"          "1.5" INCRBYFLOAT f 1.5
  t  "INCRBYFLOAT"              "1.75" INCRBYFLOAT f 0.25
  t  "INCRBYFLOAT to zero"      "0" INCRBYFLOAT f -1.75
  t  "INCRBYFLOAT on integer"   "7.5" INCRBYFLOAT n 12.5
  tc "INCRBYFLOAT non-float"    "ERR value is not a valid float" INCRBYFLOAT ap 1
  t  "SET for TTL keep"         "OK" SET cnt 1 EX 100
  t  "INCR keeps TTL"           "2" INCR cnt
  tx "TTL still set"            '^(99|100)$' TTL cnt

  section "keyspace"
  t  "DEL"                      "2" DEL a b nope
  t  "UNLINK"                   "1" UNLINK c
  t  "EXISTS counts repeats"    "2" EXISTS d d nope
  t  "TYPE string"              "string" TYPE d
  t  "TYPE missing"             "none" TYPE nope
  t  "EXPIRE"                   "1" EXPIRE d 100
  t  "TTL"                      "100" TTL d
  t  "EXPIRE GT smaller"        "0" EXPIRE d 50 GT
  t  "EXPIRE GT larger"         "1" EXPIRE d 200 GT
  t  "EXPIRE LT larger"         "0" EXPIRE d 300 LT
  t  "EXPIRE LT smaller"        "1" EXPIRE d 10 LT
  t  "EXPIRE NX with TTL"       "0" EXPIRE d 100 NX
  t  "EXPIRE XX without TTL"    "0" EXPIRE e 100 XX
  t  "EXPIRE NX without TTL"    "1" EXPIRE e 100 NX
  t  "EXPIRE GT without TTL"    "0" EXPIRE ap 100 GT
  t  "EXPIRE missing"           "0" EXPIRE nope 10
  tc "EXPIRE NX + XX"           "ERR NX and XX, GT or LT options" EXPIRE d 10 NX XX
  tc "EXPIRE GT + LT"           "ERR GT and LT options" EXPIRE d 10 GT LT
  tc "EXPIRE bad option"        "ERR Unsupported option" EXPIRE d 10 FOO
  tc "EXPIRE non-integer"       "ERR value is not an integer" EXPIRE d abc
  t  "PEXPIRE"                  "1" PEXPIRE d 100000
  tx "PTTL"                     '^(99[0-9]{3}|100000)$' PTTL d
  local at=$(( $(date +%s) + 1000 ))
  t  "EXPIREAT"                 "1" EXPIREAT d $at
  t  "EXPIRETIME"               "$at" EXPIRETIME d
  local pat=$(( $(date +%s) * 1000 + 2000000 ))
  t  "PEXPIREAT"                "1" PEXPIREAT d $pat
  t  "PEXPIRETIME"              "$pat" PEXPIRETIME d
  t  "TTL missing"              "-2" TTL nope
  t  "PTTL missing"             "-2" PTTL nope
  t  "TTL persistent"           "-1" TTL ap
  t  "EXPIRETIME persistent"    "-1" EXPIRETIME ap
  t  "EXPIRETIME missing"       "-2" EXPIRETIME nope
  t  "PERSIST"                  "1" PERSIST d
  t  "PERSIST again"            "0" PERSIST d
  t  "TTL after PERSIST"        "-1" TTL d
  t  "EXPIRE in the past"       "1" EXPIRE d -1
  t  "deleted by past EXPIRE"   "0" EXISTS d
  t  "SET PX 200"               "OK" SET short v PX 200
  t  "visible before expiry"    "v" GET short
  sleep 0.4
  t  "GET after expiry"         "" GET short
  t  "EXISTS after expiry"      "0" EXISTS short
  t  "RENAME"                   "OK" RENAME e renamed
  t  "GET renamed"              "5" GET renamed
  t  "old name gone"            "0" EXISTS e
  tx "RENAME keeps TTL"         '^(99|100)$' TTL renamed
  tc "RENAME missing"           "ERR no such key" RENAME nope x
  t  "RENAMENX target exists"   "0" RENAMENX renamed ap
  t  "RENAMENX"                 "1" RENAMENX renamed e
  t  "RENAME onto itself"       "OK" RENAME e e
  t  "FLUSHALL before KEYS"     "OK" FLUSHALL
  t  "MSET for KEYS"            "OK" MSET user:1 a user:2 b user:10 c order:1 d
  ts "KEYS prefix"              $'user:1\nuser:10\nuser:2' KEYS "user:*"
  ts "KEYS suffix"              $'order:1\nuser:1' KEYS "*:1"
  ts "KEYS ?"                   $'user:1\nuser:2' KEYS "user:?"
  ts "KEYS [set]"               $'user:1\nuser:2' KEYS "user:[12]"
  ts "KEYS [^set]"              $'user:2' KEYS "user:[^1]"
  t  "KEYS no match"            "" KEYS "nothing*"
  tx "DBSIZE"                   '^[0-9]+$' DBSIZE

  section "cache and large values"
  t  "SET small"                "OK" SET c1 small-value
  t  "GET small (cache)"        "small-value" GET c1
  tc "CACHE STATS"              "hits" CACHE STATS
  tc "CACHE CLASSES"            "chunk_size" CACHE CLASSES
  t  "CACHE CLEAR"              "OK" CACHE CLEAR
  t  "GET after CACHE CLEAR"    "small-value" GET c1
  t  "SET second small"         "OK" SET c2 v2
  t  "SAVE (to disk)"           "OK" SAVE
  t  "CACHE CLEAR again"        "OK" CACHE CLEAR
  # Reads of uncached keys on disk are loaded by the read pool and run again,
  # unless the OS page cache has their blocks (page_cache_reads: read at once);
  # a key that does not exist is answered by RocksDB's filters, from memory.
  tp "uncached reads, one connection" $'small-value\nv2\n\n2\n2' <<'EOF'
MGET c1 c2 nokey
EXISTS c1 c2 nokey
STRLEN c2
EOF
  tx "reads went through the read pool or the page cache" 'read_pool_loads:[1-9]|page_cache_reads:1' INFO stats
  tx "missing key read from memory" 'memory_reads:[1-9]' INFO stats
  t  "SET 100KB value"          "OK" -x SET bigv < "$big_file"
  t  "GET 100KB value"          "100000" STRLEN bigv
  tx "big value bypasses pool"  'rejected_too_large[^0-9]*[1-9]' CACHE STATS
  local got
  got=$(cli GET bigv | head -c 100000 | cmp -s - "$big_file" && echo same)
  [ "$got" = same ] && report "100KB value round-trips" 1 || report "100KB value round-trips" 0 same "$got" "GET bigv"
  tc "unknown CACHE subcommand" "ERR unknown subcommand" CACHE FOO

  section "json"
  t  "JSON.SET"                 "OK" JSON.SET j:1 '$' '{"name":"ana","tags":["a","b"],"age":30}'
  t  "JSON.GET root"            '{"name":"ana","tags":["a","b"],"age":30}' JSON.GET j:1
  t  "JSON.GET \$ path"         '["ana"]' JSON.GET j:1 '$.name'
  t  "JSON.GET legacy path"     '"b"' JSON.GET j:1 '.tags[1]'
  t  "JSON.NUMINCRBY"           '[31]' JSON.NUMINCRBY j:1 '$.age' 1
  t  "JSON.ARRAPPEND"           "3" JSON.ARRAPPEND j:1 '$.tags' '"c"'
  t  "JSON.TYPE"                "array" JSON.TYPE j:1 '.tags'
  t  "TYPE of a JSON key"       "ReJSON-RL" TYPE j:1
  t  "JSON.SET second"          "OK" JSON.SET j:2 '$' '{"items":[{"p":5},{"p":15}]}'
  t  "JSON.GET filter"          '[{"p":15}]' JSON.GET j:2 '$.items[?(@.p > 10)]'
  tc "GET on JSON is WRONGTYPE" "WRONGTYPE" GET j:1
  t  "JSON.DEL path"            "1" JSON.DEL j:1 '$.name'
  t  "JSON.MGET"                $'[]\n[[{"p":5},{"p":15}]]' JSON.MGET j:1 j:2 '$.items'
  tc "bad JSON refused"         "ERR" JSON.SET j:3 '$' '{nope'
  t  "SCAN TYPE ReJSON-RL"      $'0\nj:1\nj:2' SCAN 0 MATCH 'j:*' COUNT 1000 TYPE ReJSON-RL
  t  "IDX.CREATE ON JSON"       "OK" IDX.CREATE jages ON JSON PREFIX 1 j: SCHEMA '$.age' AS age NUMERIC
  wait_index jages
  t  "IDX.QUERY on JSON"        "j:1" IDX.QUERY jages age '>' 30 NOCONTENT
  t  "JSON write updates it"    "OK" JSON.SET j:1 '$.age' 20
  t  "IDX.COUNT on JSON"        "0" IDX.COUNT jages age '>' 30
  t  "IDX.DROP JSON index"      "OK" IDX.DROP jages

  section "hash"
  t  "SET string for WRONGTYPE" "OK" SET str v
  t  "HSET"                     "2" HSET h f1 v1 f2 v2
  t  "HSET update + new"        "1" HSET h f1 x f3 v3
  t  "HGET"                     "x" HGET h f1
  t  "HGET missing field"       "" HGET h nope
  t  "HGET missing key"         "" HGET nokey f
  t  "HMGET"                    $'x\n\nv3' HMGET h f1 nope f3
  t  "HLEN"                     "3" HLEN h
  t  "HLEN missing"             "0" HLEN nokey
  t  "HEXISTS"                  "1" HEXISTS h f2
  t  "HEXISTS missing"          "0" HEXISTS h nope
  t  "HSTRLEN"                  "2" HSTRLEN h f2
  t  "HGETALL"                  $'f1\nx\nf2\nv2\nf3\nv3' HGETALL h
  t  "HGETALL missing"          "" HGETALL nokey
  t  "HKEYS"                    $'f1\nf2\nf3' HKEYS h
  t  "HVALS"                    $'x\nv2\nv3' HVALS h
  t  "HSETNX existing"          "0" HSETNX h f1 y
  t  "HSETNX new"               "1" HSETNX h f4 y
  t  "HDEL"                     "2" HDEL h f4 f3 nope
  t  "HINCRBY new"              "5" HINCRBY h n 5
  t  "HINCRBY"                  "2" HINCRBY h n -3
  tc "HINCRBY non-integer"      "ERR hash value is not an integer" HINCRBY h f1 1
  t  "HINCRBYFLOAT"             "2.5" HINCRBYFLOAT h n 0.5
  tc "HINCRBYFLOAT non-float"   "ERR hash value is not a float" HINCRBYFLOAT h f1 1
  t  "HMSET"                    "OK" HMSET h2 a 1 b 2
  tc "HSET odd args"            "ERR wrong number of arguments for 'hset' command" HSET h a
  tx "HRANDFIELD"               '^(a|b)$' HRANDFIELD h2
  ts "HRANDFIELD count"         $'a\nb' HRANDFIELD h2 5
  tx "HRANDFIELD negative"      $'^[ab]\n[ab]\n[ab]$' HRANDFIELD h2 -3
  tx "HRANDFIELD WITHVALUES"    $'^(a\n1|b\n2)$' HRANDFIELD h2 1 WITHVALUES
  t  "HRANDFIELD missing"       "" HRANDFIELD nokey
  t  "TYPE hash"                "hash" TYPE h
  tc "GET on hash"              "WRONGTYPE" GET h
  tc "HGET on string"           "WRONGTYPE" HGET str f
  tc "INCR on hash"             "WRONGTYPE" INCR h
  t  "EXPIRE hash"              "1" EXPIRE h 100
  t  "HSET keeps TTL"           "1" HSET h z 1
  tx "TTL of hash"              '^(99|100)$' TTL h
  t  "RENAME hash"              "OK" RENAME h hh
  t  "HGET after RENAME"        "x" HGET hh f1
  t  "old hash gone"            "0" EXISTS h
  tx "RENAME keeps hash TTL"    '^(99|100)$' TTL hh
  t  "HDEL last fields"         "2" HDEL h2 a b
  t  "empty hash is deleted"    "0" EXISTS h2
  t  "SET over hash"            "OK" SET hh plain
  t  "TYPE after SET"           "string" TYPE hh
  t  "HSET on fresh key"        "1" HSET hh2 f v
  t  "DEL hash"                 "1" DEL hh2
  t  "HGETALL after DEL"        "" HGETALL hh2

  section "set"
  t  "SADD"                     "3" SADD s a b c
  t  "SADD with duplicate"      "1" SADD s c d
  t  "SCARD"                    "4" SCARD s
  t  "SISMEMBER"                "1" SISMEMBER s a
  t  "SISMEMBER no"             "0" SISMEMBER s z
  t  "SMISMEMBER"               $'1\n0\n1' SMISMEMBER s a z d
  ts "SMEMBERS"                 $'a\nb\nc\nd' SMEMBERS s
  t  "SREM"                     "2" SREM s a z b
  ts "SMEMBERS after SREM"      $'c\nd' SMEMBERS s
  t  "SADD s2"                  "3" SADD s2 c e f
  ts "SINTER"                   "c" SINTER s s2
  ts "SUNION"                   $'c\nd\ne\nf' SUNION s s2
  ts "SDIFF"                    "d" SDIFF s s2
  t  "SINTER with missing"      "" SINTER s nokey
  t  "SINTERSTORE"              "1" SINTERSTORE dst s s2
  ts "SINTERSTORE result"       "c" SMEMBERS dst
  t  "SUNIONSTORE"              "4" SUNIONSTORE dst s s2
  t  "SDIFFSTORE empty"         "0" SDIFFSTORE dst s s
  t  "empty result deletes dst" "0" EXISTS dst
  t  "SDIFFSTORE over string"   "1" SDIFFSTORE str s s2
  t  "TYPE after SDIFFSTORE"    "set" TYPE str
  t  "SINTERCARD"               "1" SINTERCARD 2 s s2
  t  "SINTERCARD LIMIT"         "2" SINTERCARD 1 s2 LIMIT 2
  t  "SMOVE"                    "1" SMOVE s2 s e
  t  "SMOVE missing member"     "0" SMOVE s2 s nope
  t  "SISMEMBER after SMOVE"    "1" SISMEMBER s e
  tx "SRANDMEMBER"              '^(c|d|e)$' SRANDMEMBER s
  ts "SRANDMEMBER count"        $'c\nd\ne' SRANDMEMBER s 10
  tx "SRANDMEMBER negative"     $'^([cde]\n){4}[cde]$' SRANDMEMBER s -5
  tx "SPOP"                     '^(c|d|e)$' SPOP s
  t  "SCARD after SPOP"         "2" SCARD s
  tx "SPOP count"               $'^[cde]\n[cde]$' SPOP s 5
  t  "set emptied by SPOP"      "0" EXISTS s
  t  "SPOP missing"             "" SPOP s
  t  "TYPE set"                 "set" TYPE s2
  tc "SADD on hash"             "WRONGTYPE" SADD hh x
  t  "SET for WRONGTYPE"        "OK" SET plain v
  tc "SINTER on string"         "WRONGTYPE" SINTER s2 plain

  section "sorted set"
  t  "ZADD"                     "3" ZADD z 1 a 2 b 3 c
  t  "ZADD update"              "0" ZADD z 1.5 a
  t  "ZADD CH"                  "1" ZADD z CH 2.5 a
  t  "ZADD NX"                  "1" ZADD z NX 10 a 4 d
  t  "ZADD XX"                  "0" ZADD z XX 5 e
  t  "ZADD GT lower"            "0" ZADD z GT CH 1 d
  t  "ZADD GT higher"           "1" ZADD z GT CH 6 d
  t  "ZADD LT lower"            "1" ZADD z LT CH 5 d
  t  "ZADD INCR"                "7" ZADD z INCR 2 d
  t  "ZADD NX INCR existing"    "" ZADD z NX INCR 1 d
  tc "ZADD NX + XX"             "ERR XX and NX options" ZADD z NX XX 1 a
  tc "ZADD GT + LT"             "ERR GT, LT, and/or NX" ZADD z GT LT 1 a
  tc "ZADD bad score"           "ERR value is not a valid float" ZADD z abc a
  tc "ZADD INCR two pairs"      "ERR INCR option supports a single" ZADD z INCR 1 a 2 b
  t  "ZCARD"                    "4" ZCARD z
  t  "ZSCORE"                   "2.5" ZSCORE z a
  t  "ZSCORE missing"           "" ZSCORE z nope
  t  "ZMSCORE"                  $'2.5\n\n7' ZMSCORE z a nope d
  t  "ZINCRBY"                  "3.5" ZINCRBY z 1 a
  t  "ZRANGE"                   $'b\nc\na\nd' ZRANGE z 0 -1
  t  "ZRANGE WITHSCORES"        $'b\n2\nc\n3' ZRANGE z 0 1 WITHSCORES
  t  "ZRANGE REV"               $'d\na' ZRANGE z 0 1 REV
  t  "ZREVRANGE"                $'d\na\nc\nb' ZREVRANGE z 0 -1
  t  "ZRANGE BYSCORE"           $'c\na' ZRANGE z 3 5 BYSCORE
  t  "ZRANGE BYSCORE exclusive" "a" ZRANGE z "(3" 5 BYSCORE
  t  "ZRANGE BYSCORE REV LIMIT" $'d\na' ZRANGE z +inf -inf BYSCORE REV LIMIT 0 2
  t  "ZRANGEBYSCORE"            $'b\nc\na' ZRANGEBYSCORE z -inf 4
  t  "ZRANGEBYSCORE LIMIT"      "c" ZRANGEBYSCORE z -inf +inf LIMIT 1 1
  t  "ZREVRANGEBYSCORE"         $'a\n3.5\nc\n3' ZREVRANGEBYSCORE z 4 3 WITHSCORES
  tc "ZRANGE LIMIT by rank"     "ERR syntax error, LIMIT" ZRANGE z 0 1 LIMIT 0 1
  tc "bad score range"          "ERR min or max is not a float" ZRANGEBYSCORE z a b
  t  "ZCOUNT"                   "2" ZCOUNT z 3 3.5
  t  "ZRANK"                    "2" ZRANK z a
  t  "ZREVRANK"                 "1" ZREVRANK z a
  t  "ZRANK WITHSCORE"          $'2\n3.5' ZRANK z a WITHSCORE
  t  "ZRANK missing"            "" ZRANK z nope
  t  "ZADD equal scores"        "4" ZADD lex 0 a 0 b 0 c 0 d
  t  "ZRANGEBYLEX"              $'b\nc' ZRANGEBYLEX lex "[b" "(d"
  t  "ZRANGEBYLEX all"          $'a\nb\nc\nd' ZRANGEBYLEX lex - +
  t  "ZREVRANGEBYLEX"           $'d\nc' ZREVRANGEBYLEX lex + "(b"
  t  "ZRANGE BYLEX LIMIT"       "b" ZRANGE lex - + BYLEX LIMIT 1 1
  t  "ZLEXCOUNT"                "3" ZLEXCOUNT lex "(a" +
  tc "bad lex range"            "ERR min or max not valid string range item" ZRANGEBYLEX lex a b
  t  "ZREMRANGEBYLEX"           "2" ZREMRANGEBYLEX lex "[a" "[b"
  t  "ZREM"                     "1" ZREM lex c nope
  t  "ZPOPMIN"                  $'b\n2' ZPOPMIN z
  t  "ZPOPMAX count"            $'d\n7\na\n3.5' ZPOPMAX z 2
  t  "ZCARD after pops"         "1" ZCARD z
  t  "ZADD more"                "3" ZADD z 1 x 2 y 10 w
  t  "ZREMRANGEBYRANK"          "2" ZREMRANGEBYRANK z 0 1
  t  "ZREMRANGEBYSCORE"         "1" ZREMRANGEBYSCORE z "(3" +inf
  t  "ZRANGE remaining"         "c" ZRANGE z 0 -1
  t  "ZADD +inf"                "2" ZADD zi +inf top -inf bottom
  t  "ZSCORE +inf"              "inf" ZSCORE zi top
  t  "ZRANGE infinities"        $'bottom\n-inf\ntop\ninf' ZRANGE zi 0 -1 WITHSCORES
  t  "TYPE zset"                "zset" TYPE z
  tc "ZADD on string"           "WRONGTYPE" ZADD plain 1 a

  section "list"
  t  "RPUSH"                    "3" RPUSH l a b c
  t  "LPUSH"                    "5" LPUSH l y z
  t  "LRANGE"                   $'z\ny\na\nb\nc' LRANGE l 0 -1
  t  "LRANGE negative"          $'b\nc' LRANGE l -2 -1
  t  "LRANGE out of range"      "" LRANGE l 10 20
  t  "LLEN"                     "5" LLEN l
  t  "LINDEX"                   "a" LINDEX l 2
  t  "LINDEX negative"          "c" LINDEX l -1
  t  "LINDEX out of range"      "" LINDEX l 10
  t  "LSET"                     "OK" LSET l 0 Z
  tc "LSET out of range"        "ERR index out of range" LSET l 10 x
  tc "LSET missing"             "ERR no such key" LSET nolist 0 x
  t  "LPOP"                     "Z" LPOP l
  t  "RPOP"                     "c" RPOP l
  t  "LPOP count"               $'y\na' LPOP l 2
  t  "RPUSHX"                   "2" RPUSHX l q
  t  "LPUSHX missing"           "0" LPUSHX nolist x
  t  "LPUSHX did not create"    "0" EXISTS nolist
  t  "LPOP missing"             "" LPOP nolist
  t  "LPOP count missing"       "" LPOP nolist 2
  t  "RPUSH with duplicates"    "6" RPUSH l1 a b a c a b
  t  "LREM 2"                   "2" LREM l1 2 a
  t  "LRANGE after LREM"        $'b\nc\na\nb' LRANGE l1 0 -1
  t  "LREM -1"                  "1" LREM l1 -1 b
  t  "LREM 0"                   "1" LREM l1 0 c
  t  "LINSERT BEFORE"           "3" LINSERT l1 BEFORE a X
  t  "LINSERT AFTER"            "4" LINSERT l1 AFTER a Y
  t  "LINSERT at head"          "5" LINSERT l1 BEFORE b H
  t  "LINSERT missing pivot"    "-1" LINSERT l1 BEFORE nope x
  t  "LINSERT missing key"      "0" LINSERT nolist BEFORE a x
  t  "LRANGE after LINSERT"     $'H\nb\nX\na\nY' LRANGE l1 0 -1
  t  "LTRIM"                    "OK" LTRIM l1 1 -2
  t  "LRANGE after LTRIM"       $'b\nX\na' LRANGE l1 0 -1
  t  "RPUSH for LPOS"           "6" RPUSH lp a b c a b c
  t  "LPOS"                     "0" LPOS lp a
  t  "LPOS RANK 2"              "3" LPOS lp a RANK 2
  t  "LPOS RANK -1"             "3" LPOS lp a RANK -1
  t  "LPOS COUNT 0"             $'0\n3' LPOS lp a COUNT 0
  t  "LPOS MAXLEN"              "" LPOS lp c MAXLEN 2
  t  "LPOS missing element"     "" LPOS lp z
  tc "LPOS RANK 0"              "ERR RANK can't be zero" LPOS lp a RANK 0
  t  "LMOVE"                    "c" LMOVE lp ldst RIGHT LEFT
  t  "RPOPLPUSH"                "b" RPOPLPUSH lp ldst
  t  "LRANGE destination"       $'b\nc' LRANGE ldst 0 -1
  t  "LMOVE rotate same list"   "a" LMOVE lp lp RIGHT LEFT
  t  "LRANGE rotated"           $'a\na\nb\nc' LRANGE lp 0 -1
  t  "LMOVE empty source"       "" LMOVE nolist ldst LEFT LEFT
  tc "LMOVE bad side"           "ERR syntax error" LMOVE lp ldst UP DOWN
  t  "LTRIM to empty"           "OK" LTRIM ldst 5 10
  t  "empty list is deleted"    "0" EXISTS ldst
  t  "TYPE list"                "list" TYPE lp
  tc "LPUSH on zset"            "WRONGTYPE" LPUSH z x
  tc "LMOVE into string"        "WRONGTYPE" LMOVE lp plain LEFT LEFT
  t  "LMOVE did not pop"        "4" LLEN lp
  tp "pipeline on one connection" $'3\na\nb\nc\n1\n2' <<< $'RPUSH pl a b c\nLRANGE pl 0 -1\nLREM pl 1 b\nLLEN pl'
  ts "KEYS with collections"    $'l\nl1\nlex\nlp' KEYS "l*"

  section "secondary indexes"
  t  "FLUSHALL"                 "OK" FLUSHALL
  t  "HSET user:1"              "4" HSET user:1 name ana city SP age 30 email ana@x
  t  "HSET user:2"              "4" HSET user:2 name bia city SP age 25 email bia@x
  t  "HSET user:3"              "4" HSET user:3 name caio city RJ age 41 email caio@x
  t  "IDX.CREATE with backfill" "OK" IDX.CREATE users PREFIX 1 user: SCHEMA city TAG age NUMERIC
  wait_index users
  t  "IDX.CREATE UNIQUE"        "OK" IDX.CREATE emails PREFIX 1 user: SCHEMA email TAG UNIQUE
  wait_index emails
  tc "IDX.CREATE duplicate"     "ERR Index already exists" IDX.CREATE users SCHEMA x TAG
  tc "IDX.CREATE bad type"      "ERR unknown field type" IDX.CREATE x SCHEMA f TEXT
  tc "IDX.CREATE no SCHEMA"     "ERR expected SCHEMA" IDX.CREATE x PREFIX 1 a: f TAG
  ts "IDX.LIST"                 $'emails\nusers' IDX.LIST
  tc "IDX.INFO"                 "ready" IDX.INFO users
  t  "IDX.QUERY equality"       $'user:2\nuser:1' IDX.QUERY users city = SP NOCONTENT
  t  "IDX.QUERY range"          "user:2" IDX.QUERY users city = SP age "<" 30 NOCONTENT
  t  "IDX.QUERY DESC LIMIT"     "user:1" IDX.QUERY users city = SP DESC LIMIT 0 1 NOCONTENT
  t  "IDX.QUERY LIMIT 0 0"      "" IDX.QUERY users city = SP LIMIT 0 0 NOCONTENT
  t  "IDX.QUERY RETURN"         $'user:3\nname\ncaio' IDX.QUERY users city = RJ RETURN 1 name
  t  "IDX.QUERY extra filter"   "user:1" IDX.QUERY users city = SP name = ana NOCONTENT
  t  "IDX.QUERY all fields"     $'user:3\nage\n41\ncity\nRJ\nemail\ncaio@x\nname\ncaio' IDX.QUERY users city = RJ
  t  "IDX.COUNT"                "2" IDX.COUNT users city = SP
  t  "IDX.COUNT whole index"    "3" IDX.COUNT users
  t  "IDX.GET unique"           $'user:2\nage\n25\ncity\nSP\nemail\nbia@x\nname\nbia' IDX.GET emails bia@x
  t  "IDX.GET missing"          "" IDX.GET emails nobody@x
  tc "IDX.GET wrong arity"      "give one value for each" IDX.GET users SP
  tc "UNIQUE violation"         "ERR UNIQUE index 'emails'" HSET user:4 email ana@x city SP
  t  "violating HSET undone"    "0" EXISTS user:4
  t  "HSET moves slice"         "0" HSET user:1 city RJ
  t  "query after update"       "user:2" IDX.QUERY users city = SP NOCONTENT
  t  "HINCRBY"                  "26" HINCRBY user:2 age 1
  t  "range as filter"          "user:2" IDX.QUERY users age ">=" 26 age "<" 27 NOCONTENT
  t  "DEL"                      "1" DEL user:2
  t  "count after DEL"          "2" IDX.COUNT users
  t  "RENAME"                   "OK" RENAME user:3 user:30
  t  "query after RENAME"       $'user:1\nuser:30' IDX.QUERY users city = RJ NOCONTENT
  tp "pipelined write + query"  $'2\nuser:6' <<< $'HSET user:6 city POA age 1\nIDX.QUERY users city = POA NOCONTENT'
  tc "query on missing index"   "ERR no such index" IDX.QUERY nope
  tc "bad condition"            "ERR syntax error near" IDX.QUERY users city "~" SP
  tc "non-numeric bound"        "is not a number" IDX.QUERY users city = SP age ">" abc
  t  "IDX.DROP"                 "OK" IDX.DROP emails
  tc "IDX.DROP missing"         "ERR no such index" IDX.DROP emails
  t  "value free after DROP"    "1" HSET user:7 email ana@x
  t  "IDX.DROP users"           "OK" IDX.DROP users
  t  "no indexes left"          "" IDX.LIST
  t  "HSET dev:1"               "2" HSET dev:1 skills "c++, go" level 3
  t  "HSET dev:2"               "2" HSET dev:2 skills go level 5
  t  "IDX.CREATE SEPARATOR"     "OK" IDX.CREATE devs PREFIX 1 dev: SCHEMA skills TAG SEPARATOR , level NUMERIC
  wait_index devs
  t  "IDX.QUERY several values" $'dev:1\ndev:2' IDX.QUERY devs skills = go NOCONTENT
  t  "IDX.QUERY other value"    "dev:1" IDX.QUERY devs skills = c++ NOCONTENT
  t  "IDX.COUNT counts keys"    "2" IDX.COUNT devs
  t  "IDX.DROP devs"            "OK" IDX.DROP devs

  section "search (FT.*)"
  t  "HSET item:1"              "3" HSET item:1 color "Red, Green" price 10 size M
  t  "HSET item:2"              "3" HSET item:2 color blue price 25.5 size m
  t  "HSET item:3"              "2" HSET item:3 color red price 40
  t  "FT.CREATE"                "OK" FT.CREATE items ON HASH PREFIX 1 item: SCHEMA color TAG price NUMERIC SORTABLE size AS sz TAG
  wait_index items
  tc "FT.CREATE TEXT"           "TEXT fields are not supported" FT.CREATE t SCHEMA body TEXT
  t  "FT._LIST"                 "items" FT._LIST
  t  "FT.SEARCH tag"            $'2\nitem:1\nitem:3' FT.SEARCH items "@color:{RED}" NOCONTENT
  t  "FT.SEARCH union"          $'3\nitem:1\nitem:2\nitem:3' FT.SEARCH items "@color:{red|blue}" NOCONTENT
  t  "FT.SEARCH prefix"         $'1\nitem:1' FT.SEARCH items "@color:{gr*}" NOCONTENT
  t  "FT.SEARCH range"          $'1\nitem:2' FT.SEARCH items "@price:[(10 (40]" NOCONTENT
  t  "FT.SEARCH and / not"      $'1\nitem:1' FT.SEARCH items "@color:{red} -@price:[20 +inf]" NOCONTENT
  t  "FT.SEARCH PARAMS"         $'1\nitem:2' FT.SEARCH items '@price:[$lo $hi]' PARAMS 4 lo 20 hi 30 DIALECT 2 NOCONTENT
  t  "FT.SEARCH SORTBY LIMIT"   $'3\nitem:3\nitem:2' FT.SEARCH items "*" SORTBY price DESC LIMIT 0 2 NOCONTENT
  t  "FT.SEARCH count only"     "3" FT.SEARCH items "*" LIMIT 0 0
  t  "FT.SEARCH RETURN AS"      $'1\nitem:2\np\n25.5' FT.SEARCH items "@sz:{M} @price:[20 30]" RETURN 3 price AS p
  t  "FT.SEARCH whole hash"     $'1\nitem:3\ncolor\nred\nprice\n40' FT.SEARCH items "@price:[40 40]"
  tc "FT.SEARCH text"           "no full-text search" FT.SEARCH items hello
  tc "FT.SEARCH unknown index"  "no such index" FT.SEARCH nope "*"
  t  "HDEL updates"             "1" HDEL item:1 color
  t  "FT.SEARCH after HDEL"     $'1\nitem:3' FT.SEARCH items "@color:{red}" NOCONTENT
  t  "FT.TAGVALS"               $'blue\nred' FT.TAGVALS items color
  tc "FT.INFO"                  "num_docs" FT.INFO items
  t  "JSON.SET doc:1"           "OK" JSON.SET doc:1 '$' '{"name":"Ana","tags":["a","B"],"score":7}'
  t  "JSON.SET doc:2"           "OK" JSON.SET doc:2 '$' '{"name":"Bia","tags":["b"]}'
  t  "FT.CREATE ON JSON"        "OK" FT.CREATE docs ON JSON PREFIX 1 doc: SCHEMA '$.tags[*]' AS tag TAG '$.score' AS score NUMERIC
  wait_index docs
  t  "FT.SEARCH JSON array"     $'2\ndoc:1\ndoc:2' FT.SEARCH docs "@tag:{b}" NOCONTENT
  t  "FT.SEARCH JSON content"   $'1\ndoc:1\n$\n{"name":"Ana","tags":["a","B"],"score":7}' FT.SEARCH docs "@score:[7 7]"
  t  "FT.SEARCH JSON RETURN"    $'1\ndoc:2\n$.name\nBia' FT.SEARCH docs "-@score:[-inf +inf]" RETURN 1 '$.name'
  t  "JSON.ARRAPPEND"           "2" JSON.ARRAPPEND doc:2 '$.tags' '"c"'
  t  "FT.SEARCH after append"   $'1\ndoc:2' FT.SEARCH docs "@tag:{c}" NOCONTENT
  t  "FT.DROPINDEX DD"          "OK" FT.DROPINDEX docs DD
  t  "documents deleted"        "0" EXISTS doc:1 doc:2
  t  "FT.DROPINDEX"             "OK" FT.DROPINDEX items
  t  "items kept"               "3" EXISTS item:1 item:2 item:3
  t  "no FT indexes left"       "" FT._LIST

  section "transactions"
  t  "DEL txn"                  "0" DEL txn txd
  tp "MULTI / EXEC"             $'OK\nQUEUED\nQUEUED\n1\n2' <<< $'MULTI\nINCR txn\nINCR txn\nEXEC'
  tp "DISCARD"                  $'OK\nQUEUED\nOK\n0' <<< $'MULTI\nSET txd 1\nDISCARD\nEXISTS txd'
  tpc "EXECABORT"               "EXECABORT Transaction discarded" <<< $'MULTI\nSET txd\nEXEC'
  tp "error inside EXEC"        $'OK\nQUEUED\nQUEUED\nQUEUED\nOK\nWRONGTYPE Operation against a key holding the wrong kind of value\n\n3' <<< $'MULTI\nSET txd x\nLPUSH txd y\nINCR txn\nEXEC'
  tp "WATCH unchanged"          $'OK\nOK\nQUEUED\n4' <<< $'WATCH txn\nMULTI\nINCR txn\nEXEC'
  tc "EXEC without MULTI"       "ERR EXEC without MULTI" EXEC
  tpc "WATCH inside MULTI"      "ERR WATCH inside MULTI is not allowed" <<< $'MULTI\nWATCH txn'

  section "blocking commands"
  t  "DEL queues"               "0" DEL bq bq2 bz blm bdst
  ( sleep 0.3; cli RPUSH bq job >/dev/null ) &
  t  "BLPOP waits for a push"   $'bq\njob' BLPOP bq2 bq 5
  wait
  t  "BLPOP timeout"            "" BLPOP bq 0.1
  t  "RPUSH for BRPOP"          "2" RPUSH bq a b
  t  "BRPOP ready"              $'bq\nb' BRPOP bq 1
  ( sleep 0.3; cli ZADD bz 2 b 1 a >/dev/null ) &
  t  "BZPOPMIN waits"           $'bz\na\n1' BZPOPMIN bz 5
  wait
  ( sleep 0.3; cli RPUSH blm x >/dev/null ) &
  t  "BLMOVE waits"             "x" BLMOVE blm bdst LEFT RIGHT 5
  wait
  t  "BLMOVE moved it"          "x" LRANGE bdst 0 -1
  t  "RPUSH for LMPOP"          "3" RPUSH blm 1 2 3
  t  "LMPOP"                    $'blm\n3\n2' LMPOP 2 nolist blm RIGHT COUNT 2
  t  "BLMPOP ready"             $'blm\n1' BLMPOP 1 1 blm LEFT
  t  "ZMPOP"                    $'bz\nb\n2' ZMPOP 1 bz MIN
  tc "BLPOP bad timeout"        "ERR timeout is not a float" BLPOP bq abc
  tc "BLPOP negative timeout"   "ERR timeout is negative" BLPOP bq -1

  section "pub/sub"
  sub_out=$(mktemp)
  ( timeout 2 redis-cli -h "$H" -p "$P" $CLI_OPTS SUBSCRIBE news > "$sub_out" 2>&1 ) &
  sleep 0.5
  t  "PUBLISH to a subscriber"  "1" PUBLISH news hello
  t  "PUBSUB CHANNELS"          "news" PUBSUB CHANNELS
  t  "PUBSUB NUMSUB"            $'news\n1' PUBSUB NUMSUB news
  wait
  got=$(cat "$sub_out")
  [[ "$got" == *$'message\nnews\nhello'* ]] && report "subscriber got the message" 1 ||
    report "subscriber got the message" 0 "*message news hello*" "$got" "SUBSCRIBE news"
  rm -f "$sub_out"
  t  "PUBLISH to nobody"        "0" PUBLISH news again
  t  "PUBSUB NUMPAT"            "0" PUBSUB NUMPAT
  got=$(timeout 1 redis-cli -h "$H" -p "$P" $CLI_OPTS PSUBSCRIBE 'news.*' 2>&1)
  [[ "$got" == *$'psubscribe\nnews.*\n1'* ]] && report "PSUBSCRIBE" 1 ||
    report "PSUBSCRIBE" 0 "*psubscribe news.* 1*" "$got" "PSUBSCRIBE news.*"

  section "scripting"
  t  "EVAL"                     "hello" EVAL "return 'hello'" 0
  t  "EVAL KEYS and ARGV"       $'k1\nv1' EVAL "return {KEYS[1], ARGV[1]}" 1 k1 v1
  t  "EVAL redis.call"          "OK" EVAL "return redis.call('SET', KEYS[1], ARGV[1])" 1 lua:k 5
  t  "EVAL integer reply"       "12" EVAL "return redis.call('INCRBY', KEYS[1], ARGV[1])" 1 lua:k 7
  sha=$(cli SCRIPT LOAD "return ARGV[1] .. ARGV[1]")
  tx "SCRIPT LOAD"              '^[0-9a-f]{40}$' SCRIPT LOAD "return ARGV[1] .. ARGV[1]"
  t  "EVALSHA"                  "abab" EVALSHA "$sha" 0 ab
  t  "SCRIPT EXISTS"            $'1\n0' SCRIPT EXISTS "$sha" 0000000000000000000000000000000000000000
  tc "error from redis.call"    "WRONGTYPE" EVAL "return redis.call('LPUSH', KEYS[1], 'x')" 1 lua:k
  t  "redis.pcall"              "WRONGTYPE Operation against a key holding the wrong kind of value" EVAL "return redis.pcall('LPUSH', KEYS[1], 'x').err" 1 lua:k
  tc "EVAL_RO refuses writes"   "Write commands are not allowed from read-only scripts" EVAL_RO "return redis.call('DEL', KEYS[1])" 1 lua:k
  t  "cjson"                    '{"a":[1,2]}' EVAL "return cjson.encode({a = {1, 2}})" 0
  tc "no new globals"           "Script attempted to create global variable" EVAL "x = 1" 0
  tp "EVAL inside MULTI"        $'OK\nQUEUED\nQUEUED\nOK\n1' <<< $'MULTI\nSET lua:m 0\nEVAL "return redis.call(\'INCR\', KEYS[1])" 1 lua:m\nEXEC'
  t  "SCRIPT FLUSH"             "OK" SCRIPT FLUSH
  tc "EVALSHA after flush"      "NOSCRIPT" EVALSHA "$sha" 0 x
  t  "FUNCTION LOAD"            "clilib" FUNCTION LOAD REPLACE $'#!lua name=clilib\nredis.register_function("cli_double", function(keys, args) return args[1] * 2 end)'
  t  "FCALL"                    "42" FCALL cli_double 0 21
  t  "FCALL_RO needs no-writes" "ERR Can not execute a script with write flag using *_ro command." FCALL_RO cli_double 0 21
  tc "FUNCTION LIST"            "clilib" FUNCTION LIST LIBRARYNAME "cli*"
  t  "FUNCTION DELETE"          "OK" FUNCTION DELETE clilib
  t  "FCALL after delete"       "ERR Function not found" FCALL cli_double 0 21

  section "operations"
  t  "SET for OBJECT"           "OK" SET objn 12345
  t  "OBJECT ENCODING int"      "int" OBJECT ENCODING objn
  t  "OBJECT ENCODING missing"  "" OBJECT ENCODING nosuchkey
  t  "TOUCH"                    "1" TOUCH objn nosuchkey
  tx "MEMORY USAGE"             '^[0-9]+$' MEMORY USAGE objn
  tc "CLIENT LIST"              "cmd=client" CLIENT LIST
  t  "CLIENT KILL no such"      "0" CLIENT KILL ADDR 1.2.3.4:5
  t  "SLOWLOG RESET"            "OK" SLOWLOG RESET
  t  "CONFIG SET slowlog"       "OK" CONFIG SET slowlog-log-slower-than 0
  t  "PING for SLOWLOG"         "PONG" PING
  tx "SLOWLOG LEN"              '^[1-9][0-9]*$' SLOWLOG LEN
  tc "SLOWLOG GET"              "PING" SLOWLOG GET 3
  t  "CONFIG SET slowlog back"  "OK" CONFIG SET slowlog-log-slower-than 10000
  tx "LASTSAVE"                 '^[0-9]+$' LASTSAVE
  mon_out=$(mktemp)
  ( timeout 1.5 redis-cli -h "$H" -p "$P" $CLI_OPTS MONITOR > "$mon_out" 2>&1 ) &
  sleep 0.5
  t  "SET seen by MONITOR"      "OK" SET monkey monvalue
  wait
  got=$(cat "$mon_out")
  [[ "$got" == *'"SET" "monkey" "monvalue"'* ]] && report "MONITOR shows commands" 1 ||
    report "MONITOR shows commands" 0 '*"SET" "monkey" "monvalue"*' "$got" "MONITOR"
  rm -f "$mon_out"
  tc "BACKUP outside backup-dir" "backup-dir" BACKUP /

  section "scan"
  t  "FLUSHALL"                 "OK" FLUSHALL
  local mset=(MSET) expected_all="" i
  for i in $(seq 1 60); do mset+=("scan:$i" "v"); done
  t  "MSET 60 keys"             "OK" "${mset[@]}"
  t  "HSET for TYPE"            "1" HSET scanhash f v
  ts "redis-cli --scan"         "$(printf 'scan:%s\n' $(seq 1 60) | sort; echo scanhash)" --scan
  ts "--scan --pattern"         "$(printf 'scan:%s\n' 1 10 11 12 13 14 15 16 17 18 19 | sort)" --scan --pattern 'scan:1*'
  ts "--scan --count"           "$(printf 'scan:%s\n' $(seq 1 60) | sort; echo scanhash)" --scan --count 7
  t  "SCAN TYPE hash"           $'0\nscanhash' SCAN 0 COUNT 1000 TYPE hash
  tx "SCAN first page"          $'^[1-9][0-9]*\n(scan:[0-9]+\n){9}scan:[0-9]+$' SCAN 0 COUNT 10
  tc "SCAN bad cursor"          "ERR invalid cursor" SCAN 4242
  tc "SCAN COUNT 0"             "ERR syntax error" SCAN 0 COUNT 0
  t  "HSET small hash"          "3" HSET hs a 1 b 2 c 3
  t  "HSCAN"                    $'0\na\n1\nb\n2\nc\n3' HSCAN hs 0
  t  "HSCAN NOVALUES MATCH"     $'0\nb' HSCAN hs 0 MATCH "b*" NOVALUES
  t  "SADD"                     "3" SADD ss x y z
  t  "SSCAN"                    $'0\nx\ny\nz' SSCAN ss 0
  t  "ZADD"                     "2" ZADD zs 1.5 m1 2 m2
  t  "ZSCAN"                    $'0\nm1\n1.5\nm2\n2' ZSCAN zs 0
  tx "HSCAN with COUNT pages"   $'^[1-9][0-9]*\na\n1$' HSCAN hs 0 COUNT 1
  t  "SSCAN missing key"        "0" SSCAN nokey 0
  tc "SSCAN wrong type"         "WRONGTYPE" SSCAN hs 0

  section "config set"
  t  "CONFIG SET one"           "OK" CONFIG SET cache-max-idle 2m
  t  "CONFIG GET it"            $'cache-max-idle\n2m' CONFIG GET cache-max-idle
  t  "CONFIG SET several"       "OK" CONFIG SET fsync always cache-on-write no
  tc "INFO shows fsync"         "fsync:always" INFO persistence
  t  "write with fsync always"  "OK" SET fsynced v
  t  "CONFIG SET back"          "OK" CONFIG SET fsync everysec cache-on-write yes cache-max-idle 5m
  tc "immutable parameter"      "can't set immutable config" CONFIG SET port 7000
  tc "unknown parameter"        "Unknown option" CONFIG SET no-such-option 10
  tc "bad value"                "ERR CONFIG SET failed" CONFIG SET fsync sometimes
  tc "all or nothing"           "ERR CONFIG SET failed" CONFIG SET log-level debug cache-min-memory 99gb
  t  "nothing was applied"      $'log-level\ninfo' CONFIG GET log-level

  section "auth"
  tc "AUTH without password"    "without any password configured" AUTH x
  t  "CONFIG SET requirepass"   "OK" CONFIG SET requirepass s3cret
  tc "new connection: NOAUTH"   "NOAUTH Authentication required" GET k
  tc "wrong password"           "WRONGPASS" AUTH nope
  tc "wrong user"               "WRONGPASS" AUTH bob s3cret
  tp "AUTH then commands"       $'OK\nPONG' <<< $'AUTH s3cret\nPING'
  tp "AUTH with user"           $'OK\nPONG' <<< $'AUTH default s3cret\nPING'
  local got
  got=$(REDISCLI_AUTH=s3cret redis-cli -h "$H" -p "$P" $CLI_OPTS PING 2>&1)
  [ "$got" = PONG ] && report "REDISCLI_AUTH" 1 || report "REDISCLI_AUTH" 0 PONG "$got" "PING"
  got=$(redis-cli -h "$H" -p "$P" $CLI_OPTS HELLO 3 AUTH default s3cret 2>&1)
  [[ "$got" == *datadam* ]] && report "HELLO AUTH" 1 || report "HELLO AUTH" 0 "*datadam*" "$got" "HELLO 3 AUTH"
  got=$(REDISCLI_AUTH=s3cret redis-cli -h "$H" -p "$P" $CLI_OPTS CONFIG SET requirepass "" 2>&1)
  [ "$got" = OK ] && report "clear requirepass" 1 || report "clear requirepass" 0 OK "$got" "CONFIG SET requirepass ''"
  t  "no AUTH needed again"     "PONG" PING

  section "acl"
  t  "ACL SETUSER"              "OK" ACL SETUSER alice on '>pw' '~app:*' +@read +set
  t  "ACL USERS"                $'alice\ndefault' ACL USERS
  tc "ACL LIST"                 "user alice on #" ACL LIST
  tc "ACL GETUSER"              "~app:*" ACL GETUSER alice
  tu "logged in as alice"       "alice" alice pw ACL WHOAMI
  tu "alice writes her key"     "OK" alice pw SET app:1 v
  tu "alice reads her key"      "v" alice pw GET app:1
  tu "other key denied"         "NOPERM No permissions to access a key" alice pw GET other
  tu "command denied"           "NOPERM User alice has no permissions to run the 'del' command" alice pw DEL app:1
  tu "MGET checks every key"    "NOPERM No permissions to access a key" alice pw MGET app:1 secret
  t  "SET a key alice can't see" "OK" SET secret x
  tu "SCAN shows only hers"     $'0\napp:1' alice pw SCAN 0 COUNT 1000
  tpc "AUTH user password"      "alice" <<< $'AUTH alice pw\nACL WHOAMI'
  tpc "wrong password"          "WRONGPASS" <<< $'AUTH alice nope'
  t  "permissions change live"  "OK" ACL SETUSER alice -set
  tu "SET now denied"           "NOPERM User alice has no permissions to run the 'set' command" alice pw SET app:1 w
  t  "read-only pattern"        "OK" ACL SETUSER ro on nopass '%R~app:*' +@all
  tu "read-only reads"          "v" ro x GET app:1
  tu "read-only cannot write"   "NOPERM No permissions to access a key" ro x SET app:1 w
  t  "subcommand rule"          "OK" ACL SETUSER ops on nopass allkeys +@all -config '+config|get'
  tu "CONFIG GET allowed"       $'port\n6379' ops x CONFIG GET port
  tu "CONFIG SET denied"        "NOPERM User ops has no permissions to run the 'config|set' command" ops x CONFIG SET log-level info
  tc "ACL CAT"                  "sortedset" ACL CAT
  tc "ACL CAT hash"             "hgetall" ACL CAT hash
  tc "bad rule"                 "Unknown command or category name in ACL" ACL SETUSER alice +nosuchcmd
  tc "off user cannot log in"   "OK" ACL SETUSER alice off
  tpc "AUTH as disabled user"   "WRONGPASS" <<< $'AUTH alice pw'
  t  "ACL DELUSER"              "3" ACL DELUSER alice ro ops nobody
  tc "default cannot go"        "cannot be removed" ACL DELUSER default
  t  "only default left"        "default" ACL USERS
  tc "CONFIG REWRITE"           "OK" CONFIG REWRITE

  section "change feed"
  if [ "$(cli CDC.INFO | sed -n 2p)" = yes ]; then
    local next out
    next=$(( $(cli CDC.INFO | sed -n 10p) + 1 ))
    t  "SET for the feed"         "OK" SET cdc:1 v
    t  "HSET for the feed"        "1" HSET cdc:h f v
    tx "CDC.READ sees the SET"    $'set\ncdc:1\nv\n0' CDC.READ "$next" MATCH 'cdc:*'
    tx "CDC.READ sees the HSET"   $'hset\ncdc:h\nf\nv' CDC.READ "$next" MATCH 'cdc:*'
    tx "COUNT stops after a batch" $'^[0-9]+\n[0-9]+\n[0-9]+\nset\ncdc:1\nv\n0$' CDC.READ "$next" COUNT 1 MATCH 'cdc:*'
    tx "nothing new after \$"     '^[0-9]+$' CDC.READ '$' MATCH 'cdc:*'
    out=$(mktemp)
    cli CDC.READ '$' MATCH 'cdc:blk' BLOCK 10000 > "$out" &
    sleep 0.5
    t  "write while blocked"      "OK" SET cdc:blk x
    wait
    [[ "$(cat "$out")" == *"cdc:blk"* ]] && report "BLOCK wakes on the write" 1 \
      || report "BLOCK wakes on the write" 0 "*cdc:blk*" "$(cat "$out")" "CDC.READ \$ BLOCK 10000"
    rm -f "$out"
    tx "BLOCK times out empty"    '^[0-9]+$' CDC.READ '$' MATCH 'cdc:none' BLOCK 200
  else
    tc "feed off without wal-retention" "wal-retention" CDC.READ 0
  fi
}

write_phase() {
  section "write data set"
  t "FLUSHALL"            "OK" FLUSHALL
  t "SET string"          "OK" SET persist:str "olá mundo"
  t "INCRBY counter"      "42" INCRBY persist:counter 42
  t "SET with 1h TTL"     "OK" SET persist:ttl v EX 3600
  t "SET with 1.5s TTL"   "OK" SET persist:short v PX 1500
  t "SET 100KB"           "OK" -x SET persist:big < "$big_file"
  t "MSET"                "OK" MSET persist:m1 1 persist:m2 2
  cli SET persist:deleted x >/dev/null
  t "DEL"                 "1" DEL persist:deleted
  t "RENAME"              "OK" RENAME persist:m2 persist:renamed
  t "APPEND"              "18" APPEND persist:str " de novo"
  t "HSET"                "2" HSET persist:hash f1 v1 f2 v2
  t "SADD"                "3" SADD persist:set a b c
  t "ZADD"                "3" ZADD persist:zset 1 one 2.5 two -3 minus
  t "RPUSH"               "3" RPUSH persist:list x y z
  t "LPUSH"               "4" LPUSH persist:list w
  t "LPOP"                "w" LPOP persist:list
  t "HSET with 1h TTL"    "1" HSET persist:hashttl f v
  t "EXPIRE hash"         "1" EXPIRE persist:hashttl 3600
  t "ZADD with 1.5s TTL"  "1" ZADD persist:zshort 1 a
  t "PEXPIRE zset"        "1" PEXPIRE persist:zshort 1500
  t "SADD to delete"      "2" SADD persist:delset a b
  t "DEL set"             "1" DEL persist:delset
  t "RPUSH to rename"     "2" RPUSH persist:list2 p q
  t "RENAME list"         "OK" RENAME persist:list2 persist:listmoved
  cli IDX.DROP pidx >/dev/null  # left over from an earlier cycle
  t "IDX.CREATE"          "OK" IDX.CREATE pidx PREFIX 1 persist:h SCHEMA kind TAG n NUMERIC
  t "HSET indexed 1"      "2" HSET persist:h1 kind a n 1
  t "HSET indexed 2"      "2" HSET persist:h2 kind a n 2
  t "HSET indexed 3"      "2" HSET persist:h3 kind b n 3
  wait_index pidx
  t "IDX.QUERY"           $'persist:h1\npersist:h2' IDX.QUERY pidx kind = a NOCONTENT
  t "CONFIG SET to keep"  "OK" CONFIG SET cache-max-idle 3m
  t "ACL user to keep"    "OK" ACL SETUSER keeper on '>kpw' '~persist:*' +@read
  t "CONFIG REWRITE"      "OK" CONFIG REWRITE
  t "FUNCTION LOAD"       "plib" FUNCTION LOAD REPLACE $'#!lua name=plib\nredis.register_function("p_get", function(keys) return redis.call("GET", keys[1]) end)'
}

verify_phase() {
  section "verify data set after restart"
  t  "string"                   "olá mundo de novo" GET persist:str
  t  "counter"                  "42" GET persist:counter
  t  "counter still increments" "43" INCR persist:counter
  tx "1h TTL preserved"         '^3[0-9]{3}$' TTL persist:ttl
  t  "1.5s key expired"         "0" EXISTS persist:short
  t  "100KB value size"         "100000" STRLEN persist:big
  t  "MSET values"              "1" MGET persist:m1 persist:m2
  t  "renamed key"              "2" GET persist:renamed
  t  "deleted key stays gone"   "0" EXISTS persist:deleted
  t  "hash"                     $'f1\nv1\nf2\nv2' HGETALL persist:hash
  ts "set"                      $'a\nb\nc' SMEMBERS persist:set
  t  "zset"                     $'minus\n-3\none\n1\ntwo\n2.5' ZRANGE persist:zset 0 -1 WITHSCORES
  t  "list"                     $'x\ny\nz' LRANGE persist:list 0 -1
  t  "list still pushes"        "4" LPUSH persist:list v
  tx "hash 1h TTL preserved"    '^3[0-9]{3}$' TTL persist:hashttl
  t  "1.5s zset expired"        "0" EXISTS persist:zshort
  t  "deleted set stays gone"   "0" SCARD persist:delset
  t  "renamed list"             $'p\nq' LRANGE persist:listmoved 0 -1
  ts "KEYS"                     $'persist:big\npersist:counter\npersist:h1\npersist:h2\npersist:h3\npersist:hash\npersist:hashttl\npersist:list\npersist:listmoved\npersist:m1\npersist:renamed\npersist:set\npersist:str\npersist:ttl\npersist:zset' KEYS "persist:*"
  t  "rewritten setting kept"   $'cache-max-idle\n3m' CONFIG GET cache-max-idle
  tu "rewritten user kept"      "43" keeper kpw GET persist:counter
  tu "and its limits"           "NOPERM User keeper has no permissions to run the 'set' command" keeper kpw SET persist:x 1
  t  "index definition kept"    "pidx" IDX.LIST
  wait_index pidx
  t  "index query after restart" $'persist:h1\npersist:h2' IDX.QUERY pidx kind = a NOCONTENT
  t  "range after restart"      $'persist:h3' IDX.QUERY pidx kind = b n ">=" 3 NOCONTENT
  t  "index still maintained"   "2" HSET persist:h4 kind a n 0
  t  "query sees new hash"      $'persist:h4\npersist:h1\npersist:h2' IDX.QUERY pidx kind = a NOCONTENT
  t  "function kept"            "43" FCALL p_get 1 persist:counter
}

case "$PHASE" in
  commands) commands ;;
  write) write_phase ;;
  verify) verify_phase ;;
  *) echo "unknown phase $PHASE"; exit 2 ;;
esac
rm -f "$big_file"
echo "$PHASE: $pass passed, $fail failed"
[ "$fail" = 0 ]
