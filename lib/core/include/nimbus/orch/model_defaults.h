#pragma once
#include <cstdint>
#include <string>

// model_defaults - the ONE role-default table: which model each role runs on when the
// owner has not picked one (owner requirement, 21 Oct 2026: defensive defaults for new
// users). Portable (NO Arduino), host-tested in test/test_model_defaults over every row.
//
// Roles:
//   Head        the assistant itself (owner turns, sub-agent synthesis);
//   Sub         sub-agents (spawned helpers);
//   Caption     the fallback image captioner;
//   Background  routines, wakeups, reminders and the nightly dream (scheduled turns
//               nobody is waiting on), which also run with a low effort cap.
//
// Ruling R7 (21 Oct): defaults are picked by PRICE CLASS from the device's model
// catalog. Each row names its class (the built-in price-table families it may come
// from, model_price_defaults.h) and a named fallback: the release-time pick from the
// recorded provider catalogs (test/support/fixtures/models_2026-10). The shape:
//   anthropic  Head claude-sonnet-5 (the sonnet class; never opus or fable),
//              Sub / Caption / Background claude-haiku-5-5;
//   openai     Head in the mini class, the rest in the nano class (the current
//              catalog prices these as gpt-5.6-luna and gpt-6-luna);
//   mistral    Head mistral-medium-latest, the rest mistral-small-latest;
//   zai        Head glm-5.3, the rest glm-5.3-flash;
//   cumulo     Head unchanged (the router's default; the cloud meters it), the rest
//              the router's cheapest tier.
// A custom endpoint has no row: its one model is whatever the owner configured.

namespace nimbus {
namespace orch {

enum class DefaultRole : uint8_t { Head = 0, Sub, Caption, Background };
inline constexpr int kDefaultRoleCount = 4;
const char* defaultRoleSlug(DefaultRole r);   // "head" | "sub" | "caption" | "background"

struct RoleDefault {
  const char* slot;       // provider slot
  DefaultRole   role;
  const char* tier;       // the price class in plain words ("standard", "cheapest")
  const char* families;   // the class: '|'-separated price-table families, each also
                          // holding its later versions ("claude-sonnet" -> "-5", "-5-5")
  const char* fallback;   // the named fallback (priced; in the recorded catalog)
};

inline constexpr RoleDefault kRoleDefaults[] = {
    {"anthropic", DefaultRole::Head, "standard", "claude-sonnet", "claude-sonnet-5"},
    {"anthropic", DefaultRole::Sub, "cheapest", "claude-haiku", "claude-haiku-5-5"},
    {"anthropic", DefaultRole::Caption, "cheapest", "claude-haiku", "claude-haiku-5-5"},
    {"anthropic", DefaultRole::Background, "cheapest", "claude-haiku", "claude-haiku-5-5"},
    {"openai", DefaultRole::Head, "mini", "gpt-5.6-luna", "gpt-5.6-luna"},
    {"openai", DefaultRole::Sub, "nano", "gpt-6-luna", "gpt-6-luna"},
    {"openai", DefaultRole::Caption, "nano", "gpt-6-luna", "gpt-6-luna"},
    {"openai", DefaultRole::Background, "nano", "gpt-6-luna", "gpt-6-luna"},
    {"mistral", DefaultRole::Head, "standard", "mistral-medium", "mistral-medium-latest"},
    {"mistral", DefaultRole::Sub, "cheapest", "mistral-small", "mistral-small-latest"},
    {"mistral", DefaultRole::Caption, "cheapest", "mistral-small", "mistral-small-latest"},
    {"mistral", DefaultRole::Background, "cheapest", "mistral-small", "mistral-small-latest"},
    {"zai", DefaultRole::Head, "standard", "glm-5.3|glm-5.2", "glm-5.3"},
    {"zai", DefaultRole::Sub, "cheapest", "glm-5.3-flash", "glm-5.3-flash"},
    {"zai", DefaultRole::Caption, "cheapest", "glm-5.3-flash", "glm-5.3-flash"},
    {"zai", DefaultRole::Background, "cheapest", "glm-5.3-flash", "glm-5.3-flash"},
    {"cumulo", DefaultRole::Head, "standard", "gpt-5.6", "gpt-5.6"},
    {"cumulo", DefaultRole::Sub, "cheapest", "gpt-5.6-luna|gpt-6-luna", "gpt-5.6-luna"},
    {"cumulo", DefaultRole::Caption, "cheapest", "gpt-5.6-luna|gpt-6-luna", "gpt-5.6-luna"},
    {"cumulo", DefaultRole::Background, "cheapest", "gpt-5.6-luna|gpt-6-luna", "gpt-5.6-luna"},
};
inline constexpr size_t kRoleDefaultCount = sizeof(kRoleDefaults) / sizeof(kRoleDefaults[0]);

// The row for (slot, role), or nullptr (custom, unknown slot).
const RoleDefault* roleDefaultRow(const char* slot, DefaultRole role);

// Whether `model` belongs to the row's price class: its built-in price row (the
// money-safe matching rule of model_price_defaults.h, a Cumulo id read through its
// upstream) is one of the row's families or a version of one. An unpriced id is in no
// class.
bool inPriceClass(const RoleDefault& row, const char* model);

// The default model for (slot, role) given the provider's catalog (`catalogCsv`, the
// effective choice list) and whether that list was harvested from the provider (a
// never-verified provider's cold-start list is the release-time pick itself, so it
// answers the named fallback). From a harvested list: the cheapest priced member of
// the class (input plus output price per 1M tokens), the named fallback winning a tie;
// the named fallback when no member is listed. "" for a slot with no row.
std::string roleDefaultModel(const char* slot, DefaultRole role, const std::string& catalogCsv,
                             bool harvested);

// ---- the turn in flight ---------------------------------------------------------
// The role the current task's turn runs under. Thread-local: the device runs one turn
// at a time on its turn task, and a web request answered on another task meanwhile
// reads the owner's configured models, never the background turn's.
enum class TurnRole : uint8_t { Owner = 0, Background };
TurnRole currentTurnRole();

// Marks the calling task's turn as background for its lifetime (scheduled turns).
class BackgroundTurnScope {
 public:
  BackgroundTurnScope();
  ~BackgroundTurnScope();
  BackgroundTurnScope(const BackgroundTurnScope&) = delete;
  BackgroundTurnScope& operator=(const BackgroundTurnScope&) = delete;

 private:
  TurnRole prev_;
};

}  // namespace orch
}  // namespace nimbus
