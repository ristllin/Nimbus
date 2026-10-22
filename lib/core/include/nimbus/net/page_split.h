#pragma once
#include <cstddef>
#include <cstring>

// page_split - the config page is served as TWO responses (CUM-565): GET / sends the
// HTML fragments followed by a one-line script tag, and GET app.js sends the page
// script (the body of the last fragment, include/web/ui_js.h, without its <script>
// and </script> tags). The cloud relay buffers each tunneled response whole under
// kLoopbackMaxRespBody (cloud/loopback_target.h), so the biggest body is what must
// fit, and splitting the ~290 KB script from the ~85 KB HTML leaves room to grow
// where one ~375 KB page had about 400 bytes left.
//
// The tag's URL is RELATIVE ("app.js", not "/app.js"): the cloud serves the device
// page under /d/<id>/, so an absolute path would leave the tunnel. A script that
// fails to load (the busy answer while memory is short) reloads the page after a
// pause, like the HTML busy page does.
//
// Pure and host-tested: test_loopback_capacity measures both bodies from the blessed
// snapshot with these helpers, and webui.cpp serves exactly the same split.
namespace nimbus {
namespace net {

inline constexpr char kPageScriptOpen[] = "<script>";
inline constexpr char kPageScriptClose[] = "</script>\n";
inline constexpr char kPageScriptTag[] =
    "<script src=\"app.js\" onerror=\"setTimeout(function(){location.reload()},4000)\"></script>\n";

inline constexpr size_t kPageScriptOpenLen = sizeof(kPageScriptOpen) - 1;
inline constexpr size_t kPageScriptCloseLen = sizeof(kPageScriptClose) - 1;
inline constexpr size_t kPageScriptTagLen = sizeof(kPageScriptTag) - 1;

// Where the script body sits inside the script fragment `frag` (`len` bytes): from
// `off`, `n` bytes. False when the fragment is not exactly <script>...</script>\n,
// in which case the caller serves the page whole (the old single response).
inline bool pageScriptBody(const char* frag, size_t len, size_t& off, size_t& n) {
  if (!frag || len < kPageScriptOpenLen + kPageScriptCloseLen) return false;
  if (std::memcmp(frag, kPageScriptOpen, kPageScriptOpenLen) != 0) return false;
  if (std::memcmp(frag + len - kPageScriptCloseLen, kPageScriptClose, kPageScriptCloseLen) != 0)
    return false;
  off = kPageScriptOpenLen;
  n = len - kPageScriptOpenLen - kPageScriptCloseLen;
  return true;
}

// The two response sizes for an assembled page of `pageLen` bytes whose script
// fragment is `scriptFragLen` bytes long: GET / (the HTML plus the tag) and GET
// app.js (the script body).
inline size_t splitHtmlBytes(size_t pageLen, size_t scriptFragLen) {
  return pageLen - scriptFragLen + kPageScriptTagLen;
}
inline size_t splitScriptBytes(size_t scriptFragLen) {
  return scriptFragLen - kPageScriptOpenLen - kPageScriptCloseLen;
}

}  // namespace net
}  // namespace nimbus
