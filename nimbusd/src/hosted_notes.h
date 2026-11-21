#pragma once
#include <ArduinoJson.h>
#include <unistd.h>

#include <cerrno>

#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "nimbus/harness/plain/engine.h"    // ToolDispatchFn
#include "nimbus/harness/plain/rails.h"     // RailPendingFn, turnTainted
#include "nimbus/harness/plain/request.h"   // toolDecl, declSchema
#include "nimbus/orch/notes.h"
#include "nimbus/orch/tool_registry.h"
#include "posix_fs.h"

// hosted_notes - engine 2's notes on nimbusd (lane RB-N1; PRD section 4 "Notes"): the library's
// Notebook (lib/core orch_notes.cpp, the same rules and result texts as the device) over a tenant's
// volume, and the three bindings a hosted chat needs:
//
//   nimbusd::HostedNotes notes(tenantRoot);            // files under <tenantRoot>/notes/
//   b.dispatch = notes.dispatch(inner);                // records each call's turn (origin)
//   nimbusd::PlainRig rig(cfg, b);
//   notes.attach(rig.registry());                      // registers `note`, before boot
//   rd.pending = notes.railPending(inner);             // a tainted turn's `note` is kept
//   facts.pinnedNotes = notes.pinnedBlock(who);        // RB-I2's setup: the pinned block
//
// Files, one set per person (owner: "owner", a member: their chat id), written whole with the
// atomic writer (tmp, fsync, rename): <root>/notes/<person>.md (pinned), <person>.more.md
// (unpinned), <person>.pending.md (kept for approval). A tenant's root is its own volume, so two
// tenants never share a file. Every tenant gets the hosted caps (notes::kHostedCaps).
//
// The origin of a call is read from the running turn: dispatch() records it on the calling thread
// for the call's duration. A `note` call that reaches the registry with no turn recorded (a host
// that did not wire dispatch()) is kept for approval, never run: fail closed.
namespace nimbusd {

namespace notes = nimbus::orch::notes;

// The notes seam over a POSIX tree. available() is false only for a Principal with no person.
class PosixNotesIo : public notes::NotesIo {
 public:
  explicit PosixNotesIo(std::string root, notes::Caps caps = notes::kHostedCaps)
      : root_(std::move(root)), caps_(caps) {}

  bool available(const std::string& person) override { return !person.empty(); }
  bool read(const std::string& person, notes::Shelf s, std::string& out) override {
    out.clear();
    const std::string p = path(person, s);
    if (::access(p.c_str(), F_OK) != 0) return errno == ENOENT;   // missing: empty
    return fsutil::readFile(p, out);
  }
  bool write(const std::string& person, notes::Shelf s, const std::string& bytes) override {
    return fsutil::writeFileAtomic(path(person, s), bytes);
  }
  notes::Caps caps(const std::string&) override { return caps_; }

  std::string path(const std::string& person, notes::Shelf s) const {
    const char* ext = s == notes::Shelf::Pinned ? ".md" : s == notes::Shelf::More ? ".more.md" : ".pending.md";
    return root_ + "/notes/" + person + ext;
  }

 private:
  std::string root_;
  notes::Caps caps_;
};

// The origin of a call in `turn`: tainted when the turn read outside content (or is a sub-agent
// report), unattended when no person's message or the console started it, else attended.
inline notes::Origin originOf(const agent::plain::TurnCtx& turn) {
  if (agent::plain::turnTainted(turn)) return notes::Origin::Tainted;
  if (turn.trigger == agent::plain::Trigger::Owner || turn.trigger == agent::plain::Trigger::Console)
    return notes::Origin::Attended;
  return notes::Origin::Unattended;
}
inline const char* originSlug(notes::Origin o) {
  switch (o) {
    case notes::Origin::Attended: return "attended";
    case notes::Origin::Tainted: return "tainted";
    case notes::Origin::Unattended: return "unattended";
  }
  return "unattended";
}

class HostedNotes {
 public:
  explicit HostedNotes(std::string root) : io_(std::move(root)), book_(io_) {}
  HostedNotes(const HostedNotes&) = delete;
  HostedNotes& operator=(const HostedNotes&) = delete;

