// test_engine2_host - the owner chat on engine 2 on a Virtual Nimbus (lane E2-HOST), offline:
//   (a) the transcript store (hosted_tx.h): paths outside /mem/tx/ are refused, bytes round-trip,
//       a missing folder lists as empty;
//   (b) a scripted Anthropic head through Engine2Host with the real Anthropic wire: a file write
//       and a note through the tools, the replies leave through the host's deliver to the turn's
//       chat, and a host rebuilt over the same data directory carries the conversation (restart);
//   (c) the startup switch's head rule: anything but an Anthropic head with a claude- model is
//       refused in one plain line, before any request;
//   (d) the storage rule of ruling 112 on the shared file store (hosted_file_tools.h): an append
//       whose index is behind lands at the size on disk, keeps every stored byte, and the index
//       catches up; a write to a path holding bytes the index does not know is refused and the
//       bytes stay; a name ending in .tmp and the chat folders are refused in one plain line;
//   (e) /compact on engine 2 queues the driver once.
#include <sys/stat.h>

#include <string>
#include <vector>

#include "../../test/support/scripted_provider/scripted_provider.h"
#include "engine2_host.h"
#include "test_util.h"

namespace sp = harness_test::scripted;
namespace tx = nimbus::orch::tx;

namespace {

struct Out {
  std::string chat, text;
};

struct Host {
  std::string dir;
  nimbusd::PosixFiles files;
  sp::ScriptedProvider model{sp::Dialect::Anthropic};
  std::vector<Out> sent;
  std::unique_ptr<nimbusd::Engine2Host> host;
  std::string headSlug = "anthropic", modelId = "claude-haiku-5-5";

  explicit Host(const std::string& d) : dir(d), files(d + "/mem/files") {}

