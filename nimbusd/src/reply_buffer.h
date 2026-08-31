#pragma once
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>

// reply_buffer - a bounded, thread-safe ring of the most recent chat entries
// (owner prompts and assistant replies). The web chat page (served at "/") polls
// it read-only through GET /api/replies. Two producers write to it: the HTTP
// control surface records the owner's prompt when a web message is posted, and
// the engine's delivery hook records every reply the engine produces (the same
// hook that forwards to Telegram).
//
// Read discipline: entries are NEVER cleared on read. A poll only ever asks for
// entries newer than a sequence number, so multiple browser tabs can each poll
// independently and all see the same transcript. Memory stays bounded: at most
// `cap` entries are retained; the oldest are evicted past that.
namespace nimbusd {

class ReplyBuffer {
 public:
  struct Entry {
    uint64_t    seq;      // monotonic, starts at 1
    uint64_t    tsEpoch;  // wall-clock seconds when recorded
    std::string role;     // "user" or "assistant"
    std::string text;
  };

  explicit ReplyBuffer(size_t cap = 50) : cap_(cap ? cap : 1) {}

  // Append an entry, assign the next sequence number, evict past the cap.
  // Returns the assigned sequence. Thread-safe.
  uint64_t push(const std::string& role, const std::string& text) {
    std::lock_guard<std::mutex> lk(mu_);
    const uint64_t seq = ++lastSeq_;
    entries_.push_back(Entry{seq, (uint64_t)time(nullptr), role, text});
    while (entries_.size() > cap_) entries_.pop_front();
    return seq;
  }

  // The highest sequence assigned so far (0 = nothing recorded yet).
  uint64_t lastSeq() const {
    std::lock_guard<std::mutex> lk(mu_);
    return lastSeq_;
  }

  // Number of retained entries (for tests).
  size_t size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return entries_.size();
  }

  // The text of the newest assistant entry with seq > afterSeq, or "" if none.
  // A typed read under the mutex (the web chat surface uses it to fill the pending
  // bubble) - no re-parsing of the JSON wire form.
  std::string lastAssistantTextSince(uint64_t afterSeq) const {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it)
      if (it->seq > afterSeq && it->role == "assistant") return it->text;
    return std::string();
  }

  // A JSON array of the retained entries with seq > afterSeq, oldest first.
  std::string sinceJsonArray(uint64_t afterSeq) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::string j = "[";
    bool first = true;
    for (const auto& e : entries_) {
      if (e.seq <= afterSeq) continue;
      if (!first) j += ",";
      first = false;
      // role is a fixed literal ("user"/"assistant") today; encode it through the
      // same escaper as text anyway, so a future non-literal role can never break
      // out of the JSON string (defense in depth).
      j += "{\"seq\":" + std::to_string(e.seq) +
           ",\"ts\":" + std::to_string(e.tsEpoch) +
           ",\"role\":" + jsonString(e.role) +
           ",\"text\":" + jsonString(e.text) + "}";
    }
    j += "]";
    return j;
  }

  // Encode an arbitrary byte string as a JSON string literal (quotes included).
  // Escapes the JSON-reserved characters and all C0 control bytes; UTF-8 bytes
  // pass through unchanged (valid inside a JSON string).
  static std::string jsonString(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
      switch (c) {
        case '"':  o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n";  break;
        case '\r': o += "\\r";  break;
        case '\t': o += "\\t";  break;
        default:
          if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
          else o += (char)c;
      }
    }
    o += "\"";
    return o;
  }

 private:
  mutable std::mutex   mu_;
  std::deque<Entry>    entries_;
  uint64_t             lastSeq_ = 0;
  size_t               cap_;
};

}  // namespace nimbusd
