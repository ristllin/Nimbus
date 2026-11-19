// test_hosted_files - CUM-543: the Files panel and the not-yet-hosted surfaces
// answer honestly.
//
//   (a) an upload from the page lands on the instance disk, lists, downloads byte
//       for byte (binary-safe, served as octet-stream) and is readable by the
//       assistant's own files.read tool - it used to answer {"ok":true} and store
//       nothing (list count 0);
//   (b) delete and delete-project work, and a missing file is a 404;
//   (b2) the files.delete TOOL removes a file (bytes + index), refuses a missing
//       file and a traversal name, and refuses a non-owner;
//   (c) a traversal name is refused by the same gate the tools use;
//   (d) routines, skills, wake-ups and downloads refuse a write by name (the page
//       printed "Created" for a routine that never existed), and still answer an
//       honest empty GET;
//   (e) the store has no count cap (lane FSCAP: 300+ files store fine); an upload
//       a zero-byte quota refuses leaves no bytes on the disk, so the volume never
//       holds a file the panel cannot show or delete;
//   (h) lane FSCAP: the byte quota evicts the oldest regenerable copies first and
//       never an owner artifact;
//   (i) lane FSCAP: per-call listings are bounded (web limit/offset, tool paging);
//   (f) files.read refuses a binary file in words instead of handing its bytes to the
//       provider request.
// Offline: no provider traffic at all.
#include <sys/stat.h>

#include <cstdio>
#include <ctime>
#include <string>

#include "engine_thread.h"
#include "nimbus/orch/turn_effects.h"
#include "reply_buffer.h"
#include "rig.h"
#include "test_util.h"
#include "web_api.h"

using namespace nimbusd;

