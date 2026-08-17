#include "nimbus/orch/result_store.h"

#include "nimbus/mem_cap.h"  // utf8CapLen

namespace nimbus {
namespace orch {

namespace {

// Data boundary (prism 2026-08-05, CRITICAL): the ring holds FULL tool outputs
// and sub-agent replies from every turn, so it is a cross-tenant read channel
// unless every read is scoped. Same rule the memory tools use: you see your own
// namespace; readAll (admin) sees everything. A device-internal spill (empty ns)
// is admin-only - never guest-readable.
bool visibleTo(const ResultStore::Entry& e, const Principal& who) {
  if (who.perms().readAll) return true;
  if (e.ns.empty()) return false;
  return who.valid() && e.ns == who.ns;
}

}  // namespace

std::string ResultStore::put(const char* kind, const std::string& name,
                             const std::string& fullText, uint32_t nowMs,
                             const std::string& jobTag, const std::string& ns) {
  Entry en;
  en.kind = kind ? kind : "tool";
  en.name = name;
  en.ns = ns;
  en.atMs = nowMs;
  // ⚠ jobTag is model/provider-influenced: strip the ':' that separates the
  // "sub:" prefix so a crafted tag cannot forge another entry's namespace-key.
  std::string safeTag = jobTag;
  for (char& c : safeTag)
    if (c == ':' || c == ' ' || c == '\n') c = '_';
  en.tag = (en.kind == "sub" && !safeTag.empty()) ? ("sub:" + safeTag)
                                                  : ("r" + std::to_string(++seq_));
  // Entry clip (UTF-8-safe). The full durable copy is the episodic row - this
  // ring serves the LIVE fetches, so a 64 KB view is plenty.
  if (fullText.size() > kEntryMax) {
    int keep = utf8CapLen(fullText.c_str(), (int)fullText.size(), (int)kEntryMax);
    en.text = fullText.substr(0, (size_t)keep);
  } else {
    en.text = fullText;
  }
  // A re-put under the same tag (a job re-delivering) replaces the old entry.
  for (size_t i = 0; i < e_.size(); i++) {
    if (e_[i].tag == en.tag) {
      bytes_ -= e_[i].text.size();
      e_.erase(e_.begin() + (long)i);
      break;
    }
  }
  bytes_ += en.text.size();
  e_.push_back(std::move(en));
  // Ring bounds: slots AND summed bytes - evict oldest first.
  while (e_.size() > (size_t)kSlots || bytes_ > kTotalMax) {
    bytes_ -= e_.front().text.size();
    e_.erase(e_.begin());
  }
  return e_.back().tag;
}

bool ResultStore::get(const std::string& tag, size_t offset, size_t maxBytes,
                      std::string& out, size_t& total, const Principal& who) const {
  for (const auto& en : e_) {
    if (en.tag != tag) continue;
    if (!visibleTo(en, who)) return false;   // unreadable == missing (no probing)
    total = en.text.size();
    if (offset >= total) {
      out.clear();
      return true;  // valid tag, empty window (offset past end)
    }
    // Align the window START to a character boundary: a caller-supplied offset
    // can land mid-codepoint, and a leading continuation byte is invalid UTF-8
    // (which 400s the next provider request).
    while (offset < total && ((unsigned char)en.text[offset] & 0xC0) == 0x80) ++offset;
    size_t want = maxBytes ? maxBytes : total;
    size_t end = offset + want;
    if (end > total) end = total;
    int keep = utf8CapLen(en.text.c_str() + offset, (int)(total - offset), (int)(end - offset));
    out = en.text.substr(offset, (size_t)keep);
    return true;
  }
  return false;
}

std::string ResultStore::list(const Principal& who) const {
  std::string o;
  for (const auto& en : e_) {
    if (!visibleTo(en, who)) continue;
    o += en.tag + " " + en.kind + " " + en.name + " " + std::to_string((unsigned)en.text.size()) +
         "B\n";
  }
  return o.empty() ? std::string("(no stored results)") : o;
}

void registerResultTools(ToolRegistry& reg, const ResultHandlers& h) {
  reg.add("results.get",
          "Fetch the full text of a stored result by tag - tool outputs that were "
          "truncated, or overflowed sub-agent results. Large results are paged: pass "
          "'offset' to continue from where the previous view ended.",
          [h](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal& who) -> ToolResult {
            if (!h.get) return ToolResult::fail("results not supported on this device");
            std::string tag = a["tag"].is<const char*>() ? a["tag"].as<const char*>() : "";
            if (tag.empty()) return ToolResult::fail("missing 'tag'");
            size_t offset = a["offset"].is<float>() ? (size_t)a["offset"].as<double>() : 0;
            // ⚠ The page must fit inside the loop's per-result clamp TOGETHER with
            // the header, or the loop clips the tail off the payload while the
            // header still claims the full window - the model then pages past
            // bytes it never saw (prism 2026-08-05, CRITICAL). Reserve the header.
            const size_t cap = h.viewCap ? h.viewCap() : 2048;
            const size_t kHeaderMax = 64;
            const size_t view = cap > kHeaderMax * 2 ? cap - kHeaderMax : cap / 2;
            std::string out;
            size_t total = 0;
            if (!h.get(tag, offset, view, out, total, who))
              return ToolResult::fail("'" + tag +
                                      "' is not in the recent-results ring - try "
                                      "memory.episodic with text '" + tag + "'");
            std::string head = "bytes " + std::to_string((unsigned)offset) + "-" +
                               std::to_string((unsigned)(offset + out.size())) + " of " +
                               std::to_string((unsigned)total) + "\n";
            return ToolResult::ok(head + out);
          },
          R"({"type":"object","properties":{"tag":{"type":"string"},)"
          R"("offset":{"type":"number"}},"required":["tag"]})");

  reg.add("results.list",
          "List the stored recent results (tag, kind, name, size) fetchable with results.get.",
          [h](ArduinoJson::JsonObjectConst, const nimbus::orch::Principal& who) -> ToolResult {
            if (!h.list) return ToolResult::fail("results not supported on this device");
            return ToolResult::ok(h.list(who));
          },
          R"({"type":"object","properties":{}})");
}

}  // namespace orch
}  // namespace nimbus
