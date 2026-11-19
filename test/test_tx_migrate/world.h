#pragma once
// The migration's test world (lane RB-MG1a), shared by test_tx_migrate and test_tx_reentry.
//
// An invented old store on the battery's fault-injecting file fake (txb::MemFiles): day
// streams /mem/episodic/YYYY-MM-DD.jsonl in the old engine's row format, a /data/chatsum.txt
// FoldStore record, and a RowSource that reads them the way the binding's after-query does.
// The fixtures are built to the SHAPES of a real month-old store (counts, sizes, kinds,
// pointer layout); no content of a real store is in them.
//
// Shape figures used (a real month, counted, never copied): about 6,000 rows over 31 days in
// three sessions (97% one chat); per owner message about 1.15 tool rows, 1.2 log rows, 0.2
// photo rows, 0.17 raw response rows and 0.5 assistant replies; owner text median 12 B
// (30% are 100 to 400 B, longest 3.5 KB), assistant median 263 B (p90 833 B, longest 4.3 KB),
// tool rows 400 to 1,200 B, log rows about 94 B, photo descriptions about 74 B; rows a day
// from 3 to over 2,000 (one heavy day); times kept in hours.
#include <ArduinoJson.h>
#include <unity.h>

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <functional>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "../support/tx_battery/mem_files.h"
#include "nimbus/orch/compact.h"
#include "nimbus/orch/tx/digest.h"
#include "nimbus/orch/tx/log.h"
#include "nimbus/orch/tx/migrate.h"

namespace mg {

namespace tx = nimbus::orch::tx;

// Sat 17 Oct 2026 00:00:00 UTC.
constexpr uint64_t kMonthStart = 1792195200ULL;
constexpr uint64_t kHour = 3600;
constexpr uint64_t kDay = 86400;
const char* const kChat = "6012345678";   // the chat's session id (a Telegram chat id)
const char* const kChatKey = "6012345678";

std::string dayOf(uint64_t t) {
  // the civil date of t (UTC)
  int64_t z = (int64_t)(t / kDay) + 719468;
  const int64_t era = z / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  char b[16];
  std::snprintf(b, sizeof b, "%04d-%02u-%02u", (int)(yoe + era * 400 + (m <= 2)), m, d);
  return b;
}

// ---- the old store ------------------------------------------------------------------------

struct OldRow {
  uint64_t id;
  std::string session;
  uint64_t hours;
  std::string role, kind, text, tags;
  std::string path;     // the day stream it lives in
  size_t off, len;
};

class OldStore {
 public:
  explicit OldStore(txb::MemFiles& f) : files(f) {}
  txb::MemFiles& files;
  std::vector<OldRow> rows;     // every session, id order
  uint64_t nextId = 0x1000;

