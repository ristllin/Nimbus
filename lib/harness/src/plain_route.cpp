// The route of a head and the pre-flight of every request (plain/route.h), lane E2-R4a.
#include "nimbus/harness/plain/route.h"

#include <cstring>

#include "nimbus/harness/output_caps.h"
#include "nimbus/harness/plain/exchange.h"   // headSlugOf
#include "nimbus/orch/router_route.h"

namespace agent {
namespace plain {

namespace {

namespace caps = nimbus::orch::caps;

// A required cap the tables leave at 0 (a model no table row names): what the request carries.
constexpr uint32_t kRequiredCapFloor = 4096;

const char* pathTail(Dialect d) {
  switch (d) {
    case Dialect::AnthropicMessages: return "/messages";
    case Dialect::OpenAIResponses: return "/responses";
    case Dialect::ChatCompletions:
    case Dialect::Flat: return "/chat/completions";
  }
  return "/chat/completions";
}

// The router's upstream for a selector that names none: the one the dialect speaks for.
const char* defaultUpstream(Dialect d) { return d == Dialect::AnthropicMessages ? "anthropic" : "openai"; }

// The table slot of a provider slug; a custom endpoint is Messages or Chat by its dialect.
bool slotOf(const std::string& provider, Dialect d, caps::Slot& out) {
  if (!caps::slotFromHost(provider.c_str(), &out)) return false;
  if (out == caps::Slot::CustomChat && d == Dialect::AnthropicMessages) out = caps::Slot::CustomMessages;
  return true;
}

// The effort:: bit of an effort value (0: a value no row lists).
uint8_t effortBit(const std::string& e) {
  static const char* const kNames[] = {"none", "minimal", "low", "medium", "high", "xhigh", "max"};
  for (uint8_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); ++i)
    if (e == kNames[i]) return (uint8_t)(1u << i);
  return 0;
}

// The table facts a route reads beside its rows: the path's base, auth and version header.
void tableFacts(const HeadEndpoint& head, const std::string& routerBase, Route& r) {
  const caps::CapsTable& t = caps::builtinCaps();
  const caps::ProviderRow* p = r.caps.provider;
  std::string base = head.basePath;
  if (base.empty()) base = r.routed ? routerBase : p ? caps::poolStr(t, p->basePath) : "";
  if (base.empty()) base = "/v1";
  r.path = base + pathTail(r.dialect);
  const bool messages = r.dialect == Dialect::AnthropicMessages;
  r.auth = r.routed ? caps::Auth::Bearer : p ? p->auth : messages ? caps::Auth::XApiKey : caps::Auth::Bearer;
  r.version = p ? caps::poolStr(t, p->versionHeader) : "";
  if (!*r.version && messages) r.version = "2023-06-01";   // the Messages API requires one
}

// The output cap by the row's cap mode, then the caller's ceiling.
uint16_t capRule(const Route& route, uint32_t capCeiling, RenderInput& in) {
  const caps::CapMode mode = route.caps.provider ? route.caps.row.capMode : route.dialect == Dialect::AnthropicMessages
                                                                                   ? caps::CapMode::Required
                                                                                   : caps::CapMode::SendAlways;
  if (mode == caps::CapMode::Omit || (mode == caps::CapMode::OmitDirectCeilRouted && !route.routed)) {
    in.maxTokens = 0;
    return 0;
  }
  if (mode == caps::CapMode::OmitDirectCeilRouted) {   // routed: the billing ceiling (a router clamp lowers it)
    in.maxTokens = in.maxTokens && in.maxTokens < kRoutedCeilingTokens ? in.maxTokens : kRoutedCeilingTokens;
  } else if (in.maxTokens == 0 && mode == caps::CapMode::Required) {
    const providers::RoundKind kind = in.toolChoiceNone ? providers::RoundKind::FinalRound : providers::RoundKind::ToolRound;
    in.maxTokens = providers::tableCap("anthropic", route.model, kind).maxTokens;
    if (in.maxTokens == 0) in.maxTokens = kRequiredCapFloor;
  }
  if (!capCeiling || !in.maxTokens || in.maxTokens <= capCeiling) return 0;
  in.maxTokens = capCeiling;
  return dropped::kCapClamped;
}

// Photos: none as pixels on a row without image input; no provider file id where the route takes none.
uint16_t photoRule(const caps::ModelRow& row, PreflightState& own, RenderInput& in) {
  if (!in.photo) return 0;
  if (!(row.inputKinds & caps::input::kImage)) {
    const bool was = own.turn.photosBlind;
    own.turn.photosBlind = true;
    return was ? 0 : dropped::kPhotos;
  }
  if (row.flags & caps::flag::kFileRef) return 0;
  own.photo = in.photo;
  in.photo = [&own](const std::string& ref, PhotoBytes& px) {
    const Status st = own.photo(ref, px);
    if (st.ok() && !px.fileId.empty()) {
      px.fileId.clear();
      own.lateDropped |= dropped::kFileRef;
    }
    return st;
  };
  return 0;
}

}  // namespace

Status resolveRoute(const HeadEndpoint& head, const TurnCtx& turn, Route& out, const caps::RefusedFields* refused) {
  out = Route{};
  out.head = &head;
  out.dialect = turn.dialect;
  out.model = turn.model;
  out.routed = head.routed;
  const std::string slug = headSlugOf(turn, head);
  if (slug.empty()) {   // nothing names the head: the wire's own facts, no row
    tableFacts(head, "", out);
    return Status::fail(Err::InvalidArgument, "plain::resolveRoute.head");
  }
  out.routed = out.routed || slug == "cumulo";
  out.key = nimbus::orch::tx::HeadKey{slug, turn.model, head.routed};
  out.provider = slug;
  std::string routerBase;
  if (slug == "cumulo") {   // a routed head is its upstream's, under the router's path
    const nimbus::orch::RouterRoute rr = nimbus::orch::resolveRouterRoute(turn.model, defaultUpstream(turn.dialect));
    out.provider = rr.upstream;
    out.model = rr.model;
    routerBase = rr.basePath;
  }
  out.capHost = out.provider == "custom" && out.dialect == Dialect::AnthropicMessages ? "anthropic" : out.provider;
  out.hasSlot = slotOf(out.provider, out.dialect, out.slot);
  if (out.hasSlot) out.caps = caps::capsFor(out.slot, out.model.c_str(), out.routed, refused);
  if (out.hasSlot && refused) out.refused = refused->fieldsFor(out.slot, out.model.c_str(), out.routed);
  tableFacts(head, routerBase, out);
  out.realBytesMax = out.routed ? kRoutedRealBytesMax : 0;
  nimbus::orch::tx::HeadBudget b;
  if (nimbus::orch::tx::budgetFor(out.key, nimbus::orch::tx::kDefaultProfile, b).ok()) out.bodyCap = b.bodyCap;
  return Status::success();
}

uint16_t preflight(const Route& route, uint32_t capCeiling, PreflightState& own, RenderInput& in) {
  own.turn = *in.turn;
  own.head = *in.head;
  own.lateDropped = 0;
  in.turn = &own.turn;
  in.head = &own.head;
  in.route = &route;
  uint16_t d = capRule(route, capCeiling, in);
  if (!route.caps.provider) return d;   // a host no table slot names: the wire's defaults, as before
  const caps::ModelRow& row = route.caps.row;
  if (!own.head.effort.empty() && !(row.effortSet & effortBit(own.head.effort))) {
    own.head.effort.clear();
    d |= dropped::kEffort;
  }
  if (in.answerSchema && *in.answerSchema && row.structured == caps::Structured::None) {
    in.answerSchema = nullptr;
    d |= dropped::kSchema;
  }
  // Tool choice none is a guard's last round: left out only where the table knows the model takes
  // none, or the model refused it this boot (an unknown model's 400 brings it here once).
  const bool noneKnown = route.caps.known || (route.refused & caps::refused::kChoiceNone);
  if (in.toolChoiceNone && noneKnown && !(row.toolChoice & caps::choice::kNone)) {
    in.toolChoiceNone = false;
    d |= dropped::kChoiceNone;
  }
  return d | photoRule(row, own, in);
}

}  // namespace plain
}  // namespace agent
