#include "nimbus/orch/claims.h"
#include "nimbus/orch/spawn_claim.h"

#include <cstdlib>
#include <cstring>

// The claim scanner. Read claims.h first: it states the rules (R1-R6) and why
// precision comes first. This file is the token matcher; the cue table below is
// the single source both the device and the property tests read.

namespace nimbus {
namespace orch {

namespace {

constexpr uint32_t fxBit(FxKind k) { return 1u << (unsigned)k; }

enum CueFlag : uint16_t {
  kSelf       = 1u << 0,   // counts only after the assistant, a passive, or at clause start
  kAnySubject = 1u << 1,   // no other-person / adjective filter (states, sources)
  kNeverDecay = 1u << 2,   // durable: backed by a pin or a permanent memory write
  kPin        = 1u << 3,   // changed: a real pin (an already pinned memory does not back it)
  kSpawnVerb  = 1u << 4,   // spawned: needs a sub-agent noun within the window
  kFigure     = 1u << 5,   // figure: every number in the span must be in a source
  kNoPassive  = 1u << 6,   // with kSelf: a passive does not count ("is recorded at 75 g")
  kRecallOk   = 1u << 7,   // figure: the owner's own report, so recalled memories count as a source
  kLogSense   = 1u << 8,   // "logged": after a system-data noun it is the device's log, not a save
  kState      = 1u << 9,   // a state claim: plain future tense ("it will survive a reboot") still claims it
  kAsName     = 1u << 10,  // "saved as <name>": followed by a/an/the it names a kind, not a file
  kFaultCtx   = 1u << 11,  // cause: counts only with a fault word in the sentence ("stuck", "flaky")
  kContraRepeat = 1u << 12,  // cause about the sender repeating: a gap when the device repeated
  kSelfOk     = 1u << 13,  // cause: "it is working": needs a health read and no failure on record
  kLogObject  = 1u << 14,  // saved: a log, entry, ledger or total within reach ("updated the log")
};

struct Cue {
  ClaimClass  cls;
  const char* phrase;
  uint16_t    flags;
  uint32_t    backs;   // FxKind bits that back the claim (0 = class-specific rule)
};

// A scratchpad write whose save landed is saved; one kept in RAM only is not.
constexpr uint32_t kSavedBacks = fxBit(FxKind::MemWrite) | fxBit(FxKind::MemUpdate) |
                                 fxBit(FxKind::MemPin) | fxBit(FxKind::FileWrite) |
                                 fxBit(FxKind::SkillSave) | fxBit(FxKind::Scratch);
// R4: "noted" (a note, not a stored record) is also backed by a RAM-only
// scratchpad write: it IS noted, until restart.
constexpr uint32_t kNotedBacks = kSavedBacks | fxBit(FxKind::ScratchRamOnly);
constexpr uint32_t kFileBacks = fxBit(FxKind::FileWrite) | fxBit(FxKind::SkillSave);
constexpr uint32_t kSendBacks = fxBit(FxKind::Send);
constexpr uint32_t kSpeakBacks = fxBit(FxKind::Speak);
constexpr uint32_t kSchedBacks = fxBit(FxKind::Schedule);
constexpr uint32_t kChangeBacks =
    fxBit(FxKind::MemPin) | fxBit(FxKind::MemUpdate) | fxBit(FxKind::MemDelete) |
    fxBit(FxKind::MemWrite) | fxBit(FxKind::Scratch) | fxBit(FxKind::ScratchRamOnly) |
    fxBit(FxKind::Config) | fxBit(FxKind::FileWrite) | fxBit(FxKind::SkillSave) |
    fxBit(FxKind::Schedule);
constexpr uint32_t kCheckBacks = fxBit(FxKind::Read) | fxBit(FxKind::SubSourced);
// "switched", "turned on", "set it to": a setting (a config change that stored a new
// value) or a routine; a memory or file write does not back it.
constexpr uint32_t kSwitchBacks = fxBit(FxKind::Config) | fxBit(FxKind::Schedule);
// "updated the log", "adjusted the entry" (19 Oct: the day log was said to be updated
// after a correction and was not): only a file write backs it.
constexpr uint32_t kLogBacks = fxBit(FxKind::FileWrite);
constexpr uint32_t kDurableStoreBacks = fxBit(FxKind::MemWrite) | fxBit(FxKind::MemUpdate) |
                                        fxBit(FxKind::MemPin) | fxBit(FxKind::FileWrite) |
                                        fxBit(FxKind::SkillSave);

using C = ClaimClass;

// THE cue table. Lowercase words separated by single spaces; a word ending in
// '*' matches as a prefix; '#' matches a number (digits or one..twelve); a word
// written "(x)" matches x only between parentheses. Keep each class's cues
// narrow: every cue here was read against real replies (claims.h).
constexpr Cue kCues[] = {
    // ---- saved / logged / noted
    {C::Saved, "logged", kLogSense, kSavedBacks},
    {C::Saved, "saved", 0, kSavedBacks},
    {C::Saved, "noted", 0, kNotedBacks},
    {C::Saved, "stored", 0, kSavedBacks},
    {C::Saved, "recorded", kSelf | kNoPassive | kLogSense, kSavedBacks},
    {C::Saved, "added to memory", 0, kSavedBacks},
    {C::Saved, "added to your memory", 0, kSavedBacks},
    {C::Saved, "added to my memory", 0, kSavedBacks},
    {C::Saved, "added it to memory", 0, kSavedBacks},
    {C::Saved, "added that to memory", 0, kSavedBacks},
    {C::Saved, "committed to memory", 0, kSavedBacks},
    {C::Saved, "made a note", kSelf, kNotedBacks},
    {C::Saved, "jotted down", kSelf, kNotedBacks},
    // a correction carried into a record (L19-50): with a log noun near, a file write
    // backs it; "corrected" alone is backed by any save
    {C::Saved, "updated", kLogObject, kLogBacks},
    {C::Saved, "corrected", kLogObject, kLogBacks},
    {C::Saved, "adjusted", kLogObject, kLogBacks},
    {C::Saved, "amended", kLogObject, kLogBacks},
    {C::Saved, "corrected", kSelf | kNoPassive, kSavedBacks},
    // ---- a deliverable that exists outside the reply
    {C::Deliverable, "written and waiting", kAnySubject, kFileBacks},
    {C::Deliverable, "drafted and waiting", kAnySubject, kFileBacks},
    {C::Deliverable, "ready and waiting", kAnySubject, kFileBacks},
    {C::Deliverable, "waiting whenever you", kAnySubject, kFileBacks},
    {C::Deliverable, "ready whenever you", kAnySubject, kFileBacks},
    {C::Deliverable, "saved as", kAsName, kFileBacks},
    {C::Deliverable, "saved it as", kAsName, kFileBacks},
    {C::Deliverable, "saved to your files", 0, kFileBacks},
    {C::Deliverable, "saved in your files", 0, kFileBacks},
    {C::Deliverable, "is in your files", kAnySubject, kFileBacks},
    {C::Deliverable, "waiting for you", kAnySubject, kFileBacks},
    {C::Deliverable, "in your documents", kAnySubject, kFileBacks},
    {C::Deliverable, "in your documents folder", kAnySubject, kFileBacks},
    {C::Deliverable, "saved to your documents", 0, kFileBacks},
    {C::Deliverable, "are in your files", kAnySubject, kFileBacks},
    {C::Deliverable, "is written up", kAnySubject, kFileBacks},
    {C::Deliverable, "has been written up", kAnySubject, kFileBacks},
    // ---- sub-agents started (the verb needs a sub-agent noun nearby, kSpawnNouns)
    {C::Spawned, "started", kSpawnVerb, 0},
    {C::Spawned, "restarted", kSpawnVerb, 0},
    {C::Spawned, "spawned", kSpawnVerb, 0},
    {C::Spawned, "respawned", kSpawnVerb, 0},
    {C::Spawned, "kicked off", kSpawnVerb, 0},
    {C::Spawned, "kicking off", kSpawnVerb, 0},
    {C::Spawned, "spun up", kSpawnVerb, 0},
    {C::Spawned, "spinning up", kSpawnVerb, 0},
    {C::Spawned, "launching", kSpawnVerb, 0},
    {C::Spawned, "firing off", kSpawnVerb, 0},
    {C::Spawned, "launched", kSpawnVerb, 0},
    {C::Spawned, "relaunched", kSpawnVerb, 0},
    {C::Spawned, "fired off", kSpawnVerb, 0},
    {C::Spawned, "dispatched", kSpawnVerb, 0},
    {C::Spawned, "put", kSpawnVerb, 0},
    {C::Spawned, "put # into this turn", 0, 0},
    // ---- sent / shared / spoken / scheduled
    {C::Sent, "sent", kSelf, kSendBacks},
    {C::Sent, "shared", kSelf, kSendBacks},
    {C::Sent, "emailed", kSelf, kSendBacks},
    {C::Sent, "forwarded", kSelf, kSendBacks},
    {C::Sent, "delivered", kSelf, kSendBacks},
    {C::Sent, "posted", kSelf, kSendBacks},
    {C::Sent, "voice message sent", 0, kSendBacks},
    {C::Sent, "voice messages sent", 0, kSendBacks},
    {C::Sent, "voice note sent", 0, kSendBacks},
    {C::Sent, "voice notes sent", 0, kSendBacks},
    {C::Sent, "audio message sent", 0, kSendBacks},
    {C::Sent, "voice reply sent", 0, kSendBacks},
    {C::Sent, "spoken", kSelf, kSpeakBacks},
    {C::Sent, "read it aloud", 0, kSpeakBacks},
    {C::Sent, "read aloud", kSelf, kSpeakBacks},
    {C::Sent, "reading it aloud", 0, kSpeakBacks},
    {C::Sent, "reminder set", kAnySubject, kSchedBacks},
    {C::Sent, "reminder is set", kAnySubject, kSchedBacks},
    {C::Sent, "set a reminder", kSelf, kSchedBacks},
    {C::Sent, "set you a reminder", kSelf, kSchedBacks},
    {C::Sent, "scheduled", kSelf, kSchedBacks},
    {C::Sent, "alarm set", kAnySubject, kSchedBacks},
    {C::Sent, "routine set", kAnySubject, kSchedBacks},
    // ---- changed state
    {C::Changed, "pinned", kPin, 0},
    {C::Changed, "unpinned", 0, fxBit(FxKind::MemUpdate)},
    {C::Changed, "switched", kSelf, kSwitchBacks},
    {C::Changed, "turned off", kSelf, kSwitchBacks},
    {C::Changed, "turned on", kSelf, kSwitchBacks},
    {C::Changed, "updated", kSelf, kChangeBacks},
    {C::Changed, "changed", kSelf, kChangeBacks},
    {C::Changed, "enabled", kSelf, kSwitchBacks},
    {C::Changed, "disabled", kSelf, kSwitchBacks},
    {C::Changed, "i set", 0, kSwitchBacks},
    {C::Changed, "i've set", 0, kSwitchBacks},
    {C::Changed, "i have set", 0, kSwitchBacks},
    {C::Changed, "we set", 0, kSwitchBacks},
    {C::Changed, "set it to", kSelf | kNoPassive, kSwitchBacks},
    {C::Changed, "set to", kSelf | kNoPassive, kSwitchBacks},
    {C::Changed, "now set to", kAnySubject, kSwitchBacks},
    // ---- durable (the scratchpad's save, a pin)
    {C::Durable, "survive* reboot*", kAnySubject | kState, 0},
    {C::Durable, "survive* a reboot", kAnySubject | kState, 0},
    {C::Durable, "survive* restart*", kAnySubject | kState, 0},
    {C::Durable, "survive* a restart", kAnySubject | kState, 0},
    {C::Durable, "reboot or not", kAnySubject | kState, 0},
    {C::Durable, "restart or not", kAnySubject | kState, 0},
    {C::Durable, "even after a reboot", kAnySubject | kState, 0},
    {C::Durable, "even after a restart", kAnySubject | kState, 0},
    {C::Durable, "across reboots", kAnySubject | kState, 0},
    {C::Durable, "and reboots", kAnySubject | kState, 0},
    {C::Durable, "and restarts", kAnySubject | kState, 0},
    {C::Durable, "across restarts", kAnySubject | kState, 0},
    {C::Durable, "saved permanently", kAnySubject | kState, 0},
    {C::Durable, "stored permanently", kAnySubject | kState, 0},
    {C::Durable, "kept permanently", kAnySubject | kState, 0},
    {C::Durable, "permanently saved", kAnySubject | kState, 0},
    {C::Durable, "permanently stored", kAnySubject | kState, 0},
    {C::Durable, "every single turn", kAnySubject | kState, 0},
    {C::Durable, "every turn", kAnySubject | kState, 0},
    {C::Durable, "loaded into every", kAnySubject | kState, 0},
    {C::Durable, "always loaded", kAnySubject | kState, 0},
    {C::Durable, "in every prompt", kAnySubject | kState, 0},
    {C::Durable, "never decay*", kAnySubject | kState | kNeverDecay, 0},
    {C::Durable, "won't decay", kAnySubject | kState | kNeverDecay, 0},
    {C::Durable, "cannot decay", kAnySubject | kState | kNeverDecay, 0},
    {C::Durable, "can't decay", kAnySubject | kState | kNeverDecay, 0},
    {C::Durable, "never be forgotten", kAnySubject | kState | kNeverDecay, 0},
    {C::Durable, "never expire*", kAnySubject | kState | kNeverDecay, 0},
    {C::Durable, "won't expire", kAnySubject | kState | kNeverDecay, 0},
    // ---- checked / looked up
    {C::Checked, "i checked", 0, kCheckBacks},
    {C::Checked, "i've checked", 0, kCheckBacks},
    {C::Checked, "i have checked", 0, kCheckBacks},
    {C::Checked, "just checked", 0, kCheckBacks},
    {C::Checked, "double-checked", 0, kCheckBacks},
    {C::Checked, "double checked", 0, kCheckBacks},
    {C::Checked, "i verified", 0, kCheckBacks},
    {C::Checked, "i've verified", 0, kCheckBacks},
    {C::Checked, "i have verified", 0, kCheckBacks},
    {C::Checked, "verified live", kAnySubject, kCheckBacks},
    {C::Checked, "i confirmed", 0, kCheckBacks},
    {C::Checked, "i've confirmed", 0, kCheckBacks},
    {C::Checked, "i have confirmed", 0, kCheckBacks},
    {C::Checked, "confirmed live", kAnySubject, kCheckBacks},
    {C::Checked, "checked live", kAnySubject, kCheckBacks},
    {C::Checked, "looked it up", 0, kCheckBacks},
    {C::Checked, "looked that up", 0, kCheckBacks},
    {C::Checked, "i looked up", 0, kCheckBacks},
    {C::Checked, "i've looked up", 0, kCheckBacks},
    {C::Checked, "i searched", 0, kCheckBacks},
    {C::Checked, "i've searched", 0, kCheckBacks},
    {C::Checked, "searched the web", 0, kCheckBacks},
    {C::Checked, "live data", kAnySubject, kCheckBacks},
    {C::Checked, "live status", kAnySubject, kCheckBacks},
    {C::Checked, "real-time data", kAnySubject, kCheckBacks},
    // ---- a figure credited to a source the turn holds
    {C::Figure, "(label)", kAnySubject | kState | kFigure, 0},
    {C::Figure, "label-exact", kAnySubject | kState | kFigure, 0},
    {C::Figure, "label exact", kAnySubject | kState | kFigure, 0},
    {C::Figure, "per the label", kAnySubject | kState | kFigure, 0},
    {C::Figure, "from the label", kAnySubject | kState | kFigure, 0},
    {C::Figure, "on the label", kAnySubject | kState | kFigure, 0},
    {C::Figure, "label says", kAnySubject | kState | kFigure, 0},
    {C::Figure, "label shows", kAnySubject | kState | kFigure, 0},
    {C::Figure, "label lists", kAnySubject | kState | kFigure, 0},
    {C::Figure, "label states", kAnySubject | kState | kFigure, 0},
    {C::Figure, "per your watch", kAnySubject | kState | kFigure | kRecallOk, 0},
    {C::Figure, "watch said", kAnySubject | kState | kFigure | kRecallOk, 0},
    {C::Figure, "watch says", kAnySubject | kState | kFigure | kRecallOk, 0},
    {C::Figure, "watch shows", kAnySubject | kState | kFigure | kRecallOk, 0},
    {C::Figure, "from your watch", kAnySubject | kState | kFigure | kRecallOk, 0},
    {C::Figure, "per the receipt", kAnySubject | kState | kFigure, 0},
    {C::Figure, "on the receipt", kAnySubject | kState | kFigure, 0},
    {C::Figure, "from the receipt", kAnySubject | kState | kFigure, 0},
    {C::Figure, "receipt says", kAnySubject | kState | kFigure, 0},
    {C::Figure, "receipt shows", kAnySubject | kState | kFigure, 0},
    {C::Figure, "per the package", kAnySubject | kState | kFigure, 0},
    {C::Figure, "per the lab report", kAnySubject | kState | kFigure, 0},
    {C::Figure, "per the report", kAnySubject | kState | kFigure, 0},
    {C::Figure, "report says", kAnySubject | kState | kFigure, 0},
    {C::Figure, "report shows", kAnySubject | kState | kFigure, 0},
    {C::Figure, "per the invoice", kAnySubject | kState | kFigure, 0},
    {C::Figure, "invoice says", kAnySubject | kState | kFigure, 0},
    {C::Figure, "invoice shows", kAnySubject | kState | kFigure, 0},
    {C::Figure, "per the menu", kAnySubject | kState | kFigure, 0},
    {C::Figure, "menu says", kAnySubject | kState | kFigure, 0},
    {C::Figure, "per your scale", kAnySubject | kState | kFigure | kRecallOk, 0},
    {C::Figure, "scale said", kAnySubject | kState | kFigure | kRecallOk, 0},
    {C::Figure, "scale says", kAnySubject | kState | kFigure | kRecallOk, 0},
    {C::Figure, "scale shows", kAnySubject | kState | kFigure | kRecallOk, 0},
    {C::Figure, "on the package", kAnySubject | kState | kFigure, 0},
    {C::Figure, "package says", kAnySubject | kState | kFigure, 0},
    {C::Figure, "per the packaging", kAnySubject | kState | kFigure, 0},
    {C::Figure, "on the packaging", kAnySubject | kState | kFigure, 0},
    // ---- causes (backs = the CauseEvidence bit that supports the cause)
    // the owner's phone, app or client (a fault word in the sentence makes it a cause)
    {C::Cause, "telegram client", kAnySubject | kState | kFaultCtx, kEvClient},
    {C::Cause, "telegram app", kAnySubject | kState | kFaultCtx, kEvClient},
    {C::Cause, "your client", kAnySubject | kState | kFaultCtx, kEvClient},
    {C::Cause, "your app", kAnySubject | kState | kFaultCtx, kEvClient},
    {C::Cause, "your phone", kAnySubject | kState | kFaultCtx, kEvClient},
    {C::Cause, "on your end", kAnySubject | kState | kFaultCtx, kEvClient | kEvNetwork},
    {C::Cause, "on your side", kAnySubject | kState | kFaultCtx, kEvClient | kEvNetwork},
    {C::Cause, "your outbox", kAnySubject | kState, kEvClient},
    {C::Cause, "stuck outbox", kAnySubject | kState, kEvClient},
    // advice that blames the owner's phone or app
    {C::Cause, "force-close*", kAnySubject | kState, kEvClient},
    {C::Cause, "force close*", kAnySubject | kState, kEvClient},
    {C::Cause, "force-quit*", kAnySubject | kState, kEvClient},
    {C::Cause, "force quit*", kAnySubject | kState, kEvClient},
    {C::Cause, "clear its outbox", kAnySubject | kState, kEvClient},
    {C::Cause, "clearing its outbox", kAnySubject | kState, kEvClient},
    {C::Cause, "clear the outbox", kAnySubject | kState, kEvClient},
    {C::Cause, "clear your outbox", kAnySubject | kState, kEvClient},
    {C::Cause, "reinstall the app", kAnySubject | kState, kEvClient},
    {C::Cause, "reinstalling the app", kAnySubject | kState, kEvClient},
    {C::Cause, "reinstall telegram", kAnySubject | kState, kEvClient},
    {C::Cause, "restart the app", kAnySubject | kState, kEvClient},
    {C::Cause, "restarting the app", kAnySubject | kState, kEvClient},
    {C::Cause, "restart your phone", kAnySubject | kState, kEvClient},
    {C::Cause, "restarting your phone", kAnySubject | kState, kEvClient},
    {C::Cause, "restart telegram", kAnySubject | kState, kEvClient},
    {C::Cause, "update the app", kAnySubject | kState, kEvClient},
    {C::Cause, "update your app", kAnySubject | kState, kEvClient},
    {C::Cause, "update telegram", kAnySubject | kState, kEvClient},
    // the owner's connection
    {C::Cause, "your network", kAnySubject | kState | kFaultCtx, kEvNetwork},
    {C::Cause, "your connection", kAnySubject | kState | kFaultCtx, kEvNetwork},
    {C::Cause, "your internet", kAnySubject | kState | kFaultCtx, kEvNetwork},
    {C::Cause, "your wi-fi", kAnySubject | kState | kFaultCtx, kEvNetwork},
    {C::Cause, "your wifi", kAnySubject | kState | kFaultCtx, kEvNetwork},
    {C::Cause, "your signal", kAnySubject | kState | kFaultCtx, kEvNetwork},
    {C::Cause, "your router", kAnySubject | kState | kFaultCtx, kEvNetwork},
    {C::Cause, "network glitch*", kAnySubject | kState, kEvNetwork},
    {C::Cause, "network hiccup*", kAnySubject | kState, kEvNetwork},
    {C::Cause, "network issue*", kAnySubject | kState, kEvNetwork},
    {C::Cause, "network problem*", kAnySubject | kState, kEvNetwork},
    {C::Cause, "connection issue*", kAnySubject | kState, kEvNetwork},
    {C::Cause, "connection problem*", kAnySubject | kState, kEvNetwork},
    {C::Cause, "connectivity issue*", kAnySubject | kState, kEvNetwork},
    {C::Cause, "connectivity problem*", kAnySubject | kState, kEvNetwork},
    {C::Cause, "flaky connection", kAnySubject | kState, kEvNetwork},
    {C::Cause, "poor connection", kAnySubject | kState, kEvNetwork},
    {C::Cause, "spotty connection", kAnySubject | kState, kEvNetwork},
    {C::Cause, "unstable connection", kAnySubject | kState, kEvNetwork},
    {C::Cause, "bad connection", kAnySubject | kState, kEvNetwork},
    {C::Cause, "weak signal", kAnySubject | kState, kEvNetwork},
    {C::Cause, "check your connection", kAnySubject | kState, kEvNetwork},
    {C::Cause, "check your network", kAnySubject | kState, kEvNetwork},
    {C::Cause, "check your internet", kAnySubject | kState, kEvNetwork},
    {C::Cause, "check your wi-fi", kAnySubject | kState, kEvNetwork},
    {C::Cause, "check your wifi", kAnySubject | kState, kEvNetwork},
    // a damaged or incomplete file
    {C::Cause, "corrupt", kAnySubject | kState, kEvCorrupt},
    {C::Cause, "corrupted", kAnySubject | kState, kEvCorrupt},
    {C::Cause, "partially uploaded", kAnySubject | kState, kEvCorrupt},
    {C::Cause, "partially-uploaded", kAnySubject | kState, kEvCorrupt},
    {C::Cause, "partial upload", kAnySubject | kState, kEvCorrupt},
    {C::Cause, "incomplete upload", kAnySubject | kState, kEvCorrupt},
    {C::Cause, "upload was incomplete", kAnySubject | kState, kEvCorrupt},
    {C::Cause, "didn't fully upload", kAnySubject | kState, kEvCorrupt},
    {C::Cause, "did not fully upload", kAnySubject | kState, kEvCorrupt},
    {C::Cause, "damaged file", kAnySubject | kState, kEvCorrupt},
    {C::Cause, "file is damaged", kAnySubject | kState, kEvCorrupt},
    {C::Cause, "file was damaged", kAnySubject | kState, kEvCorrupt},
    {C::Cause, "malformed", kAnySubject | kState, kEvCorrupt},
    {C::Cause, "broken file", kAnySubject | kState, kEvCorrupt},
    {C::Cause, "file is broken", kAnySubject | kState, kEvCorrupt},
    // the sender repeating himself (a gap when the device recorded its own repeat)
    {C::Cause, "you keep sending", kAnySubject | kState | kContraRepeat, 0},
    {C::Cause, "you keep resending", kAnySubject | kState | kContraRepeat, 0},
    {C::Cause, "you've been sending", kAnySubject | kState | kContraRepeat, 0},
    {C::Cause, "you have been sending", kAnySubject | kState | kContraRepeat, 0},
    {C::Cause, "you sent the same", kAnySubject | kState | kContraRepeat, 0},
    {C::Cause, "you've sent the same", kAnySubject | kState | kContraRepeat, 0},
    {C::Cause, "you sent it again", kAnySubject | kState | kContraRepeat, 0},
    {C::Cause, "you sent this again", kAnySubject | kState | kContraRepeat, 0},
    {C::Cause, "you sent that again", kAnySubject | kState | kContraRepeat, 0},
    {C::Cause, "you re-sent", kAnySubject | kState | kContraRepeat, 0},
    {C::Cause, "you resent", kAnySubject | kState | kContraRepeat, 0},
    {C::Cause, "you're resending", kAnySubject | kState | kContraRepeat, 0},
    {C::Cause, "you're sending the same", kAnySubject | kState | kContraRepeat, 0},
    {C::Cause, "you sent it # times", kAnySubject | kState | kContraRepeat, 0},
    {C::Cause, "you've sent it # times", kAnySubject | kState | kContraRepeat, 0},
    {C::Cause, "you sent this # times", kAnySubject | kState | kContraRepeat, 0},
    {C::Cause, "you've sent this # times", kAnySubject | kState | kContraRepeat, 0},
    // the assistant's own subsystems failing: timed out, out of credit
    {C::Cause, "timed out", kAnySubject | kState, kEvTimeout},
    {C::Cause, "timeout", kAnySubject | kState, kEvTimeout},
    {C::Cause, "time limit", kAnySubject | kState, kEvTimeout},
    {C::Cause, "took too long", kAnySubject | kState, kEvTimeout},
    {C::Cause, "out of credit*", kAnySubject | kState, kEvBilling},
    {C::Cause, "no credits", kAnySubject | kState, kEvBilling},
    {C::Cause, "credit balance", kAnySubject | kState, kEvBilling},
    {C::Cause, "billing issue*", kAnySubject | kState, kEvBilling},
    {C::Cause, "billing problem*", kAnySubject | kState, kEvBilling},
    // ... or working (needs a health read this turn and no failure on record)
    {C::Cause, "vision path is working", kAnySubject | kState | kSelfOk, kEvImageFail},
    {C::Cause, "vision path is healthy", kAnySubject | kState | kSelfOk, kEvImageFail},
    {C::Cause, "vision path is fine", kAnySubject | kState | kSelfOk, kEvImageFail},
    {C::Cause, "vision path works", kAnySubject | kState | kSelfOk, kEvImageFail},
    {C::Cause, "vision is working", kAnySubject | kState | kSelfOk, kEvImageFail},
    {C::Cause, "vision is fine", kAnySubject | kState | kSelfOk, kEvImageFail},
    {C::Cause, "vision is healthy", kAnySubject | kState | kSelfOk, kEvImageFail},
    {C::Cause, "vision works", kAnySubject | kState | kSelfOk, kEvImageFail},
    {C::Cause, "image reading is working", kAnySubject | kState | kSelfOk, kEvImageFail},
    {C::Cause, "image reading works", kAnySubject | kState | kSelfOk, kEvImageFail},
    {C::Cause, "photo reading is working", kAnySubject | kState | kSelfOk, kEvImageFail},
    {C::Cause, "nothing wrong on my end", kAnySubject | kState | kSelfOk, kEvFailure},
    {C::Cause, "nothing is wrong on my end", kAnySubject | kState | kSelfOk, kEvFailure},
    {C::Cause, "nothing wrong on my side", kAnySubject | kState | kSelfOk, kEvFailure},
    {C::Cause, "nothing is wrong on my side", kAnySubject | kState | kSelfOk, kEvFailure},
    {C::Cause, "no problem on my end", kAnySubject | kState | kSelfOk, kEvFailure},
    {C::Cause, "no problem on my side", kAnySubject | kState | kSelfOk, kEvFailure},
    {C::Cause, "no issue on my end", kAnySubject | kState | kSelfOk, kEvFailure},
    {C::Cause, "no issues on my end", kAnySubject | kState | kSelfOk, kEvFailure},
    {C::Cause, "everything is working on my end", kAnySubject | kState | kSelfOk, kEvFailure},
    {C::Cause, "everything is fine on my end", kAnySubject | kState | kSelfOk, kEvFailure},
    {C::Cause, "my end is fine", kAnySubject | kState | kSelfOk, kEvFailure},
    {C::Cause, "my side is fine", kAnySubject | kState | kSelfOk, kEvFailure},
    {C::Cause, "all systems are working", kAnySubject | kState | kSelfOk, kEvFailure},
    {C::Cause, "all systems are fine", kAnySubject | kState | kSelfOk, kEvFailure},
};
constexpr size_t kCueCount = sizeof(kCues) / sizeof(kCues[0]);

// Sub-agent nouns a spawn verb pairs with (within kSpawnWindow tokens, either
// side, same clause). "agent" alone needs a number or a qualifier in front.
constexpr const char* kSpawnNouns[] = {
    "sub-agent*", "subagent*", "sub agent*", "# agent*", "# research agent*",
    "research agent*", "background agent*", "parallel agent*", "research task*",
    "research job*", "background task*", "background job*", "background search*",
    "background research", "parallel search*", "deep research", "# research",
    "research run*", "research sweep*", "# researcher*",
};
constexpr int kSpawnWindow = 6;

constexpr const char* kNumberWords[] = {"one", "two",   "three", "four",   "five",  "six",
                                        "seven", "eight", "nine",  "ten", "eleven", "twelve"};

// R1: negation words (and the apostrophe-less spellings models also write).
constexpr const char* kNegations[] = {
    "not",    "no",      "never",  "nothing", "none",    "nor",      "neither", "without",
    "cannot", "failed",  "unable", "dont",    "didnt",   "doesnt",   "isnt",    "wasnt",
    "werent", "arent",   "havent", "hasnt",   "hadnt",   "couldnt",  "wouldnt", "shouldnt",
    "wont",   "cant",
};
// R2: an offer, condition or future anywhere earlier in the clause.
constexpr const char* kFutureWords[] = {
    "will",  "would", "could",  "should",   "shall", "might", "may",     "must",
    "if",    "unless", "once",  "whenever", "until", "maybe", "perhaps", "gonna",
    "wanna", "let's",  "lets",  "want",     "wants", "hope",  "hopefully",
    "probably", "possibly", "likely",
};
// Two-word R2 markers (the first word alone is too common to block on).
constexpr const char* kFuturePairs[] = {
    "i can",   "we can",   "you can",  "can i",   "can be",     "let me",   "to be",
    "have to", "has to",   "when you", "as soon", "about to",   "going to", "ready to",
    "happy to", "able to", "need to",  "needs to", "plan to",   "planning to", "try to",
    "trying to", "aim to",
};
// Another person as the subject, or a past reference: not this turn's claim.
constexpr const char* kOtherSubjects[] = {
    "you", "you've", "you'd", "you're", "they", "they've", "they'd", "he",
    "she", "he's",   "she's", "someone", "somebody", "already", "previously", "earlier",
    "still",
};
// Directly before an action word, these make it an adjective or a reference
// ("your logged runs", "the saved list", "as noted").
constexpr const char* kDeterminers[] = {
    "the", "a",     "an",    "your",  "my",   "our",  "their", "his", "her",
    "its", "this",  "these", "those", "any",  "some", "every", "each", "as",
    "same",
};
// "228 episodic messages logged", "in/out counts logged for openai": the device's
// own records, described; not something this turn saved.
constexpr const char* kLogNouns[] = {"messages", "counts", "tokens", "entries", "rows", "lines",
                                     "events", "errors", "calls", "requests", "turns", "runs"};
// Before a bare participle, these make it a running state, not this turn's act
// ("currently 746 kcal logged", "the day's total logged").
constexpr const char* kStateMarkers[] = {"currently", "total", "totals", "overall"};
// Anywhere earlier in the sentence, these make it a reference to earlier work
// ("both already landed: the skill is active and your baseline is saved").
constexpr const char* kEarlierWork[] = {"already", "previously", "earlier"};
// Right after a cue, these place the action in an earlier turn ("switched over
// per your last request", "saved yesterday").
constexpr const char* kPastAfter[] = {"earlier", "yesterday", "previously", "before", "last"};
// kSelf: the assistant as subject (within 3 tokens) or a passive auxiliary
// (within 2 tokens) or the cue opening its clause (after a filler word).
constexpr const char* kSelfSubjects[] = {"i", "i've", "we", "we've", "i'm"};
constexpr const char* kPassives[] = {"is", "are", "was", "were", "been", "be", "it's", "that's", "now", "all", "both"};
constexpr const char* kFillers[] = {"and", "also", "just", "now", "so", "then", "ok", "okay", "both", "all"};

// ---- tokens --------------------------------------------------------------

enum : uint8_t { kTokQuoted = 1u << 0, kTokNum = 1u << 1 };

struct Tok {
  uint32_t b = 0;
  uint16_t len = 0;
  uint8_t  flags = 0;
  uint16_t sent = 0;
  uint16_t clause = 0;
};

using TokVec = std::vector<Tok, WorkingAllocator<Tok>>;
using ByteVec = std::vector<uint8_t, WorkingAllocator<uint8_t>>;

bool isAsciiLetter(unsigned char c) { return (c | 0x20) >= 'a' && (c | 0x20) <= 'z'; }
bool isDigit(unsigned char c) { return c >= '0' && c <= '9'; }

// The three UTF-8 bytes at p (if that many remain) equal a,b,c.
bool u8at(const std::string& s, size_t p, unsigned char a, unsigned char b, unsigned char c) {
  return p + 2 < s.size() && (unsigned char)s[p] == a && (unsigned char)s[p + 1] == b &&
         (unsigned char)s[p + 2] == c;
}
bool isRightSingleQuote(const std::string& s, size_t p) { return u8at(s, p, 0xE2, 0x80, 0x99); }
bool isDash3(const std::string& s, size_t p) {   // en or em dash
  return u8at(s, p, 0xE2, 0x80, 0x93) || u8at(s, p, 0xE2, 0x80, 0x94);
}
bool isCurlyOpenQuote(const std::string& s, size_t p) { return u8at(s, p, 0xE2, 0x80, 0x9C); }
bool isCurlyCloseQuote(const std::string& s, size_t p) { return u8at(s, p, 0xE2, 0x80, 0x9D); }
bool isEllipsis(const std::string& s, size_t p) { return u8at(s, p, 0xE2, 0x80, 0xA6); }

// Byte at p of a token, lowercased, with the typographic apostrophe folded to '.
// Advances p past the whole character.
char foldAt(const std::string& s, size_t& p) {
  if (isRightSingleQuote(s, p)) { p += 3; return '\''; }
  const unsigned char c = (unsigned char)s[p++];
  return (c >= 'A' && c <= 'Z') ? (char)(c | 0x20) : (char)c;
}

// Does the token text equal `w` (or start with it, prefix=true)?
bool tokIs(const std::string& s, const Tok& t, const char* w, size_t wn, bool prefix) {
  size_t p = t.b, end = t.b + t.len, i = 0;
  while (p < end && i < wn) {
    if (foldAt(s, p) != w[i]) return false;
    i++;
  }
  if (i < wn) return false;
  return prefix || p == end;
}
bool tokIs(const std::string& s, const Tok& t, const char* w) {
  return tokIs(s, t, w, std::strlen(w), false);
}
template <size_t N>
bool tokIn(const std::string& s, const Tok& t, const char* const (&set)[N]) {
  for (const char* w : set)
    if (tokIs(s, t, w)) return true;
  return false;
}
// Does the folded token end with `suf`? Folding first, so a typographic
// apostrophe (three bytes) matches the ASCII one in "n't".
bool endsWith(const std::string& s, const Tok& t, const char* suf) {
  char buf[32];
  size_t n = 0, p = t.b;
  const size_t end = t.b + t.len;
  while (p < end && n < sizeof(buf)) buf[n++] = foldAt(s, p);
  if (p < end) return false;   // a token this long is no contraction
  const size_t m = std::strlen(suf);
  return n >= m && std::memcmp(buf + n - m, suf, m) == 0;
}

// ---- R5: quoted and code bytes -------------------------------------------

// Single quotes (' or the curly pair, often written as two right quotes): an
// opener follows a space or bracket and precedes a word; a closer follows a
// word and precedes a space or punctuation. An apostrophe inside a word ("it's")
// is neither, so contractions never open a quote.
size_t singleQuoteLen(const std::string& s, size_t p) {
  if (s[p] == '\'') return 1;
  return (u8at(s, p, 0xE2, 0x80, 0x98) || u8at(s, p, 0xE2, 0x80, 0x99)) ? 3 : 0;
}
bool wordish(unsigned char c) { return c >= 0x80 || isAsciiLetter(c) || isDigit(c); }

void markSingleQuotes(const std::string& s, size_t b, size_t e, ByteVec& q) {
  size_t open = std::string::npos;
  for (size_t p = b; p < e;) {
    const size_t n = singleQuoteLen(s, p);
    if (!n) { p++; continue; }
    const bool before = p > b && wordish((unsigned char)s[p - 1]);
    const bool after = p + n < e && wordish((unsigned char)s[p + n]);
    if (open == std::string::npos && !before && after) {
      open = p;
    } else if (open != std::string::npos && before && !after) {
      for (size_t k = open; k < p + n; k++) q[k] = 1;
      open = std::string::npos;
    }
    p += n;
  }
}

void markLineQuotes(const std::string& s, size_t b, size_t e, ByteVec& q) {
  // Pairs of " and of ` within one line; an unmatched opener is ignored.
  // Curly double quotes pair open/close. Quotes never span a line.
  const char straight[] = {'"', '`'};
  for (char qc : straight) {
    size_t open = std::string::npos;
    for (size_t p = b; p < e; p++) {
      if (s[p] != qc) continue;
      if (open == std::string::npos) { open = p; continue; }
      for (size_t k = open; k <= p; k++) q[k] = 1;
      open = std::string::npos;
    }
  }
  size_t open = std::string::npos;
  for (size_t p = b; p + 2 < e; p++) {
    if (isCurlyOpenQuote(s, p)) open = p;
    else if (isCurlyCloseQuote(s, p) && open != std::string::npos) {
      for (size_t k = open; k < p + 3; k++) q[k] = 1;
      open = std::string::npos;
    }
  }
  markSingleQuotes(s, b, e, q);
}

void markQuoted(const std::string& s, size_t n, ByteVec& q) {
  q.assign(n, 0);
  bool fence = false;
  size_t b = 0;
  while (b < n) {
    size_t e = s.find('\n', b);
    if (e == std::string::npos || e > n) e = n;
    const bool fenceLine = e - b >= 3 && s.compare(b, 3, "```") == 0;
    if (fenceLine || fence)
      for (size_t k = b; k < e; k++) q[k] = 1;
    else
      markLineQuotes(s, b, e, q);
    if (fenceLine) fence = !fence;
    b = e + 1;
  }
}

// ---- tokenizer -------------------------------------------------------------

struct Scan {
  const std::string& s;
  size_t n;
  ByteVec quoted;
  TokVec toks;
  std::vector<uint8_t, WorkingAllocator<uint8_t>> sentQuestion;   // per sentence
  uint16_t sent = 0, clause = 0;
  bool tokSinceBreak = false;
  explicit Scan(const std::string& str) : s(str), n(str.size() < kScanMax ? str.size() : kScanMax) {}
};

void endClause(Scan& sc) {
  if (sc.tokSinceBreak && sc.clause < 0xFFFF) sc.clause++;
  sc.tokSinceBreak = false;
}
void endSentence(Scan& sc, bool question) {
  if (question) sc.sentQuestion[sc.sent] = 1;
  if (sc.tokSinceBreak || question) {
    if (sc.sent < 0xFFFE) sc.sent++;
    sc.sentQuestion.push_back(0);
  }
  endClause(sc);
}

// A non-ASCII byte is part of a word unless it opens a dash, a curly double
// quote or an ellipsis.
bool unicodeWordByte(const std::string& s, size_t p) {
  return !isDash3(s, p) && !isCurlyOpenQuote(s, p) && !isCurlyCloseQuote(s, p) && !isEllipsis(s, p);
}

// A joiner inside a word or a number: sub-agent, label-exact, 6.08, 1,500, 35:38.
bool wordJoiner(const std::string& s, size_t p, size_t n) {
  const unsigned char c = (unsigned char)s[p];
  if ((c != '-' && c != '.' && c != ',' && c != ':' && c != '/') || p == 0 || p + 1 >= n) return false;
  const unsigned char a = (unsigned char)s[p - 1], z = (unsigned char)s[p + 1];
  if (c == '-') return (isAsciiLetter(a) || isDigit(a)) && (isAsciiLetter(z) || isDigit(z));
  return isDigit(a) && isDigit(z);
}

bool wordByte(const std::string& s, size_t p, size_t n) {
  const unsigned char c = (unsigned char)s[p];
  if (isAsciiLetter(c) || isDigit(c) || c == '\'') return true;
  if (c == 0xC2 && p + 1 < n && (unsigned char)s[p + 1] == 0xA0) return false;   // no-break space
  if (c >= 0x80) return unicodeWordByte(s, p);
  return wordJoiner(s, p, n);
}

size_t charLen(const std::string& s, size_t p) {
  const unsigned char c = (unsigned char)s[p];
  if (c < 0x80) return 1;
  if (c >= 0xF0) return 4;
  if (c >= 0xE0) return 3;
  return c >= 0xC0 ? 2 : 1;
}

size_t readToken(Scan& sc, size_t p) {
  Tok t;
  t.b = (uint32_t)p;
  while (p < sc.n && wordByte(sc.s, p, sc.n)) p += charLen(sc.s, p);
  if (p > sc.n) p = sc.n;
  t.len = (uint16_t)(p - t.b > 0xFFFF ? 0xFFFF : p - t.b);
  t.sent = sc.sent;
  t.clause = sc.clause;
  if (sc.quoted[t.b]) t.flags |= kTokQuoted;
  if (isDigit((unsigned char)sc.s[t.b])) t.flags |= kTokNum;
  // A trailing apostrophe is a closing single quote, not part of the word.
  while (t.len > 1 && sc.s[t.b + t.len - 1] == '\'') t.len--;
  sc.toks.push_back(t);
  sc.tokSinceBreak = true;
  return p;
}

// After a sentence mark: whitespace, a closing bracket or quote, or the end.
bool sentenceEnds(const std::string& s, size_t p, size_t n) {
  size_t q = p + 1;
  while (q < n && (s[q] == ')' || s[q] == '"' || s[q] == '*' || s[q] == '_' || s[q] == '\'')) q++;
  return q >= n || s[q] == ' ' || s[q] == '\n' || s[q] == '\t' || s[q] == '\r';
}

bool clauseBreak(const std::string& s, size_t p, size_t n) {
  const char c = s[p];
  const bool spacedHyphen = c == '-' && p > 0 && s[p - 1] == ' ' && p + 1 < n && s[p + 1] == ' ';
  return c == ',' || c == ';' || c == ':' || c == '(' || c == ')' || c == '[' || c == ']' ||
         c == '|' || spacedHyphen;
}

size_t punct(Scan& sc, size_t p) {
  const char c = sc.s[p];
  if (c == '\n') { endSentence(sc, false); return p + 1; }
  if (c == '?' || c == '!' || c == '.') {
    if (sentenceEnds(sc.s, p, sc.n)) endSentence(sc, c == '?');
    return p + 1;
  }
  if (isDash3(sc.s, p)) { endClause(sc); return p + 3; }
  if (isEllipsis(sc.s, p)) { endSentence(sc, false); return p + 3; }
  if (clauseBreak(sc.s, p, sc.n)) endClause(sc);
  return p + charLen(sc.s, p);
}

void tokenize(Scan& sc) {
  markQuoted(sc.s, sc.n, sc.quoted);
  sc.toks.reserve(sc.n / 5 + 8);
  sc.sentQuestion.push_back(0);
  size_t p = 0;
  while (p < sc.n) p = wordByte(sc.s, p, sc.n) ? readToken(sc, p) : punct(sc, p);
  endSentence(sc, false);
}

// ---- R6 --------------------------------------------------------------------

bool mostlyNonEnglish(const std::string& s, size_t n) {
  size_t ascii = 0, other = 0;
  for (size_t p = 0; p < n; p++) {
    const unsigned char c = (unsigned char)s[p];
    if (isAsciiLetter(c)) ascii++;
    else if ((c >= 0xC3 && c <= 0xDF) || (c >= 0xE3 && c <= 0xED)) other++;   // letters, not symbols
  }
  const size_t total = ascii + other;
  return total >= 12 && ascii * 10 < total * 7;
}

// ---- phrase matching -------------------------------------------------------

int numberWordValue(const std::string& s, const Tok& t) {
  for (size_t i = 0; i < sizeof(kNumberWords) / sizeof(kNumberWords[0]); i++)
    if (tokIs(s, t, kNumberWords[i])) return (int)i + 1;
  return 0;
}
int tokNumber(const std::string& s, const Tok& t) {
  if (t.flags & kTokNum) return std::atoi(s.c_str() + t.b);
  return numberWordValue(s, t);
}

// Match the phrase starting at token i, all in one clause. Returns the token
// count matched (0 = no match); *num receives a '#' word's value.
// One phrase word against token k: '#' is a number, "(x)" is x in parentheses,
// a trailing '*' is a prefix.
bool matchWord(const Scan& sc, size_t k, const char* w, size_t wl, int* num) {
  const Tok& t = sc.toks[k];
  if (wl == 1 && w[0] == '#') {
    const int v = tokNumber(sc.s, t);
    if (v <= 0) return false;
    if (num) *num = v;
    return true;
  }
  if (w[0] == '(' && wl > 2 && w[wl - 1] == ')')
    return tokIs(sc.s, t, w + 1, wl - 2, false) && t.b > 0 && sc.s[t.b - 1] == '(' &&
           t.b + t.len < sc.s.size() && sc.s[t.b + t.len] == ')';
  const bool prefix = w[wl - 1] == '*';
  return tokIs(sc.s, t, w, prefix ? wl - 1 : wl, prefix);
}

size_t matchPhrase(const Scan& sc, size_t i, const char* phrase, int* num) {
  size_t k = i;
  const char* w = phrase;
  while (*w) {
    const char* sp = std::strchr(w, ' ');
    const size_t wl = sp ? (size_t)(sp - w) : std::strlen(w);
    if (k >= sc.toks.size() || sc.toks[k].clause != sc.toks[i].clause) return 0;
    if (!matchWord(sc, k, w, wl, num)) return 0;
    k++;
    w = sp ? sp + 1 : w + wl;
  }
  return k - i;
}

// ---- scoping rules ---------------------------------------------------------

bool sameClause(const Scan& sc, size_t a, size_t b) {
  return sc.toks[a].clause == sc.toks[b].clause;
}

bool isNegation(const Scan& sc, const Tok& t) {
  return tokIn(sc.s, t, kNegations) || endsWith(sc.s, t, "n't");
}

// Plain future tense and likelihood words: they exempt an action ("I'll save
// it", "I probably saved it") but not a state ("it'll probably survive a
// reboot" still claims durability).
constexpr const char* kTenseOnly[] = {"will", "gonna", "shall", "probably", "possibly", "likely"};
// A cause stated as a guess is still a cause ("it's probably your connection", "it
// might be your phone"): these words do not exempt a cause claim; offers, conditions
// and questions still do.
constexpr const char* kSpeculation[] = {"could", "might",  "may",      "would",    "should",  "must",
                                        "maybe", "perhaps", "probably", "possibly", "likely",  "will",
                                        "gonna", "shall",  "hope",     "hopefully"};

// How a claim reads futures: an action (plain futures exempt it), a state (they do
// not), or a cause (guesses do not either).
enum class Tense : uint8_t { Action, State, Cause };

bool isFutureMarker(const Scan& sc, size_t k, Tense tense) {
  const Tok& t = sc.toks[k];
  if (tense != Tense::Action && (tokIn(sc.s, t, kTenseOnly) || endsWith(sc.s, t, "'ll"))) return false;
  if (tense == Tense::Cause && (tokIn(sc.s, t, kSpeculation) || endsWith(sc.s, t, "'d"))) return false;
  if (tokIn(sc.s, t, kFutureWords) || endsWith(sc.s, t, "'ll") || endsWith(sc.s, t, "'d")) return true;
  if (k + 1 >= sc.toks.size() || !sameClause(sc, k, k + 1)) return false;
  for (const char* pair : kFuturePairs) {
    const char* sp = std::strchr(pair, ' ');
    if (tense == Tense::Cause && std::strcmp(pair, "can be") == 0) continue;   // "can be your wifi"
    if (tokIs(sc.s, t, pair, (size_t)(sp - pair), false) && tokIs(sc.s, sc.toks[k + 1], sp + 1))
      return true;
  }
  return false;
}

// R1: a negation within 4 tokens before token i (same clause).
bool negatedBefore(const Scan& sc, size_t i) {
  for (size_t d = 1; d <= 4 && d <= i; d++) {
    if (!sameClause(sc, i - d, i)) break;
    if (isNegation(sc, sc.toks[i - d])) return true;
  }
  return false;
}

// R1, clause scope: a negative quantifier as the subject of the clause negates
// all of it, however far back ("and nothing in the recent logs or messages
// shows it was saved", "no record shows that I saved it"). It opens the clause
// or follows a join, "that", "there is" or a sentence adverb. Its scope ends
// where a join starts a new clause with its own subject ("nothing was lost
// because I saved a copy", "and the file was saved"); a noun list stays inside
// it ("nothing in the logs or the messages shows ...").
constexpr const char* kNegativeSubjects[] = {"nothing", "none", "nobody", "neither"};
// "no" plus one of these is a negative subject too ("no record shows ...").
constexpr const char* kNoSubjectNouns[] = {"record", "records", "sign", "evidence", "trace",
                                           "indication", "mention", "entry", "one"};
constexpr const char* kClauseJoins[] = {"and",   "but",    "so",     "yet",      "then",  "or",
                                        "because", "since", "though", "although", "while"};
constexpr const char* kSubjectLeadWords[] = {"that", "there's", "theres"};
constexpr const char* kSubjectPronouns[] = {"i",    "i've", "i'm", "we",      "we've", "it",  "it's",
                                            "they", "they've", "he", "she", "you",  "you've", "that's"};
constexpr const char* kThereBe[] = {"is", "are", "was", "were"};
constexpr const char* kBeHave[] = {"is", "are", "was", "were", "has", "have", "had"};

bool negativeSubjectAt(const Scan& sc, size_t k, size_t i) {
  const Tok& t = sc.toks[k];
  if (tokIn(sc.s, t, kNegativeSubjects)) return true;
  return tokIs(sc.s, t, "no") && k + 1 < i && tokIn(sc.s, sc.toks[k + 1], kNoSubjectNouns);
}

// Token k can be a subject: it opens the clause, or follows a join, a lead word,
// "there is", or a sentence adverb that does ("Sadly nothing").
bool leadsASubject(const Scan& sc, size_t k, bool adverbOk = true) {
  if (k == 0 || !sameClause(sc, k - 1, k)) return true;
  const Tok& p = sc.toks[k - 1];
  if (tokIn(sc.s, p, kClauseJoins) || tokIn(sc.s, p, kSubjectLeadWords)) return true;
  if (adverbOk && endsWith(sc.s, p, "ly")) return leadsASubject(sc, k - 1, false);
  return tokIn(sc.s, p, kThereBe) && k >= 2 && sameClause(sc, k - 2, k) && tokIs(sc.s, sc.toks[k - 2], "there");
}

// A join at k starts a new clause before the cue at i: a pronoun follows it, or
// a determined noun with "be" or "have" after it ("and the file was saved").
bool newClauseAt(const Scan& sc, size_t k, size_t i) {
  if (!tokIn(sc.s, sc.toks[k], kClauseJoins)) return false;
  if (tokIn(sc.s, sc.toks[k + 1], kSubjectPronouns)) return true;
  if (!tokIn(sc.s, sc.toks[k + 1], kDeterminers)) return false;
  for (size_t j = k + 2; j < i && j <= k + 5; j++)
    if (tokIn(sc.s, sc.toks[j], kBeHave)) return true;
  return false;
}

bool negativeSubjectScope(const Scan& sc, size_t i) {
  for (size_t k = i; k-- > 0 && sameClause(sc, k, i);) {
    if (newClauseAt(sc, k, i)) return false;
    if (negativeSubjectAt(sc, k, i) && leadsASubject(sc, k)) return true;
  }
  return false;
}

// R2: an offer, condition or future earlier in the clause.
bool futureBefore(const Scan& sc, size_t i, Tense tense) {
  for (size_t k = i; k-- > 0;) {
    if (!sameClause(sc, k, i)) break;
    if (isFutureMarker(sc, k, tense)) return true;
  }
  return false;
}

// The first token of token k's clause, in *first; false when the assistant is
// a subject in the clause before k (the clause has its own subject).
bool openerOf(const Scan& sc, size_t k, size_t* first) {
  size_t f = k;
  while (f > 0 && sameClause(sc, f - 1, k)) {
    f--;
    if (tokIn(sc.s, sc.toks[f], kSelfSubjects)) return false;
  }
  *first = f;
  return true;
}

// R2 across "or": "It may have been discussed without being saved, or saved
// under a different chat", "It was probably never created, or it was saved
// somewhere else", "I can change the tone, make it shorter, or read it aloud".
// A clause opened by "or" with no assistant subject before the cue shares the
// offer, condition or possibility of the clauses before it in the sentence,
// back to the nearest clause that has the assistant as its subject.
bool orClauseScope(const Scan& sc, size_t i, Tense tense) {
  size_t f = 0;
  if (tokIn(sc.s, sc.toks[i], kSelfSubjects) || !openerOf(sc, i, &f)) return false;
  if (f == 0 || !tokIs(sc.s, sc.toks[f], "or")) return false;
  for (int hop = 0; hop < 8 && f > 0; hop++) {
    const size_t p = f - 1;   // the last token of the clause before
    if (sc.toks[p].sent != sc.toks[i].sent) return false;
    if (isFutureMarker(sc, p, tense) || futureBefore(sc, p, tense)) return true;
    if (!openerOf(sc, p, &f)) return false;   // that clause has its own subject
  }
  return false;
}

// Another person or a past reference within 3 tokens, or a determiner right
// before: "the photo you sent", "already logged", "your logged runs".
bool otherSubjectBefore(const Scan& sc, size_t i) {
  if (i > 0 && sameClause(sc, i - 1, i) && tokIn(sc.s, sc.toks[i - 1], kDeterminers)) return true;
  for (size_t d = 1; d <= 3 && d <= i; d++) {
    if (!sameClause(sc, i - d, i)) break;
    if (tokIn(sc.s, sc.toks[i - d], kOtherSubjects)) return true;
  }
  return false;
}

// kSelf: the assistant (or a passive, unless noPassive) is the subject, or the
// cue opens its clause.
// Token i-d as the assistant subject: "I", "we've", a passive auxiliary (within
// 2, unless noPassive) or "I have" (within 2).
bool selfSubjectAt(const Scan& sc, size_t i, size_t d, bool noPassive) {
  const Tok& t = sc.toks[i - d];
  if (tokIn(sc.s, t, kSelfSubjects)) return true;
  if (d > 2) return false;
  if (!noPassive && tokIn(sc.s, t, kPassives)) return true;
  return tokIs(sc.s, t, "have") && d + 1 <= i && tokIs(sc.s, sc.toks[i - d - 1], "i");
}

bool selfContext(const Scan& sc, size_t i, bool noPassive) {
  size_t k = i;
  while (k > 0 && sameClause(sc, k - 1, i) && tokIn(sc.s, sc.toks[k - 1], kFillers)) k--;
  if (k == 0 || !sameClause(sc, k - 1, i)) return true;   // opens the clause
  for (size_t d = 1; d <= 3 && d <= i; d++) {
    if (!sameClause(sc, i - d, i)) break;
    if (selfSubjectAt(sc, i, d, noPassive)) return true;
  }
  return false;
}

bool anyQuoted(const Scan& sc, size_t i, size_t len) {
  for (size_t k = i; k < i + len; k++)
    if (sc.toks[k].flags & kTokQuoted) return true;
  return false;
}

// A spawn claim is negated by ANY negation in its clause ("the sub-agents I
// said I had started are not running").
bool clauseNegated(const Scan& sc, size_t i) {
  for (size_t k = i; k-- > 0 && sameClause(sc, k, i);)
    if (isNegation(sc, sc.toks[k])) return true;
  for (size_t k = i; k < sc.toks.size() && sameClause(sc, k, i); k++)
    if (isNegation(sc, sc.toks[k])) return true;
  return false;
}

// A sub-agent noun within the window, in the same sentence (a comma may sit
// between: "I have started it now, six sub-agents"); *count gets a number in
// front of it.
// A number one or two tokens before token k, same clause ("six research agents").
int numberBefore(const Scan& sc, size_t k) {
  for (size_t d = 1; d <= 2 && d <= k; d++) {
    if (!sameClause(sc, k - d, k)) break;
    const int n = tokNumber(sc.s, sc.toks[k - d]);
    if (n) return n;
  }
  return 0;
}

bool spawnNounNear(const Scan& sc, size_t vb, size_t ve, int* count) {
  const size_t lo = vb > (size_t)kSpawnWindow ? vb - kSpawnWindow : 0;
  const size_t hi = ve + kSpawnWindow < sc.toks.size() ? ve + kSpawnWindow : sc.toks.size();
  for (size_t k = lo; k < hi; k++) {
    if ((k >= vb && k < ve) || sc.toks[k].sent != sc.toks[vb].sent) continue;
    if (sc.toks[k].flags & kTokQuoted) continue;
    for (const char* noun : kSpawnNouns) {
      int num = 0;
      if (!matchPhrase(sc, k, noun, &num)) continue;
      *count = num ? num : numberBefore(sc, k);
      return true;
    }
  }
  return false;
}

// The byte span a claim reports: its table row (a line holding '|') for a
// figure, else its clause.
void claimSpan(const Scan& sc, size_t i, bool figure, uint32_t& b, uint32_t& e) {
  if (figure) {
    const size_t ls = sc.s.rfind('\n', sc.toks[i].b);
    const size_t lb = ls == std::string::npos ? 0 : ls + 1;
    size_t le = sc.s.find('\n', sc.toks[i].b);
    if (le == std::string::npos || le > sc.n) le = sc.n;
    if (sc.s.find('|', lb) < le) { b = (uint32_t)lb; e = (uint32_t)le; return; }
    // Otherwise the sentence: a source cue often sits in parentheses after its
    // figure ("142 bpm on the climb (per your watch)").
    size_t f = i, l = i;
    while (f > 0 && sc.toks[f - 1].sent == sc.toks[i].sent) f--;
    while (l + 1 < sc.toks.size() && sc.toks[l + 1].sent == sc.toks[i].sent) l++;
    b = sc.toks[f].b;
    e = sc.toks[l].b + sc.toks[l].len;
    return;
  }
  size_t f = i, l = i;
  while (f > 0 && sameClause(sc, f - 1, i)) f--;
  while (l + 1 < sc.toks.size() && sameClause(sc, l + 1, i)) l++;
  b = sc.toks[f].b;
  e = sc.toks[l].b + sc.toks[l].len;
}

// An -ing word right before the cue makes it an adjective ("recurring
// scheduled tasks"); "being" is the passive ("being saved" is R2 anyway).
bool adjectiveBefore(const Scan& sc, size_t i) {
  if (i == 0 || !sameClause(sc, i - 1, i)) return false;
  const Tok& t = sc.toks[i - 1];
  return t.len > 4 && endsWith(sc.s, t, "ing") && !tokIs(sc.s, t, "being");
}

// A past marker within 4 tokens after the cue, same clause.
bool pastAfter(const Scan& sc, size_t i, size_t len) {
  for (size_t k = i + len; k < sc.toks.size() && k < i + len + 4; k++) {
    if (!sameClause(sc, k, i)) break;
    if (tokIn(sc.s, sc.toks[k], kPastAfter)) return true;
  }
  return false;
}

// A bare participle ("saved", "logged") with no subject or auxiliary next to it,
// inside a noun phrase that a determiner or a negation opens within 6 tokens:
// "a packing list saved to your files", "no list being created or saved", "your
// sister's chat or contact saved". A headline claim has neither ("Run logged:").
// A subject pronoun right after a determined noun opens a relative clause:
// "the name it's saved under", "the list I saved" - a reference, not a claim.
bool relativeClauseAt(const Scan& sc, size_t subj) {
  static const char* const kPronouns[] = {"i", "i've", "we", "we've", "it's", "that's", "it", "they"};
  if (!tokIn(sc.s, sc.toks[subj], kPronouns)) return false;
  for (size_t d = 2; d <= 3 && d <= subj; d++) {
    if (!sameClause(sc, subj - d, subj)) break;
    if (tokIn(sc.s, sc.toks[subj - d], kDeterminers)) return true;
  }
  return false;
}

bool subjectOrAux(const Scan& sc, const Tok& t) {
  return tokIn(sc.s, t, kSelfSubjects) || tokIn(sc.s, t, kPassives) || tokIs(sc.s, t, "have") ||
         tokIs(sc.s, t, "has") || tokIs(sc.s, t, "i'm");
}
bool qualifier(const Scan& sc, const Tok& t) {
  return tokIn(sc.s, t, kDeterminers) || isNegation(sc, t) || tokIn(sc.s, t, kStateMarkers);
}

bool qualifiedParticiple(const Scan& sc, size_t i) {
  for (size_t d = 1; d <= 2 && d <= i; d++) {
    if (!sameClause(sc, i - d, i)) break;
    const Tok& t = sc.toks[i - d];
    if (subjectOrAux(sc, t)) return relativeClauseAt(sc, i - d);
    if (tokIs(sc.s, t, "being")) return true;   // still in progress
  }
  for (size_t d = 1; d <= 6 && d <= i; d++) {
    if (!sameClause(sc, i - d, i)) break;
    if (qualifier(sc, sc.toks[i - d])) return true;
  }
  return false;
}

bool logNounBefore(const Scan& sc, size_t i) {
  for (size_t d = 1; d <= 2 && d <= i; d++) {
    if (!sameClause(sc, i - d, i)) break;
    if (tokIn(sc.s, sc.toks[i - d], kLogNouns)) return true;
  }
  return false;
}

// A save cue that opens with a bare participle ("saved", "logged", "saved as",
// "saved to your files"): the qualifier rules above apply to it.
bool participleLed(const Cue& cue) {
  if ((cue.cls != C::Saved && cue.cls != C::Deliverable) || (cue.flags & (kSelf | kAnySubject)))
    return false;
  const char* sp = std::strchr(cue.phrase, ' ');
  const size_t n = sp ? (size_t)(sp - cue.phrase) : std::strlen(cue.phrase);
  return n > 2 && cue.phrase[n - 2] == 'e' && cue.phrase[n - 1] == 'd';
}

bool earlierInSentence(const Scan& sc, size_t i) {
  for (size_t k = i; k-- > 0 && sc.toks[k].sent == sc.toks[i].sent;)
    if (tokIn(sc.s, sc.toks[k], kEarlierWork)) return true;
  return false;
}

Tense tenseOf(const Cue& cue) {
  if (cue.cls == C::Cause) return Tense::Cause;
  return (cue.flags & kState) ? Tense::State : Tense::Action;
}

// R1, R2 and R5: quoted, a question, negated close by, or an offer/future.
bool outOfScope(const Scan& sc, size_t i, size_t len, const Cue& cue) {
  const Tense tense = tenseOf(cue);
  return anyQuoted(sc, i, len) || sc.sentQuestion[sc.toks[i].sent] || negatedBefore(sc, i) ||
         negativeSubjectScope(sc, i) || futureBefore(sc, i, tense) || orClauseScope(sc, i, tense);
}

// Not this turn's act by the assistant: another subject, an adjective, a past
// marker right after (cues that need no particular subject skip this).
bool notOurAct(const Scan& sc, size_t i, size_t len, const Cue& cue) {
  if (cue.flags & kAnySubject) return false;
  return otherSubjectBefore(sc, i) || adjectiveBefore(sc, i) || pastAfter(sc, i, len);
}

// A send cue that is about a sub-agent ("I sent a sub-agent to check") or sits
// in a relative clause ("the commute check I sent out came back"): earlier work
// or a spawn, not a delivery this turn.
bool sendReference(const Scan& sc, size_t i, size_t len, const Cue& cue) {
  if (cue.cls != C::Sent) return false;
  for (size_t k = i + len; k < sc.toks.size() && k < i + len + 3; k++) {
    if (!sameClause(sc, k, i)) break;
    for (const char* noun : kSpawnNouns)
      if (matchPhrase(sc, k, noun, nullptr)) return true;
  }
  for (size_t d = 1; d <= 2 && d <= i; d++) {
    if (!sameClause(sc, i - d, i)) break;
    if (tokIn(sc.s, sc.toks[i - d], kSelfSubjects)) return relativeClauseAt(sc, i - d);
  }
  return false;
}

// A save cue in a reference: a system log noun, a qualified participle, or
// earlier work named in the sentence.
bool saveReference(const Scan& sc, size_t i, const Cue& cue) {
  if ((cue.flags & kLogSense) && logNounBefore(sc, i)) return true;
  if (participleLed(cue) && qualifiedParticiple(sc, i)) return true;
  return (cue.cls == C::Saved || cue.cls == C::Deliverable) && earlierInSentence(sc, i);
}

bool passesScope(const Scan& sc, size_t i, size_t len, const Cue& cue) {
  if (outOfScope(sc, i, len, cue) || notOurAct(sc, i, len, cue) || saveReference(sc, i, cue) ||
      sendReference(sc, i, len, cue))
    return false;
  return !(cue.flags & kSelf) || selfContext(sc, i, (cue.flags & kNoPassive) != 0);
}

// "saved as a permanent note" names a kind of record (a save claim), not a
// file ("saved as plan.md"): it is checked as one, against the save cues.
bool namesAKind(const Scan& sc, size_t i, size_t len, const Cue& cue) {
  return (cue.flags & kAsName) && i + len < sc.toks.size() && sameClause(sc, i, i + len) &&
         tokIn(sc.s, sc.toks[i + len], kDeterminers);
}

// A spawn verb counts only with a sub-agent noun near it and no negation in
// its clause; *num gets the count it states.
bool spawnRejected(const Scan& sc, size_t i, size_t len, const Cue& cue, int* num) {
  return (cue.flags & kSpawnVerb) && (clauseNegated(sc, i) || !spawnNounNear(sc, i, i + len, num));
}

// Fault words: in a cause's sentence they make "your phone" a cause ("your Telegram
// client is re-sending a stuck item"), not a mention ("send it from your phone").
constexpr const char* kFaultWords[] = {
    "stuck",     "re-send*", "resend*",   "re-sent",  "glitch*", "bug*",      "broken",  "corrupt*",
    "fail*",     "issue*",   "problem*",  "outbox",   "keeps",   "repeat*",   "duplicat*", "crash*",
    "flaky",     "drop*",    "unstable",  "slow*",    "lag*",    "weak",      "poor",    "spotty",
    "interrupt*", "lost",    "losing",    "error*",   "offline", "hiccup*",   "fault*",  "misbehav*",
    "unreliable", "blocking", "blocked",  "cut",      "patchy",  "intermittent*",
};

bool faultWord(const Scan& sc, const Tok& t) {
  for (const char* w : kFaultWords) {
    const size_t n = std::strlen(w);
    const bool prefix = w[n - 1] == '*';
    if (tokIs(sc.s, t, w, prefix ? n - 1 : n, prefix)) return true;
  }
  return false;
}

bool faultWordInSentence(const Scan& sc, size_t i, size_t len) {
  for (size_t k = i; k-- > 0 && sc.toks[k].sent == sc.toks[i].sent;)
    if (faultWord(sc, sc.toks[k])) return true;
  for (size_t k = i + len; k < sc.toks.size() && sc.toks[k].sent == sc.toks[i].sent; k++)
    if (faultWord(sc, sc.toks[k])) return true;
  return false;
}

// Records a correction is carried into ("updated the log", "the entry is corrected").
constexpr const char* kRecordNouns[] = {"log",   "logs",  "entry", "entries", "ledger", "total",
                                        "totals", "tally", "diary", "tracker", "record"};

bool recordNounNear(const Scan& sc, size_t i, size_t len) {
  for (size_t k = i + len; k < sc.toks.size() && k < i + len + 4 && sameClause(sc, k, i); k++)
    if (tokIn(sc.s, sc.toks[k], kRecordNouns)) return true;
  for (size_t d = 1; d <= 3 && d <= i && sameClause(sc, i - d, i); d++)
    if (tokIn(sc.s, sc.toks[i - d], kRecordNouns)) return true;
  return false;
}

// A cue that needs company it does not have: a cause noun with no fault word in its
// sentence, a record verb with no record near.
bool contextMissing(const Scan& sc, size_t i, size_t len, const Cue& cue) {
  if ((cue.flags & kFaultCtx) && !faultWordInSentence(sc, i, len)) return true;
  return (cue.flags & kLogObject) && !recordNounNear(sc, i, len);
}

void addClaim(const Scan& sc, size_t i, size_t c, int num, ClaimScan& out) {
  Claim cl;
  cl.cls = kCues[c].cls;
  cl.cue = (uint16_t)c;
  cl.count = (uint8_t)(num > 0 && num < 256 ? num : 0);
  claimSpan(sc, i, (kCues[c].flags & kFigure) != 0, cl.begin, cl.end);
  for (const Claim& o : out.claims)
    if (o.cls == cl.cls && o.begin == cl.begin) return;
  out.claims.push_back(cl);
}

// Try every cue at token i; append at most one claim per class. A record verb with
// its record ("updated the log") claims the token for the save class: the plain
// "updated" of the change class stands down.
void matchAt(const Scan& sc, size_t i, ClaimScan& out) {
  const char first = sc.s[sc.toks[i].b] | 0x20;
  for (size_t c = 0; c < kCueCount; c++) {
    const Cue& cue = kCues[c];
    const char lead = cue.phrase[0] == '(' ? cue.phrase[1] : cue.phrase[0];
    if (lead != '#' && lead != first) continue;
    int num = 0;
    const size_t len = matchPhrase(sc, i, cue.phrase, &num);
    if (!len || contextMissing(sc, i, len, cue) || !passesScope(sc, i, len, cue)) continue;
    if (spawnRejected(sc, i, len, cue, &num) || namesAKind(sc, i, len, cue)) continue;
    addClaim(sc, i, c, num, out);
    if (cue.flags & kLogObject) return;
  }
}

// ---- figures ---------------------------------------------------------------

// A number token normalized for comparison: thousands commas dropped, a unit
// suffix cut ("20g" -> "20", "6.08km" -> "6.08"). "" when the token has none.
std::string normNumber(const std::string& s, size_t b, size_t len) {
  std::string out;
  for (size_t p = b; p < b + len; p++) {
    const char c = s[p];
    if (isDigit((unsigned char)c) || c == '.' || c == ':') out += c;
    else if (c != ',') break;
  }
  while (!out.empty() && (out.back() == '.' || out.back() == ':')) out.pop_back();
  return out;
}

bool estimateBefore(const Scan& sc, size_t k) {
  const Tok& t = sc.toks[k];
  if (t.b > 0 && (sc.s[t.b - 1] == '~')) return true;
  if (t.b >= 3 && u8at(sc.s, t.b - 3, 0xE2, 0x89, 0x88)) return true;   // approximately-equal sign
  static const char* const kEst[] = {"about", "around", "roughly", "approx", "approximately",
                                     "est", "estimated", "nearly", "almost", "under", "over"};
  return k > 0 && sameClause(sc, k - 1, k) && tokIn(sc.s, sc.toks[k - 1], kEst);
}

// Part of a range: "45-60", "45 to 60", or the second half of one.
bool inRange(const Scan& sc, size_t k) {
  const Tok& t = sc.toks[k];
  for (size_t p = t.b; p < t.b + t.len; p++)
    if (sc.s[p] == '-') return true;
  const size_t e = t.b + t.len;
  if (e < sc.s.size() && (sc.s[e] == '-' || isDash3(sc.s, e))) return true;
  if (t.b > 0 && (sc.s[t.b - 1] == '-' || (t.b >= 3 && isDash3(sc.s, t.b - 3)))) return true;
  const bool toAfter = k + 2 < sc.toks.size() && tokIs(sc.s, sc.toks[k + 1], "to") &&
                       (sc.toks[k + 2].flags & kTokNum);
  const bool toBefore = k >= 2 && tokIs(sc.s, sc.toks[k - 1], "to") && (sc.toks[k - 2].flags & kTokNum);
  return toAfter || toBefore;
}

bool numberInSet(const std::string& n, const std::vector<std::string>& set) {
  const double v = std::atof(n.c_str());
  for (const std::string& g : set)
    if (g == n || (n.find(':') == std::string::npos && g.find(':') == std::string::npos &&
                   std::atof(g.c_str()) == v))
      return true;
  return false;
}

std::vector<std::string> groundingNumbers(const std::string& g) {
  std::vector<std::string> out;
  size_t p = 0;
  while (p < g.size()) {
    if (!isDigit((unsigned char)g[p])) { p++; continue; }
    size_t e = p;
    while (e < g.size() && (isDigit((unsigned char)g[e]) ||
                            ((g[e] == '.' || g[e] == ',' || g[e] == ':') && e + 1 < g.size() &&
                             isDigit((unsigned char)g[e + 1]))))
      e++;
    out.push_back(normNumber(g, p, e - p));
    p = e;
  }
  return out;
}

// The span's numbers that no source holds, joined ("60, 200, 21").
std::string missingFigures(const Scan& sc, const Claim& cl, const std::vector<std::string>& ground) {
  std::string miss;
  for (size_t k = 0; k < sc.toks.size(); k++) {
    const Tok& t = sc.toks[k];
    if (t.b < cl.begin || t.b >= cl.end || !(t.flags & kTokNum)) continue;
    if (estimateBefore(sc, k) || inRange(sc, k)) continue;
    const std::string n = normNumber(sc.s, t.b, t.len);
    if (n.empty() || n == "0" || n == "1" || numberInSet(n, ground)) continue;
    if (miss.find(n) != std::string::npos) continue;
    miss += (miss.empty() ? "" : ", ") + n;
  }
  return miss;
}

// ---- gaps --------------------------------------------------------------------

enum GapVariant : uint8_t {
  kGapPlain = 0, kGapRamOnly = 1, kGapNotPinned = 2, kGapPinNoop = 3, kGapSomeStarted = 4,
  kGapNoVoiceReceipt = 5, kGapQueued = 6, kGapNoReceipt = 7,
  // causes
  kGapCauseOwner = 8,        // the owner's phone, app or connection
  kGapCauseFile = 9,         // a damaged or incomplete file
  kGapCauseSender = 10,      // the sender repeating himself, while the device repeated
  kGapCauseOkFailed = 11,    // "it is working", with a failure on record
  kGapCauseOkUnchecked = 12, // "it is working", nothing read to confirm it
  kGapCauseOwn = 13,         // the assistant's own subsystem (timed out, out of credit)
};

// Phrases that make a send claim a voice-message claim: the delivery itself named as
// spoken audio. A lone "voice" or "audio" is not enough ("sent you the recording of
// your voice" is a file send).
constexpr const char* kVoiceNouns[] = {
    "voice message*", "voice note*", "voice reply", "voice replies", "voice memo*",
    "voicemail*",     "voice-message*", "audio message*", "audio note*", "audio clip*",
    "audio version",  "spoken version", "as audio",       "as spoken audio",
};
constexpr int kVoiceBefore = 4, kVoiceAfter = 8;

bool backedBy(const TurnEffects& fx, uint32_t mask) {
  for (unsigned k = 0; k < (unsigned)FxKind::kCount; k++)
    if ((mask & (1u << k)) && fx.anyOk((FxKind)k)) return true;
  return false;
}

std::string excerptOf(const std::string& s, const Claim& cl) {
  size_t b = cl.begin, e = cl.end;
  while (b < e && (s[b] == ' ' || s[b] == '|' || s[b] == '*')) b++;
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '|' || s[e - 1] == '*')) e--;
  if (e - b > 100) {
    e = b + 100;
    while (e > b && ((unsigned char)s[e] & 0xC0) == 0x80) e--;
  }
  return s.substr(b, e - b);
}

bool pinBacked(const TurnEffects& fx) {
  return fx.anyOk(FxKind::MemPin) || fx.anyOkWith(FxKind::MemWrite, kFxPermanent);
}

// Per-class gap rules for the effect classes that have variants.
// R4 lives in the save cues' backs (kNotedBacks); a durability claim on a
// RAM-only note is checked as Durable. When the turn's only save was a RAM-only
// note, the note was kept until restart, not lost, and the line says so (a
// refused memory write keeps the plain line).
// Every memory write or update that did not land was queued (not refused).
bool failedWritesQueued(const TurnEffects& fx) {
  bool any = false;
  for (size_t i = 0; i < fx.size(); i++) {
    const Effect& e = fx.at(i);
    if (e.ok || (e.kind != FxKind::MemWrite && e.kind != FxKind::MemUpdate)) continue;
    if (!(e.flags & kFxQueued)) return false;
    any = true;
  }
  return any;
}

bool savedGap(const Cue& cue, const TurnEffects& fx, uint8_t* variant) {
  if (backedBy(fx, cue.backs)) return false;
  if (fx.anyOk(FxKind::ScratchRamOnly) && fx.countFailed(FxKind::MemWrite) == 0) *variant = kGapRamOnly;
  else if (failedWritesQueued(fx)) *variant = kGapQueued;
  return true;
}

// A delivery claim: a Telegram send backs it only with Telegram's receipt; a send
// that was only queued gets its own line (it may still arrive).
bool sentGap(const Cue& cue, const TurnEffects& fx, uint8_t* variant) {
  if (cue.backs != kSendBacks) return !backedBy(fx, cue.backs);
  if (sendDelivered(fx)) return false;
  if (sendQueuedOnly(fx)) *variant = kGapNoReceipt;
  return true;
}

// A cause against what this turn's records show (cue.backs: the supporting bit; for
// "it is working" the bit that contradicts it).
bool causeGap(const Cue& cue, uint32_t ev, uint8_t* variant) {
  if (cue.flags & kContraRepeat) {
    *variant = kGapCauseSender;
    return (ev & kEvDeviceRepeat) != 0;
  }
  if (cue.flags & kSelfOk) {
    *variant = (ev & cue.backs) ? kGapCauseOkFailed : kGapCauseOkUnchecked;
    return (ev & cue.backs) || !(ev & kEvHealthRead);
  }
  if (ev & cue.backs) return false;
  if (cue.backs & (kEvClient | kEvNetwork)) *variant = kGapCauseOwner;
  else if (cue.backs & kEvCorrupt) *variant = kGapCauseFile;
  else *variant = kGapCauseOwn;
  return true;
}

bool changedGap(const Cue& cue, const TurnEffects& fx, uint8_t* variant) {
  if (!(cue.flags & kPin)) return !backedBy(fx, cue.backs);
  if (pinBacked(fx)) return false;
  *variant = fx.anyOk(FxKind::MemPinNoop) ? kGapPinNoop : kGapNotPinned;
  return true;
}

bool durableGap(const Cue& cue, const TurnEffects& fx, uint8_t* variant) {
  if (cue.flags & kNeverDecay) {
    if (pinBacked(fx) || fx.anyOk(FxKind::MemPinNoop)) return false;
    *variant = kGapNotPinned;
    return true;
  }
  // A RAM-only scratchpad write makes "survives a restart" false - unless a
  // durable store also landed this turn (a memory, a file, a skill), which the
  // claim may be about (precision first).
  *variant = kGapRamOnly;
  return fx.anyOk(FxKind::ScratchRamOnly) && !backedBy(fx, kDurableStoreBacks);
}

// One claim against the ledger: true (and *variant set) when it is a gap.
bool isGap(const Cue& cue, const Claim& cl, const TurnEffects& fx, uint8_t* variant) {
  *variant = kGapPlain;
  switch (cue.cls) {
    case C::Saved: return savedGap(cue, fx, variant);
    case C::Spawned: {   // R3
      const int started = fx.countOk(FxKind::Spawn);
      if (started >= (cl.count ? cl.count : 1)) return false;
      if (started > 0) *variant = kGapSomeStarted;
      return true;
    }
    case C::Changed: return changedGap(cue, fx, variant);
    case C::Durable: return durableGap(cue, fx, variant);
    case C::Sent: return sentGap(cue, fx, variant);
    default: return !backedBy(fx, cue.backs);
  }
}

// "already" within 3 tokens before token i, in its clause.
bool alreadyBefore(const Scan& sc, size_t i) {
  for (size_t d = 1; d <= 3 && d <= i && sameClause(sc, i - d, i); d++)
    if (tokIs(sc.s, sc.toks[i - d], "already")) return true;
  return false;
}

// "already pinned" disclosures among the tokens in the byte span [b, e).
int pinDisclosuresIn(const Scan& sc, uint32_t b, uint32_t e) {
  int n = 0;
  for (size_t i = 0; i < sc.toks.size() && sc.toks[i].b < e; i++)
    if (sc.toks[i].b >= b && tokIs(sc.s, sc.toks[i], "pinned") && alreadyBefore(sc, i)) n++;
  return n;
}

// The reply itself says the memory was already pinned ("It was already pinned,
// so nothing changed"). Each disclosure answers one pin claim beside it ("the
// memory is pinned"), so the pin claims need no correction while disclosures
// cover them all; more pin claims than disclosures keep the line.
bool pinNoopDisclosed(const Scan& sc, const ClaimScan& scan) {
  const int disclosed = pinDisclosuresIn(sc, 0, 0xFFFFFFFFu);
  if (disclosed == 0) return false;
  int undisclosed = 0;
  for (const Claim& cl : scan.claims)
    if ((kCues[cl.cue].flags & kPin) && pinDisclosuresIn(sc, cl.begin, cl.end) == 0) undisclosed++;
  return undisclosed <= disclosed;
}

// "pinned" within 5 tokens after a check cue that ends at token k - 1, before a
// join: "I've confirmed it's pinned", "I checked that the memory is pinned", but
// not "I checked the weather and pinned the note".
bool pinnedRightAfter(const Scan& sc, size_t k) {
  for (size_t j = k; j < sc.toks.size() && j < k + 5 && sameClause(sc, j, k - 1); j++) {
    if (tokIn(sc.s, sc.toks[j], kClauseJoins)) return false;
    if (tokIs(sc.s, sc.toks[j], "pinned")) return true;
  }
  return false;
}

// A voice-message phrase near a send cue that starts at token i: starting up to
// kVoiceBefore tokens before it ("the voice message has been sent") or kVoiceAfter after
// its start ("sent you the summary as a voice message"), in its clause, not past a
// clause join ("I sent the text since voice messages failed" is a text send).
bool voiceNounNear(const Scan& sc, size_t i) {
  auto voiceAt = [&](size_t k) {
    for (const char* w : kVoiceNouns)
      if (matchPhrase(sc, k, w, nullptr)) return true;
    return false;
  };
  for (size_t k = i; k < sc.toks.size() && k < i + kVoiceAfter && sameClause(sc, k, i); k++) {
    if (k > i && tokIn(sc.s, sc.toks[k], kClauseJoins)) break;
    if (voiceAt(k)) return true;
  }
  for (size_t d = 1; d <= (size_t)kVoiceBefore && d <= i && sameClause(sc, i - d, i); d++) {
    if (tokIn(sc.s, sc.toks[i - d], kClauseJoins)) break;
    if (voiceAt(i - d)) return true;
  }
  return false;
}

// The cue's first token inside the claim's span (the claim records the clause).
size_t cueToken(const Scan& sc, const Cue& cue, const Claim& cl) {
  for (size_t k = 0; k < sc.toks.size() && sc.toks[k].b < cl.end; k++)
    if (sc.toks[k].b >= cl.begin && matchPhrase(sc, k, cue.phrase, nullptr)) return k;
  return sc.toks.size();
}

// The gap checks for one reply. The reply is tokenized at most once, and only
// when a figure claim, a pin no-op or a send claim needs its tokens.
struct GapCheck {
  Scan sc;
  const ClaimScan& scan;
  const ClaimEvidence& ev;
  bool tokenized = false;
  int8_t pinCovered = -1;   // pinNoopDisclosed, computed on first use
  std::vector<std::string> ground, recalled;

