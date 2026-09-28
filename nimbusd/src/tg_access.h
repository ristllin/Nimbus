#pragma once
#include <ctime>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "nimbus/mem_cap.h"
#include "nimbus/orch/rbac.h"
#include "nimbus/orch/tool_registry.h"
#include "nimbus/tg_updates.h"
#include "posix_fs.h"

// tg_access - who may talk to this instance over Telegram, and as whom.
//
// A Virtual Nimbus runs the PHYSICAL DEVICE's trust model (owner ruling, CUM-459):
// nobody is trusted by default, the owner approves each chat in the instance's own
// web app, and the existing RBAC roles decide what an approved chat may do. The
// device behavior mirrored here (src/agent/telegram.cpp, src/net/webui.cpp,
// src/agent/orchestrator.cpp):
//   * tgAllow - the allowlist on message.chat.id (never from.id). It fails CLOSED:
//     an EMPTY list rejects every chat (telegram.cpp allowed()).
//   * An unlisted sender lands in a 5-slot first-message approval ring (dedup by
//     chat, oldest dropped, RAM only) and gets no turn (pendingPush).
//   * Approve = add to tgAllow + the display-name sidecar + drop from the ring.
//   * Roles come from the SHARED TenantStore (lib/core rbac.h). An empty table is
//     adopted from the legacy lists exactly as the device's loadTenants() does
//     (owners -> Admin, the rest -> User, no owners -> the FIRST allow-listed chat
//     is Admin), and a chat's role follows the device's roleOfChat(): an explicit
//     row wins (an explicit Unknown is a revocation), else allow-listed -> User,
//     else Unknown.
//   * The instance's own token-gated surfaces (the web chat, the control API) are
//     the owner: the device answers Admin for web/serial/voice. Telegram chat ids
//     are numeric (Bot API chat.id), so a non-numeric chat id is a local surface.
//
// Hosted differences, each one fail-closed: an unlisted or revoked chat is told so
// in one polite, rate-limited message instead of silence (a hosted bot has no
// screen for the owner to notice a knock on); a revoked chat never reaches a turn
// (the device still runs one and relies on the prompt to refuse); there is no open
// access mode (a public bot on a hosted instance spends the owner's keys and reaches
// the owner's connectors); allowlist ids must be numeric.
//
// Durable on the instance volume (<data>/mem, so GET /backup carries it):
// telegram.txt holds the allowlist, owners, names and the consumed legacy seed, in
// the device's NVS key names; tenants.txt holds the RBAC table in the device's
// /data/tenants.txt format. The approval ring and the refusal memo are RAM, as on
// the device. Thread-safe: the Telegram poll thread, the HTTP thread and the engine
// thread all consult it.
namespace nimbusd {

class TelegramAccess {
 public:
  using Role = nimbus::orch::Role;
  using Quota = nimbus::orch::Quota;
  using Tenant = nimbus::orch::Tenant;

  enum class Admit { Serve, Unlisted, Revoked };
  struct Pending { std::string chatId, name, preview; };
  struct Member { std::string id, name; bool owner = false; };

  static constexpr size_t kPendingMax = 5;        // device s_pending[5]
  static constexpr size_t kIdMax = 23;            // device Pending.chatId[24]
  static constexpr int kNameMax = 32;             // device approvePending name cap
  static constexpr int kPreviewMax = 63;          // device Pending.preview[64]
  static constexpr time_t kRefusalCooldownS = 600;
  static constexpr size_t kRefusalMemoMax = 256;  // bounded: strangers cannot grow it
  static constexpr const char* kErrSave =
      "Couldn't save that change (storage full?), so nothing was changed.";
  static constexpr const char* kErrBadId = "Chat IDs are numbers, like 123456789.";
  static constexpr const char* kErrNotApproved = "That chat isn't approved yet. Approve it first.";
  static constexpr const char* kErrStillAllowed =
      "They are still approved. Remove them from the allowlist first, or set their role "
      "to unknown to revoke access.";

  explicit TelegramAccess(std::string dir) : dir_(std::move(dir)) { load(); }

  std::string accessPath() const { return dir_ + "/telegram.txt"; }
  std::string tenantsPath() const { return dir_ + "/tenants.txt"; }