static bool has(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

static NimbusdRig::Options opts(const std::string& tag) {
  NimbusdRig::Options o;
  o.dataDir = ndtest::scratchDir(tag) + "/data";
  ndtest::rmTree(ndtest::scratchDir(tag));
  o.embeddings = false;
  o.embedDims = 64;
  return o;
}

struct Web {
  explicit Web(NimbusdRig& rig) : eng(&rig), api(&rig, &eng, &replies) { eng.start(); }
  ~Web() { eng.stop(); }
  ApiResp call(const std::string& m, const std::string& path, const std::string& body = "") {
    ApiResp out;
    api.handle(m, path, body, out);
    return out;
  }
  EngineThread eng;
  ReplyBuffer replies;
  WebApi api;
};

static std::string multipart(const std::string& filename, const std::string& bytes) {
  const std::string b = "----NimbusTestBoundary7";
  return "--" + b + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"" + filename +
         "\"\r\nContent-Type: application/octet-stream\r\n\r\n" + bytes + "\r\n--" + b + "--\r\n";
}

static void testUploadListDownload(ndtest::Ctx& c) {
  std::printf("  -- (a) upload, list, download, and the assistant can read it --\n");
  Config cfg;
  NimbusdRig rig(cfg, opts("hf-a"));
  const std::string bin = std::string("line one\r\n--not-a-boundary\r\n") + std::string(1, '\0') + "tail";
  {
    Web web(rig);
    const ApiResp up = web.call("POST", "/api/files/upload?project=chat&name=pots-notes.md",
                                multipart("pots-notes.md", "# Pots\n- launch week 6\n"));
    c.eqi(up.status, 200, "the upload answers 200");
    const ApiResp bin1 = web.call("POST", "/api/files/upload?project=chat&name=blob.bin", multipart("blob.bin", bin));
    c.eqi(bin1.status, 200, "a binary upload answers 200");
    const ApiResp list = web.call("GET", "/api/files/list");
    c.ok(has(list.body, "\"count\":2"), "the list counts both files");
    c.ok(has(list.body, "\"name\":\"pots-notes.md\""), "the list names the upload");
    const ApiResp dl = web.call("GET", "/api/files/dl?project=chat&name=blob.bin");
    c.eqi(dl.status, 200, "the download answers 200");
    c.ok(dl.body == bin, "the download is byte for byte (binary-safe)");
    c.eq(dl.ctype, "application/octet-stream", "served as octet-stream (never rendered on the page origin)");
  }
  c.ok(has(rig.callTool("files.read", R"({"project":"chat","name":"pots-notes.md"})"), "launch week 6"),
       "the assistant's files.read sees the uploaded file");
}

static void testRemove(ndtest::Ctx& c) {
  std::printf("  -- (b) delete and delete-project --\n");
  Config cfg;
  NimbusdRig rig(cfg, opts("hf-b"));
  Web web(rig);
  web.call("POST", "/api/files/upload?project=p1&name=a.txt", multipart("a.txt", "A"));
  web.call("POST", "/api/files/upload?project=p1&name=b.txt", multipart("b.txt", "B"));
  web.call("POST", "/api/files/upload?project=p2&name=c.txt", multipart("c.txt", "C"));
  c.eqi(web.call("POST", "/api/files/rm", "project=p2&name=c.txt").status, 200, "delete answers 200");
  c.eqi(web.call("POST", "/api/files/rm", "project=p2&name=c.txt").status, 404, "a missing file is a 404");
  c.eqi(web.call("POST", "/api/files/rmproject", "project=p1").status, 400, "delete-project needs the confirm phrase");
  const ApiResp rp = web.call("POST", "/api/files/rmproject", "project=p1&confirm=DELETE");
  c.ok(rp.status == 200 && has(rp.body, "\"removed\":2"), "delete-project removes the project's files");
  c.ok(has(web.call("GET", "/api/files/list").body, "\"count\":0"), "the store is empty");
}

static void testTraversalRefused(ndtest::Ctx& c) {
  std::printf("  -- (c) a traversal name is refused --\n");
  Config cfg;
  NimbusdRig rig(cfg, opts("hf-c"));
  Web web(rig);
  const ApiResp r = web.call("POST", "/api/files/upload?project=..&name=passwd", multipart("passwd", "x"));
  c.eqi(r.status, 400, "a '..' project is refused");
  c.ok(has(web.call("GET", "/api/files/list").body, "\"count\":0"), "nothing was stored");
}

// RB-REC1 (owner ruling 41): the hosted files.append is the device's pure append, the same
// for a food log, an expenses log and a training log: the rows go on the end as sent, nothing
// stored is rewritten, an older keyed call is one error naming its arguments and writes nothing,
// a row of another width is said in the reply, and the tail's day total adds up the day's rows.
static void testThreeTablesAppendAlike(ndtest::Ctx& c) {
  std::printf("  -- three tables append alike, the same receipt --\n");
  struct Table {
    const char *project, *name, *header, *row, *col, *counted;
  };
  const Table tables[] = {
      {"food", "food-log.csv", "date,time,item,qty,kcal", ",12:40,soup,1,300", "kcal", "600 counted from 2 rows"},
      {"money", "expenses.csv", "date,time,item,qty,amount", ",12:40,train,1,24.5", "amount", "49 counted from 2 rows"},
      {"gym", "training.csv", "date,time,item,qty,kg", ",12:40,squat,5,80", "kg", "160 counted from 2 rows"},
  };
  for (const Table& t : tables) {
    const std::string dir = ndtest::scratchDir(std::string("hf-tbl-") + t.project);
    ndtest::rmTree(dir);
    mkdir(dir.c_str(), 0755);
    PosixFiles files(dir);
    nimbus::orch::ToolRegistry reg;
    files.registerTools(reg);
    nimbus::orch::Principal owner = nimbus::orch::principalForRole("1", nimbus::orch::Role::Admin);
    owner.owner = true;
    const auto call = [&](const char* name, const std::string& json) {
      JsonDocument d;
      deserializeJson(d, json);
      return reg.dispatch(name, d.as<JsonObjectConst>(), owner);
    };
    const std::string today = nimbus::orch::logDayOf(std::time(nullptr), true).date;
    const std::string row = today + t.row;
    const std::string where = std::string("{\"project\":\"") + t.project + "\",\"name\":\"" + t.name + "\"";
    const auto first = call("files.append", where + ",\"text\":\"" + t.header + "\\n" + row + "\"}");
    c.ok(first.success && has(first.output, "appended 2 rows"), std::string("the first append lands: ") + t.name);
    // An older keyed call: one error naming its arguments, nothing written.
    const auto keyed = call("files.append", where + ",\"key\":\"item\",\"sum\":\"" + t.col + "\",\"text\":\"" + row + "\"}");
    c.ok(!keyed.success && has(keyed.error, "key, sum are not an argument of it") && !has(keyed.error, "not read"),
         std::string("a keyed call is refused by name: ") + keyed.error);
    const auto again = call("files.append", where + ",\"text\":\"" + row + "\"}");
    c.ok(again.success && has(again.output, "appended 1 row") && !has(again.output, "field count"),
         std::string("a plain append lands: ") + again.output);
    const auto narrow = call("files.append", where + ",\"text\":\"" + today + ",x\"}");
    c.ok(narrow.success && has(narrow.output, "1 row has a field count other than the header's 5"),
         std::string("a narrow row lands and is said: ") + narrow.output);
    const std::string body = files.get(std::string(t.project) + "/" + t.name);
    c.ok(body == std::string(t.header) + "\n" + row + "\n" + row + "\n" + today + ",x\n",
         std::string("byte for byte, the rows as sent: ") + t.name);
    const auto tail = call("files.read", where + ",\"tail\":5,\"sum\":\"" + t.col + "\"}");
    c.ok(has(tail.output, "not known") && has(tail.output, t.counted),
         std::string("the day total adds up both rows and says the narrow one is unread: ") + tail.output);
  }
}

// files.delete: the chat can delete a stored file (the privacy persona asks for
// exactly this), the bytes and the index entry both go, and a non-owner is refused.
static void testFilesDeleteTool(ndtest::Ctx& c) {
  std::printf("  -- (b2) files.delete removes the file, the bytes, and the index entry --\n");
  const std::string dir = ndtest::scratchDir("hf-del");
  ndtest::rmTree(dir);
  mkdir(dir.c_str(), 0755);
  PosixFiles files(dir);
  nimbus::orch::ToolRegistry reg;
  files.registerTools(reg);
  nimbus::orch::Principal owner = nimbus::orch::principalForRole("1", nimbus::orch::Role::Admin);
  owner.owner = true;
  nimbus::orch::Principal member = nimbus::orch::principalForRole("2", nimbus::orch::Role::User);
  const auto call = [&](const char* name, const std::string& json, const nimbus::orch::Principal& who) {
    JsonDocument d;
    deserializeJson(d, json);
    return reg.dispatch(name, d.as<JsonObjectConst>(), who);
  };
  c.ok(call("artifact.save", R"({"project":"personal","name":"notes.txt","text":"secret"})", owner).success,
       "the file is saved first");
  c.ok(files.has("personal/notes.txt"), "the bytes are on the disk");
  const auto gone = call("files.delete", R"({"project":"personal","name":"notes.txt"})", owner);
  c.ok(gone.success && has(gone.output, "deleted personal/notes.txt"), "files.delete answers deleted");
  c.ok(!files.has("personal/notes.txt"), "the index entry is gone");
  struct stat st;
  c.ok(::stat((dir + "/personal/notes.txt").c_str(), &st) != 0, "the bytes are gone from the disk");
  const auto again = call("files.delete", R"({"project":"personal","name":"notes.txt"})", owner);
  c.ok(!again.success && has(again.error, "no such file"), "deleting a missing file says so");
  const auto trav = call("files.delete", R"({"project":"..","name":"passwd"})", owner);
  c.ok(!trav.success && has(trav.error, "invalid project or name"), "a traversal name is refused");
  c.ok(call("artifact.save", R"({"project":"personal","name":"notes.txt","text":"secret"})", owner).success,
       "saved again for the member check");
  const auto mem = call("files.delete", R"({"project":"personal","name":"notes.txt"})", member);
  c.ok(!mem.success && has(mem.error, "owner"), "a non-owner is refused");
  c.ok(files.has("personal/notes.txt"), "the member's refusal deleted nothing");
}

static void testNotYetHosted(ndtest::Ctx& c) {
  std::printf("  -- (d) a routine needs a schedule, and downloads refuse writes by name --\n");
  Config cfg;
  NimbusdRig rig(cfg, opts("hf-d"));
  Web web(rig);
  const ApiResp loop = web.call("POST", "/api/loops", "action=create&name=weekday+brief&prompt=hi");
  c.eqi(loop.status, 400, "a routine with no schedule is refused");
  c.ok(has(loop.body, "\"ok\":false"), "and the page reads why (x.error)");
  c.ok(web.call("GET", "/api/loops").body == "[]", "a routine with no schedule was not stored");
  const ApiResp fetchq = web.call("POST", "/api/fetchq", "id=x");
  c.ok(fetchq.status == 400 && has(fetchq.body, "Not available on a hosted Nimbus yet"), "/api/fetchq refuses a write");
}

// The text of a tools/call answer (rig.callTool returns the whole JSON-RPC envelope).
static std::string toolText(const std::string& rpc) {
  JsonDocument d;
  deserializeJson(d, rpc);
  return std::string(d["result"]["content"][0]["text"] | "");
}

static void testRefusedUploadLeavesNoFile(ndtest::Ctx& c) {
  std::printf("  -- (e) no count cap: the 257th file stores; a byte-quota refusal leaves no bytes --\n");
  Config cfg;
  const NimbusdRig::Options o = opts("hf-e");
  NimbusdRig rig(cfg, o);
  std::string err;
  bool all = true;
  for (int i = 0; i < 300; i++) all = rig.files().put("bulk", "f" + std::to_string(i) + ".txt", "x", err) && all;
  c.ok(all, "lane FSCAP: the store takes 300 files - the count cap is gone");
  Web web(rig);
  const ApiResp r = web.call("POST", "/api/files/upload?project=chat&name=new.bin", multipart("new.bin", "orphan?"));
  c.eqi(r.status, 200, "the 301st file is accepted too (the byte quota is the only space limit)");
  c.ok(has(web.call("GET", "/api/files/list").body, "\"count\":301"), "the list counts 301");
  const ApiResp over = web.call("POST", "/api/files/upload?project=bulk&name=f3.txt", multipart("f3.txt", "new bytes"));
  c.eqi(over.status, 200, "replacing an existing file is not a new entry, so it still saves");
  c.ok(web.call("GET", "/api/files/dl?project=bulk&name=f3.txt").body == "new bytes", "with the new bytes");
  // A store whose byte quota is zero takes nothing, and the refusal leaves no
  // bytes on the volume (the index is what the panel lists and deletes by).
  nimbus::orch::FileStore::Limits zero;
  zero.maxTotalBytes = 0;
  rig.files().setLimits(zero);
  const ApiResp full = web.call("POST", "/api/files/upload?project=chat&name=full.bin", multipart("full.bin", "no room"));
  c.eqi(full.status, 400, "a zero-quota store refuses the upload");
  struct stat st;
  c.ok(::stat((o.dataDir + "/mem/files/chat/full.bin").c_str(), &st) != 0, "no bytes were written for it");
  c.ok(has(web.call("GET", "/api/files/list").body, "\"count\":301"), "the list still counts 301");
}

// Lane FSCAP: when the byte quota is hit, the oldest regenerable copies (chat
// attachments) go first so the save lands; owner artifacts are never evicted.
static void testByteQuotaEvictsCopiesFirst(ndtest::Ctx& c) {
  std::printf("  -- (h) the byte quota evicts the oldest regenerable copies, never owner artifacts --\n");
  Config cfg;
  const NimbusdRig::Options o = opts("hf-h");
  NimbusdRig rig(cfg, o);
  nimbus::orch::FileStore::Limits l;
  l.maxTotalBytes = 1200;   // tiny quota so the test needs no gigabytes
  rig.files().setLimits(l);
  std::string err;
  c.ok(rig.files().put("docs", "keep.md", std::string(200, 'k'), err), "the owner artifact stores");
  for (int i = 0; i < 5; i++)
    c.ok(rig.files().put("chat", "att-" + std::to_string(i) + ".jpg", std::string(200, 'a'), err),
         "chat attachment " + std::to_string(i) + " stores");
  c.eqi((long)rig.files().count(), 6, "six files fit exactly under the quota (200 + 5*200 = 1200)");
  // The next save needs 400 more: the oldest attachments (att-0, att-1 = 400 B)
  // are reclaimed so it lands; the owner artifact is never a candidate.
  c.ok(rig.files().put("docs", "new.md", std::string(400, 'n'), err), "the new save lands after reclaiming");
  c.ok(rig.files().has("docs/keep.md"), "the owner artifact is never evicted");
  c.ok(rig.files().has("docs/new.md"), "the new file is there");
  c.ok(!rig.files().has("chat/att-0.jpg") && !rig.files().has("chat/att-1.jpg"),
       "the oldest regenerable copies went first");
  c.ok(rig.files().has("chat/att-2.jpg") && rig.files().has("chat/att-4.jpg"),
       "the newer copies survive");
  c.eqi((long)rig.files().count(), 5, "keep + new + three attachments");
  struct stat st;
  c.ok(::stat((o.dataDir + "/mem/files/chat/att-0.jpg").c_str(), &st) != 0, "the evicted copy's bytes are gone too");
  // A need the remaining copies cannot cover (400 B left, 4000 needed): nothing is
  // evicted for a save that would still refuse, and the store says so in words.
  c.ok(!rig.files().put("docs", "more.md", std::string(4000, 'm'), err), "the copies cannot cover it: refused");
  c.ok(has(err, "full"), "with the store-full reason");
  c.ok(!rig.files().has("docs/more.md"), "and no bytes for it");
  c.ok(rig.files().has("chat/att-2.jpg"), "the remaining copies were NOT evicted for nothing");
  c.eqi((long)rig.files().count(), 5, "the count is unchanged");
}

// Lane FSCAP: per-call listings are bounded - the web list pages with
// limit/offset and says the total, and the files.list tool does the same.
static void testListingsArePaged(ndtest::Ctx& c) {
  std::printf("  -- (i) listings are bounded per call (web pages, the tool pages) --\n");
  Config cfg;
  NimbusdRig rig(cfg, opts("hf-i"));
  std::string err;
  for (int i = 0; i < 7; i++)
    rig.files().put("bulk", "f" + std::to_string(i) + ".txt", "x", err);
  Web web(rig);
  const ApiResp p1 = web.call("GET", "/api/files/list?limit=3&offset=0");
  c.ok(has(p1.body, "\"count\":7"), "the payload's count is the total");
  c.ok(has(p1.body, "\"shown\":3"), "shown is what this page carries");
  c.ok(has(p1.body, "f0.txt") && !has(p1.body, "f3.txt"), "the first page holds the first three");
  const ApiResp p3 = web.call("GET", "/api/files/list?limit=3&offset=6");
  c.ok(has(p3.body, "\"shown\":1") && has(p3.body, "f6.txt"), "the last page holds the remainder");
  const ApiResp all = web.call("GET", "/api/files/list");
  c.ok(has(all.body, "\"shown\":7"), "no params: the default page covers all seven");
  // review fix pass (F2): the payload carries listCount/nextOffset/more computed
  // from the STORE rows of the page's filter, so a client pages on store rows only
  c.ok(has(p1.body, "\"listCount\":7"), "listCount is the store total the page keys on");
  c.ok(has(p1.body, "\"nextOffset\":3") && has(p1.body, "\"more\":true"), "page 1: nextOffset=3, more");
  c.ok(has(p3.body, "\"nextOffset\":7") && has(p3.body, "\"more\":false"), "last page: nextOffset=7, no more");
  const std::string t1 = toolText(rig.callTool("files.list", R"({"limit":2,"offset":0})"));
  c.ok(has(t1, "showing 2 of 7") && has(t1, "f0.txt") && !has(t1, "f2.txt"), "the tool pages too");
  const std::string tAll = toolText(rig.callTool("files.list", R"({})"));
  c.ok(has(tAll, "f6.txt") && !has(tAll, "showing"), "the tool's default page lists all seven");
  rig.files().put("other", "o0.txt", "x", err);
  const ApiResp pf = web.call("GET", "/api/files/list?project=bulk&limit=3&offset=0");
  c.ok(has(pf.body, "\"count\":8") && has(pf.body, "\"listCount\":7") && has(pf.body, "\"nextOffset\":3"),
       "a project filter: count is the whole store, listCount the filtered rows");
}

// files.read of a binary file used to put its bytes in the tool result, and the next
// request was unreadable on every provider: the turn died with "That didn't finish".
static void testBinaryReadIsRefused(ndtest::Ctx& c) {
  std::printf("  -- (g) files.read refuses a binary file, by name and by content --\n");
  Config cfg;
  NimbusdRig rig(cfg, opts("hf-g"));
  Web web(rig);
  const std::string png("\x89PNG\r\n\x1a\n\0\0\0\rIHDR\x01\x02\x03\xff\xfe", 21);
  web.call("POST", "/api/files/upload?project=chat&name=photo.png", multipart("photo.png", png));
  web.call("POST", "/api/files/upload?project=chat&name=disguised.txt", multipart("disguised.txt", png));
  web.call("POST", "/api/files/upload?project=chat&name=sound.mp3", multipart("sound.mp3", "ID3 not really audio"));
  web.call("POST", "/api/files/upload?project=chat&name=notes.yaml", multipart("notes.yaml", "plain: text\nkey: 1\n"));
  web.call("POST", "/api/files/upload?project=chat&name=accents.txt", multipart("accents.txt", "caf\xC3\xA9 cr\xC3\xA8me\n"));
  for (const char* name : {"photo.png", "disguised.txt", "sound.mp3"}) {
    const std::string args = std::string(R"({"project":"chat","name":")") + name + R"("})";
    const std::string got = rig.callTool("files.read", args);
    c.ok(has(got, "not a text file"), (std::string("refused in words: ") + name).c_str());
    c.ok(!has(got, "PNG") && !has(got, "ID3"), (std::string("none of its bytes in the result: ") + name).c_str());
    const std::string tail = rig.callTool("files.read", std::string(R"({"project":"chat","tail":3,"name":")") + name + R"("})");
    c.ok(has(tail, "not a text file"), (std::string("the tail view refuses it too: ") + name).c_str());
  }
  c.ok(has(rig.callTool("files.read", R"({"project":"chat","name":"notes.yaml"})"), "plain: text"),
       "a text file with any other extension still reads");
  c.ok(has(rig.callTool("files.read", R"({"project":"chat","name":"accents.txt"})"), "caf\xC3\xA9"),
       "UTF-8 text still reads");
  // The download is untouched: refusing the tool is not losing the file.
  c.ok(web.call("GET", "/api/files/dl?project=chat&name=photo.png").body == png, "the download still has its bytes");
}

int main() {
  ndtest::Ctx c;
  std::printf("=== hosted files + not-yet-hosted surfaces (CUM-543, offline) ===\n");
  testUploadListDownload(c);
  testRemove(c);
  testFilesDeleteTool(c);
  testTraversalRefused(c);
  testThreeTablesAppendAlike(c);
  testNotYetHosted(c);
  testRefusedUploadLeavesNoFile(c);
  testByteQuotaEvictsCopiesFirst(c);
  testListingsArePaged(c);
  testBinaryReadIsRefused(c);
  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  std::printf("%s\n", c.failures ? "FAILED" : "PASSED");
  return c.failures ? 1 : 0;
}
