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
    std::string line;
    while (std::getline(f, line)) {
      const size_t b = line.find_first_not_of(" \t");
      if (b == std::string::npos || line[b] == '#') continue;
      line = line.substr(b);
      if (line.rfind("export ", 0) == 0) line = line.substr(7);
      const size_t eq = line.find('=');
      if (eq == std::string::npos) continue;
      std::string k = line.substr(0, eq), v = line.substr(eq + 1);
      while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
      if (k.empty() || k.find_first_of(" \t") != std::string::npos) continue;
      while (!v.empty() && (v.back() == '\r' || v.back() == '\n')) v.pop_back();
      if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front())
        v = v.substr(1, v.size() - 2);
      if (!file_.count(k)) file_[k] = v;
    }
  }

  // Environment wins over the file.
  std::string get(const std::string& name, const std::string& dflt = "") const {
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

  // Provider host -> key, from the canonical env names.
  std::string providerKey(const std::string& host) const {
    if (host == "openai")    return get("OPENAI_API_KEY");
    if (host == "anthropic") return get("ANTHROPIC_API_KEY");
    if (host == "mistral")   return get("MISTRAL_API_KEY");
    return std::string();
  }

  // Mask a secret to its first 4 chars for safe logging ("sk-a...(28)").
  static std::string mask(const std::string& secret) {
    if (secret.empty()) return "(unset)";
    const std::string head = secret.substr(0, secret.size() < 4 ? secret.size() : 4);
    return head + "...(" + std::to_string(secret.size()) + ")";
  }

 private:
  std::map<std::string, std::string> file_;
};

}  // namespace nimbusd
