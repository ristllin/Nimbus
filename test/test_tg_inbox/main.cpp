// test_tg_inbox - where engine 2 keeps an owner's document or long paste (nimbus/net/tg_inbox.h,
// lane RB-TG1).
//
// The class: every name a sender can give lands at a valid store path under inbox/ that the
// update id leads (two files with one name never collide), the extension is kept when a long name
// is cut, and a paste over the cap is kept whole on the card with a pointer in the u text, or, when
// the save is refused, passes whole (never a silent cut).
#include <unity.h>

#include <map>
#include <string>

#include "nimbus/net/tg_inbox.h"
#include "nimbus/orch/path_rules.h"

using nimbus::net::documentDetail;
using nimbus::net::inboxDocName;
using nimbus::net::inboxDocPath;
using nimbus::net::inboxPastePath;
using nimbus::net::isPdf;
using nimbus::net::kInboxPdfNotRead;
using nimbus::net::OwnerText;
using nimbus::net::ownerTextIntake;
namespace orch = nimbus::orch;

void setUp() {}
void tearDown() {}

namespace {

bool startsWith(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; }

// A name the store keeps as it is, under the inbox, that the update id leads.
void assertValidDocName(uint64_t id, const std::string& asked) {
  const std::string name = inboxDocName(id, asked);
  const std::string lead = std::to_string(id) + "-";
  TEST_ASSERT_TRUE_MESSAGE(startsWith(name, lead), ("the update id does not lead: " + name).c_str());
  TEST_ASSERT_TRUE_MESSAGE(name.size() > lead.size(), "nothing follows the update id");
  TEST_ASSERT_TRUE_MESSAGE(name.size() <= orch::kStoreNameMax, ("over the store name limit: " + name).c_str());
  TEST_ASSERT_EQUAL_STRING_MESSAGE(name.c_str(), orch::validSegmentFor(name, orch::kStoreNameMax).c_str(),
                                   "the name is not a valid store segment");
  const orch::StorePath p = orch::storePathFor(inboxDocPath(id, asked));
  TEST_ASSERT_TRUE_MESSAGE(p.ok(), "the inbox path is not a store path");
  TEST_ASSERT_FALSE_MESSAGE(p.changed, "the store would rename the inbox path");
  TEST_ASSERT_EQUAL_STRING("inbox", p.project.c_str());
}

}  // namespace

void test_every_byte_class_makes_a_valid_name_the_update_id_leads() {
  for (int b = 1; b < 256; ++b) {
    const std::string c(1, (char)b);
    assertValidDocName(912345678, c);
    assertValidDocName(912345678, "a" + c + "b.txt");
    assertValidDocName(912345678, c + c + c);
  }
  for (const char* odd : {"", ".", "..", "...", "/", "../../etc/passwd", ".hidden", "a/b/c.pdf", "name.",
                          "\xC3\xA9t\xC3\xA9.pdf", "\xF0\x9F\x93\x84 doc.txt", "\xFF\xFE", "con", "a b c.md"})
    assertValidDocName(1, odd);
}

void test_a_name_with_nothing_usable_is_id_dash_file() {
  TEST_ASSERT_EQUAL_STRING("77-file", inboxDocName(77, "").c_str());
  TEST_ASSERT_EQUAL_STRING("77-_", inboxDocName(77, "..").c_str());   // never "77-" (the draft's bug)
  TEST_ASSERT_EQUAL_STRING("inbox/77-report.pdf", inboxDocPath(77, "report.pdf").c_str());
}

void test_two_files_with_one_name_are_two_paths() {
  TEST_ASSERT_TRUE(inboxDocPath(100, "notes.txt") != inboxDocPath(101, "notes.txt"));
}

void test_a_long_name_keeps_the_id_and_the_extension() {
  const std::string longName = std::string(200, 'x') + ".csv";
  const uint64_t id = 18446744073709551615ULL;   // the widest update id
  const std::string name = inboxDocName(id, longName);
  assertValidDocName(id, longName);
  TEST_ASSERT_EQUAL_UINT(orch::kStoreNameMax, name.size());
  TEST_ASSERT_EQUAL_STRING(".csv", name.substr(name.size() - 4).c_str());
  TEST_ASSERT_TRUE(inboxDocName(id, longName) != inboxDocName(id, std::string(201, 'x') + ".csv"));
}