  GapCheck(const std::string& reply, const ClaimScan& cs, const ClaimEvidence& e) : sc(reply), scan(cs), ev(e) {}

  void ensureTokens() {
    if (tokenized) return;
    tokenize(sc);
    tokenized = true;
  }
  // The figure sources: the grounding numbers, plus recalled memories for the
  // cues that may use them.
  void loadFigureSources() {
    ensureTokens();
    ground = groundingNumbers(*ev.grounding);
    if (!ev.recalled) return;
    recalled = ground;
    const std::vector<std::string> more = groundingNumbers(*ev.recalled);
    recalled.insert(recalled.end(), more.begin(), more.end());
  }
  bool figureGap(const Cue& cue, const Claim& cl, ClaimGap& g) {
    if (!ev.grounding) return false;
    const bool recallOk = (cue.flags & kRecallOk) && ev.recalled;
    g.missing = missingFigures(sc, cl, recallOk ? recalled : ground);
    return !g.missing.empty();
  }
  // "I've confirmed it's pinned" after a pin call: the pin tool answered with
  // the memory's pin state, so a check claim whose object is the pin is backed.
  // Every other check claim still needs a lookup.
  bool pinStateChecked(const Cue& cue, const Claim& cl) {
    if (!ev.fx->anyOk(FxKind::MemPin) && !ev.fx->anyOk(FxKind::MemPinNoop)) return false;
    ensureTokens();
    for (size_t k = 0; k < sc.toks.size() && sc.toks[k].b < cl.end; k++) {
      if (sc.toks[k].b < cl.begin) continue;
      const size_t len = matchPhrase(sc, k, cue.phrase, nullptr);
      if (len) return pinnedRightAfter(sc, k + len);
    }
    return false;
  }
  // A send claim about a voice message needs a voice message's receipt.
  bool voiceClaim(const Cue& cue, const Claim& cl) {
    if (cue.cls != C::Sent || cue.backs != kSendBacks) return false;
    ensureTokens();
    const size_t k = cueToken(sc, cue, cl);
    return k < sc.toks.size() && voiceNounNear(sc, k);
  }
  bool effectGap(const Cue& cue, const Claim& cl, ClaimGap& g) {
    if (cue.cls == C::Cause) return causeGap(cue, ev.causeEvidence, &g.variant);
    if (voiceClaim(cue, cl)) {
      if (voiceDelivered(*ev.fx)) return false;
      g.variant = kGapNoVoiceReceipt;
      g.deviceOnly = true;
      return true;
    }
    if (!isGap(cue, cl, *ev.fx, &g.variant)) return false;
    if (cue.cls == C::Checked) return !pinStateChecked(cue, cl);
    if (g.variant != kGapPinNoop) return true;
    if (pinCovered < 0) {
      ensureTokens();
      pinCovered = pinNoopDisclosed(sc, scan) ? 1 : 0;
    }
    return pinCovered == 0;
  }
};

std::string joinNumbers(const std::string& csv) {
  // "60, 200, 21" -> "60, 200 and 21"
  const size_t last = csv.rfind(", ");
  return last == std::string::npos ? csv : csv.substr(0, last) + " and " + csv.substr(last + 2);
}

const char* causeWhat(uint8_t variant) {
  switch (variant) {
    case kGapCauseOwner:
      return "no tool result or device record this turn shows a problem with the owner's phone, app "
             "or connection; say what the records show, or that the cause is not recorded";
    case kGapCauseFile:
      return "no record this turn shows the file was damaged or incomplete; say what the records show, "
             "or that the cause is not recorded";
    case kGapCauseSender:
      return "the device's records show it repeated these deliveries itself, so do not say the owner "
             "sent them again";
    case kGapCauseOkFailed: return "the device's records this turn show a failure there, so it is not confirmed working";
    case kGapCauseOkUnchecked: return "nothing read this turn confirms it is working";
    default: return "no tool result or device record this turn shows that cause";
  }
}

const char* gapWhat(const ClaimGap& g) {
  switch (g.cls) {
    case C::Saved:
      if (g.variant == kGapQueued) return "the memory write was only queued; it is not stored yet";
      return g.variant == kGapRamOnly ? "it was kept in RAM only; saving failed" : "nothing was saved";
    case C::Deliverable: return "no file was written";
    case C::Spawned: return "fewer sub-agents were started than it says (or none)";
    case C::Sent:
      if (g.variant == kGapNoVoiceReceipt)
        return "no voice message reached Telegram (no delivery receipt with a message_id)";
      return g.variant == kGapNoReceipt
                 ? "the Telegram send was only queued: no delivery receipt (message_id) came back, so it "
                   "is not known to have arrived"
                 : "nothing was sent, spoken or scheduled";
    case C::Changed:
      if (g.variant == kGapPinNoop) return "that memory was already pinned, so nothing changed";
      return g.variant == kGapNotPinned ? "no pin ran" : "no change was made";
    case C::Durable: return g.variant == kGapNotPinned
                                ? "no pin or permanent memory write ran"
                                : "the scratchpad save failed; it is kept in RAM only until restart";
    case C::Checked: return "no lookup ran";
    case C::Figure: return "these numbers are in no source you were given";
    case C::Cause: return causeWhat(g.variant);
    default: return "this turn did not do it";
  }
}

}  // namespace

