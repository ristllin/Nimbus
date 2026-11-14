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
//   - Only CLOSED marker pairs convert: **bold**, *italic* / _italic_,
//     `code`, ```pre blocks```, [text](http://url). An unpaired marker stays
//     a literal (escaped) char - the converter must never emit unbalanced
//     tags (Telegram rejects them).
//   - #/##/### heading lines render as <b>line</b> (Telegram has no headings).
//   - Italic uses the web renderer's boundary rules (opened after whitespace,
//     closed before punctuation/whitespace/end on the same line, no edge
//     whitespace, no leading digit), so math/globs stay literal ("2*3",
//     "*.cpp", "snake_case").
//   - Bullet lines (-, *, + at line start) render as a text bullet U+2022:
//     Telegram HTML has no list elements; numbered items read as a list as-is.
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
