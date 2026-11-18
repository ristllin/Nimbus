// test_ledger - the food ledger store (PRD section 4, lane RB-L1a): rows with device ids, the
// write-ahead change log, recovery after a power cut at any step, totals computed by the
// device, the 04:00 ledger day and the exact warnings.
//
// The test of record is test_kill_at_every_step_recovers_old_or_new: every write of a set of
// scenarios is cut at every storage step it takes (each log append, torn at several lengths,
// each part write, rename and remove of every month swap, the log roll); after recovery the
// store holds the old rows or the new rows, never a mix, the totals equal the sum of the rows,
// and the same call again either says "already applied" with the first receipt (new) or
// applies it exactly once (old).
#include <unity.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "nimbus/orch/ledger.h"

namespace tx = nimbus::orch::tx;
using namespace nimbus::orch::ledger;

void setUp() {}
void tearDown() {}

namespace {

const size_t kTestRoll = 6 * 1024;   // the roll size the tests run with (the same code path)

// ---- storage with power cuts -------------------------------------------------------------
// TxFiles over a map. Every mutating call (append, writeWhole, rename, remove) is one step.
// A cut at step k lands the first `tornBytes` bytes of an append or whole write (rename and
// remove are atomic: the cut comes before them), then the storage is dead: every later call
// answers false and changes nothing, as after a power cut. The test then opens a new store
// over the same files, which is the reboot.
struct Step {
  char kind;   // 'a'ppend, 'w'hole write, 'r'ename, 'd'elete
  std::string path;
  size_t len;
};

class CutFiles : public tx::TxFiles {
 public:
  std::map<std::string, std::string> files;
  long cutAt = -1;
  size_t tornBytes = 0;
  bool dead = false;
  bool refuseAppend = false;   // the card answers false without writing
  long steps = 0;
  std::vector<Step> trace;

  bool cut(char kind, const std::string& p, size_t len) {
    if (dead) return true;
    trace.push_back(Step{kind, p, len});
    if (steps++ == cutAt) {
      dead = true;
      return true;
    }
    return false;
  }
  bool append(const std::string& p, const char* d, size_t n) override {
    if (dead) return false;
    if (refuseAppend) return false;
    if (cut('a', p, n)) {
      files[p].append(d, std::min(n, tornBytes));
      return false;
    }
    files[p].append(d, n);
    return true;
  }
  bool readAt(const std::string& p, size_t off, char* buf, size_t want, size_t& got) override {
    got = 0;
    if (dead) return false;
    auto it = files.find(p);
    if (it == files.end()) return false;
    if (off >= it->second.size()) return true;
    got = std::min(want, it->second.size() - off);
    it->second.copy(buf, got, off);
    return true;
  }
  bool size(const std::string& p, size_t& out) override {
    if (dead) return false;
    auto it = files.find(p);
    if (it == files.end()) return false;
    out = it->second.size();
    return true;
  }
  bool writeWhole(const std::string& p, const std::string& d) override {
    if (dead) return false;
    if (cut('w', p, d.size())) {
      files[p] = d.substr(0, std::min(d.size(), tornBytes));
      return false;
    }
    files[p] = d;
    return true;
  }
  bool rename(const std::string& from, const std::string& to) override {
    if (dead) return false;
    if (cut('r', from, 0)) return false;
    auto it = files.find(from);
    if (it == files.end() || files.count(to)) return false;   // FAT: no rename over a file
    files[to] = it->second;
    files.erase(it);
    return true;
  }
  bool remove(const std::string& p) override {
    if (dead) return false;
    if (cut('d', p, 0)) return false;
    return files.erase(p) > 0;
  }
  bool list(const std::string& dir, std::vector<std::string>& names) override {
    names.clear();
    if (dead) return false;
    const std::string pre = dir + "/";
    for (const auto& kv : files)
      if (kv.first.rfind(pre, 0) == 0 && kv.first.find('/', pre.size()) == std::string::npos)
        names.push_back(kv.first.substr(pre.size()));
    return true;
  }
  void reboot() {
    dead = false;
    cutAt = -1;
    steps = 0;
    trace.clear();
  }
};

// ---- helpers -------------------------------------------------------------------------------
int64_t epochOf(const char* day, const char* hhmm, int32_t offset) {
  struct tm t = {};
  sscanf(day, "%d-%d-%d", &t.tm_year, &t.tm_mon, &t.tm_mday);
  sscanf(hhmm, "%d:%d", &t.tm_hour, &t.tm_min);
  t.tm_year -= 1900;
  t.tm_mon -= 1;
  return (int64_t)timegm(&t) - offset;
}

Clock at(const char* day, const char* hhmm, int32_t offset = 0) {
  Clock c;
  c.epoch = epochOf(day, hhmm, offset);
  c.utcOffsetSec = offset;
  c.set = true;
  return c;
}

Fixed fx(const char* text, int dp) {
  Fixed f;
  TEST_ASSERT_TRUE_MESSAGE(parseFixed(text, dp, f), text);
  return f;
}

Row food(const char* item, const char* kcal, const char* protein = nullptr) {
  Row r;
  r.item = item;
  r.kcal = fx(kcal, kKcalDp);
  if (protein) r.protein = fx(protein, kGramDp);
  r.basis = Basis::Estimate;
  return r;
}

Op add(const Row& r, const char* day = nullptr, const char* time = nullptr) {
  Op o;
  o.kind = OpKind::Add;
  o.row = r;
  if (day) o.row.day = day;
  if (time) o.row.time = time;
  return o;
}

Op removeOp(const std::string& id) {
  Op o;
  o.kind = OpKind::Remove;
  o.ids = {id};
  return o;
}

Op moveOp(const std::string& id, const char* day) {
  Op o;
  o.kind = OpKind::Move;
  o.ids = {id};
  o.day = day;
  return o;
}

Op updateKcal(const std::string& id, const char* kcal) {
  Op o;
  o.kind = OpKind::Update;
  o.ids = {id};
  o.setMask = kFKcal;
  o.set.kcal = fx(kcal, kKcalDp);
  return o;
}

bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

std::vector<std::string> snapshot(CutFiles& fs, const char* person = "owner") {
  LedgerStore s(fs, person, kTestRoll);
  std::vector<Row> rows;
  std::string err;
  TEST_ASSERT_TRUE_MESSAGE(s.allRows(rows, &err), err.c_str());
  std::vector<std::string> out;
  for (const Row& r : rows) out.push_back(encodeRow(r));
  return out;
}

// Totals of every day equal the sum of that day's rows, read back through day().
void assertTotalsAreSums(CutFiles& fs) {
  LedgerStore s(fs, "owner", kTestRoll);
  std::vector<Row> rows;
  TEST_ASSERT_TRUE(s.allRows(rows));
  std::map<std::string, std::pair<int64_t, int64_t>> sums;
  std::map<std::string, size_t> counts;
  for (const Row& r : rows) {
    sums[r.day].first += r.kcal.v;
    sums[r.day].second += r.protein.set ? r.protein.v : 0;
    ++counts[r.day];
  }
  for (const auto& kv : sums) {
    DayView v;
    TEST_ASSERT_TRUE(s.day(kv.first, v));
    TEST_ASSERT_EQUAL_INT64(kv.second.first, v.totals.kcal);
    TEST_ASSERT_EQUAL_INT64(kv.second.second, v.totals.protein);
    TEST_ASSERT_EQUAL_UINT32(counts[kv.first], v.totals.rows);
    TEST_ASSERT_EQUAL_UINT32(counts[kv.first], v.rows.size());
  }
}

std::string firstId(const WriteResult& w) {
  TEST_ASSERT_FALSE(w.changed.empty());
  return w.changed[0].id;
}

}  // namespace

// ---- ids, figures, the CSV -------------------------------------------------------------------
void test_add_returns_id() {
  CutFiles fs;
  LedgerStore s(fs, "owner", kTestRoll);
  WriteResult w = s.write({add(food("oat porridge", "312", "10.5"), "2026-11-17", "08:10")}, "c1",
                          at("2026-11-17", "08:15"), nullptr);
  TEST_ASSERT_EQUAL_MESSAGE((int)WriteStatus::Ok, (int)w.status, w.text.c_str());
  const std::string id = firstId(w);
  TEST_ASSERT_EQUAL(6, id.size());
  TEST_ASSERT_EQUAL('L', id[0]);
  TEST_ASSERT_TRUE_MESSAGE(has(w.text, "added " + id + " | 312 kcal, P 10.5 g"), w.text.c_str());
  TEST_ASSERT_TRUE_MESSAGE(
      has(w.text, "day 2026-11-17 (ledger day, 04:00 to 04:00): 1 row, 312 kcal, P 10.5 g; no target set"),
      w.text.c_str());
  TEST_ASSERT_TRUE(fs.files.count("/mem/ledger/owner/2026-11.csv"));
  TEST_ASSERT_TRUE(fs.files.count("/mem/ledger/owner/changes.log"));
}

