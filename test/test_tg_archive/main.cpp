// Telegram archive (lane R): host tests for the naming, the raw page splitter, the
// content policy class, inbound capture with replay dedupe, the streamed download
// path, outbound capture, the resume pass, storage pressure, the retention
// exemption, and the outbound send-site count over telegram.cpp.
#include <unity.h>

#include <ArduinoJson.h>

#include <algorithm>
#include <cstdio>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "fakes.h"
#include "nimbus/orch/episodic_log.h"
#include "nimbus/orch/retention.h"
#include "nimbus/tg_archive.h"
#include "nimbus/tg_updates.h"

using namespace nimbus::tgarc;
using fakes::Card;
using fakes::TgSource;

void setUp() {}
void tearDown() {}

namespace {

const int64_t kOct18 = 1792330000;   // 2026-10-18, mid-morning UTC

std::string update(long long id, const std::string& chat, long long date, const std::string& body,
                   const std::string& extra = "") {
  char head[256];
  std::snprintf(head, sizeof head,
                "{\"update_id\":%lld,\"message\":{\"message_id\":%lld,\"from\":{\"id\":42,"
                "\"first_name\":\"Sam\"},\"chat\":{\"id\":%s,\"type\":\"private\"},\"date\":%lld",
                id, id + 1000, chat.c_str(), date);
  return std::string(head) + extra + (body.empty() ? "" : "," + body) + "}}";
}

std::string page(const std::vector<std::string>& ups) {
  std::string s = "{\"ok\":true,\"result\":[";
  for (size_t i = 0; i < ups.size(); ++i) s += (i ? "," : "") + ups[i];
  return s + "]}";
}

std::string text(const char* t) { return std::string("\"text\":\"") + t + "\""; }

uint32_t g_ms = 0;
uint32_t fakeMs() { return g_ms; }

Step step(Archive& a, TgSource& src, uint32_t budget = 0xFFFFFFFFu, size_t cap = 4096) {
  static std::vector<uint8_t> buf(64 * 1024);
  return a.downloadStep(src, buf.data(), cap, budget, fakeMs, [] { return false; });
}

void drain(Archive& a, TgSource& src) {
  for (int i = 0; i < 200 && a.pending(); ++i) step(a, src);
}

JsonDocument parseLine(const std::string& l) {
  JsonDocument d;
  TEST_ASSERT_FALSE_MESSAGE(deserializeJson(d, l), l.c_str());
  return d;
}

std::string idx(const std::string& chat, const std::string& day) { return indexPathOf(chat, day); }

}  // namespace

// ---- names --------------------------------------------------------------------

static void test_archive_path_predicate() {
  TEST_ASSERT_TRUE(isArchivePath("/mem/tg"));
  TEST_ASSERT_TRUE(isArchivePath("/mem/tg/123/2026-10-18/5.json"));
  TEST_ASSERT_FALSE(isArchivePath("/mem/tgx/1"));
  TEST_ASSERT_FALSE(isArchivePath("/mem/blobs/ab.jpg"));
  TEST_ASSERT_FALSE(isArchivePath("/mem/episodic/2026-10-18.jsonl"));
  TEST_ASSERT_FALSE(isArchivePath("/mem"));
  // The archive lives inside the durable store root the factory sweep removes.
  TEST_ASSERT_EQUAL_INT(0, std::string(kRoot).compare(0, 5, "/mem/"));
}

static void test_names_and_days() {
  TEST_ASSERT_TRUE(validChatDir("6098532481"));
  TEST_ASSERT_TRUE(validChatDir("-1001234567890"));
  TEST_ASSERT_TRUE(validChatDir("restored"));
  TEST_ASSERT_FALSE(validChatDir("web"));
  TEST_ASSERT_FALSE(validChatDir("serial"));
  TEST_ASSERT_FALSE(validChatDir(""));
  TEST_ASSERT_FALSE(validChatDir("-"));
  TEST_ASSERT_FALSE(validChatDir("../etc"));
  TEST_ASSERT_TRUE(validDay("2026-10-18"));
  TEST_ASSERT_TRUE(validDay("undated"));
  TEST_ASSERT_FALSE(validDay("2026-13-01"));
  TEST_ASSERT_FALSE(validDay("2026-10-1x"));
  TEST_ASSERT_TRUE(validItemName("123.jpg"));
  TEST_ASSERT_TRUE(validItemName("o7.json"));
  TEST_ASSERT_FALSE(validItemName("../x"));
  TEST_ASSERT_FALSE(validItemName(".pressure"));
  TEST_ASSERT_FALSE(validItemName("a/b"));
  TEST_ASSERT_EQUAL_STRING("2026-10-18", dayFromEpoch(kOct18).c_str());
  TEST_ASSERT_EQUAL_STRING("1970-01-01" == dayFromEpoch(0) ? "x" : "undated", dayFromEpoch(0).c_str());
  TEST_ASSERT_EQUAL_STRING("undated", dayFromEpoch(5000).c_str());   // an unset device clock
  TEST_ASSERT_EQUAL_STRING("/mem/tg/42/2026-10-18/index.jsonl", indexPathOf("42", "2026-10-18").c_str());
  TEST_ASSERT_EQUAL_UINT32(1, nextOutSeq({}));
  TEST_ASSERT_EQUAL_UINT32(10, nextOutSeq({"o9.json", "o3.jpg", "123.json", "index.jsonl", "o.json", "oX.json"}));
}

// TGINJECT (test builds): "LOCAL_/mem/..." names a card file; nothing else does.
static void test_local_inject_path() {
  std::string path;
  TEST_ASSERT_TRUE(localInjectPath("LOCAL_/mem/inject/p1.jpg", path));
  TEST_ASSERT_EQUAL_STRING("/mem/inject/p1.jpg", path.c_str());
  for (const char* bad : {"LOCAL_/etc/passwd", "LOCAL_/mem/../log/x", "LOCAL_mem/x", "LOCAL_/mem/a b",
                          "LOCAL_/mem/", "AgACAgQAAxkBAAI", "local_/mem/x", "LOCAL_/mem/x?y"})
    TEST_ASSERT_FALSE_MESSAGE(localInjectPath(bad, path), bad);
}

// ---- raw page splitter --------------------------------------------------------

static void test_split_result_spans() {
  const std::string u1 = update(1, "5", kOct18, text("a ] } [ { \\\" tricky"));
  const std::string u2 = update(2, "5", kOct18, "\"photo\":[{\"file_id\":\"x\",\"width\":1,\"height\":1}]");
  const std::string body = "{\"ok\":true, \"result\" : [ " + u1 + " ,\n" + u2 + " ] }";
  std::vector<Span> sp;
  TEST_ASSERT_TRUE(splitResult(body.data(), body.size(), sp));
  TEST_ASSERT_EQUAL_INT(2, (int)sp.size());
  TEST_ASSERT_EQUAL_STRING(u1.c_str(), body.substr(sp[0].off, sp[0].len).c_str());
  TEST_ASSERT_EQUAL_STRING(u2.c_str(), body.substr(sp[1].off, sp[1].len).c_str());
  // "result" may come before "ok"; an empty result is a valid page with nothing in it.
  const std::string b2 = "{\"result\":[],\"ok\":true}";
  TEST_ASSERT_TRUE(splitResult(b2.data(), b2.size(), sp));
  TEST_ASSERT_EQUAL_INT(0, (int)sp.size());
  // A body cut mid-update keeps only the whole updates before the cut.
  const std::string cut = body.substr(0, body.size() - 20);
  TEST_ASSERT_FALSE(splitResult(cut.data(), cut.size(), sp));
  TEST_ASSERT_EQUAL_INT(1, (int)sp.size());
  const std::string notOk = "{\"ok\":false,\"description\":\"Unauthorized\"}";
  TEST_ASSERT_FALSE(splitResult(notOk.data(), notOk.size(), sp));
}

static void test_meta_largest_photo_and_album_fields() {
  const std::string u = update(77, "-100200", kOct18,
      "\"photo\":[{\"file_id\":\"small\",\"file_size\":1200,\"width\":90,\"height\":67},"
      "{\"file_id\":\"mid\",\"file_size\":90000,\"width\":1280,\"height\":960},"
      "{\"file_id\":\"big\",\"file_unique_id\":\"U\",\"file_size\":2100000,\"width\":2560,\"height\":1920}],"
      "\"caption\":\"dinner\"", ",\"media_group_id\":\"13579\"");
  InMeta m;
  TEST_ASSERT_TRUE(parseUpdateMeta(u.data(), u.size(), m));
  TEST_ASSERT_EQUAL_INT64(77, m.updateId);
  TEST_ASSERT_EQUAL_STRING("message", m.updateType.c_str());
  TEST_ASSERT_EQUAL_STRING("-100200", m.chat.c_str());
  TEST_ASSERT_EQUAL_STRING("Sam", m.from.c_str());
  TEST_ASSERT_EQUAL_STRING("42", m.fromId.c_str());
  TEST_ASSERT_EQUAL_INT64(1077, m.messageId);
  TEST_ASSERT_EQUAL_INT64(kOct18, m.date);
  TEST_ASSERT_EQUAL_STRING("13579", m.mediaGroupId.c_str());
  TEST_ASSERT_TRUE(m.content == Content::Photo);
  TEST_ASSERT_TRUE(m.hasMedia);
  // The largest rendition, never the model path's budget-bound one (that would be "mid").
  TEST_ASSERT_EQUAL_STRING("big", m.media.fileId.c_str());
  TEST_ASSERT_EQUAL_UINT64(2100000, m.media.size);
  TEST_ASSERT_EQUAL_STRING("jpg", mediaExt(m.media).c_str());
  // An edit carries its edit time.
  const std::string e = "{\"update_id\":9,\"edited_message\":{\"message_id\":3,\"chat\":{\"id\":5},"
                        "\"date\":100,\"edit_date\":" + std::to_string(kOct18) + ",\"text\":\"fixed\"}}";
  TEST_ASSERT_TRUE(parseUpdateMeta(e.data(), e.size(), m));
  TEST_ASSERT_TRUE(m.edited);
  TEST_ASSERT_EQUAL_INT64(kOct18, m.date);
  // A non-message update keeps its type, chat and sender.
  const std::string cb = "{\"update_id\":10,\"callback_query\":{\"id\":\"q\",\"from\":{\"id\":7,\"first_name\":\"Al\"},"
                         "\"message\":{\"message_id\":1,\"date\":" + std::to_string(kOct18) + ",\"chat\":{\"id\":5}}}}";
  TEST_ASSERT_TRUE(parseUpdateMeta(cb.data(), cb.size(), m));
  TEST_ASSERT_EQUAL_STRING("callback_query", m.updateType.c_str());
  TEST_ASSERT_EQUAL_STRING("5", m.chat.c_str());
  TEST_ASSERT_EQUAL_INT64(kOct18, m.date);
  // Not an update.
  TEST_ASSERT_FALSE(parseUpdateMeta("{\"x\":1}", 7, m));
  TEST_ASSERT_FALSE(parseUpdateMeta("[1]", 3, m));
}

