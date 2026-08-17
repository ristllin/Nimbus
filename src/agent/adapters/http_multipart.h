#pragma once
#include <Arduino.h>
#include <FS.h>

#include <vector>

// http_multipart - a tiny RFC-2388 multipart/form-data POST over WiFiClientSecure,
// streaming ONE file straight from LittleFS so a multi-KB (audio/image) upload
// never has to sit in the S3's scarce internal heap. Shared by the STT adapter
// (audio -> /v1/audio/transcriptions) and Telegram media send (sendVoice/sendPhoto/
// sendDocument). TLS buffers come from PSRAM (the global mbedTLS allocator in
// main.cpp), so only the small framing lives on internal heap.
//
// Content-Length is computed up front (HTTP/1.0, Connection: close) from the field
// framing + the on-disk file size, then the body is streamed. Returns the response
// body (bounded) so the caller can parse the provider/Telegram JSON.
namespace agent {
namespace httpmp {

struct Field {
  String name;
  String value;
};

// POST multipart/form-data to https://host:port/path. `bearer` (may be empty) is
// sent as "Authorization: Bearer <bearer>". The single file part is read from
// `filePath` on LittleFS; if `filePath` is empty, no file part is sent (fields
// only). On success returns true and fills `respBody` (truncated to its capacity);
// on failure returns false and sets `err`.
// srcFs: the filesystem the file part streams from - nullptr = LittleFS (the
// historical default); pass &memory::dataFs() to stream an SD /mem/... artifact
// (E1 files.send) without staging it into the small LittleFS partition first.
// lockSrc: serialize every SD touch (open/read/close) under memory::Lock - REQUIRED
// (true) when srcFs is the SD data store, since this runs on tg_poll concurrently
// with Lock-holding SD writers and the firmware allows only one task inside SD at a
// time. The lock is taken per-call, never across the TLS write. Leave false for
// LittleFS (voice/STT), which isn't under that mutex.
// filePrefix/filePrefixLen: optional bytes injected between the file part's headers
// and the on-disk bytes, counted into Content-Length. Lets STT stream a headerless
// PCM capture as a WAV (44-byte RIFF header inline) WITHOUT writing a second copy
// of the whole recording to flash - the double-copy was the ceiling on recording
// length (owner 2026-07-16).
bool post(const char* host, int port, const char* path, const String& bearer,
          const std::vector<Field>& fields, const char* fileField,
          const char* fileName, const char* fileMime, const char* filePath,
          String& respBody, String& err, fs::FS* srcFs = nullptr, bool lockSrc = false,
          const uint8_t* filePrefix = nullptr, size_t filePrefixLen = 0);

}  // namespace httpmp
}  // namespace agent