void test_ids_are_unique_and_neighbours_look_unlike() {
  std::set<std::string> seen;
  size_t oneCharApart = 0;
  for (uint64_t seq = 1; seq <= 4000; ++seq)
    for (unsigned i = 0; i < 16; ++i) {
      const std::string id = rowId(seq, i);
      TEST_ASSERT_EQUAL(6, id.size());
      TEST_ASSERT_TRUE_MESSAGE(seen.insert(id).second, id.c_str());
      if (i) {
        const std::string prev = rowId(seq, i - 1);
        int diff = 0;
        for (size_t k = 1; k < 6; ++k) diff += prev[k] != id[k];
        if (diff < 2) ++oneCharApart;
      }
      for (char ch : id.substr(1)) TEST_ASSERT_TRUE(strchr("0123456789ABCDEFGHJKMNPQRSTVWXYZ", ch) != nullptr);
    }
  TEST_ASSERT_EQUAL_MESSAGE(0, oneCharApart, "two ids of one write differ in a single character");
  // The full space is a bijection: the top sequence still gives distinct ids.
  TEST_ASSERT_TRUE(rowId(kMaxSeq, 15) != rowId(kMaxSeq, 14));
  std::string lower = rowId(7, 3);
  for (char& ch : lower) ch = (char)tolower(ch);
  TEST_ASSERT_EQUAL_STRING(rowId(7, 3).c_str(), normalizeId(" " + lower + " ").c_str());
}

void test_fixed_point_parse_and_format_are_exact() {
  Fixed f;
  TEST_ASSERT_TRUE(parseFixed("118.3", 2, f));
  TEST_ASSERT_EQUAL_INT64(11830, f.v);
  TEST_ASSERT_TRUE(parseFixed("0.25", 1, f));
  TEST_ASSERT_EQUAL_INT64(3, f.v);   // half away from zero
  TEST_ASSERT_TRUE(parseFixed("-0.25", 1, f));
  TEST_ASSERT_EQUAL_INT64(-3, f.v);
  TEST_ASSERT_TRUE(parseFixed("1612", 1, f));
  TEST_ASSERT_EQUAL_STRING("1,612", formatFixed(f, 1).c_str());
  TEST_ASSERT_TRUE(parseFixed("2300.50", 1, f));
  TEST_ASSERT_EQUAL_STRING("2,300.5", formatFixed(f, 1).c_str());
  TEST_ASSERT_EQUAL_STRING("2300.5", formatFixed(f, 1, false).c_str());
  TEST_ASSERT_TRUE(parseFixed(".5", 2, f));
  TEST_ASSERT_EQUAL_STRING("0.5", formatFixed(f, 2).c_str());
  for (const char* bad : {"", "-", ".", "1e3", "1.2.3", "12a", "1,000", "1234567890123"})
    TEST_ASSERT_FALSE_MESSAGE(parseFixed(bad, 1, f), bad);
  TEST_ASSERT_TRUE(fixedFromDouble(0.15, 1, f));   // 0.15 is 0.1499999... as a double
  TEST_ASSERT_EQUAL_INT64(2, f.v);
  TEST_ASSERT_TRUE(fixedFromDouble(118.25, 2, f));
  TEST_ASSERT_EQUAL_INT64(11825, f.v);
  TEST_ASSERT_FALSE(fixedFromDouble(1e300, 1, f));
  // Every value prints back to itself.
  for (int64_t v = -2500; v <= 2500; v += 7) {
    Fixed a = Fixed::of(v), b;
    TEST_ASSERT_TRUE(parseFixed(formatFixed(a, 2, false), 2, b));
    TEST_ASSERT_EQUAL_INT64(v, b.v);
  }
}

void test_csv_round_trip_keeps_every_field() {
  std::mt19937 rng(7);
  const std::string alphabet = "ab ,\"'xyz;|\xC3\xA4\xC3\x84-0.9";
  for (int n = 0; n < 500; ++n) {
    Row r;
    r.id = rowId(n + 1, n % 16);
    r.day = "2026-11-17";
    r.time = n % 3 ? "08:10" : "";
    auto text = [&](size_t len) {
      std::string s;
      for (size_t i = 0; i < len; ++i) s += alphabet[rng() % alphabet.size()];
      return s;
    };
    r.meal = text(rng() % 6);
    r.item = "x" + text(rng() % 30);
    r.unit = text(rng() % 3);
    r.note = text(rng() % 40);
    r.src = text(rng() % 5);
    r.call = "call-" + std::to_string(n);
    r.kcal = Fixed::of((int64_t)(rng() % 100000));
    if (n % 2) r.protein = Fixed::of((int64_t)(rng() % 100000));
    if (n % 3) r.qty = Fixed::of((int64_t)(rng() % 100000));
    r.basis = (Basis)(rng() % 5);
    Row back;
    TEST_ASSERT_TRUE_MESSAGE(decodeRow(encodeRow(r), back), encodeRow(r).c_str());
    TEST_ASSERT_TRUE_MESSAGE(back == r, encodeRow(r).c_str());
  }
  Row bad;
  TEST_ASSERT_FALSE(decodeRow("L1,2026-11-17", bad));
  TEST_ASSERT_FALSE(decodeRow("\"unclosed,2026", bad));
}

// ---- the property: totals are the sum of the rows after any sequence ------------------------
namespace {

// The rows a random sequence must leave: id -> (day, kcal).
using Model = std::map<std::string, std::pair<std::string, int64_t>>;
const char* kRandomDays[] = {"2026-10-31", "2026-11-01", "2026-11-17", "2026-11-30", "2026-12-01", "2027-01-01"};

Op randomOp(std::mt19937& rng, const std::string& id) {
  if (id.empty()) {
    Row r = food(("item " + std::to_string(rng() % 9)).c_str(), std::to_string(rng() % 900).c_str(),
                 std::to_string(rng() % 60).c_str());
    return add(r, kRandomDays[rng() % 6], "12:00");
  }
  switch (rng() % 3) {
    case 0: return updateKcal(id, std::to_string(rng() % 900).c_str());
    case 1: return moveOp(id, kRandomDays[rng() % 6]);
    default: return removeOp(id);
  }
}

// 1 to 4 ops; an existing row is named at most once per write.
std::vector<Op> randomOps(std::mt19937& rng, const Model& model) {
  std::vector<Op> ops;
  std::set<std::string> used;
  const size_t n = 1 + rng() % 4;
  for (size_t k = 0; k < n; ++k) {
    std::string id;
    if (!model.empty() && rng() % 4) {
      auto it = model.begin();
      std::advance(it, rng() % model.size());
      id = it->first;
      if (!used.insert(id).second) continue;
    }
    ops.push_back(randomOp(rng, id));
  }
  return ops;
}

void applyToModel(Model& model, const std::vector<Op>& ops, const WriteResult& w) {
  for (size_t c = 0; c < ops.size(); ++c) {
    const Op& o = ops[c];
    if (o.kind == OpKind::Add) model[w.changed[c].id] = {w.changed[c].day, w.changed[c].kcal.v};
    else if (o.kind == OpKind::Remove) model.erase(o.ids[0]);
    else if (o.kind == OpKind::Move) model[o.ids[0]].first = o.day;
    else model[o.ids[0]].second = o.set.kcal.v;
  }
}

}  // namespace

void test_totals_equal_sum_after_random_ops() {
  for (unsigned seed = 1; seed <= 6; ++seed) {
    std::mt19937 rng(seed);
    CutFiles fs;
    LedgerStore s(fs, "owner", kTestRoll);
    Model model;
    for (int step = 0; step < 220; ++step) {
      const std::vector<Op> ops = randomOps(rng, model);
      if (ops.empty()) continue;
      const std::string call = "s" + std::to_string(seed) + "-" + std::to_string(step);
      WriteResult w = s.write(ops, call, at("2026-11-17", "12:00"), nullptr);
      TEST_ASSERT_EQUAL_MESSAGE((int)WriteStatus::Ok, (int)w.status, w.text.c_str());
      applyToModel(model, ops, w);
      if (step % 37 == 0) s.open();   // a reboot now and then
    }
    std::vector<Row> rows;
    TEST_ASSERT_TRUE(s.allRows(rows));
    TEST_ASSERT_EQUAL(model.size(), rows.size());
    for (const Row& r : rows) {
      TEST_ASSERT_TRUE(model.count(r.id));
      TEST_ASSERT_EQUAL_STRING(model[r.id].first.c_str(), r.day.c_str());
      TEST_ASSERT_EQUAL_INT64(model[r.id].second, r.kcal.v);
    }
    assertTotalsAreSums(fs);
  }
}

