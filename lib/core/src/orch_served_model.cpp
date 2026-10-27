#include "nimbus/orch/served_model.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>

#include "nimbus/orch/model_catalog.h"  // cleanModelId - provider text is untrusted
#include "nimbus/orch/model_key.h"      // kModelKeyCap - one model id's buffer
#include "nimbus/orch/model_ledger.h"   // kProvKeyCap - one provider id's buffer

namespace nimbus {
namespace orch {

namespace {

std::atomic<ServedModelResolver> g_resolver{nullptr};

struct Note {
  char     host[kProvKeyCap] = {0};
  char     asked[kModelKeyCap] = {0};
  char     model[kModelKeyCap] = {0};
  uint32_t seq = 0;   // 0 = free; larger = newer
};

// The notes are allocated on the first answer (on the device a block this size comes
// from PSRAM), so a build that never answers keeps no RAM for them.
struct Notes {
  Note     n[kServedNotes];
  uint32_t seq = 0;
};

std::mutex g_mx;
std::unique_ptr<Notes> g_notes;

void copyBounded(char* dst, size_t cap, const char* src) { std::snprintf(dst, cap, "%s", src ? src : ""); }

bool blank(const char* s) { return !s || !*s; }

// The note for (host, asked): its own, else a free one, else the oldest.
Note& slotFor(Notes& t, const char* host, const char* asked) {
  for (Note& e : t.n)
    if (e.seq && !std::strcmp(e.host, host) && !std::strcmp(e.asked, asked)) return e;
  Note* oldest = &t.n[0];
  for (Note& e : t.n)
    if (e.seq < oldest->seq) oldest = &e;
  return *oldest;
}

}  // namespace

void installServedModelResolver(ServedModelResolver r) { g_resolver.store(r); }

void resolveServedModel(const char* host, const char* echo, char* out, size_t cap) {
  if (!out || !cap) return;
  out[0] = 0;
  // Provider text: only a clean model id (no newline, tab or markup) is recorded, so no
  // echo can forge a log line, a page line or a prompt line.
  if (!echo || !*echo || !cleanModelId(echo)) return;
  const ServedModelResolver r = g_resolver.load();
  if (r) r(host ? host : "", echo, out, cap);
  if (!out[0] || !cleanModelId(out)) copyBounded(out, cap, echo);   // no answer: the echo stands
}

void noteServedModel(const char* host, const char* asked, const char* model) {
  if (blank(host) || blank(asked) || blank(model)) return;
  std::lock_guard<std::mutex> lk(g_mx);
  if (!g_notes) g_notes.reset(new (std::nothrow) Notes());
  if (!g_notes) return;   // no memory: the turn record still carries the model
  Note& slot = slotFor(*g_notes, host, asked);
  copyBounded(slot.host, sizeof(slot.host), host);
  copyBounded(slot.asked, sizeof(slot.asked), asked);
  copyBounded(slot.model, sizeof(slot.model), model);
  slot.seq = ++g_notes->seq;
}

std::string servedModelFor(const std::string& host, const std::string& asked) {
  std::lock_guard<std::mutex> lk(g_mx);
  if (!g_notes) return std::string();
  for (const Note& e : g_notes->n)
    if (e.seq && host == e.host && asked == e.asked) return e.model;
  return std::string();
}

ServedModelNote lastServedModel() {
  ServedModelNote out;
  std::lock_guard<std::mutex> lk(g_mx);
  if (!g_notes) return out;
  const Note* newest = nullptr;
  for (const Note& e : g_notes->n)
    if (e.seq && (!newest || e.seq > newest->seq)) newest = &e;
  if (newest) out = ServedModelNote{newest->host, newest->asked, newest->model};
  return out;
}

std::string runsOnLine(const std::string& host, const std::string& asked, const std::string& concrete,
                       const std::string& headFact) {
  std::string line = "[THIS TURN RUNS ON] " + host;
  if (!asked.empty()) line += " / " + (concrete.empty() ? asked : concrete);
  if (!asked.empty() && !concrete.empty() && concrete != asked) line += " (asked for " + asked + ")";
  line += ".";
  if (headFact.empty()) return line + " When asked which model you run, name this one only.\n";
  return line + " " + headFact +
         " When asked which model you run, name this one and say why the chosen head is not running this turn.\n";
}

void clearServedModels() {
  std::lock_guard<std::mutex> lk(g_mx);
  g_notes.reset();
}

}  // namespace orch
}  // namespace nimbus
