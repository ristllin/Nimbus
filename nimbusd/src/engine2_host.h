#pragma once
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>

#include <dirent.h>

#include <map>
#include <set>
#include <vector>

#include "daemon_http.h"
#include "engine2_parity.h"   // lane E2-P-HOST2: the outside mark, scope files, the counters, the owner lines
#include "hosted_file_tools.h"
#include "hosted_fold.h"   // hostedTextIO
#include "hosted_loops.h"   // the schedule tool and its fires (lane E2-SCHED)
#include "hosted_notes.h"
#include "hosted_tx.h"
#include "hosted_web_search.h"   // web_search (lane E2-P-HOST1)
#include "nimbus/harness/plain/chat_binding.h"   // the device's binding (lane E2-P-HOST1)
#include "nimbus/harness/plain/chat_commands.h"   // /clear's generation
#include "nimbus/harness/plain/chat_photos.h"
#include "nimbus/harness/plain/chat_skills.h"
#include "nimbus/harness/plain/history.h"   // search and view (lane E2-SRCH)
#include "nimbus/harness/plain/schedule_binding.h"   // heldFileOn, heldPathFor, drainHeld (lane E2-SCHED)
#include "nimbus/harness/plain/texts.h"
#include "nimbus/harness/websearch.h"
#include "nimbus/orch/blob_store.h"
#include "nimbus/orch/calc_tool.h"
#include "nimbus/orch/episodic.h"
#include "nimbus/orch/media_label.h"   // validRef
#include "nimbus/orch/router_route.h"  // resolveRouterRoute
#include "nimbus/orch/spend_ceiling.h"   // kSpendFrozenError (lane E2-FIX7)
#include "nimbus/orch/turn_images.h"
#include "nimbus/orch/turn_spend.h"   // TurnSpend (lane E2-CMP)
#include "nimbus/orch/tx/old_engine.h"   // EpisodicRowSource, oldFoldOf (the history import)
#include "plain_rig.h"   // PlainTurn, settleEnding
#include "posix_files.h"
#include "posix_fs.h"   // fsutil::readFile
#include "posix_platform.h"

