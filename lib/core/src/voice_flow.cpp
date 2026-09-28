#include "nimbus/voice_flow.h"

#include <cstdlib>

namespace nimbus::voice {

namespace {

using solide::ring::RGB;

std::string trimmed(const std::string& s) {
  const size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return std::string();
  const size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

// "<Provider> error" when the provider is known; the title must fit the ring
// center, so an unknown slug gets the short generic form.
std::string errorTitle(const std::string& slug) {
  if (slug == "mistral" || slug == "openai" || slug == "cumulo")
    return providerName(slug) + " error";
  return "Voice error";
}

std::string httpDetail(int http) {
  const std::string code = std::to_string(http);
  if (http == 0) return "No answer from speech-to-text. Try again.";
  if (http == 401 || http == 403)
    return "Key rejected (HTTP " + code + "). Check it in the web app.";
  if (http == 429) return "Too many requests (HTTP 429). Wait a minute.";
  return "Speech-to-text HTTP " + code + ". Try again.";
}

RGB scaled(RGB c, uint8_t lvl) {
  return RGB{uint8_t(c.r * lvl / 255), uint8_t(c.g * lvl / 255), uint8_t(c.b * lvl / 255)};
}

void fill(RGB c, RGB* out, int n) {
  for (int i = 0; i < n; ++i) out[i] = c;
}

// Processing comet: a bright head sweeping once per kSpinMs, a fading tail, and a
// dim floor so the whole ring still reads as "busy" rather than dark.
constexpr int kTail = 10;
constexpr int kFloor = 20;

void comet(uint32_t elapsedMs, RGB accent, RGB* out, int n) {
  const int head = int((elapsedMs % kSpinMs) * uint32_t(n) / kSpinMs);
  for (int i = 0; i < n; ++i) {
    const int d = (head - i + n) % n;   // LEDs behind the head
    const int lvl = d < kTail ? 255 - d * (255 - kFloor) / kTail : kFloor;
    out[i] = scaled(accent, uint8_t(lvl));
  }
}

}  // namespace

SttResult::Kind sttKindForError(const std::string& err, int* httpOut) {
  if (httpOut) *httpOut = 0;
  if (err == "connect failed") return SttResult::Kind::NoNetwork;
  if (err == "tls arbiter busy") return SttResult::Kind::Busy;
  if (err == "file open failed") return SttResult::Kind::NoAudio;
  if (err.rfind("HTTP ", 0) == 0 && httpOut) *httpOut = std::atoi(err.c_str() + 5);
  return SttResult::Kind::Http;
}

Outcome classify(const SttResult& r) {
  switch (r.kind) {
    case SttResult::Kind::Ok:
      return trimmed(r.text).empty() ? Outcome::EmptyTranscript : Outcome::None;
    case SttResult::Kind::NoNetwork: return Outcome::NoNetwork;
    case SttResult::Kind::Http:      return Outcome::SttHttp;
    case SttResult::Kind::Refused:   return Outcome::SttRefused;
    case SttResult::Kind::BadReply:  return Outcome::SttBadReply;
    case SttResult::Kind::Busy:      return Outcome::Busy;
    case SttResult::Kind::NoAudio:   return Outcome::NoAudio;
  }
  return Outcome::SttHttp;   // unreachable; an unknown kind is never "empty"
}

std::string providerName(const std::string& slug) {
  if (slug == "mistral") return "Mistral";
  if (slug == "openai") return "OpenAI";
  if (slug == "cumulo") return "Cumulo";
  return "Speech-to-text";
}

Line lineFor(Outcome o, const SttResult& r) {
  switch (o) {
    case Outcome::None:            return {};
    case Outcome::NoNetwork:       return {"No network", "Check Wi-Fi and try again."};
    case Outcome::SttHttp:         return {errorTitle(r.provider), httpDetail(r.http)};
    case Outcome::SttRefused:
      return {"Voice unavailable",
              r.refusal.empty() ? std::string("Try again soon.") : r.refusal};
    case Outcome::SttBadReply:
      return {errorTitle(r.provider), "Unreadable speech-to-text reply. Try again."};
    case Outcome::Busy:
      return {"Busy", "Another request is running. Try again in a moment."};
    case Outcome::NoAudio:         return {"No audio", "The mic recorded nothing. Try again."};
    case Outcome::EmptyTranscript: return {"Didn't catch that", "Hold the mic button and speak."};
    case Outcome::TurnError:
      return {"No answer", "The assistant could not finish. Try again."};
    case Outcome::NoReply:         return {"No reply", "Nothing came back. Try again."};
  }
  return {};
}

Line lineFor(Outcome o) { return lineFor(o, SttResult{}); }

std::string sentence(const Line& l) {
  if (l.detail.empty()) return l.title;
  if (l.title.empty()) return l.detail;
  return l.title + ". " + l.detail;
}

Cue cueFor(Outcome o) {
  switch (o) {
    case Outcome::None:
    case Outcome::EmptyTranscript: return Cue::None;   // nothing went wrong: no alarm
    case Outcome::NoNetwork:       return Cue::Offline;
    default:                       return Cue::Alert;
  }
}

bool sfxFor(Outcome o, sfx::Ev& out) {
  if (o == Outcome::None || o == Outcome::EmptyTranscript) return false;
  out = sfx::Ev::Error;   // voiced from the Light level up, in both modes
  return true;
}

std::vector<std::string> wrap(const std::string& s, size_t maxChars, size_t maxLines) {
  std::vector<std::string> out;
  if (maxChars == 0 || maxLines == 0) return out;
  std::string cur;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && s[i] == ' ') ++i;
    size_t j = i;
    while (j < s.size() && s[j] != ' ') ++j;
    std::string w = s.substr(i, j - i);
    i = j;
    if (w.empty()) continue;
    while (w.size() > maxChars) {   // hard-split an over-long word
      if (!cur.empty()) { out.push_back(cur); cur.clear(); }
      out.push_back(w.substr(0, maxChars));
      w = w.substr(maxChars);
    }
    if (cur.empty()) cur = w;
    else if (cur.size() + 1 + w.size() <= maxChars) cur += " " + w;
    else { out.push_back(cur); cur = w; }
  }
  if (!cur.empty()) out.push_back(cur);
  if (out.size() > maxLines) {
    out.resize(maxLines);
    std::string& last = out.back();
    if (last.size() + 3 > maxChars) last.resize(maxChars >= 3 ? maxChars - 3 : 0);
    last += "...";
  }
  return out;
}

