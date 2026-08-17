#pragma once
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>

// Key loading for the lab. Real provider keys, never committed: the process
// environment wins, and a dotenv file (default: the repo-root .env, the same
// one the HIL suite reads) fills the gaps.
namespace lab {

class Env {
 public:
  // Parse a KEY=VALUE dotenv file. Tolerant on purpose - a real .env has
  // comments, blank lines, `export` prefixes, quotes, and lines that are not
  // assignments at all; none of that should abort a test run.
  void loadDotenv(const std::string& path) {
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

  std::string get(const char* name, const std::string& dflt = "") const {
    if (const char* e = std::getenv(name)) {
      if (*e) return e;
    }
    auto it = file_.find(name);
    return it == file_.end() ? dflt : it->second;
  }

  bool has(const char* name) const { return !get(name).empty(); }

  // Provider host ("openai"|"anthropic"|"mistral") -> key.
  std::string providerKey(const std::string& host) const {
    if (host == "openai")    return get("OPENAI_API_KEY");
    if (host == "anthropic") return get("ANTHROPIC_API_KEY");
    if (host == "mistral")   return get("MISTRAL_API_KEY");
    return std::string();
  }

  static std::string defaultDotenvPath() {
    const char* home = std::getenv("HOME");
    if (const char* nf = std::getenv("NIMBUS_ENV_FILE")) return std::string(nf);
    return std::string(home ? home : ".") + "/.env";
  }

 private:
  std::map<std::string, std::string> file_;
};

}  // namespace lab
