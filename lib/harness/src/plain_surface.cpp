// plain_surface.cpp - lane E2-P-SURF: the web pages' text for an engine-2 chat (plain/surface.h).
#include "nimbus/harness/plain/surface.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "nimbus/orch/tx/arena.h"
#include "nimbus/orch/tx/digest.h"

namespace agent {
namespace plain {
namespace surface {

namespace tx = nimbus::orch::tx;

namespace {

std::string num(unsigned long long v) { return std::to_string(v); }

std::string clip(const std::string& s, size_t n) {
  if (s.size() <= n) return s;
  size_t k = n;
  while (k > 0 && (static_cast<unsigned char>(s[k]) & 0xC0) == 0x80) --k;   // never inside a UTF-8 character
  return s.substr(0, k) + "... (+" + num(s.size() - k) + " B)";
}

std::string oneLine(const std::string& s, size_t n) {
  std::string t = clip(s, n);
  for (char& c : t)
    if (c == '\n' || c == '\r') c = ' ';
  return t;
}

std::string dateOf(uint64_t epoch) {
  if (!epoch) return std::string();
  const time_t t = static_cast<time_t>(epoch);
  struct tm tmv;
  if (!gmtime_r(&t, &tmv)) return std::string();
  char b[16];
  std::snprintf(b, sizeof b, "%04d-%02d-%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
  return b;
}

// Why a request cannot be rendered again, in a person's words.
std::string whyNot(const Status& st) {
  const std::string w = st.where ? st.where : "";
  if (st.err == tx::Err::NotFound && w.find("rebuildLast") != std::string::npos)
    return "No turn has closed since this device started.";
  if (st.err == tx::Err::NotFound && (w.find(".items") != std::string::npos || w.find(".range") != std::string::npos))
    return "Part of the conversation this request was made from has been summarized since, so it cannot be rendered again.";
  if (st.err == tx::Err::NotFound)
    return "The system text or the tools have changed since this request, so it cannot be rendered again.";
  if (st.err == tx::Err::Torn)
    return "The render differs from what was sent (photo pixels are not kept), so it is not shown as the request.";
  return std::string("It could not be rendered again (") + (w.empty() ? "internal" : w) + ").";
}

}  // namespace

std::string lastTurnText(const PlainEngine& e, size_t round, tx::ArenaAllocFn alloc, tx::ArenaFreeFn release, bool& found) {
  const LastTurn* l = e.lastTurn();
  found = l != nullptr;
  if (!l) return "No turn has run since this device started.\n";
  const size_t n = l->end.exchanges.size();
  std::string out = "Engine 2: the last request of turn " + num(l->ctx.turn) + " (" + l->ctx.host + " " + l->ctx.model +
                    "), rendered again from the stored conversation.\n";
  out += "Turn ended: " + l->end.cause + ", " + num(l->end.rounds) + " round(s), " + num(l->end.tools) + " tool call(s), " +
         num(l->end.headRequests) + " head request(s), " + num(l->end.promptTokens) + " tokens in, " +
         num(l->end.outputTokens) + " out, " + num(l->end.cacheReadTokens) + " from cache.\n";
  if (!l->end.servedModel.empty()) out += "Served by: " + l->end.servedModel + "\n";
  if (!n) return out + "The turn sent no request.\n";
  const size_t pick = round >= 1 && round <= n ? round - 1 : n - 1;
  for (size_t i = 0; i < n; ++i) {
    const tx::ExchangeFacts& x = l->end.exchanges[i];
    out += "  request " + num(i + 1) + ": " + num(x.requestBytes) + " B, " + num(x.ms) + " ms, HTTP " + num(x.httpStatus) +
           (x.bodyHash ? "" : ", no reference kept") + (i == pick ? "  <- shown" : "") + "\n";
  }
  HttpRequest req;
  const Status st = e.rebuildLast(pick, req, alloc, release);
  if (!st.ok()) return out + "\n" + whyNot(st) + "\n";
  out += "\nPOST " + req.path + "  (" + num(req.body.size()) + " B";
  if (!req.splices.empty()) out += ", " + num(req.splices.size()) + " photo part(s) not shown";
  out += ")\n\n";
  const size_t room = out.size() < kTextMax ? kTextMax - out.size() : 0;
  if (req.body.size() > room) {
    out += req.body.substr(0, room);
    out += "\n... (cut: the page shows the first " + num(room) + " B)\n";
  } else {
    out += req.body + "\n";
  }
  return out;
}

std::string turnLogText(const PlainEngine& e, uint64_t row, bool& found) {
  found = false;
  const tx::TxArena* a = e.deps().arena;
  const LastTurn* l = e.lastTurn();
  if (!a) return "This chat holds no conversation in memory.\n";
  uint32_t turn = 0;
  if (row) {
    tx::Item it;
    for (size_t i = 0; i < a->count(); ++i)
      if (a->get(i, it).ok() && it.kind == tx::ItemKind::User && it.m == row) {
        turn = it.turn;
        break;
      }
  } else if (l) {
    turn = l->ctx.turn;
  }
  if (!turn) return row ? "This turn is no longer in memory: it was summarized, or it ran before the device started.\n"
                        : "No turn has run since this device started.\n";
  std::string out = "Engine 2 turn log, turn " + num(turn) + ".\n";
  tx::Item it;
  size_t count = 0;
  for (size_t i = 0; i < a->count(); ++i) {
    if (!a->get(i, it).ok() || it.turn != turn || it.kind == tx::ItemKind::Digest) continue;
    ++count;
    if (out.size() > kTextMax) {
      out += "... (cut)\n";
      break;
    }
    out += "q" + num(it.q) + " " + std::string(1, static_cast<char>(it.kind)) + " ";
    switch (it.kind) {
      case tx::ItemKind::User: out += "message via " + it.ch + ": " + oneLine(it.x, kItemTextMax); break;
      case tx::ItemKind::TurnOpen:
        out += "open by " + it.trigger + ", head " + it.head + ", attempt " + num(it.attempt); break;
      case tx::ItemKind::Assistant:
        out += std::string("round ") + num(it.rd) + (it.fin ? " (final): " : ": ") + oneLine(it.x, kItemTextMax); break;
      case tx::ItemKind::Call: out += it.n + " " + oneLine(it.st.empty() ? it.args : it.st, 400) + " [" + it.id + "]"; break;
      case tx::ItemKind::Intent: out += "intent [" + it.id + "]"; break;
      case tx::ItemKind::Result:
        out += std::string(it.ok ? "ok " : "failed ") + tx::resultClassSlug(it.cls) + " [" + it.id + "] " + num(it.bytes) + " B: " +
               oneLine(it.x.empty() ? it.st : it.x, 400);
        break;
      case tx::ItemKind::RoundMeta: out += "round meta"; break;
      case tx::ItemKind::DeviceNote: out += std::string("device note (") + tx::noteClassSlug(it.note) + "): " + oneLine(it.x, 400); break;
      case tx::ItemKind::PointerMove: out += "stub pointer to q" + num(it.stub) + " (" + it.why + ")"; break;
      case tx::ItemKind::Delivery: out += "delivered " + num(it.msgIds.size()) + " message(s)"; break;
      case tx::ItemKind::TurnEnd: {
        const tx::TurnEndFacts& f = it.end;
        out += "end: " + f.cause + ", " + num(f.rounds) + " round(s), " + num(f.tools) + " call(s), " + num(f.headRequests) +
               " request(s), in " + num(f.promptTokens) + " out " + num(f.outputTokens) + " cache " + num(f.cacheReadTokens) +
               ", " + num(f.microUsd) + " micro-USD, " + num(f.ms) + " ms";
        for (const std::string& ev : f.events) out += "\n    event: " + ev;
        for (size_t k = 0; k < f.exchanges.size(); ++k)
          out += "\n    request " + num(k + 1) + ": " + num(f.exchanges[k].requestBytes) + " B, " + num(f.exchanges[k].ms) +
                 " ms, HTTP " + num(f.exchanges[k].httpStatus);
        break;
      }
      default: break;
    }
    out += "\n";
  }
  found = count > 0;
  return out;
}

std::string foldStatusJson(const PlainEngine* e, const std::string& chat, bool held, uint64_t nowEpoch) {
  auto esc = [](const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
      if (c == '"' || c == '\\') { o += '\\'; o += static_cast<char>(c); }
      else if (c == '\n') o += "\\n";
      else if (c < 0x20) o += ' ';
      else o += static_cast<char>(c);
    }
    return o;
  };
  std::string summary, day;
  uint64_t q = 0, thru = 0, when = 0;
  if (held && e) {
    const std::vector<tx::DigestEntry>* d = const_cast<PlainEngine*>(e)->digests();
    if (d)
      for (const tx::DigestEntry& x : *d)
        if (tx::isSummaryText(x.text) && x.q >= q) { summary = x.text; day = x.day; q = x.q; thru = x.thru; }
    const tx::TxArena* a = e->deps().arena;
    size_t i = 0;
    tx::Item it;
    if (q && a && a->find(q, i).ok() && a->get(i, it).ok()) when = it.t;
  }
  std::string thruDate;
  if (thru && e && e->deps().arena) {
    size_t i = 0;
    tx::Item it;
    if (e->deps().arena->find(thru, i).ok() && e->deps().arena->get(i, it).ok()) thruDate = dateOf(it.t);
  }
  const bool has = !summary.empty();
  const bool fromItem = when != 0;
  if (has && !when && day.size() == 10) {   // the item left memory: the digest's own day, at noon UTC
    // Days from 1970-01-01 (the civil calendar), noon UTC: newlib has no timegm.
    long y = std::atol(day.c_str());
    const long mth = std::atol(day.c_str() + 5), dd = std::atol(day.c_str() + 8);
    y -= mth <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400;
    const long doy = (153 * (mth + (mth > 2 ? -3 : 9)) + 2) / 5 + dd - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long long t = (era * 146097LL + doe - 719468LL) * 86400LL + 12 * 3600;
    if (t > 0) when = static_cast<uint64_t>(t);
  }
  double ageH = 0;
  if (has && when && nowEpoch > when) ageH = static_cast<double>(nowEpoch - when) / 3600.0;
  const std::string date = has ? (fromItem ? dateOf(when) : day) : std::string();
  const char* state = !has ? "none" : ageH > 72 ? "stale" : "fresh";
  std::string lines;
  lines += "\"" + esc(held ? kFoldNote : "This conversation is not loaded on engine 2 now; it loads when it next talks.") + "\"";
  char ageBuf[32];
  std::snprintf(ageBuf, sizeof ageBuf, "%.1f", ageH);
  std::string j = "{\"engine\":2,\"chat\":\"" + esc(chat) + "\",\"chats\":[{\"chat\":\"" + esc(chat) + "\"}],\"state\":\"" +
                  state + "\",\"lastOk\":" + (has ? "true" : "false") + ",\"lastOkDate\":\"" + esc(date) +
                  "\",\"summaryAgeH\":" + ageBuf + ",\"thruDate\":\"" + esc(thruDate) +
                  "\",\"uncovered\":false,\"running\":false,\"queued\":false,\"cooldownS\":0,\"failedCycles\":0,"
                  "\"nextTryInS\":0,\"paidToday\":0,\"cap\":0,\"canRun\":false,\"why\":\"" + esc(kFoldNote) +
                  "\",\"lines\":[" + lines + "],\"summary\":\"" + esc(summary) + "\"}";
  return j;
}

AskBox& askBox() {
  static AskBox box;
  return box;
}

std::string answerAsk(const AskReq& q, const PlainEngine* e, const std::string& residentChat, uint64_t nowEpoch,
                      tx::ArenaAllocFn alloc, tx::ArenaFreeFn release) {
  bool found = false;
  switch (q.kind) {
    case Ask::LastTurn:
      if (!e) return "No chat is held on this device now, so there is no last request to show.\n";
      return lastTurnText(*e, q.round, alloc, release, found);
    case Ask::Trace:
      if (!e) return "No chat is held on this device now, so there is no turn log to show.\n";
      return turnLogText(*e, q.arg, found);
    case Ask::Fold: {
      const std::string chat = q.chat.empty() ? residentChat : q.chat;
      return foldStatusJson(e, chat, e && chat == residentChat, nowEpoch);
    }
    case Ask::None: break;
  }
  return std::string();
}

}  // namespace surface
}  // namespace plain
}  // namespace agent
