#include "nimbus/orch/file_card_io.h"

namespace nimbus::orch {

std::string CardFilesIo::pathOf(const std::string& p, const std::string& n) const {
  return layout_.root + "/" + p + "/" + n;
}

bool CardFilesIo::swapNoted(const std::string& path) { return card_.readText(layout_.swap.note) == path; }

bool CardFilesIo::locked(const std::function<void()>& fn) {
  if (!host_.locked) return false;
  return host_.locked([&] {
    synced_ = false;
    fn();
    if (synced_ && host_.persistIndex) (void)host_.persistIndex();   // an adopted size, kept
    synced_ = false;
  });
}

bool CardFilesIo::readAt(const std::string& p, const std::string& n, uint64_t offset, size_t len, std::string& out) {
  return card_.readAt(pathOf(p, n), offset, len, out);
}

bool CardFilesIo::append(const std::string& p, const std::string& n, std::string_view bytes) {
  const std::string path = pathOf(p, n);
  if (swapNoted(path)) return false;   // its only good copy may be the backup: no write until boot repairs it
  const bool indexed = store_.find(p, n) != nullptr;
  const long long before = card_.size(path);
  if (!indexed && before >= 0) return false;   // bytes the index does not know: never written to
  if (!card_.mkdir(layout_.root + "/" + p)) return false;
  size_t landed = 0;
  const bool ok = card_.append(path, bytes, landed) && landed == bytes.size();
  if (!ok && before < 0) card_.remove(path);   // a file this call created: only its own bytes go
  return ok;
}

bool CardFilesIo::partWrite(const std::string&, const std::string&, std::string_view bytes, bool first) {
  return card_.write(layout_.toolsPart, bytes, first);
}

bool CardFilesIo::partSwap(const std::string& p, const std::string& n) {
  const std::string dst = pathOf(p, n);
  const FileEntry* e = store_.find(p, n);
  const long long onCard = card_.size(dst);
  // Never over bytes the index does not know (no entry, or an entry behind the card that sync
  // could not bring up): the stored file stays as it is.
  if ((!e && onCard >= 0) || (e && onCard >= 0 && uint64_t(onCard) != e->bytes)) return false;
  if (!card_.mkdir(layout_.root + "/" + p)) return false;
  std::string err;
  if (!swapIn(card_, layout_.swap, dst, err)) return false;
  swapDone(card_, layout_.swap);
  return true;
}

void CardFilesIo::partDiscard(const std::string&, const std::string&) { card_.remove(layout_.toolsPart); }

bool CardFilesIo::persistIndex() { return host_.persistIndex && host_.persistIndex(); }

bool CardFilesIo::roomFor(const std::string& p, const std::string& n, uint64_t addBytes, std::string& err) {
  if (host_.roomFor) return host_.roomFor(p, n, addBytes, err);
  return FileToolsIo::roomFor(p, n, addBytes, err);
}

bool CardFilesIo::remove(const std::string& p, const std::string& n) {
  const std::string path = pathOf(p, n);
  if (swapNoted(path)) return false;
  return card_.remove(path) || !card_.exists(path);
}

void CardFilesIo::sync(const std::string& p, const std::string& n) {
  const FileEntry* e = store_.find(p, n);
  if (!e) return;
  const std::string path = pathOf(plainCopy(e->project), plainCopy(e->name));
  if (swapNoted(path)) return;   // mid-replace: boot's recovery decides, not a size read now
  const long long onCard = card_.size(path);
  if (onCard < 0 || uint64_t(onCard) == e->bytes || uint64_t(onCard) > UINT32_MAX) return;
  FileEntry next = *e;
  next.bytes = uint32_t(onCard);
  next.hash = 0;   // bytes the index never saw: the hash is unknown, not guessed
  next.providerTag.clear();
  next.providerFileId.clear();
  std::string err;
  if (store_.add(next, err)) synced_ = true;
}

std::string CardFilesIo::timeText(uint32_t epoch) {
  if (!epoch || !host_.utcOffsetMin) return FileToolsIo::timeText(epoch);
  const int64_t local = int64_t(epoch) + int64_t(host_.utcOffsetMin(epoch)) * 60;
  return FileToolsIo::timeText(local > 0 ? uint32_t(local) : 0);
}

}  // namespace nimbus::orch