// ---- crash safety -------------------------------------------------------------------------
namespace {

struct Scenario {
  const char* name;
  std::function<void(LedgerStore&, CutFiles&, std::vector<std::string>& ids)> before;   // the old state
  std::function<std::vector<Op>(const std::vector<std::string>& ids)> ops;  // the write under test
};

std::vector<Scenario> scenarios() {
  auto seed = [](LedgerStore& s, CutFiles&, std::vector<std::string>& ids) {
    WriteResult w = s.write({add(food("toast", "180", "6"), "2026-11-30", "08:00"),
                             add(food("apple", "80"), "2026-11-30", "10:00"),
                             add(food("soup", "320", "14"), "2026-12-01", "12:30")},
                            "seed", at("2026-12-01", "13:00"), nullptr);
    TEST_ASSERT_EQUAL((int)WriteStatus::Ok, (int)w.status);
    for (const Row& r : w.changed) ids.push_back(r.id);
  };
  std::vector<Scenario> out;
  out.push_back({"first write into an empty store", [](LedgerStore&, CutFiles&, std::vector<std::string>&) {},
                 [](const std::vector<std::string>&) {
                   return std::vector<Op>{add(food("tea", "2"), "2026-11-17", "07:00")};
                 }});
  out.push_back({"add to an existing month", seed, [](const std::vector<std::string>&) {
                   return std::vector<Op>{add(food("cheese", "110", "7"), "2026-11-30", "15:00"),
                                          add(food("nuts", "190", "6"), "2026-11-30", "16:00")};
                 }});
  out.push_back({"move across a month boundary (two swaps)", seed, [](const std::vector<std::string>& ids) {
                   return std::vector<Op>{moveOp(ids[0], "2026-12-01")};
                 }});
  out.push_back({"update, remove and add in two months", seed, [](const std::vector<std::string>& ids) {
                   return std::vector<Op>{updateKcal(ids[1], "95"), removeOp(ids[2]),
                                          add(food("rice", "260", "5"), "2026-12-01", "19:00")};
                 }});
  out.push_back({"write that rolls the log", [](LedgerStore& s, CutFiles& fs, std::vector<std::string>& ids) {
                   Row big = food("filler", "1");
                   big.note = std::string(380, 'n');
                   // Fill the log until the write under test is the one that passes the roll size.
                   for (int k = 0; fs.files["/mem/ledger/owner/changes.log"].size() + 1200 < kTestRoll; ++k) {
                     WriteResult w = s.write({add(big, "2026-11-02", "09:00")}, "fill" + std::to_string(k),
                                             at("2026-11-02", "09:00"), nullptr);
                     TEST_ASSERT_EQUAL((int)WriteStatus::Ok, (int)w.status);
                     if (k == 0) ids.push_back(firstId(w));
                   }
                 },
                 [](const std::vector<std::string>& ids) {
                   Row big = food("filler", "1");
                   big.note = std::string(380, 'm');
                   return std::vector<Op>{add(big, "2026-11-03", "09:00"), removeOp(ids[0])};
                 }});
  out.push_back({"a month whose early rows are older than both logs", [](LedgerStore& s, CutFiles& fs,
                                                                         std::vector<std::string>& ids) {
                   WriteResult first = s.write({add(food("early", "111", "1"), "2026-11-01", "07:00")}, "early",
                                               at("2026-11-01", "07:00"), nullptr);
                   ids.push_back(firstId(first));
                   Row big = food("filler", "1");
                   big.note = std::string(380, 'n');
                   // Roll until the first record is in neither log: only the month file holds it.
                   for (int k = 0; has(fs.files["/mem/ledger/owner/changes.old.log"] + fs.files["/mem/ledger/owner/changes.log"],
                                       "early") ||
                                   !fs.files.count("/mem/ledger/owner/changes.old.log");
                        ++k) {
                     WriteResult w = s.write({add(big, "2026-10-02", "09:00")}, "roll" + std::to_string(k),
                                             at("2026-11-02", "09:00"), nullptr);
                     TEST_ASSERT_EQUAL((int)WriteStatus::Ok, (int)w.status);
                     TEST_ASSERT_TRUE(k < 100);
                   }
                 },
                 [](const std::vector<std::string>&) {
                   return std::vector<Op>{add(food("late", "222", "2"), "2026-11-01", "21:00")};
                 }});
  return out;
}

enum class Outcome { Old, New, Mixed };

Outcome classify(const std::vector<std::string>& got, const std::vector<std::string>& oldRows,
                 const std::vector<std::string>& newRows) {
  if (got == oldRows) return Outcome::Old;
  if (got == newRows) return Outcome::New;
  return Outcome::Mixed;
}

// Run the scenario's write once without a cut: its new rows, receipt and storage steps.
struct Baseline {
  CutFiles before;
  std::vector<std::string> oldRows, newRows, ids;
  std::string receipt;
  std::vector<Step> steps;
};

void baseline(const Scenario& sc, Baseline& b) {
  LedgerStore s(b.before, "owner", kTestRoll);
  sc.before(s, b.before, b.ids);
  b.oldRows = snapshot(b.before);
  CutFiles after = b.before;
  after.reboot();
  LedgerStore s2(after, "owner", kTestRoll);
  TEST_ASSERT_TRUE(s2.open());
  after.trace.clear();
  after.steps = 0;
  WriteResult w = s2.write(sc.ops(b.ids), "under-test", at("2026-12-01", "20:00"), nullptr);
  TEST_ASSERT_EQUAL_MESSAGE((int)WriteStatus::Ok, (int)w.status, w.text.c_str());
  b.receipt = w.text;
  b.steps = after.trace;
  b.newRows = snapshot(after);
  TEST_ASSERT_TRUE_MESSAGE(b.oldRows != b.newRows, sc.name);
}

// The torn lengths tried for a write of `len` bytes.
std::vector<size_t> tornLengths(const Step& st) {
  if (st.kind != 'a' && st.kind != 'w') return {0};
  std::set<size_t> l = {0, 1, st.len / 2, st.len > 1 ? st.len - 2 : 0, st.len > 0 ? st.len - 1 : 0, st.len};
  return std::vector<size_t>(l.begin(), l.end());
}

// After a cut: reboot, recover, check old-or-new, then send the same call again.
Outcome recoverAndRetry(const Scenario& sc, const Baseline& b, CutFiles& fs, const std::string& where) {
  fs.reboot();
  const std::vector<std::string> got = snapshot(fs);
  const Outcome o = classify(got, b.oldRows, b.newRows);
  TEST_ASSERT_TRUE_MESSAGE(o != Outcome::Mixed, where.c_str());
  assertTotalsAreSums(fs);
  LedgerStore s(fs, "owner", kTestRoll);
  WriteResult w = s.write(sc.ops(b.ids), "under-test", at("2026-12-01", "20:00"), nullptr);
  if (o == Outcome::New) {
    TEST_ASSERT_EQUAL_MESSAGE((int)WriteStatus::AlreadyApplied, (int)w.status, where.c_str());
    TEST_ASSERT_TRUE_MESSAGE(has(w.text, b.receipt), where.c_str());
  } else {
    TEST_ASSERT_EQUAL_MESSAGE((int)WriteStatus::Ok, (int)w.status, where.c_str());
    TEST_ASSERT_EQUAL_STRING_MESSAGE(b.receipt.c_str(), w.text.c_str(), where.c_str());
  }
  TEST_ASSERT_TRUE_MESSAGE(snapshot(fs) == b.newRows, where.c_str());
  WriteResult again = s.write(sc.ops(b.ids), "under-test", at("2026-12-01", "20:05"), nullptr);
  TEST_ASSERT_EQUAL_MESSAGE((int)WriteStatus::AlreadyApplied, (int)again.status, where.c_str());
  TEST_ASSERT_TRUE_MESSAGE(snapshot(fs) == b.newRows, where.c_str());
  return o;
}

}  // namespace

void test_kill_at_every_step_recovers_old_or_new() {
  size_t cuts = 0, olds = 0, news = 0;
  bool rolled = false;
  std::set<char> kinds;
  for (const Scenario& sc : scenarios()) {
    Baseline b;
    baseline(sc, b);
    TEST_ASSERT_TRUE_MESSAGE(b.steps.size() >= 2, sc.name);
    for (size_t k = 0; k < b.steps.size(); ++k) {
      for (size_t torn : tornLengths(b.steps[k])) {
        CutFiles fs = b.before;
        fs.reboot();
        fs.cutAt = (long)k;
        fs.tornBytes = torn;
        LedgerStore s(fs, "owner", kTestRoll);
        WriteResult w = s.write(sc.ops(b.ids), "under-test", at("2026-12-01", "20:00"), nullptr);
        (void)w;
        TEST_ASSERT_TRUE_MESSAGE(fs.dead, "the cut was never reached: the write took other steps");
        char where[200];
        snprintf(where, sizeof where, "%s: cut at step %zu (%c %s), %zu bytes landed", sc.name, k, b.steps[k].kind,
                 b.steps[k].path.c_str(), torn);
        const Outcome o = recoverAndRetry(sc, b, fs, where);
        ++cuts;
        (o == Outcome::Old ? olds : news)++;
        kinds.insert(b.steps[k].kind);
        if (b.steps[k].kind == 'r' && has(b.steps[k].path, "changes.log")) rolled = true;
      }
    }
  }
  TEST_ASSERT_TRUE_MESSAGE(rolled, "no scenario cut the log roll");
  char msg[120];
  snprintf(msg, sizeof msg, "%zu cuts: %zu old, %zu new", cuts, olds, news);
  TEST_MESSAGE(msg);
  TEST_ASSERT_TRUE(olds > 0 && news > 0);
  TEST_ASSERT_EQUAL_MESSAGE(4, kinds.size(), "append, whole write, rename and remove were all cut");
}

