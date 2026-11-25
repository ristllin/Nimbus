#include "nimbus/net/tg_inbox.h"

#include <cctype>

#include "nimbus/orch/path_rules.h"
#include "nimbus/orch/tx/rules.h"

// tg_inbox - see the header (lane RB-TG1).
namespace nimbus::net {

namespace {
constexpr char kInboxDir[] = "inbox/";

bool endsWithNoCase(std::string_view s, std::string_view tail) {
  if (s.size() < tail.size()) return false;
  for (size_t i = 0; i < tail.size(); ++i)
    if (std::tolower((unsigned char)s[s.size() - tail.size() + i]) != tail[i]) return false;
  return true;
}
}  // namespace

std::string inboxDocName(uint64_t updateId, std::string_view fileName) {
  // The sender's part is made valid alone first, so a name with nothing usable left ("", "..")
  // is "file", never an empty tail; then the whole is cut to the store limit (the id leads).
  std::string part = nimbus::orch::validSegmentFor(fileName, fileName.size() + 8);
  if (part.empty()) part = "file";
  return nimbus::orch::validSegmentFor(std::to_string(updateId) + "-" + part, nimbus::orch::kStoreNameMax);
}

std::string inboxDocPath(uint64_t updateId, std::string_view fileName) {
  return kInboxDir + inboxDocName(updateId, fileName);
}

std::string inboxPastePath(uint64_t updateId) { return kInboxDir + std::to_string(updateId) + ".txt"; }

bool isPdf(std::string_view mime, std::string_view path) {
  return mime == "application/pdf" || endsWithNoCase(path, ".pdf");
}

std::string documentDetail(const std::string& path, std::string_view mime) {
  return isPdf(mime, path) ? path + kInboxPdfNotRead : path;
}

OwnerText ownerTextIntake(const std::string& text, uint64_t updateId, size_t cap, const InboxSave& save) {
  OwnerText out;
  if (!cap || !updateId || text.size() <= cap || !save) {
    out.text = text;
    return out;
  }
  const std::string path = inboxPastePath(updateId);
  if (!save(path.substr(sizeof(kInboxDir) - 1), text) ||
      !nimbus::orch::tx::longTextHead(text, cap, nimbus::orch::tx::pathPointer(path), out.text).ok()) {
    out.text = text;   // not kept: whole, never a silent cut
    return out;
  }
  out.saved = path;
  return out;
}

}  // namespace nimbus::net
