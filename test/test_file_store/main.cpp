#include <cstring>
#include <unity.h>

#include <string>
#include <vector>

#include "nimbus/orch/blob_store.h"   // BlobHasher - the streamed content hash
#include "nimbus/orch/file_store.h"

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

static FileEntry mk(const char* proj, const char* name, uint32_t bytes,
                    uint64_t hash = 0x1234) {
  FileEntry e;
  e.project = proj; e.name = name; e.bytes = bytes;
  e.createdAt = 1700000000; e.kind = fileKindForName(name); e.hash = hash;
  return e;
}

// ---- path safety: THE traversal gate ---------------------------------------

static void test_segment_safety_rejects_traversal() {
  TEST_ASSERT_FALSE(FileStore::validSegment("..", 48));
  TEST_ASSERT_FALSE(FileStore::validSegment(".", 48));
  TEST_ASSERT_FALSE(FileStore::validSegment("a/b", 48));
  TEST_ASSERT_FALSE(FileStore::validSegment("a\\b", 48));
  TEST_ASSERT_FALSE(FileStore::validSegment("..evil", 48));      // leading dot
  TEST_ASSERT_FALSE(FileStore::validSegment(".hidden", 48));
  TEST_ASSERT_FALSE(FileStore::validSegment("", 48));
  TEST_ASSERT_FALSE(FileStore::validSegment("a:b", 48));         // FAT quirks
  TEST_ASSERT_FALSE(FileStore::validSegment("a b", 48));         // spaces
  TEST_ASSERT_FALSE(FileStore::validSegment("a\tb", 48));        // tab (format-critical)
  TEST_ASSERT_FALSE(FileStore::validSegment("a\nb", 48));
  TEST_ASSERT_FALSE(FileStore::validSegment("caf\xC3\xA9", 48)); // non-ASCII
  TEST_ASSERT_FALSE(FileStore::validSegment("aaaaaaaaaaaaaaaaa", 16));  // too long
  TEST_ASSERT_TRUE(FileStore::validSegment("report-2026_v1.pdf", 48));
  TEST_ASSERT_TRUE(FileStore::validSegment("trading", 24));
}

static void test_relpath_validates_both_segments() {
  FileStore s;
  TEST_ASSERT_EQUAL_STRING("proj/a.md", s.relPath("proj", "a.md").c_str());
  TEST_ASSERT_TRUE(s.relPath("..", "a.md").empty());
  TEST_ASSERT_TRUE(s.relPath("proj", "../etc").empty());
}

// ---- kinds -------------------------------------------------------------------

static void test_kind_inference() {
  TEST_ASSERT_EQUAL(int(FileKind::Doc),   int(fileKindForName("report.PDF")));
  TEST_ASSERT_EQUAL(int(FileKind::Doc),   int(fileKindForName("notes.md")));
  TEST_ASSERT_EQUAL(int(FileKind::Image), int(fileKindForName("chart.png")));
  TEST_ASSERT_EQUAL(int(FileKind::Audio), int(fileKindForName("memo.ogg")));
  TEST_ASSERT_EQUAL(int(FileKind::Data),  int(fileKindForName("blob.bin")));
  TEST_ASSERT_EQUAL(int(FileKind::Data),  int(fileKindForName("noext")));
  TEST_ASSERT_EQUAL_STRING("image", fileKindName(FileKind::Image));
}

// ---- add / quota / durability ------------------------------------------------

static void test_add_find_replace_remove() {
  FileStore s;
  std::string err;
  TEST_ASSERT_TRUE(s.add(mk("proj", "a.md", 100), err));
  TEST_ASSERT_TRUE(s.add(mk("proj", "b.md", 200), err));
  TEST_ASSERT_EQUAL(2, int(s.count()));
  TEST_ASSERT_EQUAL(300, int(s.totalBytes()));

  const FileEntry* e = s.find("proj", "a.md");
  TEST_ASSERT_NOT_NULL(e);
  TEST_ASSERT_EQUAL(100, int(e->bytes));

  // Replace same project+name: entry count stays, bytes update.
  TEST_ASSERT_TRUE(s.add(mk("proj", "a.md", 150, 0x9999), err));
  TEST_ASSERT_EQUAL(2, int(s.count()));
  TEST_ASSERT_EQUAL(350, int(s.totalBytes()));
  TEST_ASSERT_EQUAL_UINT64(0x9999, s.find("proj", "a.md")->hash);

  TEST_ASSERT_TRUE(s.remove("proj", "a.md"));
  TEST_ASSERT_FALSE(s.remove("proj", "a.md"));   // already gone
  TEST_ASSERT_EQUAL(1, int(s.count()));
}

