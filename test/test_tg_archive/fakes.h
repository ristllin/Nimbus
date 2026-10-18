#pragma once
// In-memory card and Telegram file source for the archive tests. The card counts
// every overwrite of a finished file (a naming collision) and can fail any write;
// the source serves files in random slice sizes, honors or ignores a resume, and
// can cut a transfer at any byte.
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "nimbus/tg_archive.h"

namespace fakes {

using nimbus::tgarc::Entry;

inline bool endsWith(const std::string& s, const char* suf) {
  const size_t n = std::char_traits<char>::length(suf);
  return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}

struct Card : nimbus::tgarc::Fs {
  std::map<std::string, std::string> files;
  std::set<std::string> dirs;
  int violations = 0;     // a finished file overwritten or renamed onto
  int writeCalls = 0;
  size_t maxWriteLen = 0;  // the largest single write (proves slice-sized writes)
  bool dead = false;       // every operation fails (card absent)
  // Return true to fail this write (path, offset in file, length).
  std::function<bool(const std::string&, size_t, size_t)> failWrite;
  std::vector<std::string> removed;

  static bool temp(const std::string& p) { return endsWith(p, ".tmp") || endsWith(p, ".part"); }

  struct W : nimbus::tgarc::Writer {
    Card* c;
    std::string path;
    bool open = true;
    bool write(const uint8_t* d, size_t n) override {
      if (!open || c->dead) return false;
      c->writeCalls++;
      if (n > c->maxWriteLen) c->maxWriteLen = n;
      std::string& f = c->files[path];
      if (c->failWrite && c->failWrite(path, f.size(), n)) return false;
      f.append((const char*)d, n);
      return true;
    }
    bool close() override {
      const bool was = open;
      open = false;
      return was && !c->dead;
    }
  };
  struct R : nimbus::tgarc::Reader {
    std::string data;
    size_t at = 0;
    int read(uint8_t* d, size_t cap) override {
      const size_t n = std::min(cap, data.size() - at);
      std::copy(data.begin() + (long)at, data.begin() + (long)(at + n), d);
      at += n;
      return (int)n;
    }
  };

  bool exists(const std::string& p) override { return !dead && (files.count(p) || dirs.count(p)); }
  bool mkdirs(const std::string& dir) override {
    if (dead) return false;
    for (size_t i = 1; i <= dir.size(); ++i)
      if (i == dir.size() || dir[i] == '/') dirs.insert(dir.substr(0, i));
    return true;
  }
  std::unique_ptr<nimbus::tgarc::Writer> openWrite(const std::string& p, bool append) override {
    if (dead) return nullptr;
    const size_t slash = p.find_last_of('/');
    if (slash != std::string::npos && slash > 0 && !dirs.count(p.substr(0, slash))) return nullptr;
    if (!append && files.count(p) && !temp(p)) violations++;
    if (!append) files[p].clear();
    else files[p];
    auto w = std::make_unique<W>();
    w->c = this;
    w->path = p;
    return w;
  }
  std::unique_ptr<nimbus::tgarc::Reader> openRead(const std::string& p) override {
    if (dead || !files.count(p)) return nullptr;
    auto r = std::make_unique<R>();
    r->data = files[p];
    return r;
  }
  bool rename(const std::string& from, const std::string& to) override {
    if (dead || !files.count(from)) return false;
    if (files.count(to)) violations++;
    files[to] = files[from];
    files.erase(from);
    return true;
  }
  bool remove(const std::string& p) override {
    if (dead) return false;
    removed.push_back(p);
    return files.erase(p) > 0;
  }
  bool removeTree(const std::string& dir) override {
    if (dead) return false;
    const std::string pre = dir + "/";
    for (auto it = files.begin(); it != files.end();)
      if (it->first.compare(0, pre.size(), pre) == 0) { removed.push_back(it->first); it = files.erase(it); }
      else ++it;
    for (auto it = dirs.begin(); it != dirs.end();)
      if (*it == dir || it->compare(0, pre.size(), pre) == 0) it = dirs.erase(it);
      else ++it;
    return true;
  }
  int64_t size(const std::string& p) override {
    auto it = files.find(p);
    return (dead || it == files.end()) ? -1 : (int64_t)it->second.size();
  }
  std::vector<Entry> list(const std::string& dir) override {
    std::vector<Entry> out;
    if (dead) return out;
    const std::string pre = dir + "/";
    for (const auto& kv : files) {
      if (kv.first.compare(0, pre.size(), pre) != 0) continue;
      const std::string rest = kv.first.substr(pre.size());
      if (rest.find('/') == std::string::npos) out.push_back({rest, false, kv.second.size()});
    }
    for (const auto& d : dirs) {
      if (d.compare(0, pre.size(), pre) != 0) continue;
      const std::string rest = d.substr(pre.size());
      if (!rest.empty() && rest.find('/') == std::string::npos) out.push_back({rest, true, 0});
    }
    return out;
  }
  // Every line of an index file.
  std::vector<std::string> lines(const std::string& p) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : files[p]) {
      if (ch == '\n') { out.push_back(cur); cur.clear(); }
      else cur.push_back(ch);
    }
    return out;
  }
  size_t countUnder(const std::string& prefix) const {
    size_t n = 0;
    for (const auto& kv : files)
      if (kv.first.compare(0, prefix.size(), prefix) == 0) ++n;
    return n;
  }
};

struct TgSource : nimbus::tgarc::Source {
  std::map<std::string, std::string> files;   // fileId -> bytes
  bool supportsResume = true;
  bool reportTotal = true;
  long failAtByte = -1;      // cut the next transfer once it has served this many bytes (from 0)
  int failOpens = 0;         // the next N opens fail (transient)
  std::set<std::string> gone;   // permanent: Telegram no longer has the file
  std::mt19937 rng{7};
  int opens = 0;
  uint64_t lastResumeAsk = 0;

  std::string cur;
  size_t at = 0;
  bool open_ = false;

  nimbus::tgarc::OpenResult open(const std::string& id, uint64_t resumeAt) override {
    nimbus::tgarc::OpenResult r;
    opens++;
    lastResumeAsk = resumeAt;
    if (gone.count(id)) { r.permanent = true; r.err = "file not found"; return r; }
    if (failOpens > 0) { failOpens--; r.err = "connect failed"; return r; }
    auto it = files.find(id);
    if (it == files.end()) { r.permanent = true; r.err = "file not found"; return r; }
    cur = it->second;
    at = 0;
    r.ok = true;
    if (resumeAt && supportsResume && resumeAt <= cur.size()) {
      at = (size_t)resumeAt;
      r.resumed = true;
    }
    if (reportTotal) r.total = (int64_t)(cur.size() - at);
    open_ = true;
    return r;
  }
  int read(uint8_t* buf, size_t cap) override {
    if (!open_) return -1;
    if (failAtByte >= 0 && (long)at >= failAtByte) { failAtByte = -1; return -1; }
    std::uniform_int_distribution<size_t> d(1, cap);
    size_t n = std::min(d(rng), cur.size() - at);
    if (failAtByte >= 0 && (long)(at + n) > failAtByte) n = (size_t)(failAtByte - (long)at);
    std::copy(cur.begin() + (long)at, cur.begin() + (long)(at + n), buf);
    at += n;
    return (int)n;
  }
  void close() override { open_ = false; }
};

inline std::string bytes(size_t n, uint32_t seed) {
  std::string s(n, '\0');
  std::mt19937 r(seed);
  for (size_t i = 0; i < n; ++i) s[i] = (char)(r() & 0xFF);
  return s;
}

}  // namespace fakes
