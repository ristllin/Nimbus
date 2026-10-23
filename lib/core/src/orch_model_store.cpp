// orch_model_store - the streamed /models reader and the per-provider model store
// (/data/models_<slug>.txt) behind the model list the prompt, the checks, the picker
// and GET /api/models read (lane F). Portable: no Arduino, no file system; the device
// layer (src/agent/store.cpp, provider_verify.cpp) does the I/O. Host-tested in
// test/test_model_catalog and test/test_model_selectable.
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "nimbus/orch/model_catalog.h"
#include "nimbus/orch/model_choices.h"   // choiceItems - the one CSV reader
#include "nimbus/orch/model_price_defaults.h"   // builtinModelPrice - the estimate per model
#include "nimbus/orch/router_route.h"           // routableSelector - a router row some upstream runs
#include "nimbus/mem_cap.h"                     // utf8CapLen - names cut on a code point

namespace nimbus {
namespace orch {

// ---- untrusted text ----------------------------------------------------------

bool cleanModelId(const std::string& id) {
  if (id.empty() || id.size() > 64) return false;
  for (unsigned char c : id) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '.' || c == '_' || c == ':' || c == '/' || c == '-';
    if (!ok) return false;
  }
  return true;
}

std::string cleanModelText(const std::string& s, size_t maxBytes) {
  std::string out;
  out.reserve(s.size() < maxBytes ? s.size() : maxBytes);
  for (unsigned char c : s)
    if (c >= 0x20 && c != 0x7f) out += (char)c;
  if (out.size() > maxBytes)
    out.resize((size_t)utf8CapLen(out.c_str(), (int)out.size(), (int)maxBytes));
  return out;
}

namespace {
// Days from 1970-01-01 to y-m-d (proleptic Gregorian, H. Hinnant's civil algorithm).
int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int64_t)era * 146097 + (int64_t)doe - 719468;
}
// `n` digits at `p` as an int, or -1.
int digitsAt(const char* p, int n) {
  int v = 0;
  for (int i = 0; i < n; ++i) {
    if (p[i] < '0' || p[i] > '9') return -1;
    v = v * 10 + (p[i] - '0');
  }
  return v;
}
}  // namespace

namespace {
// "HH:MM:SS" after a 'T' or ' ' at s[10] as seconds of the day; 0 when absent, -1 when
// present but unreadable.
int64_t timeOfDay(const char* s) {
  if ((s[10] != 'T' && s[10] != ' ') || std::strlen(s) < 19) return 0;
  const int hh = digitsAt(s + 11, 2), mm = digitsAt(s + 14, 2), ss = digitsAt(s + 17, 2);
  if (hh < 0 || hh > 23 || mm < 0 || mm > 59 || ss < 0 || ss > 60) return -1;
  return hh * 3600 + mm * 60 + ss;
}
}  // namespace