static void test_media_ext_never_shadows_a_record() {
  Media m;
  m.type = Content::Document;
  m.fileName = "data.JSON";
  TEST_ASSERT_EQUAL_STRING("json.bin", mediaExt(m).c_str());
  m.fileName = "report.pdf";
  TEST_ASSERT_EQUAL_STRING("pdf", mediaExt(m).c_str());
  m.fileName = "noext";
  m.mime = "image/png";
  TEST_ASSERT_EQUAL_STRING("png", mediaExt(m).c_str());
  m.mime = "";
  TEST_ASSERT_EQUAL_STRING("bin", mediaExt(m).c_str());
  m.fileName = "../../x.p/df";
  TEST_ASSERT_EQUAL_STRING("bin", mediaExt(m).c_str());
}

// ---- the class rule: every type is archived or explicitly not archived ----------

// One raw message body per content type. A new Content value without a fixture
// here fails test_every_content_type_is_archived_or_explicitly_not.
struct Fixture { Content c; const char* body; };
static const Fixture kFixtures[] = {
    {Content::Text, "\"text\":\"hello\""},
    {Content::Photo, "\"photo\":[{\"file_id\":\"F-photo\",\"file_size\":10,\"width\":90,\"height\":90},{\"file_id\":\"F-photo-big\",\"file_size\":40,\"width\":900,\"height\":900}]"},
    {Content::Document, "\"document\":{\"file_id\":\"F-document\",\"file_name\":\"notes.pdf\",\"mime_type\":\"application/pdf\",\"file_size\":40}"},
    {Content::Voice, "\"voice\":{\"file_id\":\"F-voice\",\"mime_type\":\"audio/ogg\",\"file_size\":40,\"duration\":2}"},
    {Content::Audio, "\"audio\":{\"file_id\":\"F-audio\",\"mime_type\":\"audio/mpeg\",\"file_name\":\"song.mp3\",\"file_size\":40}"},
    {Content::Video, "\"video\":{\"file_id\":\"F-video\",\"mime_type\":\"video/mp4\",\"file_size\":40,\"width\":64,\"height\":48}"},
    {Content::VideoNote, "\"video_note\":{\"file_id\":\"F-video_note\",\"length\":240,\"file_size\":40}"},
    {Content::Sticker, "\"sticker\":{\"file_id\":\"F-sticker\",\"is_animated\":true,\"is_video\":false,\"file_size\":40}"},
    {Content::Animation, "\"animation\":{\"file_id\":\"F-animation\",\"mime_type\":\"video/mp4\",\"file_size\":40},\"document\":{\"file_id\":\"F-animation\",\"mime_type\":\"video/mp4\"}"},
    {Content::Location, "\"location\":{\"latitude\":51.5,\"longitude\":-0.12}"},
    {Content::Contact, "\"contact\":{\"phone_number\":\"+10000000000\",\"first_name\":\"A\"}"},
    {Content::Venue, "\"venue\":{\"location\":{\"latitude\":1,\"longitude\":2},\"title\":\"T\",\"address\":\"A\"},\"location\":{\"latitude\":1,\"longitude\":2}"},
    {Content::Poll, "\"poll\":{\"id\":\"1\",\"question\":\"Q\",\"options\":[]}"},
    {Content::Dice, "\"dice\":{\"emoji\":\"\\u2680\",\"value\":3}"},
    {Content::Service, "\"new_chat_title\":\"Team\""},
    {Content::Story, "\"story\":{\"chat\":{\"id\":1},\"id\":5}"},
    {Content::PaidMedia, "\"paid_media\":{\"star_count\":1,\"paid_media\":[]}"},
    {Content::Game, "\"game\":{\"title\":\"G\",\"description\":\"d\",\"photo\":[]}"},
    {Content::Invoice, "\"invoice\":{\"title\":\"I\",\"description\":\"d\",\"start_parameter\":\"s\",\"currency\":\"USD\",\"total_amount\":1}"},
    {Content::Unknown, "\"checklist\":{\"title\":\"c\"}"},
};

// One raw message body per content type. A new Content value without a fixture
// here fails test_every_content_type_is_archived_or_explicitly_not.
static const char* fixtureFor(Content c) {
  for (const Fixture& f : kFixtures)
    if (f.c == c) return f.body;
  return nullptr;
}

static void test_every_parser_kind_has_an_archive_decision() {
  using K = nimbus::tg::Attachment::Kind;
  for (int k = 0; k < (int)K::Count; ++k) {
    const Content c = contentForKind((K)k);
    char msg[64];
    std::snprintf(msg, sizeof msg, "parser kind %d has no archive decision", k);
    TEST_ASSERT_TRUE_MESSAGE(contentInfo(c).policy != Policy::Unknown, msg);
  }
  // The sentinel itself has none (this is what a new kind would read).
  TEST_ASSERT_TRUE(contentInfo(contentForKind(K::Count)).policy == Policy::Unknown);
}

static void test_every_content_type_is_archived_or_explicitly_not() {
  for (int ci = 0; ci < (int)Content::Count; ++ci) {
    const Content c = (Content)ci;
    const ContentInfo info = contentInfo(c);
    char msg[96];
    std::snprintf(msg, sizeof msg, "content %d (%s)", ci, info.name);
    TEST_ASSERT_TRUE_MESSAGE(info.policy != Policy::Unknown, msg);
    const char* body = fixtureFor(c);
    TEST_ASSERT_NOT_NULL_MESSAGE(body, msg);
    Card card;
    Archive a(card);
    TgSource src;
    src.files["F-" + std::string(info.name)] = fakes::bytes(40, (uint32_t)ci);
    src.files["F-photo-big"] = fakes::bytes(40, 99);
    const std::string p = page({update(100 + ci, "5", kOct18, body)});
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, a.captureInboundPage(p.data(), p.size(), {kOct18, 0}), msg);
    InMeta m;
    const std::string u = update(100 + ci, "5", kOct18, body);
    TEST_ASSERT_TRUE(parseUpdateMeta(u.data(), u.size(), m));
    TEST_ASSERT_TRUE_MESSAGE(m.content == c, msg);
    drain(a, src);
    const std::vector<std::string> rows = card.lines(idx("5", "2026-10-18"));
    TEST_ASSERT_TRUE_MESSAGE(!rows.empty(), msg);
    JsonDocument row = parseLine(rows[0]);
    const std::string st = row["st"] | "";
    TEST_ASSERT_EQUAL_STRING_MESSAGE(info.name, (row["kind"] | ""), msg);
    TEST_ASSERT_TRUE_MESSAGE(card.files.count("/mem/tg/5/2026-10-18/" + std::to_string(100 + ci) + ".json"), msg);
    if (info.policy == Policy::Download) {
      TEST_ASSERT_EQUAL_STRING_MESSAGE("pending", st.c_str(), msg);
      const std::string file = row["file"] | "";
      TEST_ASSERT_TRUE_MESSAGE(card.files.count("/mem/tg/5/2026-10-18/" + file), msg);
      JsonDocument ev = parseLine(rows.back());
      TEST_ASSERT_EQUAL_STRING_MESSAGE("ok", (ev["st"] | ""), msg);
    } else if (info.policy == Policy::RawOnly) {
      TEST_ASSERT_EQUAL_STRING_MESSAGE("ok", st.c_str(), msg);
      TEST_ASSERT_TRUE_MESSAGE(row["file"].isNull(), msg);
    } else {
      TEST_ASSERT_EQUAL_STRING_MESSAGE((std::string("not archived: ") + info.name).c_str(), st.c_str(), msg);
    }
  }
}

// ---- inbound capture and replay dedupe ----------------------------------------

static void test_inbound_writes_raw_update_and_index_row() {
  Card card;
  Archive a(card);
  const std::string u = update(500, "6098", kOct18, text("hi there"));
  const std::string p = page({u});
  TEST_ASSERT_EQUAL_INT(1, a.captureInboundPage(p.data(), p.size(), {0, 0}));
  // The raw update exactly as it arrived.
  TEST_ASSERT_EQUAL_STRING(u.c_str(), card.files["/mem/tg/6098/2026-10-18/500.json"].c_str());
  const auto rows = card.lines(idx("6098", "2026-10-18"));
  TEST_ASSERT_EQUAL_INT(1, (int)rows.size());
  JsonDocument r = parseLine(rows[0]);
  TEST_ASSERT_EQUAL_STRING("in", r["dir"] | "");
  TEST_ASSERT_EQUAL_INT64(500, r["u"] | 0ll);
  TEST_ASSERT_EQUAL_STRING("6098", r["chat"] | "");
  TEST_ASSERT_EQUAL_STRING("Sam", r["from"] | "");
  TEST_ASSERT_EQUAL_INT64(1500, r["mid"] | 0ll);
  TEST_ASSERT_EQUAL_INT64(kOct18, r["date"] | 0ll);
  TEST_ASSERT_EQUAL_STRING("text", r["kind"] | "");
  TEST_ASSERT_EQUAL_STRING("ok", r["st"] | "");
  TEST_ASSERT_EQUAL_INT(0, card.violations);
  // No temp file is left behind.
  for (const auto& kv : card.files) TEST_ASSERT_FALSE(fakes::endsWith(kv.first, ".tmp"));
}