void test_a_pdf_says_it_cannot_be_read_and_other_types_show_the_path_only() {
  TEST_ASSERT_TRUE(isPdf("application/pdf", "inbox/1-x"));
  TEST_ASSERT_TRUE(isPdf("", "inbox/1-x.PDF"));
  TEST_ASSERT_FALSE(isPdf("text/plain", "inbox/1-x.txt"));
  TEST_ASSERT_EQUAL_STRING((std::string("inbox/1-a.pdf") + kInboxPdfNotRead).c_str(),
                           documentDetail("inbox/1-a.pdf", "application/pdf").c_str());
  TEST_ASSERT_EQUAL_STRING("inbox/1-a.txt", documentDetail("inbox/1-a.txt", "text/plain").c_str());
}

void test_a_paste_over_the_cap_round_trips_through_the_saved_file() {
  std::map<std::string, std::string> card;
  auto save = [&](const std::string& name, const std::string& text) {
    card[name] = text;
    return true;
  };
  std::string paste;
  for (int i = 0; i < 9000; ++i) paste += (i % 7 == 0) ? "\xC3\xA9" : "p";   // multibyte at the cut too
  const size_t cap = 4096;
  const OwnerText out = ownerTextIntake(paste, 4242, cap, save);
  TEST_ASSERT_EQUAL_STRING("inbox/4242.txt", out.saved.c_str());
  TEST_ASSERT_EQUAL_STRING("inbox/4242.txt", inboxPastePath(4242).c_str());
  TEST_ASSERT_EQUAL_UINT(1, card.size());
  TEST_ASSERT_TRUE_MESSAGE(card["4242.txt"] == paste, "the saved file is not the paste byte for byte");
  TEST_ASSERT_TRUE_MESSAGE(out.text.find("read_file path=inbox/4242.txt") != std::string::npos,
                           "the u text does not point at the saved whole");
  TEST_ASSERT_TRUE(out.text.size() < paste.size());
  TEST_ASSERT_TRUE_MESSAGE(startsWith(paste, out.text.substr(0, out.text.find("\n[text continues: "))),
                           "the u text head is not the paste's first bytes");
  TEST_ASSERT_TRUE(out.text.find("\n[text continues: ") >= cap - 3);   // at most one character short
}

void test_a_refused_save_or_a_short_text_passes_whole() {
  const std::string paste(9000, 'q');
  int calls = 0;
  auto refuse = [&](const std::string&, const std::string&) {
    ++calls;
    return false;
  };
  OwnerText out = ownerTextIntake(paste, 5, 4096, refuse);
  TEST_ASSERT_EQUAL(1, calls);
  TEST_ASSERT_TRUE_MESSAGE(out.text == paste, "a refused save cut the text");
  TEST_ASSERT_TRUE(out.saved.empty());
  auto keep = [&](const std::string&, const std::string&) {
    ++calls;
    return true;
  };
  calls = 0;
  for (size_t n : {size_t(0), size_t(1), size_t(4095), size_t(4096)}) {
    out = ownerTextIntake(std::string(n, 's'), 5, 4096, keep);
    TEST_ASSERT_EQUAL_UINT(n, out.text.size());
    TEST_ASSERT_TRUE(out.saved.empty());
  }
  out = ownerTextIntake(paste, 0, 4096, keep);   // no update id: nothing to name the file by
  TEST_ASSERT_TRUE(out.text == paste);
  out = ownerTextIntake(paste, 5, 0, keep);      // no cap set
  TEST_ASSERT_TRUE(out.text == paste);
  out = ownerTextIntake(paste, 5, 4096, nullptr);
  TEST_ASSERT_TRUE(out.text == paste);
  TEST_ASSERT_EQUAL_MESSAGE(0, calls, "a text that needs no file was saved");
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_every_byte_class_makes_a_valid_name_the_update_id_leads);
  RUN_TEST(test_a_name_with_nothing_usable_is_id_dash_file);
  RUN_TEST(test_two_files_with_one_name_are_two_paths);
  RUN_TEST(test_a_long_name_keeps_the_id_and_the_extension);
  RUN_TEST(test_a_pdf_says_it_cannot_be_read_and_other_types_show_the_path_only);
  RUN_TEST(test_a_paste_over_the_cap_round_trips_through_the_saved_file);
  RUN_TEST(test_a_refused_save_or_a_short_text_passes_whole);
  return UNITY_END();
}
