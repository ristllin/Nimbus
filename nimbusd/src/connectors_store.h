#pragma once
// connectors_store - the hosted instance's connector registry (CUM-424).
//
// The device keeps its connectors in ONE NVS JSON-array blob (key "connectors",
// shape in src/agent/connectors.h) behind the token-gated /api/connectors
// endpoint. A hosted instance has no NVS, so this is the same blob as a file:
// <mem>/connectors.json on the instance PVC (it rides the /backup tar with the
// rest of the mem tree). The blob carries pasted bearer tokens, so it is written
// with the SECRETS discipline (0600 from creation, then an atomic rename - the
// same rationale as NimbusdRig::saveSecrets, never the 0644 writeFileAtomic).
//
// The web contract (GET sanitization, POST blob/del/patch semantics, the 3500B
// cap, save-time validation, patch-preserves-secrets) mirrors the device handler
// in src/net/webui.cpp byte-for-byte: the SAME web UI drives both, and the HIL
// suite's expectations must hold on either. Parsing and validation are the
// portable helpers in nimbus/orch/connectors_wire.h - nothing here re-derives a
// rule the device already encodes.
//
// v1 scope: tiers T1 (Studio-authenticated, no secret stored) and T2 (static
// bearer in "tok") resolve fully. T3 (OAuth refresh broker) entries are stored
// and validated but the mint is not implemented on a hosted instance yet -
// bearerForName returns "" for them, and the attach builders already skip an
// unauthenticated first-party connector rather than send a doomed request.
//
// Threading: the web handlers write the registry on the HTTP thread while the engine
// thread reads it mid-turn (catalog + attach), so every public method holds mu_; the
// private helpers assume it is already held.

#include <ArduinoJson.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "nimbus/orch/connectors_wire.h"
#include "posix_fs.h"

namespace nimbusd {

class ConnectorsStore {
 public:
  // Device parity: the same entry cap every firmware consumer uses
  // (agent::connectors::kMaxConnectors) and the same web-endpoint byte cap.
  static constexpr int kMaxConnectors = 24;
  static constexpr size_t kBlobCap = 3500;

  void setPath(const std::string& p) { path_ = p; }

  // Tolerant load: an absent or torn file simply means "no connectors yet".
  void load() {
    std::string raw;
    if (!fsutil::readFile(path_, raw)) return;
    JsonDocument d;
    if (deserializeJson(d, raw) || !d.is<JsonArray>()) return;  // torn -> keep []
    std::lock_guard<std::mutex> lk(mu_);
    blob_ = raw;
  }

  // A copy (never a reference): the HTTP thread may replace the blob concurrently.
  std::string blob() const {
    std::lock_guard<std::mutex> lk(mu_);
    return blob_;
  }
  bool empty() const {
    std::lock_guard<std::mutex> lk(mu_);
    return blob_ == "[]" || blob_.empty();
  }

  // The sanitized configured[] view for GET /api/connectors: secrets are never
  // echoed - tok becomes hasTok, oauth becomes hasOauth (device webui.cpp rule).
  // When `authOf` is given, each entry also carries the derived credential state
  // `auth` (-1 n/a, 1 ok / listed in the Mistral workspace, 0 sign-in failed, 2
  // missing / not listed / not checked yet), built from the raw entry exactly as the
  // device GET builds it, so the web badge reads the same rule the catalog does.
  using AuthOf = std::function<int8_t(const nimbus::orch::ConnectorInfo&)>;
  void sanitizedConfigured(JsonArray out, const AuthOf& authOf = nullptr) const {
    JsonDocument in;
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (deserializeJson(in, blob_)) return;
    }
    for (JsonObjectConst c : in.as<JsonArrayConst>()) {
      JsonObject o = out.add<JsonObject>();
      o["name"] = c["name"];
      o["prov"] = c["prov"];
      o["kind"] = c["kind"];
      o["url"] = c["url"];
      o["cid"] = c["cid"];
      o["en"] = c["en"];
      o["type"] = (const char*)(c["type"] | "");
      o["hasTok"] = ((const char*)(c["tok"] | ""))[0] != 0;
      o["hasOauth"] = !c["oauth"].isNull();
      if (authOf) o["auth"] = authOf(entryInfo(c));
    }
  }

  // POST mode 1: whole-blob replace (the Advanced raw editor). Returns the
  // owner-facing error string, "" on success. Error strings match the device.
  std::string replaceBlob(const std::string& newBlob) {
    std::lock_guard<std::mutex> lk(mu_);
    JsonDocument d;
    if (newBlob.size() > kBlobCap || deserializeJson(d, newBlob) || !d.is<JsonArray>())
      return "blob must be a JSON array (<=3500B)";
    std::string err = validateSet(newBlob);
    if (!err.empty()) return err;
    blob_ = newBlob;
    persist();
    return {};
  }

  // POST mode 2: remove the FIRST entry whose name matches. A missing name is a
  // no-op success (device parity).
  std::string removeByName(const std::string& name) {
    std::lock_guard<std::mutex> lk(mu_);
    JsonDocument cur = current();
    JsonArray arr = cur.as<JsonArray>();
    for (size_t i = 0; i < arr.size(); i++) {
      if (name == (const char*)(arr[i]["name"] | "")) {
        arr.remove(i);
        break;
      }
    }
    return commit(cur);
  }