static void test_replay_of_the_same_page_adds_nothing() {
  Card card;
  Archive a(card);
  TgSource src;
  src.files["P"] = fakes::bytes(3000, 1);
  const std::string p = page({update(1, "9", kOct18, text("one")),
                              update(2, "9", kOct18, "\"photo\":[{\"file_id\":\"P\",\"width\":9,\"height\":9}]"),
                              update(3, "9", kOct18, "\"text\":\"/help\"")});
  TEST_ASSERT_EQUAL_INT(3, a.captureInboundPage(p.data(), p.size(), {kOct18, 0}));
  drain(a, src);
  const auto before = card.files;
  for (int replay = 0; replay < 3; ++replay)
    TEST_ASSERT_EQUAL_INT(0, a.captureInboundPage(p.data(), p.size(), {kOct18, 0}));
  // A fresh boot (new engine, no RAM state) dedupes by name all the same.
  Archive b(card);
  TEST_ASSERT_EQUAL_INT(0, b.captureInboundPage(p.data(), p.size(), {kOct18, 0}));
  TEST_ASSERT_TRUE(before == card.files);
  TEST_ASSERT_EQUAL_UINT32(9, a.stats().dupes);
  TEST_ASSERT_EQUAL_UINT32(3, b.stats().dupes);
  TEST_ASSERT_EQUAL_UINT32(0, b.pending());
  TEST_ASSERT_EQUAL_INT(0, card.violations);
}

static void test_replay_restores_an_index_line_lost_to_a_crash() {
  Card card;
  Archive a(card);
  const std::string p = page({update(11, "9", kOct18, text("x"))});
  a.captureInboundPage(p.data(), p.size(), {0, 0});
  card.files[idx("9", "2026-10-18")].clear();   // the crash: raw written, line lost
  Archive b(card);
  TEST_ASSERT_EQUAL_INT(0, b.captureInboundPage(p.data(), p.size(), {0, 0}));
  TEST_ASSERT_EQUAL_UINT32(1, b.stats().repaired);
  TEST_ASSERT_EQUAL_INT(1, (int)card.lines(idx("9", "2026-10-18")).size());
  // And only once.
  b.captureInboundPage(p.data(), p.size(), {0, 0});
  TEST_ASSERT_EQUAL_INT(1, (int)card.lines(idx("9", "2026-10-18")).size());
}

// The repair runs with the trust of the update being repaired, never the previous
// one's: a stranger's photo repaired right after an owner's update is not fetched.
static void test_repair_uses_this_updates_trust() {
  Card card;
  auto trust = [](const std::string& c) { return c == "9" ? Trust::Allowed : Trust::Stranger; };
  const std::string photo = "\"photo\":[{\"file_id\":\"P\",\"width\":9,\"height\":9}]";
  const std::string stranger = page({update(31, "666", kOct18, photo)});
  {
    Archive a(card);
    a.setTrust(trust);
    a.captureInboundPage(stranger.data(), stranger.size(), {0, 0});
  }
  card.files[idx("666", "2026-10-18")].clear();   // the crash
  Archive b(card);
  b.setTrust(trust);
  const std::string owner = page({update(30, "9", kOct18, text("hi")), update(31, "666", kOct18, photo)});
  b.captureInboundPage(owner.data(), owner.size(), {0, 0});
  TEST_ASSERT_EQUAL_UINT32(1, b.stats().repaired);
  TEST_ASSERT_EQUAL_UINT32(0, b.pending());   // nothing queued for the stranger
  JsonDocument r = parseLine(card.lines(idx("666", "2026-10-18"))[0]);
  TEST_ASSERT_EQUAL_STRING("not archived: sender not on the allowlist", r["st"] | "");
}

static void test_secrets_never_reach_the_card() {
  Card card;
  Archive a(card);
  const std::string token = std::string("12345:") + "AAFakeTokenForTests";
  const std::string key = std::string("sk-ant-") + "fakekeyfortests0000";
  a.setRedactions({token, key, "short"});
  // Inbound, outbound text, an outbound caption and a source path all carry one.
  const std::string p = page({update(4, "9", kOct18, std::string("\"text\":\"leak ") + token + "\"")});
  a.captureInboundPage(p.data(), p.size(), {0, 0});
  a.captureOutText("9", "my key is " + key, "send", {kOct18, 0});
  Card lfs;
  lfs.mkdirs("/a");
  lfs.files["/a/x.txt"] = "file";
  a.captureOutMedia(OutMedia{"9", "document", "/a/x.txt", "token " + token, "document"}, lfs, {kOct18, 0});
  for (const auto& kv : card.files) {
    TEST_ASSERT_EQUAL(std::string::npos, kv.second.find(token));
    TEST_ASSERT_EQUAL(std::string::npos, kv.second.find(key));
  }
  TEST_ASSERT_NOT_EQUAL(std::string::npos, card.files["/mem/tg/9/2026-10-18/4.json"].find("[redacted]"));
  TEST_ASSERT_NOT_EQUAL(std::string::npos, card.files["/mem/tg/9/2026-10-18/o1.json"].find("[redacted]"));
}

// A sender name or text with raw control bytes still makes valid index lines.
static void test_control_bytes_keep_the_index_valid() {
  Card card;
  Archive a(card);
  std::string u = update(5, "9", kOct18, text("ok"));
  const size_t at = u.find("Sam");
  u.replace(at, 3, std::string("S\x01a\x1fm"));   // raw bytes, as a lax sender could produce
  const std::string p = page({u});
  a.captureInboundPage(p.data(), p.size(), {0, 0});
  for (const auto& l : card.lines(idx("9", "2026-10-18"))) {
    for (char c : l) TEST_ASSERT_TRUE((unsigned char)c >= 0x20);
    parseLine(l);
  }
  TEST_ASSERT_EQUAL_STRING("a\\u0001b", escapeControls(std::string("a\x01" "b")).c_str());
}

static void test_album_items_share_a_group_and_keep_their_own_names() {
  Card card;
  Archive a(card);
  TgSource src;
  std::vector<std::string> ups;
  for (int i = 0; i < 3; ++i) {
    const std::string id = "A" + std::to_string(i);
    src.files[id] = fakes::bytes(1000 + (size_t)i, (uint32_t)i);
    ups.push_back(update(20 + i, "9", kOct18,
                         "\"photo\":[{\"file_id\":\"" + id + "\",\"width\":9,\"height\":9}]" +
                             (i == 0 ? ",\"caption\":\"all of this\"" : ""),
                         ",\"media_group_id\":\"777\""));
  }
  const std::string p = page(ups);
  TEST_ASSERT_EQUAL_INT(3, a.captureInboundPage(p.data(), p.size(), {0, 0}));
  drain(a, src);
  for (int i = 0; i < 3; ++i) {
    const std::string f = "/mem/tg/9/2026-10-18/" + std::to_string(20 + i) + ".jpg";
    TEST_ASSERT_TRUE(card.files.count(f));
    TEST_ASSERT_EQUAL_UINT64(1000 + (size_t)i, card.files[f].size());
  }
  int grouped = 0;
  for (const auto& l : card.lines(idx("9", "2026-10-18"))) {
    JsonDocument d = parseLine(l);
    if (std::string(d["mg"] | "") == "777") ++grouped;
  }
  TEST_ASSERT_EQUAL_INT(3, grouped);
}

static void test_strangers_are_text_only_and_capped() {
  Card card;
  Archive a(card);
  a.setTrust([](const std::string& c) { return c == "9" ? Trust::Allowed : Trust::Stranger; });
  TgSource src;
  src.files["P"] = fakes::bytes(100, 1);
  const std::string photo = "\"photo\":[{\"file_id\":\"P\",\"width\":9,\"height\":9}]";
  const std::string p1 = page({update(1, "666", kOct18, photo), update(2, "9", kOct18, photo)});
  TEST_ASSERT_EQUAL_INT(2, a.captureInboundPage(p1.data(), p1.size(), {0, 0}));
  TEST_ASSERT_EQUAL_UINT32(1, a.pending());   // only the allowed chat's photo
  drain(a, src);
  TEST_ASSERT_TRUE(card.files.count("/mem/tg/9/2026-10-18/2.jpg"));
  TEST_ASSERT_FALSE(card.files.count("/mem/tg/666/2026-10-18/1.jpg"));
  JsonDocument r = parseLine(card.lines(idx("666", "2026-10-18"))[0]);
  TEST_ASSERT_EQUAL_STRING("not archived: sender not on the allowlist", r["st"] | "");
  // A flood from one stranger stops at the per-chat cap; the owner's chat never does.
  for (long long i = 0; i < 200; ++i) {
    const std::string p = page({update(100 + i, "666", kOct18, text("spam")), update(1000 + i, "9", kOct18, text("me"))});
    a.captureInboundPage(p.data(), p.size(), {0, 0});
  }
  TEST_ASSERT_EQUAL_size_t(kStrangerDailyCap, card.lines(idx("666", "2026-10-18")).size());
  TEST_ASSERT_EQUAL_size_t(201, card.lines(idx("9", "2026-10-18")).size() - 1);   // + the photo's event
  TEST_ASSERT_TRUE(a.stats().strangerSkipped > 0);
}

// Strangers rotating through 40 chat ids (several accounts, or the bot added to
// groups) never get past the global budget or the chat-directory cap, and the
// owner's chat is untouched, both while the flood runs and under storage pressure.
static void test_stranger_rotation_holds_the_global_budget() {
  Card card;
  Archive a(card);
  a.setTrust([](const std::string& c) { return c == "9" ? Trust::Allowed : Trust::Stranger; });
  for (int d = 1; d <= 3; ++d) {   // the owner's history
    char day[16];
    std::snprintf(day, sizeof day, "2026-10-%02d", 14 + d);
    card.mkdirs(dayDirPath("9", day));
    card.files[dayDirPath("9", day) + "/1.json"] = std::string(500, 'o');
  }
  long long id = 5000;
  for (int round = 0; round < 30; ++round)
    for (int c = 0; c < 40; ++c) {
      const std::string p = page({update(++id, std::to_string(7000 + c), kOct18, text("spam spam spam"))});
      a.captureInboundPage(p.data(), p.size(), {0, 0});
    }
  size_t strangerRaw = 0, strangerBytes = 0, strangerDirs = 0;
  for (const std::string& chat : listChats(card)) {
    if (chat == "9") continue;
    if (chat != "0") ++strangerDirs;
    for (const auto& kv : card.files)
      if (kv.first.compare(0, chatDirPath(chat).size() + 1, chatDirPath(chat) + "/") == 0 &&
          fakes::endsWith(kv.first, ".json")) {
        ++strangerRaw;
        strangerBytes += kv.second.size();
      }
  }
  TEST_ASSERT_TRUE(strangerRaw <= kStrangerDayUpdates);
  TEST_ASSERT_TRUE(strangerBytes <= kStrangerDayBytes);
  TEST_ASSERT_TRUE(strangerDirs <= kStrangerChatsMax);
  TEST_ASSERT_TRUE(a.stats().strangerSkipped >= 30u * 40u - kStrangerDayUpdates);
  // Chats past the directory cap got one explicit row each in "0", inside the budget.
  bool overflowRow = false;
  for (const auto& l : card.lines(idx("0", "2026-10-18")))
    overflowRow |= l.find("not archived: stranger limit") != std::string::npos;
  TEST_ASSERT_TRUE(overflowRow);
  // The owner still writes freely today.
  const std::string mine = page({update(++id, "9", kOct18, text("still here"))});
  TEST_ASSERT_EQUAL_INT(1, a.captureInboundPage(mine.data(), mine.size(), {0, 0}));
  // Under pressure, every stranger day goes before any of the owner's days.
  const uint64_t total = 16ull << 30;
  PressureOutcome o = a.pressurePass(lowWater(total) - 1, total);
  TEST_ASSERT_TRUE(o.removedDays > 0);
  for (const std::string& chat : listChats(card)) TEST_ASSERT_EQUAL_STRING("9", chat.c_str());
  TEST_ASSERT_TRUE(card.files.count("/mem/tg/9/2026-10-18/" + std::to_string(id) + ".json"));
}