  // The old engine (or engine 2 with tags "e2") writes one row: straight into the map, so the
  // file fake's fault counters only count the migration's own writes.
  uint64_t add(const std::string& session, uint64_t t, const std::string& role, const std::string& kind,
               const std::string& text, const std::string& tags = "") {
    OldRow r{nextId++, session, t / kHour, role, kind, text, tags, "", 0, 0};
    r.path = "/mem/episodic/" + dayOf(t) + ".jsonl";
    JsonDocument doc;
    char id[16];
    std::snprintf(id, sizeof id, "m%08llx", (unsigned long long)r.id);
    doc["id"] = id;
    doc["session"] = session;
    doc["ts"] = r.hours;
    doc["role"] = role;
    doc["kind"] = kind;
    doc["channel"] = session == kChat ? "telegram" : "system";
    doc["text"] = text;
    doc["blob"] = kind == "image" ? "/sd/mem/blobs/x.jpg" : "";
    doc["tags"] = tags;
    std::string line;
    serializeJson(doc, line);
    line += '\n';
    std::string& f = files.files[r.path];
    r.off = f.size();
    r.len = line.size();
    f += line;
    rows.push_back(r);
    return r.id;
  }
  // The old engine's summary record (FoldStore's own encoder).
  void fold(const std::string& summary, uint64_t thru, uint64_t shift, uint64_t epoch) {
    struct IO : nimbus::orch::FoldStoreIO {
      txb::MemFiles* f;
      std::string load() override { return f->files["/data/chatsum.txt"]; }
      void save(const std::string& b) override { f->files["/data/chatsum.txt"] = b; }
    } io;
    io.f = &files;
    nimbus::orch::FoldStore fs;
    fs.begin(&io);
    char a[16], b[16];
    std::snprintf(a, sizeof a, "m%08llx", (unsigned long long)thru);
    std::snprintf(b, sizeof b, "m%08llx", (unsigned long long)shift);
    fs.applyFold(kChat, summary, (uint32_t)epoch, thru ? a : "");
    if (shift) fs.setSoftStart(kChat, b);
    fs.noteTurn(kChat);
  }
  // What the binding reads back from /data/chatsum.txt (FoldStore's decoder).
  tx::OldFold oldFold() {
    struct IO : nimbus::orch::FoldStoreIO {
      txb::MemFiles* f;
      std::string load() override { return f->files["/data/chatsum.txt"]; }
      void save(const std::string&) override { TEST_FAIL_MESSAGE("the old store was written"); }
    } io;
    io.f = &files;
    nimbus::orch::FoldStore fs;
    fs.begin(&io);
    const nimbus::orch::ChatFold c = fs.get(kChat);
    tx::OldFold o;
    o.summary = c.summary;
    o.thru = idNumber(c.thru);
    o.shift = idNumber(c.softStart);
    o.foldEpoch = c.lastFoldEpoch;
    return o;
  }
  static uint64_t idNumber(const std::string& id) {
    if (id.size() < 2 || id[0] != 'm') return 0;
    return std::strtoull(id.c_str() + 1, nullptr, 16);
  }
  // The files of the old store, for "never modified".
  std::map<std::string, std::string> snapshot() const {
    std::map<std::string, std::string> out;
    for (const auto& kv : files.files)
      if (kv.first.rfind("/mem/episodic/", 0) == 0 || kv.first.rfind("/data/", 0) == 0) out.insert(kv);
    return out;
  }
};

// The binding's row source: the chat's rows by id, read back from the day streams through
// TxFiles::readAt and decoded as the old row format.
class StoreSource : public tx::RowSource {
 public:
  StoreSource(OldStore& s, const std::string& session) : store_(s), session_(session) {}
  long refuseAt = -1;   // the read with this index answers Io
  long reads = 0;
  uint64_t rowsRead = 0;
  tx::Status read(uint64_t after, uint64_t upTo, size_t max, std::vector<tx::DayRow>& out, bool& end) override {
    out.clear();
    end = false;
    if (reads++ == refuseAt) return tx::Status::fail(tx::Err::Io, "test source");
    auto it = std::upper_bound(store_.rows.begin(), store_.rows.end(), after,
                               [](uint64_t v, const OldRow& r) { return v < r.id; });
    for (; it != store_.rows.end(); ++it) {
      if (upTo && it->id > upTo) break;
      if (it->session != session_) continue;
      if (out.size() == max) return tx::Status::success();
      tx::DayRow d;
      if (!decode(*it, d)) return tx::Status::fail(tx::Err::Io, "test source decode");
      out.push_back(std::move(d));
      ++rowsRead;
    }
    end = true;
    return tx::Status::success();
  }
  tx::Status newest(uint64_t& id) override {
    id = 0;
    for (const OldRow& r : store_.rows)
      if (r.session == session_) id = r.id;
    return tx::Status::success();
  }

