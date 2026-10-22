#include "nimbus/orch/pending_events.h"

#include <cstdio>
#include <cstdlib>

namespace nimbus::orch {

namespace {

std::string inlineText(const std::string& in, size_t cap) {
  std::string out;
  out.reserve(in.size() < cap ? in.size() : cap);
  for (unsigned char c : in) {
    if (out.size() >= cap) break;
    out.push_back(c < 0x20 || c == 0x7f ? ' ' : (char)c);
  }
  return out;
}

std::string evToken(const std::string& in) {
  std::string out;
  for (unsigned char c : in) {
    if (out.size() >= 16) break;
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '-' || c == '_';
    out.push_back(ok ? (char)c : '_');
  }
  return out.empty() ? std::string("misc") : out;
}

bool parseU32(const std::string& s, uint32_t& out) {
  if (s.empty() || s.size() > 10) return false;
  uint64_t v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    v = v * 10 + (uint64_t)(c - '0');
  }
  if (v > 0xFFFFFFFFull) return false;
  out = (uint32_t)v;
  return true;
}

constexpr char kDroppedHeader[] = "# dropped=";

}  // namespace

std::string encodePendingEvent(const PendingEvent& e) {
  return std::to_string(e.bootNo) + " " + std::to_string(e.upMs) + " " + evToken(e.ev) + " " +
         inlineText(e.text, kPendingEvMaxText) + "\n";
}

bool decodePendingEvent(const std::string& line, PendingEvent& out) {
  const size_t a = line.find(' ');
  if (a == std::string::npos) return false;
  const size_t b = line.find(' ', a + 1);
  if (b == std::string::npos) return false;
  const size_t c = line.find(' ', b + 1);
  if (c == std::string::npos) return false;
  PendingEvent e;
  if (!parseU32(line.substr(0, a), e.bootNo)) return false;
  if (!parseU32(line.substr(a + 1, b - a - 1), e.upMs)) return false;
  e.ev = line.substr(b + 1, c - b - 1);
  if (e.ev.empty() || evToken(e.ev) != e.ev) return false;
  std::string text = line.substr(c + 1);
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
  if (text.empty()) return false;
  e.text = inlineText(text, kPendingEvMaxText);
  out = e;
  return true;
}

PendingFile parsePendingFile(const std::string& content) {
  PendingFile f;
  size_t pos = 0;
  while (pos < content.size()) {
    size_t nl = content.find('\n', pos);
    if (nl == std::string::npos) nl = content.size();
    const std::string line = content.substr(pos, nl - pos);
    pos = nl + 1;
    if (line.compare(0, sizeof(kDroppedHeader) - 1, kDroppedHeader) == 0) {
      uint32_t n = 0;
      if (parseU32(line.substr(sizeof(kDroppedHeader) - 1), n)) f.dropped += n;
      continue;
    }
    PendingEvent e;
    if (decodePendingEvent(line, e)) f.rows.push_back(std::move(e));
  }
  return f;
}

std::string renderPendingFile(const PendingFile& f) {
  std::string out;
  if (f.dropped) out = std::string(kDroppedHeader) + std::to_string(f.dropped) + "\n";
  for (const auto& e : f.rows) out += encodePendingEvent(e);
  return out;
}

std::string appendPendingEvent(const std::string& content, const PendingEvent& e) {
  PendingFile f = parsePendingFile(content);
  f.rows.push_back(e);
  while (f.rows.size() > kPendingEvMaxRows) {
    f.rows.erase(f.rows.begin());
    ++f.dropped;
  }
  std::string out = renderPendingFile(f);
  while (out.size() > kPendingEvMaxBytes && f.rows.size() > 1) {
    f.rows.erase(f.rows.begin());
    ++f.dropped;
    out = renderPendingFile(f);
  }
  return out;
}

std::string stampedPendingText(const PendingEvent& e, bool thisBoot) {
  if (thisBoot) return e.text + " [pre-sync, stamped at first clock sync]";
  const std::string from = e.bootNo ? "boot n=" + std::to_string(e.bootNo) : std::string("an earlier boot");
  return e.text + " [from " + from + ", clock never synced; stamped at first clock sync]";
}

std::string droppedPendingText(uint32_t dropped) {
  if (!dropped) return {};
  return std::to_string(dropped) +
         " earlier device event(s) were dropped while the clock was not set";
}

std::vector<StampedRow> planPendingFlush(const PendingFile& f, size_t ownTail) {
  std::vector<StampedRow> out;
  if (f.dropped) out.push_back({"misc", droppedPendingText(f.dropped)});
  const size_t firstOwn = ownTail >= f.rows.size() ? 0 : f.rows.size() - ownTail;
  for (size_t i = 0; i < f.rows.size(); ++i)
    out.push_back({f.rows[i].ev, stampedPendingText(f.rows[i], i >= firstOwn)});
  return out;
}

}  // namespace nimbus::orch
