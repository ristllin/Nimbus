#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

// tg_inbox - where engine 2 keeps what an owner sends that is not a plain short text (PRD
// sections 2 and 5, lane RB-TG1). One rule for the device, nimbusd and the host battery.
//
// - A document lands in the file store at inbox/<update id>-<valid name>: the update id leads,
//   so two files with the same name never overwrite each other and a file sent again is a new
//   file; the name is made valid by the file tools' own path rule (path_rules.h, 48 B with the
//   extension kept). The message shows that path (documentDetail): a text type is read with
//   read_file; a PDF is stored and the message says it cannot be read yet (RB-P2 reads PDFs).
// - An owner text over the inline cap is saved whole as inbox/<update id>.txt before the u item;
//   the u item keeps the first `cap` bytes (at least the head's v5.1.2 message cap) and the
//   pointer line of tx::longTextHead, naming how much more there is and where (ruling 81: the
//   whole is kept, nothing is dropped). When the save is refused (no card, a full store) the text
//   goes on whole: the engine stores it, or refuses it in its one true line, never a silent cut.
// The inbox is device-written: the file tools refuse a model's write there (path_rules.h).
// Portable, Arduino-free, host-tested (test_tg_inbox).
namespace nimbus::net {

// Model-facing (catalog class stubs, E2_EXTRA_SOURCES): what a PDF's line says after its path.
inline constexpr char kInboxPdfNotRead[] = " (a PDF: it cannot be read yet)";

// "<update id>-<valid name>"; a name with nothing usable left is "<update id>-file".
std::string inboxDocName(uint64_t updateId, std::string_view fileName);
// "inbox/" + inboxDocName(...).
std::string inboxDocPath(uint64_t updateId, std::string_view fileName);
// "inbox/<update id>.txt".
std::string inboxPastePath(uint64_t updateId);
// What the message shows for a stored document: its path, and for a PDF kInboxPdfNotRead.
std::string documentDetail(const std::string& path, std::string_view mime);
bool isPdf(std::string_view mime, std::string_view path);

// The store write the intake uses: (project "inbox", name, whole text) -> stored.
using InboxSave = std::function<bool(const std::string& name, const std::string& text)>;
struct OwnerText {
  std::string text;    // what goes into the u item
  std::string saved;   // the inbox path of the whole ("" when nothing was saved)
};
// The owner text intake: whole at or under `cap` (or when `cap` is 0, or update id is 0); over
// it, saved through `save` and cut by tx::longTextHead to its pointer; a refused save passes
// the text whole.
OwnerText ownerTextIntake(const std::string& text, uint64_t updateId, size_t cap, const InboxSave& save);

}  // namespace nimbus::net