// engine2_host - the owner chat of a Virtual Nimbus on engine 2 (lane E2-HOST). Started once at
// boot when the instance's startup switch says engine 2 (NIMBUSD_ENGINE=2); engine 1 stays the
// default and is untouched when the switch is off.
//
// Lane E2-P-HOST1: the owner's conversation runs through the device's own binding
// (plain::ChatBinding, chat_binding.h), so what the device's engine 2 does a hosted owner turn does:
// the arena sized for the head, compaction, the suppression seam, the safety rails, the money guard,
// the crash resume at boot, the progress line, the earlier engine's conversation imported before the
// log opens, photos and skills. The transcript key is "owner" (<data>/mem/tx/owner/), or "owner_c<n>"
// after /clear (the number in <data>/mem/tx/owner.gen). The host gives the binding the daemon's
// platform, a libcurl transport of its own (the engine thread is the only caller), its one tool
// registry and these parts:
//   * compaction's control record <data>/mem/compact_ctl.txt (no v5.1.1 chatsum mirror);
//   * notes (hosted_notes.h): <data>/mem/notes/owner.md, the pinned block in every request; a tainted
//     turn's note change goes to the pending shelf (the rails);
//   * calc; the five file tools and delete_file behind more (hosted_file_tools.h) over the daemon's
//     one file store, skills/<id>.md read from the skill store;
//   * search and view over the turn's own conversation (lane E2-SRCH);
//   * web_search over the old engine's web.search handler (Tavily, else the provider's own), whose
//     results are outside content;
//   * the owner's approved skills from <data>/mem/skills/<id>/SKILL.md;
//   * photos (lane E2-P-PHOTO, folded in here): the set engine 1's channel offered under the turn's
//     text (hosted_photos.h offerTurn: each photo kept by its hash in Engine2Options::photoDir, its row
//     written, an album of up to ten whole) becomes the message's image parts, as on the device; a head
//     that cannot see gets each photo's one description (the set's caption walk) and a blind turn; an
//     earlier photo in view is read back from photoDir;
//   * the earlier engine's owner rows (Engine2Options::oldRows) and fold record for the import;
//   * lane E2-SCHED: `schedule` over the instance's routines table (hosted_loops.h, loops.json with
//     loops2.json beside it) when Engine2Options::loops is set, and its fires (fire(), the routines
//     tick's hook while engine 2 runs): a reminder's text with no model request, a routine isolated on
//     the sub-agent model through plain::runRoutine, its state and outbox in <data>/mem/routines/.
// Replies leave through the host's deliver function (the web chat ring and Telegram), to the
// chat the running turn came from.
// Lane E2-R8b: the head is the instance's head slot and model, read on every turn: a provider slot
// direct on its own key, or the Cumulo router on the instance's router key, whose selector names the
// upstream (Engine2Head). The summarizer rungs and routines run on the same route. Spend is booked to
// the head's provider in the ledger's own counts. A head no route serves is refused in one plain line.
// The monthly ceiling stops a running turn: no request leaves while paid calls are paused (CeilingGate).
namespace nimbusd {

struct Engine2Options {
  std::string dataDir;
  PosixFiles* files = nullptr;                       // the daemon's file store (engine 1's, shared)
  std::function<std::string()> head;                 // the head slot now (a provider slug, or "cumulo")
  std::function<std::string()> model;                // the head model now (a router selector on "cumulo")
  std::function<std::string()> summarizerModel;      // the compaction and routine model (the sub-agent pick)
  std::function<std::string()> key;                  // the head slot's key, read at render time
  std::string routerHost = "app.cumulo-nimbus.ai";   // lane E2-R8b: the Cumulo router's host
  std::function<int32_t(uint32_t)> utcOffsetMin;     // minutes east of UTC at an instant
  std::function<uint16_t()> dayStartMin;
  std::string timeZone;                              // IANA name for the system block ("" = none)
  nimbus::orch::tx::Profile profile = nimbus::orch::tx::kDefaultProfile;   // the owner's pinned profile
  // Lane E2-P-PHOTO: where the instance keeps each photo by its hash (hosted_photos.h, <hash>.bin), read
  // back while an earlier photo is still in view. "": an earlier photo renders as its line.
  std::string photoDir;
  uint32_t bodyCapLimit = 0;   // lane E2-FIX9: a test rig's smaller request body cap (NIMBUSD_E2_BODY_CAP); 0: none
  // A reply of the running turn, to the chat it came from (the web ring, Telegram).
  std::function<void(const std::string& chat, const std::string& text)> deliver;
  agent::HttpTransport* transport = nullptr;         // null: the host's own libcurl transport (tests script one)
  std::string chatKey = "owner";                     // /mem/tx/<chatKey>
  std::string chatId = "owner";                      // the engine's routing chat (the web chat id)
  HostedLoops* loops = nullptr;                      // lane E2-SCHED: the routines table; null: no schedule tool
  // Lane E2-CMP: every paid request for the instance's monthly ceiling: each turn's spend ("turn") and
  // each summarizer request of the compaction ("compact"). Null: not counted.
  std::function<void(const nimbus::orch::TurnSpend& spend, const std::string& tag)> spend;
  // Lane E2-FIX7: the instance's monthly ceiling is reached. Then no engine-2 path makes a paid request: a
  // person's turn on any channel gets kPausedLine, routines wait (reminders still go), and the idle step runs
  // no compaction. Null: never paused.
  std::function<bool()> paused;
  // Lane E2-P-HOST1. The earlier engine's rows of the owner's conversation (every owner chat, never a
  // member's), the store's forward query; null: no import. Its fold record of the owner chat.
  std::function<std::vector<nimbus::orch::EpisodicMessage>(const nimbus::orch::MsgQuery&, nimbus::orch::EpiQueryInfo&)>
      oldRows;
  std::function<nimbus::orch::ChatFold()> oldFold;
  // web_search: the old engine's handler deps (search unset: no web_search), and whether a search key is there
  // now (null: always).
  agent::websearch::WebSearchToolDeps webSearch;
  std::function<bool()> webSearchOpen;
  // A running turn's round signals on its chat: a typing keep-alive (keepAlive) or the one progress line (line).
  // Called for a Telegram chat only (the web chat shows its own spinner). Null: none.
  std::function<void(const std::string& chat, bool keepAlive, const std::string& line)> turnProgress;
  std::string deviceName;   // the plain crash notice's name ("": Nimbus)
  // Lane E2-P-HOST2.
  // The role of a chat (null: every chat is the owner's). Admin: the owner's conversation; anyone else gets
  // a conversation of their own (<data>/mem/tx/<chat>), on their role and principal, one resident at a time.
  std::function<nimbus::orch::Role(const std::string& chat)> roleOf;
  // A member's own earlier-engine rows (their chat only), for the import at that chat's first start; null: none.
  std::function<std::vector<nimbus::orch::EpisodicMessage>(const std::string& chat, const nimbus::orch::MsgQuery&,
                                                           nimbus::orch::EpiQueryInfo&)>
      memberRows;
  // A row of engine 1's day stream (the web chat's history and the way back to engine 1): the person's message
  // and the answer of each engine-2 turn, tagged e2 so engine 2's own import never takes them. Null: none.
  std::function<void(const std::string& chat, const char* role, const std::string& text, const std::string& tags)>
      captureRow;
  // search scope memories: the earlier engine's long-term memories, read only (its memory.search). Null: not valid.
  std::function<bool(const nimbus::orch::Principal& who, const std::string& query, std::string& out)> memories;
  agent::JobEngine* jobs = nullptr;   // sub-agents (agent_start, agent_list, agent_stop); null: none
  Engine2Stats* stats = nullptr;      // the counters /api/state reads; null: none
};

// The owner's lines of /clear and of a skill left out of the pinned block (owner-facing only).
inline constexpr char kClearAskLine[] =
    "Start a new conversation? The current one stays stored on this instance. Send /clear yes to confirm.";
inline constexpr char kClearDoneLine[] = "Started a new conversation. The earlier one stays stored on this instance.";
inline constexpr char kClearNotStoredLine[] =
    "The new conversation was not started: the volume did not store it. This conversation goes on; send /clear "
    "again later.";
inline constexpr char kNoStartLine[] = "The assistant could not start. Check the instance log and restart it.";
inline constexpr char kNoHeadLine[] =
    "This assistant cannot run on the chosen provider. Pick a provider with a key on Assistant > Providers and try "
    "again.";

// What a person's message gets while paid calls are paused (the web chat's own refusal, lane DEF).
inline constexpr char kPausedLine[] =
    "Paid AI calls are paused: this month's spend ceiling is reached. Raise it on Assistant > Usage, or send the "
    "/budget command from the pause message.";

// Lane E2-R8b: the head engine 2 runs on, from the instance's head slot and model, as the device's binding resolves
// it (plain::headSlotFor, lane E2-R8a): a provider slot goes direct to its host in its own request format;
// "cumulo" goes to the router, under the path of the upstream its selector names, in that upstream's format.
// Not ok: no route serves it (a custom slot, which the instance has no base for; an unknown name; no model).
struct Engine2Head {
  plain::HeadSlot slot;   // provider (the slot's name), model, dialect and endpoint
  std::string upstream;   // the provider the request reaches (its effort and photo rows)
  std::string bare;       // the model the upstream is asked for
  bool found = false;
  bool ok() const { return found; }
  bool operator==(const Engine2Head& o) const { return slot.provider == o.slot.provider && slot.model == o.slot.model; }
};
inline Engine2Head engine2Head(const std::string& slot, const std::string& model, const std::string& routerHost) {
  Engine2Head h;
  h.slot.provider = slot;
  h.slot.model = model;
  plain::SlotSettings st;
  st.routerHost = routerHost;
  plain::HeadSlot hs;
  h.found = plain::headSlotFor(st, slot, model, hs).ok();
  if (h.found) h.slot = hs;
  const bool routed = h.found && hs.endpoint.routed;
  const nimbus::orch::RouterRoute rr = nimbus::orch::resolveRouterRoute(model);
  h.upstream = routed ? rr.upstream : slot;
  h.bare = routed ? rr.model : model;
  return h;
}

// The one plain line a refused switch logs (and the daemon exits on).
inline std::string engine2HeadRefusal(const std::string& head, const std::string& model) {
  return "engine 2 has no route for this instance's head " + (head.empty() ? std::string("(none)") : head) +
         (model.empty() ? std::string() : " " + model) +
         ": set NIMBUSD_ORCH_HOST to a keyed provider (or the Cumulo router) or turn NIMBUSD_ENGINE off";
}

// Lane E2-R8b: the monthly ceiling stops a running turn as engine 1's does: while paid calls are paused no
// request of engine 2 leaves (the next one is refused here, before a byte is sent), and the person gets the
// pause line in place of the failure line.
class CeilingGate : public agent::HttpTransport {
 public:
  CeilingGate(std::function<agent::HttpTransport*()> inner, std::function<bool()> paused)
      : inner_(std::move(inner)), paused_(std::move(paused)) {}
  bool exec(const agent::HttpRequest& req, agent::HttpResponse& out, std::string& err) override {
    if (paused_ && paused_()) {
      ++refused_;
      out.status = 0;
      err = nimbus::orch::kSpendFrozenError;
      return false;
    }
    return inner_()->exec(req, out, err);
  }
  uint32_t refused() const { return refused_; }

