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
    std::string chat;     // delivery channel of an assistant reply ("owner" for web,
                          // a Telegram chat id otherwise); "" for untagged/user rows
    uint64_t    turnId;   // the web turn this assistant reply answers (its prompt-row
                          // seq); 0 for user rows and non-web deliveries (CUM-293)
  };

  explicit ReplyBuffer(size_t cap = 50) : cap_(cap ? cap : 1) {}

  // Append an entry, assign the next sequence number, evict past the cap.
  // Returns the assigned sequence. `chat` tags an assistant reply's delivery channel
  // so the web chat surface can match only its own turns' replies (CUM-218); `turnId`
  // tags WHICH web turn it answers so the match is by id, not ring position (CUM-293).
  // Thread-safe.
  uint64_t push(const std::string& role, const std::string& text,
                const std::string& chat = "", uint64_t turnId = 0) {
    std::lock_guard<std::mutex> lk(mu_);
    const uint64_t seq = ++lastSeq_;
    entries_.push_back(Entry{seq, (uint64_t)time(nullptr), role, text, chat, turnId});
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

  // Outcome of matching a web turn against the ring (CUM-293):
  //   Own   - this turn delivered at least one reply; outText is its LAST (final) one.
  //   Later - this turn delivered nothing, but a LATER web turn already has, which (by
  //           the strict-FIFO one-turn-at-a-time engine) proves this turn ran and
  //           produced nothing; its bubble resolves to an honest empty.
  //   None  - neither yet; this turn is still in flight.
  enum class TurnMatch { None, Own, Later };

  // Match web turn `turnId` on the `chat` channel in ONE locked pass over the ring.
  // Matching by turn id (not ring position) is what makes the web reply matcher robust
  // to a turn that delivers zero or several messages: no earlier turn's spillover and
  // no later turn's reply can ever satisfy a different turn's id. A single scan (vs two
  // separate locked queries) also closes the window where this turn's own reply could
  // land between two calls and be skipped in favour of a later turn's. turnId 0 never
  // matches as "own", so user rows and non-web deliveries are excluded.
  TurnMatch matchWebTurn(uint64_t turnId, const std::string& chat, std::string& outText) const {
    std::lock_guard<std::mutex> lk(mu_);
    bool own = false, later = false;
    for (const auto& e : entries_) {
      if (e.role != "assistant" || e.chat != chat) continue;
      if (turnId != 0 && e.turnId == turnId) { outText = e.text; own = true; }  // last wins
      else if (e.turnId > turnId) later = true;
    }
    return own ? TurnMatch::Own : (later ? TurnMatch::Later : TurnMatch::None);
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
