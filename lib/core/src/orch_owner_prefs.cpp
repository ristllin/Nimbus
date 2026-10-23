#include "nimbus/orch/owner_prefs.h"

#include <algorithm>
#include <cstdlib>

#include "nimbus/mem_cap.h"   // utf8CapLen
#include "nimbus/utf8.h"      // utf8Repair

namespace nimbus {
namespace orch {

namespace {

constexpr const char kHeader[] = "#nimbus-prefs v1";

struct OriginSlug {
  PrefOrigin  o;
  const char* slug;
};
constexpr OriginSlug kOrigins[] = {
    {PrefOrigin::Chat, "chat"},     {PrefOrigin::Web, "web"},   {PrefOrigin::Command, "tgcmd"},
    {PrefOrigin::Seed, "seed"},     {PrefOrigin::Import, "import"},
};

PrefOrigin originFromSlug(const std::string& s) {
  for (const OriginSlug& e : kOrigins)
    if (s == e.slug) return e.o;
  return PrefOrigin::Chat;
}

std::string lowerAscii(const std::string& s) {
  std::string out = s;
  for (char& c : out)
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
  return out;
}

bool contains(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

bool containsAny(const std::string& hay, const char* const* needles, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (contains(hay, needles[i])) return true;
  return false;
}

bool isWordChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (unsigned char)c >= 0x80;
}

// `phrase` occurs in `s` (lowercase) as whole words: no letter or digit touches it.
bool hasPhrase(const std::string& s, const char* phrase) {
  const std::string p = phrase;
  for (size_t at = s.find(p); at != std::string::npos; at = s.find(p, at + 1)) {
    const bool startOk = at == 0 || !isWordChar(s[at - 1]);
    const size_t end = at + p.size();
    const bool endOk = end >= s.size() || !isWordChar(s[end]);
    if (startOk && endOk) return true;
  }
  return false;
}

bool hasAnyPhrase(const std::string& s, const char* const* phrases, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (hasPhrase(s, phrases[i])) return true;
  return false;
}

// Collapse runs of spaces to one and trim both ends.
std::string collapseSpaces(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (char c : in) {
    if (c == ' ' && (out.empty() || out.back() == ' ')) continue;
    out += c;
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

// Is the 3-byte sequence at s[i] a line or paragraph separator (U+2028, U+2029)?
bool isUnicodeLineBreak(const std::string& s, size_t i) {
  return i + 2 < s.size() && (unsigned char)s[i] == 0xE2 && (unsigned char)s[i + 1] == 0x80 &&
         ((unsigned char)s[i + 2] == 0xA8 || (unsigned char)s[i + 2] == 0xA9);
}

bool isNextLine(const std::string& s, size_t i) {   // U+0085
  return i + 1 < s.size() && (unsigned char)s[i] == 0xC2 && (unsigned char)s[i + 1] == 0x85;
}

// Cut to at most `cap` bytes at a word boundary, with "..." when anything was cut.
std::string cutAtWord(const std::string& t, size_t cap) {
  if (t.size() <= cap) return t;
  const size_t room = cap - 3;
  size_t keep = (size_t)utf8CapLen(t.c_str(), (int)t.size(), (int)room);
  const size_t sp = t.rfind(' ', keep);
  if (sp != std::string::npos && sp > room / 2) keep = sp;
  std::string out = t.substr(0, keep);
  while (!out.empty() && (out.back() == ' ' || out.back() == ',' || out.back() == ';')) out.pop_back();
  return out + "...";
}

bool allDigits(const std::string& s) {
  if (s.empty()) return false;
  for (char c : s)
    if (c < '0' || c > '9') return false;
  return true;
}

uint32_t toU32(const std::string& s) { return allDigits(s) ? (uint32_t)std::strtoul(s.c_str(), nullptr, 10) : 0; }

std::vector<std::string> splitTabs(const std::string& line, size_t maxFields) {
  std::vector<std::string> f;
  size_t pos = 0;
  while (f.size() + 1 < maxFields) {
    const size_t tab = line.find('\t', pos);
    if (tab == std::string::npos) break;
    f.push_back(line.substr(pos, tab - pos));
    pos = tab + 1;
  }
  f.push_back(line.substr(pos));   // the text keeps any further tabs (sanitized later)
  return f;
}

std::string filtersSlug(uint32_t filters) { return (filters & kPrefFilterNoDash) ? "nodash" : "-"; }

uint32_t filtersFromSlug(const std::string& s) {
  uint32_t out = 0;
  size_t pos = 0;
  while (pos <= s.size()) {
    size_t comma = s.find(',', pos);
    if (comma == std::string::npos) comma = s.size();
    if (s.compare(pos, comma - pos, "nodash") == 0) out |= kPrefFilterNoDash;
    pos = comma + 1;
  }
  return out;   // unknown names are ignored (a newer firmware's filter)
}

// One rule line of the file, or false when the line is not a well-formed rule.
bool parseRuleLine(const std::string& line, PrefEntry& e) {
  const std::vector<std::string> f = splitTabs(line, 6);
  if (f.size() != 6 || f[0].size() < 2 || (f[0][0] != 'p' && f[0][0] != 'P')) return false;
  e.id = toU32(f[0].substr(1));
  if (e.id == 0 || (f[1] != "A" && f[1] != "P")) return false;
  e.pending = f[1] == "P";
  e.createdHours = toU32(f[2]);
  e.origin = originFromSlug(f[3]);
  e.filters = filtersFromSlug(f[4]);
  std::string t = sanitizePrefText(f[5]);
  if (t.size() > kPrefItemMax) t.resize((size_t)utf8CapLen(t.c_str(), (int)t.size(), (int)kPrefItemMax));
  e.text = t;
  return !e.text.empty();
}

// The header's key=value flags.
void parseHeader(const std::string& line, bool& seeded, bool& listed, uint32_t& next) {
  size_t pos = 0;
  while (pos < line.size()) {
    size_t sp = line.find(' ', pos);
    if (sp == std::string::npos) sp = line.size();
    const std::string tok = line.substr(pos, sp - pos);
    if (tok == "seeded=1") seeded = true;
    if (tok == "listed=1") listed = true;
    if (tok.rfind("next=", 0) == 0) next = toU32(tok.substr(5));
    pos = sp + 1;
  }
}

std::string serializeState(const std::vector<PrefEntry>& items, bool seeded, bool listed,
                           uint32_t nextId) {
  std::string out = std::string(kHeader) + " seeded=" + (seeded ? "1" : "0") +
                    " listed=" + (listed ? "1" : "0") + " next=" + std::to_string(nextId) + "\n";
  for (const PrefEntry& e : items) {
    out += e.idStr() + "\t" + (e.pending ? "P" : "A") + "\t" + std::to_string(e.createdHours) + "\t" +
           prefOriginSlug(e.origin) + "\t" + filtersSlug(e.filters) + "\t" + e.text + "\n";
  }
  return out;
}

size_t activeBytesOf(const std::vector<PrefEntry>& v) {
  size_t n = 0;
  for (const PrefEntry& e : v)
    if (!e.pending) n += e.text.size();
  return n;
}

size_t pendingOf(const std::vector<PrefEntry>& v) {
  size_t n = 0;
  for (const PrefEntry& e : v)
    if (e.pending) n++;
  return n;
}

// Would this entry fit the parsed list's caps? (The tolerant parse keeps every
// intact line up to the caps, in file order.)
bool fitsCaps(const std::vector<PrefEntry>& kept, const PrefEntry& e) {
  if (kept.size() >= kPrefsMaxItems) return false;
  if (e.pending) return pendingOf(kept) < kPrefsPendingMax;
  return activeBytesOf(kept) + e.text.size() <= kPrefsMaxBytes;
}

bool knownIdOrKey(const std::vector<PrefEntry>& kept, const PrefEntry& e) {
  const std::string key = prefKey(e.text);
  for (const PrefEntry& k : kept)
    if (k.id == e.id || prefKey(k.text) == key) return true;
  return false;
}

}  // namespace

// ---- words -------------------------------------------------------------------

const char* prefOriginSlug(PrefOrigin o) {
  for (const OriginSlug& e : kOrigins)
    if (e.o == o) return e.slug;
  return "chat";
}

const char* prefErrWords(PrefErr e) {
  switch (e) {
    case PrefErr::Ok:            return "ok";
    case PrefErr::Empty:         return "the rule is empty";
    case PrefErr::TooLong:       return "keep the rule under 240 characters";
    case PrefErr::Full:          return "the standing rules are full (12 rules, 1,200 characters); remove one first";
    case PrefErr::PendingFull:   return "4 rules already wait for the owner's approval; approve or remove one first";
    case PrefErr::NotFound:      return "no standing rule with that id";
    case PrefErr::NotPending:    return "that rule is already active";
    case PrefErr::PersistFailed: return "could not save the standing rules (storage error); nothing changed";
  }
  return "error";
}

// ---- text helpers ------------------------------------------------------------

std::string sanitizePrefText(const std::string& in) {
  std::string s = in;
  utf8Repair(s);
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = (unsigned char)s[i];
    if (isUnicodeLineBreak(s, i)) { out += ' '; i += 3; continue; }
    if (isNextLine(s, i)) { out += ' '; i += 2; continue; }
    if (c == '\r' || c == '\n' || c == '\t') { out += ' '; i++; continue; }
    if (c < 0x20 || c == 0x7F) { i++; continue; }
    out += (char)c;
    i++;
  }
  return collapseSpaces(out);
}

std::string prefKey(const std::string& text) {
  std::string k = collapseSpaces(lowerAscii(sanitizePrefText(text)));
  while (!k.empty() && (k.back() == '.' || k.back() == '!' || k.back() == ' ')) k.pop_back();
  return k;
}

uint32_t filtersForRule(const std::string& text) {
  const std::string s = lowerAscii(text);
  static const char* const kDash[] = {"em dash", "em-dash", "emdash", "mdash", "m-dash",
                                      "long dash", "\xE2\x80\x94"};   // em-dash-ok: rule words
  if (!containsAny(s, kDash, sizeof(kDash) / sizeof(kDash[0]))) return 0;
  static const char* const kAllow[] = {"can use em",           "may use em",         "use em dashes again",
                                       "use em-dashes again",  "em dashes are fine", "em dashes are ok",
                                       "em dashes are allowed", "allow em",          "em dashes back"};
  if (containsAny(s, kAllow, sizeof(kAllow) / sizeof(kAllow[0]))) return 0;
  static const char* const kBan[] = {"never",   "no em",  "don't", "dont",    "do not",     "avoid",
                                     "stop",    "without", "not use", "ban", "remove",     "replace",
                                     "instead of", "no more", "drop", "no long dash", "not to use"};
  return containsAny(s, kBan, sizeof(kBan) / sizeof(kBan[0])) ? kPrefFilterNoDash : 0;
}

uint32_t parsePrefId(const std::string& raw) {
  std::string s = collapseSpaces(raw);
  if (!s.empty() && (s[0] == 'p' || s[0] == 'P')) s.erase(0, 1);
  return toU32(s);
}

// ---- the store ---------------------------------------------------------------

void OwnerPrefs::begin(PrefsStoreIO* io) {
  io_ = io;
  items_.clear();
  seeded_ = listed_ = false;
  nextId_ = 1;
  std::string blob;
  existed_ = io_ && io_->load(blob);
  if (existed_) parse(blob);
}

size_t OwnerPrefs::parse(const std::string& blob) {
  items_.clear();
  seeded_ = listed_ = false;
  uint32_t next = 1, maxId = 0;
  size_t pos = 0;
  while (pos < blob.size()) {
    size_t nl = blob.find('\n', pos);
    if (nl == std::string::npos) nl = blob.size();
    std::string line = blob.substr(pos, nl - pos);
    pos = nl + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind(kHeader, 0) == 0) { parseHeader(line, seeded_, listed_, next); continue; }
    PrefEntry e;
    if (!parseRuleLine(line, e) || knownIdOrKey(items_, e) || !fitsCaps(items_, e)) continue;
    maxId = std::max(maxId, e.id);
    items_.push_back(std::move(e));
  }
  nextId_ = std::max(next, maxId + 1);
  return items_.size();
}

std::string OwnerPrefs::serialize() const { return serializeState(items_, seeded_, listed_, nextId_); }

int OwnerPrefs::indexOf(uint32_t id) const {
  for (size_t i = 0; i < items_.size(); i++)
    if (items_[i].id == id) return (int)i;
  return -1;
}

int OwnerPrefs::indexOfKey(const std::string& key) const {
  for (size_t i = 0; i < items_.size(); i++)
    if (prefKey(items_[i].text) == key) return (int)i;
  return -1;
}

const PrefEntry* OwnerPrefs::find(uint32_t id) const {
  const int i = indexOf(id);
  return i < 0 ? nullptr : &items_[(size_t)i];
}

size_t OwnerPrefs::activeCount() const { return items_.size() - pendingOf(items_); }
size_t OwnerPrefs::pendingCount() const { return pendingOf(items_); }
size_t OwnerPrefs::activeBytes() const { return activeBytesOf(items_); }

PrefErr OwnerPrefs::checkRoom(const std::vector<PrefEntry>& next) const {
  if (next.size() > kPrefsMaxItems) return PrefErr::Full;
  if (pendingOf(next) > kPrefsPendingMax) return PrefErr::PendingFull;
  if (activeBytesOf(next) > kPrefsMaxBytes) return PrefErr::Full;
  return PrefErr::Ok;
}

PrefErr OwnerPrefs::commit(const std::vector<PrefEntry>& next, bool seeded, bool listed,
                           uint32_t nextId) {
  if (!io_ || !io_->saveChecked(serializeState(next, seeded, listed, nextId))) return PrefErr::PersistFailed;
  items_ = next;
  seeded_ = seeded;
  listed_ = listed;
  nextId_ = nextId;
  existed_ = true;
  return PrefErr::Ok;
}

PrefErr OwnerPrefs::add(const std::string& text, PrefOrigin origin, uint32_t nowHours, bool pending,
                        std::string* idOut) {
  const std::string t = sanitizePrefText(text);
  if (t.empty()) return PrefErr::Empty;
  if (t.size() > kPrefItemMax) return PrefErr::TooLong;
  const int dup = indexOfKey(prefKey(t));
  if (dup >= 0) {
    if (idOut) *idOut = items_[(size_t)dup].idStr();
    // The same words again: nothing new. A human adding a rule that waits for
    // approval IS the approval.
    if (!items_[(size_t)dup].pending || pending) return PrefErr::Ok;
    return approve(items_[(size_t)dup].id);
  }
  PrefEntry e;
  e.id = nextId_;
  e.pending = pending;
  e.createdHours = nowHours;
  e.origin = origin;
  e.filters = filtersForRule(t);
  e.text = t;
  std::vector<PrefEntry> next = items_;
  next.push_back(e);
  const PrefErr room = checkRoom(next);
  if (room != PrefErr::Ok) return room;
  const PrefErr r = commit(next, seeded_, listed_, nextId_ + 1);
  if (r == PrefErr::Ok && idOut) *idOut = e.idStr();
  return r;
}

PrefErr OwnerPrefs::replace(uint32_t id, const std::string& text, PrefOrigin origin, uint32_t nowHours) {
  const int i = indexOf(id);
  if (i < 0) return PrefErr::NotFound;
  const std::string t = sanitizePrefText(text);
  if (t.empty()) return PrefErr::Empty;
  if (t.size() > kPrefItemMax) return PrefErr::TooLong;
  std::vector<PrefEntry> next = items_;
  const int same = indexOfKey(prefKey(t));
  if (same >= 0 && same != i) {
    next.erase(next.begin() + i);   // it now says what another rule says: keep that one
  } else {
    PrefEntry& e = next[(size_t)i];
    e.text = t;
    e.filters = filtersForRule(t);
    e.origin = origin;
    e.createdHours = nowHours;
  }
  const PrefErr room = checkRoom(next);
  if (room != PrefErr::Ok) return room;
  return commit(next, seeded_, listed_, nextId_);
}

PrefErr OwnerPrefs::remove(uint32_t id) {
  const int i = indexOf(id);
  if (i < 0) return PrefErr::NotFound;
  std::vector<PrefEntry> next = items_;
  next.erase(next.begin() + i);
  return commit(next, seeded_, listed_, nextId_);
}

PrefErr OwnerPrefs::approve(uint32_t id) {
  const int i = indexOf(id);
  if (i < 0) return PrefErr::NotFound;
  if (!items_[(size_t)i].pending) return PrefErr::NotPending;
  std::vector<PrefEntry> next = items_;
  next[(size_t)i].pending = false;
  const PrefErr room = checkRoom(next);
  if (room != PrefErr::Ok) return room;
  return commit(next, seeded_, listed_, nextId_);
}

PrefErr OwnerPrefs::clear() { return commit({}, seeded_, listed_, nextId_); }

PrefErr OwnerPrefs::seed(const std::vector<std::string>& texts, uint32_t nowHours,
                         std::vector<std::string>* idsOut) {
  std::vector<PrefEntry> next = items_;
  uint32_t nextId = nextId_;
  std::vector<std::string> ids;
  for (const std::string& raw : texts) {
    PrefEntry e;
    e.text = cutAtWord(sanitizePrefText(raw), kPrefItemMax);
    if (e.text.empty() || e.text == "...") continue;
    e.id = nextId;
    e.createdHours = nowHours;
    e.origin = PrefOrigin::Seed;
    e.filters = filtersForRule(e.text);
    if (knownIdOrKey(next, e) || !fitsCaps(next, e)) continue;
    next.push_back(e);
    ids.push_back(e.idStr());
    nextId++;
  }
  const PrefErr r = commit(next, /*seeded=*/true, /*listed=*/ids.empty(), nextId);
  if (r == PrefErr::Ok && idsOut) *idsOut = ids;
  return r;
}

PrefErr OwnerPrefs::markListed() { return commit(items_, seeded_, true, nextId_); }

std::string OwnerPrefs::renderBlock() const {
  std::string out;
  size_t textBytes = 0;
  for (const PrefEntry& e : items_) {
    if (e.pending) continue;
    if (textBytes + e.text.size() > kPrefsMaxBytes) continue;   // whole lines only
    textBytes += e.text.size();
    if (!out.empty()) out += '\n';
    out += "- (" + e.idStr() + ") " + e.text;
  }
  return out;
}

uint32_t OwnerPrefs::activeFilters() const {
  uint32_t f = 0;
  for (const PrefEntry& e : items_)
    if (!e.pending) f |= e.filters;
  return f;
}

// ---- who sees them, and the first-boot seed ------------------------------------

bool standingRulesApply(Role role) { return role == Role::Admin; }

namespace {

// A standing marker: the words that make a sentence a rule for every later time.
const char* const kStandingMarkers[] = {
    "always",          "never",         "from now on",     "every time",    "each time",
    "whenever",        "by default",    "default",         "defaults",      "unless",
    "otherwise",       "assume",        "at all times",    "going forward", "in every reply",
    "in all replies",  "stop using",    "no more",         "don't ever",    "do not ever",
    "standing rule",   "standing default",
};
// Behavior markers: about how the assistant replies, logs or reads the owner.
const char* const kStrongBehavior[] = {
    "reply",    "replies",   "respond",   "response", "responses", "answer",   "answers",
    "format",   "formatting", "call me",  "call him", "call her",  "address him", "address her",
    "address me", "refer to", "tone",     "concise",  "emoji",     "emojis",   "em dash",
    "em dashes", "em-dash",  "em-dashes", "dash",     "dashes",    "bullet",   "bullets",
    "bullet points", "language", "english", "assistant", "you",    "your",     "log",
    "logs",     "logging",   "logged",    "treat",    "interpret", "assume",   "count as",
    "weight",   "weights",   "portion",   "portions", "units",     "metric",   "imperial",
    "remind me", "notify me",
};
// Weak behavior markers: count only in a request or an imperative ("Never use
// exclamation marks"), because "Roy never uses Uber" is a fact about him.
const char* const kWeakBehavior[] = {"use", "uses", "using", "write", "writes", "writing",
                                     "send", "sends", "message", "messages", "speak", "voice"};
// A request verb: with an output-style marker it is a rule even without "always".
const char* const kRequestVerbs[] = {"prefers", "prefer", "wants", "want", "would like", "asked",
                                     "likes", "like"};
const char* const kOutputStyle[] = {"reply",   "replies", "response", "responses", "answers",
                                    "format",  "tone",    "language", "english",   "emoji",
                                    "emojis",  "em dash", "em dashes", "dashes",   "bullet",
                                    "bullets", "concise", "brief",    "short",     "call me",
                                    "call him", "units",  "metric",   "imperial"};

template <size_t N>
bool anyOf(const std::string& s, const char* const (&list)[N]) {
  return hasAnyPhrase(s, list, N);
}

bool startsWithStanding(const std::string& s) {
  static const char* const kLead[] = {"always ", "never ", "don't ", "do not ", "please ",
                                      "from now on", "stop ", "no more "};
  for (const char* p : kLead)
    if (s.rfind(p, 0) == 0) return true;
  return false;
}

// A pinned memory about how to read the owner's quantities ("meat weights are
// raw") is a standing default even without "always".
bool quantityConvention(const std::string& s) {
  static const char* const kState[] = {"raw", "cooked", "uncooked"};
  static const char* const kQty[] = {"weight", "weights", "grams", "gram", "portion", "portions"};
  return anyOf(s, kState) && anyOf(s, kQty);
}

bool isRule(const std::string& s, const std::string& dev) {
  const bool devNamed = !dev.empty() && hasPhrase(s, dev.c_str());
  if (anyOf(s, kStandingMarkers)) {
    if (anyOf(s, kStrongBehavior) || devNamed) return true;
    if (anyOf(s, kWeakBehavior) && (startsWithStanding(s) || anyOf(s, kRequestVerbs))) return true;
  }
  return anyOf(s, kRequestVerbs) && anyOf(s, kOutputStyle);
}

}  // namespace

bool looksLikeStandingRule(const std::string& text, const std::string& devName) {
  const std::string s = collapseSpaces(lowerAscii(sanitizePrefText(text)));
  return !s.empty() && isRule(s, lowerAscii(devName));
}

std::vector<std::string> pickSeedRules(const std::vector<SeedCandidate>& in, const std::string& devName) {
  std::vector<const SeedCandidate*> picked;
  for (const SeedCandidate& c : in) {
    const std::string s = lowerAscii(sanitizePrefText(c.text));
    if (looksLikeStandingRule(c.text, devName) || (c.pinned && quantityConvention(s)))
      picked.push_back(&c);
  }
  std::stable_sort(picked.begin(), picked.end(), [](const SeedCandidate* a, const SeedCandidate* b) {
    if (a->pinned != b->pinned) return a->pinned;
    if (a->importance != b->importance) return a->importance > b->importance;
    return a->createdHours > b->createdHours;
  });
  std::vector<std::string> out;
  std::vector<std::string> keys;
  for (const SeedCandidate* c : picked) {
    const std::string k = prefKey(c->text);
    if (k.empty() || std::find(keys.begin(), keys.end(), k) != keys.end()) continue;
    keys.push_back(k);
    out.push_back(c->text);
  }
  return out;
}

std::string seedListingText(const OwnerPrefs& p) {
  std::string lines;
  for (const PrefEntry& e : p.entries())
    if (!e.pending && e.origin == PrefOrigin::Seed) lines += "(" + e.idStr() + ") " + e.text + "\n";
  if (lines.empty()) return std::string();
  return "Standing rules found in earlier conversations. They now apply to every reply:\n" + lines +
         "To drop one, send /prefs rm and its id (for example /prefs rm p2), or use the device's "
         "web page. /prefs lists them all.";
}

}  // namespace orch
}  // namespace nimbus
