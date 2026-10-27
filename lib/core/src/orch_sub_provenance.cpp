#include "nimbus/orch/sub_provenance.h"

#include <cstring>
#include <vector>

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

namespace {
// A provider's tool name as printable ASCII without the list's own separators, so it can
// never close the bracket or start a new line in the head's input.
size_t cleanToolName(const std::string& name, char* out, size_t cap) {
  size_t k = 0;
  for (size_t i = 0; i < name.size() && k < cap; i++) {
    const unsigned char c = (unsigned char)name[i];
    if (c > 0x20 && c < 0x7F && c != ',' && c != '[' && c != ']') out[k++] = (char)c;
  }
  out[k] = '\0';
  return k;
}
}  // namespace

void ToolTally::bump(const std::string& name, uint16_t by) {
  char clean[kNameMax + 1];
  if (cleanToolName(name, clean, kNameMax) == 0) return;
  for (size_t i = 0; i < n_; i++) {
    if (std::strcmp(rows_[i].name, clean) != 0) continue;
    if (rows_[i].runs <= 0xFFFF - by) rows_[i].runs = (uint16_t)(rows_[i].runs + by);
    return;
  }
  if (n_ < kMaxNames) {
    std::memcpy(rows_[n_].name, clean, sizeof(clean));
    rows_[n_++].runs = by;
  } else if (otherRuns_ <= 0xFFFF - by) {
    otherRuns_ = (uint16_t)(otherRuns_ + by);
  }
}

std::string ToolTally::render(size_t cap) const {
  std::vector<std::string> items;   // the tools that ran first, then the idle ones
  for (int pass = 0; pass < 2; pass++)
    for (size_t i = 0; i < n_; i++)
      if ((rows_[i].runs > 0) == (pass == 0)) items.push_back(std::string(rows_[i].name) + " x" + std::to_string(rows_[i].runs));
  if (otherRuns_) items.push_back("other tools x" + std::to_string(otherRuns_));
  // The most whole items that fit, with "+N more" for the rest.
  for (size_t keep = items.size(); keep > 0; keep--) {
    std::string out;
    for (size_t i = 0; i < keep; i++) out += (i ? ", " : "") + items[i];
    if (keep < items.size()) out += ", +" + std::to_string(items.size() - keep) + " more";
    if (out.size() <= cap) return out;
  }
  return std::string();
}

std::string toolRunsTag(const char* names) {
  if (!names || !names[0]) return std::string();
  return std::string(" [ran: ") + names + "]";
}

}  // namespace orch
}  // namespace nimbus