  // A Telegram chat id: an optional '-' (groups) then digits, bounded.
  static bool isChatId(const std::string& s) {
    if (s.empty() || s.size() > kIdMax) return false;
    size_t i = s[0] == '-' ? 1 : 0;
    if (i == s.size()) return false;
    for (; i < s.size(); i++)
      if (s[i] < '0' || s[i] > '9') return false;
    return true;
  }

  // ---- the gate -------------------------------------------------------------
  bool allowed(const std::string& chat) const {
    std::lock_guard<std::mutex> lk(mu_);
    return inAllow(st_, chat);
  }
  Admit admit(const std::string& chat) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (!inAllow(st_, chat)) return Admit::Unlisted;
    return roleIn(st_, chat) == Role::Unknown ? Admit::Revoked : Admit::Serve;
  }
  // May a turn run for this chat? The instance's own surfaces always may; a
  // Telegram chat only when approved and not revoked. The engine seam checks this
  // too, so no producer (the poll, POST /api/message) can run a turn for a chat the
  // owner has not approved.
  bool mayConverse(const std::string& chat) const {
    return !isChatId(chat) || admit(chat) == Admit::Serve;
  }
  Role roleOf(const std::string& chat) const {
    std::lock_guard<std::mutex> lk(mu_);
    return roleIn(st_, chat);
  }
  // The WHOLE principal (namespace, role, quota) - what the device's principalFor
  // hands every rail, so a role or limit change applies on the next message.
  nimbus::orch::Principal principalFor(const std::string& chat) const {
    std::lock_guard<std::mutex> lk(mu_);
    Quota q;
    if (const Tenant* t = st_.tenants.find(chat)) q = t->quota;
    return nimbus::orch::principalForRole(chat, roleIn(st_, chat), q);
  }
  // Display name for the prompt's speaker line ("" when none).
  std::string labelOf(const std::string& chat) const {
    std::lock_guard<std::mutex> lk(mu_);
    const Tenant* t = st_.tenants.find(chat);
    if (t && !t->label.empty()) return t->label;
    auto it = st_.names.find(chat);
    return it == st_.names.end() ? std::string() : it->second;
  }