const char* claimClassSlug(ClaimClass c) {
  static const char* const kSlugs[] = {"saved",   "deliverable", "spawned", "sent",  "changed",
                                       "durable", "checked",     "figure",  "cause"};
  static_assert(sizeof(kSlugs) / sizeof(kSlugs[0]) == (size_t)ClaimClass::kCount,
                "every ClaimClass needs a slug");
  return (size_t)c < (size_t)ClaimClass::kCount ? kSlugs[(size_t)c] : "unknown";
}

size_t claimCueCount() { return kCueCount; }

ClaimCueInfo claimCue(size_t i) {
  const Cue& c = kCues[i < kCueCount ? i : 0];
  return ClaimCueInfo{c.cls, c.phrase, (c.flags & kSelf) != 0, (c.flags & kFaultCtx) != 0,
                      (c.flags & kLogObject) != 0};
}

ClaimScan scanClaims(const std::string& reply) {
  ClaimScan out;
  Scan sc(reply);
  if (mostlyNonEnglish(reply, sc.n)) {
    out.skippedNonEnglish = true;
    return out;
  }
  tokenize(sc);
  for (size_t i = 0; i < sc.toks.size(); i++) matchAt(sc, i, out);
  return out;
}

std::vector<ClaimGap> claimGaps(const std::string& reply, const ClaimScan& scan,
                                const ClaimEvidence& ev) {
  std::vector<ClaimGap> gaps;
  if (!ev.fx || scan.claims.empty()) return gaps;
  GapCheck chk(reply, scan, ev);
  bool figures = false;
  for (const Claim& cl : scan.claims) figures = figures || cl.cls == C::Figure;
  if (figures && ev.grounding) chk.loadFigureSources();
  for (const Claim& cl : scan.claims) {
    const Cue& cue = kCues[cl.cue];
    ClaimGap g;
    g.cls = cl.cls;
    const bool gap = cl.cls == C::Figure ? chk.figureGap(cue, cl, g) : chk.effectGap(cue, cl, g);
    if (!gap) continue;
    g.excerpt = excerptOf(reply, cl);
    gaps.push_back(std::move(g));
  }
  return gaps;
}