 private:
  std::function<agent::HttpTransport*()> inner_;
  std::function<bool()> paused_;
  uint32_t refused_ = 0;
};

class Engine2Host {
 public:
  explicit Engine2Host(Engine2Options o)
      : o_(std::move(o)), platform_(makePosixPlatform(&mem_)), txFiles_(o_.dataDir), notes_(o_.dataDir + "/mem"),
        fileTools_(*o_.files, HostedFileTools::Deps{o_.utcOffsetMin,
                                                    [this](const std::string& id, std::string& out) {
                                                      return plain::skillBody(skillFiles(), id, out);
                                                    }}),
        routineFiles_(o_.dataDir, nimbus::orch::schedule::kStateDir),
        held_(plain::heldFileOn(routineFiles_, plain::heldPathFor(o_.chatId))), history_(historyDeps()) {
    // No retry waits while paid calls are paused: the gate refuses the retry too (lane E2-R8b).
    platform_.delayMs = [this, wait = platform_.delayMs](uint32_t ms) {
      if (!paused()) wait(ms);
    };
    notes_.attach(reg_);
    nimbus::orch::registerCalcTool(reg_);
    fileTools_.attach(reg_);
    history_.registerInto(reg_);
    registerHostedWebSearch(reg_, webInner_, o_.webSearch, o_.webSearchOpen);
    if (o_.loops) {
      o_.loops->attachSchedule(reg_);
      o_.loops->setStateFiles(&routineFiles_);
    }
    if (o_.jobs) {   // lane E2-P-HOST2: the device's agent tools over the instance's one JobEngine
      plain::AgentsDeps a;
      a.jobs = o_.jobs;
      a.chat = [this] { return curChat_; };
      a.dispatchNow = [this] { (void)o_.jobs->dispatchQueued(); };
      (void)plain::registerAgentTools(reg_, a);
    }
  }
  Engine2Host(const Engine2Host&) = delete;
  Engine2Host& operator=(const Engine2Host&) = delete;

  // Boots the owner's binding over the stored transcript of the conversation /clear last started
  // (the earlier engine's conversation imported first, a turn a restart cut resumed). "" on
  // success, else the one plain line that says why engine 2 cannot run.
  std::string start() {
    const Engine2Head head = headNow();
    if (!head.ok()) return engine2HeadRefusal(head.slot.provider, head.slot.model);
    head_ = head;
    binding_.reset();   // a reboot: the old arena goes back before the new one is taken
    std::string key;
    if (!conversationKey(o_.chatKey, key))
      return "engine 2 did not start: the number of the owner's conversation (mem/tx/" + o_.chatKey +
             ".gen) could not be read";
    std::unique_ptr<plain::ChatBinding> b(
        new plain::ChatBinding(chatConfig(key, o_.chatId, nimbus::orch::Role::Admin), bindingDeps(o_.chatId, true)));
    (void)held_.load();
    const plain::Status st = b->boot();
    if (b->migrationRan()) logImport(*b);
    if (!st.ok())
      return std::string("engine 2 did not start: ") + nimbus::orch::tx::errSlug(st.err) + "@" + (st.where ? st.where : "");
    binding_ = std::move(b);
    return std::string();
  }

  // One owner message from `chat` (the web chat or an owner's Telegram chat). Its photos, when it has any,
  // are the set engine 1's channel offered under `text` (NimbusdRig::offerPhotoTurn), taken here.
  // Logs every ending that is not a reply.
  // A head model the owner changed since boot boots the chat again over its stored transcript, so the
  // budget and the compaction marks are the new head's; a model engine 2 cannot run is said in one line.
  // Lane E2-P-HOST2: a chat whose person is not the owner (roleOf) runs in a conversation of its own.
  PlainTurn say(const std::string& chat, const std::string& text, const char* ch) {
    const nimbus::orch::Role role = roleOf(chat);
    if (role == nimbus::orch::Role::Admin && !ownerOnModel(chat)) {
      PlainTurn t;
      t.stated = "plain-rig:failed:head";
      return t;
    }
    if (paused()) {   // fail closed: no head or summarizer request past the ceiling, on any channel
      if (o_.deliver) o_.deliver(chat, kPausedLine);
      PlainTurn t;
      t.stated = "plain-rig:paused";
      return t;
    }
    std::string line = kNoStartLine;
    plain::ChatBinding* b = role == nimbus::orch::Role::Admin ? binding_.get() : memberFor(chat, role, line);
    if (!b) {
      PlainTurn t;
      t.stated = "plain-rig:not-booted";
      if (o_.deliver) o_.deliver(chat, line);
      return t;
    }
    plain::ChatMessage msg;
    msg.ch = ch;
    msg.tzMinutes = tzNow();
    // The device's photo path (orchestrator_plain.cpp): parts for every photo, and for a head that cannot
    // see, each one's description and the text without the "attached" words.
    photoSet_ = nimbus::orch::takeTurnImages(chat, text);
    msg.photosBlind = photoSet_ && !nimbus::orch::headTakesImages(head_.upstream, head_.bare);
    msg.text = plain::photoText(photoSet_.get(), head_.upstream, head_.bare, text);
    msg.parts = plain::photoParts(photoSet_.get(), head_.upstream, head_.bare);
    // The way back: the person's message in engine 1's day stream (a photo message has its row already,
    // written by the channel's offer), tagged e2 so engine 2's own import never takes it.
    if (!photoSet_ && o_.captureRow) o_.captureRow(chat, "user", text, plain::ownerRowTags(std::string(), true));
    PlainTurn t = runTurn(*b, chat, msg);
    dropPhotos();
    tellSkillsLeftOut(chat);
    return t;
  }

