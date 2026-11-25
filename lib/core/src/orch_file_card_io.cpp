#include "nimbus/orch/file_card_io.h"

#include <cstdlib>
#include <vector>

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
  uint64_t landed = 0;
  return appendCounted(p, n, bytes, landed);
}

bool CardFilesIo::appendCounted(const std::string& p, const std::string& n, std::string_view bytes,
                                uint64_t& landed) {
  landed = 0;
  const std::string path = pathOf(p, n);
  const FileEntry* e = store_.find(p, n);
  const long long before = card_.size(path);
  // Its only good copy may be the backup (no write until boot repairs it); bytes the index does not
  // know (no entry, or an entry sync could not bring to the card's size) are never appended to.
  const bool refuse = swapNoted(path) || (!e && before >= 0) || (e && before >= 0 && uint64_t(before) != e->bytes);
  if (refuse || !card_.mkdir(layout_.root + "/" + p)) {
    unnote();
    return false;
  }
  size_t got = 0;
  if (card_.append(path, bytes, got) && got == bytes.size()) return true;
  landed = undoShort(path, before, got);
  unnote();
  return false;
}

// Short (S2): undo this call's own bytes, measured in this hold: back to `before`, or gone when the
// call created the file. When the card refuses, count what stayed.
uint64_t CardFilesIo::undoShort(const std::string& path, long long before, size_t got) {
  if (before < 0) {
    if (card_.remove(path) || !card_.exists(path)) return 0;
  } else if (card_.size(path) == before || card_.truncate(path, uint64_t(before))) {
    return 0;
  }
  const long long after = card_.size(path);
  const long long base = before < 0 ? 0 : before;
  return after > base ? uint64_t(after - base) : uint64_t(got);
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
  std::string err;
  if ((!e && onCard >= 0) || (e && onCard >= 0 && uint64_t(onCard) != e->bytes) ||
      !card_.mkdir(layout_.root + "/" + p) || !swapIn(card_, layout_.swap, dst, err)) {
    unnote();
    return false;
  }
  swapDone(card_, layout_.swap);   // the index entry: the journal note covers it until the index lands
  return true;
}

void CardFilesIo::partDiscard(const std::string&, const std::string&) { card_.remove(layout_.toolsPart); }

bool CardFilesIo::persistIndex() {
  const bool landed = host_.persistIndex && host_.persistIndex();
  if (noted_ || notePending_) {
    // Landed: the note's work is done. An append whose index did not land drops its note too: its
    // call returned, so its bytes stay (a longer file is adopted at its size, never cut later).
    if (landed || noteOp_ == 'a') {
      (void)dropNote();
    } else {
      noted_ = false;
      notePending_ = true;
    }
  }
  return landed;
}

bool CardFilesIo::dropNote() {
  noted_ = false;
  const bool gone = clearNote();
  notePending_ = !gone;
  return gone;
}

// The note off the card, or made unreadable as a change: removed, else overwritten by the marker,
// else cut to empty (ruling 115 item 2). False only when the card refused all three.
bool CardFilesIo::clearNote() {
  const std::string& note = layout_.toolsNote;
  if (card_.remove(note) || !card_.exists(note)) return true;
  return card_.writeText(note, kToolsNoteCleared) || card_.truncate(note, 0);
}

void CardFilesIo::unnote() {
  if (noted_) (void)dropNote();
}

bool CardFilesIo::settle() { return host_.persistIndex && host_.persistIndex() && dropNote(); }

bool CardFilesIo::journal(const Change& c) {
  if (layout_.toolsNote.empty()) return true;
  if (notePending_ && !settle()) return false;   // never written over an earlier change's note unresolved
  std::string text(1, c.op);
  text += "\n" + c.person + "\n" + c.project + "\n" + c.name + "\n" + std::to_string(c.before) + "\n" +
          std::to_string(c.bytes) + "\n";
  if (!card_.writeText(layout_.toolsNote, text)) {
    notePending_ = !clearNote();   // a part of a note never parses as a change; a kept one is settled first
    return false;
  }
  noted_ = true;
  noteOp_ = c.op;
  return true;
}

bool CardFilesIo::roomFor(const std::string& p, const std::string& n, uint64_t addBytes, std::string& err) {
  if (host_.roomFor) return host_.roomFor(p, n, addBytes, err);
  return FileToolsIo::roomFor(p, n, addBytes, err);
}

