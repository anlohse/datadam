#include "server/scripting.h"

#include <lua.hpp>
#include <luajit.h>
#include <openssl/evp.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <random>
#include <set>

#include "index/index_def.h"
#include "json/json.h"
#include "server/command.h"
#include "storage/data_keys.h"
#include "storage/rocks_storage.h"
#include "util/clock.h"
#include "util/log.h"
#include "util/strings.h"

// A Lua error unwinds the C stack (an SEH exception on Windows, the
// unwinder elsewhere), which C++ code compiled for C++ exceptions only must
// not see: a C function that holds C++ objects is an `impl` that never
// raises. It leaves the error value on the stack and returns -1, and
// raising<impl>, which holds nothing, raises it. The impls are not inlined,
// so their objects stay in their own frame.

#if defined(_MSC_VER)
#define DATADAM_NOINLINE __declspec(noinline)
#else
#define DATADAM_NOINLINE __attribute__((noinline))
#endif

namespace datadam {

namespace {

template <int (*Impl)(lua_State*)>
int raising(lua_State* L) {
  int n = Impl(L);
  return n < 0 ? lua_error(L) : n;
}

// The string argument at idx, or (with an error message pushed) null.
const char* string_arg(lua_State* L, int idx, size_t* len) {
  int t = lua_type(L, idx);
  if (t != LUA_TSTRING && t != LUA_TNUMBER) {
    lua_pushfstring(L, "bad argument #%d (string expected, got %s)", idx, lua_typename(L, t));
    return nullptr;
  }
  return lua_tolstring(L, idx, len);
}

constexpr std::string_view kFunctionLibPrefix = "function:lib:";
constexpr std::string_view kFunctionVersionKey = "function:version";
constexpr int kMaxReplyDepth = 100;
constexpr int kHookInstructions = 100000;  // SCRIPT KILL is checked this often

char kEngineKey;  // its address keys the engine in the Lua registry

// Commands a script may not call (as Redis's "noscript" ones).
int64_t steady_us() {
  using namespace std::chrono;
  return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

bool forbidden_in_scripts(std::string_view name) {
  static const std::set<std::string_view> names = {
      "auth",      "hello",      "quit",        "reset",       "multi",       "exec",        "discard",
      "watch",     "unwatch",    "subscribe",   "psubscribe",  "ssubscribe",  "unsubscribe", "punsubscribe",
      "sunsubscribe", "monitor", "eval",        "evalsha",     "eval_ro",     "evalsha_ro",  "fcall",
      "fcall_ro",  "script",     "function",    "replicaof",   "slaveof",     "replsync",    "replconf",
      "failover",  "shutdown",   "wait",        "client",      "cdc.read",    "sync",        "psync",
  };
  return names.contains(name);
}

// ---- RESP reply (as execute_redis_command wrote it) -> Lua value

void set_field(lua_State* L, const char* key) { lua_setfield(L, -2, key); }

// Pushes the reply at the front of `in`; false if it is malformed. *error is
// set when the reply (at the top level) is an error.
bool push_reply(lua_State* L, std::string_view& in, int depth, bool* error) {
  if (in.size() < 3 || depth > kMaxReplyDepth) return false;
  char type = in[0];
  size_t eol = in.find("\r\n");
  if (eol == std::string_view::npos) return false;
  std::string_view head = in.substr(1, eol - 1);
  in.remove_prefix(eol + 2);
  auto count = [&]() -> int64_t { return parse_int64(head).value_or(-1); };
  switch (type) {
    case '+':
      lua_newtable(L);
      lua_pushlstring(L, head.data(), head.size());
      set_field(L, "ok");
      return true;
    case '-':
      lua_newtable(L);
      lua_pushlstring(L, head.data(), head.size());
      set_field(L, "err");
      if (depth == 0 && error) *error = true;
      return true;
    case ':':
      lua_pushnumber(L, static_cast<lua_Number>(parse_int64(head).value_or(0)));
      return true;
    case '$': {
      int64_t n = count();
      if (n < 0) {
        lua_pushboolean(L, 0);
        return true;
      }
      if (in.size() < static_cast<size_t>(n) + 2) return false;
      lua_pushlstring(L, in.data(), static_cast<size_t>(n));
      in.remove_prefix(static_cast<size_t>(n) + 2);
      return true;
    }
    case '*':
    case '~':
    case '>': {
      int64_t n = count();
      if (n < 0) {
        lua_pushboolean(L, 0);
        return true;
      }
      lua_newtable(L);
      for (int64_t i = 1; i <= n; ++i) {
        if (!push_reply(L, in, depth + 1, nullptr)) return false;
        if (type == '~') {  // RESP3 set: {set = {member = true}}
          lua_pushboolean(L, 1);
          lua_rawset(L, -3);
        } else {
          lua_rawseti(L, -2, static_cast<int>(i));
        }
      }
      if (type == '~') {
        lua_newtable(L);
        lua_insert(L, -2);
        set_field(L, "set");
      }
      return true;
    }
    case '%': {  // RESP3 map: {map = {key = value}}
      int64_t n = count();
      lua_newtable(L);
      lua_newtable(L);
      for (int64_t i = 0; i < n; ++i) {
        if (!push_reply(L, in, depth + 1, nullptr) || !push_reply(L, in, depth + 1, nullptr)) return false;
        lua_rawset(L, -3);
      }
      set_field(L, "map");
      return true;
    }
    case '_':
      lua_pushnil(L);
      return true;
    case '#':
      lua_pushboolean(L, head == "t");
      return true;
    case ',': {
      lua_newtable(L);
      auto d = parse_double(head);
      lua_pushnumber(L, d ? *d : 0);
      set_field(L, "double");
      return true;
    }
    case '(':
      lua_newtable(L);
      lua_pushlstring(L, head.data(), head.size());
      set_field(L, "big_number");
      return true;
    default:
      return false;
  }
}

// ---- Lua value -> reply

bool is_cjson_null(lua_State* L, int idx) { return lua_islightuserdata(L, idx) && lua_touserdata(L, idx) == nullptr; }

void reply_from_lua(lua_State* L, int idx, Reply& r, int depth) {
  idx = idx < 0 ? lua_gettop(L) + idx + 1 : idx;
  switch (lua_type(L, idx)) {
    case LUA_TNUMBER: return r.integer(static_cast<int64_t>(lua_tonumber(L, idx)));
    case LUA_TSTRING: {
      size_t n;
      const char* s = lua_tolstring(L, idx, &n);
      return r.bulk({s, n});
    }
    case LUA_TBOOLEAN: return lua_toboolean(L, idx) ? r.integer(1) : r.null();
    case LUA_TTABLE: break;
    default: return r.null();
  }
  if (depth > kMaxReplyDepth) return r.error("ERR reached lua stack limit");
  // {err = ...}, {ok = ...}, {double = ...}, {map = ...}, {set = ...}, or an array.
  lua_getfield(L, idx, "err");
  if (lua_type(L, -1) == LUA_TSTRING) {
    std::string msg = lua_tostring(L, -1);
    lua_pop(L, 1);
    return r.error(msg);
  }
  lua_pop(L, 1);
  lua_getfield(L, idx, "ok");
  if (lua_type(L, -1) == LUA_TSTRING) {
    std::string msg = lua_tostring(L, -1);
    lua_pop(L, 1);
    return r.simple(msg);
  }
  lua_pop(L, 1);
  lua_getfield(L, idx, "double");
  if (lua_type(L, -1) == LUA_TNUMBER) {
    double d = lua_tonumber(L, -1);
    lua_pop(L, 1);
    return r.double_value(d);
  }
  lua_pop(L, 1);
  lua_getfield(L, idx, "map");
  if (lua_type(L, -1) == LUA_TTABLE) {
    int map = lua_gettop(L);
    size_t n = 0;
    for (lua_pushnil(L); lua_next(L, map); lua_pop(L, 1)) ++n;
    r.map(n);
    for (lua_pushnil(L); lua_next(L, map); lua_pop(L, 1)) {
      reply_from_lua(L, -2, r, depth + 1);
      reply_from_lua(L, -1, r, depth + 1);
    }
    lua_pop(L, 1);
    return;
  }
  lua_pop(L, 1);
  lua_getfield(L, idx, "set");
  if (lua_type(L, -1) == LUA_TTABLE) {
    int set = lua_gettop(L);
    size_t n = 0;
    for (lua_pushnil(L); lua_next(L, set); lua_pop(L, 1)) ++n;
    r.set(n);
    for (lua_pushnil(L); lua_next(L, set); lua_pop(L, 1)) reply_from_lua(L, -2, r, depth + 1);
    lua_pop(L, 1);
    return;
  }
  lua_pop(L, 1);
  // An array: elements 1, 2, ... up to the first nil, as in Redis.
  size_t n = 0;
  for (;; ++n) {
    lua_rawgeti(L, idx, static_cast<int>(n + 1));
    bool nil = lua_isnil(L, -1);
    lua_pop(L, 1);
    if (nil) break;
  }
  r.array(n);
  for (size_t i = 1; i <= n; ++i) {
    lua_rawgeti(L, idx, static_cast<int>(i));
    reply_from_lua(L, -1, r, depth + 1);
    lua_pop(L, 1);
  }
}

// ---- cjson

void push_json(lua_State* L, const Json& v) {
  if (v.is_null()) {
    lua_pushlightuserdata(L, nullptr);
  } else if (v.is_boolean()) {
    lua_pushboolean(L, v.get<bool>());
  } else if (v.is_number()) {
    lua_pushnumber(L, v.get<double>());
  } else if (v.is_string()) {
    const auto& s = v.get_ref<const std::string&>();
    lua_pushlstring(L, s.data(), s.size());
  } else if (v.is_array()) {
    lua_createtable(L, static_cast<int>(v.size()), 0);
    int i = 1;
    for (const auto& e : v) {
      push_json(L, e);
      lua_rawseti(L, -2, i++);
    }
  } else {
    lua_createtable(L, 0, static_cast<int>(v.size()));
    for (const auto& [k, e] : v.items()) {
      lua_pushlstring(L, k.data(), k.size());
      push_json(L, e);
      lua_rawset(L, -3);
    }
  }
}

// The Lua value as JSON; false (and *error) if it cannot be encoded.
bool to_json(lua_State* L, int idx, Json* out, int depth, std::string* error) {
  idx = idx < 0 ? lua_gettop(L) + idx + 1 : idx;
  if (depth > 1000) {
    *error = "Cannot serialise, excessive nesting (1001)";
    return false;
  }
  switch (lua_type(L, idx)) {
    case LUA_TNIL: *out = nullptr; return true;
    case LUA_TBOOLEAN: *out = lua_toboolean(L, idx) != 0; return true;
    case LUA_TNUMBER: {
      double d = lua_tonumber(L, idx);
      if (std::isnan(d) || std::isinf(d)) {
        *error = "Cannot serialise number: must not be NaN or Inf";
        return false;
      }
      if (d == std::floor(d) && std::fabs(d) < 9.0e15) *out = static_cast<int64_t>(d);
      else *out = d;
      return true;
    }
    case LUA_TSTRING: {
      size_t n;
      const char* s = lua_tolstring(L, idx, &n);
      *out = std::string(s, n);
      return true;
    }
    case LUA_TLIGHTUSERDATA:
      if (is_cjson_null(L, idx)) {
        *out = nullptr;
        return true;
      }
      break;
    case LUA_TTABLE: {
      // An array if its keys are exactly 1..n.
      size_t count = 0, max = 0;
      bool array = true;
      for (lua_pushnil(L); lua_next(L, idx); lua_pop(L, 1)) {
        ++count;
        if (lua_type(L, -2) != LUA_TNUMBER) {
          array = false;
          continue;
        }
        double k = lua_tonumber(L, -2);
        if (k < 1 || k != std::floor(k)) array = false;
        else max = std::max(max, static_cast<size_t>(k));
      }
      if (array && max == count && count > 0) {
        *out = Json::array();
        for (size_t i = 1; i <= count; ++i) {
          lua_rawgeti(L, idx, static_cast<int>(i));
          Json e;
          bool ok = to_json(L, -1, &e, depth + 1, error);
          lua_pop(L, 1);
          if (!ok) return false;
          out->push_back(std::move(e));
        }
        return true;
      }
      *out = Json::object();
      for (lua_pushnil(L); lua_next(L, idx); lua_pop(L, 1)) {
        std::string key;
        if (lua_type(L, -2) == LUA_TSTRING) {
          key = lua_tostring(L, -2);
        } else if (lua_type(L, -2) == LUA_TNUMBER) {
          lua_pushvalue(L, -2);
          key = lua_tostring(L, -1);
          lua_pop(L, 1);
        } else {
          *error = "Cannot serialise table: table key must be a number or string";
          lua_pop(L, 2);
          return false;
        }
        Json e;
        if (!to_json(L, -1, &e, depth + 1, error)) {
          lua_pop(L, 2);
          return false;
        }
        (*out)[key] = std::move(e);
      }
      return true;
    }
    default: break;
  }
  *error = std::format("Cannot serialise {}: type not supported", lua_typename(L, lua_type(L, idx)));
  return false;
}

DATADAM_NOINLINE int cjson_encode(lua_State* L) {
  std::string error;
  Json v;
  if (!to_json(L, 1, &v, 0, &error)) {
    lua_pushstring(L, error.c_str());
    return -1;
  }
  std::string text = dump_json(v);
  lua_pushlstring(L, text.data(), text.size());
  return 1;
}

DATADAM_NOINLINE int cjson_decode(lua_State* L) {
  size_t n;
  const char* s = string_arg(L, 1, &n);
  if (!s) return -1;
  std::string error;
  auto v = parse_json({s, n}, &error);
  if (!v) {
    std::string_view msg = error;
    if (msg.starts_with("ERR ")) msg.remove_prefix(4);  // run() adds the code
    lua_pushlstring(L, msg.data(), msg.size());
    return -1;
  }
  push_json(L, *v);
  return 1;
}

// ---- small redis.* helpers

DATADAM_NOINLINE int redis_sha1hex(lua_State* L) {
  size_t n;
  const char* s = string_arg(L, 1, &n);
  if (!s) return -1;
  std::string hex = sha1_hex({s, n});
  lua_pushlstring(L, hex.data(), hex.size());
  return 1;
}

int redis_reply_table(lua_State* L, const char* field) {
  luaL_checkstring(L, 1);
  lua_newtable(L);
  lua_pushvalue(L, 1);
  lua_setfield(L, -2, field);
  return 1;
}
int redis_error_reply(lua_State* L) { return redis_reply_table(L, "err"); }
int redis_status_reply(lua_State* L) { return redis_reply_table(L, "ok"); }

DATADAM_NOINLINE int redis_log(lua_State* L) {
  if (lua_type(L, 1) != LUA_TNUMBER) {
    lua_pushstring(L, "First argument must be a number (log level).");
    return -1;
  }
  int level = static_cast<int>(lua_tointeger(L, 1));
  std::string msg;
  for (int i = 2; i <= lua_gettop(L); ++i) {
    if (i > 2) msg += ' ';
    if (const char* s = lua_tostring(L, i)) msg += s;
  }
  if (level >= 3) log_warn("script: {}", msg);
  else if (level == 2) log_info("script: {}", msg);
  else log_debug("script: {}", msg);
  return 0;
}

int redis_true(lua_State* L) {
  lua_pushboolean(L, 1);
  return 1;
}
int redis_nothing(lua_State*) { return 0; }

// The message of an error a script raised: a redis.call error table keeps its
// text; anything else becomes "ERR <message>".
std::string error_message(lua_State* L, int idx) {
  if (lua_type(L, idx) == LUA_TTABLE) {
    lua_getfield(L, idx, "err");
    std::string msg = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "ERR unknown error";
    lua_pop(L, 1);
    return msg;
  }
  const char* s = lua_tostring(L, idx);
  return std::format("ERR {}", s ? s : "unknown error");
}

// "#!lua flags=no-writes,..." or "#!lua name=lib": the words after the engine
// name, and the body with that line left empty (so line numbers stay right).
bool shebang(std::string_view body, std::vector<std::string>* words, std::string* code, std::string* error) {
  *code = std::string(body);
  if (!body.starts_with("#!")) return true;
  size_t eol = body.find('\n');
  std::string_view line = body.substr(2, eol == std::string_view::npos ? std::string_view::npos : eol - 2);
  if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
  auto parts = split_ws(line);
  if (parts.empty() || parts[0] != "lua") {
    *error = std::format("ERR Could not find engine '{}'", parts.empty() ? "" : parts[0]);
    return false;
  }
  for (size_t i = 1; i < parts.size(); ++i) words->emplace_back(parts[i]);
  code->replace(0, eol == std::string_view::npos ? body.size() : eol, "");
  return true;
}

}  // namespace

std::string sha1_hex(std::string_view data) {
  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned int len = 0;
  EVP_Digest(data.data(), data.size(), md, &len, EVP_sha1(), nullptr);
  static const char* hex = "0123456789abcdef";
  std::string out;
  for (unsigned i = 0; i < len; ++i) {
    out.push_back(hex[md[i] >> 4]);
    out.push_back(hex[md[i] & 15]);
  }
  return out;
}

struct ScriptEngine::Run {
  Cmd& cmd;
  ClientState client;
  bool read_only;
};

struct ScriptEngine::Loading {
  std::vector<std::pair<std::string, Function>> functions;
};

ScriptEngine* ScriptEngine::of(lua_State* L) {
  lua_pushlightuserdata(L, &kEngineKey);
  lua_rawget(L, LUA_REGISTRYINDEX);
  auto* e = static_cast<ScriptEngine*>(lua_touserdata(L, -1));
  lua_pop(L, 1);
  return e;
}

ScriptEngine::ScriptEngine(RocksStorage& storage) : storage_(storage) {
  std::lock_guard lock(mu_);
  setup();
  try {
    reload_libraries();
  } catch (const std::exception& e) {
    log_error("scripting: cannot load the stored functions: {}", e.what());
  }
}

ScriptEngine::~ScriptEngine() {
  if (L_) lua_close(L_);
}

void ScriptEngine::setup() {
  L_ = luaL_newstate();
  lua_State* L = L_;
  luaL_openlibs(L);
  // The interpreter only: the instruction hook (SCRIPT KILL) does not see
  // JIT-compiled code. After openlibs: opening the jit library turns it on.
  luaJIT_setmode(L, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF);
  lua_pushlightuserdata(L, &kEngineKey);
  lua_pushlightuserdata(L, this);
  lua_rawset(L, LUA_REGISTRYINDEX);

  // What a script must not reach: files, processes, modules, the JIT, FFI.
  // load and loadstring take bytecode too, which can break out of the sandbox.
  for (const char* name : {"io", "package", "debug", "jit", "require", "module", "dofile", "loadfile", "load",
                           "loadstring", "print", "newproxy", "gcinfo"}) {
    lua_pushnil(L);
    lua_setglobal(L, name);
  }
  // os: only clock, as in Redis.
  lua_newtable(L);
  lua_getglobal(L, "os");
  if (lua_istable(L, -1)) {
    lua_getfield(L, -1, "clock");
    lua_setfield(L, -3, "clock");
  }
  lua_pop(L, 1);
  lua_setglobal(L, "os");

  // cjson
  lua_newtable(L);
  lua_pushcfunction(L, raising<cjson_encode>);
  lua_setfield(L, -2, "encode");
  lua_pushcfunction(L, raising<cjson_decode>);
  lua_setfield(L, -2, "decode");
  lua_pushlightuserdata(L, nullptr);
  lua_setfield(L, -2, "null");
  lua_setglobal(L, "cjson");

  // redis
  lua_newtable(L);
  auto fn = [&](const char* name, lua_CFunction f) {
    lua_pushcfunction(L, f);
    lua_setfield(L, -2, name);
  };
  fn("call", lua_call_cmd);
  fn("pcall", lua_pcall_cmd);
  fn("sha1hex", raising<redis_sha1hex>);
  fn("error_reply", redis_error_reply);
  fn("status_reply", redis_status_reply);
  fn("log", raising<redis_log>);
  fn("replicate_commands", redis_true);
  fn("set_repl", redis_nothing);
  fn("breakpoint", redis_nothing);
  fn("debug", redis_nothing);
  fn("setresp", [](lua_State* L) -> int {
    int v = static_cast<int>(luaL_checkinteger(L, 1));
    if (v != 2 && v != 3) return luaL_error(L, "RESP version must be 2 or 3.");
    if (ScriptEngine* e = of(L); e && e->run_) e->run_->client.proto = v;
    return 0;
  });
  for (auto [name, value] : {std::pair{"LOG_DEBUG", 0}, {"LOG_VERBOSE", 1}, {"LOG_NOTICE", 2}, {"LOG_WARNING", 3},
                             {"REPL_NONE", 0}, {"REPL_AOF", 1}, {"REPL_SLAVE", 2}, {"REPL_REPLICA", 2},
                             {"REPL_ALL", 3}}) {
    lua_pushinteger(L, value);
    lua_setfield(L, -2, name);
  }
  lua_pushstring(L, "7.2.0");
  lua_setfield(L, -2, "REDIS_VERSION");
  lua_pushinteger(L, 0x070200);
  lua_setfield(L, -2, "REDIS_VERSION_NUM");
  lua_pushvalue(L, -1);
  redis_ref_ = luaL_ref(L, LUA_REGISTRYINDEX);
  lua_setglobal(L, "redis");

  // Lua 5.1 names scripts written for Redis use.
  luaL_dostring(L, "if not table.getn then table.getn = function(t) return #t end end");

  lua_sethook(L, [](lua_State* L, lua_Debug*) {
    ScriptEngine* e = of(L);
    if (e && e->kill_.load()) {
      // From now on at every instruction: a script that catches the error
      // with pcall gets it again at its next one, until nothing catches it.
      lua_sethook(L, lua_gethook(L), LUA_MASKCOUNT, 1);
      luaL_error(L, "Script killed by user with SCRIPT KILL...");
    }
  }, LUA_MASKCOUNT, kHookInstructions);
  protect_globals();
}

void ScriptEngine::protect_globals() {
  // Scripts share one interpreter, whoever runs them: none may change what
  // the next one finds (redis.call replaced would run in another user's
  // script, with that user's permissions). As in Redis 7, the globals and the
  // libraries are read-only, and no script may create a global. Each is a
  // proxy that reads through to the real table and hides it (__metatable);
  // rawset refuses them, and getfenv/setfenv, which reach the real globals,
  // are gone.
  static constexpr const char* kProtect = R"lua(
    local real, rawset_, setmetatable_, error_, type_, tostring_ = _G, rawset, setmetatable, error, type, tostring
    local readonly = setmetatable_({}, {__mode = "k"})
    local function refuse() error_("Attempt to modify a readonly table", 3) end
    local function readonly_table(t)
      local proxy = setmetatable_({}, {__index = t, __newindex = refuse, __metatable = false})
      readonly[proxy] = true
      return proxy
    end
    for _, name in ipairs({"redis", "cjson", "string", "table", "math", "os", "coroutine", "bit"}) do
      if type_(real[name]) == "table" then real[name] = readonly_table(real[name]) end
    end
    local string_meta = getmetatable("")
    string_meta.__index = real.string
    string_meta.__metatable = false
    real.getfenv, real.setfenv = nil, nil
    real.rawset = function(t, k, v)
      if readonly[t] then refuse() end
      return rawset_(t, k, v)
    end
    local globals = setmetatable_({}, {
      __index = function(_, n)
        local v = real[n]
        if v == nil then
          error_("Script attempted to access nonexistent global variable '" .. tostring_(n) .. "'", 2)
        end
        return v
      end,
      __newindex = function(_, n)
        if real[n] ~= nil then refuse() end
        error_("Script attempted to create global variable '" .. tostring_(n) .. "'", 2)
      end,
      __metatable = false,
    })
    readonly[globals] = true
    real._G = globals
    return globals, real.redis
  )lua";
  lua_State* L = L_;
  if (luaL_loadstring(L, kProtect) != 0 || lua_pcall(L, 0, 2, 0) != 0) {
    log_error("Lua: protecting the globals failed: {}", lua_tostring(L, -1) ? lua_tostring(L, -1) : "?");
    lua_pop(L, 1);
    return;
  }
  // The redis table functions get (FUNCTION libraries) is the read-only one too.
  luaL_unref(L, LUA_REGISTRYINDEX, redis_ref_);
  redis_ref_ = luaL_ref(L, LUA_REGISTRYINDEX);
  lua_replace(L, LUA_GLOBALSINDEX);  // what scripts load from now on run in
}

// ---- EVAL / SCRIPT

std::string ScriptEngine::load_script(std::string_view body, std::string* sha, bool from_eval) {
  std::lock_guard lock(mu_);
  return load_locked(body, sha, from_eval);
}

void ScriptEngine::eval_script(Cmd& c, std::string_view body, std::span<const std::string_view> keys,
                               std::span<const std::string_view> args, bool read_only) {
  std::lock_guard lock(mu_);
  std::string sha;
  std::string error = load_locked(body, &sha, true);
  if (!error.empty()) return c.r.error(error);
  auto it = scripts_.find(sha);
  run(c, it->second.ref, false, keys, args, read_only || it->second.no_writes, it->first);
}

std::string ScriptEngine::load_locked(std::string_view body, std::string* sha, bool from_eval) {
  *sha = sha1_hex(body);
  if (auto it = scripts_.find(*sha); it != scripts_.end()) {
    Script& s = it->second;
    if (s.evictable && from_eval) {
      eval_lru_.splice(eval_lru_.end(), eval_lru_, s.lru);
    } else if (s.evictable) {  // SCRIPT LOAD keeps it from now on
      eval_lru_.erase(s.lru);
      s.evictable = false;
    }
    return {};
  }
  std::vector<std::string> words;
  std::string code, error;
  if (!shebang(body, &words, &code, &error)) return error;
  bool no_writes = false;
  for (const auto& w : words) {
    if (!w.starts_with("flags=")) return std::format("ERR Unknown lua shebang option: {}", w);
    std::string flags = w.substr(6);
    for (size_t at = 0; at <= flags.size();) {
      size_t comma = flags.find(',', at);
      std::string f = flags.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
      if (f == "no-writes") no_writes = true;
      else if (!f.empty() && f != "allow-oom" && f != "allow-stale" && f != "no-cluster" &&
               f != "allow-cross-slot-keys") {
        return std::format("ERR Unexpected flag in script shebang: {}", f);
      }
      if (comma == std::string::npos) break;
      at = comma + 1;
    }
  }
  if (luaL_loadbuffer(L_, code.data(), code.size(), "@user_script") != 0) {
    std::string msg = lua_tostring(L_, -1);
    lua_pop(L_, 1);
    return std::format("ERR Error compiling script (new function): {}", msg);
  }
  Script& s = scripts_[*sha] = Script{luaL_ref(L_, LUA_REGISTRYINDEX), no_writes};
  if (from_eval) {
    s.evictable = true;
    s.lru = eval_lru_.insert(eval_lru_.end(), *sha);
    if (eval_lru_.size() > kMaxEvalScripts) {
      auto oldest = scripts_.find(eval_lru_.front());
      luaL_unref(L_, LUA_REGISTRYINDEX, oldest->second.ref);
      scripts_.erase(oldest);
      eval_lru_.pop_front();
    }
  }
  return {};
}

bool ScriptEngine::has_script(std::string_view sha) {
  std::lock_guard lock(mu_);
  return scripts_.contains(to_lower(sha));
}

bool ScriptEngine::script_reads_only(std::string_view sha) {
  std::lock_guard lock(mu_);
  auto it = scripts_.find(to_lower(sha));
  return it != scripts_.end() && it->second.no_writes;
}

void ScriptEngine::flush_scripts() {
  std::lock_guard lock(mu_);
  for (auto& [sha, s] : scripts_) luaL_unref(L_, LUA_REGISTRYINDEX, s.ref);
  scripts_.clear();
  eval_lru_.clear();
}

void ScriptEngine::run_script(Cmd& c, std::string_view sha, std::span<const std::string_view> keys,
                              std::span<const std::string_view> args, bool read_only) {
  std::lock_guard lock(mu_);
  auto it = scripts_.find(to_lower(sha));
  if (it == scripts_.end()) return c.r.error("NOSCRIPT No matching script. Please use EVAL.");
  if (it->second.evictable) eval_lru_.splice(eval_lru_.end(), eval_lru_, it->second.lru);
  run(c, it->second.ref, false, keys, args, read_only || it->second.no_writes, it->first);
}

void ScriptEngine::run_function(Cmd& c, std::string_view name, std::span<const std::string_view> keys,
                                std::span<const std::string_view> args, bool read_only) {
  std::lock_guard lock(mu_);
  sync_libraries();
  auto it = functions_.find(name);
  if (it == functions_.end()) return c.r.error("ERR Function not found");
  if (read_only && !it->second.no_writes) {
    return c.r.error("ERR Can not execute a script with write flag using *_ro command.");
  }
  run(c, it->second.ref, true, keys, args, read_only || it->second.no_writes, it->first);
}

void ScriptEngine::run(Cmd& c, int ref, bool as_function, std::span<const std::string_view> keys,
                       std::span<const std::string_view> args, bool read_only, std::string_view what) {
  lua_State* L = L_;
  Run current{c, ClientState{}, read_only};
  current.client.id = c.client.id;
  current.client.user = c.client.user;
  current.client.acl_version = c.client.acl_version;
  current.client.authenticated = true;
  current.client.addr = "lua";
  current.client.proto = 2;
  current.client.in_script = true;
  run_ = &current;
  wrote_ = false;
  kill_ = false;
  started_us_ = steady_us();
  runner_ = std::this_thread::get_id();
  running_ = true;

  int base = lua_gettop(L);
  lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
  auto push_list = [&](std::span<const std::string_view> items) {
    lua_createtable(L, static_cast<int>(items.size()), 0);
    for (size_t i = 0; i < items.size(); ++i) {
      lua_pushlstring(L, items[i].data(), items[i].size());
      lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
  };
  int nargs = 0;
  if (as_function) {
    push_list(keys);
    push_list(args);
    nargs = 2;
  } else {
    lua_pushstring(L, "KEYS");
    push_list(keys);
    lua_rawset(L, LUA_GLOBALSINDEX);
    lua_pushstring(L, "ARGV");
    push_list(args);
    lua_rawset(L, LUA_GLOBALSINDEX);
  }
  // A SCRIPT KILL of the last script left the hook at every instruction.
  if (lua_gethookcount(L) != kHookInstructions) lua_sethook(L, lua_gethook(L), LUA_MASKCOUNT, kHookInstructions);
  int status = lua_pcall(L, nargs, 1, 0);
  run_ = nullptr;
  running_ = false;
  kill_ = false;
  std::vector<std::shared_ptr<KeyWatch::Waiter>> waiting;
  {
    std::lock_guard lock(done_mu_);
    waiting.swap(done_waiters_);
  }
  for (auto& w : waiting) w->fire();
  if (status != 0) {
    std::string msg = error_message(L, -1);
    lua_settop(L, base);
    lua_gc(L, LUA_GCSTEP, 0);
    return c.r.error(std::format("{} script: {}", msg, what));
  }
  reply_from_lua(L, -1, c.r, 0);
  lua_settop(L, base);
}

int ScriptEngine::lua_call_cmd(lua_State* L) {
  ScriptEngine* e = of(L);
  int r = e->call_command(L, false);
  return r < 0 ? lua_error(L) : r;
}

int ScriptEngine::lua_pcall_cmd(lua_State* L) {
  ScriptEngine* e = of(L);
  int r = e->call_command(L, true);
  return r < 0 ? lua_error(L) : r;
}

DATADAM_NOINLINE int ScriptEngine::call_command(lua_State* L, bool protected_call) {
  bool raise = false;
  {
    auto fail = [&](std::string_view msg) {
      lua_settop(L, 0);
      lua_newtable(L);
      lua_pushlstring(L, msg.data(), msg.size());
      lua_setfield(L, -2, "err");
      raise = !protected_call;
    };
    int n = lua_gettop(L);
    if (!run_) {
      fail("ERR redis.call is not available here (only while a script or function runs)");
    } else if (n == 0) {
      fail("ERR Please specify at least one argument for this redis lib call");
    } else {
      std::vector<std::string> args;
      bool bad = false;
      for (int i = 1; i <= n; ++i) {
        int t = lua_type(L, i);
        if (t != LUA_TSTRING && t != LUA_TNUMBER) {
          bad = true;
          break;
        }
        size_t len;
        const char* s = lua_tolstring(L, i, &len);
        args.emplace_back(s, len);
      }
      std::string name = bad ? std::string() : to_lower(args[0]);
      const CommandSpec* spec = bad ? nullptr : find_command(name);
      if (bad) {
        fail("ERR Lua redis lib command arguments must be strings or integers");
      } else if (!spec) {
        fail("ERR Unknown Redis command called from script");
      } else if (forbidden_in_scripts(name)) {
        fail("ERR This Redis command is not allowed from script");
      } else if (run_->read_only && (spec->categories & kCatWrite)) {
        fail("ERR Write commands are not allowed from read-only scripts.");
      } else {
        std::vector<std::string_view> views(args.begin(), args.end());
        // The keys it uses stay locked while the script's writes commit; a
        // command that may touch keys it does not name keeps everything.
        std::vector<std::string_view> keys;
        for_each_key(*spec, views, [&](std::string_view k, uint8_t) {
          keys.push_back(k);
          return true;
        });
        Database& db = run_->cmd.db();
        if ((keys.empty() && (spec->categories & (kCatRead | kCatWrite))) || name == "sort" || name == "sort_ro") {
          db.note_any_key();
        } else {
          db.note_keys(keys);
        }
        std::string out;
        Reply reply(out, run_->client.proto);
        execute_redis_command(run_->cmd.ctx, run_->client, views, reply);
        run_->client.retry_later = false;
        lua_settop(L, 0);
        bool error = false;
        std::string_view in = out;
        if (!push_reply(L, in, 0, &error)) {
          lua_settop(L, 0);
          lua_pushboolean(L, 0);
        }
        if (!error && (spec->categories & kCatWrite)) wrote_ = true;
        if (error) raise = !protected_call;
      }
    }
  }
  return raise ? -1 : 1;
}

int64_t ScriptEngine::running_us() const {
  if (!running_.load(std::memory_order_relaxed)) return 0;
  return std::max<int64_t>(steady_us() - started_us_.load(), 0);
}

std::shared_ptr<KeyWatch::Waiter> ScriptEngine::wait_until_done() {
  std::lock_guard lock(done_mu_);
  if (!running_) return nullptr;  // run() clears running_ before it takes done_mu_
  return done_waiters_.emplace_back(std::make_shared<KeyWatch::Waiter>());
}

std::string ScriptEngine::kill() {
  if (!running_) return "NOTBUSY No scripts in execution right now.";
  if (wrote_) {
    return "UNKILLABLE Sorry the script already executed write commands against the dataset. You can either "
           "wait the script termination or kill the server in a hard way using the SHUTDOWN NOSAVE command.";
  }
  kill_ = true;
  return {};
}

// ---- FUNCTION

int ScriptEngine::lua_register_function(lua_State* L) {
  int r = register_function(L);
  return r < 0 ? lua_error(L) : r;
}

DATADAM_NOINLINE int ScriptEngine::register_function(lua_State* L) {
  ScriptEngine* e = of(L);
  bool failed = false;
  {
    std::string error;
    std::string name;
    std::vector<std::string> flags;
    int callback = 0;
    if (!e->loading_) {
      error = "redis.register_function can only be called on FUNCTION LOAD command";
    } else if (lua_gettop(L) == 2 && lua_type(L, 1) == LUA_TSTRING && lua_type(L, 2) == LUA_TFUNCTION) {
      name = lua_tostring(L, 1);
      callback = 2;
    } else if (lua_gettop(L) == 1 && lua_type(L, 1) == LUA_TTABLE) {
      lua_getfield(L, 1, "function_name");
      if (lua_type(L, -1) == LUA_TSTRING) name = lua_tostring(L, -1);
      lua_getfield(L, 1, "callback");
      if (lua_type(L, -1) == LUA_TFUNCTION) callback = lua_gettop(L);
      lua_getfield(L, 1, "flags");
      if (lua_type(L, -1) == LUA_TTABLE) {
        for (int i = 1;; ++i) {
          lua_rawgeti(L, -1, i);
          if (lua_type(L, -1) != LUA_TSTRING) {
            lua_pop(L, 1);
            break;
          }
          flags.emplace_back(lua_tostring(L, -1));
          lua_pop(L, 1);
        }
      }
      if (name.empty() || !callback) error = "the function_name and callback fields are required";
    } else {
      error = "wrong arguments given to redis.register_function";
    }
    if (error.empty()) {
      for (char ch : name) {
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_') {
          error = "Function names can only contain letters, numbers, or underscores(_) and must be at least one "
                  "character long";
        }
      }
      for (const auto& f : flags) {
        if (f != "no-writes" && f != "allow-oom" && f != "allow-stale" && f != "no-cluster" &&
            f != "allow-cross-slot-keys") {
          error = std::format("unknown flag given: {}", f);
        }
      }
      for (const auto& [existing, fn] : e->loading_->functions) {
        if (existing == name) error = std::format("Function {} already exists", name);
      }
    }
    if (error.empty()) {
      lua_pushvalue(L, callback);
      Function fn{luaL_ref(L, LUA_REGISTRYINDEX), false, flags, {}};
      for (const auto& f : flags) fn.no_writes = fn.no_writes || f == "no-writes";
      e->loading_->functions.emplace_back(name, std::move(fn));
    } else {
      lua_pushstring(L, error.c_str());
      failed = true;
    }
  }
  return failed ? -1 : 0;
}

std::string ScriptEngine::compile_library(std::string_view code, bool replace, std::string* name) {
  std::vector<std::string> words;
  std::string body, error;
  if (!code.starts_with("#!")) return "ERR Missing library metadata";
  if (!shebang(code, &words, &body, &error)) return error;
  for (const auto& w : words) {
    if (w.starts_with("name=")) *name = w.substr(5);
    else return std::format("ERR Invalid metadata value given: {}", w);
  }
  if (name->empty()) return "ERR Library name was not given";
  if (libraries_.contains(*name) && !replace) return std::format("ERR Library '{}' already exists", *name);

  lua_State* L = L_;
  int base = lua_gettop(L);
  if (luaL_loadbuffer(L, body.data(), body.size(), "@user_function") != 0) {
    std::string msg = lua_tostring(L, -1);
    lua_settop(L, base);
    return std::format("ERR Error compiling function: {}", msg);
  }
  // The library runs in an environment of its own, where redis has only
  // register_function and log; its functions get the full redis table.
  lua_newtable(L);  // env
  lua_newtable(L);  // the redis table while loading
  lua_pushcfunction(L, lua_register_function);
  lua_setfield(L, -2, "register_function");
  lua_pushcfunction(L, raising<redis_log>);
  lua_setfield(L, -2, "log");
  for (auto [n, v] : {std::pair{"LOG_DEBUG", 0}, {"LOG_VERBOSE", 1}, {"LOG_NOTICE", 2}, {"LOG_WARNING", 3}}) {
    lua_pushinteger(L, v);
    lua_setfield(L, -2, n);
  }
  lua_setfield(L, -2, "redis");
  lua_newtable(L);  // metatable: everything else from _G
  lua_pushvalue(L, LUA_GLOBALSINDEX);
  lua_setfield(L, -2, "__index");
  lua_setmetatable(L, -2);
  int env = lua_gettop(L);
  lua_pushvalue(L, env);
  lua_setfenv(L, -3);  // the chunk's environment

  Loading loading;
  loading_ = &loading;
  lua_pushvalue(L, -2);
  int status = lua_pcall(L, 0, 0, 0);
  loading_ = nullptr;
  auto unref_all = [&] {
    for (auto& [n, fn] : loading.functions) luaL_unref(L, LUA_REGISTRYINDEX, fn.ref);
  };
  if (status != 0) {
    std::string msg = lua_tostring(L, -1) ? lua_tostring(L, -1) : "unknown error";
    unref_all();
    lua_settop(L, base);
    return std::format("ERR Error registering functions: {}", msg);
  }
  if (loading.functions.empty()) {
    lua_settop(L, base);
    return "ERR No functions registered";
  }
  for (const auto& [n, fn] : loading.functions) {
    auto it = functions_.find(n);
    if (it != functions_.end() && it->second.library != *name) {
      unref_all();
      lua_settop(L, base);
      return std::format("ERR Function {} already exists", n);
    }
  }
  drop_library(*name);
  // From now on the functions see the full redis table.
  lua_rawgeti(L, LUA_REGISTRYINDEX, redis_ref_);
  lua_setfield(L, env, "redis");
  lua_settop(L, base);
  Library lib{std::string(code), {}};
  for (auto& [n, fn] : loading.functions) {
    fn.library = *name;
    lib.functions.push_back(n);
    functions_[n] = std::move(fn);
  }
  libraries_[*name] = std::move(lib);
  library_count_ = libraries_.size();
  function_count_ = functions_.size();
  return {};
}

void ScriptEngine::drop_library(const std::string& name) {
  auto it = libraries_.find(name);
  if (it == libraries_.end()) return;
  for (const auto& f : it->second.functions) {
    if (auto fi = functions_.find(f); fi != functions_.end()) {
      luaL_unref(L_, LUA_REGISTRYINDEX, fi->second.ref);
      functions_.erase(fi);
    }
  }
  libraries_.erase(it);
  library_count_ = libraries_.size();
  function_count_ = functions_.size();
}

uint64_t ScriptEngine::stored_version() {
  std::string raw;
  if (!storage_.get(Cf::System, kFunctionVersionKey, &raw) || raw.size() != 8) return 0;
  return read_be64(raw);
}

void ScriptEngine::persist_library(const std::string& name, const std::string* code) {
  // Random, not a counter: after a full sync from another primary, an equal
  // counter could stand for other libraries.
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  uint64_t version = rng() | 1;
  auto batch = storage_.batch();
  std::string key(kFunctionLibPrefix);
  key += name;
  if (code) batch.put(Cf::System, key, *code);
  else batch.del(Cf::System, key);
  batch.put(Cf::System, kFunctionVersionKey, encode_u64(version));
  storage_.write(batch);
  loaded_version_ = version;
}

void ScriptEngine::reload_libraries() {
  std::vector<std::string> names;
  for (const auto& [n, lib] : libraries_) names.push_back(n);
  for (const auto& n : names) drop_library(n);
  std::string prefix(kFunctionLibPrefix);
  auto c = storage_.cursor(Cf::System, prefix, prefix_end(prefix));
  for (c->seek_first(); c->valid(); c->next()) {
    std::string name, code(c->value());
    std::string error = compile_library(code, true, &name);
    if (!error.empty()) log_warn("scripting: stored library '{}' does not load: {}", c->key(), error);
  }
  c->check_status();
  loaded_version_ = stored_version();
}

void ScriptEngine::sync_libraries() {
  if (stored_version() != loaded_version_) reload_libraries();
}

std::string ScriptEngine::load_library(std::string_view code, bool replace, std::string* name) {
  std::lock_guard lock(mu_);
  sync_libraries();
  std::string error = compile_library(code, replace, name);
  if (!error.empty()) return error;
  std::string text(code);
  persist_library(*name, &text);
  return {};
}

std::string ScriptEngine::delete_library(std::string_view name) {
  std::lock_guard lock(mu_);
  sync_libraries();
  std::string n(name);
  if (!libraries_.contains(n)) return "ERR Library not found";
  drop_library(n);
  persist_library(n, nullptr);
  return {};
}

void ScriptEngine::flush_libraries() {
  std::lock_guard lock(mu_);
  sync_libraries();
  std::vector<std::string> names;
  for (const auto& [n, lib] : libraries_) names.push_back(n);
  for (const auto& n : names) {
    drop_library(n);
    persist_library(n, nullptr);
  }
}

std::vector<ScriptEngine::LibraryInfo> ScriptEngine::libraries() {
  std::lock_guard lock(mu_);
  sync_libraries();
  std::vector<LibraryInfo> out;
  for (const auto& [n, lib] : libraries_) {
    LibraryInfo info{n, lib.code, {}};
    for (const auto& f : lib.functions) {
      auto it = functions_.find(f);
      info.functions.push_back({f, it == functions_.end() ? std::vector<std::string>{} : it->second.flags});
    }
    out.push_back(std::move(info));
  }
  return out;
}

std::optional<bool> ScriptEngine::function_reads_only(std::string_view name) {
  std::lock_guard lock(mu_);
  sync_libraries();
  auto it = functions_.find(name);
  if (it == functions_.end()) return std::nullopt;
  return it->second.no_writes;
}


}  // namespace datadam