  // A head (slot or model) the owner changed since boot boots the owner's chat again; false (the owner told in
  // one line) when it could not start.
  bool ownerOnModel(const std::string& chat) {
    const Engine2Head now = headNow();
    if (now == head_) return true;
    const std::string why = start();
    if (why.empty()) return true;
    std::fprintf(stderr, "[nimbusd] engine2: %s\n", why.c_str());
    if (o_.deliver) o_.deliver(chat, now.ok() ? kNoStartLine : kNoHeadLine);
    return false;
  }

  // Lane E2-P-HOST2: one sub-agent's report as its own turn in the chat that started it (plain::deliverReports,
  // after the job pump, between turns). Busy, nothing stored, when no turn can run for that chat now.
  plain::Status reportTurn(const std::string& chat, const std::string& note) {
    const plain::Status busy = plain::Status::fail(nimbus::orch::tx::Err::Busy, "nimbusd::Engine2Host::reportTurn");
    const nimbus::orch::Role role = roleOf(chat);
    std::string line;
    plain::ChatBinding* b = paused() ? nullptr : role == nimbus::orch::Role::Admin ? binding_.get() : memberFor(chat, role, line);
    if (!b) return busy;
    plain::ChatMessage msg;
    msg.trigger = plain::Trigger::AgentResult;
    msg.text = note;
    msg.ch = chat == o_.chatId ? "web" : "tg";
    msg.tzMinutes = tzNow();
    const PlainTurn t = runTurn(*b, chat, msg);
    return t.status;
  }

  // The owner's /clear (the device's two steps): "yes" or "confirm" starts a new conversation under the
  // next free number; the stored one stays as it is. Returns the owner's one line.
  std::string clearCommand(const std::string& args) {
    std::string a = args;
    a.erase(0, a.find_first_not_of(" \t"));
    a.erase(a.find_last_not_of(" \t") + 1);
    for (char& ch : a) ch = char(std::tolower((unsigned char)ch));
    if (a != "yes" && a != "confirm") return kClearAskLine;
    // A binding whose turns are held only in RAM keeps them (ruling 81): only a history import that
    // cannot finish lets /clear store them as the start of the new conversation (E2-FIX3).
    const bool held = binding_ && binding_->unloaded() && !binding_->reload();
    if (held && !binding_->importStuck()) return kClearNotStoredLine;
    uint32_t now = 0, next = 0;
    const bool read = plain::readGeneration(txFiles_, o_.chatKey, now) != plain::GenRead::Unknown;
    if (!plain::freeGeneration(txFiles_, o_.chatKey, read ? now : 0, next) ||
        !plain::storeGeneration(txFiles_, o_.chatKey, next)) {
      std::fprintf(stderr, "[nimbusd] engine2: /clear refused: conversation number %u not stored\n", unsigned(next));
      return kClearNotStoredLine;
    }
    const std::string key = o_.chatKey + "_c" + std::to_string(next);
    if (held && !binding_->storeHeldAs(key).ok()) {
      // Still held: every later turn stores them into `key` first (ChatBinding::reload).
      std::fprintf(stderr, "[nimbusd] engine2: /clear: the turns held in RAM are not stored in %s yet\n", key.c_str());
      return kClearDoneLine;
    }
    const std::string why = start();
    std::fprintf(stderr, "[nimbusd] engine2: /clear: conversation %s%s%s\n", key.c_str(), why.empty() ? "" : ": ",
                 why.c_str());
    return why.empty() ? kClearDoneLine : kNoStartLine;
  }

  // The owner's /compact: queued on the binding's driver, answered by it on this chat.
  bool requestCompaction() { return binding_ && binding_->requestCompaction(); }
  // Lane E2-FIX7: the instance's ceiling is reached (no paid engine-2 request runs).
  bool paused() const { return o_.paused && o_.paused(); }
  // The answer to the owner's /compact: queued (or already queued), or the pause line while paused (no summary
  // runs past the ceiling, so none is promised).
  std::string compactCommand() {
    if (paused()) return kPausedLine;
    return requestCompaction() ? "Compacting this conversation. A message follows when it is done."
                               : "A compaction is already queued. A message follows when it is done.";
  }

  // One unit of idle work (the compaction) between turns, on the engine thread. True when it
  // did work (a paid call may have run).
  bool idle(bool ownerWaiting) {
    if (!running()) return false;
    // Lane E2-SCHED: a routine note held for quiet hours goes once they end (one note a step).
    if (o_.loops && plain::drainHeld(held_, binding_->ownerSink(), vclock::epoch(), tzNow(), quiet(), dayStart()))
      return true;
    if (paused()) return false;   // no summarizer request past the ceiling (an attempt in flight has finished)
    plain::IdleCtx ctx;
    ctx.now = vclock::epoch();
    ctx.nowMs = vclock::steadyMs();
    ctx.ownerWaiting = ownerWaiting;
    ctx.quietForS = lastTurnEnd_ && ctx.now > lastTurnEnd_ ? uint32_t(ctx.now - lastTurnEnd_) : 0;
    ctx.tzMinutes = tzNow();
    ctx.clockSet = true;
    plain::IdleOutcome out;
    const plain::Status st = binding_->engine()->idle(ctx, out);
    if (!st.ok())
      std::fprintf(stderr, "[nimbusd] engine2: idle step failed %s@%s\n", nimbus::orch::tx::errSlug(st.err),
                   st.where);
    if (out.worked)
      std::fprintf(stderr, "[nimbusd] engine2: compaction step (thru %llu)\n",
                   (unsigned long long)(binding_->engine()->head() ? binding_->engine()->head()->thru : 0));
    return out.worked;
  }

