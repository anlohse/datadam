#include "server/metrics.h"

#include <cctype>
#include <format>
#include <set>
#include <vector>

#include "util/strings.h"

namespace datadam {

namespace {

// INFO fields that only grow (Prometheus counters); the others are gauges.
bool is_counter(std::string_view field) {
  static const std::set<std::string_view, std::less<>> counters = {
      "keyspace_hits", "keyspace_misses", "evicted_keys", "auth_failures", "acl_access_denied", "tls_connections",
      "tls_handshake_failures", "deferred_reads", "memory_reads", "read_pool_loads", "read_pool_batches",
      "read_pool_joined", "write_batches", "write_batch_ops", "write_batch_conflicts", "cache_hits", "cache_misses",
      "cache_rejected_too_large", "cache_rejected_admission", "cache_alloc_failures", "cache_page_steals",
      "rocksdb_write_stall_delays", "rocksdb_write_stall_stops"};
  return field.starts_with("total_") || field.starts_with("cache_evicted_") ||
         (field.starts_with("rocksdb_stall_") && !field.ends_with("_rate")) || counters.contains(field);
}

// Fields that are not measurements.
bool skipped(std::string_view field) {
  return field == "process_id" || field == "uptime_in_days" || field == "tcp_port" || field == "memcache_port";
}

std::string metric_name(std::string_view field) {
  std::string out = "datadam_";
  for (char c : field) out.push_back((std::isalnum(static_cast<unsigned char>(c)) || c == '_') ? c : '_');
  return out;
}

// A label value: backslash, double quote and newline escaped.
std::string label(std::string_view v) {
  std::string out;
  for (char c : v) {
    if (c == '\n') {
      out += "\\n";
      continue;
    }
    if (c == '\\' || c == '"') out.push_back('\\');
    out.push_back(c);
  }
  return out;
}

std::string format_number(double v) {
  if (v == static_cast<double>(static_cast<int64_t>(v))) return std::to_string(static_cast<int64_t>(v));
  return std::format("{}", v);
}

void family(std::string& out, std::string_view name, std::string_view type, std::string_view help) {
  out += std::format("# HELP {} {}\n# TYPE {} {}\n", name, help, name, type);
}

std::vector<std::string_view> lines_of(std::string_view text) {
  std::vector<std::string_view> out;
  for (size_t pos = 0; pos < text.size();) {
    size_t end = std::min(text.find('\n', pos), text.size());
    std::string_view line = text.substr(pos, end - pos);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (!line.empty()) out.push_back(line);
    pos = end + 1;
  }
  return out;
}

// datadam_info, then every number of the default INFO sections.
void info_fields(std::string& out, ServerContext& ctx) {
  std::string info = build_info(ctx, "default");
  std::vector<std::string_view> lines = lines_of(info);
  auto text = [&](std::string_view field) {
    for (std::string_view line : lines) {
      if (line.size() > field.size() && line.starts_with(field) && line[field.size()] == ':') {
        return line.substr(field.size() + 1);
      }
    }
    return std::string_view();
  };
  family(out, "datadam_info", "gauge", "Version and role (always 1)");
  out += std::format("datadam_info{{version=\"{}\",redis_version=\"{}\",role=\"{}\"}} 1\n",
                     label(text("datadam_version")), label(text("redis_version")),
                     ctx.db.read_only() ? "replica" : "master");

  std::set<std::string, std::less<>> seen;
  bool numbers = true;
  for (std::string_view line : lines) {
    if (line.starts_with('#')) {
      // These have metrics of their own, with labels.
      numbers = line != "# Latency" && line != "# Errorstats" && line != "# Keyspace";
      continue;
    }
    size_t colon = line.find(':');
    if (!numbers || colon == std::string_view::npos) continue;
    std::string_view field = line.substr(0, colon);
    auto v = parse_double(line.substr(colon + 1));
    if (!v || skipped(field)) continue;
    bool counter = is_counter(field);
    std::string name = counter ? metric_name(field.starts_with("total_") ? field.substr(6) : field) + "_total"
                               : metric_name(field);
    if (!seen.insert(name).second) continue;
    family(out, name, counter ? "counter" : "gauge", std::format("INFO {}", field));
    out += std::format("{} {}\n", name, format_number(*v));
  }
  family(out, "datadam_db_keys", "gauge", "Keys (an estimate, as INFO keyspace)");
  out += std::format("datadam_db_keys{{db=\"db0\"}} {}\n", ctx.db.approx_size());
}

void commands(std::string& out, ServerContext& ctx) {
  std::vector<CommandTotals> totals = command_totals(ctx.stats);
  struct Family {
    std::string_view name, help;
    double (*value)(const CommandTotals&);
  };
  static constexpr Family kFamilies[] = {
      {"datadam_commands_total", "Commands run (INFO commandstats calls)",
       [](const CommandTotals& c) { return static_cast<double>(c.calls); }},
      {"datadam_commands_duration_seconds_total", "Time spent running commands (INFO commandstats usec)",
       [](const CommandTotals& c) { return static_cast<double>(c.usec) / 1e6; }},
      {"datadam_commands_failed_total", "Commands that ran and replied an error",
       [](const CommandTotals& c) { return static_cast<double>(c.failed); }},
      {"datadam_commands_rejected_total", "Commands refused before running (arity, ACL, READONLY...)",
       [](const CommandTotals& c) { return static_cast<double>(c.rejected); }},
  };
  for (const Family& f : kFamilies) {
    family(out, f.name, "counter", f.help);
    for (const CommandTotals& c : totals) {
      out += std::format("{}{{cmd=\"{}\"}} {}\n", f.name, label(c.name), format_number(f.value(c)));
    }
  }
  family(out, "datadam_errors_total", "counter", "Error replies by code (INFO errorstats)");
  for (const auto& [code, n] : error_totals(ctx.stats)) {
    out += std::format("datadam_errors_total{{code=\"{}\"}} {}\n", label(code), n);
  }
}

// The INFO latency histograms, with a bound at every power of two
// microseconds rather than at each of their four buckets per power: 27
// series per histogram instead of 108.
void histograms(std::string& out, ServerContext& ctx) {
  for (const auto& [field, h] : latency_histograms(ctx)) {
    std::string_view base = field.ends_with("_usec") ? field.substr(0, field.size() - 5) : field;
    std::string name = metric_name(base) + "_seconds";
    family(out, name, "histogram", std::format("INFO latency {}", field));
    uint64_t cumulative = 0;
    h->for_each_bucket([&](uint64_t bound, uint64_t n) {
      cumulative += n;
      if (bound >= 3 && ((bound + 1) & bound) != 0) return;  // not 2^k - 1
      out += std::format("{}_bucket{{le=\"{}\"}} {}\n", name, format_number(static_cast<double>(bound) / 1e6),
                         cumulative);
    });
    out += std::format("{}_bucket{{le=\"+Inf\"}} {}\n{}_sum {}\n{}_count {}\n", name, cumulative, name,
                       format_number(static_cast<double>(h->sum()) / 1e6), name, cumulative);
  }
}

}  // namespace

std::string render_prometheus(ServerContext& ctx) {
  std::string out;
  info_fields(out, ctx);
  commands(out, ctx);
  histograms(out, ctx);
  return out;
}

HttpResponse metrics_http(ServerContext& ctx, std::string_view request_line) {
  const std::string plain = "text/plain; charset=utf-8";
  auto parts = split_ws(request_line);
  if (parts.size() != 3 || !parts[2].starts_with("HTTP/1.")) return {"400 Bad Request", plain, "bad request\n"};
  if (parts[0] != "GET" && parts[0] != "HEAD") return {"405 Method Not Allowed", plain, "GET or HEAD\n"};
  HttpResponse r{"200 OK", plain, "", parts[0] == "HEAD"};
  std::string_view path = parts[1].substr(0, parts[1].find('?'));
  if (path == "/metrics") {
    r.content_type = "text/plain; version=0.0.4; charset=utf-8";
    r.body = render_prometheus(ctx);
  } else if (path == "/health") {
    r.body = "OK\n";
  } else if (path == "/ready") {
    if (ctx.repl.syncing()) {
      r.status = "503 Service Unavailable";
      r.body = "LOADING\n";
    } else {
      r.body = "OK\n";
    }
  } else {
    r.status = "404 Not Found";
    r.body = "/metrics, /health or /ready\n";
  }
  return r;
}

}  // namespace datadam
