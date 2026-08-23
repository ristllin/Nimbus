#pragma once
#include <curl/curl.h>

#include <mutex>
#include <string>

#include "nimbus/harness/http.h"

// daemon_http - the daemon's agent::HttpTransport over libcurl.
//
// Same seam the device implements over WiFiClientSecure and the lab over a
// per-exchange curl_easy_init. The difference here is daemon-grade: ONE
// persistent CURL easy handle is reused across exchanges, so its connection +
// TLS-session cache is kept warm (the lab's per-call init/cleanup is fine for a
// CLI, wasteful for a long-lived process doing thousands of provider round
// trips). recordBodies is hard-off by construction - the log-discipline rule
// (§3.4) is that nimbusd never keeps prompt/completion content in memory or
// logs; only the injected transport ever sees the bytes, and it drops them.
//
// The handle is guarded by a mutex: the engine runs on one thread, but the
// control surface / health path can also probe, so exec() is made reentrant-safe
// rather than assuming a single caller.
namespace nimbusd {

class DaemonHttpTransport : public agent::HttpTransport {
 public:
  DaemonHttpTransport() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    handle_ = curl_easy_init();
  }
  ~DaemonHttpTransport() override {
    if (handle_) curl_easy_cleanup(handle_);
    curl_global_cleanup();
  }

  DaemonHttpTransport(const DaemonHttpTransport&) = delete;
  DaemonHttpTransport& operator=(const DaemonHttpTransport&) = delete;

  bool exec(const agent::HttpRequest& req, agent::HttpResponse& out,
            std::string& err) override {
    std::lock_guard<std::mutex> lk(mu_);
    out.status = 0;
    out.body.clear();
    err.clear();
    if (!handle_) { err = "curl handle unavailable"; return false; }

    curl_easy_reset(handle_);

    const std::string url = std::string(req.tls ? "https://" : "http://") + req.host +
                            (req.port && req.port != (req.tls ? 443 : 80)
                                 ? ":" + std::to_string(req.port)
                                 : "") +
                            req.path;

    curl_slist* hdrs = nullptr;
    for (const auto& h : req.headers)
      hdrs = curl_slist_append(hdrs, (h.first + ": " + h.second).c_str());
    hdrs = curl_slist_append(hdrs, "Expect:");  // no 100-continue stall

    curl_easy_setopt(handle_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle_, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(handle_, CURLOPT_TIMEOUT_MS, (long)req.timeoutMs);
    curl_easy_setopt(handle_, CURLOPT_CONNECTTIMEOUT_MS, 15000L);
    curl_easy_setopt(handle_, CURLOPT_WRITEFUNCTION, &DaemonHttpTransport::onWrite);
    curl_easy_setopt(handle_, CURLOPT_WRITEDATA, &out.body);
    curl_easy_setopt(handle_, CURLOPT_NOSIGNAL, 1L);
    // Keep the connection alive between exchanges (the whole point of the
    // persistent handle) and cap redirects off - provider APIs never redirect.
    curl_easy_setopt(handle_, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(handle_, CURLOPT_FOLLOWLOCATION, 0L);

    if (req.method == "POST") {
      curl_easy_setopt(handle_, CURLOPT_POST, 1L);
      curl_easy_setopt(handle_, CURLOPT_POSTFIELDS, req.body.data());
      curl_easy_setopt(handle_, CURLOPT_POSTFIELDSIZE, (long)req.body.size());
    } else if (req.method != "GET") {
      curl_easy_setopt(handle_, CURLOPT_CUSTOMREQUEST, req.method.c_str());
      if (!req.body.empty()) {
        curl_easy_setopt(handle_, CURLOPT_POSTFIELDS, req.body.data());
        curl_easy_setopt(handle_, CURLOPT_POSTFIELDSIZE, (long)req.body.size());
      }
    }

    const CURLcode rc = curl_easy_perform(handle_);
    long code = 0;
    curl_easy_getinfo(handle_, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(hdrs);

    if (rc != CURLE_OK) {
      err = curl_easy_strerror(rc);
      out.status = 0;
      return false;
    }
    out.status = (int)code;
    return true;
  }

 private:
  static size_t onWrite(char* p, size_t sz, size_t nm, void* ud) {
    static_cast<std::string*>(ud)->append(p, sz * nm);
    return sz * nm;
  }

  std::mutex mu_;
  CURL* handle_ = nullptr;
};

}  // namespace nimbusd