uint32_t parseIsoTime(const char* s) {
  if (!s || std::strlen(s) < 10 || s[4] != '-' || s[7] != '-') return 0;
  const int y = digitsAt(s, 4), mo = digitsAt(s + 5, 2), d = digitsAt(s + 8, 2);
  if (y < 1970 || y > 2105 || mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
  const int64_t tod = timeOfDay(s);
  if (tod < 0) return 0;
  const int64_t t = daysFromCivil(y, (unsigned)mo, (unsigned)d) * 86400 + tod;
  return (t > 0 && t <= (int64_t)UINT32_MAX) ? (uint32_t)t : 0;
}

std::string sameModelKey(const std::string& id) {
  std::string k = id;
  for (size_t i = 1; i + 1 < k.size(); ++i)
    if (k[i] == '.' && k[i - 1] >= '0' && k[i - 1] <= '9' && k[i + 1] >= '0' && k[i + 1] <= '9')
      k[i] = '-';
  return k;
}

bool neverChatFamily(const std::string& id) {
  static const char* const kNever[] = {"embed", "moderation", "audio", "realtime",
                                       "transcribe", "tts", "whisper", "image",
                                       "dall-e", "ocr", "voxtral", "davinci",
                                       "babbage", "instruct", "search-", "chatgpt",
                                       "research",   // deep-research models can't run chat turns
                                       "fim",        // fill-in-middle = code completion
                                       nullptr};
  for (int i = 0; kNever[i]; ++i)
    if (id.find(kNever[i]) != std::string::npos) return true;
  return false;
}

namespace {
constexpr size_t kInObjectChars = 15;   // libstdc++'s small-string buffer
void offInternal(std::string& s) {
  if (s.size() > kInObjectChars && s.capacity() < kPsramStringFloor) s.reserve(kPsramStringFloor);
}
}  // namespace

void keepOffInternalHeap(ModelInfo& m) {
  for (std::string* s : {&m.id, &m.name, &m.family, &m.line, &m.life, &m.upstream}) offInternal(*s);
}

void assignOffInternal(std::string& dst, const char* p, size_t n) {
  if (n > kInObjectChars && dst.capacity() < kPsramStringFloor) dst.reserve(kPsramStringFloor);
  dst.assign(p, n);
}

bool routerRowKept(const std::string& selector) {
  static const char kBatch[] = ":batch";
  const size_t n = sizeof(kBatch) - 1;
  if (selector.size() > n && selector.compare(selector.size() - n, n, kBatch) == 0) return false;
  return routableSelector(selector);
}

bool routerFlagshipSelector(const std::string& selector) {
  const RouterRoute r = resolveRouterRoute(selector);
  if (r.upstream == "openai" || r.upstream == "zai") return isFlagshipFamily(r.upstream, r.model);
  if (r.upstream == "anthropic") return r.model.rfind("claude-3", 0) != 0;
  return true;
}

// ---- the streamed body ---------------------------------------------------------

void ModelsBodyReader::feed(const char* p, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    const char c = p[i];
    ++bytes_;
    if (inJson_) {
      jsonChar(c);
      continue;
    }
    // Before the JSON: HTTP response headers (the status line is the caller's) end at
    // the first empty line; a body that starts with '{' has none.
    if (!headers_) {
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
      if (c == '{') {
        inJson_ = true;
        jsonChar(c);
        continue;
      }
      headers_ = true;
      lineEmpty_ = false;
      continue;
    }
    if (c == '\r') continue;
    if (c == '\n') {
      if (lineEmpty_) inJson_ = true;
      lineEmpty_ = true;
      continue;
    }
    lineEmpty_ = false;
  }
}

// A top-level value ended: keep the two the paging needs.
void ModelsBodyReader::topValueChar(char c) {
  (void)c;
  if (key_ == "has_more" && !valStr_) hasMore_ = (val_ == "true");
  if (key_ == "last_id" && valStr_ && cleanModelId(val_)) lastId_ = val_;
  inVal_ = valStr_ = afterColon_ = false;
  val_.clear();
}

// Inside one "data" element: copy it (bounded; an oversize element is skipped and
// counted) and track its nesting until it closes.
void ModelsBodyReader::captureChar(char c) {
  if (capturing_) {
    if (row_.size() < kModelRowMaxBytes) {
      row_ += c;
    } else {
      capturing_ = false;
      skipping_ = true;
      row_.clear();
      ++oversize_;
    }
  }
  if (inStr_) {
    if (esc_) esc_ = false;
    else if (c == '\\') esc_ = true;
    else if (c == '"') inStr_ = false;
    return;
  }
  if (c == '"') {
    inStr_ = true;
  } else if (c == '{' || c == '[') {
    ++depth_;
  } else if ((c == '}' || c == ']') && --depth_ == dataDepth_) {
    if (capturing_) fn_(ctx_, row_.data(), row_.size());
    row_.clear();
    capturing_ = skipping_ = false;
  }
}

// Inside a string outside any element: a top-level key or string value is kept (to
// 128 bytes); any other string is only walked past.
void ModelsBodyReader::stringChar(char c) {
  std::string* into = inKey_ ? &key_ : (inVal_ && valStr_ ? &val_ : nullptr);
  if (esc_) {
    esc_ = false;
  } else if (c == '\\') {
    esc_ = true;
    return;
  } else if (c == '"') {
    inStr_ = false;
    if (inKey_) inKey_ = expectKey_ = false;
    return;
  }
  if (into && into->size() < 128) *into += c;
}

