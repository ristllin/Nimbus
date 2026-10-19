#include "nimbus/orch/sub_provenance.h"

#include <cstring>

namespace nimbus {
namespace orch {

namespace {

// Single words that make a task live (matched as whole words, any case).
constexpr const char* kLiveWords[] = {
    "status",   "disruption", "disruptions", "delay",     "delays",  "today's", "todays",
    "current",  "currently",  "latest",      "live",      "weather", "forecast", "price",
    "prices",   "news",       "headline",    "headlines", "score",   "scores",   "traffic",
    "tube",     "train",      "trains",      "rail",      "bus",     "buses",    "departures",
    "arrivals", "stock",      "stocks",      "exchange",  "outage",  "outages",  "tonight",
};
// Two-word phrases (the words alone are too common).
constexpr const char* kLivePairs[] = {
    "right now", "open now", "this morning", "this afternoon", "this evening", "as of",
};

bool isWordByte(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '\'';
}

// The task lowercased, words separated by single spaces (bounded: tasks are short).
std::string normalize(const std::string& task) {
  std::string out;
  const size_t n = task.size() < 4096 ? task.size() : 4096;
  out.reserve(n + 2);
  out += ' ';
  bool space = true;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)task[i];
    if (isWordByte(c)) {
      out += (char)((c >= 'A' && c <= 'Z') ? (c | 0x20) : c);
      space = false;
    } else if (!space) {
      out += ' ';
      space = true;
    }
  }
  if (!space) out += ' ';
  return out;
}

bool hasWord(const std::string& norm, const char* w) {
  std::string needle = " ";
  needle += w;
  needle += ' ';
  return norm.find(needle) != std::string::npos;
}

}  // namespace

bool liveTask(const std::string& task) {
  const std::string norm = normalize(task);
  for (const char* w : kLiveWords)
    if (hasWord(norm, w)) return true;
  for (const char* p : kLivePairs)
    if (hasWord(norm, p)) return true;
  return false;
}

std::string liveDataNotCheckedNote() {
  return "[NOT CHECKED] No lookup ran for this task (0 tool calls). The text below is the "
         "model's own guess, not live data: do not present it as current, and do not store it.";
}

std::string freshProvenanceTag(uint16_t toolRuns, uint16_t webRuns) {
  if (toolRuns == 0 && webRuns == 0) return " [no tools ran]";
  std::string out = " [tools: " + std::to_string(toolRuns > webRuns ? toolRuns : webRuns);
  if (webRuns) out += ", web: " + std::to_string(webRuns);
  return out + "]";
}

}  // namespace orch
}  // namespace nimbus
