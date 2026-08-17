#pragma once
// Docs pack (W13) - the device's OWN documentation, embedded in the firmware
// image so the Orchestrator model can consult it (docs.list / docs.search /
// docs.read) instead of guessing what it can or cannot do.
//
// The data table lives in the GENERATED nimbus/docs_pack_data.h
// (tools/gen_docs_pack.py - edit docs/, re-run; the pack rides firmware.bin,
// so OTA updates it and it can never version-skew). This module is the
// portable, host-tested access layer over that table: bodies are flash rodata
// scanned in place - a read never copies a body through an internal-heap
// intermediate buffer (the ToolResult string is built directly from it).

#include <cstddef>
#include <string>

namespace nimbus {
namespace docs {

// One `##` section of a curated doc, split at BUILD time so every body fits
// under the head-loop's per-tool-result clamp. id = "<file-slug>#<heading-slug>".
struct DocSection {
  const char* id;
  const char* title;
  const char* body;
};

// One curated doc file: its sections are the contiguous run
// kDocsSections[first .. first+count) - cheap grouped listing, no runtime scan.
struct DocFile {
  const char* slug;
  const char* title;
  unsigned short first;
  unsigned short count;
};

size_t sectionCount();
const DocSection& section(size_t i);
size_t fileCount();
const DocFile& file(size_t i);

// Exact-id lookup; nullptr when unknown.
const DocSection* find(const std::string& id);
// The file whose slug matches; nullptr when unknown.
const DocFile* findFile(const std::string& slug);

// Keyword AND-match (epiTextMatch - the host-tested episodic matcher) over
// title+body. Fills out[0..max) with matching sections in table order; returns
// the number of hits (never more than max).
size_t search(const std::string& query, const DocSection** out, size_t max);

// A one-line snippet around the first occurrence of the query's first keyword
// in the section body (title fallback: the body's first line). <= ~160 bytes,
// trimmed at UTF-8 boundaries.
std::string snippet(const DocSection& s, const std::string& query);

// For an honest unknown-id error: up to max near-miss ids (same file prefix,
// or ids containing the heading part), comma-separated; empty when none.
std::string nearMisses(const std::string& id, size_t max);

}  // namespace docs
}  // namespace nimbus