void cueFrame(Cue c, uint32_t elapsedMs, RGB accent, RGB alert, RGB* out, int n) {
  if (!out || n <= 0) return;
  switch (c) {
    case Cue::None:       fill(RGB{0, 0, 0}, out, n); return;
    case Cue::Listening:  fill(accent, out, n); return;
    case Cue::Alert:      fill(alert, out, n); return;
    case Cue::Offline:
      fill(scaled(alert, solide::ring::breatheLevel(elapsedMs, kOfflineBreatheMs)), out, n);
      return;
    case Cue::Processing: comet(elapsedMs, accent, out, n); return;
  }
}

// ---- Flow -------------------------------------------------------------------

bool Flow::go(Phase p, uint32_t now) {
  if (p == phase_) return false;
  phase_ = p;
  since_ = now;
  ++transitions_;
  if (p == Phase::Idle) line_ = Line{};
  return true;
}

Cue Flow::cue() const {
  switch (phase_) {
    case Phase::Recording:    return Cue::Listening;
    case Phase::Transcribing:
    case Phase::Thinking:     return Cue::Processing;
    case Phase::Notice:       return cueFor(outcome_);
    case Phase::Idle:         break;
  }
  return Cue::None;
}

bool Flow::press(uint32_t now) {
  if (!canPress()) return false;
  outcome_ = Outcome::None;
  replyShown_ = false;
  line_ = Line{"Listening", "Release to send."};
  return go(Phase::Recording, now);
}

bool Flow::release(uint32_t now) {
  if (phase_ != Phase::Recording) return false;
  line_ = Line{"Transcribing", "One moment."};
  return go(Phase::Transcribing, now);
}

bool Flow::transcribed(const std::string& heard, uint32_t now) {
  if (phase_ != Phase::Transcribing) return false;
  line_ = Line{"Thinking", "You: " + trimmed(heard)};
  lastBusy_ = now;
  return go(Phase::Thinking, now);
}

bool Flow::fail(Outcome o, const Line& line, uint32_t now) {
  if (o == Outcome::None || !busy()) return false;
  outcome_ = o;
  line_ = line;
  return go(Phase::Notice, now);
}

bool Flow::replyLanded(bool turnInFlight, uint32_t now) {
  if (phase_ != Phase::Thinking) return false;
  replyShown_ = true;
  lastBusy_ = now;
  if (turnInFlight) return false;   // still working: a mid-turn notice is not the end
  outcome_ = Outcome::None;
  return go(Phase::Idle, now);
}

bool Flow::turnEnded(bool ok, uint32_t now) {
  if (phase_ != Phase::Thinking) return false;
  if (ok) {
    outcome_ = Outcome::None;
    return go(Phase::Idle, now);
  }
  outcome_ = Outcome::TurnError;
  line_ = lineFor(Outcome::TurnError);
  return go(Phase::Notice, now);
}

bool Flow::tick(bool turnInFlight, uint32_t now) {
  if (phase_ == Phase::Notice)
    return (now - since_ >= kNoticeHoldMs) ? go(Phase::Idle, now) : false;
  if (phase_ != Phase::Thinking) return false;
  if (turnInFlight) lastBusy_ = now;
  const bool quiet = !turnInFlight && now - lastBusy_ >= kQuietMs;
  const bool ceiling = now - since_ >= kMaxThinkMs;
  if (!quiet && !ceiling) return false;
  if (replyShown_) {   // the answer is already on screen; just stop processing
    outcome_ = Outcome::None;
    return go(Phase::Idle, now);
  }
  outcome_ = Outcome::NoReply;
  line_ = lineFor(Outcome::NoReply);
  return go(Phase::Notice, now);
}

bool Flow::dismiss(uint32_t now) {
  if (phase_ != Phase::Notice) return false;
  return go(Phase::Idle, now);
}

// ---- the release path -------------------------------------------------------

namespace {
Outcome finish(Flow& f, Port& p, Outcome o, const SttResult& r) {
  f.fail(o, lineFor(o, r), p.now());
  sfx::Ev e;
  if (sfxFor(o, e)) p.sound(e);
  p.show(f);
  return o;
}
}  // namespace

Outcome afterRelease(Flow& f, Port& p) {
  if (!f.release(p.now())) return Outcome::None;   // not recording: nothing to run
  p.show(f);                  // processing is on the ring + screen BEFORE any I/O
  if (!p.linkUp()) {          // cached association state - no network call
    SttResult r;
    r.kind = SttResult::Kind::NoNetwork;
    return finish(f, p, Outcome::NoNetwork, r);
  }
  p.sound(sfx::Ev::VoiceStop);   // capture confirmed
  const SttResult r = p.transcribe();
  const Outcome o = classify(r);
  if (o != Outcome::None) return finish(f, p, o, r);
  const std::string text = trimmed(r.text);
  f.transcribed(text, p.now());
  p.show(f);
  if (!p.sendTurn(text)) return finish(f, p, Outcome::Busy, r);
  return Outcome::None;
}

}  // namespace nimbus::voice