  // Register `note` (the manifest's description and schema) on a chat's registry, before boot.
  // The registry must not outlive this object.
  void attach(nimbus::orch::ToolRegistry& reg) {
    reg_ = &reg;
    const auto* decl = agent::plain::toolDecl("note");
    reg.add(
        "note", decl ? decl->description : "",
        [this](ArduinoJson::JsonObjectConst args, const nimbus::orch::Principal& who) {
          const notes::Origin o = current() ? *current() : notes::Origin::Unattended;
          std::lock_guard<std::mutex> g(mu_);
          return book_.call(args, who, o);
        },
        decl ? agent::plain::declSchema(*decl, false) : "{}");
  }

  // The PlainRigBindings::dispatch binding: records the turn's origin on this thread for the
  // call, then runs `inner` (null: registryDispatch, the rig's own registry dispatch).
  agent::plain::ToolDispatchFn dispatch(agent::plain::ToolDispatchFn inner = nullptr) {
    return [this, inner](const agent::plain::ToolCall& c, const agent::plain::TurnCtx& t) {
      struct Scope {
        const notes::Origin* prev;
        explicit Scope(const notes::Origin* now) : prev(current()) { current() = now; }
        ~Scope() { current() = prev; }
      };
      const notes::Origin o = originOf(t);
      Scope s(&o);
      return inner ? inner(c, t) : registryDispatch(c, t);
    };
  }

  // The registry's answer as the model sees it (as PlainRig's own dispatch): arguments that are
  // not a JSON object reach the handler as an empty object.
  agent::plain::ToolOutput registryDispatch(const agent::plain::ToolCall& c, const agent::plain::TurnCtx& t) const {
    agent::plain::ToolOutput o;
    o.id = c.id;
    o.name = c.name;
    if (!reg_ || !t.who) {
      o.isError = true;
      o.text = "no registry for " + c.name;
      return o;
    }
    JsonDocument args;
    (void)deserializeJson(args, c.argsJson, DeserializationOption::NestingLimit(64));
    const nimbus::orch::ToolResult r = reg_->dispatch(c.name, args.as<JsonObjectConst>(), *t.who);
    o.isError = !r.success;
    o.text = r.success ? r.output : r.error;
    return o;
  }

  // RailsDeps::pending: a `note` call of a tainted turn is kept on the person's pending shelf;
  // any other call goes to `inner` (null: not kept, so the rails refuse it).
  agent::plain::RailPendingFn railPending(agent::plain::RailPendingFn inner = nullptr) {
    return [this, inner](const agent::plain::TurnCtx& t, const agent::plain::ToolCall& c) {
      namespace tx = nimbus::orch::tx;
      if (c.name != "note") {
        return inner ? inner(t, c) : agent::plain::Status::fail(tx::Err::InvalidArgument, "hosted_notes.pending.other");
      }
      if (!t.who) return agent::plain::Status::fail(tx::Err::InvalidArgument, "hosted_notes.pending.who");
      JsonDocument args;
      (void)deserializeJson(args, c.argsJson, DeserializationOption::NestingLimit(64));
      nimbus::orch::ToolResult r;
      {
        std::lock_guard<std::mutex> g(mu_);
        r = book_.keep(args.as<JsonObjectConst>(), *t.who, notes::Origin::Tainted);
      }
      return r.success ? agent::plain::Status::success()
                       : agent::plain::Status::fail(tx::Err::InvalidArgument, "hosted_notes.pending.kept");
    };
  }

  // RequestFacts::pinnedNotes for this person ("" with none).
  std::string pinnedBlock(const nimbus::orch::Principal& who) {
    std::lock_guard<std::mutex> g(mu_);
    return book_.pinnedBlock(who);
  }
  // The person's kept calls and the owner's answers (notes.h), under the same lock as the tool.
  bool pending(const nimbus::orch::Principal& who, std::vector<notes::PendingNote>& out) {
    std::lock_guard<std::mutex> g(mu_);
    return book_.pending(who, out);
  }
  nimbus::orch::ToolResult approve(const nimbus::orch::Principal& who, const std::string& id) {
    std::lock_guard<std::mutex> g(mu_);
    return book_.approve(who, id);
  }
  nimbus::orch::ToolResult reject(const nimbus::orch::Principal& who, const std::string& id) {
    std::lock_guard<std::mutex> g(mu_);
    return book_.reject(who, id);
  }

 private:
  static const notes::Origin*& current() {
    static thread_local const notes::Origin* now = nullptr;
    return now;
  }

  PosixNotesIo io_;
  notes::Notebook book_;
  nimbus::orch::ToolRegistry* reg_ = nullptr;
  std::mutex mu_;
};

}  // namespace nimbusd
