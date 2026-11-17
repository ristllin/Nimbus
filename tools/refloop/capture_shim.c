// capture_shim - sends an unmodified nimbusd's provider requests through the local
// capture proxy (capture_proxy.py), so the rig can record the exact request bodies
// the harness sends. nimbusd keeps no request bodies by design and has no base-URL
// override for Anthropic or OpenAI, and the rig may not change its source, so this
// interposes on libcurl's curl_easy_setopt from outside the process image:
//
//   DYLD_INSERT_LIBRARIES=capture_shim.dylib  (macOS)
//   LD_PRELOAD=capture_shim.so                (Linux)
//   REFLOOP_CAPTURE=http://127.0.0.1:<port>/<label>
//
// Only CURLOPT_URL is touched, and only for the hosts listed in kHosts:
//   https://api.anthropic.com/v1/messages
//   -> http://127.0.0.1:<port>/<label>/api.anthropic.com/v1/messages
// The proxy forwards the same method, headers and body bytes to the real host over
// TLS. Every other option and every other host passes through untouched. With
// REFLOOP_CAPTURE unset the shim does nothing.
#include <curl/curl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* const kHosts[] = {"api.anthropic.com", "api.openai.com", "api.mistral.ai"};

static int captured_host(const char* rest) {
  for (size_t i = 0; i < sizeof(kHosts) / sizeof(kHosts[0]); i++) {
    const size_t n = strlen(kHosts[i]);
    if (strncmp(rest, kHosts[i], n) == 0 && (rest[n] == '/' || rest[n] == '\0')) return 1;
  }
  return 0;
}

#ifdef __APPLE__
#define REAL_SETOPT curl_easy_setopt
static CURLcode shim_setopt(CURL* h, CURLoption opt, ...);
#else
#define _GNU_SOURCE_SHIM 1
#include <dlfcn.h>
typedef CURLcode (*setopt_fn)(CURL*, CURLoption, ...);
static setopt_fn real_setopt(void) {
  static setopt_fn fn;
  if (!fn) fn = (setopt_fn)dlsym(RTLD_NEXT, "curl_easy_setopt");
  return fn;
}
#define REAL_SETOPT real_setopt()
#define shim_setopt curl_easy_setopt
#undef curl_easy_setopt
#endif

static CURLcode set_url(CURL* h, const char* url) {
  const char* base = getenv("REFLOOP_CAPTURE");
  if (base && *base && url && strncmp(url, "https://", 8) == 0 && captured_host(url + 8)) {
    char buf[4096];
    const int n = snprintf(buf, sizeof(buf), "%s/%s", base, url + 8);
    if (n > 0 && (size_t)n < sizeof(buf)) return (REAL_SETOPT)(h, CURLOPT_URL, buf);
  }
  return (REAL_SETOPT)(h, CURLOPT_URL, url);
}

#ifdef __APPLE__
static
#endif
CURLcode shim_setopt(CURL* h, CURLoption opt, ...) {
  va_list ap;
  va_start(ap, opt);
  CURLcode rc;
  // libcurl's option numbering: 0 long, 10000 object pointer, 20000 function pointer,
  // 30000 curl_off_t, 40000 blob pointer.
  if (opt == CURLOPT_URL) rc = set_url(h, va_arg(ap, const char*));
  else if (opt < 10000) rc = (REAL_SETOPT)(h, opt, va_arg(ap, long));
  else if (opt >= 30000 && opt < 40000) rc = (REAL_SETOPT)(h, opt, va_arg(ap, curl_off_t));
  else rc = (REAL_SETOPT)(h, opt, va_arg(ap, void*));
  va_end(ap);
  return rc;
}

#ifdef __APPLE__
__attribute__((used)) static struct {
  const void* replacement;
  const void* replacee;
} interpose_setopt __attribute__((section("__DATA,__interpose"))) = {
    (const void*)(unsigned long)&shim_setopt, (const void*)(unsigned long)&curl_easy_setopt};
#endif