void test_kill_during_recovery_then_recover() {
  for (const Scenario& sc : scenarios()) {
    Baseline b;
    baseline(sc, b);
    for (size_t k = 0; k < b.steps.size(); ++k) {
      CutFiles first = b.before;
      first.reboot();
      first.cutAt = (long)k;
      first.tornBytes = b.steps[k].len;   // the whole record lands: recovery has work to do
      LedgerStore s(first, "owner", kTestRoll);
      (void)s.write(sc.ops(b.ids), "under-test", at("2026-12-01", "20:00"), nullptr);
      first.reboot();
      // Count the steps recovery takes, then cut each of them.
      CutFiles probe = first;
      { LedgerStore r(probe, "owner", kTestRoll); r.open(); }
      const size_t recSteps = probe.trace.size();
      for (size_t j = 0; j < recSteps; ++j) {
        for (size_t torn : tornLengths(probe.trace[j])) {
          CutFiles fs = first;
          fs.cutAt = (long)j;
          fs.tornBytes = torn;
          { LedgerStore r(fs, "owner", kTestRoll); r.open(); }
          char where[200];
          snprintf(where, sizeof where, "%s: cut at write step %zu, then recovery step %zu (%zu bytes)", sc.name, k, j,
                   torn);
          recoverAndRetry(sc, b, fs, where);
        }
      }
    }
  }
}

// Counter-test: the checker sees a mix, and without the change log a cut between two month
// swaps leaves one (so the log is what makes the move all or nothing).
void test_counter_without_the_log_a_cut_leaves_a_mix() {
  const Scenario sc = scenarios()[2];   // the move across a month boundary
  Baseline b;
  baseline(sc, b);
  TEST_ASSERT_TRUE(classify({"x"}, b.oldRows, b.newRows) == Outcome::Mixed);
  // Cut right after the first month's swap is complete: its .part renamed into place and
  // its .bak removed.
  long cutAt = -1;
  bool placed = false;
  for (size_t k = 0; k < b.steps.size() && cutAt < 0; ++k) {
    if (b.steps[k].kind == 'r' && has(b.steps[k].path, ".csv.part")) placed = true;
    if (placed && b.steps[k].kind == 'd' && has(b.steps[k].path, ".csv.bak")) cutAt = (long)k + 1;
  }
  TEST_ASSERT_TRUE_MESSAGE(cutAt > 0, "no completed month swap before the second");
  CutFiles fs = b.before;
  fs.reboot();
  fs.cutAt = cutAt;
  LedgerStore s(fs, "owner", kTestRoll);
  (void)s.write(sc.ops(b.ids), "under-test", at("2026-12-01", "20:00"), nullptr);
  fs.reboot();
  fs.files.erase("/mem/ledger/owner/changes.log");
  fs.files.erase("/mem/ledger/owner/changes.old.log");
  TEST_ASSERT_TRUE(classify(snapshot(fs), b.oldRows, b.newRows) == Outcome::Mixed);
}

void test_repeated_call_id_returns_first_receipt() {
  CutFiles fs;
  WriteResult first;
  {
    LedgerStore s(fs, "owner", kTestRoll);
    first = s.write({add(food("yoghurt", "150", "12"), "2026-11-17", "09:00")}, "toolu_01", at("2026-11-17", "09:05"),
                    nullptr);
    TEST_ASSERT_EQUAL((int)WriteStatus::Ok, (int)first.status);
    // A different payload under the same call id changes nothing.
    WriteResult again = s.write({add(food("cake", "400"), "2026-11-17", "10:00")}, "toolu_01",
                                at("2026-11-17", "10:05"), nullptr);
    TEST_ASSERT_EQUAL((int)WriteStatus::AlreadyApplied, (int)again.status);
    TEST_ASSERT_TRUE(has(again.text, "already applied"));
    TEST_ASSERT_TRUE(has(again.text, first.text));
  }
  // After a reboot the receipt comes from the log.
  LedgerStore s2(fs, "owner", kTestRoll);
  WriteResult again = s2.write({add(food("yoghurt", "150", "12"), "2026-11-17", "09:00")}, "toolu_01",
                               at("2026-11-17", "11:00"), nullptr);
  TEST_ASSERT_EQUAL((int)WriteStatus::AlreadyApplied, (int)again.status);
  TEST_ASSERT_TRUE(has(again.text, first.text));
  TEST_ASSERT_EQUAL(1, snapshot(fs).size());
  // No call id: every write is new.
  TEST_ASSERT_EQUAL((int)WriteStatus::Ok,
                    (int)s2.write({add(food("tea", "2"), "2026-11-17")}, "", at("2026-11-17", "11:00"), nullptr).status);
  TEST_ASSERT_EQUAL((int)WriteStatus::Ok,
                    (int)s2.write({add(food("tea", "2"), "2026-11-17")}, "", at("2026-11-17", "11:00"), nullptr).status);
  TEST_ASSERT_EQUAL(3, snapshot(fs).size());
}

void test_call_ids_and_recent_adds_survive_a_log_roll() {
  CutFiles fs;
  LedgerStore s(fs, "owner", kTestRoll);
  Row big = food("filler", "1");
  big.note = std::string(390, 'n');
  WriteResult first = s.write({add(big, "2026-11-02", "09:00")}, "keep-me", at("2026-11-02", "09:00"), nullptr);
  int k = 0;
  while (!fs.files.count("/mem/ledger/owner/changes.old.log")) {
    WriteResult w = s.write({add(big, "2026-11-02", "09:00")}, "f" + std::to_string(k++), at("2026-11-02", "09:00"),
                            nullptr);
    TEST_ASSERT_EQUAL((int)WriteStatus::Ok, (int)w.status);
    TEST_ASSERT_TRUE(k < 2000);
  }
  // One write after the roll, then a reboot: both logs are read.
  Row cake = food("cake", "400");
  WriteResult c = s.write({add(cake, "2026-11-02", "09:10")}, "after-roll", at("2026-11-02", "09:10"), nullptr);
  LedgerStore s2(fs, "owner", kTestRoll);
  WriteResult again = s2.write({add(big, "2026-11-02", "09:00")}, "keep-me", at("2026-11-02", "09:30"), nullptr);
  TEST_ASSERT_EQUAL((int)WriteStatus::AlreadyApplied, (int)again.status);
  TEST_ASSERT_TRUE(has(again.text, first.text));
  WriteResult dup = s2.write({add(cake, "2026-11-02", "09:20")}, "dup", at("2026-11-02", "09:20"), nullptr);
  TEST_ASSERT_TRUE_MESSAGE(has(dup.text, "identical to " + firstId(c)), dup.text.c_str());
  assertTotalsAreSums(fs);
}

void test_torn_log_tail_is_void_and_the_next_record_is_clean() {
  CutFiles fs;
  {
    LedgerStore s(fs, "owner", kTestRoll);
    TEST_ASSERT_EQUAL((int)WriteStatus::Ok,
                      (int)s.write({add(food("egg", "78", "6.3"), "2026-11-17")}, "a", at("2026-11-17", "08:00"), nullptr)
                          .status);
  }
  fs.files["/mem/ledger/owner/changes.log"] += "L1 2 99 0000abcd call\tb";   // a torn record
  LedgerStore s(fs, "owner", kTestRoll);
  WriteResult w = s.write({add(food("bread", "90"), "2026-11-17")}, "c", at("2026-11-17", "08:30"), nullptr);
  TEST_ASSERT_EQUAL((int)WriteStatus::Ok, (int)w.status);
  TEST_ASSERT_EQUAL_UINT64(2, w.seq);
  LedgerStore again(fs, "owner", kTestRoll);
  TEST_ASSERT_TRUE(again.open());
  TEST_ASSERT_EQUAL_UINT64(2, again.lastSeq());
  TEST_ASSERT_EQUAL(2, snapshot(fs).size());
  // A record whose crc does not match is void too.
  std::string& log = fs.files["/mem/ledger/owner/changes.log"];
  const size_t p = log.rfind("bread");
  log[p] = 'B';
  fs.files.erase("/mem/ledger/owner/2026-11.csv");
  TEST_ASSERT_EQUAL(1, snapshot(fs).size());
}