bool unbackedCause(const std::string& text, uint32_t causeEvidence, std::string* excerpt) {
  const ClaimScan scan = scanClaims(text);
  bool any = false;
  for (const Claim& cl : scan.claims) any = any || cl.cls == C::Cause;
  if (!any) return false;
  TurnEffects none;
  ClaimEvidence ev;
  ev.fx = &none;
  ev.causeEvidence = causeEvidence;
  for (const ClaimGap& g : claimGaps(text, scan, ev)) {
    if (g.cls != C::Cause) continue;
    if (excerpt) *excerpt = g.excerpt;
    return true;
  }
  return false;
}

namespace {

// What a record says about causes, phrase by phrase (lowercase; the record is
// folded first). Each phrase reads as a failure or a fact in the device's own words
// (its log lines, its labels, its tool results), never a mere mention of a part
// ("network: ok" backs nothing).
struct EvidencePhrases {
  uint32_t bit;
  const char* const* phrases;
  size_t n;
};

constexpr const char* kEvNetworkWords[] = {
    "timed out", "timeout", "connect fail", "connection failed", "connection reset", "connection refused",
    "connection lost", "lost connection", "network error", "network failure", "unreachable", "dns ",
    "offline", "no network", "not connected", "did not answer", "no answer", "handshake fail",
    "my wifi", "my wi-fi", "my connection", "my internet", "my network", "my signal",
};
constexpr const char* kEvClientWords[] = {
    "telegram client", "client error", "client closed", "your client", "my phone", "my app",
    "my telegram", "outbox",
};
constexpr const char* kEvCorruptWords[] = {
    "corrupt", "partial upload", "partially uploaded", "incomplete", "malformed", "damaged",
    "decode fail", "could not decode", "couldn't decode", "invalid image", "not a valid image",
};
constexpr const char* kEvRepeatWords[] = {
    "\"tgoffsetsaved\":false", "tgoffsetsaved: false", "[tgoffset]", "offset write", "redeliver",
    "re-served", "replay", "delivered again", "duplicate", ", sent ", "(sent ",
};
constexpr const char* kEvImageWords[] = {
    "couldn't be described", "could not be described", "(not read", "not read:", "could not be fetched",
    "couldn't be fetched", "\"visionlasterr\":{",
};
constexpr const char* kEvFailureWords[] = {
    "failed", "failure", "refused", "out of credit", "http 4", "http 5", "unavailable", "parked",
    "not stored", "storage full", "storage is almost full", "write failed", "low memory", "did not answer",
};
constexpr const char* kEvBillingWords[] = {
    "out of credit", "credit balance", "no credits", "insufficient_quota", "billing", "payment required",
    "http 402",
};
constexpr const char* kEvTimeoutWords[] = {"timed out", "timeout", "deadline"};

#define NIMBUS_EV(bit, arr) EvidencePhrases{bit, arr, sizeof(arr) / sizeof(arr[0])}
constexpr EvidencePhrases kEvidence[] = {
    NIMBUS_EV(kEvNetwork, kEvNetworkWords),     NIMBUS_EV(kEvClient, kEvClientWords),
    NIMBUS_EV(kEvCorrupt, kEvCorruptWords),     NIMBUS_EV(kEvDeviceRepeat, kEvRepeatWords),
    NIMBUS_EV(kEvImageFail, kEvImageWords),     NIMBUS_EV(kEvFailure, kEvFailureWords),
    NIMBUS_EV(kEvFailure, kEvImageWords),       NIMBUS_EV(kEvBilling, kEvBillingWords),
    NIMBUS_EV(kEvTimeout, kEvTimeoutWords),
};
#undef NIMBUS_EV

}  // namespace

