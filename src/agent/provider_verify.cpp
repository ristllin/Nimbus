#include "provider_verify.h"

#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

#include "../sys/ps_json.h"       // PsramJsonAllocator - Mistral model-metadata parse off internal heap

#include "esp_heap_caps.h"
#include "agent_config.h"
#include "../sys/agent_log.h"
#include "../sys/net_util.h"      // tlsClose - RST-close on every path
#include "store.h"
#include "../sys/tls_arbiter.h"   // one work TLS at a time (coexists with Telegram's)

namespace agent {
namespace provider_verify {

// One-slot queue. g_pending is the handoff flag between the web task (request)
// and the verify task (run); the provider string is only written while
// g_pending is false and read after it flips true, so the volatile flag is the
// only synchronization needed (single producer, single consumer).
static volatile bool g_pending = false;
static char          g_provider[16] = {};

// Largest-contiguous-INTERNAL-block floor below which we DON'T attempt the handshake.
// mbedTLS's big RX/TX content buffers (~16 KB each) are now routed to PSRAM (main.cpp
// installs a PSRAM-backed mbedTLS allocator), so the handshake's remaining INTERNAL
// need is much smaller than the pre-PSRAM 50 KB - mostly lwIP/socket + cert-parse
// transients. Gated on the largest CONTIGUOUS internal block (not total free) because
// the handshake still wants one modest contiguous buffer. 16 KB: measured live on
// v2.0.0 the largest CONTIGUOUS internal block rests ~19 KB (fragmentation caps it
// well below the ~78 KB total free), and the PSRAM-routed mbedTLS means the real
// contiguous internal need is only lwIP/cert transients - so the old 30 KB floor
// (a copy of the total-heap-era value) perpetually DEFERRED verify even with the
// key present (proven on-device: max8=19444 -> deferred). Below 16 KB we record -1
// ("couldn't verify - low memory") without a doomed attempt; a genuine OOM
// handshake still fails soft. See docs/memory-model.md.
static const size_t VERIFY_MIN_MAX8 = 16000;

static void verifyTask(void*);   // spawned per request(); self-deletes

bool request(const String& provider) {
  if (g_pending) return false;  // slot busy - one verify at a time
  strncpy(g_provider, provider.c_str(), sizeof(g_provider) - 1);
  g_provider[sizeof(g_provider) - 1] = 0;
  g_pending = true;
  // Spawn the worker HERE rather than keeping one resident. A verify happens
  // when a human clicks "verify" in the web UI - a handful of times in a device's
  // life - but the task's 8 KB stack is INTERNAL SRAM held for the whole uptime,
  // and internal SRAM is the resource the turn engine actually runs out of
  // (a multi-round tool loop caps at `cap=heap` when the turn-time heap dips
  // below ORCH_LOOP_MIN_HEAP). Reclaiming these 8 KB is what buys the tool loop
  // its rounds back. The task self-deletes when the verify completes.
  if (xTaskCreate(verifyTask, "pverify", 8192, nullptr, 1, nullptr) != pdPASS) {
    g_pending = false;   // nothing will run it - fail the request honestly
    alog("verify: could not start worker (low memory)");
    return false;
  }
  return true;
}

bool pending() { return g_pending; }

static void runOne() {
  String provider = g_provider;

  const char* host = nullptr;
  String path = "/v1/models";
  String key;
  // Telegram is a pseudo-provider: same one-slot, arbited, watchdog-free verify seam,
  // but it proves the BOT TOKEN via getMe instead of an API key via /v1/models (owner:
  // "when you save the telegram token, first run a tiny verification"). The token rides
  // the URL path (/bot<token>/getMe), so there's no auth header.
  const bool isTg  = (provider == "telegram");
  // Tavily is POST-shaped: there is no cheap GET, so the verify is one minimal
  // /search (1 credit) - the same "tiny real call" contract as the others.
  const bool isTav = (provider == "tavily");
  if (provider == "openai")         { host = OPENAI_HOST;    key = store::openaiKey(); }
  else if (provider == "anthropic") { host = ANTHROPIC_HOST; key = store::anthropicKey(); }
  else if (provider == "mistral")   { host = MISTRAL_HOST;   key = store::mistralKey(); }
  else if (isTg)                    { host = "api.telegram.org"; key = store::telegramToken();
                                      path = String("/bot") + key + "/getMe"; }
  else if (isTav)                   { host = "api.tavily.com"; key = store::tavilyKey();
                                      path = "/search"; }
  else { g_pending = false; return; }  // web handler validates; defensive

  if (!key.length()) {  // nothing to verify - rejected, no TLS spent
    store::setVerify(provider, 0, (uint32_t)millis());
    alogf("verify: %s no key -> rejected", provider.c_str());
    g_pending = false;
    return;
  }
  // A transient "couldn't verify" (-1) must not DEMOTE a definitive VERIFIED (1):
  // the capProbe=2 tick re-verifies unattended (WiFi still rejoining, TLS slot
  // busy behind a turn), and one blip was erasing the catalog's VERIFIED marking
  // until the next success. Definitive verdicts (1 / 0) always overwrite.
  auto recordVerify = [&](const String& prov, int8_t result) {
    if (result == -1 && store::verifyResult(prov) == 1) {
      alogf("verify: %s transient failure - keeping cached verified", prov.c_str());
      return;
    }
    store::setVerify(prov, result, (uint32_t)millis());
  };
  // MUST include MALLOC_CAP_INTERNAL: plain MALLOC_CAP_8BIT counts PSRAM too, so on
  // this 8 MB-PSRAM board the block is always ~megabytes and the guard never fires -
  // defeating the intended INTERNAL-heap check (which matters more now that the
  // 2-slot arbiter lets verify run beside another work-TLS session).
  size_t max8 = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (max8 < VERIFY_MIN_MAX8) {
    recordVerify(provider, -1);
    alogf("verify: %s deferred (max8=%u)", provider.c_str(), (unsigned)max8);
    g_pending = false;
    return;
  }
  // Outlast one full Telegram long-poll cycle (30 s) so a verify queued behind
  // an orchestrator turn still lands instead of bouncing "tls busy".
  if (!arbiter::acquireWork(35000)) {
    recordVerify(provider, -1);
    alogf("verify: %s tls busy", provider.c_str());
    g_pending = false;
    return;
  }

  int8_t result = -1;  // couldn't verify; 1 on HTTP 200, 0 on 401/403
  {
    WiFiClientSecure client;
    tlsSetup(client);
    client.setHandshakeTimeout(12);
    client.setConnectionTimeout(15000);  // F25: real socket bound (setTimeout inert)
    if (client.connect(host, 443)) {
      // Tavily: a minimal POST /search proves the key does real work; the others
      // stay GET-shaped.
      const char* body = isTav ? "{\"query\":\"ping\",\"max_results\":1}" : nullptr;
      String req = String(isTav ? "POST " : "GET ") + path + " HTTP/1.0\r\nHost: " + host + "\r\n";
      if (provider == "anthropic")
        req += String("x-api-key: ") + key + "\r\nanthropic-version: " ANTHROPIC_VER "\r\n";
      else if (!isTg)   // telegram carries the token in the path, no auth header
        req += String("Authorization: Bearer ") + key + "\r\n";
      if (body) {
        req += "Content-Type: application/json\r\nContent-Length: ";
        req += (int)strlen(body);
        req += "\r\n";
      }
      // identity: some provider edges gzip even an HTTP/1.0 request with no
      // Accept-Encoding (live-caught: a 16 KB unreadable body -> harvest EMPTY);
      // the harvest scans for literal "id":" strings, so compression is fatal.
      req += "Accept-Encoding: identity\r\nUser-Agent: Nimbus\r\nConnection: close\r\n\r\n";
      if (body) req += body;
      client.print(req);

      uint32_t deadline = millis() + 15000;  // read just the status line
      String status;
      while ((int32_t)(millis() - deadline) < 0) {
        if (client.available()) {
          char c = client.read();
          if (c == '\n') break;
          if (c != '\r') status += c;
        } else if (!client.connected() && !client.available()) {
          break;
        } else {
          delay(2);
        }
      }
      int code = 0, sp = status.indexOf(' ');
      if (sp > 0 && (int)status.length() >= sp + 4)
        code = status.substring(sp + 1, sp + 4).toInt();
      // Telegram: a bad/malformed bot token answers 401 OR 404 (unknown bot path).
      result = (code == 200) ? 1
             : (code == 401 || code == 403 || (isTg && code == 404)) ? 0
             : -1;
      // LLM providers: the verify already fetched /v1/models - HARVEST it (owner
      // 2026-07-16: the static model dropdowns were stale; no Opus 4.8 / Fable /
      // new-gen OpenAI). Stream the body into a bounded PSRAM buffer, pull every
      // "id" that passes the per-provider chat-capable filter, cap at 8, store as
      // the live choice list (NVS; static defaults remain the fallback).
      if (!isTg && !isTav && code == 200) {
        const size_t kCap = 65536;   // OpenAI's list is the biggest (~40 KB)
        char* buf = (char*)heap_caps_malloc(kCap, MALLOC_CAP_SPIRAM);
        if (buf) {
          // Own deadline: the status-line 15 s window can be mostly spent by the
          // time a 40 KB body streams - a shared clock starved the read to zero
          // bytes on a slow round (live-caught: openai verified but no harvest).
          const uint32_t bodyDeadline = millis() + 20000;
          size_t blen = 0;
          while (millis() < bodyDeadline && blen < kCap - 1) {
            // read() drives the TLS layer to decrypt the NEXT record even when
            // available()==0 - the old available()/connected() break bailed at a
            // record boundary (~16 KB) with the rest of the body still buffered
            // below (live-caught: identical 16751-byte truncation on every run).
            int c = client.read();
            if (c >= 0) { buf[blen++] = (char)c; continue; }
            if (!client.connected()) break;   // truly drained + closed
            delay(2);
          }
          buf[blen] = 0;
          String csv;
          int kept = 0;
          bool handled = false;   // Mistral's metadata path sets this so the id
                                  // string-scan below is skipped (openai/anthropic only).
          // Non-chat FAMILIES that can't run our turn contract (embeddings/audio/
          // image/etc.), by id substring - provider-independent. Used both by the
          // Mistral metadata path (as its ONLY name filter - capability flags do the
          // rest) and by the legacy string-scan's `rejected` below.
          auto neverFamily = [](const String& id) -> bool {
            static const char* kNever[] = {"embed", "moderation", "audio", "realtime",
                                           "transcribe", "tts", "whisper", "image",
                                           "dall-e", "ocr", "voxtral", "davinci",
                                           "babbage", "instruct", "search-", "chatgpt",
                                           "research",   // deep-research models can't run chat turns
                                           "fim",        // fill-in-middle = code completion, not a chat/agentic turn
                                           nullptr};
            for (int i = 0; kNever[i]; ++i)
              if (id.indexOf(kNever[i]) >= 0) return true;
            return false;
          };
          // Per-provider chat-capable filter (the legacy id string-scan path).
          auto rejected = [&](const String& id) -> bool {
            if (neverFamily(id)) return true;
            if (provider == "openai") {
              // Dated snapshots (…-2025-04-16) duplicate their undated alias and
              // waste dropdown slots - openai-only (anthropic's haiku ids are
              // dated-ONLY, rejecting there would lose the model entirely).
              if (id.indexOf("-202") >= 0) return true;
              return !(id.startsWith("gpt-") || (id[0] == 'o' && id[1] >= '1' && id[1] <= '9'));
            }
            if (provider == "anthropic") return !id.startsWith("claude-");
            if (provider == "mistral")   return !id.endsWith("-latest");
            return true;
          };
          // Flagship-family ids first: the provider's list order is arbitrary, and a
          // cap-8 single pass filled Mistral's slots with code/tiny variants before
          // ever reaching mistral-large (live-caught). Pass 1 keeps preferred ids,
          // pass 2 fills the remaining slots with the rest.
          auto preferred = [&](const String& id) -> bool {
            if (provider == "mistral")
              return id.indexOf("large") >= 0 || id.indexOf("medium") >= 0 ||
                     id.indexOf("small") >= 0 || id.indexOf("magistral") >= 0;
            // gpt-5 family first - the o-series ids precede gpt-5* in the body and
            // were filling every slot before the flagship (live-caught).
            if (provider == "openai")    return id.startsWith("gpt-5");
            return true;   // anthropic's list arrives newest-first already
          };
          // Mistral: METADATA-driven filter. Unlike OpenAI's id-only /v1/models,
          // Mistral's carries per-model `capabilities` + `deprecation` + `aliases`.
          // The old `!endsWith("-latest")` heuristic (a) still OFFERED 5 models that
          // DEPRECATE 2026-07-31 (they keep a -latest alias) and even PRIORITISED
          // magistral-medium-latest, and (b) ignored whether a model can actually run
          // our turn (chat + function-calling). Parse ONLY those fields (ArduinoJson
          // Filter bounds the DOM; the doc rides PSRAM) and keep chat+function-calling,
          // NON-deprecated models, alias-deduped (prefer the canonical -latest form),
          // flagships first. On any parse failure -> handled stays false -> the legacy
          // string-scan runs as a safety net.
          if (provider == "mistral") {
            // Parse the WHOLE body into a PSRAM-backed doc (a Filter parse returned an
            // empty data array on-device; the full doc is ~150 KB in PSRAM, of which
            // 8 MB is free, so the internal-heap concern that would motivate a filter
            // doesn't apply here - the verify task's internal cost stays ~0).
            // buf still holds the HTTP RESPONSE HEADERS ahead of the JSON body (only
            // the status line was consumed above; the string-scan below tolerates the
            // headers, a JSON parse does not). Skip to the body: past the blank line
            // separating headers from body.
            const char* jbody = strstr(buf, "\r\n\r\n");
            jbody = jbody ? jbody + 4 : (strstr(buf, "\n\n") ? strstr(buf, "\n\n") + 2 : buf);
            JsonDocument md(&PsramJsonAllocator::instance());
            DeserializationError perr =
                deserializeJson(md, jbody, DeserializationOption::NestingLimit(16));
            if (perr)
              alogf("verify: mistral meta parse err: %s", perr.c_str());
            if (!perr) {
              JsonArrayConst arr = md["data"].as<JsonArrayConst>();
              for (int pass = 0; pass < 2 && kept < 8; ++pass) {
                for (JsonObjectConst m : arr) {
                  if (kept >= 8) break;
                  String id((const char*)(m["id"] | ""));
                  if (id.length() < 3 || neverFamily(id)) continue;   // family filter only
                  JsonObjectConst cap = m["capabilities"];
                  if (!(cap["completion_chat"] | false)) continue; // can't run a chat turn
                  if (!(cap["function_calling"] | false)) continue;// can't do the tool contract
                  if (!m["deprecation"].isNull()) continue;        // dies soon - never offer it
                  // Alias dedup: drop a dated snapshot that a -latest alias also covers.
                  if (!id.endsWith("-latest")) {
                    bool aliasedLatest = false;
                    for (JsonVariantConst a : m["aliases"].as<JsonArrayConst>())
                      if (String((const char*)(a | "")).endsWith("-latest")) { aliasedLatest = true; break; }
                    if (aliasedLatest) continue;
                  }
                  const bool flag = id.indexOf("large") >= 0 || id.indexOf("medium") >= 0 ||
                                    id.indexOf("small") >= 0 || id.indexOf("magistral") >= 0;
                  if ((pass == 0) != flag) continue;               // pass 0: flagships; pass 1: rest
                  if ((String(",") + csv + ",").indexOf(String(",") + id + ",") >= 0) continue;
                  csv += (csv.length() ? "," : "") + id;
                  ++kept;
                }
              }
              handled = true;   // parse OK -> don't also run the id string-scan
              alogf("verify: mistral metadata path kept %d (of %d models)",
                    kept, (int)md["data"].as<JsonArrayConst>().size());
            }
          }
          // openai's list is ordered OLDEST-first (created ascending) - keeping the
          // first 8 matches served gpt-5 while cutting gpt-5.5/5.4 (live-caught).
          // A ring of the LAST 8 matches fixes it; anthropic arrives newest-first
          // and mistral uses -latest aliases, so first-8 is right for them.
          if (!handled) {   // openai/anthropic (+ Mistral parse-failure fallback): id string-scan
          const bool keepNewestLast = (provider == "openai");
          String ring[8];
          int ringN = 0;
          for (int pass = 0; pass < 2 && kept < 8; ++pass) {
            const char* p = buf;
            // Tolerate pretty-printed JSON: OpenAI emits '"id": "x"' WITH a space
            // (live-caught - 125 ids in the body, zero matched the compact form);
            // Anthropic/Mistral emit compact '"id":"x"'. Match the key, then skip
            // whitespace to the value quote.
            while (kept < 8 && (p = strstr(p, "\"id\":")) != nullptr) {
              p += 5;
              while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
              if (*p != '"') continue;
              ++p;
              const char* e = strchr(p, '"');
              if (!e || e - p > 64) break;
              String id(p);
              id = id.substring(0, e - p);
              p = e;
              if (id.length() < 3 || rejected(id)) continue;
              if ((pass == 0) != preferred(id)) continue;   // pass 0: preferred; pass 1: the rest
              if (keepNewestLast) {
                bool dup = false;
                for (int r = 0; r < ringN && !dup; ++r) dup = (ring[r] == id);
                if (dup) continue;
                if (ringN == 8) {                       // slide: drop the oldest
                  for (int r = 1; r < 8; ++r) ring[r - 1] = ring[r];
                  ringN = 7;
                }
                ring[ringN++] = id;
                continue;                                // kept stays 0 -> full scan
              }
              String probe = String(",") + csv + ",";
              if (probe.indexOf(String(",") + id + ",") >= 0) continue;   // exact dedup
              csv += (csv.length() ? "," : "") + id;
              ++kept;
            }
            if (keepNewestLast) break;   // the ring pass covers everything in one sweep
          }
          if (keepNewestLast) {
            for (int r = ringN - 1; r >= 0; --r) {       // newest first in the dropdown
              csv += (csv.length() ? "," : "") + ring[r];
              ++kept;
            }
          }
          }   // end if(!handled) - legacy id string-scan
          if (kept > 0) {
            store::setModelChoices(provider, csv);
            alogf("verify: %s models harvested: %s", provider.c_str(), csv.c_str());
          } else {
            // Diagnose silently-empty harvests: read-starved (0 bytes) vs the
            // filter rejecting everything (bytes read but nothing kept).
            alogf("verify: %s harvest EMPTY (read %u bytes) - keeping the stored list",
                  provider.c_str(), (unsigned)blen);
          }
          free(buf);
        }
      }
      // Telegram bonus (owner ask: show WHICH bot is connected in the web app):
      // getMe's tiny body carries the bot's username - read the rest of the
      // response (bounded) and pull it out. Display-only NVS sidecar.
      if (isTg && code == 200) {
        String rest;
        rest.reserve(512);
        while ((int32_t)(millis() - deadline) < 0 && rest.length() < 700) {
          if (client.available()) rest += (char)client.read();
          else if (!client.connected() && !client.available()) break;
          else delay(2);
        }
        int u = rest.indexOf("\"username\":\"");
        if (u >= 0) {
          u += 12;
          int e = rest.indexOf('"', u);
          if (e > u && e - u <= 64) {
            store::setTgBotName(rest.substring(u, e));
            alogf("verify: telegram bot @%s", rest.substring(u, e).c_str());
          }
        }
      }
      alogf("verify: %s HTTP %d -> %s", provider.c_str(), code,
            result == 1 ? "verified" : result == 0 ? "rejected" : "couldn't verify");
    } else {
      alogf("verify: %s connect failed -> couldn't verify", provider.c_str());
    }
    tlsClose(client);  // RST-close on EVERY path (connected or not)
  }
  arbiter::releaseWork();
  recordVerify(provider, result);
  g_pending = false;  // clear LAST so pending() covers the whole run
}

// ONE verify, then the task exits and returns its 8 KB stack to internal SRAM.
// (It used to spin here forever polling g_pending every 250 ms, holding the stack
// for the device's whole uptime - see the note in request().)
static void verifyTask(void*) {
  runOne();                 // clears g_pending on every exit path
  g_pending = false;        // belt-and-braces: never leave the slot stuck busy
  vTaskDelete(nullptr);     // self-delete; frees the stack
}

void begin() {
  // Nothing to start: the worker is spawned per request() and self-deletes, so
  // no stack is held while idle. Kept as a stable entry point for main.cpp.
}

}  // namespace provider_verify
}  // namespace agent