// A separator of the top-level object; false for any other character.
bool ModelsBodyReader::topSeparator(char c) {
  switch (c) {
    case ':': afterColon_ = true; return true;
    case ',': topValueChar(c); expectKey_ = true; return true;
    case '}': topValueChar(c); depth_ = 0; closed_ = true; return true;
    case ' ': case '\t': case '\r': case '\n': return true;
    default: return false;
  }
}

// A character of the top-level object (depth 1): keys, the separators, the values the
// paging needs, and the opening of the "data" array.
void ModelsBodyReader::topChar(char c) {
  if (c == '"') {
    inStr_ = true;
    if (expectKey_) { inKey_ = true; key_.clear(); }
    else if (afterColon_ && !inVal_) { inVal_ = valStr_ = true; val_.clear(); }
    return;
  }
  if (topSeparator(c) || !afterColon_) return;
  if (c == '[' || c == '{') {
    ++depth_;
    if (c == '[' && key_ == "data") { inData_ = true; dataDepth_ = depth_; }
    afterColon_ = false;
  } else if (!valStr_ && val_.size() < 16) {
    inVal_ = true;
    val_ += c;
  }
}

// Below the top level, outside an element: an element of "data" opens a capture.
void ModelsBodyReader::nestedChar(char c) {
  if (c == '"') { inStr_ = true; return; }
  if (inData_ && depth_ == dataDepth_ && c == '{') {
    row_.assign(1, c);
    capturing_ = true;
    ++depth_;
    return;
  }
  if (c == '{' || c == '[') { ++depth_; return; }
  if (c != '}' && c != ']') return;
  if (inData_ && depth_ == dataDepth_) inData_ = false;
  --depth_;
}

void ModelsBodyReader::jsonChar(char c) {
  if (closed_) return;
  if (capturing_ || skipping_) return captureChar(c);
  if (inStr_) return stringChar(c);
  if (depth_ == 0) {
    if (c == '{' && !started_) {
      started_ = true;
      depth_ = 1;
      expectKey_ = true;
    }
    return;
  }
  if (depth_ == 1) return topChar(c);
  nestedChar(c);
}

// ---- the model store -------------------------------------------------------------

std::string modelStorePath(const char* slug) {
  if (!slug || !slug[0]) return {};
  size_t n = 0;
  for (; slug[n]; ++n) {
    const char c = slug[n];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) || n >= 10) return {};
  }
  return std::string("/data/models_") + slug + ".txt";
}