  std::string start() {
    nimbusd::Engine2Options o;
    o.dataDir = dir;
    o.files = &files;
    o.head = [this] { return headSlug; };
    o.model = [this] { return modelId; };
    o.summarizerModel = [] { return std::string("claude-haiku-5-5"); };
    o.key = [] { return std::string("sk-fake-e2host"); };
    o.utcOffsetMin = [](uint32_t) { return 0; };
    o.dayStartMin = [] { return uint16_t(240); };
    o.timeZone = "Europe/London";
    o.deliver = [this](const std::string& chat, const std::string& text) { sent.push_back({chat, text}); };
    o.transport = &model;
    host.reset(new nimbusd::Engine2Host(o));
    return host->start();
  }
};

std::string slurp(const std::string& path) {
  std::string s;
  nimbusd::fsutil::readFile(path, s);
  return s;
}

void caseTxStore(ndtest::Ctx& c) {
  std::printf("  (a) the transcript store\n");
  const std::string dir = ndtest::scratchDir("e2tx");
  nimbusd::PosixTxFiles f(dir);
  std::vector<std::string> names;
  c.ok(f.list("/mem/tx/owner", names) && names.empty(), "a missing folder lists as empty");
  c.ok(f.append("/mem/tx/owner/seg-000001.jsonl", "ab", 2) && f.append("/mem/tx/owner/seg-000001.jsonl", "c\n", 2),
       "appends land");
  size_t n = 0;
  c.ok(f.size("/mem/tx/owner/seg-000001.jsonl", n) && n == 4, "size is the bytes on disk");
  char buf[8] = {0};
  size_t got = 0;
  c.ok(f.readAt("/mem/tx/owner/seg-000001.jsonl", 1, buf, 8, got) && got == 3 && std::string(buf, 3) == "bc\n",
       "readAt reads from the offset to the end");
  c.ok(f.writeWhole("/mem/tx/owner/head.json", "{}\n") && slurp(dir + "/mem/tx/owner/head.json") == "{}\n",
       "a whole write lands under <data>/mem/tx/");
  c.ok(f.list("/mem/tx/owner", names) && names.size() == 2, "the folder lists its two files");
  c.ok(!f.append("/mem/fx/x.ring", "a", 1) && !f.writeWhole("/mem/files/.index", "x"), "paths outside /mem/tx/ are refused");
  c.ok(!f.writeWhole("/mem/tx/owner/../../files/x", "x") && f.diskPath("/mem/tx/a/../b").empty(), "a path with /.. is refused");
  ndtest::rmTree(dir);
}

void caseScriptedTurns(ndtest::Ctx& c) {
  std::printf("  (b) scripted turns through the host, then a restart\n");
  const std::string dir = ndtest::scratchDir("e2turn");
  {
    Host h(dir);
    h.model.push(sp::call("write_file", R"({"path":"e2/list.txt","content":"apples\n"})"));
    h.model.push(sp::say("Created e2/list.txt."));
    h.model.push(sp::call("note", R"({"op":"set","key":"bike","value":"The bike is Orca.","pin":true})"));
    h.model.push(sp::say("Noted."));
    c.eq(h.start(), "", "the host starts on a Claude head");
    const nimbusd::PlainTurn t1 = h.host->say("owner", "Create e2/list.txt with apples", "web");
    c.ok(t1.ending == nimbusd::PlainEnding::Reply && t1.stated == "Created e2/list.txt.", "turn 1 replies");
    c.eq(slurp(dir + "/mem/files/e2/list.txt"), "apples\n", "the file is on the shared store");
    c.ok(h.files.has("e2/list.txt"), "...and in its one index");
    const nimbusd::PlainTurn t2 = h.host->say("12345", "Note my bike is Orca", "tg");
    c.ok(t2.ending == nimbusd::PlainEnding::Reply, "turn 2 replies");
    c.ok(slurp(dir + "/mem/notes/owner.md").find("Orca") != std::string::npos, "the note is on the volume");
    c.ok(h.sent.size() == 2 && h.sent[0].chat == "owner" && h.sent[1].chat == "12345",
         "each reply goes to the chat its turn came from");
    std::string why;
    c.ok(h.model.verify(why), "every scripted step was asked for: " + why);
    struct stat st;
    c.ok(::stat((dir + "/mem/tx/owner/head.json").c_str(), &st) == 0, "the transcript is under <data>/mem/tx/owner");
  }
  {
    Host h(dir);
    h.model.push(sp::say("Your bike is Orca."));
    c.eq(h.start(), "", "a new host boots over the same data directory");
    const nimbusd::PlainTurn t = h.host->say("owner", "What is my bike called?", "web");
    c.ok(t.ending == nimbusd::PlainEnding::Reply, "the turn after the restart replies");
    const std::string& body = h.model.last().body;
    c.ok(body.find("Create e2/list.txt with apples") != std::string::npos, "the request carries the earlier owner text");
    c.ok(body.find("The bike is Orca.") != std::string::npos, "...and the pinned note");
  }
  ndtest::rmTree(dir);
}

void caseHeadRule(ndtest::Ctx& c) {
  std::printf("  (c) Claude heads only\n");
  const std::string dir = ndtest::scratchDir("e2head");
  for (const auto& hm : std::vector<std::pair<std::string, std::string>>{
           {"openai", "gpt-5.6"}, {"mistral", "mistral-large-latest"}, {"anthropic", "gpt-5.6"}, {"", ""}}) {
    Host h(dir);
    h.headSlug = hm.first;
    h.modelId = hm.second;
    const std::string why = h.start();
    c.ok(why.rfind("engine 2 runs on Claude heads only", 0) == 0 && why.find('\n') == std::string::npos,
         "refused in one line: " + (hm.first.empty() ? std::string("no head") : hm.first + " " + hm.second));
    c.ok(h.model.requests().empty() && !h.host->running(), "...before any request, and nothing runs");
  }
  c.ok(nimbusd::engine2HeadOk("anthropic", "claude-sonnet-5-5") && !nimbusd::engine2HeadOk("cumulo", "claude-sonnet-5-5"),
       "the rule: provider anthropic and a claude- model");
  ndtest::rmTree(dir);
}

void caseStorageRule(ndtest::Ctx& c) {
  std::printf("  (d) the storage rule on the shared store\n");
  const std::string dir = ndtest::scratchDir("e2store");
  Host h(dir);
  h.model.push(sp::call("append_file", R"({"path":"log/a.csv","text":"row3\n"})"));
  h.model.push(sp::say("Added."));
  h.model.push(sp::call("write_file", R"({"path":"log/orphan.txt","content":"new\n"})"));
  h.model.push(sp::say("Could not."));
  h.model.push(sp::calls({{"write_file", R"({"path":"log/x.txt.tmp","content":"a\n"})", ""},
                          {"append_file", R"({"path":"chat/plan.md","text":"a\n"})", ""}}));
  h.model.push(sp::say("Refused."));
  c.eq(h.start(), "", "the host starts");
  std::string err;
  c.ok(h.files.put("log", "a.csv", "row1\n", err), "engine 1's store holds log/a.csv (5 B indexed)");
  // Another writer's bytes whose index rewrite did not land: the index says 5 B, the disk 10 B.
  {
    FILE* f = std::fopen((dir + "/mem/files/log/a.csv").c_str(), "ab");
    std::fputs("row2\n", f);
    std::fclose(f);
  }
  (void)h.host->say("owner", "add row3", "web");
  c.eq(slurp(dir + "/mem/files/log/a.csv"), "row1\nrow2\nrow3\n", "the append landed at the size on disk: no byte cut");
  const nimbus::orch::FileEntry* e = h.files.store().find("log", "a.csv");
  c.ok(e && e->bytes == 15, "the index caught up to the size on disk");
  // A file on disk the index does not know: never overwritten.
  nimbusd::fsutil::writeFileAtomic(dir + "/mem/files/log/orphan.txt", "kept\n");
  (void)h.host->say("owner", "write orphan", "web");
  c.eq(slurp(dir + "/mem/files/log/orphan.txt"), "kept\n", "bytes the index does not know are not overwritten");
  c.ok(!h.files.has("log/orphan.txt"), "...and not taken into the index under a new write");
  (void)h.host->say("owner", "write reserved names", "web");
  const std::string& body = h.model.last().body;
  c.ok(body.find("kept for this instance's own temporary files") != std::string::npos, "a reserved name is refused in one line");
  struct stat st;
  c.ok(::stat((dir + "/mem/files/log/x.txt.tmp").c_str(), &st) != 0 && ::stat((dir + "/mem/files/chat/plan.md").c_str(), &st) != 0,
       "...and nothing was written for it");
  c.ok(nimbusd::HostedFileTools::reservedPath("a/b.tmp") && nimbusd::HostedFileTools::reservedPath("chat-2/x") &&
           !nimbusd::HostedFileTools::reservedPath("chats/x") && !nimbusd::HostedFileTools::reservedPath("a/tmp"),
       "the reserved-path rule");
  std::string why;
  c.ok(h.model.verify(why), "every scripted step was asked for: " + why);
  ndtest::rmTree(dir);
}

void caseCompactCommand(ndtest::Ctx& c) {
  std::printf("  (e) /compact queues the driver once\n");
  const std::string dir = ndtest::scratchDir("e2compact");
  Host h(dir);
  c.eq(h.start(), "", "the host starts");
  c.ok(h.host->requestCompaction(), "the first /compact is queued");
  c.ok(!h.host->requestCompaction(), "a second while one is queued is not");
  ndtest::rmTree(dir);
}

}  // namespace

int main() {
  ndtest::Ctx c;
  std::printf("test_engine2_host\n");
  caseTxStore(c);
  caseScriptedTurns(c);
  caseHeadRule(c);
  caseStorageRule(c);
  caseCompactCommand(c);
  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  return c.failures ? 1 : 0;
}
