#include "nimbus/orch/hosted_runs.h"

#include <cstdlib>

#include "nimbus/orch/connectors_wire.h"   // isProviderBuiltinTool

namespace nimbus {
namespace orch {

namespace {
thread_local HostedRuns t_runs;

void bump(uint16_t& n, uint16_t by) { n = (uint16_t)(n > 0xFFFF - by ? 0xFFFF : n + by); }

// A provider built-in that is neither the web nor a connector of the owner's.
bool builtinTool(const std::string& name) {
  return isProviderBuiltinTool(name) || name == "file_search" || name == "local_shell";
}

// The count after the last " x" of one "[ran: ...]" item ("gmail x2" -> 2), 0 for none.
uint16_t itemRuns(const std::string& item, std::string* name) {
  const size_t x = item.rfind(" x");
  if (x == std::string::npos || x + 2 >= item.size()) return 0;
  const long n = std::strtol(item.c_str() + x + 2, nullptr, 10);
  *name = item.substr(0, x);
  return (uint16_t)(n < 0 ? 0 : (n > 0xFFFF ? 0xFFFF : n));
}

// The number right after `key` in `record` (" [tools: 3, web: 1]"), 0 when absent.
uint16_t countAfter(const std::string& record, const char* key) {
  const size_t at = record.find(key);
  if (at == std::string::npos) return 0;
  const long n = std::strtol(record.c_str() + at + std::char_traits<char>::length(key), nullptr, 10);
  return (uint16_t)(n < 0 ? 0 : (n > 0xFFFF ? 0xFFFF : n));
}
}  // namespace

void HostedRuns::add(const HostedRuns& o) {
  bump(total, o.total);
  bump(web, o.web);
  bump(connector, o.connector);
}

bool isWebToolName(const std::string& name) {
  for (const char* w : {"web_search", "web_fetch", "news_search"})   // web_search_preview too
    if (name.find(w) != std::string::npos) return true;
  return false;
}

void countRun(const std::string& toolName, uint16_t runs, HostedRuns& into) {
  if (runs == 0) return;
  bump(into.total, runs);
  if (isWebToolName(toolName)) bump(into.web, runs);
  else if (!builtinTool(toolName)) bump(into.connector, runs);
}

void noteHostedRun(const std::string& toolName) { countRun(toolName, 1, t_runs); }

HostedRuns hostedRuns() { return t_runs; }

void resetHostedRuns() { t_runs = HostedRuns(); }

HostedRuns runsInRecord(const std::string& record) {
  HostedRuns out;
  const size_t open = record.find(" [ran: ");
  const size_t close = open == std::string::npos ? open : record.find(']', open);
  if (close != std::string::npos) {
    const std::string list = record.substr(open + 7, close - open - 7);
    for (size_t at = 0; at <= list.size();) {
      size_t end = list.find(", ", at);
      if (end == std::string::npos) end = list.size();
      std::string name;
      const uint16_t n = itemRuns(list.substr(at, end - at), &name);
      if (name == "other tools") name.clear();   // names past the list's cap: a connector
      countRun(name, n, out);
      at = end + 2;
    }
    return out;
  }
  const uint16_t web = countAfter(record, ", web: ");
  const uint16_t tools = countAfter(record, " [tools: ");
  out.total = tools > web ? tools : web;
  out.web = web;
  out.connector = (uint16_t)(out.total - web);
  return out;
}

}  // namespace orch
}  // namespace nimbus