uint32_t causeEvidenceOf(const std::string& record) {
  constexpr size_t kWindow = 32768;   // the newest 32 KB (inputs end with the owner's text)
  const size_t from = record.size() > kWindow ? record.size() - kWindow : 0;
  std::string low;
  low.reserve(record.size() - from);
  for (size_t p = from; p < record.size();) low += foldAt(record, p);
  uint32_t bits = 0;
  for (const EvidencePhrases& e : kEvidence) {
    if (bits & e.bit) continue;
    for (size_t i = 0; i < e.n; i++)
      if (low.find(e.phrases[i]) != std::string::npos) {
        bits |= e.bit;
        break;
      }
  }
  return bits;
}

std::string figureNumbers(const std::string& text, size_t cap) {
  // Bounded input as well as output: the newest 32 KB (the owner's message and
  // its captions close the input block).
  constexpr size_t kSourceScanMax = 32768;
  const std::string window =
      text.size() > kSourceScanMax ? text.substr(text.size() - kSourceScanMax) : text;
  std::string out;
  for (const std::string& n : groundingNumbers(window)) {
    if (n.empty()) continue;
    const std::string padded = " " + out + " ";
    if (padded.find(" " + n + " ") != std::string::npos) continue;
    if (out.size() + n.size() + 1 > cap) break;
    if (!out.empty()) out += ' ';
    out += n;
  }
  return out;
}

