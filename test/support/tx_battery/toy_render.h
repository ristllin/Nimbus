#pragma once
#include <set>
#include <string>
#include <vector>

#include "../scripted_provider/request_view.h"
#include "items.h"
#include "props.h"

// tx_battery/toy_render.h - a reference renderer that follows the stub rule straight into a
// RequestView (lane RB-B0b), and the deliberately wrong renderers the battery must catch.
// It exists only for the counter-tests: a property that passes the toy and fails every
// mutation is shown to bite before any real renderer exists.
namespace txb {

enum class Mutation : uint8_t {
  None = 0,
  DropOwnerMessage,     // one owner message silently gone (the v5.1.1 history window)
  DropResult,           // a result dropped, its call kept
  DropCall,             // a call dropped, its result kept
  DuplicateResult,      // a result rendered twice
  LeakOutside,          // an outside result shown whole after its turn
  StubOpenBulk,         // this turn's bulk stubbed above the pointer
  StubWithoutHint,      // a stub with its view hint cut off
  OmitSilently,         // the oldest turn left out with no omission line
  OmitNewest,           // the omission reaches the newest owner turns
  HidePhoto,            // a photo neither shown nor named
};

namespace detail {
inline sp::Message& lastOf(sp::RequestView& v, const char* role) {
  if (v.messages.empty() || v.messages.back().role != role) {
    v.messages.push_back(sp::Message{});
    v.messages.back().role = role;
  }
  return v.messages.back();
}
inline std::string toyResultText(const tx::Item& r, const CoverInput& in, const std::set<uint64_t>& sup, Mutation m) {
  const Shown show = expectedShow(r, in, sup);
  if (m == Mutation::LeakOutside && r.cls == tx::ResultClass::Outside) return resultWhole(r, in);
  if (m == Mutation::StubOpenBulk && r.cls == tx::ResultClass::Bulk && show == Shown::Whole) return r.st;
  if (show == Shown::Whole) return resultWhole(r, in);
  if (m == Mutation::StubWithoutHint) return r.st.substr(0, r.st.find(" ... (full:"));
  return r.st;
}
inline void toyItem(sp::RequestView& v, const tx::Item& it, const CoverInput& in, const std::set<uint64_t>& sup,
                    Mutation m) {
  sp::Block b;
  switch (it.kind) {
    case tx::ItemKind::User: {
      sp::Message& u = lastOf(v, "user");
      u.blocks.push_back(sp::detail::textBlock(stampOf(it, in.dayStartMin) + " " + it.x));   // RB-REC5's form
      for (const tx::Part& p : it.parts) {
        sp::Block img;
        img.kind = sp::BlockKind::Image;
        img.ref = "file_" + p.ref;
        if (m != Mutation::HidePhoto) u.blocks.push_back(img);
      }
      return;
    }
    case tx::ItemKind::Assistant:
      lastOf(v, "assistant").blocks.push_back(sp::detail::textBlock(it.x));
      return;
    case tx::ItemKind::DeviceNote:
      lastOf(v, "user").blocks.push_back(sp::detail::textBlock(it.x));
      return;
    case tx::ItemKind::Call:
      if (m == Mutation::DropCall) return;
      b.kind = sp::BlockKind::ToolCall;
      b.id = it.id;
      b.name = it.n;
      b.args = it.args;
      lastOf(v, "assistant").blocks.push_back(b);
      return;
    case tx::ItemKind::Result:
      if (m == Mutation::DropResult) return;
      b.kind = sp::BlockKind::ToolResult;
      b.id = it.id;
      b.text = toyResultText(it, in, sup, m);
      b.isError = !it.ok;
      lastOf(v, "user").blocks.push_back(b);
      if (m == Mutation::DuplicateResult) lastOf(v, "user").blocks.push_back(b);
      return;
    default:
      return;
  }
}
}  // namespace detail

namespace detail {
inline std::set<uint32_t> toyOmitted(const CoverInput& in, Mutation m, uint32_t omitTurns) {
  std::vector<uint32_t> turns;
  for (const tx::Item& it : *in.items)
    if (it.kind == tx::ItemKind::User && it.q > in.thru) turns.push_back(it.turn);
  std::set<uint32_t> omitted;
  const size_t n = m == Mutation::OmitNewest ? (turns.size() > 2 ? turns.size() - 2 : 0) : omitTurns;
  for (size_t i = 0; i < n && i < turns.size(); ++i) omitted.insert(turns[i]);
  if (m == Mutation::OmitSilently && !turns.empty()) omitted.insert(turns[0]);
  return omitted;
}
// The mutations that hit one item only: the first item of their kind.
inline bool hitsOnce(Mutation m, tx::ItemKind& kind) {
  if (m == Mutation::DropOwnerMessage) kind = tx::ItemKind::User;
  else if (m == Mutation::DropCall) kind = tx::ItemKind::Call;
  else if (m == Mutation::DropResult || m == Mutation::DuplicateResult) kind = tx::ItemKind::Result;
  else return false;
  return true;
}
}  // namespace detail

// Render `in.items` after thru; `omitTurns` oldest whole turns go into one omission line.
// A one-item mutation applies to the first item of its kind; the others apply throughout.
inline sp::RequestView toyRender(const CoverInput& in, Mutation m = Mutation::None, uint32_t omitTurns = 0) {
  sp::RequestView v;
  v.parsed = true;
  v.systemBlocks = {"core", "notes", "digests"};
  const std::set<uint64_t> sup = detail::supersededSet(*in.items);
  const std::set<uint32_t> omitted = detail::toyOmitted(in, m, omitTurns);
  if (!omitted.empty() && m != Mutation::OmitSilently)
    detail::lastOf(v, "user").blocks.push_back(sp::detail::textBlock(
        "[" + std::to_string(omitted.size()) + " earlier turns, 15-16 Nov, not shown: search finds them]"));
  tx::ItemKind target = tx::ItemKind::User;
  const bool once = detail::hitsOnce(m, target);
  bool spent = false;
  for (const tx::Item& it : *in.items) {
    if (it.q <= in.thru || omitted.count(it.turn)) continue;
    const bool hit = once && !spent && it.kind == target;
    spent |= hit;
    if (hit && m == Mutation::DropOwnerMessage) continue;
    detail::toyItem(v, it, in, sup, once && !hit ? Mutation::None : m);
  }
  return v;
}

}  // namespace txb