  // Lane E2-SCHED: the routines tick's fire hook while engine 2 runs (the engine thread, between
  // turns). The owner's entries deliver to the owner conversation, as the hosted owner chat is one; lane
  // E2-P-SUB2: an entry a member set runs in and posts to that member's chat (their binding), and waits
  // while that chat cannot run a turn.
  nimbus::orch::FireOutcome fire(const nimbus::orch::LoopFireRequest& req) {
    nimbus::orch::FireOutcome o;
    if (!o_.loops || !running()) {
      o.deferred = true;   // not started: tried again, not counted
      o.retryAfterSec = 60;
      return o;
    }
    if (paused() && o_.loops->firesAsRoutine(req.id)) {   // a routine waits while paused; a reminder still goes
      o.deferred = true;
      o.error = nimbus::orch::kSpendFrozenError;
      return o;
    }
    plain::ChatBinding* target = binding_.get();
    const nimbus::orch::Role role = roleOf(req.chatId);
    const bool member = !req.chatId.empty() && req.chatId != o_.chatId && role != nimbus::orch::Role::Admin;
    if (member) {
      std::string line;
      target = role == nimbus::orch::Role::Unknown ? nullptr : memberFor(req.chatId, role, line);
      if (!target) {
        o.deferred = true;   // the member's chat cannot run now (or is not approved): never the owner's
        o.retryAfterSec = 60;
        return o;
      }
    }
    nimbus::orch::schedule::FireBinding b;
    b.owner = target->ownerSink();
    b.tzMinutes = tzNow();
    b.dayStartMin = dayStart();
    b.files = &routineFiles_;
    b.grants = o_.loops->routineGrants();
    b.run = [this, target](const plain::RoutineFire& f, bool tainted, plain::FireReport& out) {
      plain::RoutineDeps r = target->routineDeps(target->config().summarizerModel);
      r.head.tainted = tainted;
      r.tzMinutes = tzNow();
      r.held = &held_;
      r.quiet = quiet();
      const plain::Status st = plain::runRoutine(r, f, out);
      if (o_.spend) o_.spend(plain::turnSpendOf(out.outcome, head_.slot.provider, r.head.model), "routine");   // like a turn
      return st;
    };
    nimbus::orch::LoopFireRequest owner = req;
    if (!member) owner.chatId = o_.chatId;
    o = o_.loops->fireE2(owner, b);
    std::fprintf(stderr, "[nimbusd] engine2: fire %s ok=%d deferred=%d answered=%d\n", req.id.c_str(), int(o.ok),
                 int(o.deferred), int(o.answered));
    return o;
  }

  bool running() const { return binding_ && binding_->engine(); }
  plain::ChatBinding* binding() { return binding_.get(); }
  const Engine2Head& head() const { return head_; }   // lane E2-R8b: the head the chats run on
  uint32_t ceilingRefusals() const { return gate_.refused(); }
  plain::ChatBinding* memberBinding() { return member_.get(); }   // lane E2-P-HOST2 (tests)
  // Lane E2-P-HOST2: /budget's view of engine 2 for `chat` in the owner's words ("" when no chat runs).
  std::string budgetViewText(const std::string& chat) {
    std::string line;
    const nimbus::orch::Role role = roleOf(chat);
    plain::ChatBinding* b = role == nimbus::orch::Role::Admin ? binding_.get() : memberFor(chat, role, line);
    plain::BudgetView v;
    if (!b || !b->budgetView(v).ok()) return std::string();
    char out[400];
    std::snprintf(out, sizeof(out),
                  "New engine, in tokens: system prompt %u, tools %u, fixed part %u. Summary %u (up to %u), "
                  "recent conversation %u (recent part %u). Compaction at %u, aim %u. Max rounds %u, hard time limit %u s.",
                  (unsigned)v.systemTokens, (unsigned)v.toolsTokens, (unsigned)v.fixedTokens, (unsigned)v.summaryTokens,
                  (unsigned)v.summaryBudgetTokens, (unsigned)v.recentTokens, (unsigned)v.recentPartTokens,
                  (unsigned)v.dueTokens, (unsigned)v.aimTokens, (unsigned)v.maxRounds, (unsigned)v.hardTimeS);
    return out;
  }
  nimbus::orch::ToolRegistry& registry() { return reg_; }
  HostedNotes& notes() { return notes_; }
  PosixTxFiles& txFiles() { return txFiles_; }

 private:
  agent::HttpTransport* transport() { return o_.transport ? o_.transport : &http_; }
  Engine2Head headNow() const {
    return engine2Head(o_.head ? o_.head() : std::string(), o_.model ? o_.model() : std::string(), o_.routerHost);
  }
  // The summarizer and routine model: the sub-agent pick on the head's slot (a router pick runs on its own
  // upstream's wire: the binding's slotDialect), else the head's model.
  std::string summarizerOn(const Engine2Head& h) const {
    const std::string pick = o_.summarizerModel ? o_.summarizerModel() : std::string();
    return pick.empty() ? h.slot.model : pick;
  }
  uint16_t dayStart() const { return o_.dayStartMin ? o_.dayStartMin() : plain::kDayStartDefaultMin; }
  std::string skillsDir() const { return o_.dataDir + "/mem/skills"; }   // engine 1's HostedSkills store

  // The transcript key of a conversation: the base key, or "<key>_c<n>" after /clear. False when the
  // number could not be read (never taken as generation 0).
  bool conversationKey(const std::string& base, std::string& key) {
    uint32_t g = 0;
    if (plain::readGeneration(txFiles_, base, g) == plain::GenRead::Unknown) return false;
    key = g ? base + "_c" + std::to_string(g) : base;
    return true;
  }

  nimbus::orch::Role roleOf(const std::string& chat) const {
    return o_.roleOf ? o_.roleOf(chat) : nimbus::orch::Role::Admin;
  }

  plain::ChatBindingConfig chatConfig(const std::string& key, const std::string& chat, nimbus::orch::Role role) const {
    plain::ChatBindingConfig c;
    c.chatKey = key;
    c.chatId = chat;
    plain::useSlot(head_.slot, c);   // a routed head's budget, price, spend and park key is the router's
    c.summarizerModel = summarizerOn(head_);
    c.role = role;
    c.timeZone = o_.timeZone;
    c.dayStartMin = dayStart();
    c.profile = o_.profile;
    return c;
  }

