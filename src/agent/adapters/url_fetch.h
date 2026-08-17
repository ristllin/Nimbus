#pragma once
#include <functional>
#include <string>

// url_fetch - the W18 files.fetch download engine + AI scan verdict.
//
// httpsGetStream: one bounded, streamed GET of an ARBITRARY https URL (this is
// the only place in the firmware that talks to non-provider hosts; everything
// about it is deliberately narrow):
//   - https only (parseHttpsUrl refused http upstream; MITM write primitive);
//   - HTTP/1.0 + Connection: close (a 1.0 request forbids chunked encoding,
//     so the body is Content-Length or read-to-close - no chunk parser);
//   - follows <= 3 redirects, same rules as resolveRedirect (no downgrade);
//   - hard maxBytes cap enforced DURING the stream (abort, never truncate-and-
//     pretend) - the sink never sees more than maxBytes;
//   - data flows through a 4 KB PSRAM buffer to the sink; nothing file-sized
//     ever sits in internal RAM.
// Runs on tg_poll under the work TLS slot (single-TLS discipline). Blocking,
// bounded by kFetchTimeoutMs per hop.
//
// scanVerdict: one minimal provider completion over the (printable-filtered)
// head of a quarantined download. Constrained reply contract: the model answers
// exactly SAFE or UNSAFE + one reason line. Returns 1 SAFE / 0 UNSAFE /
// -1 unavailable (no key / network / unparseable). FAIL-CLOSED at the caller:
// only an explicit SAFE promotes a scan-mode download.
namespace agent {
namespace urlfetch {

constexpr uint32_t kFetchTimeoutMs = 120000;  // per hop - matches provider_file_fetch
                                              // (8 MB at weak-WiFi rates outruns 45 s)
constexpr int      kMaxRedirects   = 3;

// Sink returns false to abort (e.g. store write failed). On success returns the
// total bytes streamed; on failure returns 0 with err set (err also set when the
// server replies non-200 after redirects: "HTTP <code>").
uint64_t httpsGetStream(const std::string& url,
                        const std::function<bool(const uint8_t*, size_t)>& sink,
                        uint64_t maxBytes, std::string& err,
                        std::string& contentTypeOut);

// 1 = SAFE, 0 = UNSAFE (reason set), -1 = scan unavailable (reason set).
int scanVerdict(const std::string& headText, const std::string& url,
                const std::string& name, std::string& reason);

}  // namespace urlfetch
}  // namespace agent
