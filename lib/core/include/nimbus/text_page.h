#pragma once
#include <string>
#include <vector>

// text_page - word-wrap + encoder paging for the e-ink detail pane.
// The brief bans marquee scrolling (e-ink can't animate); long text wraps into
// fixed-width lines shown a page (default two lines) at a time, and the encoder
// pages through. Pure string math, host-tested.

namespace nimbus {

// Greedy word-wrap. Words longer than `cols` are hard-broken. '\n' forces a
// line break. Returns at least one (possibly empty) line.
std::vector<std::string> wrapText(const std::string& text, size_t cols);

class TextPager {
 public:
  void setText(const std::string& text, size_t cols, size_t linesPerPage = 2);
  size_t pageCount() const;
  // Lines of page `idx` (clamped). Always returns exactly linesPerPage entries,
  // padded with empty strings, so screen layouts stay stable.
  std::vector<std::string> page(size_t idx) const;

 private:
  std::vector<std::string> lines_;
  size_t linesPerPage_ = 2;
};

}  // namespace nimbus