static void test_quotas_refuse_never_evict() {
  FileStore::Limits lim;
  lim.maxEntries = 2; lim.maxFileBytes = 1000; lim.maxTotalBytes = 1500;
  FileStore s(lim);
  std::string err;
  TEST_ASSERT_TRUE(s.add(mk("p", "a.bin", 900), err));
  // Per-file cap.
  TEST_ASSERT_FALSE(s.add(mk("p", "big.bin", 1001), err));
  TEST_ASSERT_EQUAL_STRING("file too large", err.c_str());
  // Total cap: 900 + 700 > 1500.
  TEST_ASSERT_FALSE(s.add(mk("p", "b.bin", 700), err));
  TEST_ASSERT_TRUE(err.find("full") != std::string::npos);
  TEST_ASSERT_TRUE(s.add(mk("p", "b.bin", 500), err));
  // Entry cap.
  TEST_ASSERT_FALSE(s.add(mk("p", "c.bin", 10), err));
  TEST_ASSERT_TRUE(err.find("too many") != std::string::npos);
  // Replacement is allowed at the entry cap (same name) and re-budgets bytes.
  TEST_ASSERT_TRUE(s.add(mk("p", "a.bin", 950), err));
  TEST_ASSERT_EQUAL(2, int(s.count()));
  // Nothing was silently evicted at any point.
  TEST_ASSERT_NOT_NULL(s.find("p", "b.bin"));
}

static void test_would_exceed_precheck_for_streaming() {
  FileStore::Limits lim;
  lim.maxFileBytes = 1000; lim.maxTotalBytes = 1500; lim.maxEntries = 8;
  FileStore s(lim);
  std::string err;
  TEST_ASSERT_TRUE(s.add(mk("p", "a.bin", 900), err));
  TEST_ASSERT_TRUE(s.wouldExceed("p", "new.bin", 700, err));      // total would blow
  TEST_ASSERT_FALSE(s.wouldExceed("p", "new.bin", 500, err));     // fits
  TEST_ASSERT_FALSE(s.wouldExceed("p", "a.bin", 1000, err));      // replacing a.bin frees 900
  TEST_ASSERT_TRUE(s.wouldExceed("p", "../x", 10, err));          // safety in the precheck too
}

// ---- provider file_id cache ----------------------------------------------------

static void test_provider_id_cache() {
  FileStore s;
  std::string err;
  TEST_ASSERT_TRUE(s.add(mk("p", "r.pdf", 100), err));
  TEST_ASSERT_FALSE(s.setProviderId("p", "nope.pdf", "anthropic", "file_x"));
  TEST_ASSERT_TRUE(s.setProviderId("p", "r.pdf", "anthropic", "file_abc123"));
  TEST_ASSERT_EQUAL_STRING("file_abc123", s.find("p", "r.pdf")->providerFileId.c_str());
  // Replacing the entry (new content) resets the cache - a stale file_id must not
  // survive a content change.
  TEST_ASSERT_TRUE(s.add(mk("p", "r.pdf", 120, 0xBEEF), err));
  TEST_ASSERT_EQUAL_STRING("", s.find("p", "r.pdf")->providerFileId.c_str());
}

// ---- persistence ----------------------------------------------------------------

static void test_dump_load_roundtrip() {
  FileStore s;
  std::string err;
  FileEntry a = mk("trading", "q3-report.pdf", 4096, 0xDEADBEEF12345678ull);
  a.providerTag = "anthropic"; a.providerFileId = "file_xyz";
  TEST_ASSERT_TRUE(s.add(a, err));
  TEST_ASSERT_TRUE(s.add(mk("notes", "todo.md", 88), err));

  FileStore s2;
  TEST_ASSERT_TRUE(s2.load(s.dump()));
  TEST_ASSERT_EQUAL(2, int(s2.count()));
  const FileEntry* e = s2.find("trading", "q3-report.pdf");
  TEST_ASSERT_NOT_NULL(e);
  TEST_ASSERT_EQUAL(4096, int(e->bytes));
  TEST_ASSERT_EQUAL_UINT64(0xDEADBEEF12345678ull, e->hash);
  TEST_ASSERT_EQUAL_STRING("anthropic", e->providerTag.c_str());
  TEST_ASSERT_EQUAL_STRING("file_xyz", e->providerFileId.c_str());
  TEST_ASSERT_EQUAL(int(FileKind::Doc), int(e->kind));
}

