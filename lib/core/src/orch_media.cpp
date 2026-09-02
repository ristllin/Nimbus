#include "nimbus/orch/media.h"

#include <cstring>

namespace nimbus {
namespace orch {

const char* mediaFormatName(MediaFormat f) {
  switch (f) {
    case MediaFormat::Wav: return "wav";
    case MediaFormat::Mp3: return "mp3";
    case MediaFormat::Unknown: break;
  }
  return "unknown";
}

const char* mediaStateName(MediaState s) {
  switch (s) {
    case MediaState::Playing: return "playing";
    case MediaState::Paused: return "paused";
    case MediaState::Stopped: break;
  }
  return "stopped";
}

static bool endsWithCI(const char* s, const char* suffix) {
  const size_t ls = std::strlen(s), lf = std::strlen(suffix);
  if (lf > ls) return false;
  const char* p = s + (ls - lf);
  for (size_t i = 0; i < lf; i++) {
    char a = p[i], b = suffix[i];
    if (a >= 'A' && a <= 'Z') a = char(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = char(b - 'A' + 'a');
    if (a != b) return false;
  }
  return true;
}

MediaFormat sniffFormat(const uint8_t* head, size_t n, const char* filename) {
  // Content sniff first (authoritative). WAV = "RIFF"...."WAVE"; MP3 = "ID3" tag
  // or an MPEG-audio frame sync (11 set bits: 0xFF 0xEx). Fall back to extension.
  if (head && n >= 12 && std::memcmp(head, "RIFF", 4) == 0 &&
      std::memcmp(head + 8, "WAVE", 4) == 0)
    return MediaFormat::Wav;
  if (head && n >= 3 && std::memcmp(head, "ID3", 3) == 0) return MediaFormat::Mp3;
  if (head && n >= 2 && head[0] == 0xFF && (head[1] & 0xE0) == 0xE0)
    return MediaFormat::Mp3;
  if (filename) {
    if (endsWithCI(filename, ".wav")) return MediaFormat::Wav;
    if (endsWithCI(filename, ".mp3")) return MediaFormat::Mp3;
  }
  return MediaFormat::Unknown;
}

// A single filename-safe character: letters/digits + '.', '_', '-', space. No path
// separators, control chars, or FAT-hostile punctuation. (Kept separate so
// validMusicName stays under the complexity gate.)
static bool isMusicNameChar(char ch) {
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
         (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-' || ch == ' ';
}

bool validMusicName(const char* name) {
  if (!name) return false;
  const size_t len = std::strlen(name);
  if (len < 5 || len > 64) return false;                 // shortest: "a.wav"
  if (!endsWithCI(name, ".wav") && !endsWithCI(name, ".mp3")) return false;
  if (name[0] == '.') return false;                       // no leading dot / hidden
  if (std::strstr(name, "..")) return false;              // no traversal
  for (const char* c = name; *c; c++)
    if (!isMusicNameChar(*c)) return false;               // no '/','\\', control, etc.
  return true;
}

bool musicUploadAllowed(const char* name, bool sdPresent, std::string& err) {
  // No card, no /music: the player reads the SD card, so an upload with no card
  // would land nowhere the player can see. Say so plainly (the web UI shows this).
  if (!sdPresent) { err = "No SD card. Add a card to the device to store music."; return false; }
  // Same gate the player and the media tools use, so an accepted upload is always
  // a track the player will list and play.
  if (!validMusicName(name)) { err = "Use a .wav or .mp3 file name (no folders)."; return false; }
  return true;
}

// ---- queue -----------------------------------------------------------------

static const std::string kEmpty;

const std::string& MediaQueue::current() const {
  if (cur_ < 0 || cur_ >= (int)tracks_.size()) return kEmpty;
  return tracks_[(size_t)cur_];
}

bool MediaQueue::enqueue(const std::string& path) {
  if (path.empty() || (int)tracks_.size() >= kMaxTracks) return false;
  tracks_.push_back(path);
  return true;
}

int MediaQueue::playNow(const std::vector<std::string>& paths) {
  tracks_.clear();
  cur_ = -1;
  state_ = MediaState::Stopped;
  for (const auto& p : paths) {
    if ((int)tracks_.size() >= kMaxTracks) break;
    if (!p.empty()) tracks_.push_back(p);
  }
  if (!tracks_.empty()) {
    cur_ = 0;
    state_ = MediaState::Playing;
  }
  return (int)tracks_.size();
}

bool MediaQueue::play() {
  if (tracks_.empty()) return false;
  if (cur_ < 0) cur_ = 0;
  state_ = MediaState::Playing;
  return true;
}

void MediaQueue::pause() {
  if (state_ == MediaState::Playing) state_ = MediaState::Paused;
}

void MediaQueue::stop() { state_ = MediaState::Stopped; }

void MediaQueue::clear() {
  tracks_.clear();
  cur_ = -1;
  state_ = MediaState::Stopped;
}

bool MediaQueue::next() {
  if (tracks_.empty()) { state_ = MediaState::Stopped; return false; }
  if (cur_ + 1 < (int)tracks_.size()) {
    cur_++;
    state_ = MediaState::Playing;
    return true;
  }
  // At the end.
  if (repeat_) {
    cur_ = 0;
    state_ = MediaState::Playing;
    return true;
  }
  state_ = MediaState::Stopped;   // keep cur_ at the last track
  return false;
}

bool MediaQueue::prev() {
  if (tracks_.empty()) { state_ = MediaState::Stopped; return false; }
  if (cur_ > 0) cur_--;
  else if (repeat_) cur_ = (int)tracks_.size() - 1;
  state_ = MediaState::Playing;
  return true;
}

}  // namespace orch
}  // namespace nimbus