  // POST mode 3: upsert ONE entry by name. tok/oauth that are null or "" in the
  // patch PRESERVE the stored secret - a card edit never round-trips a secret.
  std::string patchUpsert(const std::string& patchJson) {
    JsonDocument pd;
    if (patchJson.empty() || deserializeJson(pd, patchJson) || !pd.is<JsonObject>())
      return "patch must be a JSON object";
    const char* nm = pd["name"] | "";
    if (!nm[0]) return "patch needs a name";
    std::lock_guard<std::mutex> lk(mu_);
    JsonDocument cur = current();
    JsonArray arr = cur.as<JsonArray>();
    JsonObject dst;  // find existing (preserving its secrets) or append
    for (JsonObject o : arr)
      if (strcmp(nm, (const char*)(o["name"] | "")) == 0) {
        dst = o;
        break;
      }
    if (dst.isNull()) dst = arr.add<JsonObject>();
    for (JsonPairConst kv : pd.as<JsonObjectConst>()) {
      const bool secret =
          !strcmp(kv.key().c_str(), "tok") || !strcmp(kv.key().c_str(), "oauth");
      const bool blank = kv.value().isNull() ||
                         (kv.value().is<const char*>() && !kv.value().as<const char*>()[0]);
      if (secret && blank) continue;  // blank secret -> keep the stored one
      dst[kv.key()] = kv.value();
    }
    return commit(cur);
  }

  // The parsed non-secret view for catalog/attach (the single no-silent-drop
  // parser the device also uses). Drops past the cap are loud-logged.
  std::vector<nimbus::orch::ConnectorInfo> parsed() const {
    const std::string snap = blob();
    std::vector<nimbus::orch::ConnectorInfo> cs;
    int total = 0;
    const int n = nimbus::orch::parseConnectorsJson(snap.c_str(), cs, kMaxConnectors, &total);
    if (total > n)
      std::fprintf(stderr, "connectors: %d of %d entries active (cap %d, nameless skipped)\n",
                   n, total, kMaxConnectors);
    return cs;
  }

  // Bearer resolution for the attach seams: the stored static token (T2), else ""
  // (T1 needs none; T3 mint is not implemented on a hosted instance - the attach
  // builders skip an unauthenticated first-party connector).
  std::string bearerForName(const std::string& name) const {
    JsonDocument in;
    if (deserializeJson(in, blob())) return {};
    for (JsonObjectConst c : in.as<JsonArrayConst>())
      if (name == (const char*)(c["name"] | "")) return (const char*)(c["tok"] | "");
    return {};
  }

 private:
  // The non-secret ConnectorInfo for one RAW blob entry, with the same defaults the
  // device GET applies (prov "any", kind "mcp") - so the per-entry auth there and
  // here are computed from identical inputs.
  static nimbus::orch::ConnectorInfo entryInfo(JsonObjectConst c) {
    nimbus::orch::ConnectorInfo ci;
    ci.name = (const char*)(c["name"] | "");
    ci.prov = (const char*)(c["prov"] | "any");
    ci.kind = (const char*)(c["kind"] | "mcp");
    ci.type = (const char*)(c["type"] | "");
    ci.connectorId = (const char*)(c["cid"] | "");
    ci.enabled = (c["en"] | 0) != 0;
    ci.hasToken = ((const char*)(c["tok"] | ""))[0] != 0;
    ci.hasOauth = !c["oauth"].isNull();
    return ci;
  }

  // mu_ held by the caller.
  JsonDocument current() const {
    JsonDocument cur;
    if (deserializeJson(cur, blob_) || !cur.is<JsonArray>()) cur.to<JsonArray>();
    return cur;
  }

  // Serialize + validate + size-check + persist. Shared by del/patch.
  std::string commit(JsonDocument& cur) {
    std::string outBlob;
    serializeJson(cur, outBlob);
    std::string err = validateSet(outBlob);
    if (!err.empty()) return err;
    if (outBlob.size() > kBlobCap) return "connector set too large (<=3500B)";
    blob_ = outBlob;
    persist();
    return {};
  }

  // CUM-255 save-time validation over the FINAL serialized set: reject a
  // misconfigured entry with a clear owner-facing message (the attach guard is
  // still the safety boundary; this is UX, same as the device endpoint).
  std::string validateSet(const std::string& serialized) const {
    std::vector<nimbus::orch::ConnectorInfo> cs;
    nimbus::orch::parseConnectorsJson(serialized.c_str(), cs, kMaxConnectors);
    for (const auto& c : cs) {
      std::string e = nimbus::orch::connectorConfigError(c);
      if (!e.empty()) return e;
    }
    return {};
  }

  // Secrets-grade write: 0600 from creation, atomic rename (see header comment).
  void persist() const {
    if (path_.empty()) return;
    const std::string tmp = path_ + ".tmp";
    const int fd = ::open(tmp.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd < 0) return;
    const ssize_t n = ::write(fd, blob_.data(), blob_.size());
    ::close(fd);
    if (n != (ssize_t)blob_.size()) {
      ::unlink(tmp.c_str());
      return;
    }
    if (::rename(tmp.c_str(), path_.c_str()) != 0) ::unlink(tmp.c_str());
  }

  mutable std::mutex mu_;
  std::string path_;
  std::string blob_ = "[]";
};

}  // namespace nimbusd
