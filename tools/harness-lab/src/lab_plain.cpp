// lab_plain - the harness lab on engine 2 (lab_plain.h), lane RB-I1a.
#include "lab_plain.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "../../../nimbusd/src/plain_rig.h"
#include "../../../test/support/scripted_provider/scripted_provider.h"
#include "../../../test/support/tx_battery/mem_files.h"
#include "../../../test/support/tx_battery/stub_wire.h"

namespace lab {
namespace {

namespace plain = agent::plain;
namespace sp = harness_test::scripted;

int g_fail = 0;
void check(bool cond, const std::string& what) {
  std::printf("  %s %s\n", cond ? "ok  " : "FAIL", what.c_str());
  if (!cond) g_fail++;
}

// One lab chat on engine 2: a virtual clock (a retry's wait costs no real time), a scripted
// model at the transport, the dialect's wire (the stand-in until its wire lane lands), RAM
// storage.
struct PlainLab {
  PlainLab() {
    platform.nowMs = [this] { return ms; };
    platform.nowEpoch = [this] { return 1794920520ULL + ms / 1000u; };
    platform.delayMs = [this](uint32_t n) { ms += n; };
    platform.freeHeap = [] { return 200000u; };
    platform.allocLarge = [](size_t n) { return std::malloc(n); };
    platform.freeLarge = [](void* p) { std::free(p); };
    model.clock = &ms;
    nimbusd::PlainRigConfig cfg;
    cfg.chatKey = "lab";
    cfg.key = [] { return std::string("sk-fake-lab"); };
    rig.reset(new nimbusd::PlainRig(cfg, nimbusd::PlainRigBindings{&platform, &model, &files,
                                                                    txb::batteryWire(cfg.dialect), nullptr, nullptr}));
    booted = rig->boot().ok();
  }
  agent::Platform platform;
  uint32_t ms = 1000;
  sp::ScriptedProvider model;
  txb::MemFiles files;
  std::unique_ptr<nimbusd::PlainRig> rig;
  bool booted = false;
};

void textTurn() {
  std::puts("engine 2: one scripted text turn");
  PlainLab lab;
  check(lab.booted, "the plain rig boots");
  lab.model.push(sp::say("Hello from the lab."));
  const nimbusd::PlainTurn t = lab.rig->say("hello");
  check(t.status.ok() && t.ending == nimbusd::PlainEnding::Reply, "the turn ends with the model's reply");
  check(t.stated == "Hello from the lab." && t.sent.size() == 1 && t.sent[0] == t.stated,
        "the scripted answer is the reply, sent once and unchanged");
  check(lab.model.requests().size() == 1, "one request");
  std::string why;
  check(lab.model.verify(why), "the script was used exactly");
}

void failureFaces() {
  std::puts("engine 2: the failure faces end with a stated outcome");
  {
    PlainLab lab;
    lab.model.respondWith([](const sp::Recorded&, size_t) { return sp::httpError(503); });
    const nimbusd::PlainTurn t = lab.rig->say("hello");
    check(t.ending == nimbusd::PlainEnding::Status && t.sent.size() == 1 && !t.stated.empty(),
          "a provider error is one plain line");
  }
  {
    PlainLab lab;
    lab.model.respondWith([](const sp::Recorded&, size_t) { return sp::transportCut(120000, "read timed out"); });
    const uint32_t t0 = lab.ms;
    const nimbusd::PlainTurn t = lab.rig->say("hello");
    check(t.ending == nimbusd::PlainEnding::Status && t.sent.size() == 1, "a hung provider is one plain line");
    check(lab.ms - t0 <= plain::kHardWallMs, "inside the hard wall");
  }
  {
    PlainLab lab;
    lab.model.push(sp::say(""));
    const nimbusd::PlainTurn t = lab.rig->say("hello");
    check(t.status.ok() && !t.stated.empty() && !t.out.delivered, "an empty answer is stated, never a crash");
  }
}

}  // namespace

int cmdPlainSelftest() {
  g_fail = 0;
  textTurn();
  failureFaces();
  std::printf("%s\n", g_fail ? "engine 2 selftest FAILED" : "engine 2 selftest passed");
  return g_fail ? 1 : 0;
}

}  // namespace lab