void test_refused_append_is_not_saved_and_changes_nothing() {
  CutFiles fs;
  LedgerStore s(fs, "owner", kTestRoll);
  TEST_ASSERT_EQUAL((int)WriteStatus::Ok,
                    (int)s.write({add(food("egg", "78"), "2026-11-17")}, "a", at("2026-11-17", "08:00"), nullptr).status);
  const auto before = fs.files;
  fs.refuseAppend = true;
  WriteResult w = s.write({add(food("bread", "90"), "2026-11-17")}, "b", at("2026-11-17", "08:30"), nullptr);
  TEST_ASSERT_EQUAL((int)WriteStatus::NotSaved, (int)w.status);
  TEST_ASSERT_TRUE(has(w.text, "nothing was written"));
  TEST_ASSERT_TRUE(before == fs.files);
  fs.refuseAppend = false;
  TEST_ASSERT_EQUAL((int)WriteStatus::Ok,
                    (int)s.write({add(food("bread", "90"), "2026-11-17")}, "b", at("2026-11-17", "08:31"), nullptr).status);
  TEST_ASSERT_EQUAL(2, snapshot(fs).size());
}

// ---- the 04:00 ledger day ---------------------------------------------------------------------
void test_ledger_day_at_2350_0010_0359_0400() {
  struct Case {
    const char* date;
    const char* time;
    const char* day;
  } cases[] = {{"2026-11-17", "23:50", "2026-11-17"}, {"2026-11-18", "00:10", "2026-11-17"},
               {"2026-11-18", "03:59", "2026-11-17"}, {"2026-11-18", "04:00", "2026-11-18"},
               {"2026-12-01", "02:00", "2026-11-30"}, {"2027-01-01", "03:00", "2026-12-31"},
               {"2028-03-01", "01:00", "2028-02-29"}};
  for (const Case& c : cases) {
    TEST_ASSERT_EQUAL_STRING_MESSAGE(c.day, ledgerDayOf(c.date, c.time).c_str(), c.time);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(c.day, ledgerToday(at(c.date, c.time, 3600)).c_str(), c.time);
    // Through the store: an add with no day lands on the ledger day and the local time.
    CutFiles fs;
    LedgerStore s(fs, "owner", kTestRoll);
    WriteResult w = s.write({add(food("snack", "100"))}, "x", at(c.date, c.time, 3600), nullptr);
    TEST_ASSERT_EQUAL((int)WriteStatus::Ok, (int)w.status);
    TEST_ASSERT_EQUAL_STRING(c.day, w.changed[0].day.c_str());
    TEST_ASSERT_EQUAL_STRING(c.time, w.changed[0].time.c_str());
    TEST_ASSERT_TRUE(fs.files.count(std::string("/mem/ledger/owner/") + std::string(c.day).substr(0, 7) + ".csv"));
  }
}

void test_an_explicit_day_wins() {
  CutFiles fs;
  LedgerStore s(fs, "owner", kTestRoll);
  // 00:10 on the 18th, but the owner says it was for the 16th.
  WriteResult w = s.write({add(food("crisps", "160"), "2026-11-16")}, "x", at("2026-11-18", "00:10"), nullptr);
  TEST_ASSERT_EQUAL_STRING("2026-11-16", w.changed[0].day.c_str());
  TEST_ASSERT_EQUAL_STRING("", w.changed[0].time.c_str());
  TEST_ASSERT_TRUE(has(w.text, "day 2026-11-16 (ledger day"));
}

void test_a_time_without_a_day_takes_the_nearest_occurrence() {
  struct Case {
    const char* nowDate;
    const char* nowTime;
    const char* given;
    const char* day;
  } cases[] = {
      {"2026-11-18", "05:00", "02:00", "2026-11-17"},   // 3 h ago, before 04:00
      {"2026-11-18", "00:30", "23:30", "2026-11-17"},   // an hour ago, yesterday's date
      {"2026-11-18", "10:00", "12:00", "2026-11-18"},   // in two hours: today
      {"2026-11-18", "23:00", "01:00", "2026-11-18"},   // two hours ahead, tomorrow 01:00
      {"2026-11-18", "20:00", "08:00", "2026-11-18"},   // 12 h either way: the past
  };
  for (const Case& c : cases)
    TEST_ASSERT_EQUAL_STRING_MESSAGE(c.day, ledgerDayForTime(at(c.nowDate, c.nowTime), c.given).c_str(), c.given);
  CutFiles fs;
  LedgerStore s(fs, "owner", kTestRoll);
  WriteResult w = s.write({add(food("toast", "90"), nullptr, "02:00")}, "x", at("2026-11-18", "05:00"), nullptr);
  TEST_ASSERT_EQUAL_STRING("2026-11-17", w.changed[0].day.c_str());
  TEST_ASSERT_EQUAL_STRING("02:00", w.changed[0].time.c_str());
}

void test_time_zone_change_moves_today_not_stored_rows() {
  CutFiles fs;
  LedgerStore s(fs, "owner", kTestRoll);
  // 02:30 UTC on the 18th: in London (UTC) that is still the 17th's ledger day.
  const int64_t epoch = epochOf("2026-11-18", "02:30", 0);
  Clock london{epoch, 0, true};
  WriteResult a = s.write({add(food("milk", "60"))}, "a", london, nullptr);
  TEST_ASSERT_EQUAL_STRING("2026-11-17", a.changed[0].day.c_str());
  // The owner flies to Helsinki (UTC+2): the same instant is 04:30, the 18th's ledger day.
  Clock helsinki{epoch, 7200, true};
  TEST_ASSERT_EQUAL_STRING("2026-11-18", ledgerToday(helsinki).c_str());
  WriteResult b = s.write({add(food("milk", "60"))}, "b", helsinki, nullptr);
  TEST_ASSERT_EQUAL_STRING("2026-11-18", b.changed[0].day.c_str());
  TEST_ASSERT_EQUAL_STRING("04:30", b.changed[0].time.c_str());
  // The row written in London keeps its day and time.
  DayView v;
  TEST_ASSERT_TRUE(s.day("2026-11-17", v));
  TEST_ASSERT_EQUAL(1, v.rows.size());
  TEST_ASSERT_EQUAL_STRING("02:30", v.rows[0].time.c_str());
  // Westward (UTC-5): 21:30 on the 17th.
  TEST_ASSERT_EQUAL_STRING("2026-11-17", ledgerToday(Clock{epoch, -5 * 3600, true}).c_str());
}

void test_month_boundary_rows_land_in_their_ledger_month() {
  CutFiles fs;
  LedgerStore s(fs, "owner", kTestRoll);
  // 01:00 on 1 Dec is the 30 Nov ledger day: the November file.
  WriteResult w = s.write({add(food("pizza", "800"))}, "a", at("2026-12-01", "01:00"), nullptr);
  TEST_ASSERT_EQUAL_STRING("2026-11-30", w.changed[0].day.c_str());
  TEST_ASSERT_TRUE(fs.files.count("/mem/ledger/owner/2026-11.csv"));
  TEST_ASSERT_FALSE(fs.files.count("/mem/ledger/owner/2026-12.csv"));
  // Moving it to 1 Dec rewrites both month files, and both days' lines are in the receipt.
  WriteResult m = s.write({moveOp(firstId(w), "2026-12-01")}, "b", at("2026-12-01", "09:00"), nullptr);
  TEST_ASSERT_EQUAL((int)WriteStatus::Ok, (int)m.status);
  TEST_ASSERT_TRUE(has(m.text, "moved from 2026-11-30: " + firstId(w)));
  TEST_ASSERT_TRUE(has(m.text, "day 2026-11-30 (ledger day, 04:00 to 04:00): 0 rows, 0 kcal, P 0 g"));
  TEST_ASSERT_TRUE(has(m.text, "day 2026-12-01 (ledger day, 04:00 to 04:00): 1 row, 800 kcal"));
  TEST_ASSERT_EQUAL(2, m.days.size());
  DayView v;
  TEST_ASSERT_TRUE(s.day("2026-12-01", v));
  TEST_ASSERT_EQUAL(1, v.rows.size());
  TEST_ASSERT_TRUE(s.day("2026-11-30", v));
  TEST_ASSERT_EQUAL(0, v.rows.size());
  // Year boundary.
  WriteResult y = s.write({add(food("cava", "90"))}, "c", at("2027-01-01", "00:30"), nullptr);
  TEST_ASSERT_EQUAL_STRING("2026-12-31", y.changed[0].day.c_str());
  TEST_ASSERT_TRUE(fs.files.count("/mem/ledger/owner/2026-12.csv"));
}

