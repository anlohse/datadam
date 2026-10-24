// Lua scripting: EVAL / EVALSHA / EVAL_RO / EVALSHA_RO, SCRIPT, FUNCTION,
// FCALL / FCALL_RO (the engine is ScriptEngine, scripting.cpp).
//
// A script or function runs with the whole keyspace to itself
// (Database::ExecScope with `all`), so it is atomic whichever keys it
// touches, and its writes are committed together. Inside EXEC the block
// already has the keyspace (EXEC takes it for a block with a script), and the
// script runs in it.

#include <format>

#include "server/command.h"
#include "server/scripting.h"

namespace datadam {

namespace {

// KEYS and ARGV of `name numkeys key... arg...`; false (after replying) if numkeys is bad.
bool split_keys(Cmd& c, std::vector<std::string_view>* keys, std::vector<std::string_view>* args) {
  auto n = parse_int64(c.args[2]);
  if (!n) {
    c.r.error(kNotInteger);
    return false;
  }
  if (*n < 0) {
    c.r.error("ERR Number of keys can't be negative");
    return false;
  }
  if (static_cast<size_t>(*n) > c.args.size() - 3) {
    c.r.error("ERR Number of keys can't be greater than number of args");
    return false;
  }
  size_t nk = static_cast<size_t>(*n);
  keys->assign(c.args.begin() + 3, c.args.begin() + 3 + static_cast<std::ptrdiff_t>(nk));
  args->assign(c.args.begin() + 3 + static_cast<std::ptrdiff_t>(nk), c.args.end());
  return true;
}

// Runs f with the whole keyspace held by this thread. The script's commands
// name the keys they use (ScriptEngine::call_command), so the commit keeps
// only those locked.
template <class F>
void with_keyspace(Cmd& c, F&& f) {
  if (c.db().owns_all_keys()) return f();  // inside EXEC
  Database::ExecScope scope(c.db(), {}, true, true);
  f();
  scope.commit();
}

void eval(Cmd& c, bool read_only) {
  std::vector<std::string_view> keys, args;
  if (!split_keys(c, &keys, &args)) return;
  with_keyspace(c, [&] { c.ctx.scripts.eval_script(c, c.args[1], keys, args, read_only); });
}

void evalsha(Cmd& c, bool read_only) {
  std::vector<std::string_view> keys, args;
  if (!split_keys(c, &keys, &args)) return;
  with_keyspace(c, [&] { c.ctx.scripts.run_script(c, c.args[1], keys, args, read_only); });
}

void fcall(Cmd& c, bool read_only) {
  std::vector<std::string_view> keys, args;
  if (!split_keys(c, &keys, &args)) return;
  with_keyspace(c, [&] { c.ctx.scripts.run_function(c, c.args[1], keys, args, read_only); });
}

void cmd_eval(Cmd& c) { eval(c, false); }
void cmd_eval_ro(Cmd& c) { eval(c, true); }
void cmd_evalsha(Cmd& c) { evalsha(c, false); }
void cmd_evalsha_ro(Cmd& c) { evalsha(c, true); }
void cmd_fcall(Cmd& c) { fcall(c, false); }
void cmd_fcall_ro(Cmd& c) { fcall(c, true); }

// FLUSH [ASYNC | SYNC]: both flush at once.
bool flush_mode_ok(Cmd& c) {
  if (c.args.size() > 3 || (c.args.size() == 3 && !iequals(c.args[2], "async") && !iequals(c.args[2], "sync"))) {
    c.r.error(kSyntax);
    return false;
  }
  return true;
}

void cmd_script(Cmd& c) {
  std::string sub = to_lower(c.args[1]);
  ScriptEngine& e = c.ctx.scripts;
  if (sub == "load" && c.args.size() == 3) {
    std::string sha;
    std::string error = e.load_script(c.args[2], &sha);
    if (!error.empty()) return c.r.error(error);
    return c.r.bulk(sha);
  }
  if (sub == "exists" && c.args.size() >= 3) {
    c.r.array(c.args.size() - 2);
    for (size_t i = 2; i < c.args.size(); ++i) c.r.integer(e.has_script(c.args[i]) ? 1 : 0);
    return;
  }
  if (sub == "flush") {
    if (!flush_mode_ok(c)) return;
    e.flush_scripts();
    return c.r.ok();
  }
  if (sub == "kill" && c.args.size() == 2) {
    std::string error = e.kill();
    if (!error.empty()) return c.r.error(error);
    return c.r.ok();
  }
  if (sub == "help" && c.args.size() == 2) {
    static const char* lines[] = {
        "SCRIPT <subcommand> [<arg> [value] [opt] ...]. Subcommands are:",
        "EXISTS <sha1> [<sha1> ...]",
        "    Return information about the existence of the scripts in the script cache.",
        "FLUSH [ASYNC|SYNC]",
        "    Flush the Lua scripts cache.",
        "KILL",
        "    Kill the currently executing Lua script.",
        "LOAD <script>",
        "    Load a script into the scripts cache without executing it.",
    };
    c.r.array(std::size(lines));
    for (const char* l : lines) c.r.simple(l);
    return;
  }
  c.r.error(std::format("ERR unknown subcommand or wrong number of arguments for '{}'. Try SCRIPT HELP.", c.args[1]));
}

void function_list(Cmd& c) {
  std::string pattern;
  bool with_code = false;
  for (size_t i = 2; i < c.args.size(); ++i) {
    if (iequals(c.args[i], "withcode")) {
      with_code = true;
    } else if (iequals(c.args[i], "libraryname") && i + 1 < c.args.size()) {
      pattern = c.args[++i];
    } else {
      return c.r.error(std::format("ERR Unknown argument {}", c.args[i]));
    }
  }
  auto libs = c.ctx.scripts.libraries();
  std::erase_if(libs, [&](const auto& l) { return !pattern.empty() && !glob_match(pattern, l.name); });
  c.r.array(libs.size());
  for (const auto& lib : libs) {
    c.r.map(with_code ? 4 : 3);
    c.r.bulk("library_name");
    c.r.bulk(lib.name);
    c.r.bulk("engine");
    c.r.bulk("LUA");
    c.r.bulk("functions");
    c.r.array(lib.functions.size());
    for (const auto& f : lib.functions) {
      c.r.map(3);
      c.r.bulk("name");
      c.r.bulk(f.name);
      c.r.bulk("description");
      c.r.null();
      c.r.bulk("flags");
      c.r.set(f.flags.size());
      for (const auto& flag : f.flags) c.r.bulk(flag);
    }
    if (with_code) {
      c.r.bulk("library_code");
      c.r.bulk(lib.code);
    }
  }
}

void cmd_function(Cmd& c) {
  std::string sub = to_lower(c.args[1]);
  ScriptEngine& e = c.ctx.scripts;
  bool changes = sub == "load" || sub == "delete" || sub == "flush" || sub == "restore";
  if (changes && c.db().read_only()) return c.r.error("READONLY You can't write against a read only replica.");
  if (sub == "load" && (c.args.size() == 3 || c.args.size() == 4)) {
    bool replace = c.args.size() == 4;
    if (replace && !iequals(c.args[2], "replace")) return c.r.error(std::format("ERR Unknown option given: {}", c.args[2]));
    std::string name;
    std::string error = e.load_library(c.args.back(), replace, &name);
    if (!error.empty()) return c.r.error(error);
    return c.r.bulk(name);
  }
  if (sub == "delete" && c.args.size() == 3) {
    std::string error = e.delete_library(c.args[2]);
    if (!error.empty()) return c.r.error(error);
    return c.r.ok();
  }
  if (sub == "flush") {
    if (!flush_mode_ok(c)) return;
    e.flush_libraries();
    return c.r.ok();
  }
  if (sub == "list") return function_list(c);
  if (sub == "kill" && c.args.size() == 2) {
    std::string error = e.kill();
    if (!error.empty()) return c.r.error(error);
    return c.r.ok();
  }
  if (sub == "stats" && c.args.size() == 2) {
    c.r.map(2);
    c.r.bulk("running_script");
    if (int64_t ms = e.running_ms(); e.running()) {
      c.r.map(3);
      c.r.bulk("name");
      c.r.bulk("");
      c.r.bulk("command");
      c.r.array(0);
      c.r.bulk("duration_ms");
      c.r.integer(ms);
    } else {
      c.r.null();
    }
    c.r.bulk("engines");
    c.r.map(1);
    c.r.bulk("LUA");
    c.r.map(2);
    c.r.bulk("libraries_count");
    c.r.integer(static_cast<int64_t>(e.library_count()));
    c.r.bulk("functions_count");
    c.r.integer(static_cast<int64_t>(e.function_count()));
    return;
  }
  if (sub == "help" && c.args.size() == 2) {
    static const char* lines[] = {
        "FUNCTION <subcommand> [<arg> [value] [opt] ...]. Subcommands are:",
        "LOAD [REPLACE] <FUNCTION CODE>",
        "    Create a new library with the given library name and code.",
        "DELETE <LIBRARY NAME>",
        "    Delete the given library.",
        "LIST [LIBRARYNAME PATTERN] [WITHCODE]",
        "    Return general information on all the libraries.",
        "FLUSH [ASYNC|SYNC]",
        "    Delete all the libraries.",
        "KILL",
        "    Kill the current running function.",
        "STATS",
        "    Return information about the current function running.",
    };
    c.r.array(std::size(lines));
    for (const char* l : lines) c.r.simple(l);
    return;
  }
  if (sub == "dump" || sub == "restore") return c.r.error("ERR FUNCTION DUMP and RESTORE are not supported");
  c.r.error(std::format("ERR unknown subcommand or wrong number of arguments for '{}'. Try FUNCTION HELP.", c.args[1]));
}

}  // namespace

bool allowed_while_script_busy(std::string_view name, Args args) {
  if (name == "script" || name == "function") {
    return args.size() >= 2 && (iequals(args[1], "kill") || iequals(args[1], "stats"));
  }
  if (name == "shutdown") {
    for (size_t i = 1; i < args.size(); ++i) {
      if (iequals(args[i], "nosave")) return true;
    }
    return false;
  }
  return name == "auth" || name == "hello" || name == "quit" || name == "ping";
}

CommandList script_commands() {
  return {
      {"eval", {cmd_eval, -3}},       {"eval_ro", {cmd_eval_ro, -3}},   {"evalsha", {cmd_evalsha, -3}},
      {"evalsha_ro", {cmd_evalsha_ro, -3}}, {"fcall", {cmd_fcall, -3}}, {"fcall_ro", {cmd_fcall_ro, -3}},
      {"script", {cmd_script, -2}},   {"function", {cmd_function, -2}},
  };
}

}  // namespace datadam