  // Lane E2-P-HOST2: a member's binding (the device's chatFor): the resident one when it serves this chat, role,
  // model and conversation, else booted over that chat's own transcript. A binding whose turns are held only in
  // RAM is never dropped with them (ruling 81): another chat is refused with the true cause meanwhile. Null, with
  // `line` the person's line, when no turn can run for the chat.
  plain::ChatBinding* memberFor(const std::string& chat, nimbus::orch::Role role, std::string& line) {
    if (!(headNow() == head_)) {   // a head change: the owner's chat boots on the new head first
      const std::string why = start();
      if (!why.empty()) {
        std::fprintf(stderr, "[nimbusd] engine2: %s\n", why.c_str());
        line = headNow().ok() ? kNoStartLine : kNoHeadLine;
        return nullptr;
      }
    }
    std::string key;
    const bool keyRead = conversationKey(chat, key);
    bool keep = false;
    plain::ChatBinding* resident = residentMember(chat, role, keyRead ? key : std::string(), line, keep);
    if (keep) return resident;
    line = kNoStartLine;
    if (!keyRead || !nimbus::orch::tx::chatKeyOk(key)) {
      std::fprintf(stderr, "[nimbusd] engine2: chat %s has no readable transcript key\n", chat.c_str());
      return nullptr;
    }
    std::unique_ptr<plain::ChatBinding> b(new plain::ChatBinding(chatConfig(key, chat, role), bindingDeps(chat, false)));
    const plain::Status st = b->boot();
    if (b->migrationRan()) logImport(*b);
    if (!st.ok()) {
      std::fprintf(stderr, "[nimbusd] engine2: chat %s did not start: %s@%s\n", chat.c_str(),
                   nimbus::orch::tx::errSlug(st.err), st.where ? st.where : "");
      return nullptr;
    }
    member_ = std::move(b);
    return member_.get();
  }

  // The resident member binding when it serves this chat (`keep`); a binding whose turns are held only in RAM
  // keeps its chat and refuses another (null, `line` the true cause, keep true); else it is let go.
  plain::ChatBinding* residentMember(const std::string& chat, nimbus::orch::Role role, const std::string& key,
                                     std::string& line, bool& keep) {
    keep = false;
    if (!member_) return nullptr;
    const plain::ChatBindingConfig& c = member_->config();
    const bool same = c.chatId == chat && c.role == role;
    keep = same && c.provider == head_.slot.provider && c.model == head_.slot.model && (key.empty() || c.chatKey == key);
    if (keep) return member_.get();
    if (member_->unloaded() && !member_->reload()) {
      keep = true;
      if (same) return member_.get();
      line = hostedOwnerLine(member_->elsewhereLine());
      return nullptr;
    }
    member_.reset();   // another chat, role or model: the arena goes back before the next one
    return nullptr;
  }

  // One turn of `b` in `chat` and what follows it: the parts sent, the answer's row in engine 1's day stream,
  // the counters and the log lines.
  PlainTurn runTurn(plain::ChatBinding& b, const std::string& chat, const plain::ChatMessage& msg) {
    curChat_ = chat;
    sent_.clear();
    unsent_.clear();
    gateSeen_ = gate_.refused();
    pausedTold_ = false;
    PlainTurn t;
    t.status = b.turn(msg, t.out);
    const bool stopped = gate_.refused() != gateSeen_ || pausedTold_;
    if (stopped && !pausedTold_) (void)deliverPart(kPausedLine);   // the ceiling stopped it and no part went yet
    curChat_.clear();
    t.sent.swap(sent_);
    t.unsent.swap(unsent_);
    settleEnding(t);
    lastTurnEnd_ = vclock::epoch();
    if (stopped) {
      t.stated = "plain-rig:paused";
      std::fprintf(stderr, "[nimbusd] engine2: the monthly ceiling stopped the turn (a request was not sent)\n");
    }
    captureEnding(chat, t, stopped);
    const nimbus::orch::tx::TurnEndFacts& e = t.out.end;
    if (o_.stats && t.out.headRequests) {
      const uint32_t in = e.promptTokens + e.cacheReadTokens + e.cacheWriteTokens;
      o_.stats->turns.fetch_add(1);
      o_.stats->lastIn.store(in);
      o_.stats->lastOut.store(e.outputTokens);
      o_.stats->sessIn.fetch_add(in);
      o_.stats->sessOut.fetch_add(e.outputTokens);
    }
    if (t.ending != PlainEnding::Reply)
      std::fprintf(stderr, "[nimbusd] engine2: turn ended %s: %s\n", plainEndingSlug(t.ending),
                   (t.stated == t.out.statusText ? t.out.statusStored : t.stated).c_str());   // no provider words in the log (E2-FIX11)
    if (t.out.parkHost) std::fprintf(stderr, "[nimbusd] engine2: the head's key or credit was refused\n");
    std::fprintf(stderr,
                 "[nimbusd] engine2: turn rounds=%u calls=%u requests=%u in=%u cacheR=%u cacheW=%u cacheW1h=%u out=%u "
                 "uUSD=%u photos=%u\n",
                 unsigned(t.out.rounds), unsigned(t.out.toolCalls), unsigned(t.out.headRequests), unsigned(e.promptTokens),
                 unsigned(e.cacheReadTokens), unsigned(e.cacheWriteTokens), unsigned(e.cacheWrite1hTokens),
                 unsigned(e.outputTokens), unsigned(e.microUsd), unsigned(msg.parts.size()));
    return t;
  }

  // The turn's ending in engine 1's day stream: the answer, else its status line (the pause line when the ceiling
  // stopped it).
  void captureEnding(const std::string& chat, const PlainTurn& t, bool stopped) {
    if (!o_.captureRow) return;
    if (stopped) o_.captureRow(chat, "system", kPausedLine, "e2");
    else if (t.out.delivered && !t.out.text.empty()) o_.captureRow(chat, "assistant", t.out.text, "e2");
    else if (!t.out.statusStored.empty()) o_.captureRow(chat, "system", t.out.statusStored, "e2");
  }

