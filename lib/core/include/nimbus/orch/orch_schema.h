#pragma once
// The Head-Orchestrator turn contract - SINGLE SOURCE OF TRUTH.
//
// Everything the model is told about the orch_turn fields derives from the
// ORCH_D_* description macros below: they are embedded in the JSON schema
// (ORCH_SCHEMA_BODY, sent to every provider's structured-output path) AND
// compose the prompt's field-doc block (ORCH_FIELD_DOCS). Never hand-write
// field docs in a prompt - edit the macro here and both stay in lockstep.
// (Repo convention: never ask for JSON in prose; schemas ride the provider's
// structured-output mode - OpenAI text.format json_schema, Anthropic forced
// tool_use input_schema, Mistral response_format.json_schema.)
//
// STRICT-COMPATIBLE: OpenAI strict json_schema requires every object to carry
// additionalProperties:false with EVERY property listed in required; optional
// fields are expressed as nullable unions (["integer","null"]). device[] is a
// DISCRIMINATED UNION (anyOf on a "type" const) - the old heterogeneous
// {"led":{...}} / {"lights":"off"} shape could not satisfy strict mode, which
// is why the schema was never actually sent before. The validator
// (orch_device_actions.cpp) accepts BOTH shapes during migration.
//
// "Strict at the wire, lenient at the parser" still holds: parseTurn()
// tolerates missing arrays and null optionals (a provider may null them out).
//
// Description text rules: printable ASCII, NO double quotes (they are embedded
// inside a JSON string inside a C string).

