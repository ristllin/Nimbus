#pragma once
#include <cstdint>
#include <string>

#include "nimbus/harness/plain/turn.h"

// plain/docs.h - readable documents (PRD section 5, "Documents"): a native document part
// by file id where the head has one, else one extraction call at intake by a provider
// the owner chose. Until then a PDF is stored and the message says it cannot be read yet.
//
// Seam declared by lane RB-B0a; the body (plain_docs.cpp) is lane RB-P2's.
namespace agent {
namespace plain {

// How a document reaches the head that answers this turn.
enum class DocRoute : uint8_t { NotReadable = 0, NativePart = 1, Extracted = 2, TextFile = 3 };

// The route for a stored document at `path` with media type `mime` on this turn's head.
Status documentRoute(const TurnCtx& turn, const std::string& path, const std::string& mime, DocRoute& out);

}  // namespace plain
}  // namespace agent