namespace {
// The phrase as whole words in folded text ("no live" is not in "no liver",
// "search failed" is not in "research failed").
bool hasWordPhrase(const std::string& low, const char* phrase) {
  const size_t n = std::strlen(phrase);
  for (size_t p = low.find(phrase); p != std::string::npos; p = low.find(phrase, p + 1)) {
    const bool startOk = p == 0 || !wordish((unsigned char)low[p - 1]);
    const bool endOk = p + n >= low.size() || !wordish((unsigned char)low[p + n]);
    if (startOk && endOk) return true;
  }
  return false;
}
}  // namespace

bool replyHedgesUnchecked(const std::string& reply) {
  static const char* const kHedges[] = {
      "not checked", "not been checked", "wasn't checked", "was not checked", "couldn't check",
      "could not check", "unverified", "not verified", "not confirmed", "unconfirmed",
      "couldn't confirm", "could not confirm", "can't confirm", "cannot confirm", "from memory",
      "guess", "no lookup", "not looked up", "not live", "not a live", "may be out of date",
      "might be out of date", "not checked live", "guessed", "guessing", "didn't actually run",
      "did not actually run", "lookup didn't run", "lookup did not run", "search didn't run",
      "search did not run", "lookup never ran", "lookup failed", "search failed",
      "never looked anything up", "never looked it up", "didn't look it up", "did not look it up",
      "no live", "don't have live", "do not have live",
  };
  std::string low;
  low.reserve(reply.size() < kScanMax ? reply.size() : kScanMax);
  for (size_t p = 0; p < reply.size() && p < kScanMax;) low += foldAt(reply, p);
  for (const char* h : kHedges)
    if (hasWordPhrase(low, h)) return true;
  return false;
}

