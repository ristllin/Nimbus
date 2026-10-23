#pragma once
#include <cstddef>
#include <string>
#include <vector>

#include "nimbus/orch/episodic.h"
#include "nimbus/orch/psram_alloc.h"

// row_overlay - a photo row's text, replaced after it was stored (R1, L19-5).
//
// The episodic log is append-only, but a photo's row is written before its short
// record exists: a photo the head saw itself is stored with a pending label and gets
// its machine transcription afterwards; a photo whose reading failed (a provider out
// of credit) is stored with the typed reason and read again once the provider is open.
// The overlay keeps the newer text by row id and every read through it (the history
// window, the episodic tool, the web) returns the row with that text, so the next
// turn sees the description. Text search still matches the stored text (a known
// limit of an overlay; the record also names the photo by its ref).
//
// Bounded (kMax ids, the oldest dropped) and durable through one append-only line
// per replacement (line()/load()), which the device keeps in a file on the card.
// Portable, host-tested (test_image_records).
namespace nimbus {
namespace orch {

class RowTextOverlay : public EpisodicStore {
 public:
  static constexpr size_t kMax = 256;

  // The store reads go through (nullptr = no store: every call is a no-op).
  void bind(EpisodicStore* inner) { inner_ = inner; }
  EpisodicStore* inner() const { return inner_; }

  // Row `id` now reads `text`. A second replacement of one id wins.
  void put(const std::string& id, const std::string& text);
  bool has(const std::string& id) const;
  size_t size() const { return ids_.size(); }
  void clear() {
    ids_.clear();
    texts_.clear();
  }

  // One durable line for a replacement ({"id":..,"text":..}, no newline inside), and
  // the replay of such lines (newest last; a malformed line is skipped).
  static std::string line(const std::string& id, const std::string& text);
  size_t load(const std::string& lines);
  // Every kept replacement as lines (oldest first): the compacted file.
  std::string dump() const;

  // EpisodicStore: writes pass through; reads return rows with the replaced text.
  void addSession(const EpisodicSession& s) override;
  bool setSessionStatus(const std::string& id, const std::string& status) override;
  void addMessage(const EpisodicMessage& m) override;
  std::vector<EpisodicSession> sessions(const std::string& status = "") const override;
  std::vector<EpisodicMessage> query(const MsgQuery& q) const override;
  std::vector<EpisodicMessage> query(const MsgQuery& q, EpiQueryInfo* info) const override;
  int messageCount() const override;
  bool indexComplete() const override;

 private:
  void apply(std::vector<EpisodicMessage>& rows) const;
  EpisodicStore* inner_ = nullptr;
  // Oldest first, through the working-set allocator (PSRAM on the device): the
  // overlay lives as long as the device runs and is replayed at boot.
  WorkingVector<WorkingString> ids_;
  WorkingVector<WorkingString> texts_;
};

}  // namespace orch
}  // namespace nimbus
