#pragma once
// Turn shapes measured on a live device, rebuilt as SYNTHETIC fixtures: the
// structure (field order, markers, close-tag variants, value types and rough
// sizes) is exactly what the provider sent; every word of text is invented
// filler. No personal data is kept.
//
//   leak5ef  - 17 Oct, claude-opus-5, a long reply with a six-question ask: the
//              reply ends with </reply>, then ask and memory blocks closed with
//              </ask> and </memory>, then an unclosed device block.
//   leak613  - 18 Oct, claude-opus-5, the forced final round of a 14-tool turn:
//              </reply>, then every later field as a parameter block closed with
//              </parameter>, then </invoke>.
//   pseudo*  - arguments the head sent when it called session_ops as a TOOL
//              (13-15 Oct and 17-18 Oct): a JSON-encoded string, a list hidden in
//              op, a bare spawn object, a spawn with no task, and an empty object.
#include <ArduinoJson.h>

#include <string>
#include <vector>

namespace turn_shapes {

inline std::string filler(size_t bytes, const char* seed = "The plan covers the next step") {
  std::string out;
  while (out.size() < bytes) {
    if (!out.empty()) out += ' ';
    out += seed;
    out += '.';
  }
  out.resize(bytes);
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

struct Leak {
  std::string json;          // the orch_turn the provider returned
  std::string reply, ask, memory;   // what the repaired turn must carry
};

inline Leak leak5ef() {
  Leak l;
  l.reply = filler(700, "Here is where the research stands");
  l.ask = filler(770, "Which of the six options should come first") + "?";
  l.memory = filler(300, "Owner wants the comparison by Friday");
  ArduinoJson::JsonDocument d;
  d["reply"] = l.reply + "\n</reply>\n<parameter name=\"ask\">" + l.ask + "</ask>\n" +
               "<parameter name=\"memory\">" + l.memory + "</memory>\n" +
               "<parameter name=\"device\">[]";
  serializeJson(d, l.json);
  return l;
}

inline Leak leak613() {
  Leak l;
  l.reply = filler(2070, "The nightly consolidation did not finish");
  l.memory = filler(420, "Dream run failed twice this week");
  l.ask = "";
  const std::string memWrite =
      R"([{"content":")" + filler(400, "Nightly consolidation is failing on the snippet step") +
      R"(","importance":0.85,"permanent":false,"ttl":"weeks"}])";
  const std::string scratch =
      R"({"active":"diagnose the nightly run","short":[")" + filler(300, "check the log") +
      R"(","re-run with a smaller batch"],"mid":[")" + filler(300, "follow the fold size") +
      R"("],"long":[")" + filler(300, "keep the device stable") + R"("]})";
  ArduinoJson::JsonDocument d;
  d["reply"] = l.reply + "\n</reply>\n<parameter name=\"memory\">" + l.memory +
               "</parameter>\n<parameter name=\"ask\"></parameter>\n" +
               "<parameter name=\"device\">[]</parameter>\n" +
               "<parameter name=\"mem_write\">" + memWrite + "</parameter>\n" +
               "<parameter name=\"mem_query\">[]</parameter>\n" +
               "<parameter name=\"session_ops\">[]</parameter>\n" +
               "<parameter name=\"scratchpad\">" + scratch + "</parameter>\n</invoke>\n";
  serializeJson(d, l.json);
  return l;
}

// One pseudo-tool argument shape and the spawn count it must yield.
struct PseudoArgs {
  const char* label;
  std::string args;
  int spawns;
};

inline std::vector<PseudoArgs> pseudoToolShapes() {
  return {
      {"stringified list",
       R"({"session_ops":"[{\"op\":\"spawn\",\"provider\":\"anthropic\",\"name\":\"lineup\",\"task\":\"List the current model lineup.\"}]"})",
       1},
      {"list inside op",
       R"({"op":"[{\"op\":\"spawn\",\"name\":\"rail\",\"task\":\"Check the rail status.\"},{\"op\":\"spawn\",\"name\":\"weather\",\"task\":\"Get the forecast.\"}]"})",
       2},
      {"bare spawn object",
       R"({"op":"spawn","provider":"mistral","name":"cal","task":"Summarise today's calendar."})", 1},
      {"array argument",
       R"([{"op":"spawn","task":"Find three sources on the topic."}])", 1},
      {"spawn without a task", R"({"op":"spawn"})", 0},
      {"empty object", R"({})", 0},
  };
}

}  // namespace turn_shapes
