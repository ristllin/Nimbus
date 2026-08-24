#include "nimbus/text_page.h"

#include "nimbus/render_text.h"  // render::asciiSanitize - wrap the ASCII the font renders

// Greedy word-wrap. Words are runs of non-space, non-newline characters;
// runs of spaces collapse to a single separator and vanish entirely at line
// boundaries (leading/trailing). '\n' always forces a break, so consecutive
// newlines produce empty lines and a trailing newline yields a trailing empty
// line. A word longer than `cols` is flushed to a fresh line and hard-broken
// into cols-sized chunks. Pure string math - no framebuffer knowledge.

namespace nimbus {

std::vector<std::string> wrapText(const std::string& textRaw, size_t cols) {
  // Wrap the SANITIZED text so line lengths match what the font actually draws (a
  // multi-byte UTF-8 char collapses to its ASCII form before wrapping, not after).
  const std::string text = render::asciiSanitize(textRaw);
  if (cols == 0) cols = 1;  // degenerate but well-defined: one char per line

  std::vector<std::string> lines;
  std::string cur;
  const size_t n = text.size();
  size_t i = 0;
  while (i < n) {
    const char c = text[i];
    if (c == '\n') {  // forced break - even when cur is empty
      lines.push_back(cur);
      cur.clear();
      ++i;
      continue;
    }
    if (c == ' ') {  // collapse runs; the separator is re-added on placement
      ++i;
      continue;
    }

    size_t j = i;
    while (j < n && text[j] != ' ' && text[j] != '\n') ++j;
    std::string word = text.substr(i, j - i);
    i = j;

    const size_t sep = cur.empty() ? 0 : 1;
    if (cur.size() + sep + word.size() <= cols) {  // greedy: keep filling
      if (sep) cur += ' ';
      cur += word;
      continue;
    }
    if (!cur.empty()) {  // word starts a fresh line; the separator vanishes
      lines.push_back(cur);
      cur.clear();
    }
    while (word.size() > cols) {  // hard-break oversized words
      lines.push_back(word.substr(0, cols));
      word.erase(0, cols);
    }
    cur = word;  // never empty here (the loop leaves 1..cols chars)
  }
  lines.push_back(cur);  // final line: "" for empty text or a trailing '\n'
  return lines;
}

void TextPager::setText(const std::string& text, size_t cols,
                        size_t linesPerPage) {
  linesPerPage_ = linesPerPage ? linesPerPage : 1;
  lines_ = wrapText(text, cols);  // always >= 1 line
}

size_t TextPager::pageCount() const {
  const size_t n = lines_.empty() ? 1 : lines_.size();  // pre-setText safety
  return (n + linesPerPage_ - 1) / linesPerPage_;
}

std::vector<std::string> TextPager::page(size_t idx) const {
  const size_t pc = pageCount();  // >= 1, so pc - 1 is safe
  if (idx >= pc) idx = pc - 1;
  std::vector<std::string> out(linesPerPage_);  // padded with empty strings
  const size_t base = idx * linesPerPage_;
  for (size_t k = 0; k < linesPerPage_; ++k) {
    if (base + k < lines_.size()) out[k] = lines_[base + k];
  }
  return out;
}

}  // namespace nimbus