void test_unset_clock_needs_an_explicit_day() {
  CutFiles fs;
  LedgerStore s(fs, "owner", kTestRoll);
  Clock unset;   // a fresh device before its first time sync
  WriteResult w = s.write({add(food("banana", "105"))}, "a", unset, nullptr);
  TEST_ASSERT_EQUAL((int)WriteStatus::Refused, (int)w.status);
  TEST_ASSERT_TRUE_MESSAGE(has(w.text, "no day derivable"), w.text.c_str());
  TEST_ASSERT_TRUE(has(w.text, "nothing was written"));
  TEST_ASSERT_TRUE(fs.files.empty());
  Clock early{1000, 0, true};   // set, but to 1970
  TEST_ASSERT_EQUAL((int)WriteStatus::Refused, (int)s.write({add(food("banana", "105"))}, "a", early, nullptr).status);
  TEST_ASSERT_EQUAL((int)WriteStatus::Refused,
                    (int)s.write({add(food("banana", "105"), nullptr, "08:00")}, "a", unset, nullptr).status);
  WriteResult ok = s.write({add(food("banana", "105"), "2026-11-17")}, "a", unset, nullptr);
  TEST_ASSERT_EQUAL((int)WriteStatus::Ok, (int)ok.status);
  TEST_ASSERT_EQUAL_STRING("", ok.changed[0].time.c_str());
  TEST_ASSERT_EQUAL_STRING("", ledgerToday(unset).c_str());
  TEST_ASSERT_EQUAL_STRING("", ledgerDayForTime(unset, "08:00").c_str());
}

// ---- warnings, exactly ------------------------------------------------------------------------
namespace {

WriteResult addAt(LedgerStore& s, const Row& r, const char* hhmm, const std::string& call, const char* day = "2026-11-17") {
  return s.write({add(r, day, nullptr)}, call, at("2026-11-17", hhmm), nullptr);
}

bool warnsIdentical(const WriteResult& w) {
  for (const std::string& x : w.warnings)
    if (has(x, "identical to")) return true;
  return false;
}

}  // namespace

void test_identical_row_is_the_exact_rule() {
  Row base = food("Greek Yoghurt \xC3\xA4", "150", "12");
  base.qty = fx("170", kQtyDp);
  base.unit = "g";
  // Each variant differs from base in one dimension of the rule (or only outside it).
  struct Variant {
    const char* name;
    std::function<void(Row&)> change;
    bool identical;
  } variants[] = {
      {"same everything", [](Row&) {}, true},
      {"item case differs", [](Row& r) { r.item = "greek YOGHURT \xC3\xA4"; }, true},
      {"unit case differs", [](Row& r) { r.unit = "G"; }, true},
      {"Latin-1 case differs", [](Row& r) { r.item = "GREEK YOGHURT \xC3\x84"; }, true},
      {"protein differs (not in the rule)", [](Row& r) { r.protein = fx("13", kGramDp); }, true},
      {"meal differs (not in the rule)", [](Row& r) { r.meal = "snack"; }, true},
      {"item differs", [](Row& r) { r.item = "Greek Yoghurt 0%"; }, false},
      {"item has a trailing space", [](Row& r) { r.item += " "; }, false},
      {"qty differs", [](Row& r) { r.qty = fx("171", kQtyDp); }, false},
      {"qty absent", [](Row& r) { r.qty = Fixed(); }, false},
      {"unit differs", [](Row& r) { r.unit = "ml"; }, false},
      {"kcal differs by 0.1", [](Row& r) { r.kcal = fx("150.1", kKcalDp); }, false},
  };
  for (const Variant& v : variants) {
    CutFiles fs;
    LedgerStore s(fs, "owner", kTestRoll);
    WriteResult a = addAt(s, base, "08:00", "a");
    Row second = base;
    v.change(second);
    WriteResult b = addAt(s, second, "08:20", "b");
    TEST_ASSERT_EQUAL_MESSAGE((int)WriteStatus::Ok, (int)b.status, v.name);
    TEST_ASSERT_EQUAL_MESSAGE(v.identical, warnsIdentical(b), v.name);
    if (v.identical) TEST_ASSERT_TRUE_MESSAGE(has(b.text, "identical to " + firstId(a)), b.text.c_str());
    TEST_ASSERT_EQUAL_MESSAGE(2, snapshot(fs).size(), "a warning never refuses");
  }
  // Latin-1 capitals fold: "\xC3\x84" (A umlaut) matches "\xC3\xA4".
  TEST_ASSERT_EQUAL_STRING("\xC3\xA4pfel \xC3\x97", foldCase("\xC3\x84PFEL \xC3\x97").c_str());
  // Time: 30 minutes is inside, 31 is not; a different ledger day is not; a removed row is not.
  {
    CutFiles fs;
    LedgerStore s(fs, "owner", kTestRoll);
    addAt(s, base, "08:00", "a");
    TEST_ASSERT_TRUE(warnsIdentical(addAt(s, base, "08:30", "b")));
    TEST_ASSERT_FALSE(warnsIdentical(addAt(s, base, "09:01", "c", "2026-11-16")));
  }
  {
    CutFiles fs;
    LedgerStore s(fs, "owner", kTestRoll);
    addAt(s, base, "08:00", "a");
    TEST_ASSERT_FALSE(warnsIdentical(addAt(s, base, "08:31", "b")));
  }
  {
    CutFiles fs;
    LedgerStore s(fs, "owner", kTestRoll);
    WriteResult a = addAt(s, base, "08:00", "a");
    s.write({removeOp(firstId(a))}, "r", at("2026-11-17", "08:05"), nullptr);
    TEST_ASSERT_FALSE(warnsIdentical(addAt(s, base, "08:10", "b")));
  }
  {   // two identical adds in one write: the second names the first
    CutFiles fs;
    LedgerStore s(fs, "owner", kTestRoll);
    WriteResult w = s.write({add(base, "2026-11-17"), add(base, "2026-11-17")}, "a", at("2026-11-17", "08:00"), nullptr);
    TEST_ASSERT_EQUAL(1, w.warnings.size());
    TEST_ASSERT_TRUE(has(w.warnings[0], w.changed[1].id + " is identical to " + w.changed[0].id));
  }
  {   // unset clock: add times are unknown, no warning
    CutFiles fs;
    LedgerStore s(fs, "owner", kTestRoll);
    s.write({add(base, "2026-11-17")}, "a", Clock(), nullptr);
    TEST_ASSERT_FALSE(warnsIdentical(s.write({add(base, "2026-11-17")}, "b", Clock(), nullptr)));
  }
}

void test_no_arithmetic_warning_for_label_or_with_a_macro_missing() {
  auto row = [](Basis b, const char* kcal, const char* p, const char* c, const char* f) {
    Row r;
    r.item = "x";
    r.basis = b;
    r.kcal = fx(kcal, kKcalDp);
    if (p) r.protein = fx(p, kGramDp);
    if (c) r.carb = fx(c, kGramDp);
    if (f) r.fat = fx(f, kGramDp);
    r.id = "LTEST0";
    return r;
  };
  // A protein bar with polyols, read from its label: 4P+4C+9F alone is far off, and it is right.
  Row bar = row(Basis::Label, "198", "20", "21", "7");
  bar.polyol = fx("14", kGramDp);
  bar.fibre = fx("3", kGramDp);
  TEST_ASSERT_EQUAL(0, figureWarnings(bar).size());
  // A beer from its label, without the alcohol column: 4P+4C+9F is a third of the kcal.
  TEST_ASSERT_EQUAL(0, figureWarnings(row(Basis::Label, "142", "1.6", "10.6", "0")).size());
  // A label row with nonsense arithmetic still gets no arithmetic warning.
  TEST_ASSERT_EQUAL(0, figureWarnings(row(Basis::Label, "900", "1", "1", "1")).size());
  // Any basis with a macro missing: none.
  for (Basis b : {Basis::Estimate, Basis::Stated, Basis::Label, Basis::None, Basis::Legacy}) {
    TEST_ASSERT_EQUAL(0, figureWarnings(row(b, "900", nullptr, "10", "10")).size());
    TEST_ASSERT_EQUAL(0, figureWarnings(row(b, "900", "10", nullptr, "10")).size());
    TEST_ASSERT_EQUAL(0, figureWarnings(row(b, "900", "10", "10", nullptr)).size());
  }
  // Estimate and stated: 15% is the line. 4*10 + 4*20 + 9*10 = 210 kcal.
  for (Basis b : {Basis::Estimate, Basis::Stated}) {
    TEST_ASSERT_EQUAL(0, figureWarnings(row(b, "241.5", "10", "20", "10")).size());   // +15.0%
    TEST_ASSERT_EQUAL(0, figureWarnings(row(b, "178.5", "10", "20", "10")).size());   // -15.0%
    std::vector<std::string> w = figureWarnings(row(b, "241.6", "10", "20", "10"));
    TEST_ASSERT_EQUAL(1, w.size());
    TEST_ASSERT_TRUE_MESSAGE(has(w[0], "LTEST0: 241.6 kcal is 15% above") && has(w[0], "= 210 kcal"), w[0].c_str());
    TEST_ASSERT_EQUAL(1, figureWarnings(row(b, "178.4", "10", "20", "10")).size());
  }
  // None and legacy bases: no arithmetic warning either (the rule names estimate and stated).
  TEST_ASSERT_EQUAL(0, figureWarnings(row(Basis::None, "400", "10", "20", "10")).size());
  TEST_ASSERT_EQUAL(0, figureWarnings(row(Basis::Legacy, "400", "10", "20", "10")).size());
  // Alcohol counts 7 kcal/g, polyol 2.4, fibre 2: a stated beer with its alcohol is in line.
  Row beer = row(Basis::Stated, "142", "1.6", "10.6", "0");
  beer.alcohol = fx("13", kGramDp);
  TEST_ASSERT_EQUAL(0, figureWarnings(beer).size());
  // A kJ-sized value: 4.184 times the macro energy says kJ.
  std::vector<std::string> kj = figureWarnings(row(Basis::Estimate, "878.6", "10", "20", "10"));
  TEST_ASSERT_EQUAL(1, kj.size());
  TEST_ASSERT_TRUE_MESSAGE(has(kj[0], "kJ"), kj[0].c_str());
  TEST_ASSERT_EQUAL(0, figureWarnings(row(Basis::Label, "878.6", "10", "20", "10")).size());
}

