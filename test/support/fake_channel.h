#pragma once
#include <string>
#include <vector>

#include "nimbus/harness/channel.h"

// FakeChannel - records every delivery + spoken line for assertions.
namespace harness_test {

struct FakeChannel {
  struct Sent { std::string chatId, text; };
  std::vector<Sent> sent;
  std::vector<std::string> spoken;
  bool failSends = false;
  std::string allowedChat = "1001";   // the one allowlisted chat

  agent::Channel contract() {
    agent::Channel c;
    c.send = [this](const std::string& chat, const std::string& text) {
      if (failSends) return false;
      sent.push_back({chat, text});
      return true;
    };
    c.speak = [this](const std::string& text) { spoken.push_back(text); };
    c.isAllowed = [this](const std::string& chat) { return chat == allowedChat; };
    c.firstAllowedChat = [this] { return allowedChat; };
    return c;
  }

  std::string lastText() const { return sent.empty() ? "" : sent.back().text; }
  bool anyContains(const char* needle) const {
    for (auto& s : sent)
      if (s.text.find(needle) != std::string::npos) return true;
    return false;
  }
};

}  // namespace harness_test
