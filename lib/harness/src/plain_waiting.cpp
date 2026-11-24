// The messages kept while a chat's earlier conversation is read in (plain/waiting.h), lane E2-FIX13.
#include "nimbus/harness/plain/waiting.h"

#include <ArduinoJson.h>

#include "nimbus/harness/log.h"

namespace agent {
namespace plain {

namespace {

namespace tx = nimbus::orch::tx;
using tx::Err;

constexpr char kTmpExt[] = ".tmp";

void partToJson(const tx::Part& p, JsonObject o) {
  o["ref"] = p.ref;
  o["mime"] = p.mime;
  o["b"] = p.bytes;
}

void msgToJson(const WaitingMessage& m, JsonObject o) {
  o["text"] = m.text;
  if (!m.from.empty()) o["from"] = m.from;
  o["ch"] = m.ch;
  o["tz"] = m.tzMinutes;
  o["m"] = m.m;
  JsonArray src = o["src"].to<JsonArray>();
  for (uint64_t id : m.src) src.add(id);
  JsonArray stored = o["stored"].to<JsonArray>();
  for (const std::string& s : m.stored) stored.add(s);
  JsonArray parts = o["parts"].to<JsonArray>();
  for (const tx::Part& p : m.parts) partToJson(p, parts.add<JsonObject>());
}

// A message as stored; false when a field a turn needs is missing or of the wrong kind.
bool msgFromJson(JsonObjectConst o, WaitingMessage& m) {
  if (!o["text"].is<const char*>() || !o["ch"].is<const char*>() || !o["m"].is<uint64_t>()) return false;
  m.text = o["text"].as<const char*>();
  m.from = o["from"] | "";
  m.ch = o["ch"].as<const char*>();
  m.tzMinutes = o["tz"] | (int16_t)0;
  m.m = o["m"].as<uint64_t>();
  for (JsonVariantConst id : o["src"].as<JsonArrayConst>()) {
    if (!id.is<uint64_t>()) return false;
    m.src.push_back(id.as<uint64_t>());
  }
  for (JsonVariantConst s : o["stored"].as<JsonArrayConst>()) {
    if (!s.is<const char*>()) return false;
    m.stored.push_back(s.as<const char*>());
  }
  for (JsonObjectConst p : o["parts"].as<JsonArrayConst>()) {
    if (!p["ref"].is<const char*>()) return false;
    tx::Part part;
    part.kind = tx::PartKind::Image;
    part.ref = p["ref"].as<const char*>();
    part.mime = p["mime"] | "";
    part.bytes = p["b"] | (uint32_t)0;
    m.parts.push_back(part);
  }
  return true;
}

}  // namespace

std::string waitingToText(const std::vector<WaitingMessage>& msgs) {
  JsonDocument d;
  d["v"] = 1;
  JsonArray a = d["msgs"].to<JsonArray>();
  for (const WaitingMessage& m : msgs) msgToJson(m, a.add<JsonObject>());
  std::string out;
  serializeJson(d, out);
  out += '\n';
  return out;
}

bool waitingFromText(const std::string& text, std::vector<WaitingMessage>& out) {
  out.clear();
  JsonDocument d;
  if (deserializeJson(d, text) || d["v"] != 1 || !d["msgs"].is<JsonArrayConst>()) return false;
  for (JsonObjectConst o : d["msgs"].as<JsonArrayConst>()) {
    WaitingMessage m;
    if (!msgFromJson(o, m)) return false;
    out.push_back(std::move(m));
  }
  return true;
}

Status WaitingInbox::load() {
  if (!files_) {
    loaded_ = true;   // RAM only: what this boot kept
    return Status::success();
  }
  const std::string path = waitingPathFor(chatId_);
  bool present = false;
  if (!listed(present)) return Status::fail(Err::Io, "plain::WaitingInbox::load.list");
  std::vector<WaitingMessage> msgs;
  if (present) {
    size_t size = 0, got = 0;
    std::string text;
    if (!files_->size(path, size)) return Status::fail(Err::Io, "plain::WaitingInbox::load.size");
    text.resize(size);
    if (size && (!files_->readAt(path, 0, &text[0], size, got) || got != size))
      return Status::fail(Err::Io, "plain::WaitingInbox::load.read");
    if (!waitingFromText(text, msgs)) {
      // Read whole but not a list: set aside as it is (never removed), and start empty.
      std::string aside;
      if (!asideName(aside) || !files_->rename(path, aside)) return Status::fail(Err::Io, "plain::WaitingInbox::load.aside");
      hlog::logf("e2: the waiting messages of %s did not decode: set aside as %s", chatId_.c_str(), aside.c_str());
      msgs.clear();
    }
  }
  msgs_ = std::move(msgs);
  loaded_ = true;
  return Status::success();
}

// Does the card's listing of /mem/tx name this chat's file? False when the card could not tell.
bool WaitingInbox::listed(bool& present) const {
  present = false;
  std::vector<std::string> names;
  if (!files_ || !files_->list(tx::kTxRoot, names)) return false;
  const std::string path = waitingPathFor(chatId_);
  const std::string name = path.substr(std::string(tx::kTxRoot).size() + 1);
  for (const std::string& n : names) present = present || n == name;
  return true;
}

// A name beside the list that the card does not hold yet (a rename never replaces an earlier one).
bool WaitingInbox::asideName(std::string& out) const {
  std::vector<std::string> names;
  if (!files_->list(tx::kTxRoot, names)) return false;
  const std::string base = waitingPathFor(chatId_).substr(std::string(tx::kTxRoot).size() + 1) + ".unread-";
  for (unsigned n = 1; n < 1000; ++n) {
    bool used = false;
    for (const std::string& name : names) used = used || name == base + std::to_string(n);
    if (!used) {
      out = std::string(tx::kTxRoot) + "/" + base + std::to_string(n);
      return true;
    }
  }
  return false;
}

// The file is there, or the card could not tell (a removal is then not taken as done).
bool WaitingInbox::onCard() const {
  bool present = true;
  return !listed(present) || present;
}

bool WaitingInbox::holds(const std::vector<uint64_t>& src) const {
  for (const WaitingMessage& m : msgs_)
    for (uint64_t a : m.src)
      for (uint64_t b : src)
        if (a == b) return true;
  return false;
}

Status WaitingInbox::save(const std::vector<WaitingMessage>& msgs) {
  if (!files_) return Status::fail(Err::Io, "plain::WaitingInbox::save.nocard");
  const std::string path = waitingPathFor(chatId_), tmp = path + kTmpExt;
  if (msgs.empty()) return files_->remove(path) || !onCard() ? Status::success() : Status::fail(Err::Io, "plain::WaitingInbox::save.remove");
  if (!files_->writeWhole(tmp, waitingToText(msgs)) || !files_->rename(tmp, path))
    return Status::fail(Err::Io, "plain::WaitingInbox::save");
  return Status::success();
}

HoldVerdict WaitingInbox::hold(const WaitingMessage& m) {
  if (!loaded_) return HoldVerdict::Unread;
  if (!m.src.empty() && holds(m.src)) return HoldVerdict::Duplicate;
  if (msgs_.size() >= kWaitingMax) return HoldVerdict::Full;
  msgs_.push_back(m);
  return save(msgs_).ok() ? HoldVerdict::Kept : HoldVerdict::KeptInRam;
}

Status WaitingInbox::pop() {
  if (msgs_.empty()) return Status::success();
  std::vector<WaitingMessage> rest(msgs_.begin() + 1, msgs_.end());
  if (files_) {
    const Status st = save(rest);
    if (!st.ok()) return st;
  }
  msgs_ = std::move(rest);
  return Status::success();
}

size_t answerWaiting(WaitingInbox& in, const WaitingDrainFns& f) {
  size_t answered = 0;
  while (in.loaded() && !in.empty()) {
    const WaitingMessage& m = in.messages().front();
    if (f.taken && f.taken(m)) {
      hlog::logf("e2: a waiting message of %s was answered before a restart: not answered again", in.chatId().c_str());
    } else {
      if (f.answer(m) == WaitingAnswer::Later) break;
      ++answered;
    }
    if (!in.pop().ok()) {
      hlog::logf("e2: a waiting message of %s is answered but still listed on the card", in.chatId().c_str());
      break;
    }
  }
  return answered;
}

}  // namespace plain
}  // namespace agent
