#pragma once
#include <curl/curl.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "nimbus/harness/http.h"

// CurlHttpTransport - the HOST implementation of agent::HttpTransport.
//
// This is the whole reason the lab works: every provider adapter, the tool loop,
// and web.search already speak to the network through one injected seam
// (agent::HttpTransport). The device implements it over WiFiClientSecure; here
// it is libcurl. Nothing above the seam knows the difference, so the code under
// test on this Mac is byte-for-byte the code that runs on the board.
//
// It also keeps a full transcript of every exchange. On the device that
// visibility costs a 1280-byte log ring that wraps in seconds; here it is just
// a vector, which is what makes "what did the model actually see?" answerable.
namespace lab {

class CurlHttpTransport : public agent::HttpTransport {
 public:
  struct Call {
    std::string method, host, path;
    std::string reqBody, respBody;
    int         status = 0;
    double      seconds = 0;
    std::string err;
  };

  CurlHttpTransport() { curl_global_init(CURL_GLOBAL_DEFAULT); }
  ~CurlHttpTransport() override { curl_global_cleanup(); }

  bool verbose = false;       // echo each exchange to stderr as it happens
  bool recordBodies = true;   // keep request/response bodies in the transcript
  std::vector<Call> calls;

  bool exec(const agent::HttpRequest& req, agent::HttpResponse& out,
            std::string& err) override {
    out.status = 0;
    out.body.clear();
    err.clear();

    Call rec;
    rec.method = req.method;
    rec.host = req.host;
    rec.path = req.path;
    if (recordBodies) rec.reqBody = req.body;

    CURL* c = curl_easy_init();
    if (!c) {
      err = "curl_easy_init failed";
      rec.err = err;
      calls.push_back(std::move(rec));
      return false;
    }

    const std::string url = std::string(req.tls ? "https://" : "http://") + req.host +
                            (req.port && req.port != (req.tls ? 443 : 80)
                                 ? ":" + std::to_string(req.port)
                                 : "") +
                            req.path;

    curl_slist* hdrs = nullptr;
    for (const auto& h : req.headers)
      hdrs = curl_slist_append(hdrs, (h.first + ": " + h.second).c_str());
    // The device transport frames with HTTP/1.0 + Connection: close and never
    // decodes chunked responses. curl handles framing itself, so we do not
    // reproduce that here - but we DO keep the identity encoding the device
    // relies on, so a gzip-only path can't pass here and fail there.
    hdrs = curl_slist_append(hdrs, "Expect:");

    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, (long)req.timeoutMs);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, 15000L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, &CurlHttpTransport::onWrite);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &out.body);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    if (req.method == "POST") {
      curl_easy_setopt(c, CURLOPT_POST, 1L);
      curl_easy_setopt(c, CURLOPT_POSTFIELDS, req.body.data());
      curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)req.body.size());
    } else if (req.method != "GET") {
      curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, req.method.c_str());
      if (!req.body.empty()) {
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, req.body.data());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)req.body.size());
      }
    }

    const auto t0 = std::chrono::steady_clock::now();
    const CURLcode rc = curl_easy_perform(c);
    rec.seconds = std::chrono::duration<double>(
                      std::chrono::steady_clock::now() - t0).count();

    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);

    if (rc != CURLE_OK) {
      err = curl_easy_strerror(rc);
      rec.err = err;
      rec.status = 0;
      if (verbose)
        std::fprintf(stderr, "[http] %s %s%s -> TRANSPORT FAIL: %s (%.1fs)\n",
                     req.method.c_str(), req.host.c_str(), req.path.c_str(),
                     err.c_str(), rec.seconds);
      calls.push_back(std::move(rec));
      return false;
    }

    out.status = (int)code;
    rec.status = (int)code;
    if (recordBodies) rec.respBody = out.body;
    if (verbose)
      std::fprintf(stderr, "[http] %s %s%s -> %ld  %zu B  (%.1fs)\n",
                   req.method.c_str(), req.host.c_str(), req.path.c_str(), code,
                   out.body.size(), rec.seconds);
    calls.push_back(std::move(rec));
    return true;
  }

  // Totals for the run report.
  size_t bytesIn() const {
    size_t n = 0;
    for (const auto& c : calls) n += c.respBody.size();
    return n;
  }
  double seconds() const {
    double s = 0;
    for (const auto& c : calls) s += c.seconds;
    return s;
  }
  void reset() { calls.clear(); }

 private:
  static size_t onWrite(char* p, size_t sz, size_t nm, void* ud) {
    static_cast<std::string*>(ud)->append(p, sz * nm);
    return sz * nm;
  }
};

}  // namespace lab
