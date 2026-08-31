// nimbusd - the hosted Nimbus daemon (Virtual Nimbus, Phase 0).
//
// Composes the real Nimbus orchestrator (the same TurnEngine + memory + tools +
// provider adapters the desk device runs) as a long-lived Linux process:
//   * durable POSIX stores under a data dir  -> memory survives restart
//   * an engine thread + request mailbox     -> single-context invariant held
//   * a Telegram channel                     -> the persona, same as the device
//   * a 127.0.0.1 control surface            -> the seam the relay sidecar forwards to
//
// Secrets arrive from the environment (a mounted Secret in the container) and an
// optional non-secret config file; nothing secret is ever logged (masked to 4
// chars). BYOK is the Phase 0 key mode: the operator supplies the provider keys.
//
// Usage:
//   nimbusd                 run the daemon (Telegram + control surface)
//   nimbusd --getme         validate TELEGRAM_BOT_TOKEN via getMe and exit
//   nimbusd --once "text"   run ONE turn against the composed engine, print the
//                           reply, and exit (a keyed smoke check)
//   nimbusd --help
//
// Env / config keys: NIMBUSD_DATA_DIR (/data), NIMBUSD_CONFIG (<data>/config.env),
//   NIMBUSD_CONTROL_ADDR (127.0.0.1), NIMBUSD_CONTROL_PORT (8787),
//   NIMBUSD_WEB_TOKEN, NIMBUSD_TG_CHAT_ID, NIMBUSD_DEVICE_NAME, NIMBUSD_PRIORITY,
//   TELEGRAM_BOT_TOKEN, OPENAI_API_KEY / ANTHROPIC_API_KEY / MISTRAL_API_KEY,
//   TAVILY_API_KEY, TZ.
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "daemon_config.h"
#include "daemon_http.h"
#include "engine_thread.h"
#include "http_control.h"
#include "reply_buffer.h"
#include "rig.h"
#include "telegram.h"

