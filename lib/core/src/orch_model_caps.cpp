#include "nimbus/orch/model_caps.h"

#include <cstring>

#include "nimbus/orch/model_caps_data.h"
#include "nimbus/orch/model_key.h"   // isVersionSuffix: the money-safe version rule

namespace nimbus {
namespace orch {
namespace caps {

namespace {

char lowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

// Does `model` start with `prefix`, ignoring ASCII case? Returns the prefix length or -1.
long prefixLen(const char* model, const char* prefix) {
  size_t i = 0;
  for (; prefix[i]; i++)
    if (!model[i] || lowerAscii(model[i]) != lowerAscii(prefix[i])) return -1;
  return (long)i;
}

// An exact row matches its id, or its id plus one version or snapshot suffix.
bool exactMatch(const char* model, const char* id) {
  const long n = prefixLen(model, id);
  if (n < 0) return false;
  return model[n] == 0 || isVersionSuffix(model + n);
}

const ProviderRow* providerOf(const CapsTable& t, Slot slot) {
  for (size_t i = 0; i < t.providerCount; i++)
    if (t.providers[i].slot == slot) return &t.providers[i];
  return nullptr;
}

// Exact first, then the longest prefix, then the slot's '*' row.
const ModelRow* matchRow(const CapsTable& t, Slot slot, const char* model) {
  const ModelRow* any = nullptr;
  const ModelRow* best = nullptr;
  long bestLen = -1;
  for (size_t i = 0; i < t.modelCount; i++) {
    const ModelRow& r = t.models[i];
    if (r.slot != slot) continue;
    const char* id = poolStr(t, r.match);
    if (r.kind == MatchKind::Any) {
      any = &r;
    } else if (r.kind == MatchKind::Exact) {
      if (exactMatch(model, id)) return &r;
    } else {
      const long n = prefixLen(model, id);
      if (n > bestLen) { best = &r; bestLen = n; }
    }
  }
  return best ? best : any;
}

bool customSlot(Slot s) { return s == Slot::CustomChat || s == Slot::CustomMessages; }

uint32_t fnv1a(uint32_t h, const void* p, size_t n) {
  const unsigned char* b = static_cast<const unsigned char*>(p);
  for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
  return h;
}

// The memory's key: (slot, routed, lowercased model); 0 is kept for a free entry.
uint32_t memKey(Slot slot, const char* model, bool routed) {
  const unsigned char head[2] = {(unsigned char)slot, (unsigned char)(routed ? 1 : 0)};
  uint32_t h = fnv1a(2166136261u, head, 2);
  for (const char* c = model ? model : ""; *c; c++) {
    const char l = lowerAscii(*c);
    h = fnv1a(h, &l, 1);
  }
  return h ? h : 1;
}

}  // namespace

const CapsTable& builtinCaps() {
  static constexpr CapsTable t{kCapsModels,    sizeof(kCapsModels) / sizeof(kCapsModels[0]),
                               kCapsProviders, sizeof(kCapsProviders) / sizeof(kCapsProviders[0]),
                               kCapsMarkers,   sizeof(kCapsMarkers) / sizeof(kCapsMarkers[0]),
                               kCapsPool,      sizeof(kCapsPool)};
  return t;
}

const char* poolStr(const CapsTable& t, uint16_t off) { return off < t.poolBytes ? t.pool + off : ""; }

ModelRow narrowed(const ModelRow& r, uint16_t m) {
  ModelRow o = r;
  if (m & refused::kImage) {
    o.inputKinds &= (uint8_t)~input::kImage;
    o.flags &= (uint8_t) ~(flag::kImageInToolResult | flag::kFileRef);
  }
  if (m & refused::kEffort) o.effortSet = 0;
  if (m & refused::kThinking) o.thinking = Thinking::Off;
  if (m & refused::kStructured) {
    o.structured = Structured::None;
    o.summarizerStructured = Structured::None;
  }
  if (m & refused::kChoiceNone) o.toolChoice &= (uint8_t)~choice::kNone;
  if ((m & refused::kCacheBreakpoint) && o.cacheStyle == CacheStyle::OpenaiBreakpoints)
    o.cacheStyle = CacheStyle::OpenaiImplicit;
  if (m & refused::kReplay) o.replay = Replay::None;
  if (m & refused::kFileRef) o.flags &= (uint8_t)~flag::kFileRef;
  // A wire that requires the cap field keeps it: leaving it out would be refused every time.
  if ((m & refused::kCapField) && o.capMode != CapMode::Required) o.capMode = CapMode::Omit;
  return o;
}

ModelRow routedOverlay(const ModelRow& r, const ProviderRow& p) {
  ModelRow o = r;
  o.flags &= (uint8_t)~flag::kFileRef;   // the router refuses provider file ids
  if (o.imageMaxKiB > kRoutedImageMaxKiB) o.imageMaxKiB = kRoutedImageMaxKiB;
  if (o.imagesMax > p.routedImagesMax) o.imagesMax = p.routedImagesMax;
  return o;
}

Caps capsFrom(const CapsTable& t, Slot slot, const char* model, bool routed, const RefusedFields* refused) {
  Caps c;
  c.provider = providerOf(t, slot);
  c.matched = matchRow(t, slot, model ? model : "");
  if (!c.provider || !c.matched) return c;   // a table without this slot (a test's)
  c.known = c.matched->kind != MatchKind::Any;
  c.routed = routed && !customSlot(slot) && c.provider->routedImagesMax > 0;
  c.row = c.routed ? routedOverlay(*c.matched, *c.provider) : *c.matched;
  if (refused) c.row = narrowed(c.row, refused->fieldsFor(slot, model ? model : "", c.routed));
  return c;
}

Caps capsFor(Slot slot, const char* model, bool routed, const RefusedFields* refused) {
  return capsFrom(builtinCaps(), slot, model, routed, refused);
}

MarkerSpan markersOf(const CapsTable& t, const ProviderRow& p) {
  size_t first = t.markerCount, n = 0;
  for (size_t i = 0; i < t.markerCount; i++) {
    if (t.markers[i].markerSet != p.markerSet) continue;
    if (first == t.markerCount) first = i;
    n++;
  }
  return MarkerSpan{n ? t.markers + first : nullptr, n};
}

bool slotFromHost(const char* host, Slot* out) {
  static constexpr struct {
    const char* host;
    Slot slot;
  } kHosts[] = {{"anthropic", Slot::Anthropic}, {"openai", Slot::OpenAI}, {"mistral", Slot::Mistral},
                {"zai", Slot::Zai},             {"custom", Slot::CustomChat}};
  if (!host) return false;
  for (const auto& h : kHosts)
    if (std::strcmp(host, h.host) == 0) {
      *out = h.slot;
      return true;
    }
  return false;
}

const char* slotName(Slot s) {
  static constexpr const char* kNames[kSlotCount] = {"anthropic", "openai",      "mistral",
                                                     "zai",       "custom_chat", "custom_messages"};
  const size_t i = (size_t)s;
  return i < kSlotCount ? kNames[i] : "";
}

void RefusedFields::note(Slot slot, const char* model, bool routed, uint16_t fields) {
  const uint32_t key = memKey(slot, model, routed);
  for (Entry& e : e_)
    if (e.key == key) {
      e.mask |= fields;
      return;
    }
  for (Entry& e : e_)
    if (e.key == 0) {
      e = Entry{key, fields, 0};
      return;
    }
  e_[next_] = Entry{key, fields, 0};   // full: the oldest entry goes
  next_ = (uint8_t)((next_ + 1) % kEntries);
}

uint16_t RefusedFields::fieldsFor(Slot slot, const char* model, bool routed) const {
  const uint32_t key = memKey(slot, model, routed);
  for (const Entry& e : e_)
    if (e.key == key) return e.mask;
  return 0;
}

void RefusedFields::clear() {
  for (Entry& e : e_) e = Entry{};
  next_ = 0;
}

}  // namespace caps
}  // namespace orch
}  // namespace nimbus
