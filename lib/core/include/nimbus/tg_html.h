#pragma once
#include <string>

// tg_html - Telegram parse_mode=HTML converter (v4.1.1).
//
// The bot sent every message as PLAIN text (no parse_mode), so the model's
// markdown reached the owner as literal asterisks and backticks ("✅ **PDF -
// delivered!**"). MarkdownV2 was rejected deliberately: it reserves 18
// characters that all need escaping in dynamic text, one miss 400s the WHOLE
// message, and its syntax (*bold*) differs from what models emit (**bold**).
// HTML needs only &, <, > escaped and maps 1:1 from the model's habits.
//
// Conversion contract (conservative by construction):
//   - & < > are entity-escaped FIRST - a model reply can never inject markup.
//   - Only CLOSED marker pairs convert: **bold**, `code`, ```pre blocks```,
//     [text](http://url). An unpaired marker stays a literal (escaped) char -
//     the converter must never emit unbalanced tags (Telegram rejects them).
//   - #/##/### heading lines render as <b>line</b> (Telegram has no headings).
//   - Single *asterisk* emphasis is left ALONE: models rarely use it for
//     emphasis but math/globs use it constantly ("2*3", "*.cpp").
//   - Inside `code`/```pre``` nothing else converts (verbatim, escaped).
//   - Links: only http/https URLs convert; the URL's quotes are escaped.
//
// The device pairs this with a fallback: send parse_mode=HTML first, and if
// Telegram rejects the entities, resend the ORIGINAL text plain - formatting
// can degrade, a reply can never be lost. Portable + host-tested (test_tg_html).
namespace nimbus {

// Convert model markdown to Telegram-safe HTML. Never fails.
std::string tgHtml(const std::string& text);

}  // namespace nimbus