namespace nimbus {
namespace orch {

// ---- field descriptions (the single source) ---------------------------------
#define ORCH_D_REPLY \
  "Text to send the owner now; empty string if none. Report ONLY what actually " \
  "happened: never claim an action (email sent, doc shared, page/issue created, " \
  "message spoken aloud, file sent to Telegram) succeeded unless a tool RESULT " \
  "confirms it - quote the tool's real id/result. If a tool only drafted, or " \
  "created-without-sharing, or returned an error, say exactly that. Delivery is " \
  "a tool result, not something the reply text makes true: writing 'sent to your " \
  "Telegram' or 'reading it aloud now' does NOT send or speak it - the matching " \
  "tool (files send, tts) does, and only its result lets you claim it happened. " \
  "Never invent ids, links, or successes."
#define ORCH_D_ASK \
  "A question for the owner when you need their input; empty string if none."
#define ORCH_D_SCRATCH \
  "Your PERSISTENT working notes across turns (survives reboot), in tiers: " \
  "active (the one thing you are doing right NOW), short (this task's steps / a " \
  "checklist), mid (threads to return to over days), long (standing goals). " \
  "Return this object to UPDATE it: a non-null tier REPLACES that tier's items " \
  "(<=8 each), a non-null active sets the active line, null leaves a field " \
  "unchanged; return the WHOLE field null when nothing changed. This is a FREE " \
  "write (no tool round). Unlike memory (next turn only), the scratchpad " \
  "persists - when you START a multi-step task or fan-out, write the plan here " \
  "and tick items off as they finish, so you never lose the thread across turns."
#define ORCH_D_MEMORY \
  "Your SHORT-LIVED working notes for the next turn only (active threads, " \
  "in-progress state). NOT long-term storage: any durable fact - a preference, " \
  "a name, anything the owner asks you to remember - goes in mem_write[] " \
  "instead, where it is embedded and recalled in future conversations. Keep " \
  "this brief and current; return it when it changed, else empty."
#define ORCH_D_DEVICE \
  "Local device actions, each discriminated by its type field: led paints a " \
  "ring pattern (mode solid|spinner|pulse|flash|rainbow, r/g/b 0-255 - rainbow " \
  "is a self-animating hue wheel and ignores r/g/b; brightness " \
  "0-255 or null; EVERY led mode is a LIT pattern - to turn the ring OFF use " \
  "the lights action, NOT led); lights turns the ring off or back to full " \
  "(value off|full; ALWAYS use lights:off to turn the ring off, including to " \
  "undo an led pattern; off is " \
  "overridden by any new needs-you event, a click, or lights full); tts " \
  "voices the text to your owner as a Telegram audio message; to speak aloud on " \
  "the device's own built-in speaker instead, use the reply.speak tool; reboot " \
  "restarts the device; config tunes owner-friendly " \
  "knobs - ledBrightness 0-255, priority (comma-separated sub-agent provider " \
  "preference), posture (ring level dark|calm|full), profile (power profile " \
  "battery_saver|balanced|desk), theme (LED colour theme slug: teal, ocean, " \
  "ember, forest, openai, anthropic, mistral, rainbow), attnHoldMs (how long " \
  "a needs-you cue holds, 30000-1800000 ms), ttsVoice (your speaking voice: a " \
  "provider-specific voice id/slug, empty string resets to default), " \
  "ttsProv (which provider voices you, mistral|openai), ttsOn (voice replies " \
  "master switch - set true/false ONLY when the owner explicitly asks for " \
  "spoken replies on or off), sttProv (voice transcription provider, " \
  "mistral|openai), sfxLvlN + sfxLvlO (sound-effect intensity 0-3 for " \
  "Notifier / Orchestrator mode, 0 silent), sfxVol (speaker volume 0-100), " \
  "sfxTheme (sound theme, currently pulse), sleepOvr (RISK: true disables " \
  "the low-battery deep-sleep protection so the battery can discharge below its " \
  "safe floor; a deep-discharged cell may no longer recharge normally and can " \
  "require manual bench recovery or replacement - set only when the owner " \
  "knowingly accepts that, e.g. for a measurement run, ALWAYS state the risk in " \
  "your reply, and set false again when done), brightOvr (RISK: true lifts the " \
  "LED cap from 60% to 100%; sustained high brightness can overheat the device, " \
  "damaging internal components and potentially deforming or melting the outer " \
  "shell - set only with explicit owner consent, ALWAYS state the risk in your " \
  "reply, prefer setting it back to false afterwards) and devName (the device's " \
  "name on the network + screen; takes effect after a reboot). Set only the " \
  "knobs you mean to change; null leaves a knob untouched. orch_model switches YOUR " \
  "OWN provider host + model at runtime (provider openai|anthropic|mistral, model " \
  "an id from that provider's AVAILABLE MODELS) - use it when asked to change your " \
  "model, e.g. 'switch to opus'. Secrets, API keys, and provider PRIORITY stay " \
  "BLOCKED - only the owner can change those, from the device web page."
#define ORCH_D_MEMW \
  "Durable facts to store in long-term memory (embedded + saved) so you can " \
  "recall them in future conversations. importance 0-1. ttl is how long the " \
  "fact should live - session|days|weeks|months|permanent (default weeks); pick " \
  "honestly: a meeting time is 'days', a stable preference is 'months', a name " \
  "is 'permanent'. permanent:true additionally pins it so it never decays. A " \
  "write here applies AFTER this reply is sent, so 'I saved it' in the reply is " \
  "a prediction - phrase it as intent ('I'll remember that') or confirm it a " \
  "later turn, never as a completed fact this turn."
#define ORCH_D_MEMQ \
  "Search strings for facts NOT already shown in RELEVANT MEMORIES; results " \
  "arrive next turn as MEMORY RESULTS."
#define ORCH_D_SOPS \
  "Manage sub-agents: op spawn starts one (task/provider/model), op terminate " \
  "stops the session named by id. There is no live back-and-forth with a " \
  "running sub-agent - spawn, then read FRESH RESULTS. Optional spawn fields: " \
  "skill (an approved capsule id injected into its brief), name (short display " \
  "name - also its saved-document name), project (a run tag: the sub-agent's " \
  "full result AUTO-SAVES to the durable file store as <project>/<name>-<tag>.md " \
  "- use ONE project per fan-out run, e.g. dr-topic-08061200), attach (up to 4 " \
  "'<project>/<name>' docs whose full content the device inserts into its " \
  "instruction - how a sub-agent reads files). attach must be the JSON array " \
  "FIELD on the spawn op: naming docs in the task text attaches NOTHING. Emit " \
  "one spawn op per requested unit of work, up to the amount [SPAWN CAPACITY] " \
  "says you can start this turn; for a bigger fan-out than that, start a wave now " \
  "and spawn the rest when they finish (you get an automatic turn) - sub-agents " \
  "run sequentially, so this is throughput, not a hard limit. Tell the owner what " \
  "you started, rather than silently dropping work."

// ---- prompt field-doc block (generated from the same macros) -----------------
// Appended to the role prompt by orchestrator.cpp - never hand-write these.
// NOTE: any new field's docs MUST live in an ORCH_D_* macro (so they appear
// here, in the system prompt) - Anthropic strips sub-schema `description` keys
// from the wire to fit its strict grammar budget, so the prompt block is the
// only place that provider sees field docs.
#define ORCH_FIELD_DOCS \
  "- reply: " ORCH_D_REPLY "\n" \
  "- ask: " ORCH_D_ASK "\n" \
  "- memory: " ORCH_D_MEMORY "\n" \
  "- device: " ORCH_D_DEVICE "\n" \
  "- mem_write: " ORCH_D_MEMW "\n" \
  "- mem_query: " ORCH_D_MEMQ "\n" \
  "- session_ops: " ORCH_D_SOPS "\n" \
  "- scratchpad: " ORCH_D_SCRATCH "\n"

// ---- the JSON schema (descriptions embedded; strict-compatible) --------------
static const char ORCH_SCHEMA_BODY[] =
  "{\"type\":\"object\",\"additionalProperties\":false,"
  "\"required\":[\"reply\",\"memory\",\"ask\",\"device\","
  "\"mem_write\",\"mem_query\",\"session_ops\",\"scratchpad\"],\"properties\":{"
  "\"reply\":{\"type\":\"string\",\"description\":\"" ORCH_D_REPLY "\"},"
  "\"memory\":{\"type\":\"string\",\"description\":\"" ORCH_D_MEMORY "\"},"
  "\"ask\":{\"type\":\"string\",\"description\":\"" ORCH_D_ASK "\"},"
  // device[]: discriminated union - one branch per action type.
  "\"device\":{\"type\":\"array\",\"description\":\"" ORCH_D_DEVICE "\","
  "\"items\":{\"anyOf\":["
  "{\"type\":\"object\",\"additionalProperties\":false,"
  "\"required\":[\"type\",\"mode\",\"r\",\"g\",\"b\",\"brightness\"],\"properties\":{"
  "\"type\":{\"type\":\"string\",\"const\":\"led\"},"
  "\"mode\":{\"type\":\"string\",\"enum\":[\"solid\",\"spinner\",\"pulse\",\"flash\",\"rainbow\"]},"
  "\"r\":{\"type\":\"integer\"},\"g\":{\"type\":\"integer\"},\"b\":{\"type\":\"integer\"},"
  "\"brightness\":{\"type\":[\"integer\",\"null\"]}}},"
  "{\"type\":\"object\",\"additionalProperties\":false,"
  "\"required\":[\"type\",\"value\"],\"properties\":{"
  "\"type\":{\"type\":\"string\",\"const\":\"lights\"},"
  "\"value\":{\"type\":\"string\",\"enum\":[\"off\",\"full\"]}}},"
  "{\"type\":\"object\",\"additionalProperties\":false,"
  "\"required\":[\"type\",\"text\"],\"properties\":{"
  "\"type\":{\"type\":\"string\",\"const\":\"tts\"},\"text\":{\"type\":\"string\"}}},"
  "{\"type\":\"object\",\"additionalProperties\":false,"
  "\"required\":[\"type\"],\"properties\":{\"type\":{\"type\":\"string\",\"const\":\"reboot\"}}},"
  "{\"type\":\"object\",\"additionalProperties\":false,"
  "\"required\":[\"type\",\"ledBrightness\",\"priority\",\"posture\",\"profile\","
  "\"theme\",\"attnHoldMs\",\"ttsVoice\",\"ttsProv\",\"ttsOn\",\"sttProv\","
  "\"sfxLvlN\",\"sfxLvlO\",\"sfxVol\",\"sfxTheme\",\"sleepOvr\",\"brightOvr\",\"devName\"],\"properties\":{"
  "\"type\":{\"type\":\"string\",\"const\":\"config\"},"
  "\"ledBrightness\":{\"type\":[\"integer\",\"null\"]},"
  "\"priority\":{\"type\":[\"string\",\"null\"]},"
  "\"ttsVoice\":{\"type\":[\"string\",\"null\"]},"
  "\"ttsProv\":{\"anyOf\":[{\"type\":\"string\",\"enum\":[\"mistral\",\"openai\"]},{\"type\":\"null\"}]},"
  // Nullable ENUMS use anyOf[{enum},{null}] - the one form BOTH strict
  // validators accept (Anthropic rejects enum under a union type; OpenAI
  // accepts anyOf branches). Plain nullable scalars keep type:[X,null].
  "\"posture\":{\"anyOf\":[{\"type\":\"string\",\"enum\":[\"dark\",\"calm\",\"full\"]},{\"type\":\"null\"}]},"
  "\"profile\":{\"anyOf\":[{\"type\":\"string\",\"enum\":[\"battery_saver\",\"balanced\",\"desk\"]},{\"type\":\"null\"}]},"
  "\"theme\":{\"type\":[\"string\",\"null\"]},"
  "\"attnHoldMs\":{\"type\":[\"integer\",\"null\"]},"
  // Owner-serviceable knobs (2026-07-16 - 'the agent should be able to enable most
  // configs'): voice replies on/off, STT provider, SFX levels/volume/race theme,
  // device name. Secrets/routing stay HUMAN-only (protected-key refusal).
  "\"ttsOn\":{\"type\":[\"boolean\",\"null\"]},"
  // Protection overrides (2026-07-17): plain nullable booleans on the wire; the
  // FIELD DOCS carry the risk contract (deep-discharge / panel-heat) and the
  // requirement to state it to the owner. The device validator + scheduled-turn
  // refusal are the enforcement; the wire stays strict-simple.
  "\"sleepOvr\":{\"type\":[\"boolean\",\"null\"]},"
  "\"brightOvr\":{\"type\":[\"boolean\",\"null\"]},"
  "\"sttProv\":{\"anyOf\":[{\"type\":\"string\",\"enum\":[\"mistral\",\"openai\"]},{\"type\":\"null\"}]},"
  "\"sfxLvlN\":{\"type\":[\"integer\",\"null\"]},"
  "\"sfxLvlO\":{\"type\":[\"integer\",\"null\"]},"
  "\"sfxVol\":{\"type\":[\"integer\",\"null\"]},"
  "\"sfxTheme\":{\"anyOf\":[{\"type\":\"string\",\"enum\":[\"pulse\"]},{\"type\":\"null\"}]},"
  "\"devName\":{\"type\":[\"string\",\"null\"]}}},"
  "{\"type\":\"object\",\"additionalProperties\":false,"
  "\"required\":[\"type\",\"provider\",\"model\"],\"properties\":{"
  "\"type\":{\"type\":\"string\",\"const\":\"orch_model\"},"
  "\"provider\":{\"type\":\"string\",\"enum\":[\"openai\",\"anthropic\",\"mistral\"]},"
  "\"model\":{\"type\":\"string\"}}}"
  "]}},"
  // spawn[] / await[] retired from the wire (session_ops is the one spawn
  // surface). parseTurn still tolerates them if an old client sends them.
  "\"mem_write\":{\"type\":\"array\",\"description\":\"" ORCH_D_MEMW "\","
  "\"items\":{\"type\":\"object\",\"additionalProperties\":false,"
  "\"required\":[\"content\",\"importance\",\"permanent\",\"ttl\"],\"properties\":{"
  "\"content\":{\"type\":\"string\"},"
  "\"importance\":{\"type\":[\"number\",\"null\"]},"
  "\"permanent\":{\"type\":[\"boolean\",\"null\"]},"
  "\"ttl\":{\"anyOf\":[{\"type\":\"string\",\"enum\":[\"session\",\"days\",\"weeks\",\"months\",\"permanent\"]},{\"type\":\"null\"}]}}}},"
  "\"mem_query\":{\"type\":\"array\",\"description\":\"" ORCH_D_MEMQ "\",\"items\":{\"type\":\"string\"}},"
  // session_ops: ONLY the ops that actually work on this fabric (spawn /
  // terminate). tell/poll/list were advertised before and hard-failed at
  // runtime - the schema no longer offers them (honesty: advertised == real).
  "\"session_ops\":{\"type\":\"array\",\"description\":\"" ORCH_D_SOPS "\","
  "\"items\":{\"type\":\"object\",\"additionalProperties\":false,"
  "\"required\":[\"op\",\"id\",\"task\",\"provider\",\"model\",\"skill\",\"name\","
  "\"project\",\"attach\"],\"properties\":{"
  "\"op\":{\"type\":\"string\",\"enum\":[\"spawn\",\"terminate\"]},"
  "\"id\":{\"type\":[\"string\",\"null\"]},\"task\":{\"type\":[\"string\",\"null\"]},"
  "\"provider\":{\"type\":[\"string\",\"null\"]},\"model\":{\"type\":[\"string\",\"null\"]},"
  "\"skill\":{\"type\":[\"string\",\"null\"]},\"name\":{\"type\":[\"string\",\"null\"]},"
  "\"project\":{\"type\":[\"string\",\"null\"]},"
  "\"attach\":{\"type\":[\"array\",\"null\"],\"items\":{\"type\":\"string\"}}}}},"
  // scratchpad: nullable persistent working-memory tiers. Null
  // = no change; a non-null tier REPLACES that tier. Required-but-nullable to
  // satisfy strict schemas, exactly like the other optional-in-spirit fields.
  "\"scratchpad\":{\"type\":[\"object\",\"null\"],\"description\":\"" ORCH_D_SCRATCH "\","
  "\"additionalProperties\":false,"
  "\"required\":[\"active\",\"short\",\"mid\",\"long\"],\"properties\":{"
  "\"active\":{\"type\":[\"string\",\"null\"]},"
  "\"short\":{\"type\":[\"array\",\"null\"],\"items\":{\"type\":\"string\"}},"
  "\"mid\":{\"type\":[\"array\",\"null\"],\"items\":{\"type\":\"string\"}},"
  "\"long\":{\"type\":[\"array\",\"null\"],\"items\":{\"type\":\"string\"}}}}}}";

}  // namespace orch
}  // namespace nimbus
