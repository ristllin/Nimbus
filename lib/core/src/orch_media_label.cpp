#include "nimbus/orch/media_label.h"

#include "nimbus/mem_cap.h"

// The one inbound media label (see media_label.h). Pure string building; no I/O.
namespace nimbus {
namespace orch {

namespace {

const char* kindWord(MediaKind k) { return k == MediaKind::File ? "file" : "image"; }

bool startsWith(const std::string& s, const char* p) {
  for (size_t i = 0; p[i]; ++i)
    if (i >= s.size() || s[i] != p[i]) return false;
  return true;
}

// The description, cut on a character boundary when it is over the item budget.
std::string capped(const std::string& d) {
  if (d.size() <= kMediaTextMax) return d;
  const int keep = nimbus::utf8CapLen(d.data(), (int)d.size(), (int)kMediaTextMax);
  return d.substr(0, (size_t)keep) + kMediaCutMark;
}

}  // namespace

bool validRef(const std::string& ref) {
  if (ref.size() != 16) return false;
  for (char c : ref)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

std::string mediaTag(const MediaLabel& l) {
  std::string out = "[";
  out += kindWord(l.kind);
  if (l.k > 0 && l.n > 0) out += " " + std::to_string(l.k) + "/" + std::to_string(l.n);
  if (validRef(l.ref)) out += " ref=" + l.ref;
  if (!l.sentAt.empty()) out += ", sent " + l.sentAt;
  out += "]";
  return out;
}

std::string mediaSegment(const MediaLabel& l) {
  std::string out = mediaTag(l);
  switch (l.cls) {
    case MediaClass::Described:
      if (!l.detail.empty()) return out + " " + capped(l.detail);
      return out + " (couldn't be described)";
    case MediaClass::NotDescribed:
      return out + " (couldn't be described)";
    case MediaClass::FetchFailed:
      return out + " (could not be fetched" + (l.detail.empty() ? std::string() : ": " + l.detail) + ")";
    case MediaClass::Stored:
      return out + " " + (l.detail.empty() ? std::string("attachment") : l.detail);
    case MediaClass::Count:
      break;
  }
  return out;
}

bool lateItem(int64_t nowEpoch, int64_t date) {
  constexpr int64_t kClockSet = 1577836800LL;   // 2020-01-01
  return nowEpoch >= kClockSet && date > 0 && nowEpoch - date > kLateItemS;
}

bool mediaLabelHead(const std::string& text, MediaKind& kind) {
  // "[image" or "[file" followed by the end of the bracket, a space (k/N, ref) or a
  // comma (a sent time with no number, as the history renders it).
  for (MediaKind k : {MediaKind::Image, MediaKind::File}) {
    const std::string head = std::string("[") + kindWord(k);
    if (!startsWith(text, head.c_str()) || text.size() <= head.size()) continue;
    const char next = text[head.size()];
    if (next != ']' && next != ' ' && next != ',') continue;
    kind = k;
    return true;
  }
  return false;
}

}  // namespace orch
}  // namespace nimbus
