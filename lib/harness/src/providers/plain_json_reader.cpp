// plain_json_reader.cpp - the strict JSON reader of engine 2's wires (plain_json_reader.h): the
// part every codec shares out of line. The reader itself is in the header (internal linkage, so a
// codec's parse keeps it in line).
#include "plain_json_reader.h"

namespace agent {
namespace plain {

bool fieldOnce(const std::string& key, const char* const* fields, uint32_t n, uint32_t& seen, uint32_t& k) {
  for (k = 0; k < n && key != fields[k]; ++k) {
  }
  if (k == n) return true;
  if (seen & (1u << k)) return false;
  seen |= 1u << k;
  return true;
}

}  // namespace plain
}  // namespace agent