 private:
  bool decode(const OldRow& r, tx::DayRow& d) {
    std::string line(r.len, '\0');
    size_t got = 0;
    if (!store_.files.readAt(r.path, r.off, &line[0], r.len, got) || got != r.len) return false;
    JsonDocument doc;
    if (deserializeJson(doc, line)) return false;
    const std::string id = doc["id"] | "";
    const std::string role = doc["role"] | "", kind = doc["kind"] | "", tags = doc["tags"] | "";
    d.id = OldStore::idNumber(id);
    d.t = (doc["ts"] | (uint64_t)0) * kHour;
    d.text = doc["text"] | "";
    d.photo = kind == "image";
    d.ours = ("," + tags + ",").find(",e2,") != std::string::npos;
    const bool ownerKind = kind == "message" || kind == "image" || kind == "audio" || kind == "transcript";
    if (role == "user" && ownerKind) d.role = tx::RowRole::Owner;
    else if (role == "assistant" && kind == "message") d.role = tx::RowRole::Assistant;
    else if (kind == "tool_output" || role == "tool") d.role = tx::RowRole::Tool;
    else d.role = tx::RowRole::Other;
    return true;
  }
  OldStore& store_;
  std::string session_;
};

// ---- invented text --------------------------------------------------------------------------

const char* const kWords[] = {"boat",   "garden", "lesson", "train",  "weekend", "paint",  "mooring", "plan",
                              "dinner", "walk",   "school", "letter", "repair",  "market", "river",   "piano",
                              "coffee", "bike",   "rain",   "window", "invoice", "trip",   "cousin",  "shelf"};

std::string words(std::mt19937& r, size_t bytes, bool figures = false) {
  std::string s;
  std::uniform_int_distribution<size_t> w(0, sizeof(kWords) / sizeof(kWords[0]) - 1);
  std::uniform_int_distribution<int> n(10, 9999);
  while (s.size() < bytes) {
    if (!s.empty()) s += ' ';
    if (figures && r() % 5 == 0) s += std::to_string(n(r));
    else s += kWords[w(r)];
  }
  s.resize(bytes);
  while (!s.empty() && s.back() == ' ') s.pop_back();
  return s.empty() ? std::string("ok") : s;
}

size_t ownerBytes(std::mt19937& r) {
  const int p = (int)(r() % 100);
  if (p < 70) return 3 + r() % 18;
  if (p < 99) return 100 + r() % 300;
  return 1000 + r() % 2500;
}
size_t assistantBytes(std::mt19937& r) {
  const int p = (int)(r() % 100);
  if (p < 60) return 100 + r() % 300;
  if (p < 95) return 400 + r() % 500;
  return 900 + r() % 3400;
}

// One exchange of the old engine: the owner's message (sometimes a photo first), its tool work
// and logs, sometimes a reply. Returns the owner row's id.
uint64_t exchange(OldStore& s, std::mt19937& r, uint64_t t, const std::string& session = kChat) {
  if (r() % 5 == 0) s.add(session, t, "user", "image", words(r, 50 + r() % 50));
  const uint64_t id = s.add(session, t, "user", "message", words(r, ownerBytes(r), true));
  const int tools = (int)(r() % 3);
  for (int i = 0; i < tools; ++i)
    s.add(session, t, "tool", "tool_output",
          "read notes/" + std::string(kWords[r() % 8]) + ".md: " + words(r, 400 + r() % 800, true));
  if (r() % 6 == 0) s.add(session, t, "assistant", "llm_response", words(r, 300 + r() % 600, true));
  s.add(session, t, "system", "log", "turn " + std::to_string(r() % 1000) + " " + words(r, 80));
  if (r() % 5 == 0) s.add(session, t, "system", "log", words(r, 90));
  if (r() % 2 == 0) s.add(session, t, "assistant", "message", words(r, assistantBytes(r), true));
  return id;
}

struct Month {
  uint64_t thru = 0, shift = 0;   // the old pointers
  uint64_t lastT = 0;
  std::string summary;
};

// About `rows` rows over `days` days (one heavy day), the old summary covering the first
// `thruDay` days and, when shiftDay > thruDay, the shift pointer at the end of shiftDay.
Month month(OldStore& s, uint32_t seed, size_t rows, int days, int thruDay, int shiftDay, size_t summaryBytes) {
  std::mt19937 r(seed);
  Month m;
  const size_t perDay = rows / (days + 4);
  for (int d = 0; d < days; ++d) {
    const size_t want = d == days / 3 ? perDay * 5 : perDay;
    const size_t start = s.rows.size();
    uint64_t t = kMonthStart + (uint64_t)d * kDay + 7 * kHour;
    while (s.rows.size() - start < want) {
      if (r() % 33 == 0) exchange(s, r, t, "system");
      exchange(s, r, t);
      if (r() % 3 == 0 && t < kMonthStart + (uint64_t)d * kDay + 22 * kHour) t += kHour;
    }
    m.lastT = t;
    if (d + 1 == thruDay) m.thru = s.rows.back().id;
    if (d + 1 == shiftDay) m.shift = s.rows.back().id;
  }
  if (summaryBytes) m.summary = "## Facts\n" + words(r, summaryBytes / 2) + "\n## Open\n" + words(r, summaryBytes / 2);
  if (!m.summary.empty() || m.thru)
    s.fold(m.summary, m.thru, shiftDay > thruDay ? m.shift : 0, kMonthStart + (uint64_t)thruDay * kDay);
  return m;
}

// ---- reading the new store --------------------------------------------------------------------

std::vector<tx::Item> stored(txb::MemFiles& f, const std::string& chatKey = kChatKey) {
  std::vector<tx::Item> out;
  std::string dir;
  (void)tx::txDir(chatKey, dir);
  std::set<uint64_t> seen;
  for (const std::string& seg : f.segments(dir)) {
    const std::string& s = f.files[seg];
    size_t at = 0;
    while (at < s.size()) {
      const size_t nl = s.find('\n', at);
      if (nl == std::string::npos) break;
      tx::Item it;
      if (tx::decodeLine(s.data() + at, nl + 1 - at, it).ok() && seen.insert(it.q).second) out.push_back(it);
      at = nl + 1;
    }
  }
  return out;
}

bool startsWith(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; }

// How many earlier messages a note names ("The 12 earlier messages ..."), 0 when none.
uint32_t namedIn(const std::string& note) {
  const size_t k = note.find(" earlier message");
  if (k == std::string::npos) return 0;
  size_t b = k;
  while (b > 0 && note[b - 1] >= '0' && note[b - 1] <= '9') --b;
  return (uint32_t)std::strtoul(note.substr(b, k - b).c_str(), nullptr, 10);
}
bool importNoteText(const tx::Item& it) {
  return it.kind == tx::ItemKind::DeviceNote &&
         (it.x.find("answered by the earlier engine") != std::string::npos || namedIn(it.x) > 0);
}

// The one invariant: every conversation row of the old store up to `hi` that engine 2 did not
// write is in the new store exactly once, in order: covered by digest 0 (or its omission line)
// when at or below the old pointer, else named by an import note or imported as a text item
// (u with m = its id, or a with its text). Items engine 2 stored itself ("E2 " text) are passed
// over. No q twice, q rising, every imported turn closed.
void expectAccounted(OldStore& s, txb::MemFiles& f, uint64_t oldThru, uint64_t hi, const char* when) {
  char msg[320];
  std::vector<tx::DayRow> expect;
  {
    StoreSource src(s, kChat);
    std::vector<tx::DayRow> page;
    bool end = false;
    uint64_t after = 0;
    do {
      TEST_ASSERT_TRUE(src.read(after, hi, 1000, page, end).ok());
      for (const tx::DayRow& r : page) {
        after = r.id;
        if (!r.ours && (r.role == tx::RowRole::Owner || r.role == tx::RowRole::Assistant) && !r.text.empty() &&
            r.id > oldThru)
          expect.push_back(r);
      }
    } while (!end && !page.empty());
  }
  const std::vector<tx::Item> items = stored(f);
  size_t next = 0;
  uint64_t prevQ = 0;
  std::set<uint64_t> ms;
  std::map<uint32_t, bool> open;
  uint32_t anchors = 0;
  for (const tx::Item& it : items) {
    std::snprintf(msg, sizeof msg, "%s: q%llu after q%llu", when, (unsigned long long)it.q, (unsigned long long)prevQ);
    TEST_ASSERT_TRUE_MESSAGE(it.q > prevQ, msg);
    prevQ = it.q;
    if (it.kind == tx::ItemKind::DeviceNote && it.x.find("was kept by the earlier engine") != std::string::npos) {
      ++anchors;
      continue;
    }
    if (importNoteText(it)) {
      const uint32_t n = namedIn(it.x);
      std::snprintf(msg, sizeof msg, "%s: note names %u rows, %u left", when, n, (unsigned)(expect.size() - next));
      TEST_ASSERT_TRUE_MESSAGE(next + n <= expect.size(), msg);
      next += n;
      continue;
    }
    if (it.kind == tx::ItemKind::User || it.kind == tx::ItemKind::Assistant) {
      if (startsWith(it.x, "E2 ")) continue;
      std::snprintf(msg, sizeof msg, "%s: item q%llu (%c) has no row left", when, (unsigned long long)it.q,
                    (char)it.kind);
      TEST_ASSERT_TRUE_MESSAGE(next < expect.size(), msg);
      const tx::DayRow& r = expect[next++];
      std::snprintf(msg, sizeof msg, "%s: q%llu stands for the wrong row (expected m%llx)", when,
                    (unsigned long long)it.q, (unsigned long long)r.id);
      const bool owner = r.role == tx::RowRole::Owner;
      TEST_ASSERT_TRUE_MESSAGE(owner == (it.kind == tx::ItemKind::User), msg);
      TEST_ASSERT_TRUE_MESSAGE(startsWith(r.text, it.x) && !it.x.empty(), msg);
      if (owner) {
        TEST_ASSERT_EQUAL_UINT64_MESSAGE(r.id, it.m, msg);
        TEST_ASSERT_TRUE_MESSAGE(ms.insert(it.m).second, "a row imported twice");
      }
      open[it.turn] = true;
    }
    if (it.kind == tx::ItemKind::TurnEnd) open[it.turn] = false;
  }
  std::snprintf(msg, sizeof msg, "%s: %u of %u rows accounted", when, (unsigned)next, (unsigned)expect.size());
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(expect.size(), next, msg);
  for (const auto& kv : open) {
    std::snprintf(msg, sizeof msg, "%s: imported turn %u is not closed", when, kv.first);
    TEST_ASSERT_FALSE_MESSAGE(kv.second, msg);
  }
  if (oldThru) TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, anchors, "one anchor for the rows under the old pointer");
}

}  // namespace mg