namespace {

void appendU32(std::string& out, uint32_t v) {
  char b[12];
  std::snprintf(b, sizeof(b), "%u", (unsigned)v);
  out += b;
}
void appendHex(std::string& out, unsigned v) {
  char b[8];
  std::snprintf(b, sizeof(b), "%x", v);
  out += b;
}
// A text field may hold no tab or newline (they split the store).
void appendField(std::string& out, const std::string& v) {
  for (char c : v) out += (c == '\t' || c == '\n' || c == '\r') ? ' ' : c;
}

void appendRow(std::string& out, const ModelInfo& m) {
  appendField(out, modelSelector(m));
  out += '\t';
  appendField(out, m.name);
  out += '\t';
  appendField(out, m.line);
  out += '\t';
  if (m.size) out += m.size;
  out += '\t';
  if (m.created) appendU32(out, m.created);
  out += '\t';
  if (m.ctxTokens) appendU32(out, m.ctxTokens);
  out += '\t';
  if (m.maxOutTokens) appendU32(out, m.maxOutTokens);
  out += '\t';
  appendField(out, m.life);
  out += '\t';
  if (m.retires) appendU32(out, m.retires);
  out += '\t';
  appendHex(out, m.roles);
  out += '\t';
  appendHex(out, m.caps);
  out += '\t';
  if (m.usable) out += 'u';
  if (m.probed) out += 'p';
  if (m.apiCaps) out += 'a';
  if (m.deprecated) out += 'd';
  out += '\t';
  if (m.priced) appendU32(out, m.priceIn);
  out += '\t';
  if (m.priced) appendU32(out, m.priceOut);
  out += '\n';
}

void appendHeader(std::string& out, const ModelStoreMeta& meta) {
  out += "#models v1 fetchedAt=";
  appendU32(out, meta.fetchedAt);
  out += " fw=";
  std::string fw = meta.fw.empty() ? std::string("unknown") : meta.fw;
  for (char& c : fw)
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') c = '_';
  out += fw;
  out += " total=";
  appendU32(out, meta.total);
  out += " more=";
  appendU32(out, meta.more);
  out += meta.cut ? " cut=1" : " cut=0";
  out += " src=";
  out += meta.src.empty() ? "api" : meta.src;
  if (!meta.choices.empty()) {
    out += " choices=";
    appendField(out, meta.choices);   // clean ids: no space, no tab
  }
  out += '\n';
}

uint32_t u32Of(const char* p, size_t n) {
  uint64_t v = 0;
  for (size_t i = 0; i < n; ++i) {
    if (p[i] < '0' || p[i] > '9') return 0;
    v = v * 10 + (uint64_t)(p[i] - '0');
    if (v > UINT32_MAX) return 0;
  }
  return (uint32_t)v;
}
unsigned hexOf(const char* p, size_t n) {
  unsigned v = 0;
  for (size_t i = 0; i < n && i < 4; ++i) {
    const char c = p[i];
    const int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
    if (d < 0) return 0;
    v = v * 16 + (unsigned)d;
  }
  return v;
}

}  // namespace

std::string serializeModelStore(const ModelStoreMeta& meta, const std::vector<ModelInfo>& rows) {
  std::string out;
  out.reserve(96 + rows.size() * 90);
  appendHeader(out, meta);
  for (const ModelInfo& m : rows) appendRow(out, m);
  return out;
}

bool parseModelStoreHeader(const std::string& text, ModelStoreMeta& meta) {
  meta = ModelStoreMeta{};
  if (text.rfind("#models v1", 0) != 0) return false;
  const size_t eol = text.find('\n');
  const std::string head = text.substr(0, eol == std::string::npos ? text.size() : eol);
  size_t p = 10;
  while (p < head.size()) {
    while (p < head.size() && head[p] == ' ') ++p;
    const size_t e = std::min(head.find(' ', p), head.size());
    const std::string kv = head.substr(p, e - p);
    p = e;
    const size_t eq = kv.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
    if (k == "fetchedAt") meta.fetchedAt = u32Of(v.data(), v.size());
    else if (k == "fw") meta.fw = v;
    else if (k == "total") meta.total = u32Of(v.data(), v.size());
    else if (k == "more") meta.more = u32Of(v.data(), v.size());
    else if (k == "cut") meta.cut = (v == "1");
    else if (k == "src") meta.src = v;
    else if (k == "choices") meta.choices = v;
  }
  return true;
}

namespace {
constexpr size_t kStoreFields = 14;

// The tab-separated fields of one line; returns how many (at most kStoreFields).
size_t splitFields(const char* line, size_t len, const char* f[], size_t fl[]) {
  size_t nf = 0, b = 0;
  for (size_t i = 0; i <= len && nf < kStoreFields; ++i) {
    if (i < len && line[i] != '\t') continue;
    f[nf] = line + b;
    fl[nf] = i - b;
    ++nf;
    b = i + 1;
  }
  return nf;
}

// The selector into id (+ upstream for a router row).
void applySelector(const std::string& provider, const char* sel, size_t n, ModelInfo& out) {
  const char* slash = provider == "cumulo" ? static_cast<const char*>(std::memchr(sel, '/', n)) : nullptr;
  if (slash && slash != sel) {
    assignOffInternal(out.upstream, sel, (size_t)(slash - sel));
    assignOffInternal(out.id, slash + 1, n - (size_t)(slash - sel) - 1);
  } else {
    out.upstream.clear();
    assignOffInternal(out.id, sel, n);
  }
}

// Every field of a reused row reset, its strings' capacity kept (a scan over the
// store allocates nothing after its first long string).
void resetRow(ModelInfo& out) {
  for (std::string* s : {&out.id, &out.name, &out.family, &out.line, &out.life, &out.upstream}) s->clear();
  out.roles = out.caps = 0;
  out.usable = true;
  out.probed = out.apiCaps = out.deprecated = out.priced = false;
  out.ctxTokens = out.maxOutTokens = out.created = out.retires = out.priceIn = out.priceOut = 0;
  out.size = 0;
}

void applyFlags(const char* p, size_t n, ModelInfo& out) {
  const std::string flags(p, n);
  out.usable = flags.find('u') != std::string::npos;
  out.probed = flags.find('p') != std::string::npos;
  out.apiCaps = flags.find('a') != std::string::npos;
  out.deprecated = flags.find('d') != std::string::npos;
}
}  // namespace

