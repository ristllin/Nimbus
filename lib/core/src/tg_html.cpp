#include "nimbus/tg_html.h"

namespace nimbus {
namespace {

void escapeInto(std::string& out, const std::string& s, size_t from, size_t to) {
  for (size_t i = from; i < to && i < s.size(); i++) {
    const char c = s[i];
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else out += c;
  }
}

// Is s[i..] the start of a fenced block (``` at line start)? Returns fence end
// (index AFTER the closing ``` and its newline) or npos when unclosed.
size_t fenceEnd(const std::string& s, size_t i) {
  size_t nl = s.find('\n', i + 3);
  if (nl == std::string::npos) return std::string::npos;
  // ⚠ prism: only a REAL fence line opens a block - ``` followed by nothing or
  // a bare language tag ([A-Za-z0-9_+-]*), then the newline. Treating ANY
  // line-start ``` as a fence swallowed same-line content ("```json {..}``` and
  // more") into the next fence - silent reply loss the plain-text fallback can
  // never catch, because the emitted HTML was balanced and Telegram accepted it.
  for (size_t k = i + 3; k < nl; k++) {
    const char c = s[k];
    const bool tagByte = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                         (c >= '0' && c <= '9') || c == '_' || c == '+' || c == '-';
    if (!tagByte) return std::string::npos;   // not a fence line -> caller falls through
  }
  size_t close = s.find("```", nl + 1);
  if (close == std::string::npos) return std::string::npos;
  return close + 3;
}

bool atLineStart(const std::string& s, size_t i) {
  return i == 0 || s[i - 1] == '\n';
}

}  // namespace

std::string tgHtml(const std::string& text) {
  std::string out;
  out.reserve(text.size() + 64);
  size_t i = 0;
  const size_t n = text.size();
  while (i < n) {
    // ``` fenced block (line start) -> <pre> (verbatim, escaped)
    if (atLineStart(text, i) && text.compare(i, 3, "```") == 0) {
      const size_t end = fenceEnd(text, i);
      if (end != std::string::npos) {
        size_t bodyStart = text.find('\n', i + 3);
        bodyStart = (bodyStart == std::string::npos) ? i + 3 : bodyStart + 1;
        const size_t bodyEnd = end - 3;
        out += "<pre>";
        escapeInto(out, text, bodyStart, bodyEnd);
        // trim ONE trailing newline inside the pre (the fence's own line break)
        if (out.size() >= 1 && out.back() == '\n') out.pop_back();
        out += "</pre>";
        i = end;
        if (i < n && text[i] == '\n') { out += '\n'; i++; }
        continue;
      }
    }
    // `inline code` -> <code> (same line only; unclosed stays literal)
    if (text[i] == '`') {
      const size_t close = text.find('`', i + 1);
      const size_t nl = text.find('\n', i + 1);
      if (close != std::string::npos && close > i + 1 &&
          (nl == std::string::npos || close < nl)) {
        out += "<code>";
        escapeInto(out, text, i + 1, close);
        out += "</code>";
        i = close + 1;
        continue;
      }
    }
    // **bold** -> <b> (same paragraph; unclosed stays literal)
    if (text.compare(i, 2, "**") == 0) {
      const size_t close = text.find("**", i + 2);
      const size_t para = text.find("\n\n", i + 2);
      if (close != std::string::npos && close > i + 2 &&
          (para == std::string::npos || close < para)) {
        out += "<b>";
        escapeInto(out, text, i + 2, close);
        out += "</b>";
        i = close + 2;
        continue;
      }
    }
    // # heading line -> <b>line</b>
    if (atLineStart(text, i) && text[i] == '#') {
      size_t h = i;
      while (h < n && text[h] == '#' && h - i < 6) h++;
      if (h < n && text[h] == ' ') {
        size_t eol = text.find('\n', h);
        if (eol == std::string::npos) eol = n;
        out += "<b>";
        escapeInto(out, text, h + 1, eol);
        out += "</b>";
        i = eol;
        continue;
      }
    }
    // [text](http…url) -> <a href="url">text</a>
    if (text[i] == '[') {
      const size_t tclose = text.find(']', i + 1);
      if (tclose != std::string::npos && tclose + 1 < n && text[tclose + 1] == '(') {
        const size_t uclose = text.find(')', tclose + 2);
        if (uclose != std::string::npos) {
          const std::string url = text.substr(tclose + 2, uclose - tclose - 2);
          const bool httpish =
              url.compare(0, 7, "http://") == 0 || url.compare(0, 8, "https://") == 0;
          if (httpish && url.find('"') == std::string::npos &&
              url.find('\n') == std::string::npos &&
              url.find('\r') == std::string::npos) {
            out += "<a href=\"";
            escapeInto(out, url, 0, url.size());
            out += "\">";
            escapeInto(out, text, i + 1, tclose);
            out += "</a>";
            i = uclose + 1;
            continue;
          }
        }
      }
    }
    // plain character (escaped)
    escapeInto(out, text, i, i + 1);
    i++;
  }
  return out;
}

}  // namespace nimbus
