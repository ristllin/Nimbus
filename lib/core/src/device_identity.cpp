#include "nimbus/device_identity.h"

#include <cctype>
#include <set>

namespace nimbus::identity {

namespace {
constexpr size_t kMaxName = 24;

// If `s` names a sibling of `base` (exact, numbered, with or without the
// "-setup" AP suffix), return its occupied index (1 for the bare base, N for
// "-N"); otherwise 0.
int siblingIndex(const std::string& base, std::string s) {
  const std::string suffix = kApSuffix;  // "-setup"
  if (s.size() >= suffix.size() &&
      s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0)
    s.erase(s.size() - suffix.size());
  if (s == base) return 1;
  // "<base>-N", N >= 2, all digits.
  if (s.size() <= base.size() + 1 || s.compare(0, base.size(), base) != 0 ||
      s[base.size()] != '-')
    return 0;
  const std::string digits = s.substr(base.size() + 1);
  if (digits.empty() || digits.size() > 4) return 0;
  for (char c : digits)
    if (!std::isdigit((unsigned char)c)) return 0;
  const int n = std::stoi(digits);
  return n >= 2 ? n : 0;
}
}  // namespace

std::string sanitizeName(const std::string& raw) {
  std::string out;
  bool lastBlank = true;  // also swallows leading blanks
  for (char c : raw) {
    const bool ok = std::isalnum((unsigned char)c) || c == '-' || c == '_';
    if (ok) {
      out += c;
      lastBlank = false;
    } else if ((c == ' ' || c == '\t') && !lastBlank) {
      out += ' ';
      lastBlank = true;
    }  // every other char is dropped
    if (out.size() >= kMaxName) break;
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

std::string mdnsLabel(const std::string& name) {
  std::string out;
  bool lastDash = true;  // swallow leading dashes
  for (char c : name) {
    if (std::isalnum((unsigned char)c)) {
      out += (char)std::tolower((unsigned char)c);
      lastDash = false;
    } else if (!lastDash) {
      out += '-';
      lastDash = true;
    }
    if (out.size() >= kMaxName) break;
  }
  while (!out.empty() && out.back() == '-') out.pop_back();
  return out;
}

std::string pickSiblingName(const std::string& base,
                            const std::vector<std::string>& ssids) {
  std::set<int> taken;
  for (const auto& s : ssids) {
    const int idx = siblingIndex(base, s);
    if (idx > 0) taken.insert(idx);
  }
  int i = 1;
  while (taken.count(i)) ++i;
  return i == 1 ? base : base + "-" + std::to_string(i);
}

std::string makeSetupPass(uint32_t (*rnd)()) {
  // 24 lowercase letters (no o/l) + 8 digits (no 0/1) = exactly 32 symbols,
  // so the modulo below is unbiased.
  static const char kAlphabet[] = "abcdefghijkmnpqrstuvwxyz23456789";
  std::string out;
  out.reserve(kSetupPassLen);
  for (int i = 0; i < kSetupPassLen; ++i) out += kAlphabet[rnd() % 32];
  return out;
}

std::string wifiQrPayload(const std::string& ssid, const std::string& pass) {
  if (ssid.empty()) return "";
  auto esc = [](const std::string& s) {
    std::string out;
    for (char c : s) {
      if (c == '\\' || c == ';' || c == ',' || c == ':' || c == '"') out += '\\';
      out += c;
    }
    return out;
  };
  std::string p = "WIFI:S:" + esc(ssid) + ";T:";
  p += pass.empty() ? "nopass;" : ("WPA;P:" + esc(pass) + ";");
  p += ";";
  return p;
}

}  // namespace nimbus::identity
