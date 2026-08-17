#pragma once
#include <string>

// provider_file_fetch - v4.1 provider-side FILE capture.
//
// A sub-agent ran hosted `code_interpreter` Python and produced a binary file
// (a PDF, an image, ...). Two backends (both download shapes verified live
// 2026-08-08 - raw bytes, 200, no redirect):
//   mistral - Conversations `tool_file` {file_id, file_name}; download
//             GET /v1/files/<id>/content. ⚠ Their sandbox 500s when the
//             artifact is a PDF/CSV, so mistral delivers images/text only.
//   openai  - Responses `container_file_citation` {container_id, file_id,
//             filename} (fileId here = "<container_id>/<file_id>"); download
//             GET /v1/containers/<cid>/files/<fid>/content. Full file output
//             INCLUDING PDF - the PDF path.
// The JobEngine hands the reference here on the terminal poll. This module
// STREAMS the bytes straight to the SD artifact store (Content-Length-verified
// - a truncated download is refused, never registered) and registers them, so
// the head can `files.send` the file to Telegram.
//
// ⛔ Hard constraints (AGENTS.md stability / nimbus-no-subagent-concurrency):
//   - NO on-device rendering - bytes are copied, never parsed.
//   - NO new concurrency - this runs on the CALLING task (tg_poll), takes the ONE
//     TLS work slot for a single download, and releases it. The poll that
//     precedes it has already returned (mistral: cache-local; openai: its GET
//     closed), so no TLS is held when this starts.
//   - RAM-bounded - a small fixed buffer streams to SD chunk by chunk (the OTA
//     download pattern); the file is never held whole in RAM.

namespace agent {

// Fetch the provider file `fileId` and register it under <project>/<name> in the
// artifact store, stamped with ownerNs (the spawning chat's data boundary).
// `nameHint` (the sub-agent's chosen name) and `fileName` (the sandbox file name,
// which carries the real extension) together choose the on-SD name; `tag` keeps
// it unique. `project` empty => a default "files" project (nothing is lost).
// Returns the outcome line for the fresh result - "[file saved: p/f]" or
// "[file FAILED: <why>]" - or "" when `backend` has no capture path.
std::string captureProviderFile(const std::string& backend, const std::string& fileId,
                                const std::string& fileName, const std::string& project,
                                const std::string& nameHint, const std::string& tag,
                                const std::string& ownerNs);

}  // namespace agent