bool CardFilesIo::remove(const std::string& p, const std::string& n) {
  const std::string path = pathOf(p, n);
  const bool ok = !swapNoted(path) && (card_.remove(path) || !card_.exists(path));
  if (!ok) unnote();
  return ok;
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

namespace {

constexpr long long kMaxEntryBytes = static_cast<long long>(UINT32_MAX);

// A Principal namespace an index owner field can hold: 1..64 printable ASCII, no '|' (the field's
// "|shared" grant). Empty is the legacy owner, which a note never names on its own.
bool readablePerson(const std::string& ns) {
  if (ns.empty() || ns.size() > 64) return false;
  for (const char ch : ns) {
    if (ch < 0x21 || ch > 0x7e || ch == '|') return false;
  }
  return true;
}

// A journal note's six lines: op, person, project, name, size before, an append's length.
bool parseNote(const std::string& text, FileToolsIo::Change& c) {
  std::vector<std::string> f;
  size_t at = 0;
  while (at < text.size() && f.size() < 6) {
    const size_t nl = text.find('\n', at);
    if (nl == std::string::npos) return false;   // a note cut during its own write: no byte had landed
    f.push_back(text.substr(at, nl - at));
    at = nl + 1;
  }
  if (f.size() != 6 || f[0].size() != 1 || f[4].empty() || f[5].empty()) return false;
  c.op = f[0][0];
  c.person = f[1];
  c.project = f[2];
  c.name = f[3];
  char* end = nullptr;
  c.before = std::strtoll(f[4].c_str(), &end, 10);
  if (!end || *end) return false;
  c.bytes = std::strtoull(f[5].c_str(), &end, 10);
  return end && !*end && (c.op == 'w' || c.op == 'a' || c.op == 'd');
}

}  // namespace

// The FNV-1a 64 of the file on the card, read in 4 KB steps (false: a read fault).
bool CardFilesIo::hashOnCard(const std::string& path, uint64_t size, uint64_t& hash) {
  hash = kFnvBasis;
  std::string part;
  for (uint64_t at = 0; at < size; at += part.size()) {
    if (!card_.readAt(path, at, 4096, part) || part.empty()) return false;
    hash = fnvContinue(hash, part);
  }
  return true;
}

// An append the power cut mid-write: its part of a row stays, and one line end lets the next row start
// cleanly. Only while the note still describes the card: the index at the noted size (none for a new
// file) and the card strictly inside the call's own length.
std::string CardFilesIo::endTornAppend(const Change& c, const std::string& path) {
  if (c.op != 'a') return std::string();
  const long long size = card_.size(path);
  const long long base = c.before < 0 ? 0 : c.before;
  const FileEntry* e = store_.find(c.project, c.name);
  const bool indexAsNoted = c.before < 0 ? e == nullptr : (e && e->bytes == uint64_t(c.before));
  if (!indexAsNoted || size <= base || uint64_t(size - base) >= c.bytes) return std::string();
  std::string last;
  if (!card_.readAt(path, uint64_t(size - 1), 1, last) || last.empty() || last[0] == '\n') return std::string();
  size_t landed = 0;
  if (!card_.append(path, "\n", landed) || landed != 1) return std::string();
  return "ended a torn append with a line end in " + path;
}

// A file the note says was new, in place with no entry: adopted under the person the note names, never
// the device owner by default, and only when its size fits the note.
bool CardFilesIo::adoptNoted(const Change& c, long long size, const std::string& path, std::string& did) {
  const bool fits = c.op == 'w' ? uint64_t(size) == c.bytes : (c.op == 'a' && uint64_t(size) <= c.bytes);
  if (c.before >= 0 || !fits || !readablePerson(c.person)) return false;   // unindexed and untouched
  FileEntry next;
  next.project = c.project;
  next.name = c.name;
  next.owner = c.person;
  next.bytes = uint32_t(size);
  next.kind = fileKindForName(c.name);
  next.createdAt = this->now();
  std::string err;
  if (!store_.add(next, err)) return false;
  did = "adopted the new file " + path;
  return true;
}

// The index brought to the card for the noted file, nothing else (ruling 115 item 1): an entry whose
// file is gone is dropped, an entry whose size or hash differs takes the card's size and an unknown
// hash, a noted new file is adopted (adoptNoted). True when the index changed. Lane E2-FIX3 (finding
// R3): nothing while the swap note still names the file (its restore from the backup failed this
// boot): the entry keeps its person for the boot whose swap recovery puts the file back (as sync()).
bool CardFilesIo::indexAsCard(const Change& c, const std::string& path, std::string& did) {
  if (swapNoted(path)) return false;
  const long long now = card_.size(path);
  const FileEntry* e = store_.find(c.project, c.name);
  if (now < 0) return e && !card_.exists(path) && store_.remove(c.project, c.name);
  if (now > kMaxEntryBytes) return false;
  if (!e) return adoptNoted(c, now, path, did);
  uint64_t hash = 0;
  const bool agrees = e->bytes == uint64_t(now) &&
                      (e->hash == 0 || (hashOnCard(path, uint64_t(now), hash) && hash == e->hash));
  if (agrees) return false;
  FileEntry next = *e;   // the bytes on the card, whichever copy the swap recovery kept: hash unknown
  next.bytes = uint32_t(now);
  next.hash = 0;
  next.providerTag.clear();
  next.providerFileId.clear();
  std::string err;
  return store_.add(next, err);
}

// The marker, an empty note or a part of one: nothing to act on, only cleared.
std::string CardFilesIo::clearUnreadable(const std::string& text) {
  const bool cleared = clearNote();
  if (text.empty() || text == kToolsNoteCleared) return cleared ? std::string() : "the tools note could not be cleared";
  return cleared ? "ignored an unreadable tools note" : "ignored an unreadable tools note it could not clear";
}

std::string CardFilesIo::recoverNote() {
  if (layout_.toolsNote.empty() || !card_.exists(layout_.toolsNote)) return std::string();
  const std::string text = card_.readText(layout_.toolsNote);
  Change c;
  const size_t maxP = store_.limits().maxProjectLen, maxN = store_.limits().maxNameLen;
  const bool readable = parseNote(text, c) && FileStore::validSegment(c.project, maxP) &&
                        FileStore::validSegment(c.name, maxN);
  if (!readable) return clearUnreadable(text);
  const std::string path = pathOf(c.project, c.name);
  std::string did = endTornAppend(c, path);
  const bool changed = indexAsCard(c, path, did);
  if (changed && !(host_.persistIndex && host_.persistIndex())) {
    notePending_ = true;   // the note stays: its work only ever brings the index to the card again
    return did.empty() ? "the index after a tools note was not saved" : did + "; the index was not saved";
  }
  if (!clearNote()) {
    notePending_ = true;
    return did.empty() ? "the tools note could not be cleared" : did + "; the tools note could not be cleared";
  }
  return did;
}

}  // namespace nimbus::orch
