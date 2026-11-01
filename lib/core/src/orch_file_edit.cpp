#include "nimbus/orch/file_edit.h"

#include <algorithm>

namespace nimbus::orch {

// The caps and the policy shape, before anything is opened: "" when the edit
// may proceed to the scan.
std::string validateEdit(size_t findLen, size_t replaceLen, const EditPolicy& pol) {
  if (findLen == 0 || findLen > kEditFindMaxBytes)
    return "the find text is empty or longer than " +
           std::to_string((unsigned)kEditFindMaxBytes) + " B - edit a smaller span";
  if (replaceLen > kEditReplaceMaxBytes)
    return "the replacement is longer than " +
           std::to_string((unsigned)kEditReplaceMaxBytes) + " B - split the edit";
  if (pol.count < 1 || pol.count > kEditMaxMatches)
    return "count must be between 1 and " + std::to_string(kEditMaxMatches);
  return std::string();
}

EditDecision decideEdit(size_t findLen, size_t replaceLen, const EditPolicy& pol,
                        size_t matches) {
  EditDecision d;
  d.matches = matches;
  if (const std::string bad = validateEdit(findLen, replaceLen, pol); !bad.empty()) {
    d.err = bad;
    return d;
  }
  if (matches == 0) {
    d.err = "no match for the find text - nothing changed";
    return d;
  }
  if (pol.all) {
    if ((int)matches > kEditMaxMatches) {
      d.err = "too many matches (" + std::to_string(matches) + ", the cap is " +
              std::to_string(kEditMaxMatches) + " per call) - make the find text longer";
      return d;
    }
    d.apply = (int)matches;
  } else {
    d.apply = std::min(pol.count, (int)matches);
  }
  d.sizeDelta = ((int64_t)replaceLen - (int64_t)findLen) * (int64_t)d.apply;
  d.ok = true;
  return d;
}

TextEditStream::TextEditStream(const std::string& find, const std::string& replace,
                               bool countOnly)
    : find_(find), replace_(replace), countOnly_(countOnly) {
  if (find_.empty()) find_ = "\n";   // never matches an empty span: the tool refuses it earlier
}

void TextEditStream::scan(std::string& out, bool atEof) {
  if (done_) return;
  for (;;) {
    const size_t pos = carry_.find(find_);
    if (pos == std::string::npos) break;
    if (countOnly_) {
      // The scan pass: count and skip past the span (matches never overlap).
      found_++;
      carry_.erase(0, pos + find_.size());
      continue;
    }
    if (applied_ >= applyLimit_) {
      // The limit is reached: everything left - the pre-match bytes, this
      // match and all that follows - copies through unchanged, and nothing
      // is matched again.
      done_ = true;
      out += carry_;
      carry_.clear();
      return;
    }
    out.append(carry_, 0, pos);
    out += replace_;
    applied_++;
    found_++;
    carry_.erase(0, pos + find_.size());   // the scan resumes after the span
  }
  // No match in the carry: everything but the last (find_.size() - 1) bytes is
  // settled - unless the file has ended, when all of it is. The scan pass
  // emits nothing at all.
  if (countOnly_) {
    if (carry_.size() > find_.size() - 1) carry_.erase(0, carry_.size() - (find_.size() - 1));
    return;
  }
  const size_t keep = atEof ? 0 : find_.size() - 1;
  if (carry_.size() > keep) {
    out.append(carry_, 0, carry_.size() - keep);
    carry_.erase(0, carry_.size() - keep);
  }
}

void TextEditStream::feed(const char* data, size_t len, std::string& out) {
  if (done_) {
    if (!countOnly_) out.append(data, len);
    return;
  }
  carry_.append(data, len);
  scan(out, false);
}

void TextEditStream::finish(std::string& out) {
  if (done_) {
    if (!countOnly_) out += carry_;
    carry_.clear();
    return;
  }
  scan(out, true);
  // A partial span at the end of the file is not a match: emit it as it is -
  // and the scan pass emits nothing at all, its unsettled tail included (the
  // header's contract).
  if (!countOnly_) out += carry_;
  carry_.clear();
}

}  // namespace nimbus::orch
