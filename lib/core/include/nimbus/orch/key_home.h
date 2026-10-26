#pragma once
#include <cctype>
#include <cstddef>
#include <string>

// key_home - which provider a pasted key belongs to, read from its published shape,
// and the refusal for a key posted into another provider's slot (QW5). Portable and
// host-tested (test/test_key_home, against test/support/fixtures/key_home_cases.json;
// the web page's keyHome() in include/web/ui_js.h is checked against the same table
// by tools/webui_harness/tests/89_onboarding_v5.spec.mjs).
//
// The setup wizard preselects Cumulo Nimbus. A direct OpenAI, Anthropic or Mistral key
// pasted there was stored in the Cumulo slot, checked against the router, and came
// back "Rejected - check the key": a valid key blamed, and a wrong key left stored.
//
// Shapes, most specific first:
//   cumulo     cumulo_sk_...                 (the router mints cumulo_sk_<48 hex>)
//   anthropic  sk-ant-...
//   openai     sk-...                        (sk-proj-, sk-svcacct-, legacy sk-)
//   tavily     tvly-...
//   telegram   <digits>:<30+ of A-Z a-z 0-9 _ ->   (a BotFather bot token)
//   zai        <20-64 alnum>.<8-64 alnum>   (id.secret)
//   mistral    exactly 32 alnum
// The first four are published prefixes and the bot token's digits:colon form is
// unmistakable, so those five are certain. Z.ai and Mistral are shapes only: the page
// moves such a key to its provider with a hedged "looks like", and the device never
// refuses a save on a shape alone, in case a provider changes its format.
namespace nimbus {
namespace orch {

namespace keyhome_detail {
inline bool startsWith(const std::string& s, const char* p) {
  return s.compare(0, std::char_traits<char>::length(p), p) == 0;
}
inline bool alnumRun(const std::string& s, size_t b, size_t e) {
  for (size_t i = b; i < e; ++i)
    if (!std::isalnum(static_cast<unsigned char>(s[i]))) return false;
  return true;
}
inline bool telegramShape(const std::string& k) {
  const size_t colon = k.find(':');
  if (colon < 5 || colon > 12) return false;   // npos is larger than 12
  for (size_t i = 0; i < colon; ++i)
    if (!std::isdigit(static_cast<unsigned char>(k[i]))) return false;
  if (k.size() - colon - 1 < 30) return false;
  for (size_t i = colon + 1; i < k.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(k[i]);
    if (!std::isalnum(c) && c != '_' && c != '-') return false;
  }
  return true;
}
inline bool zaiShape(const std::string& k) {
  const size_t dot = k.find('.');
  if (dot == std::string::npos || k.find('.', dot + 1) != std::string::npos) return false;
  const size_t tail = k.size() - dot - 1;
  return dot >= 20 && dot <= 64 && tail >= 8 && tail <= 64 && alnumRun(k, 0, dot) &&
         alnumRun(k, dot + 1, k.size());
}
}  // namespace keyhome_detail

// The provider slug a key's shape names ("cumulo", "anthropic", "openai", "tavily",
// "telegram", "zai", "mistral"), or "" when the shape is unknown. Spaces around the
// key are ignored (the shape check trims them the same way).
inline const char* keyHomeFor(std::string k) {
  using namespace keyhome_detail;
  const size_t b = k.find_first_not_of(' ');
  if (b == std::string::npos) return "";
  k = k.substr(b, k.find_last_not_of(' ') - b + 1);
  if (startsWith(k, "cumulo_sk_")) return "cumulo";
  if (startsWith(k, "sk-ant-")) return "anthropic";
  if (startsWith(k, "sk-")) return "openai";
  if (startsWith(k, "tvly-")) return "tavily";
  if (telegramShape(k)) return "telegram";
  if (zaiShape(k)) return "zai";
  if (k.size() == 32 && alnumRun(k, 0, k.size())) return "mistral";
  return "";
}

// The /api/orch field a provider's key is saved in, and where the owner finds it.
struct KeyHomeSlot {
  const char* home;
  const char* field;
  const char* noun;    // "an Anthropic key"
  const char* where;   // where to save it instead
  bool certain;        // read from a published prefix or form, not a shape alone
};
inline const KeyHomeSlot* keyHomeSlot(const std::string& home) {
  static const KeyHomeSlot kSlots[] = {
      {"cumulo", "cumuloKey", "a Cumulo Nimbus key", "under Cumulo Nimbus", true},
      {"anthropic", "antKey", "an Anthropic key", "under Anthropic", true},
      {"openai", "oaiKey", "an OpenAI key", "under OpenAI", true},
      {"mistral", "mistKey", "a Mistral key", "under Mistral", false},
      {"zai", "zaiKey", "a Z.ai key", "under Z.ai", false},
      {"tavily", "tavKey", "a Tavily web search key", "under Assistant > Tools > Web search", true},
      {"telegram", "tgToken", "a Telegram bot token", "under Assistant > Connectors > Telegram", true},
  };
  for (const KeyHomeSlot& s : kSlots)
    if (home == s.home) return &s;
  return nullptr;
}

// Whether `home` was read with certainty (see the table above), not a shape alone.
inline bool keyHomeCertain(const std::string& home) {
  const KeyHomeSlot* s = keyHomeSlot(home);
  return s && s->certain;
}

// "" when `value` may be stored in `field`, else the owner-facing refusal naming where
// the key goes. Refuses only on certainty: a published prefix that belongs to another
// slot, or a Cumulo slot value without the cumulo_sk_ prefix every router key has.
// Fields that are not provider key slots (and empty values) are never refused here.
inline std::string misplacedKeyError(const std::string& field, const std::string& value) {
  static const char* const kKeyFields[] = {"oaiKey", "antKey", "mistKey", "zaiKey", "cumuloKey"};
  bool keyField = false;
  for (const char* f : kKeyFields) keyField = keyField || field == f;
  if (!keyField || value.find_first_not_of(' ') == std::string::npos) return std::string();
  const std::string home = keyHomeFor(value);
  const KeyHomeSlot* slot = keyHomeSlot(home);
  if (slot && slot->certain && field != slot->field)
    return std::string("That is ") + slot->noun + ". Save it " + slot->where + ".";
  if (field == "cumuloKey" && home != "cumulo")
    return "That is not a Cumulo Nimbus key: those start with cumulo_sk_. Choose the provider "
           "your key comes from and save it there.";
  return std::string();
}

}  // namespace orch
}  // namespace nimbus
