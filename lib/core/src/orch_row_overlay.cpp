#include "nimbus/orch/row_overlay.h"

#include <ArduinoJson.h>

// A photo row's replaced text (see row_overlay.h).
namespace nimbus {
namespace orch {

void RowTextOverlay::put(const std::string& id, const std::string& text) {
  if (id.empty()) return;
  for (size_t i = 0; i < ids_.size(); ++i) {
    if (!sameText(ids_[i], id)) continue;
    ids_.erase(ids_.begin() + (long)i);
    texts_.erase(texts_.begin() + (long)i);
    break;
  }
  if (ids_.size() >= kMax) {
    ids_.erase(ids_.begin());
    texts_.erase(texts_.begin());
  }
  ids_.push_back(workingCopy(id));
  texts_.push_back(workingCopy(text));
}

bool RowTextOverlay::has(const std::string& id) const {
  for (const auto& i : ids_)
    if (sameText(i, id)) return true;
  return false;
}

std::string RowTextOverlay::line(const std::string& id, const std::string& text) {
  JsonDocument d;
  d["id"] = id;
  d["text"] = text;
  std::string out;
  serializeJson(d, out);   // JSON escapes every newline: one record per line
  return out;
}

size_t RowTextOverlay::load(const std::string& lines) {
  size_t n = 0, s = 0;
  while (s < lines.size()) {
    size_t e = lines.find('\n', s);
    if (e == std::string::npos) e = lines.size();
    JsonDocument d;
    if (e > s && !deserializeJson(d, lines.data() + s, e - s)) {
      const char* id = d["id"] | "";
      const char* text = d["text"] | (const char*)nullptr;
      if (*id && text) {
        put(id, text);
        ++n;
      }
    }
    s = e + 1;
  }
  return n;
}

std::string RowTextOverlay::dump() const {
  std::string out;
  for (size_t i = 0; i < ids_.size(); ++i) out += line(plainCopy(ids_[i]), plainCopy(texts_[i])) + "\n";
  return out;
}

void RowTextOverlay::apply(std::vector<EpisodicMessage>& rows) const {
  if (ids_.empty()) return;
  for (auto& m : rows)
    for (size_t i = ids_.size(); i-- > 0;)
      if (sameText(ids_[i], m.id)) {
        m.text.assign(texts_[i].data(), texts_[i].size());
        break;
      }
}

void RowTextOverlay::addSession(const EpisodicSession& s) {
  if (inner_) inner_->addSession(s);
}
bool RowTextOverlay::setSessionStatus(const std::string& id, const std::string& status) {
  return inner_ && inner_->setSessionStatus(id, status);
}
void RowTextOverlay::addMessage(const EpisodicMessage& m) {
  if (inner_) inner_->addMessage(m);
}
std::vector<EpisodicSession> RowTextOverlay::sessions(const std::string& status) const {
  return inner_ ? inner_->sessions(status) : std::vector<EpisodicSession>();
}
std::vector<EpisodicMessage> RowTextOverlay::query(const MsgQuery& q) const {
  if (!inner_) return {};
  std::vector<EpisodicMessage> rows = inner_->query(q);
  apply(rows);
  return rows;
}
std::vector<EpisodicMessage> RowTextOverlay::query(const MsgQuery& q, EpiQueryInfo* info) const {
  if (!inner_) {
    if (info) *info = EpiQueryInfo();
    return {};
  }
  std::vector<EpisodicMessage> rows = inner_->query(q, info);
  apply(rows);
  return rows;
}
int RowTextOverlay::messageCount() const { return inner_ ? inner_->messageCount() : 0; }
bool RowTextOverlay::indexComplete() const { return !inner_ || inner_->indexComplete(); }

}  // namespace orch
}  // namespace nimbus