bool parseModelStoreLine(const std::string& provider, const char* line, size_t len, ModelInfo& out) {
  const char* f[kStoreFields];
  size_t fl[kStoreFields];
  if (splitFields(line, len, f, fl) < kStoreFields) return false;
  if (fl[0] < 2) return false;
  const std::string sel(f[0], fl[0]);   // at most 64 chars once clean: one short allocation
  if (!cleanModelId(sel)) return false;
  // A router row no upstream runs (an older build's list merged in, CUM-530) is never
  // read back: no list, picker or check can offer it.
  if (provider == "cumulo" && !routerRowKept(sel)) return false;
  resetRow(out);
  applySelector(provider, f[0], fl[0], out);
  assignOffInternal(out.name, f[1], fl[1]);
  assignOffInternal(out.line, f[2], fl[2]);
  out.size = fl[3] ? f[3][0] : 0;
  out.created = u32Of(f[4], fl[4]);
  out.ctxTokens = u32Of(f[5], fl[5]);
  out.maxOutTokens = u32Of(f[6], fl[6]);
  assignOffInternal(out.life, f[7], fl[7]);
  out.retires = u32Of(f[8], fl[8]);
  out.roles = (uint16_t)hexOf(f[9], fl[9]);
  out.caps = (uint16_t)hexOf(f[10], fl[10]);
  applyFlags(f[11], fl[11], out);
  out.priced = fl[12] > 0 || fl[13] > 0;
  out.priceIn = u32Of(f[12], fl[12]);
  out.priceOut = u32Of(f[13], fl[13]);
  out.family = modelFamily(out.upstream.empty() ? provider : out.upstream, out.id);
  offInternal(out.family);
  return true;
}

bool parseModelStore(const std::string& provider, const std::string& text, ModelStoreMeta& meta,
                     std::vector<ModelInfo>* rows) {
  if (!parseModelStoreHeader(text, meta)) return false;
  if (rows)
    forEachStoreRow(provider, text, [&](const ModelInfo& m) {
      rows->push_back(m);
      return true;
    });
  return true;
}

namespace {
// Row indexes grouped by model line (else family), groups in order of first appearance.
std::vector<std::vector<size_t>> lineGroups(const std::vector<ModelInfo>& rows) {
  std::vector<std::string> keys;
  std::vector<std::vector<size_t>> groups;
  for (size_t i = 0; i < rows.size(); ++i) {
    const std::string& k = !rows[i].line.empty() ? rows[i].line : rows[i].family;
    size_t g = 0;
    while (g < keys.size() && keys[g] != k) ++g;
    if (g == keys.size()) {
      keys.push_back(k);
      groups.emplace_back();
    }
    groups[g].push_back(i);
  }
  return groups;
}

// Which rows stay: each group's next row in turn (round-robin), within both bounds.
std::vector<bool> roundRobinKeep(const std::vector<std::vector<size_t>>& groups,
                                 const std::vector<size_t>& rowBytes, size_t maxRows, size_t maxBytes) {
  std::vector<bool> keep(rowBytes.size(), false);
  size_t kept = 0, bytes = 128;   // the header
  for (size_t round = 0, any = 1; any && kept < maxRows; ++round) {
    any = 0;
    for (const auto& g : groups) {
      if (round >= g.size() || kept >= maxRows) continue;
      any = 1;
      const size_t i = g[round];
      if (bytes + rowBytes[i] > maxBytes) continue;
      keep[i] = true;
      bytes += rowBytes[i];
      ++kept;
    }
  }
  return keep;
}
}  // namespace