static void test_load_tolerates_garbage_and_revalidates() {
  FileStore s2;
  // Wrong header -> refuse outright.
  TEST_ASSERT_FALSE(s2.load("NOTFILES\nx"));
  // Good header, one good line, one malformed, one traversal smuggle.
  std::string blob = "FILESv1\n";
  blob += "p\tok.md\t10\t0\t0\t0000000000000001\t\t\n";
  blob += "broken line no tabs\n";
  blob += "..\tevil.md\t10\t0\t0\t0000000000000002\t\t\n";   // re-validated on load
  TEST_ASSERT_TRUE(s2.load(blob));
  TEST_ASSERT_EQUAL(1, int(s2.count()));
  TEST_ASSERT_NOT_NULL(s2.find("p", "ok.md"));
}

static void test_list_and_projects() {
  FileStore s;
  std::string err;
  TEST_ASSERT_TRUE(s.add(mk("b-proj", "x.md", 1), err));
  TEST_ASSERT_TRUE(s.add(mk("a-proj", "y.md", 1), err));
  TEST_ASSERT_TRUE(s.add(mk("a-proj", "z.md", 1), err));
  TEST_ASSERT_EQUAL(3, int(s.list().size()));
  TEST_ASSERT_EQUAL(2, int(s.list("a-proj").size()));
  auto p = s.projects();
  TEST_ASSERT_EQUAL(2, int(p.size()));
  TEST_ASSERT_EQUAL_STRING("a-proj", p[0].c_str());   // sorted
}

// ---- v3.7.0 per-principal artifact boundary -----------------------------------
static void test_owner_boundary_read_and_overwrite() {
  FileStore st;
  std::string err;
  FileEntry mine;  mine.project = "notes"; mine.name = "plan.md";
  mine.owner = "chat:alice"; mine.bytes = 10;
  TEST_ASSERT_TRUE(st.add(mine, err));
  FileEntry ownerFile; ownerFile.project = "notes"; ownerFile.name = "secret.md";
  ownerFile.owner = nimbus::orch::kOwnerNs; ownerFile.bytes = 10;
  TEST_ASSERT_TRUE(st.add(ownerFile, err));

  TEST_ASSERT_TRUE(st.ownedBy(*st.find("notes", "plan.md"), "chat:alice", false));
  TEST_ASSERT_FALSE(st.ownedBy(*st.find("notes", "secret.md"), "chat:alice", false));
  TEST_ASSERT_TRUE(st.ownedBy(*st.find("notes", "secret.md"), nimbus::orch::kOwnerNs, true));

  // A legacy row (no owner recorded) belongs to the device owner, not to all.
  FileEntry legacy; legacy.project = "old"; legacy.name = "a.txt"; legacy.bytes = 1;
  TEST_ASSERT_TRUE(st.add(legacy, err));
  TEST_ASSERT_FALSE(st.ownedBy(*st.find("old", "a.txt"), "chat:alice", false));
  TEST_ASSERT_TRUE(st.ownedBy(*st.find("old", "a.txt"), "chat:alice", true));

  // The destroy-by-replace hazard: (project,name) is the identity and add()
  // REPLACES, so a write onto someone else's path must be refused BEFORE it
  // reaches the filesystem.
  TEST_ASSERT_FALSE(st.writeAllowed("notes", "secret.md", "chat:alice", false));
  TEST_ASSERT_TRUE(st.writeAllowed("notes", "plan.md", "chat:alice", false));
  TEST_ASSERT_TRUE(st.writeAllowed("notes", "brand-new.md", "chat:alice", false));
  TEST_ASSERT_TRUE(st.writeAllowed("notes", "secret.md", nimbus::orch::kOwnerNs, true));
}