// A restart mid-day resumes the day's stranger budget from the card.
static void test_stranger_budget_survives_a_restart() {
  Card card;
  auto trust = [](const std::string& c) { return c == "9" ? Trust::Allowed : Trust::Stranger; };
  long long id = 1;
  {
    Archive a(card);
    a.setTrust(trust);
    for (int c = 0; c < 10; ++c)
      for (int k = 0; k < 20; ++k) {   // 200 updates: the global budget, spent
        const std::string p = page({update(id++, std::to_string(300 + c), kOct18, text("x"))});
        a.captureInboundPage(p.data(), p.size(), {0, 0});
      }
  }
  // Records that are not stranger updates never count against the budget.
  card.files[dayDirPath("300", "2026-10-18") + "/o1.json"] = "{}";
  card.files[dayDirPath("300", "2026-10-18") + "/r0123456789abcdef.json"] = "{}";
  Archive b(card);   // the restart
  b.setTrust(trust);
  const size_t before = card.files.size();
  const std::string more = page({update(id++, "301", kOct18, text("again"))});
  TEST_ASSERT_EQUAL_INT(0, b.captureInboundPage(more.data(), more.size(), {0, 0}));
  TEST_ASSERT_EQUAL_size_t(before, card.files.size());
  TEST_ASSERT_EQUAL_UINT32(1, b.stats().strangerSkipped);
}

// A replayed page never charges or records an over-the-limit update twice.
static void test_stranger_overflow_replay_is_not_charged_twice() {
  Card card;
  Archive a(card);
  a.setTrust([](const std::string&) { return Trust::Stranger; });
  long long id = 1;
  for (int c = 0; c < (int)kStrangerChatsMax; ++c) {   // fill the folder cap
    const std::string p = page({update(id++, std::to_string(100 + c), kOct18, text("x"))});
    a.captureInboundPage(p.data(), p.size(), {0, 0});
  }
  const std::string over = page({update(id++, "999", kOct18, text("over"))});
  a.captureInboundPage(over.data(), over.size(), {0, 0});
  TEST_ASSERT_EQUAL_size_t(1, card.lines(idx("0", "2026-10-18")).size());
  for (int r = 0; r < 3; ++r) a.captureInboundPage(over.data(), over.size(), {0, 0});
  TEST_ASSERT_EQUAL_size_t(1, card.lines(idx("0", "2026-10-18")).size());
  TEST_ASSERT_EQUAL_UINT32(3, a.stats().dupes);
}

// A full per-chat table denies; it never forgets a counted chat to make room.
static void test_stranger_table_never_evicts() {
  Card card;
  Archive a(card);
  a.setTrust([](const std::string&) { return Trust::Stranger; });
  long long id = 1;
  for (int c = 0; c < (int)kStrangerTableMax; ++c)   // fill the table, chat 100 at its cap
    for (uint32_t k = 0; k < (c == 0 ? kStrangerDailyCap : 1u); ++k) {
      const std::string p = page({update(id++, std::to_string(100 + c), kOct18, text("x"))});
      a.captureInboundPage(p.data(), p.size(), {0, 0});
    }
  // Chats past the directory cap got one row each in "0".
  TEST_ASSERT_EQUAL_size_t(kStrangerTableMax - kStrangerChatsMax, card.lines(idx("0", "2026-10-18")).size());
  const size_t before = card.files.size();
  const std::string extra = page({update(id++, "999", kOct18, text("new"))});
  TEST_ASSERT_EQUAL_INT(0, a.captureInboundPage(extra.data(), extra.size(), {0, 0}));
  const std::string again = page({update(id++, "100", kOct18, text("more"))});
  TEST_ASSERT_EQUAL_INT(0, a.captureInboundPage(again.data(), again.size(), {0, 0}));
  TEST_ASSERT_EQUAL_size_t(before, card.files.size());   // denied: nothing written at all
  TEST_ASSERT_EQUAL_size_t(kStrangerTableMax - kStrangerChatsMax, card.lines(idx("0", "2026-10-18")).size());
  TEST_ASSERT_EQUAL_size_t(kStrangerDailyCap, card.lines(idx("100", "2026-10-18")).size());
  // The next day starts fresh budgets for a chat that has its directory.
  const std::string tomorrow = page({update(id++, "100", kOct18 + 86400, text("hello"))});
  TEST_ASSERT_EQUAL_INT(1, a.captureInboundPage(tomorrow.data(), tomorrow.size(), {0, 0}));
}

// ---- naming property: no collision over bursts, reboots and clock jumps ---------

struct NamingRun {
  Card card;
  Card src;
  std::unique_ptr<Archive> a;
  std::map<long long, std::string> updates;   // an id always carries the same JSON
  size_t outOk = 0;
  std::mt19937 rng;
  explicit NamingRun(uint32_t seed) : a(new Archive(card)), rng(seed) {
    src.mkdirs("/mem/files");
    src.files["/mem/files/pic.jpg"] = fakes::bytes(500, seed);
  }
  int64_t clock() {
    static const int64_t days[] = {kOct18 - 86400, kOct18, kOct18 + 86400, 0, 999};
    return days[rng() % 5];   // jumps back and forth, or unset
  }
  void inbound(int op) {
    std::vector<std::string> ups;
    const int n = 1 + (int)(rng() % 10);
    for (int i = 0; i < n; ++i) {
      const long long id = 1 + (long long)(rng() % 60);
      if (!updates.count(id)) updates[id] = update(id, (rng() % 2) ? "7" : "-8", clock(), text("m"));
      ups.push_back(updates[id]);
    }
    const std::string p = page(ups);
    a->captureInboundPage(p.data(), p.size(), {0, (uint32_t)op * 1000});
  }
  void outText(int op) {
    if (a->captureOutText((rng() % 2) ? "7" : "-8", "reply", "reply", {clock(), (uint32_t)op * 1000})) ++outOk;
  }
  void outMedia(int op) {
    OutMedia om{"7", "photo", "/mem/files/pic.jpg", "c", "media"};
    if (a->captureOutMedia(om, src, {clock(), (uint32_t)op * 1000})) ++outOk;
  }
  void step(int op) {
    const int kind = (int)(rng() % 10);
    if (kind < 4) inbound(op);
    else if (kind < 7) outText(op);
    else if (kind < 9) outMedia(op);
    else a.reset(new Archive(card));   // reboot: no RAM state survives
  }
};

static void test_naming_property_no_collisions() {
  for (uint32_t seed = 1; seed <= 12; ++seed) {
    NamingRun run(seed);
    for (int op = 0; op < 250; ++op) run.step(op);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, run.card.violations, "a finished file was overwritten");
    size_t outRaw = 0, inRaw = 0, outRows = 0;
    for (const auto& kv : run.card.files) {
      const std::string name = kv.first.substr(kv.first.find_last_of('/') + 1);
      const bool json = fakes::endsWith(name, ".json");
      outRaw += json && name[0] == 'o';
      inRaw += json && name[0] != 'o';
      if (!fakes::endsWith(name, "index.jsonl")) continue;
      for (const auto& l : run.card.lines(kv.first)) outRows += l.find("\"dir\":\"out\"") != std::string::npos;
    }
    TEST_ASSERT_EQUAL_UINT64(run.outOk, outRaw);
    TEST_ASSERT_EQUAL_UINT64(run.outOk, outRows);
    TEST_ASSERT_EQUAL_UINT64(run.updates.size(), inRaw);   // one raw file per distinct update
  }
}

// ---- streamed download property --------------------------------------------------

static void captureOneMedia(Archive& a, long long id, const std::string& fileId, uint64_t size) {
  char body[200];
  std::snprintf(body, sizeof body, "\"document\":{\"file_id\":\"%s\",\"file_name\":\"f.bin\",\"file_size\":%llu}",
                fileId.c_str(), (unsigned long long)size);
  const std::string p = page({update(id, "9", kOct18, body)});
  a.captureInboundPage(p.data(), p.size(), {0, 0});
}

static void test_streamed_download_any_size_up_to_the_limit() {
  std::mt19937 rng(3);
  std::vector<size_t> sizes = {1, 4095, 4096, 4097, (size_t)kMaxFileBytes};
  for (int i = 0; i < 12; ++i) sizes.push_back(1 + rng() % (512 * 1024));
  long long id = 1000;
  for (size_t sz : sizes) {
    Card card;
    Archive a(card);
    TgSource src;
    src.files["D"] = fakes::bytes(sz, (uint32_t)sz);
    captureOneMedia(a, ++id, "D", sz);
    const size_t cap = 512 + rng() % 8192;
    TEST_ASSERT_TRUE(step(a, src, 0xFFFFFFFFu, cap) == Step::Done);
    const std::string f = "/mem/tg/9/2026-10-18/" + std::to_string(id) + ".bin";
    TEST_ASSERT_TRUE(card.files.count(f));
    TEST_ASSERT_TRUE(card.files[f] == src.files["D"]);
    TEST_ASSERT_TRUE(card.maxWriteLen <= cap);   // written in slices, never whole
    TEST_ASSERT_FALSE(card.files.count(f + ".part"));
    JsonDocument ev = parseLine(card.lines(idx("9", "2026-10-18")).back());
    TEST_ASSERT_EQUAL_STRING("ok", ev["st"] | "");
    TEST_ASSERT_EQUAL_UINT64(sz, ev["bytes"] | 0ull);
  }
}

