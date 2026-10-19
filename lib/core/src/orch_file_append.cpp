#include "nimbus/orch/file_append.h"

namespace nimbus::orch {

AppendPlan planAppend(const std::string& text, size_t fileBytes, char lastByte) {
  AppendPlan p;
  std::string body;
  body.reserve(text.size() + 2);
  for (size_t i = 0; i < text.size(); i++) {
    if (text[i] == '\r') {
      body += '\n';
      if (i + 1 < text.size() && text[i + 1] == '\n') i++;
    } else {
      body += text[i];
    }
  }
  if (body.find_first_not_of(" \t\n") == std::string::npos) {
    p.err = "need 'text': one or more rows to add";
    return p;
  }
  if (body.back() != '\n') body += '\n';
  if (body.size() > kAppendMaxBytes) {   // counted as written, final line end included
    p.err = "text too large for one append (max " + std::to_string((unsigned)kAppendMaxBytes) +
            " B) - add fewer rows per call";
    return p;
  }
  if (fileBytes > 0 && lastByte != '\n') {
    body.insert(body.begin(), '\n');
    p.repairedCut = true;
  }
  for (char c : body)
    if (c == '\n') p.rows++;
  if (p.repairedCut) p.rows--;   // the leading line end closes the cut row
  p.bytes = std::move(body);
  p.ok = true;
  return p;
}

TailView tailRows(const std::string& window, size_t windowStart, size_t n) {
  TailView v;
  if (n == 0) n = kTailDefaultRows;
  if (n > kTailMaxRows) n = kTailMaxRows;
  size_t begin = 0;
  if (windowStart > 0) {
    const size_t nl = window.find('\n');
    begin = nl == std::string::npos ? window.size() : nl + 1;   // drop the partial row
    v.earlier = true;
  }
  std::vector<std::string> all;
  size_t pos = begin;
  while (pos < window.size()) {
    size_t nl = window.find('\n', pos);
    if (nl == std::string::npos) {
      all.push_back(window.substr(pos));
      v.lastCut = true;
      break;
    }
    all.push_back(window.substr(pos, nl - pos));
    pos = nl + 1;
  }
  if (all.size() > n) {
    v.earlier = true;
    all.erase(all.begin(), all.end() - (long)n);
  }
  v.rows = std::move(all);
  return v;
}

std::string renderTail(const TailView& v, const std::string& name, size_t fileBytes,
                       const std::string& headerRow) {
  std::string out = "last " + std::to_string(v.rows.size()) + " row" + (v.rows.size() == 1 ? "" : "s") +
                    " of " + name + " (" + std::to_string((unsigned)fileBytes) + " B" +
                    (v.earlier ? "; earlier rows not shown" : "; the whole file") + ")\n";
  if (!headerRow.empty() && v.earlier) out += "header: " + headerRow + "\n";
  for (const auto& r : v.rows) out += r + "\n";
  if (v.lastCut)
    out += "[the last row is incomplete - the file was cut mid-write; the next "
           "files.append starts on a new line]\n";
  return out;
}

}  // namespace nimbus::orch