size_t boundModelRows(std::vector<ModelInfo>& rows, size_t maxRows, size_t maxBytes) {
  std::vector<size_t> rowBytes(rows.size());
  size_t total = 128;   // the header
  for (size_t i = 0; i < rows.size(); ++i) {
    std::string one;
    appendRow(one, rows[i]);
    rowBytes[i] = one.size();
    total += one.size();
  }
  if (rows.size() <= maxRows && total <= maxBytes) return 0;
  // A trim never removes a whole line first (the Fable trim); input order is kept.
  const std::vector<bool> keep = roundRobinKeep(lineGroups(rows), rowBytes, maxRows, maxBytes);
  std::vector<ModelInfo> out;
  for (size_t i = 0; i < rows.size(); ++i)
    if (keep[i]) out.push_back(std::move(rows[i]));
  const size_t dropped = rows.size() - out.size();
  rows.swap(out);
  return dropped;
}

bool harvestStorable(const ModelHarvest& h, bool haveStored) {
  return !h.rows.empty() && !(h.cut && haveStored);
}

std::string harvestLogLine(const ModelHarvest& h, const ModelStoreMeta& meta, size_t bytes, int pages) {
  char b[256];
  std::snprintf(b, sizeof(b),
                "verify: %s models read: %u rows in %u B (%d page%s), kept %u of %u%s%s%s, "
                "%u unsafe, %u past shutdown",
                h.provider.c_str(), (unsigned)h.seen, (unsigned)bytes, pages, pages == 1 ? "" : "s",
                (unsigned)h.rows.size(), (unsigned)meta.total,
                meta.more ? " (bound reached)" : "", h.cut ? ", body cut short" : "",
                h.hasMore ? ", more pages not read" : "", (unsigned)(h.hostile + h.oversize),
                (unsigned)h.retired);
  return b;
}

namespace {
// The older build's JSON catalog rows the store can hold (an id it can keep; for the
// router, a row some upstream runs).
void legacyJsonRows(const std::string& provider, const std::string& mcatJson, std::vector<ModelInfo>& rows) {
  JsonDocument d;
  if (mcatJson.empty() || deserializeJson(d, mcatJson)) return;
  modelsFromJson(d.as<JsonArrayConst>(), rows);
  const bool router = provider == "cumulo";
  rows.erase(std::remove_if(rows.begin(), rows.end(),
                            [router](const ModelInfo& m) {
                              const std::string sel = modelSelector(m);
                              return !cleanModelId(sel) || (router && !routerRowKept(sel));
                            }),
             rows.end());
}

// Each id of the older short list (cleaned) into `shortList`, and a row for each one the
// rows lack. True when a row was added.
bool addShortListIds(const std::string& provider, const std::string& mchCsv, std::vector<ModelInfo>& rows,
                     std::string& shortList) {
  bool added = false;
  for (const std::string& id : choiceItems(mchCsv)) {
    if (!cleanModelId(id) || (provider == "cumulo" && !routerRowKept(id))) continue;
    shortList += (shortList.empty() ? "" : ",") + id;
    const bool have = std::any_of(rows.begin(), rows.end(),
                                  [&id](const ModelInfo& m) { return modelSelector(m) == id; });
    if (have) continue;
    rows.push_back(classifyCatalogEntry(provider, id));
    added = true;
  }
  return added;
}
}  // namespace

