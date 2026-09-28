#pragma once
#include <algorithm>
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
//   * Roles come from the SHARED TenantStore (lib/core rbac.h) and the device's
//     roleOfChat(): an explicit row wins (an explicit Unknown is a revocation), else
//     allow-listed -> User, else Unknown.
//   * The instance's own token-gated surfaces (the web chat, the control API) are
//     the owner: the device answers Admin for web/serial/voice. Telegram chat ids
//     are numeric (Bot API chat.id), so a non-numeric chat id is a local surface.
//
// Hosted differences, each one fail-closed:
//   * Approving a chat grants User. Admin is only ever an explicit grant (the role
//     chip, POST /api/tenant, or the legacy NIMBUSD_TG_CHAT_ID seed). The device
//     adopts the FIRST allow-listed chat as admin; on a hosted instance the owner
//     lives in the web app, so that rule would hand owner rights to whoever the
//     owner happened to approve first.
//   * A role only exists while its chat is on the allowlist: every way off the list
//     erases the row, so re-adding a chat later never revives an old grant.
//   * An unlisted or revoked chat is told so in one polite, rate-limited message
//     instead of silence (a hosted bot has no screen where the owner would notice a
//     knock), and a revoked chat never reaches a turn (the device runs one and
//     relies on the prompt to refuse).
//   * There is no open access mode, and allowlist ids must be numeric.
//
// Durable on the instance volume as ONE file (<data>/mem/telegram.txt, so GET
// /backup carries it and a crash can never leave the allowlist and the roles out
// of step): the allowlist, names and consumed seed under the device's NVS key
// names, plus the RBAC table in TenantStore::dump form. A missing or unreadable
// file is an empty allowlist: fail closed. The approval ring and the refusal memo
// are RAM, as on the device. Thread-safe: the Telegram poll thread, the HTTP thread
// and the engine thread all consult it.
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
  static constexpr const char* kErrNoTenant = "no such tenant";   // lib/core's wording

  explicit TelegramAccess(std::string dir) : dir_(std::move(dir)) { load(); }

  std::string path() const { return dir_ + "/telegram.txt"; }

  // A Telegram chat id: an optional '-' (groups) then digits, bounded.
  static bool isChatId(const std::string& s) {
    if (s.empty() || s.size() > kIdMax) return false;
    const size_t digits = s[0] == '-' ? 1 : 0;
    return digits < s.size() &&
           std::all_of(s.begin() + (long)digits, s.end(), [](char c) { return c >= '0' && c <= '9'; });
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
  // The allow-listed chats with their names; `owner` is the effective role (Admin).
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
  // A tenant row's explicit limits; false when the chat has no row.
  bool quotaOf(const std::string& chat, Quota& out) const {
    std::lock_guard<std::mutex> lk(mu_);
    const Tenant* t = st_.tenants.find(chat);
    if (t) out = t->quota;
    return t != nullptr;
  }

  // ---- inbound bookkeeping (the poll thread) ---------------------------------
  // Queue an unlisted sender for one-tap approval (device pendingPush): dedup by
  // chat, the oldest dropped when full. The name and preview are the sender's own
  // text, so both are sanitized and capped (UTF-8 safe). A chat approved since the
  // caller checked is not queued.
  void notePending(const std::string& chat, const std::string& from, const std::string& text) {
    if (!isChatId(chat)) return;
    std::lock_guard<std::mutex> lk(mu_);
    if (inAllow(st_, chat) || pendingAt(chat) != pending_.end()) return;
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
    if (it == refused_.end() && refused_.size() >= kRefusalMemoMax) {
      refused_.erase(std::min_element(refused_.begin(), refused_.end(),
                                      [](const auto& a, const auto& b) { return a.second < b.second; }));
    }
    refused_[chat] = now;
    return true;
  }

  // ---- owner management (the web surface) ------------------------------------
  // POST /api/orch tgAllow=<csv> (device webui.cpp): replace the whole list. Every
  // id must be a chat id, or nothing changes. A chat that leaves the list loses its
  // role with it.
  bool setAllowCsv(const std::string& csv, std::string& err) {
    std::vector<std::string> ids;
    if (!parseIds(csv, ids)) { err = kErrBadId; return false; }
    if (!mutate([&](State& s, std::string&) { s.allow = ids; return true; }, err)) return false;
    for (const auto& id : ids) dropPending(id);
    return true;
  }

  // Approve / add (device approvePending): idempotent add, name upsert, drop from
  // the ring. A newly approved chat is a User. An explicit approval is an explicit
  // grant, so it also lifts a revoked (Unknown) row back to User.
  bool approve(const std::string& id, const std::string& name, std::string& err) {
    if (!isChatId(id)) { err = kErrBadId; return false; }
    const std::string nm = cleanName(name);
    const bool ok = mutate([&](State& s, std::string&) {
      if (!inAllow(s, id)) s.allow.push_back(id);
      if (!nm.empty()) s.names[id] = nm;
      std::string e;
      if (s.tenants.known(id) && s.tenants.roleOf(id) == Role::Unknown) s.tenants.setRole(id, Role::User, e);
      return true;
    }, err);
    if (ok) dropPending(id);
    return ok;
  }

  void deny(const std::string& id) { dropPending(id); }   // device: a session tombstone

  // Remove from the allowlist (device /api/telegram/remove). The chat's role goes
  // with it, even the last Telegram admin's: the web app is always the owner, so the
  // instance can never become unadministrable.
  bool remove(const std::string& id, std::string& err) {
    return mutate([&](State& s, std::string&) {
      s.allow.erase(std::remove(s.allow.begin(), s.allow.end(), id), s.allow.end());
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

  // RBAC writes (device /api/tenant). No pre-seeding: only an approved chat can be
  // given a real role; revoking (Unknown) an unlisted chat is a no-op (it is already
  // refused). The last-admin rule is the shared TenantStore's.
  bool setRole(const std::string& id, Role r, std::string& err) {
    if (!isChatId(id)) { err = kErrBadId; return false; }
    return mutate([&](State& s, std::string& e) {
      if (!inAllow(s, id)) {
        if (r == Role::Unknown) return true;
        e = kErrNotApproved;
        return false;
      }
      return s.tenants.setRole(id, r, e);
    }, err);
  }
  bool setQuota(const std::string& id, const Quota& q, std::string& err) {
    if (!isChatId(id)) { err = kErrBadId; return false; }
    return mutate([&](State& s, std::string& e) { return s.tenants.setQuota(id, q, e); }, err);
  }
  // Removing a ROW is not revoking: an allow-listed chat with no row falls back to
  // User, so a row only goes with its chat (device parity: refused while approved).
  // Here rows only exist for approved chats, so this always answers why not.
  bool removeTenant(const std::string& id, std::string& err) const {
    if (!isChatId(id)) err = kErrBadId;
    else err = allowed(id) ? kErrStillAllowed : kErrNoTenant;
    return false;
  }

  // Legacy NIMBUSD_TG_CHAT_ID: an optional SEED, never an ongoing gate. It names the
  // owner's chat, so the seeded chat is an admin. It is applied only onto an EMPTY
  // allowlist (a list the owner manages in the web app is left alone), and a value is
  // consumed once: a seeded chat the owner removes does not come back on restart.
  // (Only the most recent value is remembered.) Returns the boot-log line.
  std::string seedFromEnv(const std::string& raw) {
    const std::string id = trim(raw);
    if (id.empty()) return std::string();
    if (!isChatId(id)) return "ignored: not a numeric chat id";
    std::string note, err;
    const bool changed = mutate([&](State& s, std::string&) {
      if (s.seed == id) { note = "already applied once"; return false; }
      s.seed = id;
      if (!s.allow.empty()) { note = "not added: the allowlist is managed in the web app"; return true; }
      s.allow.push_back(id);
      std::string e;
      s.tenants.setRole(id, Role::Admin, e);
      note = "added to the allowlist as the admin";
      return true;
    }, err);
    return changed || err.empty() ? note : err;
  }

  // Test/diagnostic view of the consumed seed value.
  std::string seedApplied() const {
    std::lock_guard<std::mutex> lk(mu_);
    return st_.seed;
  }

 private:
  struct State {
    std::vector<std::string> allow;            // tgAllow, in approval order
    std::map<std::string, std::string> names;  // tgNames sidecar
    std::string seed;                          // the NIMBUSD_TG_CHAT_ID value consumed
    nimbus::orch::TenantStore tenants;         // the shared RBAC table
  };

  static bool inAllow(const State& s, const std::string& id) {
    // An empty list holds nothing: fail closed.
    return !id.empty() && std::find(s.allow.begin(), s.allow.end(), id) != s.allow.end();
  }
  // Device roleOfChat() with the hosted local-surface rule (see the file header).
  static Role roleIn(const State& s, const std::string& chat) {
    if (!isChatId(chat)) return Role::Admin;
    if (s.tenants.known(chat)) return s.tenants.roleOf(chat);
    return inAllow(s, chat) ? Role::User : Role::Unknown;
  }
  // Keep the invariant "a row exists only for an allow-listed chat". The table is
  // rebuilt rather than edited because TenantStore refuses to remove its last admin,
  // and a chat that is off the list is no admin of anything.
  static void pruneRows(State& s) {
    const auto& rows = s.tenants.all();
    if (std::all_of(rows.begin(), rows.end(), [&s](const Tenant& t) { return inAllow(s, t.chatId); }))
      return;
    nimbus::orch::TenantStore kept;
    std::string e;
    for (const Tenant& t : rows) {
      if (!inAllow(s, t.chatId)) continue;
      kept.setRole(t.chatId, t.role, e);
      kept.setQuota(t.chatId, t.quota, e);
    }
    s.tenants = std::move(kept);
  }

  static std::string trim(const std::string& v) {
    const size_t b = v.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    return v.substr(b, v.find_last_not_of(" \t\r\n") - b + 1);
  }
  static std::string capUtf8(const std::string& s, int maxBytes) {
    return s.substr(0, (size_t)nimbus::utf8CapLen(s.c_str(), (int)s.size(), maxBytes));
  }
  // A display name is the sender's ATTACKER-CONTROLLED Telegram name: drop the
  // sidecar delimiters and control characters (device approvePending), then cap.
  static std::string cleanName(const std::string& raw) {
    std::string o = raw;
    std::replace_if(o.begin(), o.end(), [](char ch) {
      const unsigned char c = (unsigned char)ch;
      return ch == ',' || ch == ':' || c < 0x20 || c == 0x7f;
    }, ' ');
    return capUtf8(trim(o), kNameMax);
  }
  static std::string join(const std::vector<std::string>& v) {
    std::string o;
    for (const auto& x : v) o += (o.empty() ? "" : ",") + x;
    return o;
  }
  // Split a comma list into trimmed, de-duplicated chat ids, in order. `strict`
  // refuses the whole list on a bad entry (a web write); otherwise a bad entry is
  // dropped (a tolerant reload).
  static bool splitIds(const std::string& csv, bool strict, std::vector<std::string>& out) {
    out.clear();
    std::stringstream ss(csv);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
      const std::string id = trim(tok);
      if (id.empty()) continue;
      if (!isChatId(id)) {
        if (strict) return false;
        continue;
      }
      if (std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
    }
    return true;
  }
  static bool parseIds(const std::string& csv, std::vector<std::string>& out) {
    return splitIds(csv, /*strict=*/true, out);
  }

  std::vector<Pending>::iterator pendingAt(const std::string& id) {
    return std::find_if(pending_.begin(), pending_.end(),
                        [&id](const Pending& p) { return p.chatId == id; });
  }
  void dropPending(const std::string& id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = pendingAt(id);
    if (it != pending_.end()) pending_.erase(it);
  }

  // Apply `fn` to a COPY of the durable state, persist it, and only then publish it:
  // a change that did not reach the disk is never reported as done (the device's
  // tenant writes roll back the same way). `fn` returning false means "no change".
  bool mutate(const std::function<bool(State&, std::string&)>& fn, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    State next = st_;
    if (!fn(next, err)) return false;
    pruneRows(next);
    if (!fsutil::writeFileAtomic(path(), serialize(next))) {
      err = kErrSave;
      return false;
    }
    st_ = std::move(next);
    return true;
  }

  static std::string serialize(const State& s) {
    std::string names;
    for (const auto& kv : s.names) names += (names.empty() ? "" : ",") + kv.first + ":" + kv.second;
    std::string rows = s.tenants.dump();   // \x1E/\x1F separated; ids are digits, no labels
    rows.erase(std::remove(rows.begin(), rows.end(), '\n'), rows.end());
    return "tgAllow=" + join(s.allow) + "\ntgNames=" + names + "\ntgSeed=" + s.seed +
           "\ntenants=" + rows + "\n";
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
  // Tolerant parse: unknown keys and malformed entries are dropped, never fatal, and
  // a torn or missing file reads as an empty allowlist (fail closed).
  static void parse(const std::string& blob, State& s) {
    std::stringstream ss(blob);
    std::string line;
    while (std::getline(ss, line)) {
      const size_t eq = line.find('=');
      if (eq == std::string::npos) continue;
      const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
      if (k == "tgAllow") splitIds(v, /*strict=*/false, s.allow);
      else if (k == "tgNames") parseNames(v, s.names);
      else if (k == "tgSeed") s.seed = trim(v);
      else if (k == "tenants") s.tenants.load(v);
    }
    pruneRows(s);
  }
  void load() {
    std::string blob;
    if (fsutil::readFile(path(), blob)) parse(blob, st_);
  }

  std::string dir_;
  mutable std::mutex mu_;
  State st_;
  std::vector<Pending> pending_;          // first-message approval ring (RAM)
  std::map<std::string, time_t> refused_; // last refusal per chat (RAM, bounded)
};

}  // namespace nimbusd
