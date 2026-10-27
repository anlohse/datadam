#pragma once

// Lua scripting (EVAL, SCRIPT, FUNCTION, FCALL), with Redis's Lua 5.1 API:
// KEYS and ARGV, redis.call / pcall / error_reply / status_reply / sha1hex /
// log / setresp, and the base, table, string, math, bit and cjson libraries.
//
// A script runs with the whole keyspace to itself (Database::ExecScope with
// every key: KeyLocks' world lock), so it is atomic, as in Redis, whichever
// keys it touches. There is one Lua state; scripts run one at a time.

#include <atomic>
#include <list>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "db/key_watch.h"

struct lua_State;

namespace datadam {

class RocksStorage;
struct Cmd;

class ScriptEngine {
 public:
  explicit ScriptEngine(RocksStorage& storage);
  ~ScriptEngine();
  ScriptEngine(const ScriptEngine&) = delete;
  ScriptEngine& operator=(const ScriptEngine&) = delete;

  // ---- EVAL / SCRIPT
  // Compiles a script and keeps it by its SHA1 (lower-case hex); "" or an error.
  // Scripts that only EVAL loaded are evicted, least recently used first,
  // beyond kMaxEvalScripts; those SCRIPT LOAD loaded stay until SCRIPT FLUSH.
  static constexpr size_t kMaxEvalScripts = 500;
  std::string load_script(std::string_view body, std::string* sha, bool from_eval = false);
  bool has_script(std::string_view sha);
  // The script's flags (#!lua flags=no-writes): whether it only reads.
  bool script_reads_only(std::string_view sha);
  void flush_scripts();

  // ---- FUNCTION
  // FUNCTION LOAD: the library's name, or an error; persisted (and so replicated).
  std::string load_library(std::string_view code, bool replace, std::string* name);
  std::string delete_library(std::string_view name);
  void flush_libraries();
  struct FunctionInfo {
    std::string name;
    std::vector<std::string> flags;
  };
  struct LibraryInfo {
    std::string name;
    std::string code;
    std::vector<FunctionInfo> functions;
  };
  std::vector<LibraryInfo> libraries();
  // nullopt if there is no such function; else whether it is flagged no-writes.
  std::optional<bool> function_reads_only(std::string_view name);

  // ---- running (the caller holds the whole keyspace: it takes that first,
  // then the engine's mutex, so callers waiting for the keyspace spin there
  // instead of sleeping on the mutex)
  // EVAL: loads (caches) the script and runs it; writes the reply to c.r.
  void eval_script(Cmd& c, std::string_view body, std::span<const std::string_view> keys,
                   std::span<const std::string_view> args, bool read_only);
  // EVALSHA: the script by its SHA1.
  void run_script(Cmd& c, std::string_view sha, std::span<const std::string_view> keys,
                  std::span<const std::string_view> args, bool read_only);
  // FCALL / FCALL_RO (read_only: refuses a function not flagged no-writes).
  void run_function(Cmd& c, std::string_view name, std::span<const std::string_view> keys,
                    std::span<const std::string_view> args, bool read_only);

  // SCRIPT KILL / FUNCTION KILL: "" when the running script will stop, else
  // the error to reply (nothing runs, or it already wrote).
  std::string kill();
  bool running() const { return running_.load(); }
  // The thread running the script, while one runs.
  std::thread::id runner() const { return runner_.load(std::memory_order_relaxed); }
  // How long the running script has been running (0: none runs).
  int64_t running_us() const;
  int64_t running_ms() const { return running_us() / 1000; }
  // Fires when the running script ends; null if none runs.
  std::shared_ptr<KeyWatch::Waiter> wait_until_done();
  // FUNCTION STATS (also while a script runs: no lock)
  size_t library_count() const { return library_count_.load(); }
  size_t function_count() const { return function_count_.load(); }

 private:
  struct Script {
    int ref;  // the compiled chunk, in the Lua registry
    bool no_writes;
    bool evictable = false;                   // loaded by EVAL only
    std::list<std::string>::iterator lru{};  // in eval_lru_, when evictable
  };
  struct Function {
    int ref;  // the callback
    bool no_writes;
    std::vector<std::string> flags;
    std::string library;
  };
  struct Library {
    std::string code;
    std::vector<std::string> functions;
  };
  // The script being run, for redis.call.
  struct Run;
  // What FUNCTION LOAD collects from redis.register_function.
  struct Loading;

  std::string load_locked(std::string_view body, std::string* sha, bool from_eval);
  void setup();
  void protect_globals();
  // Compiles and runs a library's code; "" or an error. On success the
  // library and its functions are registered.
  std::string compile_library(std::string_view code, bool replace, std::string* name);
  void drop_library(const std::string& name);
  // Reloads the libraries if another server changed them (a replica).
  void sync_libraries();
  void reload_libraries();
  uint64_t stored_version();
  void persist_library(const std::string& name, const std::string* code);
  void run(Cmd& c, int ref, bool as_function, std::span<const std::string_view> keys,
           std::span<const std::string_view> args, bool read_only, std::string_view what);

  static int lua_call_cmd(lua_State* L);
  static int lua_pcall_cmd(lua_State* L);
  int call_command(lua_State* L, bool protected_call);
  static int lua_register_function(lua_State* L);
  static int register_function(lua_State* L);  // -1: raise the error on the stack
  static ScriptEngine* of(lua_State* L);

  RocksStorage& storage_;
  std::mutex mu_;  // the Lua state and everything below
  lua_State* L_ = nullptr;
  int redis_ref_ = 0;       // the full redis table
  std::map<std::string, Script, std::less<>> scripts_;
  std::list<std::string> eval_lru_;  // evictable scripts, least recently used first
  std::map<std::string, Library, std::less<>> libraries_;
  std::map<std::string, Function, std::less<>> functions_;
  uint64_t loaded_version_ = 0;
  Run* run_ = nullptr;
  Loading* loading_ = nullptr;
  std::atomic<bool> running_{false};
  std::atomic<std::thread::id> runner_{};
  std::atomic<bool> wrote_{false};
  std::atomic<bool> kill_{false};
  std::atomic<int64_t> started_us_{0};  // steady clock
  std::mutex done_mu_;
  std::vector<std::shared_ptr<KeyWatch::Waiter>> done_waiters_;  // under done_mu_
  std::atomic<size_t> library_count_{0}, function_count_{0};
};

// SHA1 of data, lower-case hex.
std::string sha1_hex(std::string_view data);

}  // namespace datadam
