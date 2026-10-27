#pragma once

// metrics-port: what datadam answers over HTTP, for monitoring tools that do
// not speak RESP. Everything is read when asked for: the hot path only bumps
// counters (ServerStats, Database), and this runs on the control thread.

#include <string>
#include <string_view>

#include "server/redis_commands.h"

namespace datadam {

// The Prometheus text exposition format (version 0.0.4), which Prometheus,
// Grafana Alloy, VictoriaMetrics, the OpenTelemetry Collector and Datadog's
// OpenMetrics check read:
//   - every number of INFO (default sections) as datadam_<field>, counters
//     as datadam_<field>_total;
//   - datadam_commands_total and friends with a cmd label (INFO commandstats),
//     datadam_errors_total with a code label (INFO errorstats);
//   - the INFO latency histograms as histograms, in seconds.
std::string render_prometheus(ServerContext& ctx);

struct HttpResponse {
  std::string status;  // "200 OK"
  std::string content_type;
  std::string body;
  bool head_only = false;  // HEAD: headers only
};

// Answers one HTTP request line ("GET /metrics HTTP/1.1"):
//   /metrics  the Prometheus text
//   /health   200 while the process serves (liveness)
//   /ready    200 when it can serve data, 503 while loading from its primary
HttpResponse metrics_http(ServerContext& ctx, std::string_view request_line);

}  // namespace datadam