void test_errors_refuse_the_whole_write() {
  CutFiles fs;
  LedgerStore s(fs, "owner", kTestRoll);
  WriteResult seed = s.write({add(food("rice", "260"), "2026-11-17")}, "a", at("2026-11-17", "12:00"), nullptr);
  const auto before = fs.files;
  auto refused = [&](std::vector<Op> ops, const std::string& why) {
    WriteResult w = s.write(ops, "r-" + why, at("2026-11-17", "12:00"), nullptr);
    TEST_ASSERT_EQUAL_MESSAGE((int)WriteStatus::Refused, (int)w.status, why.c_str());
    TEST_ASSERT_TRUE_MESSAGE(has(w.text, why), w.text.c_str());
    TEST_ASSERT_TRUE_MESSAGE(has(w.text, "nothing was written"), w.text.c_str());
    TEST_ASSERT_TRUE_MESSAGE(before == fs.files, why.c_str());
  };
  Row oil = food("olive oil", "1850");
  oil.qty = fx("100", kQtyDp);
  oil.unit = "g";
  refused({add(food("ok", "10"), "2026-11-17"), add(oil, "2026-11-17")}, "more than 900 kcal per 100 g");
  Row per = food("bar", "200");
  per.per100 = fx("1700", kKcalDp);
  refused({add(per, "2026-11-17")}, "more than 900 kcal per 100 g");
  refused({add(food("ok", "10"), "2026-11-17"), removeOp("LZZZZZ")}, "no row LZZZZZ");
  refused({removeOp(firstId(seed)), updateKcal(firstId(seed), "1")}, "no row " + firstId(seed));
  refused({add(food("x", "10"), "2026-11-31")}, "day");
  refused({add(food("x", "10"), "2026-11-17", "24:00")}, "time");
  refused({moveOp(firstId(seed), "17 Nov")}, "day");
  Row noKcal = food("x", "1");
  noKcal.kcal = Fixed();
  refused({add(noKcal, "2026-11-17")}, "kcal");
  Row neg = food("x", "-5");
  refused({add(neg, "2026-11-17")}, "kcal");
  refused({add(food("", "5"), "2026-11-17")}, "item");
  Row longItem = food(std::string(kMaxItem + 1, 'a').c_str(), "5");
  refused({add(longItem, "2026-11-17")}, "item");
  refused({}, "1 to 16 ops");
  refused(std::vector<Op>(kMaxOps + 1, add(food("x", "1"), "2026-11-17")), "1 to 16 ops");
  Op noIds;
  noIds.kind = OpKind::Remove;
  refused({noIds}, "id");
  // Exactly 900 kcal per 100 g is allowed (pure fat), and 884 per 100 ml of oil.
  Row fat = food("lard", "900");
  fat.qty = fx("100", kQtyDp);
  fat.unit = "g";
  TEST_ASSERT_EQUAL((int)WriteStatus::Ok, (int)s.write({add(fat, "2026-11-17")}, "fat", at("2026-11-17", "12:00"), nullptr).status);
}

void test_update_move_remove_receipts_and_targets() {
  CutFiles fs;
  LedgerStore s(fs, "owner", kTestRoll);
  Targets t;
  t.kcal = fx("2300", kKcalDp);
  t.protein = fx("160", kGramDp);
  WriteResult a = s.write({add(food("chicken", "900", "100"), "2026-11-17", "13:00"),
                           add(food("salad", "712", "18.3"), "2026-11-17", "19:00")},
                          "a", at("2026-11-17", "19:05"), &t);
  TEST_ASSERT_TRUE_MESSAGE(
      has(a.text, "day 2026-11-17 (ledger day, 04:00 to 04:00): 2 rows, 1,612 kcal, P 118.3 g; target 2,300 / 160 -> left 688 kcal, 41.7 g"),
      a.text.c_str());
  const std::string chicken = a.changed[0].id;
  Op upd;
  upd.kind = OpKind::Update;
  upd.ids = {chicken};
  upd.setMask = kFKcal | kFNote | kFProtein;
  upd.set.kcal = fx("950", kKcalDp);
  upd.set.note = "thigh, not breast";
  // protein left unset in `set`: clears it
  WriteResult u = s.write({upd}, "u", at("2026-11-17", "19:10"), &t);
  TEST_ASSERT_TRUE_MESSAGE(has(u.text, "updated " + chicken + " | 950 kcal |"), u.text.c_str());
  TEST_ASSERT_TRUE(has(u.text, "note thigh, not breast"));
  TEST_ASSERT_TRUE_MESSAGE(has(u.text, "2 rows, 1,662 kcal, P 18.3 g (P missing on 1 row); target 2,300 / 160 -> left 638 kcal, 141.7 g"),
                           u.text.c_str());
  WriteResult r = s.write({removeOp(chicken)}, "r", at("2026-11-17", "19:20"), &t);
  TEST_ASSERT_TRUE(has(r.text, "removed " + chicken + " | 950 kcal"));
  TEST_ASSERT_TRUE(has(r.text, "1 row, 712 kcal, P 18.3 g; target 2,300 / 160 -> left 1,588 kcal, 141.7 g"));
  // Over target: the form stays, left goes negative.
  WriteResult o = s.write({add(food("cake", "2000"), "2026-11-17")}, "o", at("2026-11-17", "20:00"), &t);
  TEST_ASSERT_TRUE_MESSAGE(has(o.text, "left -412 kcal, 141.7 g"), o.text.c_str());
  // Ids are read case-insensitively.
  std::string lower = firstId(o);
  for (char& ch : lower) ch = (char)tolower(ch);
  TEST_ASSERT_EQUAL((int)WriteStatus::Ok, (int)s.write({removeOp(lower)}, "lc", at("2026-11-17", "20:01"), &t).status);
  // A day line from ledger_day's side is the same line.
  DayView v;
  TEST_ASSERT_TRUE(s.day("2026-11-17", v));
  TEST_ASSERT_EQUAL_STRING(
      "day 2026-11-17 (ledger day, 04:00 to 04:00): 1 row, 712 kcal, P 18.3 g; target 2,300 / 160 -> left 1,588 kcal, 141.7 g",
      renderDayLine(v.totals, &t).c_str());
  TEST_ASSERT_EQUAL_STRING("day 2026-11-17 (ledger day, 04:00 to 04:00): 1 row, 712 kcal, P 18.3 g; no target set",
                           renderDayLine(v.totals, nullptr).c_str());
}

void test_day_rows_are_ordered_by_time() {
  CutFiles fs;
  LedgerStore s(fs, "owner", kTestRoll);
  s.write({add(food("dinner", "600"), "2026-11-17", "19:00"), add(food("undated", "50"), "2026-11-17"),
           add(food("breakfast", "300"), "2026-11-17", "08:00")},
          "a", at("2026-11-17", "20:00"), nullptr);
  DayView v;
  TEST_ASSERT_TRUE(s.day("2026-11-17", v));
  TEST_ASSERT_EQUAL(3, v.rows.size());
  TEST_ASSERT_EQUAL_STRING("breakfast", v.rows[0].item.c_str());
  TEST_ASSERT_EQUAL_STRING("dinner", v.rows[1].item.c_str());
  TEST_ASSERT_EQUAL_STRING("undated", v.rows[2].item.c_str());
}

void test_text_fields_are_cleaned_and_rows_render_figures_first() {
  CutFiles fs;
  LedgerStore s(fs, "owner", kTestRoll);
  Row r = food("oat\nporridge,\"big\"", "312", "10.5");
  r.qty = fx("250", kQtyDp);
  r.unit = "g";
  r.carb = fx("54", kGramDp);
  r.fat = fx("6.2", kGramDp);
  r.fibre = fx("8", kGramDp);
  r.meal = "breakfast";
  r.src = "photo p12";
  WriteResult w = s.write({add(r, "2026-11-17", "08:10")}, "a", at("2026-11-17", "08:15"), nullptr);
  TEST_ASSERT_EQUAL_STRING("oat porridge,\"big\"", w.changed[0].item.c_str());
  TEST_ASSERT_EQUAL_STRING((w.changed[0].id +
                            " | 312 kcal, P 10.5 g, C 54 g, F 6.2 g, fibre 8 g | 250 g oat porridge,\"big\" | "
                            "2026-11-17 08:10 breakfast | estimate | src photo p12")
                               .c_str(),
                           renderRow(w.changed[0]).c_str());
  TEST_ASSERT_EQUAL(1, snapshot(fs).size());
}

