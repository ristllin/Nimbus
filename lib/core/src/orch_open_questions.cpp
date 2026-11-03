#include "nimbus/orch/open_questions.h"

#include <algorithm>
#include <cstdlib>

#include "nimbus/mem_cap.h"           // utf8CapLen
#include "nimbus/orch/word_match.h"   // lowerAscii / isWordChar (shared helpers)
#include "nimbus/utf8.h"              // utf8Repair

namespace nimbus {
namespace orch {

namespace {

constexpr const char kHeader[] = "#nimbus-openq v1";

uint32_t toU32(const std::string& s) {
  if (s.empty()) return 0;
  for (char c : s)
    if (c < '0' || c > '9') return 0;
  return (uint32_t)std::strtoul(s.c_str(), nullptr, 10);
}

// One item line of the file, or false when the line is not well-formed.
bool parseLine(const std::string& line, OpenQEntry& e) {
  // q<id> <R|-> <createdHours> <text>
  if (line.size() < 4 || (line[0] != 'q' && line[0] != 'Q')) return false;
  size_t at = 1;
  while (at < line.size() && line[at] >= '0' && line[at] <= '9') at++;
  if (at == 1 || at + 2 >= line.size() || line[at] != '\t') return false;
  e.id = toU32(line.substr(1, at - 1));
  if (e.id == 0) return false;
  at++;
  const char state = line[at];
  if (state != 'R' && state != '-') return false;
  e.raised = state == 'R';
  at++;
  if (line[at] != '\t') return false;
  at++;
  const size_t tab = line.find('\t', at);
  const std::string hours = tab == std::string::npos ? line.substr(at) : line.substr(at, tab - at);
  e.createdHours = toU32(hours);
  std::string text = tab == std::string::npos ? std::string() : line.substr(tab + 1);
  text = sanitizePrefText(text);
  if (text.size() > kOpenQItemMax) text.resize((size_t)utf8CapLen(text.c_str(), (int)text.size(), (int)kOpenQItemMax));
  e.text = text;
  return !e.text.empty();
}

std::string serializeState(const std::vector<OpenQEntry>& items, uint32_t nextId) {
  std::string out = std::string(kHeader) + " next=" + std::to_string(nextId) + "\n";
  for (const OpenQEntry& e : items)
    out += e.idStr() + "\t" + (e.raised ? "R" : "-") + "\t" + std::to_string(e.createdHours) +
           "\t" + e.text + "\n";
  return out;
}

}  // namespace

const char* openQErrWords(OpenQErr e) {
  switch (e) {
    case OpenQErr::Ok:            return "ok";
    case OpenQErr::Empty:         return "the item is empty";
    case OpenQErr::TooLong:       return "keep the item under 160 characters";
    case OpenQErr::Full:          return "the open list is full (5 items); remove one first";
    case OpenQErr::NotFound:      return "no open item with that id";
    case OpenQErr::PersistFailed: return "could not save the open list (storage error); nothing changed";
  }
  return "error";
}

// ---- the store ---------------------------------------------------------------

void OpenQuestions::begin(PrefsStoreIO* io) {
  io_ = io;
  items_.clear();
  nextId_ = 1;
  std::string blob;
  existed_ = io_ && io_->load(blob);
  if (existed_) parse(blob);
}

size_t OpenQuestions::parse(const std::string& blob) {
  items_.clear();
  uint32_t next = 1, maxId = 0;
  size_t pos = 0;
  while (pos < blob.size()) {
    size_t nl = blob.find('\n', pos);
    if (nl == std::string::npos) nl = blob.size();
    std::string line = blob.substr(pos, nl - pos);
    pos = nl + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind(kHeader, 0) == 0) {
      // next=<n> in the header
      const size_t at = line.find("next=");
      if (at != std::string::npos) next = toU32(line.substr(at + 5));
      continue;
    }
    OpenQEntry e;
    if (!parseLine(line, e) || items_.size() >= kOpenQMaxItems) continue;
    bool dup = false;
    for (const OpenQEntry& k : items_)
      if (k.id == e.id || prefKey(k.text) == prefKey(e.text)) dup = true;
    if (dup) continue;
    maxId = std::max(maxId, e.id);
    items_.push_back(std::move(e));
  }
  nextId_ = std::max(next, maxId + 1);
  return items_.size();
}

std::string OpenQuestions::serialize() const { return serializeState(items_, nextId_); }

int OpenQuestions::indexOf(uint32_t id) const {
  for (size_t i = 0; i < items_.size(); i++)
    if (items_[i].id == id) return (int)i;
  return -1;
}

int OpenQuestions::indexOfKey(const std::string& key) const {
  for (size_t i = 0; i < items_.size(); i++)
    if (prefKey(items_[i].text) == key) return (int)i;
  return -1;
}

const OpenQEntry* OpenQuestions::find(uint32_t id) const {
  const int i = indexOf(id);
  return i < 0 ? nullptr : &items_[(size_t)i];
}

OpenQErr OpenQuestions::commit(const std::vector<OpenQEntry>& next, uint32_t nextId) {
  if (!io_ || !io_->saveChecked(serializeState(next, nextId))) return OpenQErr::PersistFailed;
  items_ = next;
  nextId_ = nextId;
  existed_ = true;
  return OpenQErr::Ok;
}

OpenQErr OpenQuestions::add(const std::string& text, uint32_t nowHours, std::string* idOut) {
  std::string t = sanitizePrefText(text);
  utf8Repair(t);
  if (t.empty()) return OpenQErr::Empty;
  if (t.size() > kOpenQItemMax) return OpenQErr::TooLong;
  const int dup = indexOfKey(prefKey(t));
  if (dup >= 0) {
    if (idOut) *idOut = items_[(size_t)dup].idStr();
    return OpenQErr::Ok;
  }
  if (items_.size() >= kOpenQMaxItems) return OpenQErr::Full;
  OpenQEntry e;
  e.id = nextId_;
  e.createdHours = nowHours;
  e.text = t;
  std::vector<OpenQEntry> next = items_;
  next.push_back(e);
  const OpenQErr r = commit(next, nextId_ + 1);
  if (r == OpenQErr::Ok && idOut) *idOut = e.idStr();
  return r;
}

OpenQErr OpenQuestions::remove(uint32_t id) {
  const int i = indexOf(id);
  if (i < 0) return OpenQErr::NotFound;
  std::vector<OpenQEntry> next = items_;
  next.erase(next.begin() + i);
  return commit(next, nextId_);
}

OpenQErr OpenQuestions::clear() { return commit({}, nextId_); }

OpenQErr OpenQuestions::replaceAll(const std::vector<OpenQEntry>& next) {
  return commit(next, nextId_);
}

std::string OpenQuestions::renderBlock() const {
  std::string out;
  for (const OpenQEntry& e : items_) {
    if (!out.empty()) out += '\n';
    out += "- (" + e.idStr() + ") " + e.text;
    if (e.raised) out += " [raised: do not raise it again unless the owner returns to it]";
  }
  return out;
}

// ---- the deterministic raise-once observation ---------------------------------

// The subject words of an item: the shared subject-word helper (word_match.h),
// so the open list and the standing gate judge "same topic" the same way.
std::vector<std::string> openQKeywords(const std::string& text) {
  return words::subjectWords(text);
}

bool openQMentions(const std::string& itemText, const std::string& other) {
  const std::vector<std::string> kw = openQKeywords(itemText);
  if (kw.empty()) return false;
  const std::string hay = words::lowerAscii(sanitizePrefText(other));
  size_t shared = 0;
  for (const std::string& w : kw) {
    // whole-word containment
    size_t at = hay.find(w);
    while (at != std::string::npos) {
      const bool startOk = at == 0 || !words::isWordChar(hay[at - 1]);
      const bool endOk = at + w.size() >= hay.size() || !words::isWordChar(hay[at + w.size()]);
      if (startOk && endOk) { shared++; break; }
      at = hay.find(w, at + 1);
    }
  }
  const size_t need = (kw.size() + 1) / 2;   // half, rounded up, at least one
  return shared >= need;
}

// The owner's words that withdraw an item when they pair with its subject.
const char* const kOpenQDropCues[] = {
    "forget", "stop asking", "never ask", "don't ask", "dont ask", "do not ask",
    "no reason to ask", "no need to ask", "stop mentioning", "never mention",
    "don't mention", "dont mention", "do not mention", "drop it", "sorted",
    "resolved", "all done", "done with", "handled", "closed",
};

bool openQOwnerDrops(const std::string& itemText, const std::string& ownerText) {
  const std::string s = words::lowerAscii(sanitizePrefText(ownerText));
  for (const char* cue : kOpenQDropCues)
    if (s.find(cue) != std::string::npos) return openQMentions(itemText, ownerText);
  return false;
}

std::string observeOpenQ(OpenQuestions& q, const std::string& ownerText,
                         const std::string& reply) {
  if (q.entries().empty()) return std::string();
  std::vector<OpenQEntry> next = q.entries();
  std::string log;
  bool changed = false;
  // 1. the owner's message: a drop cue removes, a plain mention re-opens.
  for (size_t i = next.size(); i-- > 0;) {
    if (openQOwnerDrops(next[i].text, ownerText)) {
      log += "openq: removed " + next[i].idStr() + " (the owner withdrew it): " + next[i].text + "\n";
      next.erase(next.begin() + i);
      changed = true;
    } else if (next[i].raised && openQMentions(next[i].text, ownerText)) {
      next[i].raised = false;
      log += "openq: " + next[i].idStr() + " re-opened (the owner returned to it)\n";
      changed = true;
    }
  }
  // 2. the reply: a mention of the subject marks the item raised.
  for (OpenQEntry& e : next) {
    if (!e.raised && openQMentions(e.text, reply)) {
      e.raised = true;
      log += "openq: " + e.idStr() + " marked raised (the reply mentioned it)\n";
      changed = true;
    }
  }
  if (!changed) return std::string();
  return q.replaceAll(next) == OpenQErr::Ok ? log : std::string();
}

}  // namespace orch
}  // namespace nimbus
