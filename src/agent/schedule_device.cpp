#include "schedule_device.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <time.h>

#include <new>

#include "fs_atomic.h"                       // agent::writeFileAtomic
#include "loops_subsystem.h"                 // the table, its lock and its clock math (engine-2 view)
#include "memory_subsystem.h"                // the card lock, dataFs, haveSd (a deleted routine's state)
#include "nimbus/harness/plain/request.h"    // toolDecl, declSchema
#include "nimbus/harness/turn_memory.h"      // PsramFirstScope

namespace agent::scheduledev {

namespace orch = nimbus::orch;
namespace sch = nimbus::orch::schedule;

namespace {

constexpr char kSidecarPath[] = "/data/loops2.json";

class DeviceHost : public sch::Host {
 public:
  void withTable(const std::function<void(std::vector<orch::LoopRecord>&)>& fn) override { loops::withTable(fn); }
  bool saveLoops(const std::vector<orch::LoopRecord>& v) override { return loops::saveTable(v); }
  bool readSidecar(std::string& out) override {
    out.clear();
    if (!flashMounted()) return false;
    if (!LittleFS.exists(kSidecarPath)) return true;
    File f = LittleFS.open(kSidecarPath, FILE_READ);
    if (!f) return false;
    String s = f.readString();
    f.close();
    out.assign(s.c_str(), s.length());
    return true;
  }
  bool writeSidecar(const std::string& json) override {
    LittleFS.mkdir("/data");
    return writeFileAtomic(kSidecarPath, json, "loops2", false);
  }
  sch::Clock clock() override { return sch::Clock{loops::wallClockValid(), (uint64_t)time(nullptr)}; }
  bool localToEpoch(const orch::CivilTime& c, uint64_t& out) override {
    out = loops::epochOfLocal(c);
    return out > 0;
  }
  orch::CivilTime epochToLocal(uint64_t e) override { return loops::localOf(e); }
  uint64_t nextRun(const orch::LoopRecord& l, uint64_t now) override { return loops::firstRun(l, now); }
  std::string newId(const std::vector<orch::LoopRecord>& v) override { return loops::freshId(v); }
  bool askFirst() override { return loops::wakeupsAskFirst(); }
  bool reserved(const std::string& id) override { return loops::isReservedId(String(id.c_str())); }
  int maxCount() override { return loops::maxCount(); }
  uint32_t minIntervalSec() override { return loops::minIntervalSec(); }
  // The card's state file and its temp, under the card lock (no card: nothing to remove).
  void forgetState(const std::string& id) override {
    if (!memory::haveSd()) return;
    memory::Lock g;
    fs::FS& fs = memory::dataFs();
    const std::string path = sch::statePath(id);
    fs.remove(path.c_str());
    fs.remove((path + sch::store::kTmpExt).c_str());
  }
};

DeviceHost& deviceHost() {
  static DeviceHost h;
  return h;
}

}  // namespace

sch::Host& host() { return deviceHost(); }

void forget(const std::string& id) { deviceHost().forgetState(id); }

void registerSchedule(orch::ToolRegistry& reg, orch::notes::OriginFn origin, ChatFn chat) {
  const auto* decl = agent::plain::toolDecl("schedule");
  if (!decl) return;
  orch::notes::OriginFn from =
      origin ? std::move(origin) : orch::notes::OriginFn([] { return orch::notes::Origin::Unattended; });
  reg.add(
      "schedule", decl->description,
      [from, chat](ArduinoJson::JsonObjectConst args, const orch::Principal&) {
        const PsramFirstScope psram;
        sch::CallCtx ctx;
        ctx.origin = from();
        ctx.chatId = chat ? chat() : std::string();
        return sch::call(deviceHost(), args, ctx);   // a refused allocation is its error result
      },
      agent::plain::declSchema(*decl, false));
}

orch::FireOutcome fire(const orch::LoopFireRequest& req, const sch::FireBinding& b) {
  const PsramFirstScope psram;
  try {
    return sch::fire(deviceHost(), req, b, (uint64_t)time(nullptr));
  } catch (const std::bad_alloc&) {
    orch::FireOutcome o;
    o.deferred = true;   // never ran: not counted, tried again soon
    o.retryAfterSec = orch::kWakeupRetrySec;
    return o;
  }
}

}  // namespace agent::scheduledev