void test_damaged_month_file_is_named_and_not_overwritten() {
  CutFiles fs;
  LedgerStore s(fs, "owner", kTestRoll);
  s.write({add(food("egg", "78"), "2026-11-17")}, "a", at("2026-11-17", "08:00"), nullptr);
  fs.files["/mem/ledger/owner/2026-11.csv"] = "garbage\n";
  LedgerStore s2(fs, "owner", kTestRoll);
  DayView v;
  std::string err;
  TEST_ASSERT_FALSE(s2.day("2026-11-17", v, &err));
  TEST_ASSERT_TRUE_MESSAGE(has(err, "2026-11.csv"), err.c_str());
  WriteResult w = s2.write({add(food("bread", "90"), "2026-11-17")}, "b", at("2026-11-17", "08:30"), nullptr);
  TEST_ASSERT_TRUE(w.status != WriteStatus::Ok);
  TEST_ASSERT_TRUE_MESSAGE(has(w.text, "2026-11.csv"), w.text.c_str());
  TEST_ASSERT_EQUAL_STRING("garbage\n", fs.files["/mem/ledger/owner/2026-11.csv"].c_str());
  // Another month still works.
  TEST_ASSERT_EQUAL((int)WriteStatus::Ok,
                    (int)s2.write({add(food("bread", "90"), "2026-10-17")}, "c", at("2026-11-17", "08:30"), nullptr).status);
}

void test_people_are_kept_apart_and_names_checked() {
  CutFiles fs;
  LedgerStore a(fs, "owner", kTestRoll), b(fs, "6098532481", kTestRoll);
  a.write({add(food("egg", "78"), "2026-11-17")}, "x", at("2026-11-17", "08:00"), nullptr);
  WriteResult w = b.write({add(food("egg", "78"), "2026-11-17")}, "x", at("2026-11-17", "08:00"), nullptr);
  TEST_ASSERT_EQUAL((int)WriteStatus::Ok, (int)w.status);   // the same call id per person is separate
  TEST_ASSERT_EQUAL(1, snapshot(fs, "owner").size());
  TEST_ASSERT_EQUAL(1, snapshot(fs, "6098532481").size());
  TEST_ASSERT_EQUAL_STRING("/mem/ledger/6098532481", b.dir().c_str());
  for (const char* bad : {"", "..", "a/b", "a b", "x\n"}) TEST_ASSERT_FALSE_MESSAGE(validPerson(bad), bad);
  CutFiles fs2;
  LedgerStore evil(fs2, "../tx", kTestRoll);
  WriteResult e = evil.write({add(food("egg", "78"), "2026-11-17")}, "x", at("2026-11-17", "08:00"), nullptr);
  TEST_ASSERT_TRUE(e.status != WriteStatus::Ok);
  TEST_ASSERT_TRUE(fs2.files.empty());
}

void test_day_and_time_validation() {
  TEST_ASSERT_TRUE(validDay("2028-02-29"));
  TEST_ASSERT_FALSE(validDay("2026-02-29"));
  TEST_ASSERT_FALSE(validDay("2026-13-01"));
  TEST_ASSERT_FALSE(validDay("2026-1-01"));
  TEST_ASSERT_TRUE(validTime("00:00"));
  TEST_ASSERT_TRUE(validTime("23:59"));
  TEST_ASSERT_FALSE(validTime("24:00"));
  TEST_ASSERT_FALSE(validTime("8:00"));
  TEST_ASSERT_EQUAL_STRING("2026-12-01", addDays("2026-11-30", 1).c_str());
  TEST_ASSERT_EQUAL_STRING("2025-12-31", addDays("2026-01-01", -1).c_str());
  TEST_ASSERT_EQUAL(kFKcal, fieldByName("kcal"));
  TEST_ASSERT_EQUAL(kFProtein, fieldByName("protein_g"));
  TEST_ASSERT_EQUAL(kFProtein, fieldByName("protein"));
  TEST_ASSERT_EQUAL(0, fieldByName("id"));
  TEST_ASSERT_EQUAL(0, fieldByName("call"));
}

void test_odd_call_ids_are_kept_exactly() {
  CutFiles fs;
  for (const char* call : {" leading space", "tab\tinside", "line\nbreak", "back\\slash", "L1 2 3 abcdef01 x"}) {
    LedgerStore s(fs, "owner", kTestRoll);
    WriteResult a = s.write({add(food("tea", "2"), "2026-11-17")}, call, at("2026-11-17", "08:00"), nullptr);
    TEST_ASSERT_EQUAL_MESSAGE((int)WriteStatus::Ok, (int)a.status, call);
    LedgerStore again(fs, "owner", kTestRoll);   // read back from the log
    WriteResult b = again.write({add(food("tea", "2"), "2026-11-17")}, call, at("2026-11-17", "08:01"), nullptr);
    TEST_ASSERT_EQUAL_MESSAGE((int)WriteStatus::AlreadyApplied, (int)b.status, call);
    TEST_ASSERT_TRUE_MESSAGE(has(b.text, a.text), call);
  }
  TEST_ASSERT_EQUAL(5, snapshot(fs).size());
}

void test_a_damaged_row_does_not_block_other_months() {
  CutFiles fs;
  {
    LedgerStore s(fs, "owner", kTestRoll);
    s.write({add(food("egg", "78"), "2026-11-17"), add(food("pear", "60"), "2026-10-17")}, "a", at("2026-11-17", "08:00"),
            nullptr);
  }
  // The November file keeps a good header but a row is broken, and it is behind the log.
  std::string& nov = fs.files["/mem/ledger/owner/2026-11.csv"];
  nov.replace(nov.find("seq=1"), 5, "seq=0");
  nov += "not,a,row\n";
  LedgerStore s(fs, "owner", kTestRoll);
  std::string report;
  TEST_ASSERT_TRUE(s.open(&report));
  TEST_ASSERT_TRUE_MESSAGE(has(report, "2026-11.csv is damaged"), report.c_str());
  TEST_ASSERT_EQUAL((int)WriteStatus::Ok,
                    (int)s.write({add(food("plum", "30"), "2026-10-18")}, "b", at("2026-11-17", "08:30"), nullptr).status);
  DayView v;
  TEST_ASSERT_TRUE(s.day("2026-10-18", v));
  TEST_ASSERT_EQUAL(1, v.rows.size());
  TEST_ASSERT_TRUE(has(fs.files["/mem/ledger/owner/2026-11.csv"], "not,a,row"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_add_returns_id);
  RUN_TEST(test_ids_are_unique_and_neighbours_look_unlike);
  RUN_TEST(test_fixed_point_parse_and_format_are_exact);
  RUN_TEST(test_csv_round_trip_keeps_every_field);
  RUN_TEST(test_totals_equal_sum_after_random_ops);
  RUN_TEST(test_kill_at_every_step_recovers_old_or_new);
  RUN_TEST(test_kill_during_recovery_then_recover);
  RUN_TEST(test_counter_without_the_log_a_cut_leaves_a_mix);
  RUN_TEST(test_repeated_call_id_returns_first_receipt);
  RUN_TEST(test_call_ids_and_recent_adds_survive_a_log_roll);
  RUN_TEST(test_torn_log_tail_is_void_and_the_next_record_is_clean);
  RUN_TEST(test_refused_append_is_not_saved_and_changes_nothing);
  RUN_TEST(test_ledger_day_at_2350_0010_0359_0400);
  RUN_TEST(test_an_explicit_day_wins);
  RUN_TEST(test_a_time_without_a_day_takes_the_nearest_occurrence);
  RUN_TEST(test_time_zone_change_moves_today_not_stored_rows);
  RUN_TEST(test_month_boundary_rows_land_in_their_ledger_month);
  RUN_TEST(test_unset_clock_needs_an_explicit_day);
  RUN_TEST(test_identical_row_is_the_exact_rule);
  RUN_TEST(test_no_arithmetic_warning_for_label_or_with_a_macro_missing);
  RUN_TEST(test_errors_refuse_the_whole_write);
  RUN_TEST(test_update_move_remove_receipts_and_targets);
  RUN_TEST(test_day_rows_are_ordered_by_time);
  RUN_TEST(test_text_fields_are_cleaned_and_rows_render_figures_first);
  RUN_TEST(test_damaged_month_file_is_named_and_not_overwritten);
  RUN_TEST(test_people_are_kept_apart_and_names_checked);
  RUN_TEST(test_day_and_time_validation);
  RUN_TEST(test_odd_call_ids_are_kept_exactly);
  RUN_TEST(test_a_damaged_row_does_not_block_other_months);
  return UNITY_END();
}