bool unsourcedResultGap(const std::string& reply, const TurnEffects& fx, ClaimGap* out) {
  if (reply.find_first_not_of(" \t\r\n") == std::string::npos) return false;
  if (!fx.anyOk(FxKind::SubUnsourced) || fx.anyOk(FxKind::Read)) return false;
  if (replyHedgesUnchecked(reply)) return false;
  if (out) {
    *out = ClaimGap();
    out->cls = ClaimClass::Checked;
  }
  return true;
}

namespace {
// An excerpt quoted in the model-facing correction: no double quotes or control
// bytes, so reply text cannot close the quote or add lines to the instruction.
std::string quotedSafe(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c == '"') out += '\'';
    else if ((unsigned char)c < 0x20 || c == 0x7F) out += ' ';
    else out += c;
  }
  return out;
}
}  // namespace

std::string claimCorrection(const std::vector<ClaimGap>& gaps, const TurnEffects& fx) {
  if (gaps.empty()) return std::string();
  std::string out = "[CLAIM CHECK] Your reply states things this turn did not do:\n";
  size_t shown = 0;
  for (const ClaimGap& g : gaps) {
    if (shown++ == 4) break;
    out += "- ";
    if (!g.excerpt.empty()) out += "\"" + quotedSafe(g.excerpt) + "\": ";
    out += gapWhat(g);
    if (!g.missing.empty()) out += " (" + g.missing + ")";
    out += ".\n";
  }
  out += "This turn recorded: " + fx.summary() + ".\n";
  out += "Either do it now (your tools are still available) or rewrite the reply so it says "
         "only what actually happened. Do not mention this check.";
  return out;
}

