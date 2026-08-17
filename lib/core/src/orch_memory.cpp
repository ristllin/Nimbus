// Ported and adapted from Nuage-Solide src/agent/orch_memory.cpp
// (Head Orchestrator v2). The Arduino String + LittleFS + store::sysPrompt()
// dependencies are replaced by std::string and the MemoryStore seam; the cap
// rules (UTF-8-safe, device-enforced) are preserved verbatim.
#include "nimbus/orch/memory.h"

#include "nimbus/mem_cap.h"  // utf8CapLen - UTF-8-safe, device-enforced length cap

namespace nimbus {
namespace orch {

namespace {

// Cap a string to maxBytes on a complete UTF-8 boundary. Returns true if it had
// to truncate (so callers can surface the truncation signal).
bool capInPlace(std::string& s, int maxBytes) {
  const int keep = utf8CapLen(s.c_str(), (int)s.size(), maxBytes);
  if (keep < (int)s.size()) {
    s.resize(keep);
    return true;
  }
  return false;
}

}  // namespace

// Map codec (B3): records joined by \x1E, chat and memory split by \x1F.
// Control chars are stripped from values on write, so the codec can't be
// corrupted by model output.
static const char kRS = '\x1E', kUS = '\x1F';
static std::string stripCtl(std::string v) {
  for (char& c : v) if (c == kRS || c == kUS) c = ' ';
  return v;
}

void OrchMemory::begin(MemoryStore* store, const std::string& directive) {
  store_ = store;
  // Directive is user-owned; keep it capped so directive() is cheap + bounded.
  directive_ = directive;
  capInPlace(directive_, kMemDirectiveMax);

  // Load the persisted per-chat map; a LEGACY single-blob value (no \x1F) is
  // discarded - running memory is short-lived working notes, benign to reset at
  // upgrade. Every loaded entry is IMMEDIATELY capped (out-of-band edits).
  chats_.clear();
  std::string raw = store_ ? store_->loadModel() : std::string();
  if (raw.find(kUS) != std::string::npos) {
    size_t start = 0;
    while (start < raw.size() && chats_.size() < 8) {
      size_t end = raw.find(kRS, start);
      if (end == std::string::npos) end = raw.size();
      std::string rec = raw.substr(start, end - start);
      start = end + 1;
      size_t us = rec.find(kUS);
      if (us == std::string::npos) continue;
      std::string mem = rec.substr(us + 1);
      capInPlace(mem, kMemModelMax);
      if (!mem.empty()) chats_.emplace_back(rec.substr(0, us), std::move(mem));
    }
  }
}

void OrchMemory::save() {
  if (!store_) return;
  std::string raw;
  for (const auto& e : chats_) {
    if (!raw.empty()) raw += kRS;
    raw += e.first;
    raw += kUS;
    raw += e.second;
  }
  if (raw.empty()) store_->clearModel();
  else store_->saveModel(raw);
}

std::string OrchMemory::directive() const {
  // Re-cap on every read (cheap; matches the ported behaviour and stays correct
  // even if directive_ were ever set past the cap out-of-band).
  std::string d = directive_;
  capInPlace(d, kMemDirectiveMax);
  return d;
}

std::string OrchMemory::model(const std::string& chatId) const {
  for (const auto& e : chats_)
    if (e.first == chatId) return e.second;
  return std::string();
}

std::string OrchMemory::modelAny() const {
  return chats_.empty() ? std::string() : chats_.back().second;
}

bool OrchMemory::setModel(const std::string& chatId, const std::string& v) {
  std::string mem = stripCtl(v);
  const bool truncated = capInPlace(mem, kMemModelMax);
  std::string chat = stripCtl(chatId);
  // Upsert: drop any existing entry, append as NEWEST; empty memory = remove.
  for (size_t i = 0; i < chats_.size(); i++)
    if (chats_[i].first == chat) { chats_.erase(chats_.begin() + i); break; }
  if (!mem.empty()) {
    while (chats_.size() >= 8) chats_.erase(chats_.begin());   // LRU: oldest out
    chats_.emplace_back(std::move(chat), std::move(mem));
  }
  save();
  return truncated;
}

void OrchMemory::appendPromptBlock(std::string& sys, const std::string& chatId) const {
  const std::string d = directive();
  const std::string model_ = model(chatId);   // local shadow keeps the body verbatim
  // Pre-reserve so the directive + memory append chain can't realloc mid-build
  // (each realloc would carve a fresh heap block - heap-relevant on device,
  // harmless on host).
  sys.reserve(sys.size() + d.size() + model_.size() + 256);
  if (!d.empty()) {
    sys += "\n\n[USER DIRECTIVE - always honor; you cannot change this]\n";
    sys += d;
  }
  sys += "\n\n[YOUR MEMORY - core facts + active threads + latest state. Keep it "
         "current by returning an updated \"memory\" field (<=";
  sys += std::to_string(kMemModelMax);
  sys += " bytes); it is all you keep across turns and provider failovers]\n";
  sys += model_.empty() ? std::string("(empty)") : model_;
}

void OrchMemory::clear() {
  chats_.clear();
  if (store_) store_->clearModel();
}

}  // namespace orch
}  // namespace nimbus