  // ---- snapshots for the web surface ----------------------------------------
  std::string allowCsv() const {
    std::lock_guard<std::mutex> lk(mu_);
    return join(st_.allow);
  }
  bool allowEmpty() const {
    std::lock_guard<std::mutex> lk(mu_);
    return st_.allow.empty();
  }
  // The allow-listed chats with their names. `owner` is the EFFECTIVE role (Admin),
  // not the device's legacy first-entry guess, so the chip the page draws for a chat
  // with no tenant row always matches what the gate will do.
  std::vector<Member> members() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<Member> out;
    for (const auto& id : st_.allow) {
      auto it = st_.names.find(id);
      out.push_back({id, it == st_.names.end() ? std::string() : it->second,
                     roleIn(st_, id) == Role::Admin});
    }
    return out;
  }
  std::vector<Pending> pending() const {
    std::lock_guard<std::mutex> lk(mu_);
    return pending_;
  }
  std::vector<Tenant> tenants(size_t* admins = nullptr) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (admins) *admins = st_.tenants.adminCount();
    return st_.tenants.all();
  }
  bool quotaOf(const std::string& chat, Quota& out) const {
    std::lock_guard<std::mutex> lk(mu_);
    const Tenant* t = st_.tenants.find(chat);
    if (!t) return false;
    out = t->quota;
    return true;
  }

  // ---- inbound bookkeeping (the poll thread) ---------------------------------
  // Queue an unlisted sender for one-tap approval (device pendingPush): dedup by
  // chat, the oldest dropped when full. The name and preview are the sender's own
  // text, so both are sanitized and capped (UTF-8 safe).
  void notePending(const std::string& chat, const std::string& from, const std::string& text) {
    if (!isChatId(chat)) return;
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& p : pending_)
      if (p.chatId == chat) return;
    if (pending_.size() >= kPendingMax) pending_.erase(pending_.begin());
    const std::string nm = cleanName(from);
    pending_.push_back({chat, nm.empty() ? std::string("?") : nm, capUtf8(text, kPreviewMax)});
  }

  // Whether to send this chat the refusal now: at most once per cooldown per chat,
  // so a stranger (or a flood) cannot make the bot spam or trip Telegram's limits.
  bool takeRefusal(const std::string& chat, time_t now) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = refused_.find(chat);
    if (it != refused_.end() && now - it->second < kRefusalCooldownS) return false;
    if (it == refused_.end() && refused_.size() >= kRefusalMemoMax) evictOldestRefusal();
    refused_[chat] = now;
    return true;
  }

  // ---- owner management (the web surface) ------------------------------------
  // POST /api/orch tgAllow=<csv> (device webui.cpp): replace the whole list. Every
  // id must be a chat id, or nothing changes.
  bool setAllowCsv(const std::string& csv, std::string& err) {
    std::vector<std::string> ids;
    if (!parseIds(csv, ids)) { err = kErrBadId; return false; }
    if (!mutate([&](State& s, std::string&) { s.allow = ids; return true; }, err)) return false;
    for (const auto& id : ids) dropPending(id);
    return true;
  }

  // Approve / add (device approvePending): idempotent add, name upsert, drop from
  // the ring. An explicit approval is an explicit grant, so it also lifts a revoked
  // (Unknown) row back to User - otherwise re-adding someone could never let them in.
  bool approve(const std::string& id, const std::string& name, std::string& err) {
    if (!isChatId(id)) { err = kErrBadId; return false; }
    const std::string nm = cleanName(name);
    const bool ok = mutate([&](State& s, std::string&) {
      if (!inAllow(s, id)) s.allow.push_back(id);
      if (!nm.empty()) s.names[id] = nm;
      std::string e;
      if (s.tenants.known(id) && s.tenants.roleOf(id) == Role::Unknown)
        s.tenants.setRole(id, Role::User, e);
      return true;
    }, err);
    if (ok) dropPending(id);
    return ok;
  }

  void deny(const std::string& id) { dropPending(id); }   // device: a session tombstone

  // Remove from the allowlist (device /api/telegram/remove): prune an explicit owners
  // list, then drop the RBAC row. When the row cannot go (the last admin) the chat is
  // still off the allowlist, so the gate refuses them; `note` says why the row stayed.
  bool remove(const std::string& id, std::string& note, std::string& err) {
    note.clear();
    return mutate([&](State& s, std::string&) {
      eraseId(s.allow, id);
      eraseId(s.owners, id);
      std::string terr;
      if (s.tenants.known(id) && !s.tenants.remove(id, terr)) {
        std::string rerr;
        if (!s.tenants.setRole(id, Role::Unknown, rerr)) note = terr;
      }
      return true;
    }, err);
  }

  // Display-name sidecar only (device /api/telegram/rename). "" erases the name.
  bool rename(const std::string& id, const std::string& name, std::string& err) {
    if (!isChatId(id)) { err = kErrBadId; return false; }
    const std::string nm = cleanName(name);
    return mutate([&](State& s, std::string&) {
      if (nm.empty()) s.names.erase(id);
      else s.names[id] = nm;
      return true;
    }, err);
  }

  // Legacy owner flag (device /api/telegram/role): materialize the effective owner
  // set, then add or drop `id`. Never leaves zero owners.
  bool setOwner(const std::string& id, bool owner, std::string& err) {
    return mutate([&](State& s, std::string& e) {
      if (owner && !inAllow(s, id)) { e = kErrNotApproved; return false; }
      std::vector<std::string> cur = s.owners;
      if (cur.empty() && !s.allow.empty()) cur.push_back(s.allow.front());
      eraseId(cur, id);
      if (owner) cur.push_back(id);
      if (cur.empty()) { e = "At least one owner is required."; return false; }
      s.owners = cur;
      return true;
    }, err);
  }

  // RBAC writes (device /api/tenant). No pre-seeding: only an approved chat can be
  // given a real role; revoking (Unknown) is always allowed. The last-admin rule is
  // the shared TenantStore's.
  bool setRole(const std::string& id, Role r, std::string& err) {
    return mutate([&](State& s, std::string& e) {
      if (r != Role::Unknown && !inAllow(s, id)) { e = kErrNotApproved; return false; }
      return s.tenants.setRole(id, r, e);
    }, err);
  }
  bool setQuota(const std::string& id, const Quota& q, std::string& err) {
    return mutate([&](State& s, std::string& e) { return s.tenants.setQuota(id, q, e); }, err);
  }
  // Removing a ROW is not revoking: an allow-listed chat with no row falls back to
  // User, so a row only goes once the chat is off the allowlist (device parity).
  bool removeTenant(const std::string& id, std::string& err) {
    return mutate([&](State& s, std::string& e) {
      if (inAllow(s, id)) { e = kErrStillAllowed; return false; }
      return s.tenants.remove(id, e);
    }, err);
  }

  // Legacy NIMBUSD_TG_CHAT_ID: an optional one-time SEED, never an ongoing gate.
  // Applied once per value: only onto an EMPTY allowlist (a list the owner already
  // manages in the web app is left alone), and a seed the owner later removes never
  // comes back on restart. Returns whether the id was added; `note` says what
  // happened for the boot log.
  bool seedFromEnv(const std::string& raw, std::string& note) {
    const std::string id = trim(raw);
    note.clear();
    if (id.empty()) return false;
    if (!isChatId(id)) { note = "ignored: not a numeric chat id"; return false; }
    bool added = false;
    std::string err;
    mutate([&](State& s, std::string&) {
      if (s.seed == id) { note = "already applied once"; return false; }
      s.seed = id;
      if (!s.allow.empty()) { note = "not added: the allowlist is managed in the web app"; return true; }
      s.allow.push_back(id);
      added = true;
      note = "added to the allowlist";
      return true;
    }, err);
    if (!err.empty()) { note = err; added = false; }
    if (added) dropPending(id);
    return added;
  }

  // Test/diagnostic view of the consumed seed value.
  std::string seedApplied() const {
    std::lock_guard<std::mutex> lk(mu_);
    return st_.seed;
  }

 private:
  struct State {
    std::vector<std::string> allow;            // tgAllow, in approval order
    std::vector<std::string> owners;           // tgOwners (legacy owner subset)
    std::map<std::string, std::string> names;  // tgNames sidecar
    std::string seed;                          // the NIMBUSD_TG_CHAT_ID value consumed
    nimbus::orch::TenantStore tenants;         // the shared RBAC table
  };

  static bool contains(const std::vector<std::string>& v, const std::string& id) {
    for (const auto& x : v)
      if (x == id) return true;
    return false;
  }
  static bool inAllow(const State& s, const std::string& id) {
    return !id.empty() && contains(s.allow, id);   // empty list => false (fail closed)
  }
  static void eraseId(std::vector<std::string>& v, const std::string& id) {
    for (size_t i = 0; i < v.size();) {
      if (v[i] == id) v.erase(v.begin() + (long)i);
      else i++;
    }
  }
  // Device roleOfChat() with the hosted local-surface rule (see the file header).
  static Role roleIn(const State& s, const std::string& chat) {
    if (!isChatId(chat)) return Role::Admin;
    if (s.tenants.known(chat)) return s.tenants.roleOf(chat);
    return inAllow(s, chat) ? Role::User : Role::Unknown;
  }
  // The device's loadTenants(): an EMPTY table is rebuilt from the legacy lists, so
  // the first approved chat becomes the admin.
  static void adoptIfEmpty(State& s) {
    if (s.tenants.all().empty() && !s.allow.empty()) s.tenants.adoptLegacy(s.owners, s.allow);
  }

  static std::string trim(const std::string& v) {
    const size_t b = v.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    const size_t e = v.find_last_not_of(" \t\r\n");
    return v.substr(b, e - b + 1);
  }
  static std::string capUtf8(const std::string& s, int maxBytes) {
    return s.substr(0, (size_t)nimbus::utf8CapLen(s.c_str(), (int)s.size(), maxBytes));
  }
  // A display name is the sender's ATTACKER-CONTROLLED Telegram name: drop the
  // sidecar delimiters and control characters (device approvePending), then cap.
  static std::string cleanName(const std::string& raw) {
    std::string o;
    for (char ch : raw) {
      const unsigned char c = (unsigned char)ch;
      o += (ch == ',' || ch == ':' || c < 0x20 || c == 0x7f) ? ' ' : ch;
    }
    return capUtf8(trim(o), kNameMax);
  }
  static std::string join(const std::vector<std::string>& v) {
    std::string o;
    for (const auto& x : v) o += (o.empty() ? "" : ",") + x;
    return o;
  }
  // Split a comma list into trimmed, de-duplicated chat ids. False if any entry is
  // not a chat id.
  static bool parseIds(const std::string& csv, std::vector<std::string>& out) {
    out.clear();
    std::stringstream ss(csv);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
      tok = trim(tok);
      if (tok.empty()) continue;
      if (!isChatId(tok)) return false;
      if (!contains(out, tok)) out.push_back(tok);
    }
    return true;
  }

  void dropPending(const std::string& id) {
    std::lock_guard<std::mutex> lk(mu_);
    for (size_t i = 0; i < pending_.size(); i++)
      if (pending_[i].chatId == id) { pending_.erase(pending_.begin() + (long)i); return; }
  }
  void evictOldestRefusal() {
    auto oldest = refused_.begin();
    for (auto it = refused_.begin(); it != refused_.end(); ++it)
      if (it->second < oldest->second) oldest = it;
    if (oldest != refused_.end()) refused_.erase(oldest);
  }

  // Apply `fn` to a COPY of the durable state, persist it, and only then publish it:
  // a change that did not reach the disk is never reported as done (the device's
  // tenant writes roll back the same way).
  bool mutate(const std::function<bool(State&, std::string&)>& fn, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    State next = st_;
    if (!fn(next, err)) return false;
    adoptIfEmpty(next);
    if (!save(next)) {
      save(st_);   // best effort: put the last good state back on disk
      err = kErrSave;
      return false;
    }
    st_ = std::move(next);
    return true;
  }

  static std::string serialize(const State& s) {
    std::string names;
    for (const auto& kv : s.names) names += (names.empty() ? "" : ",") + kv.first + ":" + kv.second;
    return "tgAllow=" + join(s.allow) + "\ntgOwners=" + join(s.owners) + "\ntgNames=" + names +
           "\ntgSeed=" + s.seed + "\n";
  }
  static void parseNames(const std::string& v, std::map<std::string, std::string>& out) {
    std::stringstream ss(v);
    std::string entry;
    while (std::getline(ss, entry, ',')) {
      const size_t colon = entry.find(':');
      if (colon == std::string::npos) continue;
      const std::string id = trim(entry.substr(0, colon));
      if (isChatId(id)) out[id] = cleanName(entry.substr(colon + 1));
    }
  }
  // Tolerant parse: unknown keys and malformed ids are dropped, never fatal.
  static void parse(const std::string& blob, State& s) {
    std::stringstream ss(blob);
    std::string line;
    while (std::getline(ss, line)) {
      const size_t eq = line.find('=');
      if (eq == std::string::npos) continue;
      const std::string k = line.substr(0, eq), v = trim(line.substr(eq + 1));
      std::vector<std::string> ids;
      if (k == "tgAllow" || k == "tgOwners") {
        std::stringstream is(v);
        std::string tok;
        while (std::getline(is, tok, ','))
          if (isChatId(trim(tok)) && !contains(ids, trim(tok))) ids.push_back(trim(tok));
        (k == "tgAllow" ? s.allow : s.owners) = ids;
      } else if (k == "tgNames") {
        parseNames(v, s.names);
      } else if (k == "tgSeed") {
        s.seed = v;
      }
    }
  }
  bool save(const State& s) const {
    return fsutil::writeFileAtomic(tenantsPath(), s.tenants.dump()) &&
           fsutil::writeFileAtomic(accessPath(), serialize(s));
  }
  // Rehydrate, then run the device's boot-time adoption once (an allowlist written by
  // an earlier build with no tenants file gets its admin, same as a device upgrade).
  void load() {
    std::string blob;
    if (fsutil::readFile(accessPath(), blob)) parse(blob, st_);
    std::string tb;
    if (fsutil::readFile(tenantsPath(), tb)) st_.tenants.load(tb);
    if (st_.tenants.all().empty() && !st_.allow.empty()) {
      adoptIfEmpty(st_);
      save(st_);
    }
  }

  std::string dir_;
  mutable std::mutex mu_;
  State st_;
  std::vector<Pending> pending_;          // first-message approval ring (RAM)
  std::map<std::string, time_t> refused_; // last refusal per chat (RAM, bounded)
};

}  // namespace nimbusd
