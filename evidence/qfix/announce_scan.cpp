// Reads one reply per line from stdin (JSON-escaped is NOT needed: the caller replaces
// newlines with the two characters \n), prints "1\t<span>" when orch::announcesWorkNow
// matches, else "0". Build: see evidence/qfix/announce_scan.py.
#include <iostream>
#include <string>

#include "nimbus/orch/reply_guard.h"

static std::string unescape(const std::string& s) {
  std::string o;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == 'n') { o += '\n'; i++; }
    else if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == '\\') { o += '\\'; i++; }
    else o += s[i];
  }
  return o;
}

int main() {
  std::string line;
  while (std::getline(std::cin, line)) {
    const std::string r = unescape(line);
    size_t b = 0, e = 0;
    if (nimbus::orch::announcesWorkNow(r, &b, &e)) {
      std::string span = r.substr(b, e - b);
      for (auto& c : span) if (c == '\n') c = ' ';
      std::cout << "1\t" << span << "\n";
    } else {
      std::cout << "0\n";
    }
  }
  return 0;
}