  // The binding's parts for `chat`: the owner's conversation (`owner`) or a member's.
  plain::ChatBindingDeps bindingDeps(const std::string& chat, bool owner) {
    plain::ChatBindingDeps d;
    d.platform = &platform_;
    d.transport = &gate_;   // lane E2-R8b: no request leaves while paid calls are paused
    d.files = &txFiles_;
    d.registry = &reg_;
    // A member's binding sends to that member's chat (E2-P-SUB2: its routine's output arrives outside its turns);
    // the owner's to the chat of the running turn.
    const std::string to = owner ? std::string() : chat;
    d.sendPart = [this, to](const std::string&, const std::string& part) { return deliverPart(part, to); };
    d.key = o_.key;
    d.pinnedNotes = [this](const nimbus::orch::Principal& who) { return notes_.pinnedBlock(who); };
    d.compactCtl = hostedTextIO(o_.dataDir + "/mem/compact_ctl.txt");
    d.log = [](const std::string& line) { std::fprintf(stderr, "[nimbusd] engine2: %s\n", line.c_str()); };
    d.recentFiles = [this](const nimbus::orch::Principal& who) { return fileTools_.recentFilesFor(who); };
    d.photo = [this](const std::string& ref, plain::PhotoBytes& out) { return photoFor(ref, out); };
    d.skills = [this](const nimbus::orch::Principal&) { return plain::chatSkillsOf(skillFiles()); };
    d.skillRefused = [this](const std::string& id, size_t bytes) {
      if (skillsTold_.insert(id).second) skillsLeftOut_.push_back({id, bytes});
    };
    d.usage = [this](const plain::TurnOutcome& out) {
      if (o_.spend) o_.spend(plain::turnSpendOf(out, head_.slot.provider, head_.slot.model), "turn");
    };
    d.compactUsage = [this](const std::string& provider, const std::string& model, const nimbus::orch::TokenUsage& u) {
      if (!o_.spend) return;
      nimbus::orch::TurnSpend s;
      s.add(provider.c_str(), model.c_str(), u);   // the binding gives it in the ledger's counts (E2-R8a)
      o_.spend(s, "compact");
    };
    // The progress line and the typing keep-alive go to the running turn's Telegram chat only.
    d.progress.send = [this](const std::string&, const std::string& text, int64_t& msgId) {
      msgId = 0;   // a new line each time: no edit on this path
      if (!telegramTurn()) return false;
      o_.turnProgress(curChat_, false, text);
      return true;
    };
    d.onProgress = [this](const plain::ProgressEvent&) {
      if (telegramTurn()) o_.turnProgress(curChat_, true, std::string());
    };
    if (owner && o_.oldRows) d.oldEngine = [this](nimbus::orch::tx::MigrateDeps& m) { return fillOld(m); };
    if (!owner && o_.memberRows)   // lane E2-P-HOST2: the member's own earlier rows, never another chat's
      d.oldEngine = [this, chat](nimbus::orch::tx::MigrateDeps& m) {
        memberOld_.reset(new nimbus::orch::tx::EpisodicRowSource(
            m.chatKey, [this, chat](const nimbus::orch::MsgQuery& q, nimbus::orch::EpiQueryInfo& info) {
              return o_.memberRows(chat, q, info);
            }));
        m.rows = memberOld_.get();
        m.tzMinutes = tzNow();
        return true;
      };
    d.railPending = notes_.railPending();
    d.deviceName = o_.deviceName;
    // The notes' origin and the schedule's call context on every call; search and view read the turn's
    // own conversation. Lane E2-P-HOST2: a call that read outside content (web_search; a read of inbox/ or
    // agents/, or of another person's file: the device's rule) taints the turn.
    plain::ToolDispatchFn inner = o_.loops ? o_.loops->scheduleDispatch(notes_.dispatch()) : notes_.dispatch();
    d.turnTool = [this, inner](const plain::ToolCall& c, const plain::TurnCtx& t, plain::ToolOutput& out) {
      if (history_.answer(c, t, out)) return true;
      out = inner(c, t);
      const nimbus::orch::Principal who =
          t.who ? *t.who : nimbus::orch::principalForRole(o_.chatId, nimbus::orch::Role::Admin);
      out.outside = hostedReadsOutside(*o_.files, c.name, c.argsJson, !out.isError, who);
      return true;
    };
    d.moreSetup = [this](const plain::TurnCtx& t, plain::TurnSetup& s) {
      // PRD section 3: thinking as today (adaptive, effort medium), as engine 1 sends: the output table's
      // head-round effort for the model, the same function the device's binding reads (E2-FIX14).
      s.head.effort = plain::headEffortFor(head_.upstream, head_.slot.endpoint.routed ? nimbus::orch::resolveRouterRoute(t.model).model : t.model);
      s.bodyCapLimit = o_.bodyCapLimit;
    };
    return d;
  }

  // One part of the running turn to the chat it came from (a boot's resume or a fire: the owner chat).
  bool deliverPart(const std::string& raw, const std::string& to = std::string()) {
    // Lane E2-R8b: the ceiling stopped the turn: the pause line in place of the failure line, and nothing after it.
    if (pausedTold_) return true;
    std::string text = raw;
    if (gate_.refused() != gateSeen_) {
      gateSeen_ = gate_.refused();
      pausedTold_ = true;
      text = kPausedLine;
    }
    const std::string part = hostedOwnerLine(text);   // lane E2-P-HOST2: the instance's words, not the card's
    if (!o_.deliver) {
      unsent_.push_back(part);
      return false;
    }
    o_.deliver(!to.empty() ? to : curChat_.empty() ? o_.chatId : curChat_, part);
    sent_.push_back(part);
    return true;
  }
  bool telegramTurn() const { return o_.turnProgress && !curChat_.empty() && curChat_ != o_.chatId; }

  // The skill store's files: <data>/mem/skills/<id>/SKILL.md (chatSkillsOf keeps the approved ones).
  std::vector<plain::SkillFile> skillFiles() const {
    std::vector<plain::SkillFile> out;
    DIR* d = opendir(skillsDir().c_str());
    if (!d) return out;
    while (struct dirent* e = readdir(d)) {
      plain::SkillFile f;
      f.id = e->d_name;
      if (!plain::chatSkillIdOk(f.id)) continue;
      if (fsutil::readFile(skillsDir() + "/" + f.id + "/SKILL.md", f.md)) out.push_back(std::move(f));
    }
    closedir(d);
    return out;
  }
  // A context skill the pinned block cannot hold whole, told to the owner once (it is listed instead).
  void tellSkillsLeftOut(const std::string& chat) {
    for (const auto& s : skillsLeftOut_) {
      std::fprintf(stderr, "[nimbusd] engine2: skill %s (%u B) left out of the pinned block\n", s.first.c_str(),
                   unsigned(s.second));
      if (o_.deliver)
        o_.deliver(chat, "Your skill " + s.first + " is too large to ride every message (" + std::to_string(s.second) +
                             " B; notes and skills share 4,096 B). It is listed among your skills instead, and read "
                             "when a message needs it.");
    }
    skillsLeftOut_.clear();
  }

