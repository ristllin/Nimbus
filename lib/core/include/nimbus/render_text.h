#pragma once
#include <string>

// screen_text - text helpers shared by the portable pager and any renderer.

namespace nimbus::render {

// Transliterate UTF-8 text into a printable-ASCII subset: smart quotes/dashes/
// ellipsis -> ASCII, accented Latin -> base letter, arrows/bullets -> ASCII,
// everything else (emoji, CJK, symbols) dropped. Without it every byte of a
// multi-byte UTF-8 char would draw as its own '?' (e.g. a curly apostrophe ->
// "???"). The pager applies it so wrapped text stays clean by default.
std::string asciiSanitize(const std::string& in);

}  // namespace nimbus::render