static void test_over_the_limit_is_not_archived() {
  // Reported over the limit: the row says so up front and nothing is fetched.
  {
    Card card;
    Archive a(card);
    captureOneMedia(a, 1, "BIG", kMaxFileBytes + 1);
    TEST_ASSERT_EQUAL_UINT32(0, a.pending());
    JsonDocument r = parseLine(card.lines(idx("9", "2026-10-18"))[0]);
    TEST_ASSERT_EQUAL_STRING("not archived: too large", r["st"] | "");
  }
  // Size not reported, the stream runs past the limit: stopped, no .part kept.
  {
    Card card;
    Archive a(card);
    TgSource src;
    src.reportTotal = false;
    src.files["BIG"] = std::string((size_t)kMaxFileBytes + 10, 'x');
    captureOneMedia(a, 2, "BIG", 0);
    TEST_ASSERT_TRUE(step(a, src) == Step::Failed);
    TEST_ASSERT_EQUAL_size_t(0, card.countUnder("/mem/tg/9/2026-10-18/2.bin"));
    JsonDocument ev = parseLine(card.lines(idx("9", "2026-10-18")).back());
    TEST_ASSERT_EQUAL_STRING("not archived: too large", ev["st"] | "");
  }
}

static void test_write_failure_mid_way_leaves_part_and_incomplete_row() {
  std::mt19937 rng(11);
  for (int trial = 0; trial < 20; ++trial) {
    Card card;
    Archive a(card);
    TgSource src;
    const size_t sz = 1000 + rng() % 200000;
    const size_t failAt = 1 + rng() % (sz - 1);
    src.files["D"] = fakes::bytes(sz, (uint32_t)trial);
    captureOneMedia(a, 7, "D", sz);
    card.failWrite = [&](const std::string& p, size_t off, size_t len) {
      return fakes::endsWith(p, ".part") && off + len > failAt;
    };
    TEST_ASSERT_TRUE(step(a, src) == Step::Incomplete);
    const std::string part = "/mem/tg/9/2026-10-18/7.bin.part";
    TEST_ASSERT_TRUE(card.files.count(part));
    TEST_ASSERT_TRUE(card.files[part].size() <= failAt);
    TEST_ASSERT_FALSE(card.files.count("/mem/tg/9/2026-10-18/7.bin"));
    JsonDocument ev = parseLine(card.lines(idx("9", "2026-10-18")).back());
    TEST_ASSERT_EQUAL_STRING("incomplete", ev["st"] | "");
    TEST_ASSERT_EQUAL_UINT64(card.files[part].size(), ev["bytes"] | 0ull);
    TEST_ASSERT_EQUAL_UINT32(0, a.pending());   // a failing card is not hammered
    TEST_ASSERT_EQUAL_UINT32(1, a.stats().mediaIncomplete);
  }
}

static void test_budget_stops_and_resumes_where_it_left_off() {
  Card card;
  Archive a(card);
  TgSource src;
  src.files["D"] = fakes::bytes(300000, 5);
  captureOneMedia(a, 8, "D", 300000);
  g_ms = 0;
  // Every read advances the fake clock past the budget: one slice per step.
  int steps = 0;
  Step s;
  do {
    static std::vector<uint8_t> buf(4096);
    s = a.downloadStep(src, buf.data(), buf.size(), 10, [] { return g_ms += 20; }, [] { return false; });
    ++steps;
  } while (s == Step::Partial && steps < 1000);
  TEST_ASSERT_TRUE(s == Step::Done);
  TEST_ASSERT_TRUE(steps > 10);
  TEST_ASSERT_TRUE(src.lastResumeAsk > 0);
  TEST_ASSERT_TRUE(card.files["/mem/tg/9/2026-10-18/8.bin"] == src.files["D"]);
}

static void test_a_transfer_paused_at_the_very_end_completes() {
  Card card;
  Archive a(card);
  TgSource src;
  src.files["D"] = fakes::bytes(4096, 9);
  captureOneMedia(a, 19, "D", 4096);
  static std::vector<uint8_t> buf(4096);
  // One slice carries the whole file and the budget trips before the end is seen.
  src.rng.seed(1);
  Step s = a.downloadStep(src, buf.data(), buf.size(), 0, [] { return g_ms += 20; }, [] { return false; });
  if (s == Step::Partial) s = step(a, src);   // resumes at the full size: nothing left
  TEST_ASSERT_TRUE(s == Step::Done);
  TEST_ASSERT_TRUE(card.files["/mem/tg/9/2026-10-18/19.bin"] == src.files["D"]);
}

static void test_a_server_that_ignores_resume_gets_a_whole_fetch() {
  Card card;
  Archive a(card);
  TgSource src;
  src.supportsResume = false;
  src.files["D"] = fakes::bytes(200000, 6);
  captureOneMedia(a, 9, "D", 200000);
  static std::vector<uint8_t> buf(4096);
  TEST_ASSERT_TRUE(a.downloadStep(src, buf.data(), buf.size(), 10, [] { return g_ms += 20; },
                                  [] { return false; }) == Step::Partial);
  // The resume is ignored: the next step starts over and is not cut by the budget.
  TEST_ASSERT_TRUE(a.downloadStep(src, buf.data(), buf.size(), 10, [] { return g_ms += 20; },
                                  [] { return false; }) == Step::Done);
  TEST_ASSERT_TRUE(card.files["/mem/tg/9/2026-10-18/9.bin"] == src.files["D"]);
}

static void test_source_errors_retry_then_fail_with_a_row() {
  Card card;
  Archive a(card);
  TgSource src;
  src.files["D"] = fakes::bytes(50000, 8);
  captureOneMedia(a, 10, "D", 50000);
  src.failAtByte = 20000;   // cut once; the retry resumes and finishes
  TEST_ASSERT_TRUE(step(a, src) == Step::Retry);
  TEST_ASSERT_TRUE(card.files.count("/mem/tg/9/2026-10-18/10.bin.part"));
  TEST_ASSERT_TRUE(step(a, src) == Step::Done);
  TEST_ASSERT_TRUE(card.files["/mem/tg/9/2026-10-18/10.bin"] == src.files["D"]);
  // A file that keeps failing ends with an explicit row.
  captureOneMedia(a, 11, "E", 10);
  src.failOpens = 100;
  Step s = Step::Retry;
  for (int i = 0; i < 10 && s == Step::Retry; ++i) s = step(a, src);
  TEST_ASSERT_TRUE(s == Step::Failed);
  JsonDocument ev = parseLine(card.lines(idx("9", "2026-10-18")).back());
  TEST_ASSERT_EQUAL_STRING("failed", ev["st"] | "");
  // A file Telegram no longer has fails at once.
  captureOneMedia(a, 12, "GONE", 10);
  src.failOpens = 0;
  src.gone.insert("GONE");
  TEST_ASSERT_TRUE(step(a, src) == Step::Failed);
}

static void test_resume_pass_after_a_reboot() {
  Card card;
  {
    Archive a(card);
    captureOneMedia(a, 30, "R1", 100);
    captureOneMedia(a, 31, "R2", 100);
    TEST_ASSERT_EQUAL_UINT32(2, a.pending());
  }   // reboot before any download
  Archive b(card);
  TEST_ASSERT_EQUAL_UINT32(0, b.pending());
  TEST_ASSERT_EQUAL_INT(2, b.resumePending());
  TgSource src;
  src.files["R1"] = fakes::bytes(100, 1);
  src.files["R2"] = fakes::bytes(100, 2);
  drain(b, src);
  TEST_ASSERT_TRUE(card.files.count("/mem/tg/9/2026-10-18/30.bin"));
  TEST_ASSERT_TRUE(card.files.count("/mem/tg/9/2026-10-18/31.bin"));
  // Finished items are not queued again.
  Archive c(card);
  TEST_ASSERT_EQUAL_INT(0, c.resumePending());
}

static void test_queue_overflow_is_never_a_loss() {
  Card card;
  Archive a(card);
  TgSource src;
  const int n = (int)kMaxJobs + 20;
  for (int i = 0; i < n; ++i) {
    src.files["Q" + std::to_string(i)] = fakes::bytes(10, (uint32_t)i);
    captureOneMedia(a, 2000 + i, "Q" + std::to_string(i), 10);
  }
  TEST_ASSERT_EQUAL_UINT32(kMaxJobs + 1, a.pending());   // the queue, plus the overflow mark
  drain(a, src);
  TEST_ASSERT_EQUAL_UINT32(0, a.pending());
  for (int i = 0; i < n; ++i)
    TEST_ASSERT_TRUE(card.files.count("/mem/tg/9/2026-10-18/" + std::to_string(2000 + i) + ".bin"));
}

// ---- outbound ------------------------------------------------------------------

static void test_outbound_text_is_archived_whole_before_splitting() {
  Card card;
  Archive a(card);
  const std::string longReply(9000, 'r');   // send() splits this into 3 parts later
  TEST_ASSERT_TRUE(a.captureOutText("6098", longReply, "reply", {kOct18, 0}));
  const std::string raw = card.files["/mem/tg/6098/2026-10-18/o1.json"];
  JsonDocument d = parseLine(raw);
  TEST_ASSERT_TRUE(d["out"] | false);
  TEST_ASSERT_EQUAL_INT64(6098, d["chat"]["id"] | 0ll);
  TEST_ASSERT_EQUAL_size_t(9000, std::string(d["text"] | "").size());
  JsonDocument r = parseLine(card.lines(idx("6098", "2026-10-18"))[0]);
  TEST_ASSERT_EQUAL_STRING("out", r["dir"] | "");
  TEST_ASSERT_EQUAL_INT(1, r["s"] | 0);
  TEST_ASSERT_EQUAL_STRING("reply", r["origin"] | "");
  // Pseudo-chats are not Telegram.
  TEST_ASSERT_FALSE(a.captureOutText("web", "x", "reply", {kOct18, 0}));
  TEST_ASSERT_FALSE(a.captureOutText("serial", "x", "reply", {kOct18, 0}));
  TEST_ASSERT_FALSE(a.captureOutText("voice", "x", "reply", {kOct18, 0}));
}

static void test_outbound_day_without_a_clock_uses_server_time() {
  Card card;
  Archive a(card);
  // No clock and nothing heard from Telegram yet: undated, still collision-free.
  TEST_ASSERT_TRUE(a.captureOutText("5", "a", "notice", {0, 1000}));
  TEST_ASSERT_TRUE(card.files.count("/mem/tg/5/undated/o1.json"));
  // After an inbound update the server's date dates the outbound items.
  const std::string p = page({update(1, "5", kOct18, text("hi"))});
  a.captureInboundPage(p.data(), p.size(), {0, 2000});
  TEST_ASSERT_TRUE(a.captureOutText("5", "b", "reply", {0, 5000}));
  TEST_ASSERT_TRUE(card.files.count("/mem/tg/5/2026-10-18/o1.json"));
}