  void dropPhotos() {
    photoSet_.reset();   // its release hook settles the photo rows and frees the bytes
    carried_.clear();
  }
  // The photo hook: this message's photos, else an earlier one in view, read back from the volume.
  plain::Status photoFor(const std::string& ref, plain::PhotoBytes& out) {
    namespace tx = nimbus::orch::tx;
    if (plain::photoBytesFrom(photoSet_.get(), ref, out).ok()) return plain::Status::success();
    out = plain::PhotoBytes{};
    const std::string hash = ref.size() > 1 && ref[0] == 'p' ? ref.substr(1) : std::string();
    if (o_.photoDir.empty() || !nimbus::orch::validRef(hash))
      return plain::Status::fail(tx::Err::NotFound, "nimbusd::Engine2Host::photoFor");
    auto it = carried_.find(hash);
    if (it == carried_.end()) {
      std::string bytes;
      if (!fsutil::readFile(nimbus::orch::blobPath(o_.photoDir, hash, "bin"), bytes) || bytes.empty())
        return plain::Status::fail(tx::Err::NotFound, "nimbusd::Engine2Host::photoFor");
      it = carried_.emplace(hash, std::move(bytes)).first;
    }
    const char* mime = nimbus::orch::sniffImageMime((const uint8_t*)it->second.data(), it->second.size());
    if (!*mime) return plain::Status::fail(tx::Err::NotFound, "nimbusd::Engine2Host::photoFor");
    out.bytes = (const uint8_t*)it->second.data();
    out.len = it->second.size();
    out.mime = mime;
    return plain::Status::success();
  }

  // The history import's deps from the earlier engine (ChatBindingDeps::oldEngine): its owner rows, its
  // fold record, and the old summary's export to the file store.
  bool fillOld(nimbus::orch::tx::MigrateDeps& m) {
    oldRows_.reset(new nimbus::orch::tx::EpisodicRowSource(m.chatKey, o_.oldRows));
    m.rows = oldRows_.get();
    if (o_.oldFold) m.fold = nimbus::orch::tx::oldFoldOf(o_.oldFold());
    m.tzMinutes = tzNow();
    m.exportText = [this](const std::string& path, const std::string& text) {
      const size_t slash = path.find('/');
      std::string err;
      return slash != std::string::npos && o_.files->put(path.substr(0, slash), path.substr(slash + 1), text, err);
    };
    return true;
  }
  static void logImport(const plain::ChatBinding& b) {
    const nimbus::orch::tx::MigrationOutcome& o = b.migration();
    std::fprintf(stderr, "[nimbusd] engine2: import %s@%s step=%s turns=%u imported=%u items=%u ms=%u\n",
                 nimbus::orch::tx::errSlug(b.migrationStatus().err),
                 b.migrationStatus().where ? b.migrationStatus().where : "", nimbus::orch::tx::migrationStepSlug(o.step),
                 unsigned(o.turns), unsigned(o.imported), unsigned(o.items), unsigned(b.migrationMs()));
  }

  // Lane E2-SRCH: the transcripts and the owner's unpinned notes; no earlier-engine rows.
  plain::HistoryDeps historyDeps() {
    plain::HistoryDeps d;
    d.files = &txFiles_;
    d.notes = [this](const nimbus::orch::Principal& who, std::vector<nimbus::orch::notes::Note>& out) {
      return notes_.unpinned(who, out);
    };
    d.tzAt = [this](uint64_t t) { return int16_t(o_.utcOffsetMin ? o_.utcOffsetMin(uint32_t(t)) : 0); };
    d.nowMs = [] { return uint32_t(vclock::steadyMs()); };
    // Lane E2-P-HOST2: the two scopes lane E2-P-MEM added (files: the store's text files; memories: the earlier
    // engine's long-term memories, read only).
    d.fileSearch = [this](const nimbus::orch::Principal& who, const std::string& q, plain::FileSearch& out) {
      return hostedFileSearch(*o_.files, who, q, out);
    };
    d.memories = o_.memories;
    return d;
  }
  int16_t tzNow() const { return int16_t(o_.utcOffsetMin ? o_.utcOffsetMin(vclock::epoch32()) : 0); }
  // The instance keeps no quiet-hours end of its own: the default end (09:00), as plain::QuietHours.
  plain::QuietHours quiet() const { return plain::QuietHours{}; }

  Engine2Options o_;
  CgroupMemory mem_;
  agent::Platform platform_;
  DaemonHttpTransport http_;
  PosixTxFiles txFiles_;
  HostedNotes notes_;
  HostedFileTools fileTools_;
  PosixTxFiles routineFiles_;   // <data>/mem/routines: routine state files and the outbox (lane E2-SCHED)
  plain::HeldStore held_;
  plain::HistoryTools history_;   // after txFiles_ and notes_, which it reads
  nimbus::orch::ToolRegistry reg_;        // the owner chat's tools (the binding dispatches it)
  nimbus::orch::ToolRegistry webInner_;   // the old engine's web.search, reached by web_search only
  std::unique_ptr<nimbus::orch::tx::EpisodicRowSource> oldRows_;   // the last import's row source
  std::unique_ptr<plain::ChatBinding> binding_;
  std::unique_ptr<nimbus::orch::tx::EpisodicRowSource> memberOld_;   // lane E2-P-HOST2: a member's import source
  std::unique_ptr<plain::ChatBinding> member_;   // lane E2-P-HOST2: the one resident member chat
  std::unique_ptr<nimbus::orch::TurnImages> photoSet_;   // the running turn's photos (E2-P-PHOTO)
  std::map<std::string, std::string> carried_;           // earlier photos read back this turn, by hash
  std::vector<std::pair<std::string, size_t>> skillsLeftOut_;
  std::set<std::string> skillsTold_;
  std::vector<std::string> sent_, unsent_;
  std::string curChat_;
  Engine2Head head_;          // the head the chats were booted on (lane E2-R8b)
  CeilingGate gate_{[this] { return transport(); }, [this] { return paused(); }};   // every engine-2 request (E2-R8b)
  uint32_t gateSeen_ = 0;     // the gate's refusals the person was told of
  bool pausedTold_ = false;   // the pause line went out in this turn
  uint64_t lastTurnEnd_ = 0;
};

}  // namespace nimbusd
