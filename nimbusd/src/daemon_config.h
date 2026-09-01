#pragma once
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>

// daemon_config - HarnessConfig inputs over environment + an optional config
// file (§3.1). Secrets (provider keys, Telegram bot token, Tavily key) arrive
// from the process environment - in the container they come from a mounted
// Secret - and a non-secret config file supplies the rest (device name,
// timezone, provider priority, model overrides). The env always wins over the
// file so an operator can override one value without editing the mounted config.
//
// Deliberately never logs a value: callers that echo settings mask secrets to
// the first 4 chars themselves (see main.cpp).
namespace nimbusd {

class Config {
 public:
  // Load a KEY=VALUE file (tolerant: comments, blanks, `export`, quotes). Values
  // already present from a prior load are NOT overwritten (first load wins),
  // matching the dotenv precedence the lab + HIL suite use.
  void loadFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) return;
    std::string line, k, v;
    while (std::getline(f, line)) {
      if (parseEnvLine(line, k, v) && !file_.count(k)) file_[k] = v;  // first load wins
    }
  }

 private:
  // Parse one dotenv line into (k, v). Returns false for a comment/blank/malformed
  // line. Tolerant: strips a leading `export`, trims the key, and unquotes a
  // fully-quoted value.
  static bool parseEnvLine(const std::string& raw, std::string& k, std::string& v) {
    const size_t b = raw.find_first_not_of(" \t");
    if (b == std::string::npos || raw[b] == '#') return false;
    std::string line = raw.substr(b);
    if (line.rfind("export ", 0) == 0) line = line.substr(7);
    const size_t eq = line.find('=');
    if (eq == std::string::npos) return false;
    k = line.substr(0, eq);
    v = line.substr(eq + 1);
    if (!cleanKey(k)) return false;
    cleanValue(v);
    return true;
  }
  // Trim trailing whitespace; a valid key is non-empty with no internal whitespace.
  static bool cleanKey(std::string& k) {
    while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
    return !k.empty() && k.find_first_of(" \t") == std::string::npos;
  }
  // Trim a trailing CR/LF and unwrap a fully single- or double-quoted value.
  static void cleanValue(std::string& v) {
    while (!v.empty() && (v.back() == '\r' || v.back() == '\n')) v.pop_back();
    if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front())
      v = v.substr(1, v.size() - 2);
  }

 public:

  // In-app override (CUM-279): a value set through the running UI (a provider key
  // the owner typed on the Providers & keys page). It is the AUTHORITATIVE source -
  // it wins over both the environment and the file, exactly as a device treats the
  // key you set in its own UI as the one it uses. An empty value ERASES the override
  // so the env/file value shows through again (a "Clear key" from the page). The rig
  // persists these to a durable secrets file so they survive a process restart.
  void setOverride(const std::string& name, const std::string& value) {
    if (value.empty()) override_.erase(name);
    else override_[name] = value;
  }
  const std::map<std::string, std::string>& overrides() const { return override_; }

  // Override wins, then environment, then the file.
  std::string get(const std::string& name, const std::string& dflt = "") const {
    auto ov = override_.find(name);
    if (ov != override_.end() && !ov->second.empty()) return ov->second;
    if (const char* e = std::getenv(name.c_str()))
      if (*e) return e;
    auto it = file_.find(name);
    return it == file_.end() ? dflt : it->second;
  }
  bool has(const std::string& name) const { return !get(name).empty(); }

  int getInt(const std::string& name, int dflt) const {
    const std::string v = get(name);
    return v.empty() ? dflt : std::atoi(v.c_str());
  }

  // Canonical env name for a provider slug - the SINGLE source of the host->env
  // mapping (AGENTS.md: the provider list drifted twice when it was hardcoded in
  // several places). Includes the Cumulo router key; empty for an unknown slug.
  static std::string providerEnvName(const std::string& host) {
    if (host == "openai")    return "OPENAI_API_KEY";
    if (host == "anthropic") return "ANTHROPIC_API_KEY";
    if (host == "mistral")   return "MISTRAL_API_KEY";
    if (host == "cumulo")    return "CUMULO_API_KEY";
    return std::string();
  }

  // Provider host -> BYOK key. Cumulo is a router key, NOT a BYOK slot, so it is not
  // returned here (callers read it via CUMULO_API_KEY directly - see rig cumuloKey()).
  std::string providerKey(const std::string& host) const {
    if (host == "cumulo") return std::string();
    const std::string env = providerEnvName(host);
    return env.empty() ? std::string() : get(env);
  }

  // Mask a secret to its first 4 chars for safe logging ("sk-a...(28)").
  static std::string mask(const std::string& secret) {
    if (secret.empty()) return "(unset)";
    const std::string head = secret.substr(0, secret.size() < 4 ? secret.size() : 4);
    return head + "...(" + std::to_string(secret.size()) + ")";
  }

 private:
  std::map<std::string, std::string> file_;
  std::map<std::string, std::string> override_;  // in-app, authoritative (CUM-279)
};

}  // namespace nimbusd
