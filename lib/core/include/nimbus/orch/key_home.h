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
// The same mistake in the Telegram or Tavily field sent a provider key to
// api.telegram.org or Tavily on every poll.
//
// Shapes, most specific first:
//   cumulo     cumulo_sk_...                 (the router mints cumulo_sk_<48 hex>)
//   anthropic  sk-ant-...
//   (unknown)  sk-or-...                     (OpenRouter: not a key this device uses)
//   openai     sk-proj-, sk-svcacct-, sk-admin-, or sk- + exactly 48 letters/digits;
//              any other sk-... (DeepSeek, proxies) only LOOKS like an OpenAI key
//   tavily     tvly-...
//   telegram   <5-12 digits>:<30+ of A-Z a-z 0-9 _ ->   (a BotFather bot token)
//   zai        <20-64 alnum>.<8-64 alnum>   (id.secret)
//   mistral    exactly 32 alnum
// keyHomeSure() says whether the home is certain: the published prefixes above and the
// bot token's digits:colon form. Z.ai, Mistral and the other sk- keys are shapes only:
// the page names them with a hedged "looks like" and never moves them on its own, and
// the device refuses them only in a slot with a fixed form of its own (Cumulo,
// Telegram, Tavily), never in another provider's slot, in case a format changes.
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

namespace keyhome_detail {
// Spaces around a key are ignored (the shape check trims them the same way).
inline std::string trimmed(const std::string& k) {
  const size_t b = k.find_first_not_of(' ');
  if (b == std::string::npos) return std::string();
  return k.substr(b, k.find_last_not_of(' ') - b + 1);
}
// An OpenAI key by its published prefix, or the legacy sk- plus exactly 48 alnum.
inline bool openAiSure(const std::string& k) {
  if (startsWith(k, "sk-proj-") || startsWith(k, "sk-svcacct-") || startsWith(k, "sk-admin-"))
    return true;
  return k.size() == 51 && alnumRun(k, 3, k.size());
}
}  // namespace keyhome_detail

// The provider slug a key's shape names ("cumulo", "anthropic", "openai", "tavily",
// "telegram", "zai", "mistral"), or "" when the shape is unknown.
inline const char* keyHomeFor(const std::string& key) {
  using namespace keyhome_detail;
  const std::string k = trimmed(key);
  if (k.empty()) return "";
  if (startsWith(k, "cumulo_sk_")) return "cumulo";
  if (startsWith(k, "sk-ant-")) return "anthropic";
  if (startsWith(k, "sk-or-")) return "";
  if (startsWith(k, "sk-")) return "openai";
  if (startsWith(k, "tvly-")) return "tavily";
  if (telegramShape(k)) return "telegram";
  if (zaiShape(k)) return "zai";
  if (k.size() == 32 && alnumRun(k, 0, k.size())) return "mistral";
  return "";
}

// Whether keyHomeFor(key) is certain (a published prefix or the bot token's form),
// not a shape alone.
inline bool keyHomeSure(const std::string& key) {
  using namespace keyhome_detail;
  const std::string home = keyHomeFor(key);
  if (home == "openai") return openAiSure(trimmed(key));
  return home == "cumulo" || home == "anthropic" || home == "tavily" || home == "telegram";
}

// The /api/orch field a key or token is saved in, and where the owner finds it.
struct KeyHomeSlot {
  const char* home;
  const char* field;
  const char* noun;    // "an Anthropic key"
  const char* where;   // where to save it instead
};
inline const KeyHomeSlot* keyHomeSlot(const std::string& home) {
  static const KeyHomeSlot kSlots[] = {
      {"cumulo", "cumuloKey", "a Cumulo Nimbus key", "under Cumulo Nimbus"},
      {"anthropic", "antKey", "an Anthropic key", "under Anthropic"},
      {"openai", "oaiKey", "an OpenAI key", "under OpenAI"},
      {"mistral", "mistKey", "a Mistral key", "under Mistral"},
      {"zai", "zaiKey", "a Z.ai key", "under Z.ai"},
      {"tavily", "tavKey", "a Tavily web search key", "under Assistant > Tools > Web search"},
      {"telegram", "tgToken", "a Telegram bot token", "under Assistant > Connectors > Telegram"},
  };
  for (const KeyHomeSlot& s : kSlots)
    if (home == s.home) return &s;
  return nullptr;
}

namespace keyhome_detail {
// The refusal for a slot with a fixed form of its own (Cumulo, Telegram, Tavily) when
// the value is not that form: where it seems to belong, else what the slot expects.
inline std::string notThisForm(const std::string& home, bool sure, const char* what,
                               const char* expect) {
  const KeyHomeSlot* slot = keyHomeSlot(home);
  if (slot)
    return std::string(sure ? "That is " : "That looks like ") + slot->noun + ", not " + what +
           ". Save it " + slot->where + ".";
  return std::string("That is not ") + what + ": " + expect;
}
}  // namespace keyhome_detail

// "" when `value` may be stored in `field`, else the owner-facing refusal naming where
// the key goes. Guards every provider key slot and the two other secret slots that
// send their value to a fixed host (the Telegram bot token, the Tavily key):
//  - a key whose home is certain is refused in any slot but its own;
//  - the Cumulo, Telegram and Tavily slots take only their own form (cumulo_sk_..., a
//    bot token, tvly-...), so a provider key never goes to the router, api.telegram.org
//    or Tavily;
//  - a shape-only guess (Mistral, Z.ai, an unfamiliar sk- key) is never refused in
//    another provider's slot.
// Other fields (the custom endpoint key takes anything) and empty values pass.
inline std::string misplacedKeyError(const std::string& field, const std::string& value) {
  using namespace keyhome_detail;
  static const char* const kGuarded[] = {"oaiKey",    "antKey",  "mistKey", "zaiKey",
                                         "cumuloKey", "tgToken", "tavKey"};
  bool guarded = false;
  for (const char* f : kGuarded) guarded = guarded || field == f;
  if (!guarded || trimmed(value).empty()) return std::string();
  const std::string home = keyHomeFor(value);
  const bool sure = keyHomeSure(value);
  const KeyHomeSlot* slot = keyHomeSlot(home);
  if (slot && sure && field != slot->field)
    return std::string("That is ") + slot->noun + ". Save it " + slot->where + ".";
  if (field == "cumuloKey" && home != "cumulo")
    return notThisForm(home, sure, "a Cumulo Nimbus key",
                       "those start with cumulo_sk_. Choose the provider your key comes from "
                       "and save it there.");
  if (field == "tgToken" && home != "telegram")
    return notThisForm(home, sure, "a Telegram bot token",
                       "a bot token looks like 123456789:AAH... Copy it from @BotFather.");
  if (field == "tavKey" && home != "tavily")
    return notThisForm(home, sure, "a Tavily key",
                       "those start with tvly-. Copy it from your Tavily account.");
  return std::string();
}

}  // namespace orch
}  // namespace nimbus