std::string mergeLegacyLists(const std::string& provider, const std::string& storeText,
                             const std::string& mcatJson, const std::string& mchCsv,
                             const std::string& fw) {
  ModelStoreMeta meta;
  std::vector<ModelInfo> rows;
  const bool haveStore = parseModelStore(provider, storeText, meta, &rows);
  if (!haveStore) {
    legacyJsonRows(provider, mcatJson, rows);
    meta.src = "legacy";
    meta.fw = fw;
  }
  std::string shortList;
  const bool added = addShortListIds(provider, mchCsv, rows, shortList) || (!haveStore && !rows.empty());
  if (!added) return std::string();
  if (meta.choices.empty()) meta.choices = shortList;   // the older build's short list, cleaned
  meta.total = (uint32_t)rows.size();
  return serializeModelStore(meta, rows);
}

// ---- what the store offers -------------------------------------------------------------

bool rowOffered(const ModelInfo& m, uint16_t role, uint32_t now) {
  const uint16_t want = role ? role : (uint16_t)(RoleOrchestrator | RoleSubAgent);
  if (!m.usable || !(m.roles & want)) return false;
  if (neverChatFamily(m.id)) return false;
  return !(now && m.retires && m.retires <= now);
}

namespace {
// The provider's own short-list filter (the old harvest's): which offered chat rows
// a direct provider's short list may hold, and which of those come first.
bool shortListed(const std::string& provider, const std::string& id) {
  if (provider == "openai")
    return isFlagshipFamily("openai", id) && id.find("-202") == std::string::npos;
  if (provider == "anthropic") return id.rfind("claude-", 0) == 0;
  if (provider == "zai") return id.rfind("glm", 0) == 0;
  return provider == "mistral";
}
bool preferredFirst(const std::string& provider, const std::string& id) {
  if (provider == "mistral")
    return id.find("large") != std::string::npos || id.find("medium") != std::string::npos ||
           id.find("small") != std::string::npos || id.find("magistral") != std::string::npos;
  if (provider == "zai") return isFlagshipFamily("zai", id);
  return true;
}
}  // namespace

std::string directChoices(const std::string& provider, const std::vector<ModelInfo>& rows,
                          uint32_t now) {
  std::vector<const ModelInfo*> c;
  for (const ModelInfo& m : rows)
    if (rowOffered(m, RoleOrchestrator, now) && !m.deprecated && shortListed(provider, m.id))
      c.push_back(&m);
  // Preferred families first, each newest first (an undated row after the dated ones,
  // in store order).
  std::stable_sort(c.begin(), c.end(), [&](const ModelInfo* a, const ModelInfo* b) {
    const bool pa = preferredFirst(provider, a->id), pb = preferredFirst(provider, b->id);
    if (pa != pb) return pa;
    return a->created > b->created;
  });
  std::string csv;
  for (size_t i = 0; i < c.size() && i < kDirectChoicesMax; ++i) {
    if (!csv.empty()) csv += ',';
    csv += c[i]->id;
  }
  return csv;
}

std::string isoDate(uint32_t t) {
  if (!t) return std::string();
  // Civil date from days since the epoch (H. Hinnant's algorithm, the inverse above).
  const int64_t z = (int64_t)(t / 86400) + 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  const int64_t y = (int64_t)yoe + era * 400 + (m <= 2);
  char b[16];
  std::snprintf(b, sizeof(b), "%04d-%02u-%02u", (int)y, m, d);
  return b;
}

std::string modelListNote(bool haveStore, const ModelStoreMeta& meta) {
  if (!haveStore) return "(built-in list, not fetched yet)";
  if (!meta.fetchedAt) return "(list date unknown, may miss newer models)";
  return "(list from " + isoDate(meta.fetchedAt) + ", may miss newer models)";
}

// ---- price per model ----------------------------------------------------------------------

ModelPrice modelPrice(const std::string& provider, const ModelInfo& m) {
  ModelPrice p;
  if (m.priced) {
    p.in = m.priceIn;
    p.out = m.priceOut;
    p.src = "router";
    return p;
  }
  const std::string key = modelSelector(m);
  if (const BuiltinPrice* row = builtinModelPrice(provider.c_str(), key.c_str())) {
    p.in = row->microIn;
    p.out = row->microOut;
    p.src = "builtin";
  }
  return p;
}

}  // namespace orch
}  // namespace nimbus
