#include "nimbus/harness/reply_overflow.h"

namespace agent {
namespace providers {

namespace {
struct Overflow {
  std::string text;
  size_t dropped = 0;
  bool on = false;
};
Overflow& store() {
  static Overflow o;
  return o;
}

void keep(const char* s, size_t n) {
  Overflow& o = store();
  const size_t room = o.text.size() < kReplyOverflowMax ? kReplyOverflowMax - o.text.size() : 0;
  const size_t take = n < room ? n : room;
  o.text.append(s, take);
  o.dropped += n - take;
}
}  // namespace

void replyOverflowReset() {
  Overflow& o = store();
  std::string().swap(o.text);   // give the memory back, not just the length
  o.dropped = 0;
  o.on = false;
}

void replyOverflowStart(const char* head, size_t headLen) {
  replyOverflowReset();
  store().on = true;
  keep(head, headLen);
}

void replyOverflowAppend(const char* s, size_t n) {
  if (store().on) keep(s, n);
}

std::string takeReplyOverflow(size_t* dropped) {
  Overflow& o = store();
  std::string out;
  out.swap(o.text);
  if (dropped) *dropped = o.dropped;
  o.dropped = 0;
  o.on = false;
  return out;
}

}  // namespace providers
}  // namespace agent
