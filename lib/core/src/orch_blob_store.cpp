#include "nimbus/orch/blob_store.h"

namespace nimbus {
namespace orch {

namespace {
std::string fnvHex(uint64_t h) {
  char out[17];
  const char* hex = "0123456789abcdef";
  for (int i = 15; i >= 0; i--) { out[i] = hex[h & 0xf]; h >>= 4; }
  out[16] = '\0';
  return std::string(out);
}
}  // namespace

void BlobHasher::update(const void* data, size_t len) {
  const unsigned char* p = (const unsigned char*)data;
  for (size_t i = 0; i < len; i++) {
    h_ ^= (uint64_t)p[i];
    h_ *= 1099511628211ULL;
  }
}

std::string BlobHasher::hex() const { return fnvHex(h_); }

std::string blobHash(const std::string& bytes) {
  BlobHasher h;
  h.update(bytes.data(), bytes.size());
  return h.hex();
}

std::string blobPath(const std::string& dir, const std::string& hash, const std::string& ext) {
  std::string p = dir;
  if (!p.empty() && p.back() != '/') p += '/';
  p += hash;
  if (!ext.empty()) { p += '.'; p += ext; }
  return p;
}

std::string blobHashOf(const std::string& pathOrName) {
  size_t slash = pathOrName.find_last_of('/');
  size_t start = slash == std::string::npos ? 0 : slash + 1;
  size_t dot = pathOrName.find('.', start);
  size_t end = dot == std::string::npos ? pathOrName.size() : dot;
  if (end <= start) return "";
  return pathOrName.substr(start, end - start);
}

std::vector<std::string> unreferencedBlobs(const std::vector<std::string>& present,
                                           const std::set<std::string>& referenced) {
  std::vector<std::string> dead;
  for (const auto& name : present) {
    std::string h = blobHashOf(name);
    if (h.empty() || referenced.find(h) == referenced.end()) dead.push_back(name);
  }
  return dead;
}

}  // namespace orch
}  // namespace nimbus