static void test_outbound_media_is_copied() {
  Card card;
  Archive a(card);
  Card lfs;
  lfs.mkdirs("/audio");
  lfs.files["/audio/reply.ogg"] = fakes::bytes(70000, 4);
  OutMedia om{"5", "voice", "/audio/reply.ogg", "", "voice"};
  TEST_ASSERT_TRUE(a.captureOutMedia(om, lfs, {kOct18, 0}));
  TEST_ASSERT_TRUE(card.files["/mem/tg/5/2026-10-18/o1.ogg"] == lfs.files["/audio/reply.ogg"]);
  JsonDocument r = parseLine(card.lines(idx("5", "2026-10-18"))[0]);
  TEST_ASSERT_EQUAL_STRING("voice", r["kind"] | "");
  TEST_ASSERT_EQUAL_STRING("o1.ogg", r["file"] | "");
  TEST_ASSERT_EQUAL_STRING("ok", r["st"] | "");
  // A source that vanished: an incomplete row, never a crash.
  OutMedia gone{"5", "document", "/audio/missing.pdf", "", "files.send"};
  TEST_ASSERT_FALSE(a.captureOutMedia(gone, lfs, {kOct18, 0}));
  JsonDocument r2 = parseLine(card.lines(idx("5", "2026-10-18")).back());
  TEST_ASSERT_EQUAL_STRING("incomplete", r2["st"] | "");
}

// ---- storage pressure ---------------------------------------------------------

static std::vector<DayUsage> randomDays(std::mt19937& rng) {
  std::vector<DayUsage> days;
  const int n = 1 + (int)(rng() % 30);
  for (int i = 0; i < n; ++i) {
    DayUsage u;
    u.chat = (rng() % 2) ? "1" : "2";
    char d[16];
    std::snprintf(d, sizeof d, "2026-%02d-%02d", 1 + (int)(rng() % 12), 1 + (int)(rng() % 28));
    u.day = (rng() % 15 == 0) ? "undated" : d;
    u.bytes = (uint64_t)(rng() % 400) << 20;
    u.stranger = rng() % 3 == 0;
    days.push_back(u);
  }
  return days;
}

// The removal order: every stranger day before any owner day, oldest first within
// each group, never the newest owner day. Returns the bytes the plan frees.
static uint64_t checkOrder(const std::vector<DayUsage>& days, const PressurePlan& p) {
  std::string newestOwner;
  size_t strangers = 0, strangersRemoved = 0;
  for (const auto& u : days) {
    strangers += u.stranger;
    if (!u.stranger && u.day != "undated" && u.day > newestOwner) newestOwner = u.day;
  }
  uint64_t freed = 0;
  std::string lastRemoved;
  bool ownerSeen = false;
  for (size_t i : p.remove) {
    if (!days[i].stranger && !ownerSeen) lastRemoved.clear();
    TEST_ASSERT_FALSE(days[i].stranger && ownerSeen);
    ownerSeen |= !days[i].stranger;
    strangersRemoved += days[i].stranger;
    TEST_ASSERT_TRUE(days[i].stranger || days[i].day != newestOwner);
    const std::string key = days[i].day == "undated" ? "" : days[i].day;
    TEST_ASSERT_TRUE(key >= lastRemoved);
    lastRemoved = key;
    freed += days[i].bytes;
  }
  if (ownerSeen) TEST_ASSERT_EQUAL_size_t(strangers, strangersRemoved);
  return freed;
}

static void test_pressure_plan_property() {
  std::mt19937 rng(21);
  const uint64_t total = 16ull << 30;
  for (int trial = 0; trial < 200; ++trial) {
    const std::vector<DayUsage> days = randomDays(rng);
    const uint64_t freeB = (uint64_t)(rng() % 2000) << 20;
    PressurePlan p = planPressure(days, freeB, total);
    if (freeB >= lowWater(total)) {
      TEST_ASSERT_FALSE(p.triggered);
      continue;
    }
    TEST_ASSERT_TRUE(p.triggered);
    const uint64_t freed = checkOrder(days, p);
    TEST_ASSERT_EQUAL_UINT64(freed, p.freed);
    // It stops as soon as the target is met: dropping the last removal falls short.
    if (!p.remove.empty())
      TEST_ASSERT_TRUE(freeB + freed - days[p.remove.back()].bytes < targetFree(total));
  }
  // Every allowlisted chat keeps its own newest day, even a quiet one.
  std::vector<DayUsage> two = {{"1", "2026-10-01", 1ull << 30, false}, {"1", "2026-10-02", 1ull << 30, false},
                               {"2", "2026-09-01", 1ull << 30, false}, {"2", "2026-09-02", 1ull << 30, false}};
  PressurePlan keep = planPressure(two, 0, total);
  for (size_t i : keep.remove) TEST_ASSERT_TRUE(two[i].day == "2026-10-01" || two[i].day == "2026-09-01");
  // A small card is not always "low": the floor scales down with it.
  TEST_ASSERT_TRUE(lowWater(512ull << 20) < (128ull << 20));
}

static void test_pressure_pass_keeps_newest_and_tells_the_owner_once() {
  Card card;
  Archive a(card);
  for (int d = 1; d <= 5; ++d) {
    char day[16];
    std::snprintf(day, sizeof day, "2026-10-%02d", d);
    card.mkdirs(dayDirPath("5", day));
    card.files[dayDirPath("5", day) + "/1.json"] = std::string(1000, 'x');
  }
  const uint64_t total = 10000ull << 20;
  const uint64_t low = lowWater(total);
  PressureOutcome o = a.pressurePass(low - 1, total);
  TEST_ASSERT_TRUE(o.triggered);
  TEST_ASSERT_TRUE(o.tellOwner);
  TEST_ASSERT_EQUAL_UINT32(4, o.removedDays);   // tiny days: all but the newest go
  TEST_ASSERT_TRUE(card.files.count("/mem/tg/5/2026-10-05/1.json"));
  TEST_ASSERT_EQUAL_STRING("2026-10-05", o.keptFrom.c_str());
  // Still low, but already told this episode.
  PressureOutcome o2 = a.pressurePass(low - 1, total);
  TEST_ASSERT_FALSE(o2.tellOwner);
  // Space recovered: the episode ends; the next one tells again.
  a.pressurePass(targetFree(total), total);
  card.mkdirs(dayDirPath("5", "2026-10-06"));
  card.files[dayDirPath("5", "2026-10-06") + "/2.json"] = "y";
  PressureOutcome o3 = a.pressurePass(low - 1, total);
  TEST_ASSERT_TRUE(o3.tellOwner);
  // A day with a transfer in flight waits for a later pass.
  Card c3;
  Archive d3(c3);
  for (const char* day : {"2026-10-01", "2026-10-02", "2026-10-03"}) c3.mkdirs(dayDirPath("5", day));
  c3.files[dayDirPath("5", "2026-10-01") + "/7.jpg.part"] = "half";
  c3.files[dayDirPath("5", "2026-10-02") + "/8.json"] = "x";
  c3.files[dayDirPath("5", "2026-10-03") + "/9.json"] = "y";
  d3.pressurePass(low - 1, total);
  TEST_ASSERT_TRUE(c3.files.count(dayDirPath("5", "2026-10-01") + "/7.jpg.part"));
  TEST_ASSERT_FALSE(c3.files.count(dayDirPath("5", "2026-10-02") + "/8.json"));
  // Above the low-water mark nothing is touched.
  Card c2;
  Archive b(c2);
  c2.mkdirs(dayDirPath("5", "2026-10-01"));
  c2.files[dayDirPath("5", "2026-10-01") + "/1.json"] = "x";
  TEST_ASSERT_FALSE(b.pressurePass(low, total).triggered);
  TEST_ASSERT_TRUE(c2.files.count(dayDirPath("5", "2026-10-01") + "/1.json"));
}

// ---- retention exemption --------------------------------------------------------

// The episodic retention pass, over the same card holding an archive: it prunes its
// day-streams and unreferenced blobs and deletes nothing under /mem/tg.
struct EpiCard : nimbus::orch::EpiFs {
  Card& c;
  std::vector<std::string> removed;
  explicit EpiCard(Card& card) : c(card) {}
  long append(const std::string& p, const std::string& b) override {
    std::string& f = c.files[p];
    const long off = (long)f.size();
    f += b;
    return off;
  }
  std::string readAll(const std::string& p) const override {
    auto it = c.files.find(p);
    return it == c.files.end() ? "" : it->second;
  }
  std::string readRange(const std::string& p, long off, long len) const override {
    auto it = c.files.find(p);
    if (it == c.files.end() || off < 0 || (size_t)(off + len) > it->second.size()) return "";
    return it->second.substr((size_t)off, (size_t)len);
  }
  long size(const std::string& p) const override {
    auto it = c.files.find(p);
    return it == c.files.end() ? 0 : (long)it->second.size();
  }
  std::vector<std::string> list(const std::string& dir) const override {
    std::vector<std::string> out;
    for (const auto& e : c.list(dir))
      if (!e.dir) out.push_back(e.name);
    return out;
  }
  bool remove(const std::string& p) override {
    removed.push_back(p);
    return c.files.erase(p) > 0;
  }
  bool exists(const std::string& p) const override { return c.files.count(p) > 0; }
};

