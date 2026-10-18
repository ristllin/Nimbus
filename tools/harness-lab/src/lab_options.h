#pragma once
#include <cctype>
#include <cstdlib>
#include <string>

#include "lab_env.h"
#include "lab_rig.h"
#include "nimbus/harness/fault_transport.h"
#include "nimbus/orch/model_key.h"

// The run pins every live lab leg reads, from the environment (or the dotenv
// file), so a test script states its model and budget once:
//
//   NIMBUS_LAB_MODEL       [host/]model[,...]  e.g. claude-haiku-5-5 or
//                          mistral/mistral-small-latest (the host is inferred
//                          from the id when omitted)
//   NIMBUS_LAB_EFFORT      low|medium|high|xhigh|max, or "adapter" to send what
//                          the device adapter sends (default low)
//   NIMBUS_LAB_MAXTOK      output-token cap per request (default: the adapter's)
//   NIMBUS_LAB_CEILING_MS  per-request timeout ceiling (default: the adapter's)
//
// Command-line flags (--model, --effort, --max-tokens, --ceiling-ms, --rounds,
// --fault, --record) override these.
namespace lab {

inline bool parseBoundedInt(const std::string& s, long lo, long hi, long& out) {
  if (s.empty() || s.size() > 9) return false;
  for (char c : s)
    if (c < '0' || c > '9') return false;
  out = std::strtol(s.c_str(), nullptr, 10);
  return out >= lo && out <= hi;
}

inline bool validEffort(const std::string& e) {
  return e == "low" || e == "medium" || e == "high" || e == "xhigh" || e == "max" ||
         e == "adapter";
}

// A model selector token: the same bounded charset nimbusd accepts.
inline bool validModelToken(const std::string& m) {
  if (m.empty() || m.size() > 64) return false;
  for (char ch : m)
    if (!isalnum((unsigned char)ch) && ch != '.' && ch != '_' && ch != '-' && ch != ':') return false;
  return true;
}

// One "[host/]model" item into opt.models.
inline bool addModelPin(const std::string& item, LabRig::Options& opt, std::string& err) {
  const size_t sl = item.find('/');
  const std::string model = sl == std::string::npos ? item : item.substr(sl + 1);
  std::string host = sl == std::string::npos ? std::string(nimbus::orch::inferUpstream(model.c_str()))
                                             : item.substr(0, sl);
  if (host != "anthropic" && host != "openai" && host != "mistral" && host != "zai") {
    err = "NIMBUS_LAB_MODEL: unknown host '" + host + "' in '" + item + "'";
    return false;
  }
  if (!validModelToken(model)) {
    err = "NIMBUS_LAB_MODEL: bad model id '" + model + "'";
    return false;
  }
  opt.models[host] = model;
  return true;
}

inline bool applyModelPins(const std::string& csv, LabRig::Options& opt, std::string& err) {
  size_t start = 0;
  while (start <= csv.size()) {
    const size_t comma = csv.find(',', start);
    const std::string item = csv.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
    if (!item.empty() && !addModelPin(item, opt, err)) return false;
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return true;
}

inline bool applyEffort(const std::string& e, LabRig::Options& opt, std::string& err) {
  if (!validEffort(e)) {
    err = "effort '" + e + "' (want low|medium|high|xhigh|max|adapter)";
    return false;
  }
  opt.pins.effort = e == "adapter" ? std::string() : e;
  return true;
}

inline bool applyMaxTokens(const std::string& v, LabRig::Options& opt, std::string& err) {
  long n = 0;
  if (!parseBoundedInt(v, 1, 128000, n)) { err = "max tokens '" + v + "' (want 1..128000)"; return false; }
  opt.pins.maxTokens = (uint32_t)n;
  return true;
}

inline bool applyCeiling(const std::string& v, LabRig::Options& opt, std::string& err) {
  long n = 0;
  if (!parseBoundedInt(v, 1000, 3600000, n)) { err = "ceiling ms '" + v + "' (want 1000..3600000)"; return false; }
  opt.pins.ceilingMs = (uint32_t)n;
  return true;
}

inline bool applyRounds(const std::string& v, LabRig::Options& opt, std::string& err) {
  long n = 0;
  if (!parseBoundedInt(v, 1, 32, n)) { err = "rounds '" + v + "' (want 1..32, the device clamp)"; return false; }
  opt.rounds = (int)n;
  return true;
}

inline bool applyFault(const std::string& spec, LabRig::Options& opt, std::string& err) {
  agent::FaultRule r;
  std::string why;
  if (!agent::parseFaultRule(spec, r, why)) { err = "--fault=" + spec + ": " + why; return false; }
  opt.faults.push_back(r);
  return true;
}

// Move `host` to the front of a comma-separated priority list.
inline std::string promoteHost(const std::string& prio, const std::string& host) {
  std::string out = host;
  size_t start = 0;
  while (start <= prio.size()) {
    const size_t comma = prio.find(',', start);
    const std::string h = prio.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
    if (!h.empty() && h != host) out += "," + h;
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return out;
}

// The NIMBUS_LAB_* environment, applied before the command-line flags. Pinning a
// single model also makes its host the head (first in priority).
inline bool applyLabEnv(const Env& env, LabRig::Options& opt, std::string& err) {
  const std::string model = env.get("NIMBUS_LAB_MODEL");
  if (!model.empty()) {
    LabRig::Options pinned = opt;
    pinned.models.clear();
    if (!applyModelPins(model, pinned, err)) return false;
    for (const auto& kv : pinned.models) opt.models[kv.first] = kv.second;
    if (pinned.models.size() == 1) opt.priority = promoteHost(opt.priority, pinned.models.begin()->first);
  }
  const std::string effort = env.get("NIMBUS_LAB_EFFORT");
  if (!effort.empty() && !applyEffort(effort, opt, err)) return false;
  const std::string maxtok = env.get("NIMBUS_LAB_MAXTOK");
  if (!maxtok.empty() && !applyMaxTokens(maxtok, opt, err)) return false;
  const std::string ceil = env.get("NIMBUS_LAB_CEILING_MS");
  if (!ceil.empty() && !applyCeiling(ceil, opt, err)) return false;
  return true;
}

}  // namespace lab
