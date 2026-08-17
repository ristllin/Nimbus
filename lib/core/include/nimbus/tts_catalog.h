#pragma once
#include <set>
#include <string>

// Portable, host-testable transform for Mistral's GET /v1/audio/voices catalog.
// No Arduino deps (ArduinoJson v7 is header-only, host-safe).
//
// Why this exists: Mistral's voices endpoint is PAGINATED and ignores the `page`
// query field entirely - only `offset`/`limit` advance the window, while the
// response body still echoes a misleading `page`/`page_size`/`total_pages`. The
// device used to read one response (the first 10 of 30 voices) and stop, so the
// picker only ever saw a third of the catalog (e.g. Jane's sole "sarcasm" of her
// nine emotions, and Marie/French not at all). The device now paginates via
// `offset` and feeds each page here; this same code path is asserted by
// test/test_tts_voices so the device and the test can't drift.
namespace core {

// Parse ONE page body ({"items":[...],"total":N}) and append a UI row
// {"value","label","name","gender","lang","emotion"} for every voice whose slug is
// not already in `seen`. Rows are appended to `outRows` as comma-separated JSON
// objects with NO enclosing brackets (the caller wraps once with [ ]); the leading
// comma is handled internally so pages concatenate cleanly. Each emitted slug is
// added to `seen` (dedup key) so a server that ignores `offset` and re-serves the
// same page can't inflate the list. `name`/`emotion` are split from Mistral's
// "Paul - Sad" name so the web cascade groups by persona.
//
// Returns the number of raw items PROCESSED from the page (the items[] length,
// pre-dedup) - the caller advances `offset` by this. `*added` (if non-null)
// receives the count of NEW unique rows; when it is 0 the page contributed nothing
// new and the caller stops. `*total` (if non-null) receives the response's
// advertised catalog size - a loop hint only, never trusted for the final count.
int mergeMistralVoicesPage(const char* pageJson, std::set<std::string>& seen,
                           std::string& outRows, int* added = nullptr,
                           int* total = nullptr);

}  // namespace core