static void test_retention_pass_deletes_nothing_under_the_archive() {
  Card card;
  Archive a(card);
  TgSource src;
  src.files["P"] = fakes::bytes(500, 1);
  // An archive with old and new days, media and raw records.
  for (int i = 0; i < 40; ++i) {
    const std::string p = page({update(i + 1, "5", kOct18 - 86400LL * (i * 3),
                                       "\"photo\":[{\"file_id\":\"P\",\"width\":9,\"height\":9}]")});
    a.captureInboundPage(p.data(), p.size(), {0, 0});
  }
  drain(a, src);
  a.captureOutText("5", "old reply", "reply", {kOct18 - 86400LL * 100, 0});
  const size_t before = card.countUnder("/mem/tg/");
  // Episodic rows far older than the window, and an unreferenced blob.
  EpiCard epi(card);
  card.mkdirs("/mem/episodic");
  card.mkdirs("/mem/blobs");
  card.files["/mem/blobs/0123456789abcdef.jpg"] = "orphan";
  nimbus::orch::AppendLogEpisodicStore store(epi, "/mem/episodic", 16);
  nimbus::orch::EpisodicMessage m;
  m.id = "m00000001";
  m.sessionId = "5";
  m.tsHours = (uint32_t)((kOct18 - 86400LL * 200) / 3600);
  m.role = "user";
  m.text = "old";
  store.addMessage(m);
  // The pass the device runs (memory::pruneRetention), with no turn in flight.
  const nimbus::orch::RetentionResult res = nimbus::orch::runEpisodicRetention(
      &store, (uint32_t)(kOct18 / 86400), 30, "/mem/blobs", false);
  TEST_ASSERT_TRUE(res.status == nimbus::orch::RetentionStatus::Ran);
  const nimbus::orch::EpiPruneReport& rep = res.report;
  TEST_ASSERT_TRUE(rep.removedDayFiles.size() + rep.removedBlobs.size() > 0);   // it did run
  for (const auto& p : epi.removed) TEST_ASSERT_FALSE_MESSAGE(isArchivePath(p), p.c_str());
  TEST_ASSERT_EQUAL_size_t(before, card.countUnder("/mem/tg/"));
  // Every path the archive wrote is one the retention policy refuses to delete.
  size_t archived = 0;
  for (const auto& f : card.files) {
    if (!isArchivePath(f.first)) continue;
    ++archived;
    TEST_ASSERT_TRUE_MESSAGE(nimbus::orch::isRetentionExempt(f.first), f.first.c_str());
  }
  TEST_ASSERT_EQUAL_size_t(before, archived);
}

// ---- fresh device and an absent card ------------------------------------------------

static void test_fresh_card_without_an_archive_root() {
  Card card;
  Archive a(card);
  TEST_ASSERT_EQUAL_size_t(0, listChats(card).size());
  TEST_ASSERT_EQUAL_size_t(0, listDays(card, "5").size());
  TEST_ASSERT_EQUAL_size_t(0, readRows(card, "5", "2026-10-18", 0, 10).rows.size());
  TEST_ASSERT_EQUAL_size_t(0, newestMedia(card, "", 10).size());
  TEST_ASSERT_EQUAL_INT(0, a.resumePending());
  TEST_ASSERT_FALSE(a.pressurePass(0, 1ull << 34).triggered);
  const std::string p = page({update(1, "5", kOct18, text("first"))});
  TEST_ASSERT_EQUAL_INT(1, a.captureInboundPage(p.data(), p.size(), {0, 0}));
  TEST_ASSERT_TRUE(card.dirs.count("/mem/tg"));
}

static void test_absent_card_never_crashes() {
  Card card;
  card.dead = true;
  Archive a(card);
  TgSource src;
  src.files["P"] = "x";
  const std::string p = page({update(1, "5", kOct18, "\"photo\":[{\"file_id\":\"P\",\"width\":1,\"height\":1}]")});
  TEST_ASSERT_EQUAL_INT(0, a.captureInboundPage(p.data(), p.size(), {0, 0}));
  TEST_ASSERT_FALSE(a.captureOutText("5", "x", "reply", {kOct18, 0}));
  Card lfs;
  OutMedia om{"5", "photo", "/x.jpg", "", "media"};
  TEST_ASSERT_FALSE(a.captureOutMedia(om, lfs, {kOct18, 0}));
  TEST_ASSERT_TRUE(step(a, src) == Step::Idle);
  TEST_ASSERT_EQUAL_INT(0, a.resumePending());
  TEST_ASSERT_TRUE(a.stats().writeErrors > 0);
  TEST_ASSERT_EQUAL_size_t(0, listChats(card).size());
}

// ---- reading -------------------------------------------------------------------

static void test_reading_helpers() {
  Card card;
  Archive a(card);
  TgSource src;
  for (int i = 0; i < 25; ++i) {
    src.files["M" + std::to_string(i)] = fakes::bytes(10, (uint32_t)i);
    const std::string p = page({update(i + 1, (i % 2) ? "1" : "2", kOct18 - 86400LL * (i % 3),
                                       "\"voice\":{\"file_id\":\"M" + std::to_string(i) + "\"}")});
    a.captureInboundPage(p.data(), p.size(), {0, 0});
  }
  drain(a, src);
  const auto chats = listChats(card);
  TEST_ASSERT_EQUAL_size_t(2, chats.size());
  card.mkdirs(dayDirPath("1", "undated"));
  const auto days = listDays(card, "1");
  TEST_ASSERT_EQUAL_size_t(4, days.size());
  TEST_ASSERT_EQUAL_STRING("2026-10-18", days[0].c_str());   // newest first
  TEST_ASSERT_EQUAL_STRING("undated", days[3].c_str());      // and undated last
  RowPage pg = readRows(card, "1", "2026-10-18", 0, 3);
  TEST_ASSERT_EQUAL_size_t(3, pg.rows.size());
  TEST_ASSERT_TRUE(pg.more);
  RowPage pg2 = readRows(card, "1", "2026-10-18", pg.next, 100);
  TEST_ASSERT_FALSE(pg2.more);
  TEST_ASSERT_FALSE(readRows(card, "../x", "2026-10-18", 0, 3).more);
  const auto media = newestMedia(card, "", 5);
  TEST_ASSERT_EQUAL_size_t(5, media.size());
  TEST_ASSERT_EQUAL_STRING("2026-10-18", media[0].day.c_str());
  for (const auto& m : media) TEST_ASSERT_TRUE(isMediaName(m.name));
  TEST_ASSERT_EQUAL_size_t(25, newestMedia(card, "", 100).size());
  TEST_ASSERT_FALSE(isMediaName("index.jsonl"));
  TEST_ASSERT_FALSE(isMediaName("5.json"));
  TEST_ASSERT_FALSE(isMediaName("5.ogg.part"));
  TEST_ASSERT_TRUE(isMediaName("5.ogg"));
  TEST_ASSERT_TRUE(isMediaName("o3.jpg"));
}

// ---- outbound send sites: every path to Telegram passes an archive hook ----------

namespace src {

std::string readFile(const char* path) {
  FILE* f = std::fopen(path, "rb");
  if (!f) return std::string();
  std::string s;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
  std::fclose(f);
  return s;
}

// Code only: comments blanked (string and char literals kept), so a word in a
// comment never counts as a call.
std::string stripComments(const std::string& in) {
  std::string out = in;
  for (size_t i = 0; i < out.size(); ++i) {
    if (out[i] == '"' || out[i] == '\'') {
      const char q = out[i];
      for (++i; i < out.size() && out[i] != q; ++i)
        if (out[i] == '\\') ++i;
    } else if (out.compare(i, 2, "//") == 0) {
      while (i < out.size() && out[i] != '\n') out[i++] = ' ';
    } else if (out.compare(i, 2, "/*") == 0) {
      const size_t e = out.find("*/", i + 2);
      const size_t stop = e == std::string::npos ? out.size() : e + 2;
      for (; i < stop; ++i)
        if (out[i] != '\n') out[i] = ' ';
      --i;
    }
  }
  return out;
}

// [begin, end) of the body of the function whose definition starts with `sig`.
std::pair<size_t, size_t> bodyOf(const std::string& code, const std::string& sig) {
  const size_t at = code.find(sig);
  if (at == std::string::npos) return {0, 0};
  size_t i = code.find('{', at);
  int depth = 0;
  for (size_t j = i; j < code.size(); ++j) {
    const char c = code[j];
    if (c == '"' || c == '\'') {
      for (++j; j < code.size() && code[j] != c; ++j)
        if (code[j] == '\\') ++j;
    } else if (c == '{') {
      ++depth;
    } else if (c == '}' && --depth == 0) {
      return {i, j + 1};
    }
  }
  return {0, 0};
}

std::vector<size_t> findAll(const std::string& code, const std::string& needle) {
  std::vector<size_t> at;
  for (size_t p = code.find(needle); p != std::string::npos; p = code.find(needle, p + 1)) at.push_back(p);
  return at;
}

bool inside(size_t pos, std::pair<size_t, size_t> b) { return pos >= b.first && pos < b.second; }

int countIn(const std::string& code, const std::string& needle, std::pair<size_t, size_t> b) {
  int n = 0;
  for (size_t p : findAll(code, needle)) n += inside(p, b) ? 1 : 0;
  return n;
}

}  // namespace src