static void test_index_owner_field_roundtrip_and_legacy_load() {
  FileStore st;
  std::string err;
  FileEntry e; e.project = "p"; e.name = "n.txt"; e.bytes = 5;
  e.owner = "chat:bob"; e.providerTag = "openai"; e.providerFileId = "file_9";
  TEST_ASSERT_TRUE(st.add(e, err));
  FileStore back;
  TEST_ASSERT_TRUE(back.load(st.dump()));
  TEST_ASSERT_EQUAL_STRING("chat:bob", back.find("p", "n.txt")->owner.c_str());
  TEST_ASSERT_EQUAL_STRING("file_9", back.find("p", "n.txt")->providerFileId.c_str());

  // A PRE-v3.7 index (eight fields) still loads with every real column intact -
  // the migration must never trigger the rebuild-by-scan that erases createdAt,
  // hashes and provider ids.
  const std::string legacy =
      "FILESv1\nq\tm.txt\t42\t1700000000\t0\t00000000000000ff\tanthropic\tfile_1\n";
  FileStore old;
  TEST_ASSERT_TRUE(old.load(legacy));
  const FileEntry* le = old.find("q", "m.txt");
  TEST_ASSERT_NOT_NULL(le);
  TEST_ASSERT_EQUAL_UINT32(42, le->bytes);
  TEST_ASSERT_EQUAL_UINT32(1700000000, le->createdAt);
  TEST_ASSERT_EQUAL_STRING("file_1", le->providerFileId.c_str());
  TEST_ASSERT_TRUE(le->owner.empty());
}

// v3.7.0 sharing: read-only, per-artifact, owner-granted - and it must survive
// the index round-trip without a new column (rollback safety).
static void test_sharing_is_read_only_and_survives_roundtrip() {
  FileStore st;
  std::string err;
  FileEntry doc; doc.project = "docs"; doc.name = "menu.md";
  doc.owner = "chat:alice"; doc.bytes = 100; doc.shared = true;
  TEST_ASSERT_TRUE(st.add(doc, err));

  // Bob can READ a shared file he does not own...
  TEST_ASSERT_TRUE(st.readableBy(*st.find("docs", "menu.md"), "chat:bob", false));
  // ...but never WRITE it: sharing grants no authority to replace or delete.
  TEST_ASSERT_FALSE(st.writeAllowed("docs", "menu.md", "chat:bob", false));
  TEST_ASSERT_TRUE(st.writeAllowed("docs", "menu.md", "chat:alice", false));
  TEST_ASSERT_TRUE(st.writeAllowed("docs", "menu.md", "anyone", true));   // admin

  // An UNshared file stays invisible to everyone but its owner and admins.
  FileEntry priv; priv.project = "docs"; priv.name = "private.md";
  priv.owner = "chat:alice"; priv.bytes = 10;
  TEST_ASSERT_TRUE(st.add(priv, err));
  TEST_ASSERT_FALSE(st.readableBy(*st.find("docs", "private.md"), "chat:bob", false));

  // Round-trip: the shared flag rides the owner field, and a reload preserves
  // BOTH the owner and the flag.
  FileStore back;
  TEST_ASSERT_TRUE(back.load(st.dump()));
  TEST_ASSERT_TRUE(back.find("docs", "menu.md")->shared);
  TEST_ASSERT_EQUAL_STRING("chat:alice", back.find("docs", "menu.md")->owner.c_str());
  TEST_ASSERT_FALSE(back.find("docs", "private.md")->shared);
  TEST_ASSERT_EQUAL_STRING("chat:alice", back.find("docs", "private.md")->owner.c_str());
}

static void test_bytes_are_counted_per_namespace() {
  FileStore st;
  std::string err;
  FileEntry a; a.project = "p"; a.name = "a"; a.owner = "chat:alice"; a.bytes = 1000;
  FileEntry b; b.project = "p"; b.name = "b"; b.owner = "chat:bob";   b.bytes = 250;
  FileEntry c; c.project = "p"; c.name = "c"; c.owner = "chat:alice"; c.bytes = 500;
  st.add(a, err); st.add(b, err); st.add(c, err);
  TEST_ASSERT_EQUAL_UINT32(1500, st.bytesFor("chat:alice"));
  TEST_ASSERT_EQUAL_UINT32(250, st.bytesFor("chat:bob"));
  // A legacy row (no owner) counts against the device owner.
  FileEntry old; old.project = "p"; old.name = "old"; old.bytes = 7;
  st.add(old, err);
  TEST_ASSERT_EQUAL_UINT32(7, st.bytesFor(nimbus::orch::kOwnerNs));
}