namespace {
std::string causeLine(uint8_t variant) {
  switch (variant) {
    case kGapCauseOwner:
      return "(Note: the device has no record of a problem with your phone, app or connection, so that "
             "cause is not confirmed.)";
    case kGapCauseFile: return "(Note: nothing the device recorded shows the file was damaged or incomplete.)";
    case kGapCauseSender:
      return "(Note: the device recorded repeats on its own side, so these may not have come from you.)";
    case kGapCauseOkFailed: return "(Correction: the device recorded a failure there, so it is not confirmed working.)";
    case kGapCauseOkUnchecked: return "(Note: nothing was checked this turn to confirm that it is working.)";
    default: return "(Note: the device has no record of that cause.)";
  }
}
}  // namespace

std::string claimTailLine(const ClaimGap& g) {
  switch (g.cls) {
    case C::Saved:
      if (g.variant == kGapQueued)
        return "(Correction: that has not been saved yet; it is waiting on the device to be stored.)";
      return g.variant == kGapRamOnly
                 ? "(Note: this is kept only until the next restart; saving it permanently failed.)"
                 : "(Correction: that has not been saved.)";
    case C::Deliverable: return "(Correction: nothing has been written for that yet.)";
    case C::Spawned:   // the spawn check's own lines, so both say it the same way
      return g.variant == kGapSomeStarted ? kSpawnSomeNotStartedLine : kSpawnNotStartedLine;
    case C::Sent:
      if (g.variant == kGapNoVoiceReceipt) return "(Correction: no voice message was delivered.)";
      return g.variant == kGapNoReceipt
                 ? "(Note: that is queued for Telegram; the device has no delivery receipt for it yet.)"
                 : "(Correction: that has not been sent.)";
    case C::Changed:
      if (g.variant == kGapPinNoop) return "(Correction: that memory was already pinned, so nothing changed.)";
      return g.variant == kGapNotPinned ? "(Correction: nothing was pinned this turn.)"
                                        : "(Correction: nothing was changed this turn.)";
    case C::Durable:
      return g.variant == kGapNotPinned
                 ? "(Note: nothing was pinned this turn.)"
                 : "(Note: this is kept only until the next restart; saving it permanently failed.)";
    case C::Checked: return "(Note: this was not checked live; no lookup ran.)";
    case C::Figure:
      return "(Note: " + joinNumbers(g.missing) +
             " did not appear in the source I was given, so treat them as estimates.)";
    case C::Cause: return causeLine(g.variant);
    default: return std::string();
  }
}

// The statement inside a line, without its "(Correction: " or "(Note: " lead,
// so two classes that say the same thing ("nothing was pinned this turn") say
// it once.
static std::string statementOf(const std::string& line) {
  const size_t p = line.find(": ");
  return p == std::string::npos ? line : line.substr(p + 2);
}

std::string claimTail(const std::vector<ClaimGap>& gaps) {
  std::string out;
  for (size_t c = 0; c < (size_t)ClaimClass::kCount; c++) {
    for (const ClaimGap& g : gaps) {
      if ((size_t)g.cls != c) continue;
      const std::string line = claimTailLine(g);
      if (!line.empty() && out.find(statementOf(line)) == std::string::npos) out += "\n\n" + line;
      break;   // one line per class
    }
  }
  return out;
}

}  // namespace orch
}  // namespace nimbus