static void test_every_outbound_send_site_passes_an_archive_hook() {
  const std::string code = src::stripComments(src::readFile("src/agent/telegram.cpp"));
  TEST_ASSERT_TRUE_MESSAGE(code.size() > 10000, "telegram.cpp not found (run from the repo root)");
  const auto raw = src::bodyOf(code, "static bool doSendMessageRaw(");
  const auto deliver = src::bodyOf(code, "bool doSendMessage(const char* chatId, const char* text) {");
  const auto direct = src::bodyOf(code, "static bool sendNow(const char* chatId, const char* text) {");
  const auto media = src::bodyOf(code, "static void drainMedia() {");
  const auto send = src::bodyOf(code, "bool send(const String& chatId, const String& text, bool block) {");
  const auto poll = src::bodyOf(code, "void pollTask(void*) {");
  const auto page = src::bodyOf(code, "static void fetchOnePage(");
  for (auto b : {raw, deliver, direct, media, send, poll, page}) TEST_ASSERT_TRUE(b.second > b.first);

  // 1. Every Telegram send method on the wire sits in one of the two wire functions.
  const char* methods[] = {"sendMessage", "sendDocument", "sendPhoto", "sendVoice", "sendAudio",
                           "sendVideo", "sendAnimation", "sendSticker", "sendMediaGroup", "sendVideoNote"};
  int wireSites = 0;
  for (const char* m : methods)
    for (size_t p : src::findAll(code, m)) {
      ++wireSites;
      TEST_ASSERT_TRUE_MESSAGE(src::inside(p, raw) || src::inside(p, media), m);
    }
  TEST_ASSERT_EQUAL_INT(5, wireSites);   // sendMessage + the four media methods

  // 2. The wire primitives are called only from their hooked owners.
  for (size_t p : src::findAll(code, "doSendMessageRaw(")) {
    if (p == code.find("doSendMessageRaw(")) continue;   // the definition
    TEST_ASSERT_TRUE_MESSAGE(src::inside(p, deliver), "doSendMessageRaw called outside doSendMessage");
  }
  for (size_t p : src::findAll(code, "httpmp::post("))
    TEST_ASSERT_TRUE_MESSAGE(src::inside(p, media), "httpmp::post called outside drainMedia");
  // doSendMessage (delivery only): the definition, the hooked sendNow, and the
  // reply-queue drain (whose parts send() archived whole before splitting). Nothing else.
  TEST_ASSERT_EQUAL_INT(3, (int)src::findAll(code, "doSendMessage(").size());
  TEST_ASSERT_EQUAL_INT(1, src::countIn(code, "doSendMessage(", direct));
  TEST_ASSERT_EQUAL_INT(1, src::countIn(code, "doSendMessage(", poll));

  // 3. One archive hook per outbound entry point, and no other.
  const int hooks = (int)src::findAll(code, "tgarchive::onOutbound").size();
  TEST_ASSERT_EQUAL_INT(3, hooks);
  TEST_ASSERT_EQUAL_INT(1, src::countIn(code, "tgarchive::onOutboundText(", send));
  TEST_ASSERT_EQUAL_INT(1, src::countIn(code, "tgarchive::onOutboundText(", direct));
  TEST_ASSERT_EQUAL_INT(1, src::countIn(code, "tgarchive::onOutboundMedia(", media));
  // The hook comes before the send in each.
  TEST_ASSERT_TRUE(code.find("tgarchive::onOutboundMedia(", media.first) < code.find("httpmp::post(", media.first));
  TEST_ASSERT_TRUE(code.find("tgarchive::onOutboundText(", direct.first) < code.find("doSendMessage(", direct.first));

  // 4. Inbound: one hook, where the page is read, before the turn path parses it.
  TEST_ASSERT_EQUAL_INT(2, (int)src::findAll(code, "tgarchive::onInboundPage(").size());
  TEST_ASSERT_EQUAL_INT(1, src::countIn(code, "tgarchive::onInboundPage(", page));
  // The other: the TGINJECT page (test builds only), through the same receive path.
  const auto inj = src::bodyOf(code, "static void drainInjectedPage() {");
  TEST_ASSERT_TRUE(inj.second > inj.first);
  TEST_ASSERT_EQUAL_INT(1, src::countIn(code, "tgarchive::onInboundPage(", inj));
  TEST_ASSERT_TRUE(code.find("tgarchive::onInboundPage(", page.first) < code.find("parsePageBody(", page.first));

  // 5. No other source file talks to the Telegram send API.
  const char* others[] = {"src/main.cpp", "src/agent/orchestrator.cpp", "src/agent/files_subsystem.cpp",
                          "src/agent/memory_subsystem.cpp", "src/agent/adapters/http_multipart.cpp",
                          "src/agent/provider_verify.cpp", "src/net/webui.cpp", "src/test_console.cpp"};
  for (const char* f : others) {
    const std::string oc = src::stripComments(src::readFile(f));
    TEST_ASSERT_TRUE_MESSAGE(!oc.empty(), f);
    for (const char* m : methods) TEST_ASSERT_TRUE_MESSAGE(oc.find(m) == std::string::npos, f);
  }
}

namespace src {

// Offsets of `code` that lie inside an #ifdef NIMBUS_TEST block (not its #else).
std::vector<std::pair<size_t, size_t>> testOnlyRanges(const std::string& code) {
  std::vector<std::pair<size_t, size_t>> out;
  std::vector<int> stack;   // 1 = NIMBUS_TEST branch, 0 = anything else
  size_t start = 0;
  for (size_t at = 0; at < code.size();) {
    const size_t eol = code.find('\n', at) == std::string::npos ? code.size() : code.find('\n', at);
    std::string line = code.substr(at, eol - at);
    line.erase(0, line.find_first_not_of(" \t"));
    const bool wasTest = std::find(stack.begin(), stack.end(), 1) != stack.end();
    if (line.rfind("#ifdef NIMBUS_TEST", 0) == 0 || line.rfind("#if defined(NIMBUS_TEST)", 0) == 0)
      stack.push_back(1);
    else if (line.rfind("#if", 0) == 0) stack.push_back(0);
    else if (line.rfind("#else", 0) == 0 && !stack.empty()) stack.back() = stack.back() == 1 ? 0 : 0;
    else if (line.rfind("#endif", 0) == 0 && !stack.empty()) stack.pop_back();
    const bool isTest = std::find(stack.begin(), stack.end(), 1) != stack.end();
    if (!wasTest && isTest) start = eol;
    if (wasTest && !isTest) out.emplace_back(start, at);
    at = eol + 1;
  }
  return out;
}

}  // namespace src

// TGINJECT media is test-build only: every mention of its pieces in the firmware
// sources sits inside #ifdef NIMBUS_TEST (or #if defined(NIMBUS_TEST)).
static void test_tginject_media_is_test_build_only() {
  const char* files[] = {"src/agent/telegram.cpp", "src/agent/telegram.h", "src/agent/tg_archive.cpp",
                         "src/agent/tg_archive.h"};
  const char* names[] = {"injectRawPage", "drainInjectedPage", "copyLocalInject", "openLocal",
                         "localInjectPath", "s_injBody", "localFile_"};
  int seen = 0;
  for (const char* f : files) {
    const std::string code = src::stripComments(src::readFile(f));
    TEST_ASSERT_TRUE_MESSAGE(!code.empty(), f);
    const auto ranges = src::testOnlyRanges(code);
    for (const char* n : names)
      for (size_t p : src::findAll(code, n)) {
        ++seen;
        bool guarded = false;
        for (const auto& r : ranges) guarded |= p >= r.first && p < r.second;
        TEST_ASSERT_TRUE_MESSAGE(guarded, (std::string(f) + ": " + n).c_str());
      }
  }
  TEST_ASSERT_TRUE(seen >= 10);
}

// The factory sweep removes the whole durable store root on the card, and the
// archive root is inside it; the final mop removes it again. Retention stays out:
// the device pass and its battery-day sweep never name the archive or remove a
// tree, and the retention exemption names the same root as the archive.
static void test_factory_sweep_covers_the_archive_root() {
  const std::string code = src::stripComments(src::readFile("src/agent/memory_subsystem.cpp"));
  const auto erase = src::bodyOf(code, "bool eraseDurableStore() {");
  const auto mop = src::bodyOf(code, "bool finalSweepPass() {");
  TEST_ASSERT_TRUE(erase.second > erase.first && mop.second > mop.first);
  TEST_ASSERT_TRUE(src::countIn(code, "g_haveSd ? \"/mem\" : \"/data\"", erase) == 1);
  TEST_ASSERT_TRUE(src::countIn(code, "rmTree(*g_fs, root)", erase) == 1);
  TEST_ASSERT_TRUE(src::countIn(code, "rmTreeIfPresent(*g_fs, root)", mop) == 1);
  TEST_ASSERT_EQUAL_INT(0, std::string(kRoot).compare(0, 5, "/mem/"));
  // Anchored on the name and first parameter only (the pass now returns a status).
  for (const char* sig : {"pruneRetention(int retentionDays", "pruneBatteryDays(uint32_t cutoffDay"}) {
    const auto body = src::bodyOf(code, sig);
    TEST_ASSERT_TRUE_MESSAGE(body.second > body.first, sig);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, src::countIn(code, "/mem/tg", body), sig);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, src::countIn(code, "rmTree", body), sig);
  }
  TEST_ASSERT_EQUAL_STRING(kRoot, nimbus::orch::kTgArchiveRoot);
  TEST_ASSERT_TRUE(nimbus::orch::isRetentionExempt(kRoot));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_archive_path_predicate);
  RUN_TEST(test_names_and_days);
  RUN_TEST(test_local_inject_path);
  RUN_TEST(test_split_result_spans);
  RUN_TEST(test_meta_largest_photo_and_album_fields);
  RUN_TEST(test_media_ext_never_shadows_a_record);
  RUN_TEST(test_every_parser_kind_has_an_archive_decision);
  RUN_TEST(test_every_content_type_is_archived_or_explicitly_not);
  RUN_TEST(test_inbound_writes_raw_update_and_index_row);
  RUN_TEST(test_replay_of_the_same_page_adds_nothing);
  RUN_TEST(test_replay_restores_an_index_line_lost_to_a_crash);
  RUN_TEST(test_repair_uses_this_updates_trust);
  RUN_TEST(test_secrets_never_reach_the_card);
  RUN_TEST(test_control_bytes_keep_the_index_valid);
  RUN_TEST(test_album_items_share_a_group_and_keep_their_own_names);
  RUN_TEST(test_strangers_are_text_only_and_capped);
  RUN_TEST(test_stranger_rotation_holds_the_global_budget);
  RUN_TEST(test_stranger_table_never_evicts);
  RUN_TEST(test_stranger_budget_survives_a_restart);
  RUN_TEST(test_stranger_overflow_replay_is_not_charged_twice);
  RUN_TEST(test_naming_property_no_collisions);
  RUN_TEST(test_streamed_download_any_size_up_to_the_limit);
  RUN_TEST(test_over_the_limit_is_not_archived);
  RUN_TEST(test_write_failure_mid_way_leaves_part_and_incomplete_row);
  RUN_TEST(test_budget_stops_and_resumes_where_it_left_off);
  RUN_TEST(test_a_transfer_paused_at_the_very_end_completes);
  RUN_TEST(test_a_server_that_ignores_resume_gets_a_whole_fetch);
  RUN_TEST(test_source_errors_retry_then_fail_with_a_row);
  RUN_TEST(test_resume_pass_after_a_reboot);
  RUN_TEST(test_queue_overflow_is_never_a_loss);
  RUN_TEST(test_outbound_text_is_archived_whole_before_splitting);
  RUN_TEST(test_outbound_day_without_a_clock_uses_server_time);
  RUN_TEST(test_outbound_media_is_copied);
  RUN_TEST(test_pressure_plan_property);
  RUN_TEST(test_pressure_pass_keeps_newest_and_tells_the_owner_once);
  RUN_TEST(test_retention_pass_deletes_nothing_under_the_archive);
  RUN_TEST(test_fresh_card_without_an_archive_root);
  RUN_TEST(test_absent_card_never_crashes);
  RUN_TEST(test_reading_helpers);
  RUN_TEST(test_every_outbound_send_site_passes_an_archive_hook);
  RUN_TEST(test_factory_sweep_covers_the_archive_root);
  RUN_TEST(test_tginject_media_is_test_build_only);
  return UNITY_END();
}