namespace {

std::atomic<bool> g_stop{false};
void onSignal(int) { g_stop.store(true); }

void logLine(const std::string& s) { std::fprintf(stderr, "[nimbusd] %s\n", s.c_str()); }

nimbusd::NimbusdRig::Options buildOptions(const nimbusd::Config& cfg) {
  nimbusd::NimbusdRig::Options opt;
  opt.dataDir = cfg.get("NIMBUSD_DATA_DIR", "/data");
  opt.devName = cfg.get("NIMBUSD_DEVICE_NAME", "Nimbus");
  opt.priority = cfg.get("NIMBUSD_PRIORITY", "mistral,openai,anthropic");
  opt.role = "admin";  // a hosted instance is single-owner by construction
  // Embeddings default to Mistral (cheapest); disabled if no key is present so
  // the daemon still runs (recall simply returns nothing).
  opt.embedHost = cfg.get("NIMBUSD_EMBED_HOST", "mistral");
  opt.embeddings = !cfg.providerKey(opt.embedHost).empty();
  opt.embedModel = cfg.get("NIMBUSD_EMBED_MODEL", "mistral-embed");
  opt.embedDims = cfg.getInt("NIMBUSD_EMBED_DIMS", 1024);
  opt.maxVectors = cfg.getInt("NIMBUSD_MAX_VECTORS", 5000);
  return opt;
}

// Validate TELEGRAM_BOT_TOKEN via getMe. Returns 0 on success.
int cmdGetMe(nimbusd::Config& cfg) {
  const std::string token = cfg.get("TELEGRAM_BOT_TOKEN");
  if (token.empty()) { logLine("no TELEGRAM_BOT_TOKEN set"); return 2; }
  nimbusd::DaemonHttpTransport http;
  nimbusd::TelegramChannel ch(token, &http, "/tmp/nimbusd-getme-offset");
  std::string user, err;
  if (!ch.getMe(user, err)) {
    logLine("getMe FAILED for token " + nimbusd::Config::mask(token) + ": " + err);
    return 1;
  }
  logLine("getMe OK: bot @" + user + " (token " + nimbusd::Config::mask(token) + ")");
  return 0;
}

// Run one turn and print the reply (a keyed smoke check).
int cmdOnce(nimbusd::Config& cfg, const std::string& text) {
  nimbusd::NimbusdRig rig(cfg, buildOptions(cfg));
  auto t = rig.say("owner", text);
  std::printf("%s\n", t.reply.empty() ? "(no reply)" : t.reply.c_str());
  return t.reply.empty() ? 1 : 0;
}

int runDaemon(nimbusd::Config& cfg) {
  auto opt = buildOptions(cfg);
  logLine("starting nimbusd: data=" + opt.dataDir + " name=" + opt.devName +
          " priority=" + opt.priority);
  for (const char* h : {"openai", "anthropic", "mistral"})
    logLine(std::string("provider ") + h + ": " +
            (cfg.providerKey(h).empty() ? "no key" : nimbusd::Config::mask(cfg.providerKey(h))));
  logLine(std::string("embeddings: ") + (opt.embeddings ? ("on (" + opt.embedHost + ")") : "off (no key)"));

  nimbusd::NimbusdRig rig(cfg, opt);
  nimbusd::EngineThread eng(&rig);
  eng.start();

  // The reply ring that backs the web chat page (GET /api/replies). Every reply
  // the engine produces is recorded here and, when a bot is configured, also
  // forwarded to Telegram. The delivery hook is set once, before any turn runs,
  // so no reply is lost; `sender` is filled in below if a bot token is present.
  nimbusd::ReplyBuffer replies(/*cap=*/50);
  nimbusd::TelegramChannel* sender = nullptr;
  rig.setDeliver([&replies, &sender](const std::string& chat, const std::string& text) {
    replies.push("assistant", text);
    if (sender) { std::string e; sender->sendMessage(chat, text, e); }
  });

  // Control surface (loopback), token-gated.
  const std::string webToken = cfg.get("NIMBUSD_WEB_TOKEN");
  nimbusd::HttpControl http(&eng, cfg.get("NIMBUSD_CONTROL_ADDR", "127.0.0.1"),
                           cfg.getInt("NIMBUSD_CONTROL_PORT", 8787), webToken, opt.dataDir,
                           &replies);
  const int port = http.start();
  if (port < 0) { logLine("FAILED to bind the control surface"); eng.stop(); return 1; }
  logLine("control surface on " + cfg.get("NIMBUSD_CONTROL_ADDR", "127.0.0.1") + ":" +
          std::to_string(port) + (webToken.empty() ? " (UNGATED - set NIMBUSD_WEB_TOKEN)" : " (token-gated)"));

  // Telegram: separate transports for the long-poll and for sends so a held
  // long-poll never blocks a reply.
  const std::string tgToken = cfg.get("TELEGRAM_BOT_TOKEN");
  const std::string allowChat = cfg.get("NIMBUSD_TG_CHAT_ID");
  std::unique_ptr<nimbusd::DaemonHttpTransport> pollHttp, sendHttp;
  std::unique_ptr<nimbusd::TelegramChannel> tgPoll, tgSend;
  std::thread pollThread;

  if (!tgToken.empty()) {
    pollHttp.reset(new nimbusd::DaemonHttpTransport());
    sendHttp.reset(new nimbusd::DaemonHttpTransport());
    tgPoll.reset(new nimbusd::TelegramChannel(tgToken, pollHttp.get(),
                                              opt.dataDir + "/tg_offset", allowChat));
    tgSend.reset(new nimbusd::TelegramChannel(tgToken, sendHttp.get(),
                                              opt.dataDir + "/tg_send_unused", allowChat));
    std::string user, err;
    if (tgPoll->getMe(user, err)) logLine("telegram: bot @" + user + " validated");
    else logLine("telegram: getMe failed (" + err + ") - poll loop will still retry");

    // Route replies to this bot too (the delivery hook above forwards to it).
    sender = tgSend.get();

    pollThread = std::thread([&] {
      logLine("telegram long-poll started");
      while (!g_stop.load()) {
        std::vector<nimbus::tg::Update> ups;
        std::string err;
        if (!tgPoll->poll(25, ups, err)) {
          if (!g_stop.load()) { logLine("telegram poll error: " + err); std::this_thread::sleep_for(std::chrono::seconds(3)); }
          continue;
        }
        for (const auto& u : ups)
          if (!u.text.empty()) eng.postMessage(u.chatId, u.text);
      }
    });
  } else {
    logLine("telegram: no TELEGRAM_BOT_TOKEN - running control-surface only");
  }

  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);
  logLine("nimbusd ready");

  while (!g_stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(200));

  logLine("shutting down (flushing memory)");
  if (pollThread.joinable()) pollThread.join();
  http.stop();
  eng.stop();          // joins the engine thread
  rig.flush();         // final durable flush (dtor also flushes)
  logLine("stopped cleanly");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  nimbusd::Config cfg;
  // Load the config file (env still wins per-key). Default path lives under the
  // data dir so a mounted config travels with the volume.
  const char* dataEnv = std::getenv("NIMBUSD_DATA_DIR");
  const std::string dataDir = dataEnv && *dataEnv ? dataEnv : "/data";
  const char* cfgEnv = std::getenv("NIMBUSD_CONFIG");
  cfg.loadFile(cfgEnv && *cfgEnv ? cfgEnv : (dataDir + "/config.env"));

  std::string cmd, arg;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--help" || a == "-h") {
      std::puts("nimbusd - the hosted Nimbus daemon\n"
                "  nimbusd                 run the daemon\n"
                "  nimbusd --getme         validate TELEGRAM_BOT_TOKEN and exit\n"
                "  nimbusd --once \"text\"   run one turn, print the reply, exit\n");
      return 0;
    }
    if (a == "--getme") cmd = "getme";
    else if (a == "--once") { cmd = "once"; if (i + 1 < argc) arg = argv[++i]; }
  }

  if (cmd == "getme") return cmdGetMe(cfg);
  if (cmd == "once") return cmdOnce(cfg, arg);
  return runDaemon(cfg);
}