// v4.1 provider file capture - the binary register seam. A mocked "fetch"
// (a byte buffer standing in for GET /v1/files/<id>/content, WITH embedded NUL
// and high bytes so it is genuinely binary) streams in chunks to a sink using
// BYTE LENGTHS (a C-string path would truncate at the first NUL), accumulating
// the same BlobHasher the device's finishWrite uses, then registers a FileEntry.
// This exercises the real portable primitives the device seam relies on
// (BlobHasher + fileKindForName + FileStore.add + dump/load), and proves the
// pipeline is binary-safe end to end.
static void test_binary_fetch_write_and_register() {
  // A minimal fake PDF payload: magic, then a NUL, a 0xFF, and more bytes.
  std::string src = "%PDF-1.4\n";
  src.push_back('\x00');
  src.push_back('\xFF');
  src += "1 0 obj<<>>endobj\n";
  src.push_back('\x00');
  src += "%%EOF\n";
  const size_t srcLen = src.size();
  TEST_ASSERT_TRUE(srcLen > strlen(src.c_str()));   // sanity: a NUL is really embedded

  // Mock the fetch: hand out the payload in 3 uneven chunks (one splits a NUL run).
  std::vector<std::string> chunks = {src.substr(0, 5), src.substr(5, 8), src.substr(13)};

  // The "SD write": append each chunk by LENGTH (binary-safe) + hash it, exactly
  // as files::writeChunk does (f.write(data,len) + hasher.update(data,len)).
  std::string sink;
  BlobHasher hasher;
  size_t written = 0;
  for (const auto& c : chunks) {
    sink.append(c.data(), c.size());
    hasher.update(reinterpret_cast<const uint8_t*>(c.data()), c.size());
    written += c.size();
  }

  // Binary-safe: every byte survived, including the NULs.
  TEST_ASSERT_EQUAL(srcLen, written);
  TEST_ASSERT_EQUAL(srcLen, sink.size());
  TEST_ASSERT_EQUAL(0, memcmp(sink.data(), src.data(), srcLen));
  // Streaming hash == whole-blob hash over the same bytes.
  TEST_ASSERT_EQUAL_STRING(blobHash(src).c_str(), hasher.hex().c_str());

  // Register it - the finishWrite build path (owner/project/name/bytes/kind/hash).
  FileStore store;
  FileEntry e;
  e.owner = "chat:42";
  e.project = "reports";
  e.name = "report.pdf";
  e.bytes = (uint32_t)written;
  e.createdAt = 1700000000;
  e.kind = fileKindForName(e.name);
  e.hash = strtoull(hasher.hex().c_str(), nullptr, 16);
  std::string err;
  TEST_ASSERT_TRUE_MESSAGE(store.add(e, err), err.c_str());

  // A PDF classifies as a document; files.list/files.send see it via the index.
  const FileEntry* got = store.find("reports", "report.pdf");
  TEST_ASSERT_NOT_NULL(got);
  TEST_ASSERT_EQUAL((int)FileKind::Doc, (int)got->kind);
  TEST_ASSERT_EQUAL(srcLen, got->bytes);
  TEST_ASSERT_TRUE(got->hash != 0);
  TEST_ASSERT_EQUAL_STRING("chat:42", got->owner.c_str());

  // Index persistence round-trips the binary entry (bytes/hash/kind/owner).
  FileStore reloaded;
  TEST_ASSERT_TRUE(reloaded.load(store.dump()));
  const FileEntry* r = reloaded.find("reports", "report.pdf");
  TEST_ASSERT_NOT_NULL(r);
  TEST_ASSERT_EQUAL(srcLen, r->bytes);
  TEST_ASSERT_EQUAL(e.hash, r->hash);
  TEST_ASSERT_EQUAL((int)FileKind::Doc, (int)r->kind);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_binary_fetch_write_and_register);
  RUN_TEST(test_owner_boundary_read_and_overwrite);
  RUN_TEST(test_index_owner_field_roundtrip_and_legacy_load);
  RUN_TEST(test_sharing_is_read_only_and_survives_roundtrip);
  RUN_TEST(test_bytes_are_counted_per_namespace);
  RUN_TEST(test_segment_safety_rejects_traversal);
  RUN_TEST(test_relpath_validates_both_segments);
  RUN_TEST(test_kind_inference);
  RUN_TEST(test_add_find_replace_remove);
  RUN_TEST(test_quotas_refuse_never_evict);
  RUN_TEST(test_would_exceed_precheck_for_streaming);
  RUN_TEST(test_provider_id_cache);
  RUN_TEST(test_dump_load_roundtrip);
  RUN_TEST(test_load_tolerates_garbage_and_revalidates);
  RUN_TEST(test_list_and_projects);
  return UNITY_END();
}
